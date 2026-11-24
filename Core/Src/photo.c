/**
  ******************************************************************************
  * @file    photo.c
  * @brief   Photos on the SD card: save (BMP), list, and show (BMP, JPEG).
  *
  * JPEGs are decoded by the STM32's JPEG codec, streamed from the card and
  * scaled down block by block, so their size is not limited by RAM.
  ******************************************************************************
  */

#pragma GCC optimize ("O2")

#include "photo.h"
#include "ff.h"
#include "gfx.h"
#include "main.h"
#include "sd.h"
#include <stdio.h>
#include <string.h>

#define OWN_DIR      "OV5640"
#define MAX_PHOTOS   3000
#define POOL_SIZE    72000U
#define MAX_PATH_LEN 80
#define MAX_DEPTH    4
#define BMP_OFFSET   512U
#define IO_SIZE      15360U          /* 16 BMP rows, 30 sectors */

static FATFS    fs;
static FIL      file;
static uint32_t io_words[IO_SIZE / 4U];
#define io      ((uint8_t *)io_words)

/* SRAM next to the frame buffers; DTCM is too small for it. */
static char     pool[POOL_SIZE] __attribute__((section(".framebuf")));
static uint32_t pool_used;
static uint32_t path_at[MAX_PHOTOS];
static int  path_count;
static int  own_count;

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

static bool mount(void)
{
  if (!Sd_IsPresent())
  {
    return false;
  }

  Sd_AllowWrite(false);
  Sd_Protect(0U);

  if (f_mount(&fs, "", 1) != FR_OK)
  {
    return false;
  }

  Sd_Protect(fs.volbase);
  return true;
}

static uint32_t rd16(const uint8_t *p) { return p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (rd16(p + 2) << 16); }

static void wr32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t hash(uint32_t h, const uint8_t *p, uint32_t n)
{
  while (n-- > 0U)
  {
    h = (h ^ *p++) * 16777619U;
  }

  return h;
}

/** True if s ends in .ext, ignoring case. */
static bool has_ext(const char *s, const char *ext)
{
  size_t n = strlen(s);
  size_t m = strlen(ext);

  if (n <= m)
  {
    return false;
  }

  for (size_t i = 0U; i < m; i++)
  {
    char c = s[n - m + i];

    if ((c >= 'a') && (c <= 'z')) { c = (char)(c - 'a' + 'A'); }
    if (c != ext[i])              { return false; }
  }

  return true;
}

/* -------------------------------------------------------------------------- */
/* Saving                                                                     */
/* -------------------------------------------------------------------------- */

static FRESULT next_number(uint32_t *number)
{
  DIR     dir;
  FILINFO fno;
  FRESULT res = f_opendir(&dir, OWN_DIR);

  *number = 1U;

  if (res == FR_NO_PATH)
  {
    return FR_OK;
  }

  while (res == FR_OK)
  {
    unsigned n;

    res = f_readdir(&dir, &fno);

    if ((res != FR_OK) || (fno.fname[0] == '\0'))
    {
      break;
    }

    if ((sscanf(fno.fname, "IMG_%u", &n) == 1) && (n >= *number))
    {
      *number = n + 1U;
    }
  }

  (void)f_closedir(&dir);
  return res;
}

