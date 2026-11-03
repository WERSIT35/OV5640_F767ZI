#!/usr/bin/env python3
"""
Receive QVGA RGB565 frames dumped by the F767ZI over USART3 and save them
as PNGs.

    pip install pyserial pillow
    python recv_frame.py                           # list available ports
    python recv_frame.py COM5 --count 1 --show
    python recv_frame.py COM5 --count 0            # keep going until Ctrl-C
    python recv_frame.py COM5 --swap               # if red and blue look wrong

Wire format (little-endian), matching Camera_DumpFrameUart() in camera.c:

    magic  "OV56"        4 bytes
    width                2
    height               2
    bytes per pixel      1
    format code          1   (0 = RGB565)
    payload length       4
    payload              N
    checksum             4   sum of the payload bytes
"""

import argparse
import struct
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("pyserial is required:  pip install pyserial")

try:
    from PIL import Image
except ImportError:
    sys.exit("pillow is required:  pip install pillow")

MAGIC = b"OV56"
HEADER_STRUCT = struct.Struct("<HHBBI")   # width, height, bpp, format, length
HEADER_SIZE = HEADER_STRUCT.size

# Read granularity. Must be large enough that we are not doing a syscall per
# byte at 2 Mbaud (200 KB/s), or the driver buffer overflows and we silently
# lose data mid-frame.
CHUNK = 65536


class Stream:
    """
    Buffered reader over a serial port.

    Everything goes through one internal buffer so that scanning for the
    frame magic and reading the payload cannot disagree about how much has
    been consumed.
    """

    def __init__(self, port):
        self.port = port
        self.buf = bytearray()

    def _fill(self, minimum):
        """Read until the buffer holds at least `minimum` bytes."""
        while len(self.buf) < minimum:
            want = self.port.in_waiting or 1
            chunk = self.port.read(min(CHUNK, max(want, minimum - len(self.buf))))
            if not chunk:
                raise TimeoutError(
                    f"timed out with {len(self.buf)} of {minimum} bytes buffered"
                )
            self.buf += chunk

    def read(self, count):
        self._fill(count)
        out = bytes(self.buf[:count])
        del self.buf[:count]
        return out

    def request(self, command):
        """
        Ask the board for a frame.

        Deliberately does not flush the input buffer: if the board is already
        sending a frame - which it does on its own when nothing is driving
        its RX pin - discarding it would throw away the very thing we want.
        find_frame() validates headers, so stale bytes are harmless.
        """
        try:
            self.port.write(command)
            self.port.flush()
        except OSError:
            pass          # no RX wire to the board; it free-runs instead

    def peek(self, count):
        """Look ahead without consuming, so a bad header can be rejected."""
        self._fill(count)
        return bytes(self.buf[:count])

    def discard(self, count):
        self._fill(count)
        del self.buf[:count]

    def sync(self, magic):
        """
        Consume up to and including the next occurrence of `magic`.

        Returns whatever preceded it, which is the firmware's human-readable
        status text sharing the same UART.
        """
        searched = 0
        while True:
            index = self.buf.find(magic, searched)
            if index >= 0:
                skipped = bytes(self.buf[:index])
                del self.buf[: index + len(magic)]
                return skipped

            # A partial magic could straddle the boundary, so back off by
            # len(magic)-1 rather than rescanning from scratch each time.
            searched = max(0, len(self.buf) - len(magic) + 1)
            self._fill(len(self.buf) + 1)


def list_ports():
    """Print every serial port the OS knows about, with its description."""
    ports = sorted(serial.tools.list_ports.comports(), key=lambda p: p.device)

    if not ports:
        print("No serial ports found.", file=sys.stderr)
        return

    print("Available serial ports:", file=sys.stderr)
    for p in ports:
        print(f"  {p.device:<8} {p.description}", file=sys.stderr)


def rgb565_to_rgb888(payload, width, height, swap):
    """Unpack RGB565 little-endian halfwords into a flat RGB byte string."""
    out = bytearray(width * height * 3)

    for i in range(width * height):
        lo = payload[2 * i]
        hi = payload[2 * i + 1]

        if swap:
            lo, hi = hi, lo

        pixel = (hi << 8) | lo

        r5 = (pixel >> 11) & 0x1F
        g6 = (pixel >> 5) & 0x3F
        b5 = pixel & 0x1F

        # Replicate the high bits into the low ones so full-scale maps to 255.
        out[3 * i + 0] = (r5 << 3) | (r5 >> 2)
        out[3 * i + 1] = (g6 << 2) | (g6 >> 4)
        out[3 * i + 2] = (b5 << 3) | (b5 >> 2)

    return bytes(out)


