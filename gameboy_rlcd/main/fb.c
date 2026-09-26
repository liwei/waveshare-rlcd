#include "fb.h"

#include <string.h>

#include "config.h"

static uint8_t s_fb[LCD_FB_SIZE];

/* 5x7 bitmap font covering ASCII 0x20..0x7E, one byte per column with bit 0 at
 * the top. Classic Nokia 5110 / Adafruit LCD font, public domain, carried over
 * from PaperBoy's ui.c. */
static const uint8_t s_font5x7[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, /* 0x20 ' '  */
    {0x00, 0x00, 0x5F, 0x00, 0x00}, /* 0x21 '!'  */
    {0x00, 0x07, 0x00, 0x07, 0x00}, /* 0x22 '"'  */
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, /* 0x23 '#'  */
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, /* 0x24 '$'  */
    {0x23, 0x13, 0x08, 0x64, 0x62}, /* 0x25 '%'  */
    {0x36, 0x49, 0x55, 0x22, 0x50}, /* 0x26 '&'  */
    {0x00, 0x05, 0x03, 0x00, 0x00}, /* 0x27 '\'' */
    {0x00, 0x1C, 0x22, 0x41, 0x00}, /* 0x28 '('  */
    {0x00, 0x41, 0x22, 0x1C, 0x00}, /* 0x29 ')'  */
    {0x08, 0x2A, 0x1C, 0x2A, 0x08}, /* 0x2A '*'  */
    {0x08, 0x08, 0x3E, 0x08, 0x08}, /* 0x2B '+'  */
    {0x00, 0x50, 0x30, 0x00, 0x00}, /* 0x2C ','  */
    {0x08, 0x08, 0x08, 0x08, 0x08}, /* 0x2D '-'  */
    {0x00, 0x60, 0x60, 0x00, 0x00}, /* 0x2E '.'  */
    {0x20, 0x10, 0x08, 0x04, 0x02}, /* 0x2F '/'  */
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* 0x30 '0'  */
    {0x00, 0x42, 0x7F, 0x40, 0x00}, /* 0x31 '1'  */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* 0x32 '2'  */
    {0x21, 0x41, 0x45, 0x4B, 0x31}, /* 0x33 '3'  */
    {0x18, 0x14, 0x12, 0x7F, 0x10}, /* 0x34 '4'  */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* 0x35 '5'  */
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* 0x36 '6'  */
    {0x01, 0x71, 0x09, 0x05, 0x03}, /* 0x37 '7'  */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* 0x38 '8'  */
    {0x06, 0x49, 0x49, 0x29, 0x1E}, /* 0x39 '9'  */
    {0x00, 0x36, 0x36, 0x00, 0x00}, /* 0x3A ':'  */
    {0x00, 0x56, 0x36, 0x00, 0x00}, /* 0x3B ';'  */
    {0x08, 0x14, 0x22, 0x41, 0x00}, /* 0x3C '<'  */
    {0x14, 0x14, 0x14, 0x14, 0x14}, /* 0x3D '='  */
    {0x00, 0x41, 0x22, 0x14, 0x08}, /* 0x3E '>'  */
    {0x02, 0x01, 0x51, 0x09, 0x06}, /* 0x3F '?'  */
    {0x32, 0x49, 0x79, 0x41, 0x3E}, /* 0x40 '@'  */
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* 0x41 'A'  */
    {0x7F, 0x49, 0x49, 0x49, 0x36}, /* 0x42 'B'  */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* 0x43 'C'  */
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* 0x44 'D'  */
    {0x7F, 0x49, 0x49, 0x49, 0x41}, /* 0x45 'E'  */
    {0x7F, 0x09, 0x09, 0x09, 0x01}, /* 0x46 'F'  */
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, /* 0x47 'G'  */
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* 0x48 'H'  */
    {0x00, 0x41, 0x7F, 0x41, 0x00}, /* 0x49 'I'  */
    {0x20, 0x40, 0x41, 0x3F, 0x01}, /* 0x4A 'J'  */
    {0x7F, 0x08, 0x14, 0x22, 0x41}, /* 0x4B 'K'  */
    {0x7F, 0x40, 0x40, 0x40, 0x40}, /* 0x4C 'L'  */
    {0x7F, 0x02, 0x04, 0x02, 0x7F}, /* 0x4D 'M'  */
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* 0x4E 'N'  */
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* 0x4F 'O'  */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* 0x50 'P'  */
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* 0x51 'Q'  */
    {0x7F, 0x09, 0x19, 0x29, 0x46}, /* 0x52 'R'  */
    {0x46, 0x49, 0x49, 0x49, 0x31}, /* 0x53 'S'  */
    {0x01, 0x01, 0x7F, 0x01, 0x01}, /* 0x54 'T'  */
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* 0x55 'U'  */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* 0x56 'V'  */
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, /* 0x57 'W'  */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* 0x58 'X'  */
    {0x07, 0x08, 0x70, 0x08, 0x07}, /* 0x59 'Y'  */
    {0x61, 0x51, 0x49, 0x45, 0x43}, /* 0x5A 'Z'  */
    {0x00, 0x7F, 0x41, 0x41, 0x00}, /* 0x5B '['  */
    {0x02, 0x04, 0x08, 0x10, 0x20}, /* 0x5C '\\' */
    {0x00, 0x41, 0x41, 0x7F, 0x00}, /* 0x5D ']'  */
    {0x04, 0x02, 0x01, 0x02, 0x04}, /* 0x5E '^'  */
    {0x40, 0x40, 0x40, 0x40, 0x40}, /* 0x5F '_'  */
    {0x00, 0x01, 0x02, 0x04, 0x00}, /* 0x60 '`'  */
    {0x20, 0x54, 0x54, 0x54, 0x78}, /* 0x61 'a'  */
    {0x7F, 0x48, 0x44, 0x44, 0x38}, /* 0x62 'b'  */
    {0x38, 0x44, 0x44, 0x44, 0x20}, /* 0x63 'c'  */
    {0x38, 0x44, 0x44, 0x48, 0x7F}, /* 0x64 'd'  */
    {0x38, 0x54, 0x54, 0x54, 0x18}, /* 0x65 'e'  */
    {0x08, 0x7E, 0x09, 0x01, 0x02}, /* 0x66 'f'  */
    {0x08, 0x54, 0x54, 0x54, 0x3C}, /* 0x67 'g'  */
    {0x7F, 0x08, 0x04, 0x04, 0x78}, /* 0x68 'h'  */
    {0x00, 0x44, 0x7D, 0x40, 0x00}, /* 0x69 'i'  */
    {0x20, 0x40, 0x44, 0x3D, 0x00}, /* 0x6A 'j'  */
    {0x7F, 0x10, 0x28, 0x44, 0x00}, /* 0x6B 'k'  */
    {0x00, 0x41, 0x7F, 0x40, 0x00}, /* 0x6C 'l'  */
    {0x7C, 0x04, 0x18, 0x04, 0x7C}, /* 0x6D 'm'  */
    {0x7C, 0x08, 0x04, 0x04, 0x78}, /* 0x6E 'n'  */
    {0x38, 0x44, 0x44, 0x44, 0x38}, /* 0x6F 'o'  */
    {0x7C, 0x14, 0x14, 0x14, 0x08}, /* 0x70 'p'  */
    {0x08, 0x14, 0x14, 0x18, 0x7C}, /* 0x71 'q'  */
    {0x7C, 0x08, 0x04, 0x04, 0x08}, /* 0x72 'r'  */
    {0x48, 0x54, 0x54, 0x54, 0x20}, /* 0x73 's'  */
    {0x04, 0x3F, 0x44, 0x40, 0x20}, /* 0x74 't'  */
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, /* 0x75 'u'  */
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, /* 0x76 'v'  */
    {0x3C, 0x40, 0x20, 0x40, 0x3C}, /* 0x77 'w'  */
    {0x44, 0x28, 0x10, 0x28, 0x44}, /* 0x78 'x'  */
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, /* 0x79 'y'  */
    {0x44, 0x64, 0x54, 0x4C, 0x44}, /* 0x7A 'z'  */
    {0x00, 0x08, 0x36, 0x41, 0x00}, /* 0x7B '{'  */
    {0x00, 0x00, 0x7F, 0x00, 0x00}, /* 0x7C '|'  */
    {0x00, 0x41, 0x36, 0x08, 0x00}, /* 0x7D '}'  */
    {0x08, 0x04, 0x08, 0x10, 0x08}, /* 0x7E '~'  */
};