static const char *save_locked(const uint16_t *frame, char *name)
{
  char     path[32];
  uint32_t number;
  uint32_t sum = 2166136261U;
  uint32_t check = 2166136261U;
  UINT     n;
  DWORD    free_clusters;
  FATFS   *pfs;
  FRESULT  res;

  if (next_number(&number) != FR_OK)               { return "CARD ERROR"; }
  /* One cluster of directory entries at most: this FatFs version has trouble
   * growing exFAT directories. */
  if (number > 1000U)                              { return "FOLDER FULL"; }
  if (f_getfree("", &free_clusters, &pfs) != FR_OK) { return "CARD ERROR"; }

  if (((uint64_t)free_clusters * pfs->csize) < 2048U)
  {
    return "CARD FULL";
  }

  res = f_mkdir(OWN_DIR);

  if ((res != FR_OK) && (res != FR_EXIST))
  {
    return (res == FR_WRITE_PROTECTED) ? "CARD LOCKED" : "CARD ERROR";
  }

  (void)snprintf(name, 16U, "IMG_%04lu.BMP", (unsigned long)number);
  (void)snprintf(path, sizeof(path), OWN_DIR "/%s", name);

  /* CREATE_NEW fails rather than touch a file that already exists. */
  if (f_open(&file, path, FA_WRITE | FA_CREATE_NEW) != FR_OK)
  {
    return "CANNOT CREATE";
  }

  memset(io, 0, BMP_OFFSET);
  io[0] = 'B';
  io[1] = 'M';
  wr32(&io[2],  BMP_OFFSET + (GFX_W * GFX_H * 3U));
  wr32(&io[10], BMP_OFFSET);
  wr32(&io[14], 40U);
  wr32(&io[18], GFX_W);
  wr32(&io[22], GFX_H);
  io[26] = 1U;
  io[28] = 24U;
  wr32(&io[34], GFX_W * GFX_H * 3U);
  sum = hash(sum, io, BMP_OFFSET);
  res = f_write(&file, io, BMP_OFFSET, &n);

  if (n != BMP_OFFSET) { res = FR_DISK_ERR; }

  /* Bottom row first, blue first, sixteen rows per write. */
  for (int y = GFX_H - 1; (y >= 0) && (res == FR_OK); y -= 16)
  {
    uint8_t *p = io;

    for (int r = 0; r < 16; r++)
    {
      const uint16_t *row = &frame[(y - r) * GFX_W];

      for (int x = 0; x < GFX_W; x++)
      {
        uint32_t c  = row[x];
        uint32_t r5 = (c >> 11) & 0x1FU;
        uint32_t g6 = (c >> 5)  & 0x3FU;
        uint32_t b5 = c & 0x1FU;

        *p++ = (uint8_t)((b5 << 3) | (b5 >> 2));
        *p++ = (uint8_t)((g6 << 2) | (g6 >> 4));
        *p++ = (uint8_t)((r5 << 3) | (r5 >> 2));
      }
    }

    sum = hash(sum, io, IO_SIZE);
    res = f_write(&file, io, IO_SIZE, &n);

    if (n != IO_SIZE) { res = FR_DISK_ERR; }
  }

  if ((f_close(&file) != FR_OK) || (res != FR_OK))
  {
    return "WRITE FAILED";
  }

  /* Read it all back from the card. */
  Sd_AllowWrite(false);

  if (f_open(&file, path, FA_READ) != FR_OK)
  {
    return "VERIFY FAILED";
  }

  do
  {
    res = f_read(&file, io, IO_SIZE, &n);
    check = hash(check, io, n);
  } while ((res == FR_OK) && (n > 0U));

  (void)f_close(&file);

  return ((res == FR_OK) && (check == sum)) ? 0 : "VERIFY FAILED";
}

const char *Photo_Save(const uint16_t *frame, char *name)
{
  const char *error;

  name[0] = '\0';

  if (PHOTO_SAVE_ENABLED == 0) { return "SAVING IS OFF"; }
  if (!Sd_IsPresent())         { return "NO CARD"; }
  if (!mount())                { return "CARD NOT READABLE"; }

  Sd_AllowWrite(true);
  error = save_locked(frame, name);
  Sd_AllowWrite(false);

  return error;
}

/* -------------------------------------------------------------------------- */
/* Listing                                                                    */
/* -------------------------------------------------------------------------- */

static void scan_dir(char *path, int depth, bool skip_own)
{
  DIR            dir;
  static FILINFO fno;                 /* not used across the recursion */
  size_t         len = strlen(path);

  if (f_opendir(&dir, path) != FR_OK)
  {
    return;
  }

  while ((path_count < MAX_PHOTOS) && (f_readdir(&dir, &fno) == FR_OK) &&
         (fno.fname[0] != '\0'))
  {
    const char *name = fno.fname;
    size_t      n;

    /* Names with characters outside the code page: use the short name. */
    if ((strchr(name, '?') != 0) && (fno.altname[0] != '\0'))
    {
      name = fno.altname;
    }

    n = strlen(name);

    if ((name[0] == '.') || ((fno.fattrib & (AM_HID | AM_SYS)) != 0U) ||
        ((len + 1U + n) >= MAX_PATH_LEN))
    {
      continue;
    }

    if (skip_own && (len == 0U) && (strcmp(name, OWN_DIR) == 0))
    {
      continue;
    }

    if (len > 0U) { path[len] = '/'; }
    memcpy(&path[(len > 0U) ? (len + 1U) : 0U], name, n + 1U);

    if ((fno.fattrib & AM_DIR) != 0U)
    {
      if (depth < MAX_DEPTH) { scan_dir(path, depth + 1, skip_own); }
    }
    else if (has_ext(name, ".JPG") || has_ext(name, ".JPEG") ||
             has_ext(name, ".BMP"))
    {
      size_t size = strlen(path) + 1U;

      if ((pool_used + size) > POOL_SIZE) { break; }

      memcpy(&pool[pool_used], path, size);
      path_at[path_count++] = pool_used;
      pool_used += size;
    }

    path[len] = '\0';
  }

  (void)f_closedir(&dir);
}

