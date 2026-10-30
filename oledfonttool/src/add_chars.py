# -*- coding: utf-8 -*-
"""往已有字库里追加几个汉字，并可选地安装到 Keil 工程

用法：
    python add_chars.py 温度报警              # 只更新 src\\OLED_CFont.h
    python add_chars.py --dir <工程>\\Hardware 温度报警   # 同时装进工程
    python add_chars.py --size 24 你好世界    # 手动指定字号

做的事：
    1. 读现有 OLED_CFont.h（oledfonttool\\src\\）
    2. 用同一套渲染逻辑给新字生成点阵
    3. 合并、按 UTF-8 排序、重新写出
    4. 指定了 --dir、或自动找到了工程，就复制过去（自动备份旧文件）

字号默认从现有字库推断（读 OLED_CFONT_W 或数组的列数），推断不出才按 16。
"""
import glob
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_oled_hz import (glyph, bits_to_bytes, load_font, render_header,   # noqa: E402
                         bytes_per_glyph, check_size)

HERE   = os.path.dirname(os.path.abspath(__file__))
SRC_H  = os.path.join(HERE, "OLED_CFont.h")

HEAD = '''#ifndef __OLED_CFONT_H
#define __OLED_CFONT_H

/*==============================================================================
 *  汉字字模库  %(size)dx%(size)d 点阵，每个汉字 %(per)d 字节
 *
 *  取模格式（与 OLED.c 的 OLED_ShowChinese 对应）：
 *      阴码 / 列行式 / 逆向(低位在前) / C51格式
 *      一个汉字 = %(pages)d 页 × %(size)d 字节（共 %(per)d 字节）
 *
 *  每行的排布：前 3 字节是该字的 UTF-8 编码，后面 %(per)d 字节是点阵。
 *  这里全部写成十六进制字节，汉字只放在注释里。
 *
 *  本文件必须是 UTF-8 编码，且 Keil 里 Edit -> Configuration ->
 *  Encoding 也要选 UTF-8，否则源码里的中文字符串无法和这里的编码对上。
 *============================================================================*/

'''


def detect_size(text):
    """从现有字库里猜字号：优先读 OLED_CFONT_W 宏，其次看数组列数。"""
    m = re.search(r"#define\s+OLED_CFONT_W\s+(\d+)", text)
    if m:
        return check_size(int(m.group(1)))
    m = re.search(r"\[\s*\]\s*\[\s*(\d+)\s*\]", text)
    if m:
        stride = int(m.group(1))
        per = stride - 3
        for size in (16, 24, 32, 40, 48, 56, 64):
            if bytes_per_glyph(size) == per:
                return size
    return 16


def read_existing(path, stride):
    """读出已有的 (汉字, 点阵字节) 列表"""
    if not os.path.exists(path):
        return []
    text = open(path, encoding="utf-8").read()
    rows = []
    pending = None
    pat = r"\s*((?:0x[0-9A-Fa-f]{2},?\s*){%d})" % stride
    for line in text.split("\n"):
        m = re.match(r"\s*/\*\s*(.+?)\s*\*/\s*$", line)
        if m:
            pending = m.group(1)
            continue
        m = re.match(pat, line)
        if m and pending:
            vals = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1))]
            if len(vals) == stride:
                rows.append((pending, vals))
            pending = None
    return rows


# ---------------------------------------------------------------------------
# 工程定位
#
# 老办法认 Hardware\OLED.c 这个文件名，可那只是"某个工程的巧合"：
# 别人的驱动叫 lcd.c / ssd1306.c / Display.c，一样认不出来。
# 现在的判据是工程文件本身，跟源码怎么命名完全无关。
# ---------------------------------------------------------------------------

PROJECT_MARKERS = ("*.uvprojx", "*.uvproj", "*.ewp", ".cproject",
                   "platformio.ini", ".mxproject")

# 字库优先放这些目录，按顺序取第一个存在的
COMMON_DIRS = ("Hardware", "User", "BSP", "Drivers", "App",
               os.path.join("Core", "Src"), "Src", "Source", "Inc")

# 扫描时跳过：构建产物、IDE 缓存，扫了既慢又容易认错
SKIP_DIRS = {"build", "package", "output", "objects", "listings",
             "debugconfig", ".git", ".vs", "node_modules", "__pycache__",
             "tmp", "staging", "rte", ".settings"}


def _has_marker(path):
    for pat in PROJECT_MARKERS:
        if glob.glob(os.path.join(path, pat)):
            return True
    return False


def _find_projects(base, max_depth=3):
    """在 base 下按层找工程文件，返回同一层里所有命中的目录。

    只收同一层：浅层的工程比深层更可能是当前在用的那个。
    一旦某层命中就不再往里钻 —— 工程根内部不会再有工程根。
    """
    level = [base]
    for _ in range(max_depth + 1):
        hits, nxt = [], []
        for p in level:
            if _has_marker(p):
                hits.append(p)
                continue
            try:
                for name in sorted(os.listdir(p)):
                    sub = os.path.join(p, name)
                    if os.path.isdir(sub) and name.lower() not in SKIP_DIRS:
                        nxt.append(sub)
            except OSError:
                pass
        if hits:
            return hits
        level = nxt
    return []


def _find_file_under(base, name, max_depth=3):
    """在 base 下按层找某个文件，返回所在目录。"""
    level = [base]
    for _ in range(max_depth + 1):
        for p in level:
            if os.path.isfile(os.path.join(p, name)):
                return p
        nxt = []
        for p in level:
            try:
                for n in sorted(os.listdir(p)):
                    sub = os.path.join(p, n)
                    if os.path.isdir(sub) and n.lower() not in SKIP_DIRS:
                        nxt.append(sub)
            except OSError:
                pass
        level = nxt
    return None