uint8_t *fb_buffer(void) { return s_fb; }

/* ── CJK glyphs ────────────────────────────────────────────────────────────
 *
 * The 5x7 font above is ASCII only, so ROM filenames in Chinese would render
 * as placeholders. This is a 16x16 bitmap subset of GNU Unifont (SIL OFL, with
 * the GNU font embedding exception), generated by tools/mk_cjkfont.py and
 * embedded straight into flash - it is read in place, so it costs no RAM.
 *
 * Layout, little-endian: uint32 count, uint16 codepoints[count] (sorted, for
 * binary search), uint8 glyphs[count][32] (16 rows of 2 bytes, MSB leftmost).
 */
#define CJK_GLYPH_BYTES 32

extern const uint8_t _binary_cjkfont_bin_start[] asm("_binary_cjkfont_bin_start");

static uint32_t cjk_count(void) {
  const uint8_t *p = _binary_cjkfont_bin_start;
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t cjk_code_at(uint32_t index) {
  const uint8_t *p = _binary_cjkfont_bin_start + 4u + index * 2u;
  return (uint16_t)(p[0] | (p[1] << 8));
}

static const uint8_t *cjk_glyph(uint32_t codepoint) {
  if (codepoint > 0xFFFFu) {
    return NULL;
  }

  const uint32_t count = cjk_count();
  uint32_t lo = 0;
  uint32_t hi = count;

  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2u;
    const uint16_t value = cjk_code_at(mid);
    if (value == codepoint) {
      return _binary_cjkfont_bin_start + 4u + count * 2u + mid * CJK_GLYPH_BYTES;
    }
    if (value < codepoint) {
      lo = mid + 1u;
    } else {
      hi = mid;
    }
  }

  return NULL;
}

