# SPDX-License-Identifier: Apache-2.0
# -*- coding: utf-8 -*-
# Copyright (c) 2026 zhangwtee <https://github.com/zhangwtee>
# 本文件属于「OLED 点阵取模工具」。改动请保留以上两行并注明改了什么（Apache-2.0 §4b）。
"""
生成 OLED 汉字字模（SSD1306 / 江科大驱动 列行式 逆向 阴码 格式）

用法:
    python gen_oled_hz.py 你好世界 温度 湿度
    python gen_oled_hz.py --size 24 你好世界          # 24x24 大字
    python gen_oled_hz.py --dump 0x00,0x40,0xFE,...

输出:
    1. 完整的 C 头文件内容（打印到屏幕，符合 __OLED_CFONT_H 格式）
    2. 逐字符点阵，肉眼核对字形

字号必须是 8 的倍数：编码按"页"走，一页 8 行。
    16x16 -> 每字 32 字节，24x24 -> 72 字节，32x32 -> 128 字节。

为什么用脚本而不是手抄:
    手抄字模容易出现字节漏抄、错位，脚本生成可以保证
    "一个汉字 == 每页 N 字节 x N/8 页"。
"""
import sys

from PIL import Image, ImageDraw, ImageFont

FONT_CANDIDATES = [
    r"C:\Windows\Fonts\simsun.ttc",
    r"C:\Windows\Fonts\simhei.ttf",
    r"C:\Windows\Fonts\msyh.ttc",
]

SIZE = 16          # 默认点阵边长，必须是 8 的倍数
THRESHOLD = 110    # 灰度阈值，越小字越粗


def check_size(size):
    if size < 8 or size > 64 or size % 8:
        raise SystemExit("字号必须是 8 的倍数、且在 8~64 之间，现在是 %d" % size)
    return size


