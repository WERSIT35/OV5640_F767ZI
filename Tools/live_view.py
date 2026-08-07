#!/usr/bin/env python3
"""
Live viewer for the OV5640 capture running on the F767ZI.

Continuously requests frames and displays them in a window, so you can point
the camera at things and see the result without a save-open-inspect cycle.

    pip install pyserial pillow
    python live_view.py COM5

Keys:
    s       toggle RGB565 byte swap (fixes red/blue inversion)
    g       toggle greyscale, ignoring colour entirely
    space   save the current frame as a PNG
    q       quit

At 115200 baud a 320x240 RGB565 frame takes ~13 s to transfer, so this is a
slideshow rather than video.  The frame time is dominated entirely by the
UART, not by the sensor or the display.
"""

import argparse
import queue
import struct
import sys
import threading
import time
import tkinter as tk

try:
    import serial
except ImportError:
    sys.exit("pyserial is required:  pip install pyserial")

try:
    from PIL import Image, ImageTk
except ImportError:
    sys.exit("pillow is required:  pip install pillow")

from recv_frame import (MAGIC, Stream, find_frame, list_ports)

ZOOM = 2


class Receiver(threading.Thread):
    """
    Pulls frames off the serial port in the background.

    Only raw payloads cross into the UI thread; converting to RGB happens
    there, so toggling byte order can re-render the frame already in hand
    without waiting another 13 seconds for a new one.
    """

    daemon = True

    def __init__(self, port, out_queue, status_queue):
        super().__init__()
        self.port = port
        self.out = out_queue
        self.status = status_queue
        self.stop_event = threading.Event()

    def run(self):
        stream = Stream(self.port)

        while not self.stop_event.is_set():
            started = time.time()
            try:
                (w, h, bpp, fmt, length), skipped = find_frame(stream)
                payload = stream.read(length)
                (checksum,) = struct.unpack("<I", stream.read(4))
            except (TimeoutError, ValueError, OSError) as exc:
                self.status.put(f"receive error: {exc}")
                continue

            for line in self._log_lines(skipped):
                self.status.put(f"[board] {line}")

            if (sum(payload) & 0xFFFFFFFF) != checksum:
                self.status.put("checksum mismatch - showing anyway")

            self.out.put((w, h, payload, time.time() - started))

            # Ask for the next one only after this frame is fully consumed,
            # so a request never overlaps a transfer in progress.
            try:
                stream.request(b"c")
            except OSError as exc:
                self.status.put(f"write failed: {exc}")
                return

    @staticmethod
    def _log_lines(skipped):
        if not skipped or len(skipped) > 2048:
            return []

        lines = []
        for line in skipped.decode("ascii", "replace").splitlines():
            s = line.strip()
            if len(s) > 3 and all(32 <= ord(c) < 127 for c in s):
                lines.append(s)
        return lines


def to_image(width, height, payload, swap, grey):
    """Convert an RGB565 payload into a PIL image."""
    if grey:
        # Green carries the most bits and tracks luminance closely enough for
        # a sanity check, and it sidesteps byte-order questions entirely.
        out = bytearray(width * height)
        for i in range(width * height):
            lo, hi = payload[2 * i], payload[2 * i + 1]
            if swap:
                lo, hi = hi, lo
            g6 = (((hi << 8) | lo) >> 5) & 0x3F
            out[i] = (g6 << 2) | (g6 >> 4)
        return Image.frombytes("L", (width, height), bytes(out))

    out = bytearray(width * height * 3)
    for i in range(width * height):
        lo, hi = payload[2 * i], payload[2 * i + 1]
        if swap:
            lo, hi = hi, lo
        pixel = (hi << 8) | lo
        r5 = (pixel >> 11) & 0x1F
        g6 = (pixel >> 5) & 0x3F
        b5 = pixel & 0x1F
        out[3 * i + 0] = (r5 << 3) | (r5 >> 2)
        out[3 * i + 1] = (g6 << 2) | (g6 >> 4)
        out[3 * i + 2] = (b5 << 3) | (b5 >> 2)

    return Image.frombytes("RGB", (width, height), bytes(out))


