# SPDX-License-Identifier: Apache-2.0
# -*- coding: utf-8 -*-
# Copyright (c) 2026 zhangwtee <https://github.com/zhangwtee>
# 本文件属于「OLED 点阵取模工具」。改动请保留以上两行并注明改了什么（Apache-2.0 §4b）。
"""生成 OLED_CFont.h —— 汉字字模表（字号由 gen_oled_hz.SIZE 决定，默认 16x16）

改字方法：把 CHARS 改成你要显示的文字，重新运行本脚本。
输出到本脚本所在目录（oledfonttool\\src\\OLED_CFont.h），
再双击同目录的 install-cfont.cmd 搬进工程的 Hardware 目录即可。
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_oled_hz import (glyph, bits_to_bytes, load_font, show_bits,   # noqa: E402
                         render_header, bytes_per_glyph, SIZE)

# 想显示的汉字，重复的会自动去掉。ASCII 字符不用写进来（走原有字库）。
CHARS = (
    "嵌入式开发网中文测试"      # 取模软件 123.C 里的参考文字
    "你好世界"
    "温度湿度光照电量电压电流功率时间年月日时分秒"
    "开关机状态正常错误报警设置返回上下左右确认取消"
    "欢迎使用系统初始化完成运行中请稍候"
)

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "OLED_CFont.h")

HEAD = '''#ifndef __OLED_CFONT_H
#define __OLED_CFONT_H

/*==============================================================================
 *  汉字字模库  %(size)dx%(size)d 点阵，每个汉字 %(per)d 字节
 *
 *  本文件由 tools/build_cfont.py 自动生成，不要手工编辑。
 *  要增删汉字：改脚本里的 CHARS，重新运行脚本即可。
 *
 *  取模格式（与 OLED.c 的 OLED_ShowChinese 对应）：
 *      阴码 / 列行式 / 逆向(低位在前) / C51格式
 *      一个汉字 = %(pages)d 页 × %(size)d 字节（共 %(per)d 字节）
 *
 *  每行的排布：前 3 字节是该字的 UTF-8 编码，后面 %(per)d 字节是点阵。
 *  这里全部写成十六进制字节，汉字只放在注释里——因为把字符串字面量
 *  （"\\xNN\\xNN\\xNN"）和字节混在同一个初始化列表里，从第二行起编译器会
 *  把字符串当成指针值，报 initializer element is not computable at load time。
 *
 *  注意：本文件必须是 UTF-8 编码保存（Keil 里 Edit -> Configuration ->
 *        Encoding 选 UTF-8），否则注释会乱码（不影响编译，但不好读）。
 *============================================================================*/

'''

TAIL = '''
#endif
'''


def main():
    chars = sorted(set(CHARS), key=lambda c: c.encode("utf-8"))
    font = load_font(SIZE)
    per = bytes_per_glyph(SIZE)

    rows = []
    for ch in chars:
        bits = glyph(ch, font, SIZE)
        data = bits_to_bytes(bits, SIZE)
        if len(data) != per:
            raise SystemExit("字符 %r 生成了 %d 字节, 应为 %d" % (ch, len(data), per))
        show_bits(bits, "=== %s ===" % ch)
        rows.append((ch, list(ch.encode("utf-8")) + data))

    path = os.path.abspath(OUT)
    with open(path, "w", encoding="utf-8", newline="\r\n") as f:
        f.write(HEAD % {"size": SIZE, "per": per, "pages": SIZE // 8})
        f.write(render_header(rows, SIZE))
        f.write("\n")
        f.write(TAIL.lstrip("\n"))

    stride = 3 + per
    print("\n写出 %s" % path)
    print("汉字 %d 个, 每行 %d 字节, 共 %d 字节 (%d KB)"
          % (len(rows), stride, len(rows) * stride, (len(rows) * stride + 512) // 1024))
    print("接下来: 双击 install-cfont.cmd 搬进工程, 然后在 Keil 里 Rebuild")


if __name__ == "__main__":
    main()