uint8_t *fb_row(int y) {
  if (y < 0 || y >= LCD_H) {
    return NULL;
  }
  return &s_fb[y * LCD_STRIDE];
}

void fb_pixel(int x, int y, bool ink) {
  if (x < 0 || x >= LCD_W || y < 0 || y >= LCD_H) {
    return;
  }
  uint8_t *p = &s_fb[y * LCD_STRIDE + (x >> 3)];
  const uint8_t mask = (uint8_t)(0x80u >> (x & 7));
  if (ink) {
    *p &= (uint8_t)~mask;
  } else {
    *p |= mask;
  }
}

bool fb_pixel_get(int x, int y) {
  if (x < 0 || x >= LCD_W || y < 0 || y >= LCD_H) {
    return false;
  }
  return (s_fb[y * LCD_STRIDE + (x >> 3)] & (0x80u >> (x & 7))) == 0;
}

void fb_clear(bool ink) {
  memset(s_fb, ink ? 0x00 : 0xFF, sizeof(s_fb));
}

void fb_fill_rect(int x, int y, int w, int h, bool ink) {
  for (int yy = y; yy < y + h; yy++) {
    for (int xx = x; xx < x + w; xx++) {
      fb_pixel(xx, yy, ink);
    }
  }
}

void fb_hline(int x0, int x1, int y, bool ink) {
  for (int x = x0; x <= x1; x++) {
    fb_pixel(x, y, ink);
  }
}

void fb_vline(int x, int y0, int y1, bool ink) {
  for (int y = y0; y <= y1; y++) {
    fb_pixel(x, y, ink);
  }
}

int fb_char(int x, int y, char ch, int scale, bool text_ink, bool bg_ink) {
  const uint8_t idx = (uint8_t)ch;
  const uint8_t *glyph = s_font5x7[(idx < 0x20u || idx > 0x7Eu) ? ('?' - 0x20) : (idx - 0x20u)];

  for (int col = 0; col < FB_CHAR_W; col++) {
    const uint8_t bits = (col < 5) ? glyph[col] : 0x00u;
    for (int row = 0; row < FB_CHAR_H; row++) {
      const bool ink = (row < 7) ? (((bits >> row) & 1u) ? text_ink : bg_ink) : bg_ink;
      fb_fill_rect(x + col * scale, y + row * scale, scale, scale, ink);
    }
  }
  return FB_CHAR_W * scale;
}

// A box standing in for a glyph the font does not have.
static int fb_missing(int x, int y, int scale, bool text_ink, bool bg_ink) {
  const int w = FB_CHAR_W * scale;
  const int h = FB_CHAR_H * scale;

  fb_fill_rect(x, y, w, h, bg_ink);
  fb_hline(x, x + w - 2, y, text_ink);
  fb_hline(x, x + w - 2, y + h - 1, text_ink);
  fb_vline(x, y, y + h - 1, text_ink);
  fb_vline(x + w - 2, y, y + h - 1, text_ink);
  return w;
}

