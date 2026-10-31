/*==============================================================================
 *  screenprobe.h  ——  屏控制器探测（接口）
 *
 *  作者   zhangwtee
 *  GitHub https://github.com/zhangwtee
 *  Gitee  https://gitee.com/z_w_t
 *==============================================================================*/
#ifndef SCREENPROBE_H
#define SCREENPROBE_H

/*==============================================================================
 *  屏控制器探测：SSD1306 还是 SH1106
 *
 *  为什么要探
 *  ----------
 *  SSD1306 显存 128 列，列地址就写 0~127，打底命令是 0x00。
 *  SH1106   显存 132 列，可见区从第 2 列开始，所以每条列地址都要 +2，
 *           否则整幅画左移 2 像素、右边少两列。
 *
 *  这个差别不体现在数据上，只体现在「往哪个列地址写」。字库、动画、自检帧
 *  三份导出的文件里各有一个 *_COL_OFFSET 宏，默认 0。以前要用户自己去翻
 *  驱动、再手动把三个地方都改成 2 —— 记不住、也容易只改一处。
 *
 *  现在改成从驱动源码里读出来。判断依据是代码内容，不是文件名，
 *  所以 OLED.c / lcd.c / oled_drv.c 都一样找得到。
 *
 *  判据（按可信度从高到低）
 *  ------------------------
 *   1. 源码里写了型号字样：SH1106 / SSD1306（注释里写型号很常见）
 *   2. 设列地址的地方多加了 2：含 0xB0（页地址）的函数体里出现 +2 / 0x02
 *   3. 出现 0x83（132 列的末列）/ 132 这类只有 SH1106 才有的常量
 *   4. 出现 0x21、0x22（SSD1306 才有的寻址设置命令）
 *
 *  认不出来就返回 Unknown，调用方按最常见的 SSD1306（偏移 0）处理，
 *  并在界面上说清「没认出来」，让用户能自己改。宁可说不知道，
 *  也不要猜错了还装作很有把握。
 *============================================================================*/

#include <QString>
#include <QStringList>

namespace ScreenProbe
{

enum Controller
{
	Unknown = 0,
	SSD1306 = 1,
	SH1106  = 2
};

struct Result
{
	Controller  controller = Unknown;
	int         colOffset  = 0;         /* SSD1306 = 0，SH1106 = 2 */
	QString     file;                   /* 判定依据来自哪个文件 */
	int         line = 0;               /* 依据所在行号（1 基），0 = 没有具体行 */
	QString     evidenceOneLine;        /* 一行理由，界面上直接显示 */
	QStringList evidence;               /* 全部依据，鼠标悬停时看 */
	QString     error;

	bool ok() const { return controller != Unknown; }
};

/* 控制器名字，给界面下拉用 */
QString nameOf(Controller c);

/* 该控制器的列偏移量 */
int offsetOf(Controller c);

/* 反过来：从列偏移推控制器（用户手动选了「偏移 2」时用） */
Controller fromOffset(int offset);

/*
 * 在 root 下递归找驱动源码并判断。
 * root 可以是工程根目录，也可以直接是一个 .c/.h 文件。
 * 只读，不碰任何文件。
 */
Result probe(const QString &root);

/* 只查这几份文件 —— 已经有明确目标时用，省得再扫一遍目录 */
Result probeFiles(const QStringList &files);

}   // namespace ScreenProbe

#endif   // SCREENPROBE_H
