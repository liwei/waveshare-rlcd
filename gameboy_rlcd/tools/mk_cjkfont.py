#!/usr/bin/env python3
"""Build the CJK glyph blob embedded in the firmware.

The UI's 5x7 font only covers ASCII, so a Chinese ROM filename would render as
placeholders. This extracts the characters a filename is likely to use from GNU
Unifont (16x16 bitmaps, SIL OFL + GPL font embedding exception) and writes them
as a small binary blob.

Blob layout, little-endian:
    uint32  count
    uint16  codepoints[count]     sorted, for binary search
    uint8   glyphs[count][32]     16 rows of 2 bytes, MSB = leftmost

Usage: mk_cjkfont.py <unifont.hex> <out.bin> [--all-gb2312]
"""
import struct
import sys


def gb2312_chars(include_level2):
    """Every character GB2312 can encode.

    Level 1 (0xB0A1-0xD7F9) is the 3755 common characters; level 2 holds 3008
    rarer ones. Symbol rows 0xA1-0xA9 carry the punctuation.
    """
    chars = set()
    for hi in range(0xA1, 0xF8):
        if not include_level2 and hi >= 0xD8:
            continue
        for lo in range(0xA1, 0xFF):
            try:
                chars.add(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                continue
    return chars


def load_unifont(path):
    """Returns {codepoint: bytes} for the 16x16 glyphs."""
    glyphs = {}
    with open(path, "r", encoding="ascii", errors="ignore") as fh:
        for line in fh:
            line = line.strip()
            if not line or ":" not in line:
                continue
            code_hex, bitmap_hex = line.split(":", 1)
            if len(bitmap_hex) != 64:  # 32 bytes: the 16x16 cells only
                continue
            try:
                code = int(code_hex, 16)
            except ValueError:
                continue
            glyphs[code] = bytes.fromhex(bitmap_hex)
    return glyphs


def main():
    if len(sys.argv) < 3:
        sys.stderr.write(__doc__)
        return 1

    unifont_path, out_path = sys.argv[1], sys.argv[2]
    include_level2 = "--all-gb2312" in sys.argv

    wanted = gb2312_chars(include_level2)
    font = load_unifont(unifont_path)
    sys.stderr.write(f"unifont: {len(font)} 16x16 glyphs, {len(wanted)} chars wanted\n")

    selected = []
    for ch in wanted:
        code = ord(ch)
        if code in font:
            selected.append((code, font[code]))
    selected.sort(key=lambda item: item[0])

    missing = len(wanted) - len(selected)
    sys.stderr.write(f"matched {len(selected)} glyphs ({missing} absent from unifont)\n")

    blob = struct.pack("<I", len(selected))
    blob += b"".join(struct.pack("<H", code) for code, _ in selected)
    blob += b"".join(glyph for _, glyph in selected)

    with open(out_path, "wb") as fh:
        fh.write(blob)

    sys.stderr.write(f"wrote {out_path}: {len(blob)} bytes ({len(blob) // 1024} KB)\n")
    return 0


sys.exit(main())
