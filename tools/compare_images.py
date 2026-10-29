#!/usr/bin/env python3
"""Compares two builds of the same app image (yaykit_<slug>.bin).

  compare_images.py <a.bin> <b.bin>

The same source built on a Mac and in Linux (ESP-IDF 5.5.5's container, where the review rebuilds) always differs in
about 64 bytes: the ELF's SHA-256 in esp_app_desc_t (32), and what covers it, the image's one-byte checksum and its
appended SHA-256 (32). Espressif's macOS and Linux toolchain packages carry different paths in their prebuilt
libraries' debug information, so the ELF differs while the code and data do not. Anything else that differs is a
different build.

Exit 0: the same bytes, or the same code (only those differ). Exit 1: different. Standard library only.
"""
import hashlib
import sys

IMAGE_MAGIC = 0xE9
DESC_OFFSET = 0x20             # esp_app_desc_t, after the 24-byte image header and the first 8-byte segment header
DESC_MAGIC = 0xABCD5432
ELF_SHA = range(DESC_OFFSET + 144, DESC_OFFSET + 176)  # esp_app_desc_t.app_elf_sha256


def check(name, data):
    """Why `data` is not a whole ESP app image with its appended SHA-256, or None."""
    if len(data) < 0x200 or data[0] != IMAGE_MAGIC:
        return f"{name} 不是 ESP 应用镜像"
    if int.from_bytes(data[DESC_OFFSET:DESC_OFFSET + 4], "little") != DESC_MAGIC:
        return f"{name} 里找不到应用描述（esp_app_desc_t）"
    if data[23] != 1:
        return f"{name} 末尾没有 SHA-256"
    if hashlib.sha256(data[:-32]).digest() != data[-32:]:
        return f"{name} 末尾的 SHA-256 对不上它的内容：文件坏了"
    return None


def compare(a, b):
    """(exit code, sentence) for two images' bytes."""
    if len(a) != len(b):
        return 1, f"不同：大小不一样（{len(a)} 和 {len(b)} 字节）"
    if a == b:
        return 0, "一字不差"
    tail = range(len(a) - 33, len(a))  # the one-byte checksum, then the appended SHA-256: both cover the ELF hash
    other = [i for i in range(len(a)) if a[i] != b[i] and i not in ELF_SHA and i not in tail]
    if other:
        shown = ", ".join(f"0x{i:x}" for i in other[:8])
        return 1, f"不同：{len(other)} 个字节在 ELF 哈希和末尾两处校验以外（{shown}{' …' if len(other) > 8 else ''}）"
    differ = sum(1 for i in range(len(a)) if a[i] != b[i])
    return 0, (f"代码相同：只差 ELF 哈希和末尾的两处校验（{differ} 个字节）。Mac 和 Linux 上编出来就是这样："
               "乐鑫两个平台的工具链在调试信息里带的路径不一样，代码和数据一字不差")


def main(argv):
    if len(argv) != 3:
        print(__doc__.strip().splitlines()[2].strip())
        return 2
    images = []
    for name in argv[1:]:
        data = open(name, "rb").read()
        bad = check(name, data)
        if bad:
            print(bad)
            return 1
        images.append(data)
    code, sentence = compare(*images)
    print(sentence)
    print(f"  {argv[1]}: {hashlib.sha256(images[0]).hexdigest()}")
    print(f"  {argv[2]}: {hashlib.sha256(images[1]).hexdigest()}")
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv))
