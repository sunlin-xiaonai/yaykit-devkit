#!/usr/bin/env python3
"""Builds the `fonts` partition image: a pack of TrueType subsets, one per script, with the index cjk_font reads
(yk_font_pack.h). The first font starts at offset 0, so firmware that only reads one font keeps working.

    python3 gen_font_pack.py --sc NotoSansSC-Regular.ttf [--ja NotoSansJP-Regular.ttf] [--unihan Unihan.zip] \
        [--zh tgh6500|gb2312|gb2312-1] [--ja-set joyo|jis1] [--extra extra_chars.txt] [--scan ../../..] \
        [--compare tikpal_sc.ttf] [--size 0x300000] --out cjk/yaykit_fonts.bin

Needs fontTools. Sources (SIL OFL 1.1, like the current subset):
- Noto Sans SC: the copy LVGL ships, managed_components/lvgl__lvgl/tests/src/test_files/fonts/noto/;
- Noto Sans JP: a static TrueType-outline (glyf) Regular from the Noto CJK or Google Fonts releases (TinyTTF reads
  glyf outlines; a CFF/OTF one would have to be converted first).

Coverage, both fonts: ASCII, Latin-1 (es: ñ ¿ ¡ and the accents), punctuation, € ™, CJK symbols, kana with ー
and ・, full-width forms. Then:
- zh, --zh tgh6500 (needs --unihan): the 6,500 characters of levels 1 and 2 of the Table of General Standard
  Chinese Characters (通用规范汉字表, 2013; Unihan kTGH 1-6500), the list schools use, plus GB2312's symbol rows.
  --zh gb2312: all of GB2312 (6,763 hanzi); --zh gb2312-1: its 3,755 level-1 hanzi.
- ja, --ja-set joyo (needs --unihan): the 2,136 jōyō kanji (Unihan kJoyoKanji), half-width katakana and JIS X
  0208's symbol rows, drawn the Japanese way. --ja-set jis1: JIS X 0208 level 1 (2,965 kanji) instead.
  --ja-set kana-extra (no Unihan): kana, half-width katakana and only the JIS X 0208 level-1 kanji that the
  zh font lacks (込 働 畑, the shinjitai forms); kanji the two share are drawn the Chinese way, from the zh font.
--compare old.ttf (with --unihan): how many of the table's 8,105 characters the old font and the new zh font have,
and which are lost and gained. --keep old.ttf (or a .txt of its characters, cjk/tikpal_sc_chars.txt): the zh font
also takes every CJK character the old font had (its level-3 characters and radicals like 亻 扌), so nothing that
showed before goes missing. --ja-no-latin: the ja font
leaves out ASCII, Latin-1 and the full-width forms; they come from the zh font behind it (Noto Sans SC and JP
draw them alike), which makes room.
- Plus any characters in --extra (one or more per line, # comments) in every font, and with --scan every CJK, kana
  or full-width character in the firmware's C sources under that directory (the words on screens, 骰 in 骰子 say) in
  the zh font: the screens are written in Chinese, and a Japanese screen finds them there behind its own font.
"""
import argparse
import io
import os
import struct
import sys
import zlib

TAIL = 64
ALIGN = 4096  # a flash sector: each font starts on one


def common():
    chars = set(range(0x20, 0x7F))           # ASCII
    chars.update(range(0xA0, 0x100))          # Latin-1
    chars.update(range(0x2010, 0x2028))       # dashes, quotes, ellipsis
    chars.update(range(0x2030, 0x203B))       # per mille, primes, guillemets
    chars.update([0x20AC, 0x2122])            # € ™
    chars.update(range(0x3000, 0x3040))       # CJK symbols and punctuation
    chars.update(range(0x3040, 0x3100))       # hiragana, katakana, ー (30FC), ・ (30FB)
    chars.update(range(0xFF01, 0xFF61))       # full-width forms
    return chars


def double_byte(codec, first_rows, seconds=range(0xA1, 0xFF)):
    out = set()
    for b1 in first_rows:
        for b2 in seconds:
            try:
                out.add(ord(bytes([b1, b2]).decode(codec)))
            except UnicodeDecodeError:
                pass
    return out


def read_unihan(path):
    """{field: {code point: value}} for kTGH and kJoyoKanji, from Unihan.zip or a directory of its .txt files."""
    import zipfile
    fields = {"kTGH": {}, "kJoyoKanji": {}}

    def lines():
        if zipfile.is_zipfile(path):
            with zipfile.ZipFile(path) as z:
                for name in z.namelist():
                    if name.endswith(".txt"):
                        yield from z.read(name).decode("utf-8").splitlines()
        else:
            for name in sorted(os.listdir(path)):
                if name.endswith(".txt"):
                    with open(os.path.join(path, name), encoding="utf-8") as fh:
                        yield from fh.read().splitlines()

    for line in lines():
        if not line.startswith("U+"):
            continue
        cp, field, value = line.split("\t", 2)
        if field in fields:
            fields[field][int(cp[2:], 16)] = value
    return fields


