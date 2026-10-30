/* 陷阱：注释里写着"这里以前试过 0x02 起始列"和 "+2"，
 * 但真正的代码是标准 SSD1306 写法。剥注释之后应该判成 SSD1306。 */
#include "oled.h"

void OLED_SetCursor(uint8_t Y, uint8_t X)
{
	/* 曾经写成 OLED_WR_Byte(0x02, OLED_CMD); 和 (X + 2) 都不对 */
	OLED_WR_Byte(0xB0 + Y, OLED_CMD);
	OLED_WR_Byte(((X & 0xF0) >> 4) | 0x10, OLED_CMD);
	OLED_WR_Byte((X & 0x0F) | 0x00, OLED_CMD);
}
