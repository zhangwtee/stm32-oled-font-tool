#ifndef __OLED_CFONT_H
#define __OLED_CFONT_H

/*==============================================================================
 *  汉字字模库  16x16 点阵，每个汉字 32 字节
 *
 *  取模格式（与 OLED.c 的 OLED_ShowChinese 对应）：
 *      阴码 / 列行式 / 逆向(低位在前) / C51格式
 *      一个汉字 = 2 页 x 16 字节 = 32 字节（每页对应 8 行像素）
 *
 *  每行的排布：前 3 字节是该字的 UTF-8 编码，后面 32 字节是点阵，
 *  所以字库里一行是 35 字节。
 *
 *  这里全部写成十六进制字节，汉字只放在注释里——因为把字符串字面量
 *  ("\xNN\xNN\xNN") 和字节混在同一个初始化列表里，从第二行起编译器会
 *  把字符串当成指针值，报 initializer element is not computable at load time。
 *
 *  注意：本文件必须是 UTF-8 编码保存（Keil 里 Edit -> Configuration ->
 *        Encoding 选 UTF-8）。
 *
 *  下面几个宏给驱动用，别把数字写死——换字号时驱动不用改：
 *      OLED_CFONT_W / H       点阵宽高
 *      OLED_CFONT_BYTES       一个汉字占多少字节
 *      OLED_CFONT_STRIDE      字库里一行多少字节（3 + 点阵）
 *============================================================================*/

#define OLED_CFONT_W       16
#define OLED_CFONT_H       16
#define OLED_CFONT_BYTES   32
#define OLED_CFONT_STRIDE  35

/* 汉字码表：每行 35 字节 = UTF-8 三字节编码 + 16x16 点阵(32 字节) */
const uint8_t OLED_CF16x16[][35] =
{
	/* 常 */
	0xE5,0xB8,0xB8,0x00,0x00,0xFC,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0xFC,0x00,0x00,0x00,0x00,0x3F,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x3F,0x00,0x00,
	/* 度 */
	0xE5,0xBA,0xA6,0x00,0x00,0xFC,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0xFC,0x00,0x00,0x00,0x00,0x3F,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x3F,0x00,0x00,
	/* 正 */
	0xE6,0xAD,0xA3,0x00,0x00,0xFC,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0xFC,0x00,0x00,0x00,0x00,0x3F,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x3F,0x00,0x00,
	/* 温 */
	0xE6,0xB8,0xA9,0x00,0x00,0xFC,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0xFC,0x00,0x00,0x00,0x00,0x3F,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x3F,0x00,0x00,
};

/* 汉字个数 */
#define OLED_CFONT_TABLE   OLED_CF16x16
#define OLED_CFONT_NUM     (sizeof(OLED_CFONT_TABLE) / sizeof(OLED_CFONT_TABLE[0]))
#define OLED_CF16x16_NUM   OLED_CFONT_NUM

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
 *  有些工程把这两个函数只写在 .c 里、没在 .h 声明，在自己文件里调用会报
 *  未声明。把这两行补进对应的 .h 即可：
 *      void OLED_WriteCommand(uint8_t cmd);
 *      void OLED_WriteData(uint8_t dat);
 *
 *  CF_SHOW_ASCII 用来显示英文字符。没有自己的字符显示函数就改成
 *      #define CF_SHOW_ASCII(line, col, ch)  ((void)0)
 *  这样英文字符被跳过，汉字位置仍然连续。
 * ========================================================================== */
#define CF_WRITE_CMD(c)                 OLED_WriteCommand(c)
#define CF_WRITE_DATA(d)                OLED_WriteData(d)
#define CF_SHOW_ASCII(line, col, ch)    OLED_ShowChar((line), (col), (ch))

/* 屏是 SH1106（显存 132 列，可见区从第 2 列开始）时是 2，SSD1306 是 0。
   这个值是导出时按驱动源码自动判断后填进来的，一般不用改；
   万一判断错了（右边框缺两列、整幅左移 2 像素），把它改成 2 或 0 即可。 */
#define CF_COL_OFFSET                   2

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
   utf8 指向该字 3 字节 UTF-8 编码的第一个字节。 */
static CF_UNUSED const uint8_t *OLED_CFont_Find(const char *utf8)
{
    uint16_t i;

    for (i = 0; i < (uint16_t)OLED_CFONT_NUM; i++)
    {
        if (OLED_CFONT_TABLE[i][0] == (uint8_t)utf8[0] &&
            OLED_CFONT_TABLE[i][1] == (uint8_t)utf8[1] &&
            OLED_CFONT_TABLE[i][2] == (uint8_t)utf8[2])
        {
            return &OLED_CFONT_TABLE[i][3];
        }
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

#endif
