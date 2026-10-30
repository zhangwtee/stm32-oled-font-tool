/* 多份素材写进**一个** .h，但各存各的 —— 编译级验收。
 *
 * 这是「全部存进一个 .h…」那个按钮的成品：fixture/assets_pack.h 由
 * ImageGen::renderMultiHeader() 真实生成（不是手写的），里面有
 *   Logo  : 128x64，1 帧
 *   Blink : 128x32，3 帧，其中第 3 帧和第 1 帧一样 → 去重成 2 份数据
 *
 * 验的是三件事：
 *   1. 两份素材的宏、数组、显示函数互不干扰（尺寸和帧数都不一样）；
 *   2. 每份 ShowFrame 的写入量按**自己**的尺寸算，不是按别人的；
 *   3. 去重那份的映射表真的指回同一批数据，越界帧不写。
 *
 * 编译命令（Git Bash）：
 *   cl /nologo /W4 /utf-8 compile_multi_assets.c -Ifixture
 */
#include <stdint.h>
#include <stdio.h>

void OLED_WriteCommand(uint8_t cmd);
void OLED_WriteData(uint8_t dat);
void HAL_Delay(uint32_t ms);

#include "assets_pack.h"

/* 再包含一次：guard 生效，不该重复定义 */
#include "assets_pack.h"

#ifndef __ASSETS_H
#error "assets_pack.h 的 include guard 没生效"
#endif

static uint32_t g_cmds = 0;
static uint32_t g_datas = 0;

void OLED_WriteCommand(uint8_t cmd) { (void)cmd; ++g_cmds; }
void OLED_WriteData(uint8_t dat)    { (void)dat; ++g_datas; }
void HAL_Delay(uint32_t ms)         { (void)ms; }

/* 用变量接住常量表达式，免得 /W4 把比较报成"条件恒真" */
static uint32_t logo_w    = LOGO_WIDTH;
static uint32_t blink_w   = BLINK_WIDTH;
static uint32_t logo_h    = LOGO_HEIGHT;
static uint32_t blink_h   = BLINK_HEIGHT;
static uint32_t logo_cnt  = LOGO_COUNT;
static uint32_t blink_cnt = BLINK_COUNT;
static uint32_t blink_dat = BLINK_DATA_COUNT;
static size_t   logo_sz   = sizeof(Logo_Frames);
static size_t   blink_sz  = sizeof(Blink_Frames);
static size_t   logo_one = sizeof(Logo_Frames[0]);
static size_t   blink_one = sizeof(Blink_Frames[0]);

int main(void)
{
	/* 两份素材的尺寸各按各的来，没有被第一份覆盖 */
	if (logo_w != 128 || blink_w != 128)              return 1;
	if (logo_h != 64  || blink_h != 32)               return 2;
	if (logo_cnt != 1 || blink_cnt != 3)              return 3;
	if (blink_dat != 2)                               return 4;

	/* 一帧的字节数 = 宽 x (高/8)，不是同一份 */
	if (logo_one != 128u * 8u)                        return 5;
	if (blink_one != 128u * 4u)                       return 6;
	if (logo_sz != 1u * 1024u)                        return 7;
	if (blink_sz != 2u * 512u)                        return 8;

	/* 两份数据不是同一份 */
	if (Logo_Frames[0][0] == Blink_Frames[0][0])      return 9;

	/* 各刷各的：写入量按自己的页数算（每页 3 条命令 + W 个字节） */
	g_cmds = g_datas = 0;
	Logo_ShowFrame(0);
	if (g_datas != 1024u || g_cmds != 8u * 3u)        return 10;

	g_cmds = g_datas = 0;
	Blink_ShowFrame(0);
	if (g_datas != 512u  || g_cmds != 4u * 3u)        return 11;

	/* 越界帧不写 */
	g_cmds = g_datas = 0;
	Logo_ShowFrame(1);            /* LOGO_COUNT = 1，越界 */
	if (g_datas != 0 || g_cmds != 0)                  return 12;

	/* 去重：第 3 帧（下标 2）指向数据帧 0，画面和第 1 帧相同 */
	g_cmds = g_datas = 0;
	Blink_ShowFrame(2);
	if (g_datas != 512u || g_cmds != 4u * 3u)         return 13;
	if (Blink_Map[0] != 0 || Blink_Map[1] != 1 || Blink_Map[2] != 0) return 14;

	printf("multi assets in one header: ok — Logo 128x64 x%d, Blink 128x32 x%d (%d data)\n",
	       (int)logo_cnt, (int)blink_cnt, (int)blink_dat);
	return 0;
}
