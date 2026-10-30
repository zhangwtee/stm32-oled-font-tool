/* 中景园 OLED 驱动，这版驱动配的是 SH1106 的屏（132 列）
 * I2C 地址 0x78，初始化在 OLED_Init 里 */
#include "oled.h"

void OLED_SetCursor(uint8_t Y, uint8_t X)
{
	OLED_WR_Byte(0xB0 + Y, OLED_CMD);
	OLED_WR_Byte((((X + 2) & 0xF0) >> 4) | 0x10, OLED_CMD);
	OLED_WR_Byte(((X + 2) & 0x0F) | 0x00, OLED_CMD);
}
