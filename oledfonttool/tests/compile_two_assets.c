/* 两份素材的头文件放进同一个翻译单元 —— 「分开存放」真正要成立的场景。
 *
 * 改动前两份都叫 ANIM_WIDTH / Anim_Frames / Anim_ShowFrame，include guard
 * 也都是 __ANIM_FRAMES_H。第二条 #include 会被 guard 整份吃掉，一声不响：
 * 编译通过、没有警告，但第二份的帧数据和宏根本不存在，屏上永远是第一份。
 * 这比报错更难查，所以「一份素材一个 .h」在改动前是假的多份。
 *
 * 现在导出时按素材名加前缀（fixture/logo.h → LOGO_WIDTH / Logo_ShowFrame /
 * __LOGO_FRAMES_H，fixture/move.h → MOVE_* / Move_*），两份能同时包含：
 * 宏各自独立、数组不重名、guard 互不干扰。
 *
 * 编译验证：cl /W4 /utf-8 编本文件，不该有任何一条来自头文件的警告；
 * 链接运行时把写入量按各自的帧数对一遍。参数见 DEVELOPMENT.md 的验收方式。
 */
#include <stdint.h>
#include <stdio.h>

void OLED_WriteCommand(uint8_t cmd);
void OLED_WriteData(uint8_t dat);
void HAL_Delay(uint32_t ms);

#include "logo.h"
#include "move.h"

/* 再包含一次：两份的 guard 各自独立，应该各挡自己的，而不是第二个被第一个挡掉 */
#include "logo.h"
#include "move.h"

#ifndef __LOGO_FRAMES_H
#error "logo.h 的 include guard 没生效"
#endif
#ifndef __MOVE_FRAMES_H
#error "move.h 的 include guard 没生效"
#endif

/* 宏是编译期常量，用预处理比较，省得 /W4 报 C4127（条件表达式是常量） */
#if LOGO_WIDTH != 128 || MOVE_WIDTH != 128
#error "两份的宽度宏被互相覆盖了"
#endif
#if LOGO_HEIGHT != 64 || MOVE_HEIGHT != 64
#error "两份的高度宏被互相覆盖了"
#endif
#if LOGO_COUNT != 2
#error "LOGO_COUNT 不是 2"
#endif
#if MOVE_COUNT != 3
#error "MOVE_COUNT 不是 3"
#endif
#if LOGO_DELAY_MS != 100 || MOVE_DELAY_MS != 80
#error "两份的帧延时宏被互相覆盖了"
#endif

static uint32_t g_cmds = 0;
static uint32_t g_datas = 0;

void OLED_WriteCommand(uint8_t cmd) { (void)cmd; ++g_cmds; }
void OLED_WriteData(uint8_t dat)    { (void)dat; ++g_datas; }
void HAL_Delay(uint32_t ms)         { (void)ms; }

int main(void)
{
	/* 两个数组各自独立，长度跟着自己的帧数走 —— 这是被 guard 吞掉时最直接的破绽 */
	const size_t logoBytes = sizeof(Logo_Frames);
	const size_t moveBytes = sizeof(Move_Frames);

	if (logoBytes != (size_t)LOGO_COUNT * (size_t)LOGO_FRAME_SIZE)
	{
		return 1;
	}
	if (moveBytes != (size_t)MOVE_COUNT * (size_t)MOVE_FRAME_SIZE)
	{
		return 2;
	}

	/* 数据不能是同一份（被覆盖时会一模一样） */
	if (Logo_Frames[0][0] == Move_Frames[0][0])
	{
		return 3;
	}

	/* 各自刷自己的帧，写入量按各自的帧数算：每帧 8 页 × 128 字节、24 条定位命令 */
	g_cmds  = 0;
	g_datas = 0;
	Logo_ShowFrame(0);
	Logo_ShowFrame(1);
	Logo_ShowFrame(2);                  /* 越界：不许写，也不许崩 */
	if (g_datas != 2u * 1024u || g_cmds != 2u * 24u)
	{
		return 4;
	}

	g_cmds  = 0;
	g_datas = 0;
	Move_ShowFrame(0);
	Move_ShowFrame(1);
	Move_ShowFrame(2);
	Move_ShowFrame(3);                  /* 越界 */
	if (g_datas != 3u * 1024u || g_cmds != 3u * 24u)
	{
		return 5;
	}

	printf("两份素材在同一个翻译单元里共存：ok\n");
	return 0;
}