int Photo_Scan(void)
{
  char path[MAX_PATH_LEN];

  path_count = 0;
  own_count  = 0;
  pool_used  = 0U;

  if (!mount())
  {
    return -1;
  }

  strcpy(path, OWN_DIR);
  scan_dir(path, 1, false);
  own_count = path_count;

  path[0] = '\0';
  scan_dir(path, 0, true);

  return path_count;
}

const char *Photo_Path(int i)
{
  return ((i >= 0) && (i < path_count)) ? &pool[path_at[i]] : "";
}

int Photo_OwnCount(void)
{
  return own_count;
}

/* -------------------------------------------------------------------------- */
/* Fitting a picture to the screen                                            */
/* -------------------------------------------------------------------------- */

static uint16_t *canvas;
static int       src_w, src_h;        /* stored picture                       */
static int       dst_w, dst_h;        /* scaled, before rotation              */
static int       orient;              /* EXIF orientation 1..8                */
static int       off_x, off_y;

static void fit(int w, int h, int o)
{
  int rw = (o >= 5) ? h : w;
  int rh = (o >= 5) ? w : h;
  int tw;
  int th;

  if (((int64_t)rw * GFX_H) > ((int64_t)rh * GFX_W))
  {
    tw = (rw < GFX_W) ? rw : GFX_W;
    th = (int)(((int64_t)rh * tw) / rw);
  }
  else
  {
    th = (rh < GFX_H) ? rh : GFX_H;
    tw = (int)(((int64_t)rw * th) / rh);
  }

  if (tw < 1) { tw = 1; }
  if (th < 1) { th = 1; }

  src_w  = w;
  src_h  = h;
  orient = o;
  dst_w  = (o >= 5) ? th : tw;
  dst_h  = (o >= 5) ? tw : th;
  off_x  = (GFX_W - tw) / 2;
  off_y  = (GFX_H - th) / 2;
}

static inline int src_x(int dx) { return ((2 * dx + 1) * src_w) / (2 * dst_w); }
static inline int src_y(int dy) { return ((2 * dy + 1) * src_h) / (2 * dst_h); }

static inline void plot(int dx, int dy, uint16_t c)
{
  int x = dx;
  int y = dy;

  switch (orient)
  {
    case 2:  x = dst_w - 1 - dx;                          break;
    case 3:  x = dst_w - 1 - dx; y = dst_h - 1 - dy;      break;
    case 4:  y = dst_h - 1 - dy;                          break;
    case 5:  x = dy;             y = dx;                  break;
    case 6:  x = dst_h - 1 - dy; y = dx;                  break;
    case 7:  x = dst_h - 1 - dy; y = dst_w - 1 - dx;      break;
    case 8:  x = dy;             y = dst_w - 1 - dx;      break;
    default:                                              break;
  }

  canvas[((y + off_y) * GFX_W) + x + off_x] = c;
}

/* -------------------------------------------------------------------------- */
/* BMP                                                                        */
/* -------------------------------------------------------------------------- */

