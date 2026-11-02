# SPDX-License-Identifier: Apache-2.0
# -*- coding: utf-8 -*-
# Copyright (c) 2026 zhangwtee <https://github.com/zhangwtee>
# 本文件属于「OLED 点阵取模工具」。改动请保留以上两行并注明改了什么（Apache-2.0 §4b）。
"""给文件补 UTF-8 BOM（Windows PowerShell 5.1 需要）

用法: python tools\\add_bom.py <文件> [<文件>...]

为什么需要：PowerShell 5.1 读取没有 BOM 的 UTF-8 文件时，会按系统 ANSI 代码页
（中文系统是 GBK）解释；中文注释被解成乱码后会凑出非法语法，脚本连解析都过不去，
表现就是"窗口一闪而过"。
"""
import os
import sys

BOM = b"\xef\xbb\xbf"

for path in sys.argv[1:]:
    if not os.path.isfile(path):
        print("  跳过（不存在）: %s" % path)
        continue
    data = open(path, "rb").read()
    if data.startswith(BOM):
        print("  已有 BOM: %s" % path)
        continue
    open(path, "wb").write(BOM + data)
    print("  已补 BOM: %s  (%d -> %d 字节)" % (path, len(data), len(data) + 3))