class Viewer:
    def __init__(self, root, frames, status, args):
        self.root = root
        self.frames = frames
        self.status = status
        self.args = args

        self.swap = args.swap
        self.grey = False
        self.count = 0
        self.last = None          # (w, h, payload)
        self.photo = None

        root.title("OV5640 live view")

        self.canvas = tk.Label(root, background="#101010")
        self.canvas.pack()

        self.label = tk.Label(root, text="waiting for first frame...",
                              anchor="w", font=("Consolas", 9))
        self.label.pack(fill="x")

        self.log = tk.Label(root, text="", anchor="w", justify="left",
                            font=("Consolas", 8), foreground="#666")
        self.log.pack(fill="x")

        root.bind("<Key>", self.on_key)
        root.after(50, self.poll)

    def on_key(self, event):
        key = event.char.lower()

        if key == "q":
            self.root.destroy()
        elif key == "s":
            self.swap = not self.swap
            self.redraw()
        elif key == "g":
            self.grey = not self.grey
            self.redraw()
        elif key == " " and self.last:
            name = f"live_{self.count:04d}.png"
            to_image(*self.last, self.swap, self.grey).save(name)
            self.log.config(text=f"saved {name}")

    def redraw(self):
        """Re-render the frame already in hand, without waiting for a new one."""
        if not self.last:
            return

        w, h, payload = self.last
        image = to_image(w, h, payload, self.swap, self.grey)
        image = image.resize((w * ZOOM, h * ZOOM), Image.NEAREST)

        self.photo = ImageTk.PhotoImage(image)
        self.canvas.config(image=self.photo)

        self.label.config(
            text=f"frame {self.count}  {w}x{h}  "
                 f"swap={'on' if self.swap else 'off'}  "
                 f"grey={'on' if self.grey else 'off'}   "
                 f"[s]wap  [g]rey  [space]save  [q]uit"
        )

    def poll(self):
        while True:
            try:
                message = self.status.get_nowait()
            except queue.Empty:
                break
            self.log.config(text=message)

        try:
            w, h, payload, elapsed = self.frames.get_nowait()
        except queue.Empty:
            pass
        else:
            self.count += 1
            self.last = (w, h, payload)
            self.redraw()
            self.label.config(text=self.label.cget("text") +
                              f"   {elapsed:.1f}s")

        self.root.after(50, self.poll)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("port", nargs="?", help="serial port, e.g. COM5")
    parser.add_argument("--baud", type=int, default=115200,
                        help="must match MX_USART3_UART_Init (default: 115200)")
    parser.add_argument("--swap", action="store_true",
                        help="start with RGB565 byte swap enabled")
    parser.add_argument("--timeout", type=float, default=30.0,
                        help="serial read timeout in seconds (default: 30)")
    args = parser.parse_args()

    if not args.port:
        list_ports()
        sys.exit("\nPick one of the above, e.g.:  python live_view.py COM5")

    try:
        port = serial.Serial(args.port, args.baud, timeout=args.timeout)
    except serial.SerialException as exc:
        print(f"Could not open {args.port}: {exc}\n", file=sys.stderr)
        list_ports()
        sys.exit("\nClose PuTTY if it is holding the port.")

    with port:
        try:
            port.set_buffer_size(rx_size=1 << 20)
        except (AttributeError, NotImplementedError):
            pass

        frames = queue.Queue()
        status = queue.Queue()

        # Kick off the first request before the reader starts waiting.
        port.reset_input_buffer()
        port.write(b"c")
        port.flush()

        receiver = Receiver(port, frames, status)
        receiver.start()

        root = tk.Tk()
        Viewer(root, frames, status, args)
        try:
            root.mainloop()
        finally:
            receiver.stop_event.set()


if __name__ == "__main__":
    main()