static const char *show_bmp(void)
{
  uint8_t  h[54];
  UINT     n;
  uint32_t data;
  uint32_t stride;
  int32_t  w;
  int32_t  hh;
  bool     bottom_up;
  uint32_t bpp;
  uint32_t comp;

  if ((f_read(&file, h, sizeof(h), &n) != FR_OK) || (n != sizeof(h)) ||
      (h[0] != 'B') || (h[1] != 'M'))
  {
    return "BAD BMP";
  }

  data      = rd32(&h[10]);
  w         = (int32_t)rd32(&h[18]);
  hh        = (int32_t)rd32(&h[22]);
  bpp       = rd16(&h[28]);
  comp      = rd32(&h[30]);
  bottom_up = (hh > 0);
  if (hh < 0) { hh = -hh; }

  if ((w <= 0) || (hh <= 0) || (w > 20000) || (hh > 20000))
  {
    return "BAD BMP";
  }

  stride = (((uint32_t)w * bpp + 31U) / 32U) * 4U;

  if (((bpp != 24U) && (bpp != 32U)) || ((comp != 0U) && (comp != 3U)) ||
      (stride > IO_SIZE))
  {
    return "BMP TYPE NOT SUPPORTED";
  }

  fit((int)w, (int)hh, 1);

  for (int dy = 0; dy < dst_h; dy++)
  {
    int      sy  = src_y(dy);
    uint32_t row = bottom_up ? (uint32_t)(hh - 1 - sy) : (uint32_t)sy;

    if ((f_lseek(&file, data + (row * stride)) != FR_OK) ||
        (f_read(&file, io, stride, &n) != FR_OK) || (n != stride))
    {
      return "READ ERROR";
    }

    for (int dx = 0; dx < dst_w; dx++)
    {
      const uint8_t *p = &io[(uint32_t)src_x(dx) * (bpp / 8U)];

      plot(dx, dy, GFX_RGB(p[2], p[1], p[0]));
    }
  }

  return 0;
}

/* -------------------------------------------------------------------------- */
/* JPEG                                                                       */
/* -------------------------------------------------------------------------- */

#define JPEG_IDLE_MS   2000U

static uint32_t io_pos, io_len;
static bool     io_error;

static int        jw, jh, jcomp, jhs, jvs;
static uint32_t   mcu_words[(6U * 64U) / 4U];
static uint32_t   mcu_fill, mcu_need;
static int        mcu_x, mcu_y;
static uint32_t   feed_word, feed_count;
static uint32_t   progress_at;

static int get_byte(void)
{
  if (io_pos >= io_len)
  {
    UINT n = 0U;

    if (f_read(&file, io, IO_SIZE, &n) != FR_OK) { io_error = true; n = 0U; }

    io_pos = 0U;
    io_len = n;

    if (n == 0U) { return -1; }
  }

  return io[io_pos++];
}

static void skip_bytes(uint32_t n)
{
  uint32_t have = io_len - io_pos;

  if (n <= have)
  {
    io_pos += n;
  }
  else
  {
    if (f_lseek(&file, f_tell(&file) + (n - have)) != FR_OK) { io_error = true; }
    io_pos = 0U;
    io_len = 0U;
  }
}

static inline int clamp8(int v) { return (v < 0) ? 0 : ((v > 255) ? 255 : v); }

/** One decoded MCU: draw every screen pixel that falls inside it. */
static void mcu_done(void)
{
  const uint8_t *m  = (const uint8_t *)mcu_words;
  int            mw = 8 * jhs;
  int            mh = 8 * jvs;
  int            k  = src_w / dst_w;
  int            dy = (mcu_y * dst_h) / src_h;

  if (k < 1) { k = 1; }
  if (k > 8) { k = 8; }

  if (dy > 0) { dy--; }
  while ((dy < dst_h) && (src_y(dy) < mcu_y)) { dy++; }

  for (; (dy < dst_h) && (src_y(dy) < (mcu_y + mh)); dy++)
  {
    int py = src_y(dy) - mcu_y;
    int dx = (mcu_x * dst_w) / src_w;

    if (dx > 0) { dx--; }
    while ((dx < dst_w) && (src_x(dx) < mcu_x)) { dx++; }

    for (; (dx < dst_w) && (src_x(dx) < (mcu_x + mw)); dx++)
    {
      int px = src_x(dx) - mcu_x;
      int x0 = px - (k / 2);
      int y0 = py - (k / 2);
      int sum = 0;
      int cnt = 0;
      int yy;

      /* Average the luma that this screen pixel stands for. */
      for (int j = y0; j < (y0 + k); j++)
      {
        if ((j < 0) || (j >= mh)) { continue; }

        for (int i = x0; i < (x0 + k); i++)
        {
          if ((i >= 0) && (i < mw))
          {
            int b = ((j >> 3) * jhs) + (i >> 3);

            sum += m[(b * 64) + ((j & 7) * 8) + (i & 7)];
            cnt++;
          }
        }
      }

      yy = sum / cnt;

      if (jcomp == 1)
      {
        plot(dx, dy, GFX_RGB((unsigned)yy, (unsigned)yy, (unsigned)yy));
      }
      else
      {
        const uint8_t *c = &m[jhs * jvs * 64];
        int ci = ((py / jvs) * 8) + (px / jhs);
        int cb = (int)c[ci] - 128;
        int cr = (int)c[64 + ci] - 128;
        unsigned r = (unsigned)clamp8(yy + ((359 * cr) >> 8));
        unsigned g = (unsigned)clamp8(yy - ((88 * cb + 183 * cr) >> 8));
        unsigned b = (unsigned)clamp8(yy + ((454 * cb) >> 8));

        plot(dx, dy, GFX_RGB(r, g, b));
      }
    }
  }

  mcu_x += mw;

  if (mcu_x >= src_w)
  {
    mcu_x  = 0;
    mcu_y += mh;
  }
}