// Draw one 16x16 CJK glyph in the 16-pixel cell a scaled-up ASCII cell occupies.
static int fb_cjk(int x, int y, const uint8_t *glyph, bool text_ink, bool bg_ink) {
  fb_fill_rect(x, y, 16, 16, bg_ink);

  for (int row = 0; row < 16; row++) {
    const uint16_t bits = (uint16_t)((glyph[row * 2] << 8) | glyph[row * 2 + 1]);
    for (int col = 0; col < 16; col++) {
      if (bits & (uint16_t)(1u << (15 - col))) {
        fb_pixel(x + col, y + row, text_ink);
      }
    }
  }

  return 16;
}

// Decodes one UTF-8 sequence at *s, advancing past it. Returns false for a
// malformed sequence, in which case a single byte is consumed.
static bool utf8_decode(const char **s, uint32_t *codepoint) {
  const uint8_t *p = (const uint8_t *)*s;
  const uint8_t lead = p[0];
  int len;
  uint32_t value;

  if ((lead & 0xE0u) == 0xC0u) {
    value = lead & 0x1Fu;
    len = 2;
  } else if ((lead & 0xF0u) == 0xE0u) {
    value = lead & 0x0Fu;
    len = 3;
  } else if ((lead & 0xF8u) == 0xF0u) {
    value = lead & 0x07u;
    len = 4;
  } else {
    *s += 1;
    return false;
  }

  for (int i = 1; i < len; i++) {
    if ((p[i] & 0xC0u) != 0x80u) {
      *s += 1;
      return false;
    }
    value = (value << 6) | (p[i] & 0x3Fu);
  }

  *s += len;
  *codepoint = value;
  return true;
}

// Size of the cell a character occupies, without drawing it.
static int char_advance(uint32_t codepoint, int scale) {
  if (codepoint < 0x80u) {
    return FB_CHAR_W * scale;
  }
  if (scale >= 2 && cjk_glyph(codepoint) != NULL) {
    return 16;
  }
  return FB_CHAR_W * scale;
}

int fb_text(int x, int y, const char *s, int scale, bool text_ink, bool bg_ink) {
  while (*s != '\0') {
    if ((uint8_t)*s < 0x80u) {
      x += fb_char(x, y, *s++, scale, text_ink, bg_ink);
      continue;
    }

    uint32_t codepoint = 0;
    if (!utf8_decode(&s, &codepoint)) {
      x += fb_missing(x, y, scale, text_ink, bg_ink);
      continue;
    }

    const uint8_t *glyph = (scale >= 2) ? cjk_glyph(codepoint) : NULL;
    x += (glyph != NULL) ? fb_cjk(x, y, glyph, text_ink, bg_ink)
                         : fb_missing(x, y, scale, text_ink, bg_ink);
  }

  return x;
}

int fb_text_width(const char *s, int scale) {
  int width = 0;

  while (*s != '\0') {
    if ((uint8_t)*s < 0x80u) {
      s++;
      width += FB_CHAR_W * scale;
      continue;
    }

    uint32_t codepoint = 0;
    if (!utf8_decode(&s, &codepoint)) {
      width += FB_CHAR_W * scale;
      continue;
    }
    width += char_advance(codepoint, scale);
  }

  return width;
}

void fb_battery(int x, int y, int percent, bool charging) {
  if (percent < 0) {
    percent = 0;
  }
  if (percent > 100) {
    percent = 100;
  }

  const int w = 16;
  const int h = 9;

  fb_fill_rect(x, y, w, h, false);
  fb_hline(x, x + w - 1, y, true);
  fb_hline(x, x + w - 1, y + h - 1, true);
  fb_vline(x, y, y + h - 1, true);
  fb_vline(x + w - 1, y, y + h - 1, true);
  fb_fill_rect(x + w, y + 3, 2, h - 6, true);

  const int inner = w - 4;
  const int filled = (inner * percent) / 100;
  if (filled > 0) {
    fb_fill_rect(x + 2, y + 2, filled, h - 4, true);
  }

  if (charging) {
    const int cx = x + w / 2;
    fb_vline(cx, y + 2, y + h - 3, false);
    fb_pixel(cx - 1, y + 4, false);
    fb_pixel(cx + 1, y + h - 5, false);
  }
}