def tgh_index(value):
    """kTGH "2013:1234" (one or more): the character's place in the 2013 table."""
    return min(int(v.split(":")[1]) for v in value.split() if v.startswith("2013:"))


def zh_chars(which="gb2312-1", unihan=None):
    symbols = double_byte("gb2312", range(0xA1, 0xAA))  # GB2312's symbol rows A1-A9
    if which == "tgh6500":
        if not unihan:
            sys.exit("--zh tgh6500 needs --unihan")
        return common() | symbols | {cp for cp, v in unihan["kTGH"].items() if tgh_index(v) <= 6500}
    last = 0xF8 if which == "gb2312" else 0xD8  # level 1 is rows B0-D7, level 2 D8-F7
    return common() | symbols | double_byte("gb2312", range(0xB0, last))


def ja_chars(which="jis1", unihan=None, zh=frozenset()):
    chars = common() | double_byte("euc_jp", range(0xA1, 0xA9))  # JIS X 0208's symbol rows
    chars.update(range(0xFF61, 0xFFA0))       # half-width katakana
    if which == "kana-extra":
        kanji = double_byte("euc_jp", range(0xB0, 0xD0))  # level 1: rows 16-47
        return set(range(0x3000, 0x3100)) | set(range(0xFF61, 0xFFA0)) | (kanji - set(zh))
    if which == "joyo":
        if not unihan:
            sys.exit("--ja-set joyo needs --unihan")
        # kJoyoKanji: a year for the 2,136 characters on the list, U+ (the listed form) for variants of them.
        joyo = {cp for cp, v in unihan["kJoyoKanji"].items() if not v.startswith("U+")}
        return chars | joyo
    return chars | double_byte("euc_jp", range(0xB0, 0xD0))  # level 1: rows 16-47


def subset(path, chars):
    from fontTools import subset as ft_subset
    from fontTools.ttLib import TTFont

    font = TTFont(path, recalcTimestamp=False)  # the source's own date: the same inputs give the same bytes
    if "glyf" not in font:
        sys.exit(f"{path}: not TrueType outlines (glyf); TinyTTF needs those")
    cmap = font.getBestCmap()
    wanted = sorted(c for c in chars if c in cmap)
    missing = sorted(c for c in chars if c not in cmap and c >= 0x80)
    opts = ft_subset.Options()
    opts.hinting = False            # TinyTTF does not hint
    opts.layout_features = []       # nor shape: GSUB/GPOS out
    opts.name_IDs = [0, 1, 2, 3, 4, 5, 6, 13, 14]  # 13, 14: the license and its URL travel with the font
    opts.notdef_outline = True
    opts.glyph_names = False
    sub = ft_subset.Subsetter(opts)
    sub.populate(unicodes=wanted)
    sub.subset(font)
    buf = io.BytesIO()
    font.save(buf)
    data = buf.getvalue()
    if data[:4] != b"\x00\x01\x00\x00":
        sys.exit(f"{path}: the subset is not a TrueType file")
    return data, len(wanted), missing


def pack(fonts, size):
    """fonts: [(lang, bytes)] -> the partition image, `size` bytes, 0xFF where nothing is written."""
    image = bytearray(b"\xff" * size)
    entries, at = [], 0
    for lang, data in fonts:
        at = (at + ALIGN - 1) // ALIGN * ALIGN
        if at + len(data) > size - TAIL:
            sys.exit(f"the fonts need {at + len(data) + TAIL} bytes; the partition has {size}")
        image[at:at + len(data)] = data
        entries.append((lang, at, len(data)))
        at += len(data)
    head = bytearray(b"YKFP") + bytes([1, len(entries), 0, 0])
    for lang, off, n in entries:
        head += lang.encode("ascii")[:4].ljust(4, b"\0") + struct.pack("<II", off, n)
    head += b"\0" * (56 - len(head))
    tail = bytes(head) + struct.pack("<I", zlib.crc32(bytes(head)) & 0xFFFFFFFF) + b"\0" * 4
    image[size - TAIL:] = tail
    return bytes(image), entries, at