def bytes_per_glyph(size=SIZE):
    return (size // 8) * size


def load_font(size=None):
    size = check_size(SIZE if size is None else size)
    for path in FONT_CANDIDATES:
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            continue
    raise SystemExit("找不到中文字体，请检查 FONT_CANDIDATES 里的路径")


def glyph(ch, font, size=None):
    """
    把一个汉字渲染成 N×N 的 0/1 点阵（1 = 点亮）

    字体按 N 像素加载，文字直接画在 N×N 画布上（和取模软件"宋体 N"一致），
    再按实际墨迹把字形在格子里居中，避免整体偏左或贴边。
    """
    size = check_size(SIZE if size is None else size)
    canvas = Image.new("L", (size, size), 0)
    ImageDraw.Draw(canvas).text((0, 0), ch, fill=255, font=font)

    box = canvas.getbbox()
    if box is None:
        raise SystemExit("字符 %r 渲染为空，可能不是汉字或字体不支持" % ch)

    dx = ((size - (box[2] - box[0])) // 2) - box[0]
    dy = ((size - (box[3] - box[1])) // 2) - box[1]
    if dx or dy:
        shifted = Image.new("L", (size, size), 0)
        shifted.paste(canvas, (dx, dy))
        canvas = shifted

    px = canvas.load()
    return [[1 if px[x, y] >= THRESHOLD else 0 for x in range(size)]
            for y in range(size)]


def bits_to_bytes(bits, size=None):
    """
    列行式 + 逆向(低位在前):
    每次取一列 8 行，低位在上；按页从上往下排。
    一页 N 字节，共 N/8 页。
    """
    size = check_size(SIZE if size is None else size)
    out = []
    for page in range(size // 8):
        y0 = page * 8
        for x in range(size):
            byte = 0
            for i in range(8):
                if bits[y0 + i][x]:
                    byte |= (1 << i)          # 低位在前 => 上面的点进低位
            out.append(byte)
    return out


def bytes_to_bits(data, size=None):
    """上面的逆运算：把字节还原成点阵，用来逐个核对"""
    size = check_size(SIZE if size is None else size)
    bits = [[0] * size for _ in range(size)]
    for page in range(size // 8):
        y0 = page * 8
        for x in range(size):
            byte = data[page * size + x]
            for i in range(8):
                if byte & (1 << i):
                    bits[y0 + i][x] = 1
    return bits


def show_bits(bits, title=""):
    """用 ASCII 把点阵打印出来，肉眼核对字形"""
    if title:
        print(title)
    for row in bits:
        print("  " + "".join("#" if v else "." for v in row))


def table_name(size):
    return "OLED_CF%dx%d" % (size, size)


# 自带的显示函数。
# 这段文本和 C++ 版 fontgen.cpp 里 generateHeader() 的原始字符串必须逐字符一致，
# 所以里面不插任何尺寸数字——全从上面的宏来。改一边就要同步另一边。
SHOW_FUNC_TAIL = r"""
/* ============================================================================
 *  适配区：只改这里，下面的函数不用动
 *
 *  这里不假设你工程里已经写好 OLED_ShowChinese —— 很多工程没有，或者参数
 *  顺序、页数算法各不相同。本文件只跟驱动交换两个最底层的动作：
 *  写一条命令、写一个字节。这两个函数初始化时必然要用到，不可能不存在。
 *
 *  (A) 命令和数据分成两个函数（中景园新版、多数 HAL 例程）
 *      #define CF_WRITE_CMD(c)   OLED_WriteCommand(c)
 *      #define CF_WRITE_DATA(d)  OLED_WriteData(d)
 *
 *  (B) 一个函数加 cmd 标志位（正点原子、中景园旧版）
 *      #define CF_WRITE_CMD(c)   OLED_WR_Byte((c), 1)
 *      #define CF_WRITE_DATA(d)  OLED_WR_Byte((d), 0)
 *
 *  有些工程把这两个函数只写在 .c 里、没在 .h 声明，调用时会报
 *  #223-D: function declared implicitly。本文件在下面替它们补了声明，
 *  一般不用管；真碰上类型对不上，把 CF_DRIVER_DECL 改成 0 就行。
 *
 *  CF_SHOW_ASCII 用来显示英文字符。没有自己的字符显示函数就改成
 *      #define CF_SHOW_ASCII(line, col, ch)  ((void)0)
 *  这样英文字符被跳过，汉字位置仍然连续。
 * ========================================================================== */
#define CF_WRITE_CMD(c)                 OLED_WriteCommand(c)
#define CF_WRITE_DATA(d)                OLED_WriteData(d)
#define CF_SHOW_ASCII(line, col, ch)    OLED_ShowChar((line), (col), (ch))

/* 驱动函数的原型声明。
 *
 * 不少例程把 OLED_WriteCommand() / OLED_WriteData() 只写在驱动 .c 里、没在 .h 里导出，
 * 上面那些宏展开后编译器没见过这些名字，就报
 *     warning: #223-D: function "..." declared implicitly
 * 这里替你补上，不用去改驱动头文件。
 *
 * 下面三种情况把它关掉（改成 0）：
 *   1) 驱动里这些函数的参数类型不是 uint8_t —— 声明和定义对不上会报 error
 *   2) 它们是 static 函数，或者是宏
 *   3) 你就想自己维护这份声明
 */
#define CF_DRIVER_DECL                  1

#if CF_DRIVER_DECL
void OLED_WriteCommand(uint8_t);
void OLED_WriteData(uint8_t);
#endif

/* 屏是 SH1106（显存 132 列，可见区从第 2 列开始）时改成 2，SSD1306 保持 0 */
#define CF_COL_OFFSET                   0

/* 屏幕像素宽度，只用来判断一行写满后不再往后写 */
#define CF_SCREEN_W                     128

/* 屏幕像素高度，只用来判断行号是否超出屏幕 */
#define CF_SCREEN_H                     64

/* 一个汉字占几页 */
#define OLED_CFONT_PAGES                (OLED_CFONT_H / 8)

#if defined(__GNUC__) || defined(__CC_ARM)
#define CF_UNUSED  __attribute__((unused))
#else
#define CF_UNUSED
#endif

/* 定位到某一页、某一像素列。自己发命令，不调用驱动里的设光标函数，
   免得碰上各例程参数顺序不一致（有的页在前，有的列在前） */
#define CF_SET_POS(page, col)                                          \
    do {                                                               \
        CF_WRITE_CMD((uint8_t)(0xB0 | ((page) & 0x07)));                \
        CF_WRITE_CMD((uint8_t)(0x10 | (((col) >> 4) & 0x0F)));          \
        CF_WRITE_CMD((uint8_t)(0x00 | ((col) & 0x0F)));                 \
    } while (0)

/* 在码表里找这个字，返回点阵起始地址；找不到返回 0。
   utf8 指向该字 3 字节 UTF-8 编码的第一个字节。

   码表在生成时就按 UTF-8 编码升序排好了，所以这里用二分查。
   线性扫描在 70 个字时无所谓，但几百个字时刷一屏 4 行 32 个字就是上万次比较，
   帧率会肉眼可见地掉；二分 500 个字也只要 9 次比较。

   不用先转成 Unicode 码点：UTF-8 的字节序和码点序完全一致，
   逐字节比大小就等于比码点。 */
static CF_UNUSED const uint8_t *OLED_CFont_Find(const char *utf8)
{
    uint16_t lo = 0;
    uint16_t hi = (uint16_t)OLED_CFONT_NUM;
    const uint8_t b0 = (uint8_t)utf8[0];
    const uint8_t b1 = (uint8_t)utf8[1];
    const uint8_t b2 = (uint8_t)utf8[2];

    while (lo < hi)
    {
        const uint16_t mid = (uint16_t)(lo + (hi - lo) / 2);
        const uint8_t *row = OLED_CFONT_TABLE[mid];

        if (row[0] < b0)        { lo = (uint16_t)(mid + 1); }
        else if (row[0] > b0)   { hi = mid; }
        else if (row[1] < b1)   { lo = (uint16_t)(mid + 1); }
        else if (row[1] > b1)   { hi = mid; }
        else if (row[2] < b2)   { lo = (uint16_t)(mid + 1); }
        else if (row[2] > b2)   { hi = mid; }
        else                    { return &OLED_CFONT_TABLE[mid][3]; }
    }
    return 0;
}

/* 把一个字的点阵写到屏上。
   line   : 第几行文字（从 1 开始），一行占 OLED_CFONT_PAGES 页
   xPixel : 起始像素列（0 起），不是"第几个字" */
static CF_UNUSED void OLED_CFont_ShowGlyph(uint8_t line, uint16_t xPixel,
                                           const uint8_t *glyph)
{
    uint8_t  p;
    uint8_t  x;
    uint16_t base;

    if (glyph == 0 || (uint16_t)(xPixel + OLED_CFONT_W) > CF_SCREEN_W)
    {
        return;
    }

    base = (uint16_t)((line - 1) * OLED_CFONT_PAGES);
    if ((uint16_t)(base + OLED_CFONT_PAGES) > (uint16_t)(CF_SCREEN_H / 8))
    {
        return;                 /* 整行放不下了，不写。否则页地址会绕回第 0 页 */
    }

    for (p = 0; p < OLED_CFONT_PAGES; p++)
    {
        CF_SET_POS(base + p, (uint16_t)(xPixel + CF_COL_OFFSET));
        for (x = 0; x < OLED_CFONT_W; x++)
        {
            CF_WRITE_DATA(glyph[(uint16_t)p * OLED_CFONT_W + x]);
        }
    }
}

/* 显示一串 UTF-8 编码的文字（汉字 + 英文 + 数字混排）。
   line : 第几行文字（从 1 开始）
   col  : 从第几列开始，单位是 8 像素，和 OLED_ShowString 的 Column 一致。
          所以 16x16 的汉字占 2 列，24x24 占 3 列，32x32 占 4 列。

   注意：英文字符委托给 CF_SHOW_ASCII，它按 8x16 字符计算行位置。
   字号不是 16 时，英文和汉字的行高不一致，混排会错开，这种情况自己调 line。 */
static CF_UNUSED void OLED_CFont_ShowString(uint8_t line, uint8_t col,
                                            const char *utf8)
{
    const uint8_t *g;
    uint16_t xPixel;
    uint8_t  c;

    if (utf8 == 0)
    {
        return;
    }
    if (col < 1)
    {
        col = 1;
    }
    xPixel = (uint16_t)((col - 1) * 8);

    while (*utf8 != '\0')
    {
        c = (uint8_t)(*utf8);

        if (c < 0x80)                       /* ASCII */
        {
            if ((uint16_t)(xPixel + 8) <= CF_SCREEN_W)
            {
                CF_SHOW_ASCII(line, (uint8_t)(xPixel / 8 + 1), (char)c);
            }
            xPixel = (uint16_t)(xPixel + 8);
            utf8++;
        }
        else if ((c & 0xF0) == 0xE0)        /* 三字节：本字库收录的汉字 */
        {
            g = OLED_CFont_Find(utf8);
            if (g != 0)
            {
                OLED_CFont_ShowGlyph(line, xPixel, g);
            }
            xPixel = (uint16_t)(xPixel + OLED_CFONT_W);
            utf8 += 3;
        }
        else if ((c & 0xF8) == 0xF0)        /* 四字节：扩展区汉字，本库不收 */
        {
            xPixel = (uint16_t)(xPixel + OLED_CFONT_W * 2);
            utf8 += 4;
        }
        else if ((c & 0xE0) == 0xC0)        /* 二字节：拉丁扩展等，本库不收 */
        {
            xPixel = (uint16_t)(xPixel + 8);
            utf8 += 2;
        }
        else                                /* 非法字节，跳过一个继续 */
        {
            utf8++;
        }
    }
}
"""


def render_header(rows, size=None):
    """
    生成 C 数组文本。rows = [(汉字, [3 + 点阵 个字节]), ...]

    格式说明（很重要，别改成 {"\\xNN",0x..} 那种写法）：
        字符串字面量是"字符数组"类型，和字节元素混在同一个初始化列表里时，
        编译器会把后续行的字符串当成指针值，报：
            error: initializer element is not computable at load time
        所以这里把整行都写成十六进制字节，汉字只放在注释里。
    """
    size = check_size(SIZE if size is None else size)
    per = bytes_per_glyph(size)
    stride = 3 + per
    name = table_name(size)

    lines = [
        "#define OLED_CFONT_W       %d" % size,
        "#define OLED_CFONT_H       %d" % size,
        "#define OLED_CFONT_BYTES   %d" % per,
        "#define OLED_CFONT_STRIDE  %d" % stride,
        "",
        "/* 汉字码表：每行 %d 字节 = UTF-8 三字节编码 + %dx%d 点阵(%d 字节) */"
        % (stride, size, size, per),
        "const uint8_t %s[][%d] =" % (name, stride),
        "{",
    ]
    for ch, vals in rows:
        assert len(vals) == stride, "%s 应该是 %d 字节，实际 %d" % (ch, stride, len(vals))
        lines.append("\t/* %s */" % ch)
        lines.append("\t%s" % (",".join("0x%02X" % b for b in vals) + ","))
    lines.append("};")
    lines.append("")
    lines.append("/* 汉字个数 */")
    lines.append("#define OLED_CFONT_TABLE   %s" % name)
    lines.append("#define OLED_CFONT_NUM     (sizeof(OLED_CFONT_TABLE) / sizeof(OLED_CFONT_TABLE[0]))")
    if size == 16:
        # 16x16 时代驱动里写的是 OLED_CF16x16_NUM，留着这行老工程照样能编译
        lines.append("#define OLED_CF16x16_NUM   OLED_CFONT_NUM")
    lines.append(SHOW_FUNC_TAIL)
    return "\n".join(lines)


def dump_hex(text, size=None):
    """把十六进制字节串还原成点阵并打印，用于核对取模软件的输出"""
    size = check_size(SIZE if size is None else size)
    data = [int(v, 16) for v in text.replace("0x", " ").replace(",", " ").split()]
    want = bytes_per_glyph(size)
    if len(data) != want:
        raise SystemExit("一个汉字必须是 %d 字节, 现在是 %d 字节" % (want, len(data)))
    show_bits(bytes_to_bits(data, size))


def _parse_argv(argv):
    """取出 --size N，剩下的当正文"""
    size = SIZE
    rest = []
    i = 0
    while i < len(argv):
        if argv[i] == "--size" and i + 1 < len(argv):
            size = check_size(int(argv[i + 1]))
            i += 2
        elif argv[i].startswith("--size="):
            size = check_size(int(argv[i].split("=", 1)[1]))
            i += 1
        else:
            rest.append(argv[i])
            i += 1
    return size, rest


def main():
    argv = sys.argv[1:]

    if argv and argv[0] == "--dump":
        size, _ = _parse_argv(argv[1:])
        dump_hex(" ".join(a for a in argv[1:] if not a.startswith("--")), size)
        return

    size, argv = _parse_argv(argv)
    argv = [a for a in argv if not a.startswith("-")]
    text = "".join(argv)
    if not text:
        raise SystemExit("用法: python gen_oled_hz.py [--size 16|24|32] 你好世界")

    font = load_font(size)
    chars = sorted(set(text), key=lambda c: c.encode("utf-8"))   # 按 UTF-8 排序
    rows = []
    for ch in chars:
        bits = glyph(ch, font, size)
        data = bits_to_bytes(bits, size)
        if len(data) != bytes_per_glyph(size):
            raise SystemExit("字符 %r 生成了 %d 字节, 应为 %d"
                             % (ch, len(data), bytes_per_glyph(size)))
        show_bits(bits, "=== %s ===" % ch)
        rows.append((ch, list(ch.encode("utf-8")) + data))

    print()
    print(render_header(rows, size))
    print()
    print("/* 共 %d 字, 每字 = 3 字节编码 + %d 字节点阵 */"
          % (len(rows), bytes_per_glyph(size)))


if __name__ == "__main__":
    main()