def header_is_plausible(width, height, bpp, fmt, length):
    """
    Reject a magic match whose header does not hold together.

    A four-byte magic collides often inside RGB565 pixel data, whose bytes
    cluster in the same 0x20-0x60 range as "OV56". Requiring the header to be
    self-consistent - a known format, sane dimensions, and a length that is
    exactly width*height*bpp - makes a false lock effectively impossible.
    """
    return (fmt == 0
            and bpp == 2
            and 0 < width <= 4096
            and 0 < height <= 4096
            and length == width * height * bpp)


def find_frame(stream):
    """Scan until a magic with a self-consistent header. Returns the header."""
    while True:
        skipped = stream.sync(MAGIC)

        fields = HEADER_STRUCT.unpack(stream.peek(HEADER_SIZE))

        if header_is_plausible(*fields):
            stream.discard(HEADER_SIZE)
            return fields, skipped

        # False positive inside pixel data - keep scanning from here.


def receive_one(stream, args, index):
    # Ask for a frame. The board is otherwise quiet, so what follows is our
    # frame rather than something we have to dig out of a pixel stream.
    stream.request(b"c")

    (width, height, bpp, fmt, length), skipped = find_frame(stream)

    # Only echo text if we skipped a small amount. A large skip is pixel data
    # from a partial frame, and echoing it just floods the console.
    if skipped and len(skipped) < 2048:
        text = skipped.decode("ascii", "replace")
        for line in text.splitlines():
            stripped = line.strip()
            printable = sum(32 <= ord(c) < 127 for c in stripped)
            if len(stripped) > 3 and printable == len(stripped):
                print(f"  [board] {stripped}")

    payload = stream.read(length)
    (checksum,) = struct.unpack("<I", stream.read(4))

    actual = sum(payload) & 0xFFFFFFFF
    if actual != checksum:
        print(
            f"  checksum mismatch: got 0x{actual:08X}, expected 0x{checksum:08X}"
            " - saving anyway",
            file=sys.stderr,
        )

    image = Image.frombytes(
        "RGB", (width, height), rgb565_to_rgb888(payload, width, height, args.swap)
    )

    name = f"{args.prefix}{index:04d}.png"
    image.save(name)
    print(f"  saved {name}  ({width}x{height}, {length} bytes)")

    if args.show and index == 0:
        image.show()


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("port", nargs="?",
                        help="serial port, e.g. COM5 or /dev/ttyACM0; "
                             "omit to list what is available")
    parser.add_argument("--baud", type=int, default=2000000,
                        help="baud rate; must match MX_USART3_UART_Init "
                             "(default: 2000000)")
    parser.add_argument("--count", type=int, default=1,
                        help="frames to capture; 0 means run until Ctrl-C")
    parser.add_argument("--prefix", default="frame_",
                        help="output filename prefix (default: frame_)")
    parser.add_argument("--swap", action="store_true",
                        help="swap the two RGB565 bytes (fixes red/blue inversion)")
    parser.add_argument("--show", action="store_true",
                        help="open the first frame in the default image viewer")
    parser.add_argument("--timeout", type=float, default=10.0,
                        help="serial read timeout in seconds (default: 10)")
    args = parser.parse_args()

    if not args.port:
        list_ports()
        sys.exit("\nPick one of the above and pass it, e.g.:  "
                 "python recv_frame.py COM5")

    try:
        opened = serial.Serial(args.port, args.baud, timeout=args.timeout)
    except serial.SerialException as exc:
        print(f"Could not open {args.port}: {exc}\n", file=sys.stderr)
        list_ports()
        sys.exit("\nIf the port exists, something else is probably holding it "
                 "open - close PuTTY and try again.")

    with opened as port:
        # Windows defaults to a small driver buffer; at 2 Mbaud that overflows
        # during the Python-side pixel conversion between frames.
        try:
            port.set_buffer_size(rx_size=1 << 20)
        except (AttributeError, NotImplementedError):
            pass

        print(f"listening on {args.port} at {args.baud} baud")
        stream = Stream(port)

        index = 0
        while args.count == 0 or index < args.count:
            start = time.time()
            try:
                receive_one(stream, args, index)
            except (TimeoutError, ValueError) as exc:
                print(f"  frame {index} failed: {exc}", file=sys.stderr)
                continue
            print(f"  took {time.time() - start:.1f} s")
            index += 1


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nstopped")
