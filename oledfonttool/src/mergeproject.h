#ifndef MERGEPROJECT_H
#define MERGEPROJECT_H

/*==============================================================================
 *  一键接入：把「字库头文件怎么用」这件事，直接写进用户的工程里
 *
 *  解决的问题
 *  ----------
 *  OLED_CFont.h 自带显示函数，只跟驱动交换「写命令」「写数据」两个动作。
 *  但这两步仍然要手工做：
 *      1) 在驱动的 .h 里补两行函数声明（函数本来在 .c 里，只是没导出）
 *      2) 在 main.c 里补 #include 和两三行示例
 *  工具已经把工程目录找到了，这两步完全可以代劳。
 *
 *  设计原则
 *  --------
 *  · 只加不改：绝不修改用户已有的任何一行，只做「插入」。
 *  · 可预览：真正写盘前把每一处插入的文件、行号、内容全列出来。
 *  · 可撤销：每个被改的文件先备份成 <文件名>.oledbak，随时能还原。
 *  · 幂等：已经有的东西不再加第二遍，重复点不会堆出三份声明。
 *  · 认内容不认名字：驱动是 OLED.c 还是 lcd.c 都一样找得到，
 *    判据是「这个 .c 里定义了写命令函数」而不是文件名。
 *============================================================================*/

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace MergeProject {

/* 一处插入 */
struct Step
{
	QString file;               /* 目标文件绝对路径 */
	int     afterLine = 0;      /* 插在第几行之后（1 基）；0 = 插到文件最前面 */
	QString text;               /* 要插入的内容，不含行尾符 */
	QString title;              /* 一句话说明，给用户看 */
	QString detail;             /* 依据 / 所在位置 */
	bool    checked = true;     /* 默认勾选 */
};

struct Scan
{
	QString root;               /* 工程根 */
	QString fontHeader;         /* 字库头文件 */
	QString driverC;            /* 定义写命令函数的 .c */
	QString driverH;            /* 它对应的 .h */
	QString mainC;              /* 调用 OLED_Init 的那个 .c */
	QString cmdFunc;            /* 嗅探到的「写命令」函数名 */
	QString dataFunc;           /* 嗅探到的「写数据」函数名 */
	QString initFunc;           /* main.c 里实际调用的初始化函数名 */

	QVector<Step> steps;
	QStringList   notes;        /* 需要人看一眼、但工具不该动的事 */
	QString       error;
};

/*
 * 扫描工程，生成待插入清单。只读，不碰任何文件。
 *
 * fontHeaderPath ：已经导出好的 OLED_CFont.h 位置，用来推工程根、算 include 相对路径
 * sample1/sample2：写进 main.c 的两行示例文字（用户可以改）
 */
bool detect(const QString &fontHeaderPath,
            const QString &sample1,
            const QString &sample2,
            Scan *out);

/*
 * 素材包（动画页导出的那个 .h）的接入。流程和 detect 一样，只是：
 *   · 幂等判断按实际文件名来，不再写死 OLED_CFont.h
 *   · 示例是「刷第 0 帧」一行，插在 OLED_Init() 之后，不动 while(1)
 *
 * showFunc：要写进示例的显示函数名（listAssetShowFuncs 里挑一个）。
 *           传空就只补 include，不插示例。
 */
bool detectAssets(const QString &assetsHeaderPath,
                  const QString &showFunc,
                  Scan *out);

/* 从素材包头里挑出所有 Xxx_ShowFrame 函数名，供界面下拉选 */
QStringList listAssetShowFuncs(const QString &headerPath);

/*
 * 把某个文件应用完这些 step 之后的样子，返回「改动行 ± 上下文」的文本。
 * 用于写盘前让用户确认。返回值带行号前缀，一眼能看出插在哪。
 */
QString previewOf(const QString &file, const QVector<Step> &steps, int context = 3);

/* 真的写盘。每个文件第一次被改时备份成 <文件名>.oledbak */
bool apply(const Scan &scan, QStringList *touched, QString *err);

/* 从 .oledbak 还原。files 传 apply() 吐出来的那个列表 */
bool revert(const QStringList &files, QString *err);

/* 有没有备份可退（用来决定「撤销」按钮亮不亮） */
QStringList revertable(const QStringList &files);

/* 读文件、按内容判断编码（返回可读文本，供界面预览用） */
QString readAsText(const QString &path);

/*
 * 源文件的「能不能编译过去」这一关，三选一。
 *
 * 和 readAsText 那种"能不能按 UTF-8 解码"不是一回事：文件是合法 UTF-8，
 * 但少了 BOM，armcc（AC5）照样按系统代码页当双字节解析，中文串会把后面的
 * 引号吞掉，报 #8: missing closing quote。查表能对上、编译却过不去。
 *
 * hasNonAsciiLiteral 由调用方扫出来传进来 —— 中文注释不影响查表，也不算问题。
 */
enum class SrcIssue
{
	None,       /* 没问题，或者压根没有中文字符串字面量 */
	NotUtf8,    /* 按 GBK/ANSI 存的：编译能过，但查表对不上，屏幕全空 */
	NoBom       /* UTF-8 但没 BOM：查表对得上，armcc 直接报语法错 */
};

SrcIssue judgeSource(const QByteArray &raw, bool hasNonAsciiLiteral);

}   // namespace MergeProject

#endif   // MERGEPROJECT_H
