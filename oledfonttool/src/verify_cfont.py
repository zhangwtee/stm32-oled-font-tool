# -*- coding: utf-8 -*-
"""校验字库文件是否合法，以及两份文件是否逐字节一致

用法：
    python verify_cfont.py                        # 自动查找工程里的字库
    python verify_cfont.py <文件A> [<文件B>]       # 指定文件；给两个就比对一致性

只给一个文件：检查格式（每行字节数一致、UTF-8 编码唯一、结尾完整）。
给两个文件：额外比对两者是否逐字节相同。

字号不用指定 —— 从文件里的 OLED_CFONT_STRIDE 宏或数组列数自动推断，
16×16 / 24×24 / 32×32 都能校验。

自动查找：从本脚本所在目录向上找 STM32 工程（按 .uvprojx / .ewp / .cproject
判断，与 add_chars.py 用同一套逻辑），取其中的 OLED_CFont.h；
以及同目录（oledfonttool\\src）下的 OLED_CFont.h。
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

PAT = re.compile(r"0x([0-9A-Fa-f]{2})")


def find_project_font(start):
    """向上找 STM32 工程，返回工程里的 OLED_CFont.h。

    定位逻辑直接复用 add_chars.find_hw_dir() —— 认工程文件，不认驱动文件名，
    免得两个脚本各写一套、改一处漏一处。
    """
    try:
        from add_chars import find_hw_dir
    except ImportError:
        return None

    d = find_hw_dir(start)
    if not d:
        return None
    font = os.path.join(d, "OLED_CFont.h")
    return font if os.path.isfile(font) else None


def find_up(start, rel):
    """从 start 目录向上逐级查找 rel，找不到返回 None"""
    d = os.path.abspath(start)
    for _ in range(6):
        p = os.path.join(d, rel)
        if os.path.exists(p):
            return p
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent
    return None


def parse_rows(path):
    """读出所有字模行（以 0x / 0X 开头的那些）"""
    with open(path, "r", encoding="utf-8-sig") as f:
        text = f.read()
    rows = [line.strip() for line in text.split("\n")
            if line.strip().lower().startswith("0x")]
    return rows, text


def detect_stride(rows, text):
    """一行多少字节：优先读宏，其次看数组列数，最后数第一行"""
    m = re.search(r"#define\s+OLED_CFONT_STRIDE\s+(\d+)", text)
    if m:
        return int(m.group(1))
    m = re.search(r"\[\s*\]\s*\[\s*(\d+)\s*\]", text)
    if m:
        return int(m.group(1))
    return len(PAT.findall(rows[0])) if rows else 0


def size_of_stride(stride):
    """行字节数反推边长：stride = 3 + size*size/8"""
    per = stride - 3
    for size in (8, 16, 24, 32, 40, 48, 56, 64):
        if size * size // 8 == per:
            return size
    return None


def check_one(path, label):
    if not path or not os.path.exists(path):
        print("  [%s] 文件不存在: %s" % (label, path))
        return None
    try:
        rows, text = parse_rows(path)
    except Exception as e:
        print("  [%s] 读取失败: %s" % (label, e))
        return None
    if not rows:
        print("  [%s] 没有找到任何字模行" % label)
        return None

    stride = detect_stride(rows, text)
    size = size_of_stride(stride)

    bad = [(i + 1, len(PAT.findall(r))) for i, r in enumerate(rows)
           if len(PAT.findall(r)) != stride]

    codes = []
    for r in rows:
        b = PAT.findall(r)[:3]
        try:
            codes.append(bytes(int(x, 16) for x in b).decode("utf-8"))
        except Exception:
            codes.append(None)
    valid = [c for c in codes if c]
    ok_uniq = len(set(valid)) == len(valid)

    print("  [%s] %s" % (label, path))
    print("        字号           : %s" % ("%dx%d" % (size, size) if size else "未知"))
    print("        字模行数       : %d" % len(rows))
    print("        每行 %d 字节   : %s" % (stride,
                                          "是" if not bad else "否 %s" % bad[:5]))
    print("        UTF-8 编码唯一 : %s" % ("是" if ok_uniq else "否"))
    print("        结尾 #endif    : %s" % ("有" if text.rstrip().endswith("#endif") else "无"))

    if bad or not ok_uniq:
        return None
    return rows


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    here = os.path.dirname(os.path.abspath(__file__))

    if len(args) >= 2:
        a, b = args[0], args[1]
    elif len(args) == 1:
        a, b = args[0], None
    else:
        a = find_project_font(here)
        b = find_up(here, "OLED_CFont.h")
        if a is None and b is None:
            print("没找到字库文件。请把路径作为参数传进来：")
            print("    python verify_cfont.py <文件A> [<文件B>]")
            return 1

    print("校验字库：")
    ra = check_one(a, "工程") if a else None
    rb = check_one(b, "生成") if b else None

    print()
    if ra and rb:
        if ra == rb:
            print("两份文件逐字节一致: 是")
        else:
            diff = [i + 1 for i, (x, y) in enumerate(zip(ra, rb)) if x != y]
            print("两份文件逐字节一致: 否")
            print("  不同的行号(前 10): %s" % diff[:10])
    elif ra or rb:
        print("只检查了一份文件的格式。")
    else:
        print("没有可用的字库文件。")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
