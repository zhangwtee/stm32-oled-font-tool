// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 zhangwtee <https://github.com/zhangwtee>
// 本文件属于「OLED 点阵取模工具」。改动请保留以上两行并注明改了什么（Apache-2.0 §4b）。
/* 128×32 版本的自检帧单独编一遍、跑一遍。
 *
 * 为什么要单独一个文件：两份自检帧头文件的 include guard 同名
 * （都叫 __OLED_SELFTEST_H），同一个翻译单元里包含两次，第二次会被整段跳过，
 * 那样就测不到"32 行版本"了。所以分成两个 .c，各自编译。
 *
 * 除了能编译，还要数一下到底写了多少数据：
 *   4 页 × (3 条定位命令 + 128 字节) = 12 条命令、512 字节数据
 * 少一条就说明页数或宽度算错了。
 */
#include <stdint.h>

void OLED_WriteCommand(uint8_t cmd);
void OLED_WriteData(uint8_t dat);

#include "selftest32/OLED_SelfTest.h"

static uint32_t g_cmds = 0;
static uint32_t g_datas = 0;

void OLED_WriteCommand(uint8_t cmd) { (void)cmd; ++g_cmds; }
void OLED_WriteData(uint8_t dat) { (void)dat; ++g_datas; }

int main(void)
{
	/* 先落成变量再比：直接 if (宏 != 数字) 会被 /W4 报 C4127（条件表达式是常量），
	 * 那条警告本身没错，但它会盖住真正该看的告警。 */
	const unsigned w = (unsigned)OLED_SELFTEST_W;
	const unsigned h = (unsigned)OLED_SELFTEST_H;
	const unsigned pg = (unsigned)OLED_SELFTEST_PAGES;
	const unsigned sz = (unsigned)OLED_SELFTEST_SIZE;

	if (w != 128u || h != 32u || pg != 4u)
	{
		return 1;
	}
	if (sz != 512u)
	{
		return 2;
	}

	OLED_SelfTest_Show();

	if (g_datas != 512)
	{
		return 3 + (int)(g_datas & 0xFF);      /* 数据量不对 */
	}
	if (g_cmds != 12)
	{
		return 4 + (int)(g_cmds & 0xFF);       /* 定位命令数量不对 */
	}
	return 0;
}
