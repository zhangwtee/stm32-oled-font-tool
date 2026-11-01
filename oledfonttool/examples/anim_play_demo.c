// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 zhangwtee <https://github.com/zhangwtee>
//
// 这是「OLED 点阵取模工具」的动图播放示例，不是工具本身的一部分。
// 整段抄进你自己的 main.c 就行，抄的时候把这几行注释删掉没人管。

/* ==========================================================================
 *  动图播放示例：三种场景，改 DEMO_MODE 选一个
 *
 *    DEMO_MODE 1   屏上只放这一段动画（最简单，能跑通接线）
 *    DEMO_MODE 2   主循环里还有按键 / 传感器要做（非阻塞）
 *    DEMO_MODE 3   几张图按条件切换（编码器一转换一张）
 *
 *  用到的名字（以导出的 anim.h 为例）：
 *
 *    Anim_ShowFrame(i)   把第 i 帧整屏刷上去。i 从 0 到 ANIM_COUNT - 1
 *    ANIM_COUNT          逻辑帧数 —— 播放循环一律用它
 *    ANIM_DELAY_MS       帧间隔（毫秒），导出时定的
 *    ANIM_DELAY(ms)      适配区里的延时宏，指向你工程的延时函数
 *
 *  三个前提，缺一个就不亮：
 *    1. 头文件里【适配区】的三个宏已经指向你驱动的写命令 / 写数据 / 延时函数
 *    2. 头文件只在**一个** .c 里 include（数组不是 static，多处包含会重复定义）
 *    3. 调用之前 OLED_Init() 已经跑过
 *
 *  本文件是 UTF-8 **带 BOM** 存的，Keil 里直接打开中文注释不会乱码。
 *  你要是另存成别的编码，记得把 Keil 的 Edit -> Configuration -> Encoding 对上。
 * ========================================================================== */

#define DEMO_MODE 1        /* ← 改这里：1 / 2 / 3 */

/* -------------------------------------------------------------------------- */
/*  公共部分：工程里本来就有的东西                                              */
/* -------------------------------------------------------------------------- */

#include "OLED.h"          /* 你的驱动头文件，里面要有 OLED_Init() */

#if DEMO_MODE == 3
#include "oled_assets.h"   /* 多份素材打包导出的那个文件 */
#else
#include "anim.h"          /* 单个动画导出的那个文件；文件名按你导出时起的改 */
#endif


/* 毫秒计时。DEMO_MODE 2 / 3 要用，换成你工程里现成的那个：
 *
 *   HAL 库      : HAL_GetTick()
 *   标准库      : 中景园 delay.c 里的 now_time（在 SysTick 中断里累加）
 *                 正点原子 delay.c 里的 g_sys_run_time
 *   自己写的    : 下面这个就够用，前提是 SysTick 每 1ms 进一次这个函数
 */
#if DEMO_MODE != 1
static uint32_t millis_now(void)
{
#if defined(USE_HAL_DRIVER)
    return HAL_GetTick();
#else
    extern volatile uint32_t now_time;   /* ← 换成你工程里那个变量 */
    return now_time;
#endif
}
#endif


/* -------------------------------------------------------------------------- */
/*  场景一：屏上只放这一段动画                                                 */
/* -------------------------------------------------------------------------- */

#if DEMO_MODE == 1

int main(void)
{
    OLED_Init();

    while (1)
    {
        uint16_t i;

        for (i = 0; i < ANIM_COUNT; i++)
        {
            Anim_ShowFrame(i);
            ANIM_DELAY(ANIM_DELAY_MS);   /* 死等，等待期间什么都干不了 */
        }
    }
}

/* 说明：
 *  - ANIM_DELAY_MS 是导出时设的帧间隔。想改快慢，回到工具里改「帧间隔」重新导出，
 *    别在这里手改数字 —— 下次重新导出会被覆盖。
 *  - ANIM_COUNT 是逻辑帧数。导出的文件里如果还有 ANIM_DATA_COUNT，那是"实际存了
 *    几帧"（重复帧被合并了），播放循环**不要**用它，否则动画会变快。
 */


/* -------------------------------------------------------------------------- */
/*  场景二：主循环里还有别的事要做 —— 非阻塞                                     */
/* -------------------------------------------------------------------------- */

#elif DEMO_MODE == 2

