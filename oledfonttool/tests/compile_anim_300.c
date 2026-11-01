// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 zhangwtee <https://github.com/zhangwtee>
// 本文件属于「OLED 点阵取模工具」。改动请保留以上两行并注明改了什么（Apache-2.0 §4b）。
/* 300 帧的动画头文件：编一遍、跑一遍。
 *
 * 这条用例盯的是「帧号位宽」。原来生成的 Anim_ShowFrame 参数是 uint8_t，
 * 映射表元素也是 uint8_t，而界面允许导出到 600 帧，于是：
 *   · Anim_ShowFrame(299) 进函数先被截成 43，第 256 帧以后永远看不到；
 *   · 头文件里那段示例 for (uint8_t i = 0; i < ANIM_COUNT; i++)，
 *     ANIM_COUNT=300 时 i 到 255 回绕成 0，永远退不出去（死循环）；
 *   · 去重映射表存的是数据帧号，去重后仍可能超过 255。
 *
 * 这里用 uint16_t 跑满 300 帧，并核对最后一帧的数据确实是第 299 帧 ——
 * 帧号被截断的话，读到的会是第 43 帧（首字节一样是 43，但次字节是 0）。
 */
#include <stdint.h>

void OLED_WriteCommand(uint8_t cmd);
void OLED_WriteData(uint8_t dat);
void HAL_Delay(uint32_t ms);

#include "anim_300.h"

static uint32_t g_cmds  = 0;
static uint32_t g_datas = 0;

void OLED_WriteCommand(uint8_t cmd) { (void)cmd; ++g_cmds; }
void OLED_WriteData(uint8_t dat)    { (void)dat; ++g_datas; }
void HAL_Delay(uint32_t ms)         { (void)ms; }

int main(void)
{
	uint16_t i;

	/* 先落成变量再比：直接写 if (ANIM_COUNT != 300) 在 /W4 下会报 C4127
	 * （条件表达式是常量），而这几个宏本来就是常量。 */
	const unsigned n = (unsigned)ANIM_COUNT;
	const unsigned w = (unsigned)ANIM_WIDTH;
	const unsigned h = (unsigned)ANIM_HEIGHT;

	if (n != 300u)
	{
		return 1;
	}
	if (w != 128u || h != 32u)
	{
		return 2;
	}

	for (i = 0; i < ANIM_COUNT; i++)
	{
		Anim_ShowFrame(i);
	}

	/* 128×32：4 页 × 128 字节 = 512 字节/帧；每页 3 条定位命令 */
	if (g_datas != 300u * 512u)
	{
		return 3;
	}
	if (g_cmds != 300u * 4u * 3u)
	{
		return 4;
	}

	/* 第 299 帧：首字节 299 & 0xFF = 43，次字节 299 >> 8 = 1 */
	if (Anim_Frames[ANIM_COUNT - 1][0] != 43u)
	{
		return 5;
	}
	if (Anim_Frames[ANIM_COUNT - 1][1] != 1u)
	{
		return 6;
	}

	/* 越界帧：不许写，也不许崩 */
	Anim_ShowFrame(300);
	if (g_datas != 300u * 512u)
	{
		return 7;
	}
	return 0;
}
