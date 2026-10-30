/******************************************************************************
 * OLED 驱动例程（SSD1306 / SH1106 通用）
 * 128x64 点阵，I2C 接口
 *
 * 说明：本文件按 SSD1306 的 128 列寻址方式编写。
 *       SH1106 是 132 列，用同一份代码会整体偏移，需要另外改。
 ******************************************************************************/

#include "oled.h"

/*
 * 设置光标位置
 * SSD1306 的页地址命令是 0xB0 | page
 */
void OLED_SetPos(uint8_t x, uint8_t y)
{
    OLED_WR_CMD(0xb0 + y);
    OLED_WR_CMD(((x & 0xf0) >> 4) | 0x10);
    OLED_WR_CMD(x & 0x0f);
}

void OLED_Clear(void)
{
    uint8_t i, n;
    for (i = 0; i < 8; i++)
    {
        OLED_SetPos(0, i);
        for (n = 0; n < 128; n++)
        {
            OLED_WR_DATA(0x00);
        }
    }
}
