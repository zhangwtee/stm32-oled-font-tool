/* 中景园新版 SSD1306 写法：列地址直接用 X，没有偏移 */
#include "oled.h"

void OLED_SetCursor(uint8_t Y, uint8_t X)
{
	OLED_WR_Byte(0xB0 + Y, OLED_CMD);
	OLED_WR_Byte(((X & 0xF0) >> 4) | 0x10, OLED_CMD);
	OLED_WR_Byte((X & 0x0F) | 0x00, OLED_CMD);
}