static void drain(void)
{
  while ((JPEG->SR & JPEG_SR_OFNEF) != 0U)
  {
    uint32_t w = JPEG->DOR;

    progress_at = HAL_GetTick();

    if (mcu_need == 0U) { continue; }

    mcu_words[mcu_fill++] = w;

    if (mcu_fill >= mcu_need)
    {
      mcu_fill = 0U;
      if (mcu_y < src_h) { mcu_done(); }
    }
  }
}

/** Push a byte to the codec. False if it stopped accepting data. */
static bool feed(uint8_t b)
{
  feed_word |= (uint32_t)b << (8U * feed_count);

  if (++feed_count < 4U)
  {
    return true;
  }

  while ((JPEG->SR & JPEG_SR_IFNFF) == 0U)
  {
    drain();

    if (((JPEG->SR & JPEG_SR_EOCF) != 0U) ||
        ((HAL_GetTick() - progress_at) > JPEG_IDLE_MS))
    {
      return false;
    }
  }

  JPEG->DIR   = feed_word;
  progress_at = HAL_GetTick();
  feed_word   = 0U;
  feed_count  = 0U;
  return true;
}

/** Orientation from the start of an Exif block, or 1. */
static int exif_orient(const uint8_t *p, uint32_t n)
{
  bool     le;
  uint32_t ifd;
  uint32_t entries;

  if ((n < 22U) || (memcmp(p, "Exif\0\0", 6U) != 0))
  {
    return 1;
  }

  p += 6;
  n -= 6U;
  le = (p[0] == 'I');

#define E16(o) (le ? rd16(&p[o]) : (((uint32_t)p[o] << 8) | p[(o) + 1U]))
#define E32(o) (le ? rd32(&p[o]) : ((E16(o) << 16) | E16((o) + 2U)))

  ifd = E32(4U);

  if ((ifd > n) || ((ifd + 2U) > n)) { return 1; }

  entries = E16(ifd);

  for (uint32_t i = 0U; i < entries; i++)
  {
    uint32_t e = ifd + 2U + (i * 12U);

    if ((e + 12U) > n) { break; }

    if (E16(e) == 0x0112U)
    {
      uint32_t v = E16(e + 8U);

      return ((v >= 1U) && (v <= 8U)) ? (int)v : 1;
    }
  }

#undef E16
#undef E32
  return 1;
}

