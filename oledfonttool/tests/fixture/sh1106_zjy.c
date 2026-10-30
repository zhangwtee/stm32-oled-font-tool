/* 中景园 SH1106 写法：设列地址时 X 要多加 2（显存 132 列，可见区从第 2 列开始） */
#include "oled.h"

void OLED_SetCursor(uint8_t Y, uint8_t X)
{
	OLED_WR_Byte(0xB0 + Y, OLED_CMD);
	OLED_WR_Byte((((X + 2) & 0xF0) >> 4) | 0x10, OLED_CMD);
	OLED_WR_Byte(((X + 2) & 0x0F) | 0x00, OLED_CMD);
}
