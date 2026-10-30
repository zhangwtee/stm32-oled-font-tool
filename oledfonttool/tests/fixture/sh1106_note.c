/* 屏：SH1106 0.96 寸 132x64，I2C 接口
 * 驱动里没有把 +2 写进代码，只在注释里提了一句。 */
#include "oled.h"

void OLED_SetCursor(uint8_t Y, uint8_t X)
{
	OLED_WR_Byte(0xB0 + Y, OLED_CMD);
	OLED_WR_Byte(((X & 0xF0) >> 4) | 0x10, OLED_CMD);
	OLED_WR_Byte((X & 0x0F) | 0x00, OLED_CMD);
}
