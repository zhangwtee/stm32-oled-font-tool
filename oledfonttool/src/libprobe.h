/*==============================================================================
 *  libprobe.h  ——  工程用的是哪套库 / 哪个延时函数（接口）
 *
 *  作者   zhangwtee
 *  GitHub https://github.com/zhangwtee
 *  Gitee  https://gitee.com/z_w_t
 *==============================================================================*/
#ifndef LIBPROBE_H
#define LIBPROBE_H

/*==============================================================================
 *  工程底子探测：标准库还是 HAL、延时函数叫什么、写命令怎么调
 *
 *  为什么要探
 *  ----------
 *  导出的头文件里有一小块「适配区」，四个宏要跟用户的工程对上：
 *
 *      CF_WRITE_CMD(c)      写命令
 *      CF_WRITE_DATA(d)     写数据
 *      CF_SHOW_ASCII(...)   字符显示
 *      OLED_DELAY(ms)       帧间延时
 *
 *  以前这四个宏是写死的默认值（OLED_WriteCommand / OLED_WriteData /
 *  OLED_ShowChar / HAL_Delay）。问题是：标准库工程里根本没有 HAL_Delay，
 *  中景园旧版驱动里也没有 OLED_WriteCommand —— 导出、接入、编译，报错在
 *  第三行，而错误信息只会说「未定义的标识符」，指不到取模工具头上。
 *
 *  注释里其实早就写清了「三选一」，但那是让人去选，不是让工具去判断。
 *  这件事工具做得到：工程目录都已经找出来了，读一眼源码就知道。
 *
 *  判据
 *  ----
 *  认内容，不认文件名（工程叫 OLED.c 还是 lcd.c 都一样）：
 *
 *    库类型  HAL        : 有 stm32f1xx_hal.h / HAL_Init( / __HAL_RCC_
 *            标准库     : 有 stm32f10x.h / RCC_APB2PeriphClockCmd( / GPIO_InitStructure
 *    FreeRTOS          : 有 FreeRTOSConfig.h / xTaskCreate( / vTaskDelay(
 *    延时函数          : 先找「定义」（void Delay_ms(...)），再退一步找「调用」
 *    写命令            : 两个函数（XxxWriteCommand + XxxWriteData）
 *                        还是一个函数加标志位（Xxx_WR_Byte(d, 1)）
 *
 *  探不出来就返回 Unknown，调用方保留原来的默认值，并把「没认出来」写在
 *  界面上和文件注释里 —— 宁可说不知道，也不要猜错了还装作很有把握。
 *  这一点和 screenprobe 是同一套规矩。
 *============================================================================*/

#include <QString>
#include <QStringList>

namespace LibProbe
{

enum LibStyle
{
	Unknown        = 0,
	StandardPeriph = 1,     /* 标准外设库：stm32f10x.h + RCC_APB2PeriphClockCmd */
	HAL            = 2      /* HAL 库：stm32f1xx_hal.h + HAL_Init */
};

/* 写命令的两种写法，对应头文件注释里的 (A) 和 (B) */
enum WriteStyle
{
	WriteUnknown = 0,
	TwoFuncs     = 1,       /* OLED_WriteCommand(c) + OLED_WriteData(d) */
	FlagByte     = 2        /* OLED_WR_Byte(d, 1) + OLED_WR_Byte(d, 0) */
};

struct Result
{
	LibStyle    lib      = Unknown;
	bool        freertos = false;
	WriteStyle  write    = WriteUnknown;

	QString     cmdFunc;        /* 写命令函数名，如 OLED_WriteCommand */
	QString     dataFunc;       /* 写数据函数名 */
	QString     byteFunc;       /* FlagByte 写法时的函数名，如 OLED_WR_Byte */
	QString     showAsciiFunc;  /* 字符显示函数名，如 OLED_ShowChar（可能为空） */
	QString     delayFunc;      /* 延时函数名，如 Delay_ms（可能为空） */

	/* 直接能填进适配区的表达式，如 "Delay_ms(ms)"。空 = 没探出来 */
	QString     delayCall;

	QString     file;           /* 依据来自哪个文件（相对工程根） */
	int         line = 0;       /* 依据所在行号（1 基），0 = 没有具体行 */
	QString     evidenceOneLine;/* 一行理由，界面上直接显示 */
	QStringList evidence;       /* 全部依据，悬停时看 */
	QString     error;

