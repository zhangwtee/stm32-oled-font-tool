// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 zhangwtee <https://github.com/zhangwtee>
// 本文件属于「OLED 点阵取模工具」。改动请保留以上两行并注明改了什么（Apache-2.0 §4b）。
/* 有重复帧的动画头文件：编一遍，跑一遍，数清楚到底刷了多少数据。
 *
 * 去重最容易出的错是"省了 Flash 但帧序乱了"：
 * 画面每帧看着都对，连着放才发现顺序跳。所以这里不只数写入量，
 * 还要把 Anim_Map 的内容逐项对一遍。
 *
 * 单独一个文件是因为这里用的是 fixture 里那份**改动前**的头文件，
 * include guard 还是老的 `__ANIM_FRAMES_H`，和 fixture/anim.h 同门，
 * 同一个翻译单元里不能同时包含两份。
 * （新导出的头文件按素材名带前缀，guard 是 `__LOGO_FRAMES_H` 这种，
 *  两份可以放一起 —— 见 compile_two_assets.c。）
 */
#include <stdint.h>

void OLED_WriteCommand(uint8_t cmd);
void OLED_WriteData(uint8_t dat);
void HAL_Delay(uint32_t ms);

#include "anim_dedup.h"

static uint32_t g_cmds = 0;
static uint32_t g_datas = 0;

void OLED_WriteCommand(uint8_t cmd) { (void)cmd; ++g_cmds; }
void OLED_WriteData(uint8_t dat) { (void)dat; ++g_datas; }
void HAL_Delay(uint32_t ms) { (void)ms; }

int main(void)
{
	/* 4 个逻辑帧，每帧 8 页 × (3 条命令 + 128 字节) */
	Anim_ShowFrame(0);
	Anim_ShowFrame(1);
	Anim_ShowFrame(2);
	Anim_ShowFrame(3);
	Anim_ShowFrame(4);                  /* 越界：不许写，也不许崩 */

	const unsigned n  = (unsigned)ANIM_COUNT;
	const unsigned dn = (unsigned)ANIM_DATA_COUNT;

	if (n != 4u)
	{
		return 1;
	}
	if (dn != 2u)
	{
		return 2;
	}
	if (g_datas != 4u * 1024u)          /* 逻辑帧数 × 每帧字节，一帧都不能少 */
	{
		return 3 + (int)(g_datas & 0x7F);
	}
	if (g_cmds != 4u * 24u)             /* 8 页 × 3 条定位命令 */
	{
		return 4 + (int)(g_cmds & 0x7F);
	}

	/* 帧 0/2/3 是同一份数据，帧 1 是另一份 */
	if (Anim_Map[0] != 0 || Anim_Map[1] != 1 || Anim_Map[2] != 0 || Anim_Map[3] != 0)
	{
		return 5;
	}

	/* 两帧数据不能碰巧相等，否则上面那条测试就没意义了 */
	if (Anim_Frames[0][0] == Anim_Frames[1][0])
	{
		return 6;
	}
	return 0;
}