/** Find the preview picture listed in an MPF block (0 if none). */
static uint32_t mpf_preview(const uint8_t *p, uint32_t n, uint32_t tiff_pos)
{
  bool     le;
  uint32_t ifd;
  uint32_t entries;
  uint32_t table = 0U;
  uint32_t images = 0U;
  uint32_t best = 0U;
  uint32_t best_size = 0U;

  if ((n < 30U) || (memcmp(p, "MPF\0", 4U) != 0))
  {
    return 0U;
  }

  p += 4;
  n -= 4U;
  le = (p[0] == 'I');

#define E16(o) (le ? rd16(&p[o]) : (((uint32_t)p[o] << 8) | p[(o) + 1U]))
#define E32(o) (le ? rd32(&p[o]) : ((E16(o) << 16) | E16((o) + 2U)))

  ifd = E32(4U);

  if ((ifd > n) || ((ifd + 2U) > n)) { return 0U; }

  entries = E16(ifd);

  for (uint32_t i = 0U; i < entries; i++)
  {
    uint32_t e = ifd + 2U + (i * 12U);

    if ((e + 12U) > n) { break; }

    if (E16(e) == 0xB002U)
    {
      images = E32(e + 4U) / 16U;
      table  = E32(e + 8U);
    }
  }

  /* Image 0 is the photo itself; take the largest of the small ones. */
  for (uint32_t i = 1U; (table != 0U) && (i < images) && (i < 8U); i++)
  {
    uint32_t e = table + (i * 16U);
    uint32_t size;

    if ((e > n) || ((e + 16U) > n)) { break; }

    size = E32(e + 4U);

    if ((size > best_size) && (size >= 15000U) && (size < 2000000U))
    {
      best_size = size;
      best      = tiff_pos + E32(e + 8U);
    }
  }

#undef E16
#undef E32
  return best;
}

/**
  * @param  start    file offset of the JPEG
  * @param  o        orientation to use
  * @param  preview  if not NULL, stop and return "" once a preview is found
  * @param  found_o  receives the Exif orientation
  */
static const char *decode_jpeg(uint32_t start, int o, uint32_t *preview,
                               int *found_o)
{
  static uint8_t seg[1024];
  bool ready = false;

  if (f_lseek(&file, start) != FR_OK)
  {
    return "READ ERROR";
  }

  io_pos = io_len = 0U;
  io_error = false;
  mcu_need = mcu_fill = 0U;
  mcu_x = mcu_y = 0;
  feed_word = feed_count = 0U;
  progress_at = HAL_GetTick();

  if ((get_byte() != 0xFF) || (get_byte() != 0xD8))
  {
    return "BAD JPG";
  }

  __HAL_RCC_JPEG_CLK_ENABLE();
  __HAL_RCC_JPEG_FORCE_RESET();
  __HAL_RCC_JPEG_RELEASE_RESET();

  JPEG->CR     = JPEG_CR_JCEN;
  JPEG->CONFR0 = 0U;
  JPEG->CR    |= JPEG_CR_IFF | JPEG_CR_OFF;
  JPEG->CFR    = JPEG_CFR_CEOCF | JPEG_CFR_CHPDF;
  JPEG->CONFR1 = JPEG_CONFR1_DE | JPEG_CONFR1_HDR;
  JPEG->CONFR0 = JPEG_CONFR0_START;

  (void)feed(0xFFU);
  (void)feed(0xD8U);

  /* Headers: pass on what the codec needs, look inside SOF and Exif. */
  while (!ready)
  {
    int      marker;
    uint32_t len;
    uint32_t keep;
    int      b;

    do { b = get_byte(); } while ((b >= 0) && (b != 0xFF));
    do { marker = get_byte(); } while (marker == 0xFF);

    if ((b < 0) || (marker <= 0)) { return "BAD JPG"; }

    b = get_byte();
    len = (uint32_t)b << 8;
    marker &= 0xFF;
    if (b < 0) { return "BAD JPG"; }
    b = get_byte();
    if (b < 0) { return "BAD JPG"; }
    len |= (uint32_t)b;
    if (len < 2U) { return "BAD JPG"; }
    len -= 2U;

    if ((marker >= 0xC1) && (marker <= 0xCF) && (marker != 0xC4) &&
        (marker != 0xC8) && (marker != 0xCC))
    {
      return (marker == 0xC2) ? "PROGRESSIVE JPG" : "JPG NOT SUPPORTED";
    }

    if ((marker == 0xC0) || (marker == 0xC4) || (marker == 0xDB) ||
        (marker == 0xDD) || (marker == 0xDA))
    {
      (void)feed(0xFFU);
      (void)feed((uint8_t)marker);
      (void)feed((uint8_t)((len + 2U) >> 8));
      (void)feed((uint8_t)(len + 2U));

      for (uint32_t i = 0U; i < len; i++)
      {
        b = get_byte();
        if (b < 0) { return "BAD JPG"; }
        if (i < sizeof(seg)) { seg[i] = (uint8_t)b; }
        if (!feed((uint8_t)b)) { return "DECODE ERROR"; }
      }
    }
    else
    {
      uint32_t at = (uint32_t)f_tell(&file) - (io_len - io_pos);

      keep = (len < sizeof(seg)) ? len : sizeof(seg);

      if ((marker != 0xE1) && (marker != 0xE2)) { keep = 0U; }

      for (uint32_t i = 0U; i < keep; i++)
      {
        b = get_byte();
        if (b < 0) { return "BAD JPG"; }
        seg[i] = (uint8_t)b;
      }

      if ((marker == 0xE1) && (found_o != 0))
      {
        int e = exif_orient(seg, keep);

        if (e != 1) { o = e; *found_o = e; }
      }

      if ((marker == 0xE2) && (preview != 0))
      {
        *preview = mpf_preview(seg, keep, at + 4U);

        if (*preview != 0U) { return ""; }
      }

      skip_bytes(len - keep);
    }

    if (marker == 0xC0)
    {
      if (len < 9U) { return "BAD JPG"; }

      jh    = ((int)seg[1] << 8) | seg[2];
      jw    = ((int)seg[3] << 8) | seg[4];
      jcomp = seg[5];
      jhs   = seg[7] >> 4;
      jvs   = seg[7] & 0x0F;

      if ((seg[0] != 8U) || (jw < 1) || (jh < 1) ||
          ((jcomp != 1) && (jcomp != 3)))
      {
        return "JPG NOT SUPPORTED";
      }

      if (jcomp == 1)
      {
        jhs = jvs = 1;
      }
      else if ((len < 15U) || (seg[10] != 0x11U) || (seg[13] != 0x11U) ||
               (jhs < 1) || (jhs > 2) || (jvs < 1) || (jvs > 2))
      {
        return "JPG NOT SUPPORTED";
      }

      mcu_need = (uint32_t)((jhs * jvs) + ((jcomp == 3) ? 2 : 0)) * 16U;
      fit(jw, jh, o);
    }

    ready = (marker == 0xDA);
  }

  if (mcu_need == 0U)
  {
    return "BAD JPG";
  }

  fit(jw, jh, o);

  /* Compressed data straight through; the codec finds the end itself. */
  for (;;)
  {
    int b = get_byte();

    if ((b < 0) || !feed((uint8_t)b) || (mcu_y >= src_h))
    {
      break;
    }
  }

  while ((feed_count != 0U) && (mcu_y < src_h))
  {
    if (!feed(0U)) { break; }
  }

  progress_at = HAL_GetTick();

  while ((mcu_y < src_h) && ((HAL_GetTick() - progress_at) < 500U))
  {
    drain();
  }

  drain();

  if (io_error)      { return "READ ERROR"; }
  if (mcu_y < src_h) { return "DECODE ERROR"; }

  return 0;
}