	/* 有没有探到「至少一样」有用的东西。全空时调用方走老默认值 */
	bool any() const
	{
		return lib != Unknown || write != WriteUnknown || !delayCall.isEmpty()
		       || !showAsciiFunc.isEmpty();
	}
};

/* 库类型名字，给界面显示用 */
QString nameOf(LibStyle s);

/* 各 IDE 用来标记「这里是一个工程」的文件。带 * 的是通配 */
QStringList projectMarkers();

/*
 * 从任意路径（文件或目录）推出它所属的工程根。
 *
 * 为什么不能简单地「往上退一层」：字库既可能放在 Hardware\ 下面，也可能
 * 直接放在工程根。往上退一层在第一种情况下正好是工程根，在第二种情况下
 * 退到的是「装着所有课程的上一层目录」—— 拿它去扫源码，会认到隔壁工程
 * 的驱动上，然后把别人的函数名写进这个工程的字库里。
 *
 * 判据：
 *   1) 往上找最近的、带工程文件（.uvprojx / .ewp / .cproject ...）的目录，最多 3 层
 *   2) 没有工程文件时，退一步看「这一层是不是直接放着源码」，
 *      是就再往上并，并到上一层不再直接放源码为止（有人把驱动和 main.c
 *      整个拷成一个文件夹，没有 .uvprojx）
 *   3) 都不满足返回空 —— 不猜。调用方拿空的就跳过，不比认错强不到哪去
 */
QString projectRootOf(const QString &anyPath);

/* 在 root 下递归找源码判断。root 可以是工程根，也可以是单个 .c/.h。只读 */
Result probe(const QString &root);

/* 已经有明确文件清单时用，省得再扫目录 */
Result probeFiles(const QStringList &files);

/*
 * 一份「适配区填什么」的结果。
 *
 * 默认值就是改造前的写死值 —— 探不出来时行为不变，不会把老用户坑到。
 * 探出来什么就填什么，origin 里写清依据，导出时打进注释。
 *
 * hasWrite / hasAscii / hasDelay 是「这一项到底探到了没有」。
 * 必须分开记，不能拿字段值去猜：字段值永远是填好的（没探到就是默认值），
 * 光看它分不出「探到 OLED_WriteCommand」和「没探到、留在默认值」。
 * 接入时改写适配区只看这几个标记 —— 没探到的项一个字都不许动，
 * 否则用户手改正确的 OLED_WR_Byte 会被改回默认的 OLED_WriteCommand。
 */
struct CodeAdapter
{
	QString writeCmd   = QStringLiteral("OLED_WriteCommand(c)");
	QString writeData  = QStringLiteral("OLED_WriteData(d)");
	QString showAscii  = QStringLiteral("OLED_ShowChar((line), (col), (ch))");
	QString delayCall  = QStringLiteral("HAL_Delay(ms)");
	QString origin;             /* 一行来源说明；空 = 全用默认值 */
	bool    detected = false;   /* true = 至少一项是探出来的 */

	bool    hasWrite = false;   /* writeCmd / writeData 是探出来的 */
	bool    hasAscii = false;   /* showAscii 是探出来的 */
	bool    hasDelay = false;   /* delayCall 是探出来的 */
	bool    stdLib   = false;   /* 工程是标准外设库（用来提醒默认的 HAL_Delay 一定编不过） */

	/* 探到东西就往这里塞。用 setter 而不是直接写字段，
	 * 是因为忘了置 hasXxx 的话，接入时那一项会被当成"没探到"而静默不生效 ——
	 * 这种错不报、不崩，只是悄悄不改，最难查。 */
	void setWrite(const QString &cmd, const QString &dat)
	{
		writeCmd = cmd;
		writeData = dat;
		hasWrite = true;
	}
	void setShowAscii(const QString &f)
	{
		showAscii = f;
		hasAscii = true;
	}
	void setDelay(const QString &call)
	{
		delayCall = call;
		hasDelay = true;
	}
};

/* 把探测结果翻成适配区参数 */
CodeAdapter adapterOf(const Result &r);

/*
 * 写进头文件适配区上方的那行注释。
 * 探到了：「本适配区按工程源码自动识别：标准库 · 延时 Delay_ms(ms)」
 * 没探到：提醒这仍是默认值，需要自己按注释里的三种写法改
 */
QString adapterNote(const CodeAdapter &a);

}   // namespace LibProbe

#endif   // LIBPROBE_H
