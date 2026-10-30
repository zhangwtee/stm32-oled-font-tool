/* 正点原子风格：一个函数带 cmd 标志位，小写 0xb0，列地址没有偏移 */
#include "oled.h"

void OLED_SetCursor(uint8_t y, uint8_t x)
{
	OLED_WR_Byte(0xb0 + y, OLED_CMD);
	OLED_WR_Byte(((x & 0xf0) >> 4) | 0x10, OLED_CMD);
	OLED_WR_Byte((x & 0x0f) | 0x00, OLED_CMD);
}