/** The embedded preview if there is one (cameras), else the whole photo. */
static const char *show_jpeg(void)
{
  uint32_t    preview = 0U;
  int         o       = 1;
  const char *error   = decode_jpeg(0U, 1, &preview, &o);

  if ((error != 0) && (error[0] == '\0'))
  {
    if (decode_jpeg(preview, o, 0, 0) == 0)
    {
      return 0;
    }

    Gfx_Fill(canvas, 0, 0, GFX_W, GFX_H, 0x0000U);
    error = decode_jpeg(0U, o, 0, &o);
  }

  return error;
}

/* -------------------------------------------------------------------------- */
/* Showing                                                                    */
/* -------------------------------------------------------------------------- */

const char *Photo_Show(int i, uint16_t *fb)
{
  const char *path = Photo_Path(i);
  const char *error;

  Gfx_Fill(fb, 0, 0, GFX_W, GFX_H, 0x0000U);
  canvas = fb;

  if (path[0] == '\0')                          { return "NO PHOTO"; }
  if (!mount())                                 { return "CARD NOT READABLE"; }
  if (f_open(&file, path, FA_READ) != FR_OK)    { return "CANNOT OPEN"; }

  if (has_ext(path, ".BMP"))
  {
    error = show_bmp();
  }
  else
  {
    error = show_jpeg();
    JPEG->CONFR0 = 0U;
    JPEG->CR     = 0U;
  }

  (void)f_close(&file);
  return error;
}
