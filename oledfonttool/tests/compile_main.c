/* 把导出的三份头文件真的编一遍。
 * 字符串对的、宏对的，都不代表能编译 —— 注释里嵌了块注释结束符、
 * 或者少个分号这种事，只有编译器和链接器说了算。
 */
#include <stdint.h>

/* 假驱动：只要这三个函数存在，三份头文件就该能跑 */
void OLED_WriteCommand(uint8_t cmd);
void OLED_WriteData(uint8_t dat);
void OLED_ShowChar(uint8_t line, uint8_t col, char ch);
void HAL_Delay(uint32_t ms);

#include "OLED_CFont.h"
#include "anim.h"
#include "OLED_SelfTest.h"

void OLED_WriteCommand(uint8_t cmd) { (void)cmd; }
void OLED_WriteData(uint8_t dat) { (void)dat; }
void OLED_ShowChar(uint8_t line, uint8_t col, char ch)
{
	(void)line; (void)col; (void)ch;
}
void HAL_Delay(uint32_t ms) { (void)ms; }

/*
 * 二分查表只有在「码表按 UTF-8 升序」时才成立。
 * 顺序一旦坏了，表现是"某些字突然显示不出来"，编译和链接都不会报错，
 * 所以这里把库里的字一个个查一遍，再确认库外的字查不到。
 * 返回 0 = 全对；1..n = 第 n 个字没查到；90 = 库外的字被误命中。
 */
static int check_find(void)
{
	static const char *chars[] = { "温", "度", "正", "常" };
	int i;

	for (i = 0; i < 4; i++)
	{
		if (OLED_CFont_Find(chars[i]) == 0)
		{
			return i + 1;
		}
	}
	if (OLED_CFont_Find("镕") != 0)     /* 字库里没有这个字 */
	{
		return 90;
	}
	return 0;
}

int main(void)
{
	int rc;

	rc = check_find();
	if (rc != 0)
	{
		return rc;
	}

	OLED_CFont_ShowString(1, 1, "温度正常");
	Anim_ShowFrame(0);
	Anim_ShowFrame(200);            /* 越界不能写，也不能崩 */
	OLED_SelfTest_Show();
	return 0;
}