def _pick_project(roots):
    """多个工程并排时挑一个：已有字库的 > 目录最新的 > 第一个。"""
    if len(roots) == 1:
        return roots[0]
    for r in roots:
        if _find_file_under(r, "OLED_CFont.h", 2):
            return r
    try:
        return max(roots, key=lambda r: os.path.getmtime(r))
    except OSError:
        return roots[0]

def _pick_font_dir(root, strong_only=False):
    """工程根定了，字库写哪层：已有字库 > 常见目录名 > 工程根。

    已有字库必须原地更新 —— 换地方再写一份，工程里就有两个同名头文件，
    谁生效看编译顺序。

    strong_only=True 时，两者都没中就返回 None，让调用方换一个候选根再试。
    这是为了处理 .uvprojx 被放在 MDK-ARM\\ / EWARM\\ 这类子目录的情况：
    那一层里当然没有 Hardware\\，真工程根在上一层。
    """
    existing = _find_file_under(root, "OLED_CFont.h", 3)
    if existing:
        return existing
    for rel in COMMON_DIRS:
        d = os.path.join(root, rel)
        if os.path.isdir(d):
            return d
    return None if strong_only else root


def find_hw_dir(start):
    """定位该把字库放进哪个目录。

    **判据是工程文件（.uvprojx / .ewp / .cproject …），不是驱动文件名。**
    驱动叫 OLED.c、lcd.c、display.c 都不影响 —— 工程文件才是
    "这是一个工程"的硬定义，而驱动怎么命名纯属个人习惯。

    返回目录路径；找不到返回 None。
    """
    probe = os.path.abspath(start)
    for _ in range(6):
        roots = [r for r in _find_projects(probe)
                 if os.path.abspath(r) != os.path.abspath(HERE)]
        if roots:
            root = _pick_project(roots)
            # 先试工程文件那一层，不中再试上一层
            for cand in (root, os.path.dirname(root)):
                if not cand:
                    continue
                hit = _pick_font_dir(cand, strong_only=True)
                if hit:
                    return hit
            return root
        parent = os.path.dirname(probe)
        if parent == probe:
            break
        probe = parent
    return None


def main():
    argv = sys.argv[1:]
    size_opt = None
    hw_opt = None
    rest = []
    i = 0
    while i < len(argv):
        if argv[i] == "--size" and i + 1 < len(argv):
            size_opt = check_size(int(argv[i + 1]))
            i += 2
        elif argv[i] == "--dir" and i + 1 < len(argv):
            hw_opt = argv[i + 1]
            i += 2
        elif argv[i].startswith("-"):
            raise SystemExit("未知参数: %s" % argv[i])
        else:
            rest.append(argv[i])
            i += 1

    want = "".join(rest)
    if not want:
        raise SystemExit("用法: python add_chars.py 温度报警\n"
                         "      python add_chars.py --dir <工程>\\Hardware 温度报警")

    existing = open(SRC_H, encoding="utf-8").read() if os.path.exists(SRC_H) else ""
    size = size_opt or detect_size(existing)
    stride = 3 + bytes_per_glyph(size)
    rows = read_existing(SRC_H, stride)
    have = {ch for ch, _ in rows}
    print("现有字库 %d 字，按 %dx%d（每行 %d 字节）追加"
          % (len(rows), size, size, stride))

    font = load_font(size)
    added = []
    for ch in sorted(set(want), key=lambda c: c.encode("utf-8")):
        if ch in have:
            continue
        data = bits_to_bytes(glyph(ch, font, size), size)
        if len(data) != bytes_per_glyph(size):
            raise SystemExit("字符 %r 生成了 %d 字节" % (ch, len(data)))
        rows.append((ch, list(ch.encode("utf-8")) + data))
        added.append(ch)

    if not added:
        print("要加的字都已经在字库里了，无需改动")
    else:
        print("新增 %d 字: %s" % (len(added), "".join(added)))

    rows.sort(key=lambda r: r[0].encode("utf-8"))

    with open(SRC_H, "w", encoding="utf-8", newline="\r\n") as f:
        f.write(HEAD % {"size": size, "per": bytes_per_glyph(size), "pages": size // 8})
        f.write(render_header(rows, size))
        f.write("\n")
        # 之前漏了这行，生成的文件没有 #endif，include guard 形同虚设，
        # 两个 .c 同时包含会重复定义数组。和 GUI 版保持一致。
        f.write("#endif\n")

    total = len(rows)
    print("共 %d 字, %d 字节 (%d KB)"
          % (total, total * stride, (total * stride + 512) // 1024))

    hw_dir = hw_opt or find_hw_dir(HERE)
    if not hw_dir:
        print("没指定 --dir、附近也没找到 STM32 工程（按 .uvprojx/.ewp/.cproject 判断），"
              "只更新了 %s" % SRC_H)
        return
    if not os.path.isdir(hw_dir):
        print("目录不存在：%s，只更新了 %s" % (hw_dir, SRC_H))
        return

    dst = os.path.join(hw_dir, "OLED_CFont.h")
    if os.path.exists(dst):
        shutil.copyfile(dst, os.path.join(hw_dir, "OLED_CFont.old.h"))
        print("已备份旧文件 -> %s" % os.path.join(hw_dir, "OLED_CFont.old.h"))
    shutil.copyfile(SRC_H, dst)
    print("已安装 -> %s" % dst)


if __name__ == "__main__":
    main()