def compare(old_path, new_zh, unihan):
    """Coverage of the 8,105 characters of the 2013 table: the old font against the new zh font, by level."""
    from fontTools.ttLib import TTFont

    old = set(TTFont(old_path).getBestCmap())
    new = set(TTFont(io.BytesIO(new_zh)).getBestCmap())
    table = {cp: tgh_index(v) for cp, v in unihan["kTGH"].items()}

    def level(i):
        return 1 if i <= 3500 else 2 if i <= 6500 else 3

    print(f"of the {len(table)} characters of the 2013 table: old {sum(c in old for c in table)}, "
          f"new {sum(c in new for c in table)}")
    for lv in (1, 2, 3):
        chars = [c for c, i in table.items() if level(i) == lv]
        print(f"  level {lv} ({len(chars)}): old {sum(c in old for c in chars)}, new {sum(c in new for c in chars)}")
    lost = sorted((c for c in table if c in old and c not in new), key=table.get)
    gained = sorted((c for c in table if c in new and c not in old), key=table.get)
    print(f"  lost {len(lost)}: {''.join(map(chr, lost[:60]))}{' ...' if len(lost) > 60 else ''}")
    print(f"  gained {len(gained)}: {''.join(map(chr, gained[:60]))}{' ...' if len(gained) > 60 else ''}")
    other = sorted(c for c in old if c >= 0x3400 and c not in table and c not in new)
    if other:
        print(f"  lost outside the table: {len(other)}: {''.join(map(chr, other[:40]))}")


def scan_sources(root):
    """Every character from U+2E80 up in .c/.h files under root (registry components, builds and tests left out)."""
    chars = set()
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in ("managed_components", "build", "test") and not d.startswith(".")]
        for name in filenames:
            if name.endswith((".c", ".h")):
                with open(os.path.join(dirpath, name), encoding="utf-8", errors="ignore") as fh:
                    chars.update(ord(c) for c in fh.read() if ord(c) >= 0x2E80)
    return chars


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sc", required=True)
    ap.add_argument("--ja")
    ap.add_argument("--unihan", help="Unihan.zip (or its directory) from unicode.org")
    ap.add_argument("--zh", default="gb2312-1", choices=["tgh6500", "gb2312", "gb2312-1"])
    ap.add_argument("--ja-set", default="jis1", choices=["joyo", "jis1", "kana-extra"])
    ap.add_argument("--compare", help="the font the pack replaces: coverage of the 8,105 characters (needs --unihan)")
    ap.add_argument("--keep", help="the font the pack replaces: the zh font keeps every CJK character it had")
    ap.add_argument("--ja-no-latin", action="store_true", help="Latin and full-width forms from the zh font only")
    ap.add_argument("--extra")
    ap.add_argument("--scan", help="the firmware directory: add the characters its C sources use")
    ap.add_argument("--size", default="0x300000")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    extra = set()
    if a.extra:
        for line in open(a.extra, encoding="utf-8"):
            if not line.startswith("#"):
                extra.update(ord(c) for c in line.strip())
    used = scan_sources(a.scan) if a.scan else set()
    if a.scan:
        print(f"{len(used)} characters in the sources under {a.scan}")
    unihan = read_unihan(a.unihan) if a.unihan else None
    if unihan:
        joyo = sum(1 for v in unihan["kJoyoKanji"].values() if not v.startswith("U+"))
        print(f"Unihan: {len(unihan['kTGH'])} kTGH characters, {joyo} jōyō kanji")
    zh_set = zh_chars(a.zh, unihan) | used
    if a.keep:
        from fontTools.ttLib import TTFont

        if a.keep.endswith(".txt"):
            with open(a.keep, encoding="utf-8") as fh:
                kept = {ord(c) for line in fh if not line.startswith("#") for c in line.strip()}
        else:
            kept = {c for c in TTFont(a.keep).getBestCmap() if c >= 0x2E80}
        print(f"keeping {len(kept - zh_set)} more characters the old font had")
        zh_set |= kept
    fonts = []
    ja_set = ja_chars(a.ja_set, unihan, zh_set)
    if a.ja_set == "kana-extra":
        ja_set -= used
    if a.ja_no_latin:
        ja_set = {c for c in ja_set if not (c < 0x100 or 0xFF01 <= c <= 0xFF60)}
    for lang, path, chars in (("zh", a.sc, zh_set), ("ja", a.ja, ja_set)):
        if not path:
            continue
        data, n, missing = subset(path, chars | extra)
        print(f"{lang}: {n} characters, {len(data)} bytes" + (f"; not in the source: {len(missing)}" if missing else ""))
        fonts.append((lang, data))
    if a.compare and unihan:
        compare(a.compare, dict(fonts)["zh"], unihan)
    image, entries, used = pack(fonts, int(a.size, 0))
    with open(a.out, "wb") as f:
        f.write(image)
    for lang, off, n in entries:
        print(f"  {lang} at 0x{off:06x}, {n} bytes")
    print(f"{used + TAIL} of {len(image)} bytes used -> {a.out}")


if __name__ == "__main__":
    main()