static uint16_t s_frame;      /* 现在放到第几帧 */
static uint32_t s_last;       /* 上一帧刷新的时刻 */

int main(void)
{
    OLED_Init();
    s_last = millis_now();

    while (1)
    {
        /* 你自己的活照旧干，不会被动画挡住 */
        Key_Scan();
        Sensor_Poll();

        /* 到点了才刷一帧，刷完立刻回主循环 */
        {
            uint32_t now = millis_now();

            if (now - s_last >= ANIM_DELAY_MS)
            {
                s_last = now;
                Anim_ShowFrame(s_frame);
                s_frame = (uint16_t)((s_frame + 1) % ANIM_COUNT);
            }
        }
    }
}

/* 为什么不能直接用场景一那个 while(1)：
 *   ANIM_DELAY() 是死等。128x64 一帧要发 1024 个字节，刷一帧本身就要几毫秒，
 *   再死等 80ms，这 80ms 里按键扫描一次都轮不到 —— 按下去没反应，不是按键坏了。
 *   上面这个写法把"等"换成了"到点才做"，主循环转得飞快，按键随时能响应。
 *
 * 帧率会略低于设的值（刷一帧的耗时也算在里面），对动画无所谓；
 * 真要卡准，把间隔改成 (ANIM_DELAY_MS - 刷一帧的耗时)，别去改延时函数。
 */


/* -------------------------------------------------------------------------- */
/*  场景三：几张图按条件切换                                                   */
/* -------------------------------------------------------------------------- */

#elif DEMO_MODE == 3

/* 打包导出的 oled_assets.h 里，每份素材的显示函数签名都一样：
 *     static void Xxx_ShowFrame(uint16_t i);
 * 所以可以做成表，按下标调，不用写一长串 if/else。
 *
 * 名字规则：素材叫 Img_3，就是 Img_3_ShowFrame / IMG_3_COUNT / IMG_3_DELAY_MS。
 */
typedef void (*ShowFn)(uint16_t);

static const ShowFn   kShow[]  = { Img_3_ShowFrame, Miao1_ShowFrame, Xiantiaoxiaogou1_ShowFrame };
static const uint16_t kCount[] = { IMG_3_COUNT,     MIAO1_COUNT,     XIANTIAOXIAOGOU1_COUNT };
static const uint16_t kDelay[] = { IMG_3_DELAY_MS,  MIAO1_DELAY_MS,  XIANTIAOXIAOGOU1_DELAY_MS };

#define PIC_NUM   ((uint8_t)(sizeof(kShow) / sizeof(kShow[0])))

static uint8_t  s_idx;        /* 现在放第几张 */
static uint16_t s_frame;      /* 这一张放到第几帧 */
static uint32_t s_last;

int main(void)
{
    OLED_Init();

    s_idx   = 0;
    s_frame = 0;
    s_last  = millis_now();

    while (1)
    {
        /* ---- 换图：编码器转一格就换下一张 ---- */
        if (Encoder_Get() != 0)          /* 换成你的按键 / 传感器判断 */
        {
            s_idx   = (uint8_t)((s_idx + 1) % PIC_NUM);
            s_frame = 0;                 /* 新的一张从第 0 帧开始 */
            s_last  = 0;                 /* 置 0 表示"下一轮立刻刷"，不用等间隔 */
        }

        /* ---- 放当前这一张 ---- */
        {
            uint32_t now = millis_now();

            if (now - s_last >= kDelay[s_idx])
            {
                s_last  = now;
                kShow[s_idx](s_frame);                          /* 调当前这张 */
                s_frame = (uint16_t)((s_frame + 1) % kCount[s_idx]);  /* 放完循环 */
            }
        }
    }
}

/* 说明：
 *  - 每张图的帧间隔可以不一样，所以 kDelay 也做成表；都一样就写死一个数。
 *  - s_frame 一定要在新图开始时清 0：不清的话，切到帧数少的那张会从中间开始，
 *    看着像"缺了几帧"。
 *  - 只想放一张不切换，把 Encoder_Get() 那段删掉即可。
 *  - 图多了 Flash 会吃紧：128x64 一帧 1KB，五张 20 帧就是 20KB，C8T6 只有 64KB。
 *    导出时注意工具底部那行「合计」。
 */

#else
#error "DEMO_MODE 只能是 1 / 2 / 3"
#endif
