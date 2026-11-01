# SPDX-License-Identifier: Apache-2.0
# -*- coding: utf-8 -*-
# Copyright (c) 2026 zhangwtee <https://github.com/zhangwtee>
# 本文件属于「OLED 点阵取模工具」。改动请保留以上两行并注明改了什么（Apache-2.0 §4b）。
"""把取模软件导出的 32 个字节还原成点阵图，用来肉眼检查字形对不对

用法一：直接把软件里复制出来的字节粘在后面
    python dump_font.py 0x00,0x01,0x02, ... (共 32 个)

用法二：把软件里复制的内容存成 txt，再指定文件
    python dump_font.py 字模.txt

判定标准：正常汉字的笔画是连着的；如果看到一堆孤立散点，
说明"取模方式"选错了（应该是列行式）。
"""
import re
import sys

SIZE = 16


def parse(text):
    return [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", text)]


def from_col(data):
    """列行式 + 逆向(低位在前)：OLED 驱动认的格式"""
    bits = [[0] * SIZE for _ in range(SIZE)]
    for n, b in enumerate(data):
        half, x = n // 16, n % 16
        for i in range(8):
            if b & (1 << i):
                bits[half * 8 + i][x] = 1
    return bits


def from_row(data):
    """逐行式 + 逆向(低位在前)：123.C 那种"""
    bits = [[0] * SIZE for _ in range(SIZE)]
    for n, b in enumerate(data):
        y, x0 = n // 2, (n % 2) * 8
        for i in range(8):
            if b & (1 << i):
                bits[y][x0 + i] = 1
    return bits


def lone_points(bits):
    lone = 0
    for y in range(SIZE):
        for x in range(SIZE):
            if not bits[y][x]:
                continue
            nb = sum(1 for dy in (-1, 0, 1) for dx in (-1, 0, 1)
                     if (dy or dx) and 0 <= y + dy < SIZE and 0 <= x + dx < SIZE
                     and bits[y + dy][x + dx])
            if nb == 0:
                lone += 1
    return lone


def show(bits, title):
    print(title)
    for row in bits:
        print("  " + "".join("#" if v else "." for v in row))
    print()


def main():
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    if len(args) == 1 and not args[0].startswith("0x"):
        with open(args[0], "r", encoding="utf-8", errors="ignore") as f:
            text = f.read()
    else:
        text = " ".join(args)

    data = parse(text)
    if len(data) % 32 != 0:
        print("!! 共 %d 字节, 不是 32 的整数倍, 数据可能残缺" % len(data))
    for k in range(0, len(data) - 31, 32):
        chunk = data[k:k + 32]
        show(from_col(chunk), "=== 第 %d 个字 按【列行式】解释 (孤立点 %d) ==="
             % (k // 32 + 1, lone_points(from_col(chunk))))
        show(from_row(chunk), "=== 第 %d 个字 按【逐行式】解释 (孤立点 %d) ==="
             % (k // 32 + 1, lone_points(from_row(chunk))))


if __name__ == "__main__":
    main()
