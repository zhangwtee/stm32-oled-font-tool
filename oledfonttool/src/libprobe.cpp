/*==============================================================================
 *  libprobe.cpp  ——  工程用的是哪套库 / 哪个延时函数（实现）
 *
 *  作者   zhangwtee
 *  GitHub https://github.com/zhangwtee
 *  Gitee  https://gitee.com/z_w_t
 *==============================================================================*/
#include "libprobe.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace
{

/* 扫目录时的限制：一个 Keil 工程里 Library/ 下几十个头文件，加上 HAL 的
 * Drivers/ 能到几千个。这里只为了认几个关键字，没必要全读。 */
const int kMaxFiles = 1500;
const int kMaxDepth = 6;
const qint64 kMaxFileBytes = 256 * 1024;

bool isSourceName(const QString &name)
{
	return name.endsWith(QLatin1String(".c"), Qt::CaseInsensitive)
	       || name.endsWith(QLatin1String(".h"), Qt::CaseInsensitive)
	       || name.endsWith(QLatin1String(".cpp"), Qt::CaseInsensitive)
	       || name.endsWith(QLatin1String(".hpp"), Qt::CaseInsensitive);
}

/* 这些目录里全是编译产物或者第三方库，读它们只浪费时间和误判机会。
 *
 * 但 Drivers 不能跳：HAL 工程的招牌 stm32f1xx_hal.h 就躺在
 * Drivers/STM32F1xx_HAL_Driver/Inc/ 下面，跳掉等于把最硬的证据扔了。
 * 那里的头文件确实多，不过文件数和单文件字节数都有上限兜着。 */
bool isSkippedDir(const QString &name)
{
	static const QStringList skip = {
		QStringLiteral(".git"), QStringLiteral(".svn"), QStringLiteral(".vscode"),
		QStringLiteral(".idea"), QStringLiteral("build"), QStringLiteral("Build"),
		QStringLiteral("Objects"), QStringLiteral("Listings"), QStringLiteral("Output"),
		QStringLiteral("DebugConfig"), QStringLiteral("__pycache__"),
		QStringLiteral("RTE"), QStringLiteral("Middlewares")
	};
	return skip.contains(name, Qt::CaseInsensitive);
}

void walk(const QString &dir, QStringList *out, int *budget, int depth)
{
	if (*budget <= 0 || depth > kMaxDepth)
	{
		return;
	}

	const QFileInfoList entries = QDir(dir).entryInfoList(
		QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);

	for (const QFileInfo &fi : entries)
	{
		if (*budget <= 0)
		{
			return;
		}
		if (fi.isDir())
		{
			if (isSkippedDir(fi.fileName()))
			{
				continue;
			}
			walk(fi.absoluteFilePath(), out, budget, depth + 1);
		}
		else if (isSourceName(fi.fileName()))
		{
			out->append(fi.absoluteFilePath());
			--(*budget);
		}
	}
}

/* 工具自己生成的头文件不参与识别。
 *
 * 不挡的话会出现自我印证：导出的 OLED_CFont.h 里有
 *     #define CF_SHOW_ASCII(line, col, ch)  OLED_ShowChar((line), (col), (ch))
 * 下次探测读到自己这个文件，就"发现"工程里有 OLED_ShowChar —— 其实那是
 * 上一次自己写进去的默认值。认出来的东西必须来自用户的驱动，不能来自
 * 自己的产物，否则错了也永远纠不回来。
 *
 * 判据用的是本工具独有的几个名字，不是 CF_WRITE_CMD / OLED_WRITE_CMD 那几行：
 * 有些驱动会写 #define OLED_WRITE_CMD(c) 这类带参宏，拿它当判据就会把用户
 * 真正的驱动整个跳过 —— 那比自我印证还糟，直接什么都探不到了。 */
bool looksGenerated(const QString &text)
{
	return text.contains(QLatin1String("__OLED_CFONT_H"))
	       || text.contains(QLatin1String("OLED_CFONT_STRIDE"))
	       || text.contains(QLatin1String("__ANIM_FRAMES_H"))
	       || text.contains(QLatin1String("ANIM_FRAME_SIZE"));
}

/* 按 1 基行号，给证据用 */
int lineOf(const QString &text, int pos)
{
	return text.left(pos).count(QLatin1Char('\n')) + 1;
}

/* 源码可能有 GBK 的，但这里只认 ASCII 关键字。按 Latin-1 读进来，
 * 一个字节对一字符，中文不会把关键字拆坏，也不用猜编码。 */
QString readRaw(const QString &path)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
	{
		return QString();
	}
	const QByteArray raw = f.read(kMaxFileBytes);
	return QString::fromLatin1(raw);
}

struct Hit
{
	bool    found = false;
	QString name;
	QString file;
	int     line = 0;
	QString what;       /* 一行说明 */
};

void note(Hit *h, const QString &name, const QString &file, int line, const QString &what)
{
	if (h->found)
	{
		return;                 /* 先到先得，别让后面的覆盖掉 */
	}
	h->found = true;
	h->name  = name;
	h->file  = file;
	h->line  = line;
	h->what  = what;
}

}   // namespace

namespace LibProbe
{

QString nameOf(LibStyle s)
{
	switch (s)
	{
	case StandardPeriph: return QStringLiteral("标准库");
	case HAL:            return QStringLiteral("HAL 库");
	default:             return QStringLiteral("未知");
	}
}

namespace
{

/* 一次扫描把所有要认的东西都收齐。认内容不认文件名 */
void scanOne(const QString &path, Result *r, int *stdScore, int *halScore,
             Hit *delayDef, Hit *delayMs, Hit *delayHal, Hit *delayRtos,
             Hit *twoCmd, Hit *twoData, Hit *flagByte, Hit *showChar,
             QStringList *evidence)
{
	const QFileInfo fi(path);
	const QString name = fi.fileName();
	const QString text = readRaw(path);
	if (text.isEmpty())
	{
		return;
	}
	if (looksGenerated(text))
	{
		return;
	}

	/* ---- 库类型：先看有没有那个标志性的头文件 ---- */
	if (name.compare(QLatin1String("stm32f10x.h"), Qt::CaseInsensitive) == 0)
	{
		*stdScore += 6;
		evidence->append(QStringLiteral("%1：标准库的标志性头文件").arg(name));
	}
	if (name.compare(QLatin1String("stm32f1xx_hal.h"), Qt::CaseInsensitive) == 0
	    || name.compare(QLatin1String("stm32f1xx_hal_conf.h"), Qt::CaseInsensitive) == 0)
	{
		*halScore += 6;
		evidence->append(QStringLiteral("%1：HAL 库的标志性头文件").arg(name));
	}
	if (name.compare(QLatin1String("FreeRTOSConfig.h"), Qt::CaseInsensitive) == 0)
	{
		r->freertos = true;
		evidence->append(QStringLiteral("%1：工程里有 FreeRTOS").arg(name));
	}

	/* ---- 库类型：再看代码里怎么点外设 ---- */
	static const QRegularExpression reStdLib(
		QStringLiteral("RCC_APB2PeriphClockCmd\\s*\\("));
	static const QRegularExpression reStdConf(
		QStringLiteral("stm32f10x_conf\\.h"));
	static const QRegularExpression reStdGpio(
		QStringLiteral("GPIO_InitStructure"));
	static const QRegularExpression reHalInit(
		QStringLiteral("HAL_Init\\s*\\("));
	static const QRegularExpression reHalRcc(
		QStringLiteral("__HAL_RCC_[A-Z0-9_]+_CLK_ENABLE"));

	QRegularExpressionMatch m = reStdLib.match(text);
	if (m.hasMatch())
	{
		*stdScore += 5;
		evidence->append(QStringLiteral("%1:%2：调用了 RCC_APB2PeriphClockCmd")
		                 .arg(name).arg(lineOf(text, m.capturedStart())));
	}
	m = reStdConf.match(text);
	if (m.hasMatch())
	{
		*stdScore += 4;
		evidence->append(QStringLiteral("%1：包含 stm32f10x_conf.h").arg(name));
	}
	m = reStdGpio.match(text);
	if (m.hasMatch())
	{
		*stdScore += 2;
		evidence->append(QStringLiteral("%1：用的是标准库的 GPIO_InitStructure").arg(name));
	}
	m = reHalInit.match(text);
	if (m.hasMatch())
	{
		*halScore += 5;
		evidence->append(QStringLiteral("%1:%2：调用了 HAL_Init")
		                 .arg(name).arg(lineOf(text, m.capturedStart())));
	}
	m = reHalRcc.match(text);
	if (m.hasMatch())
	{
		*halScore += 4;
		evidence->append(QStringLiteral("%1:%2：用的是 __HAL_RCC_ 宏")
		                 .arg(name).arg(lineOf(text, m.capturedStart())));
	}

	/* ---- FreeRTOS ---- */
	static const QRegularExpression reTask(
		QStringLiteral("\\b(xTaskCreate|osThreadNew)\\s*\\("));
	static const QRegularExpression reRtosDelay(
		QStringLiteral("\\bvTaskDelay\\s*\\("));
	if (reTask.match(text).hasMatch())
	{
		r->freertos = true;
		evidence->append(QStringLiteral("%1：用了 RTOS 的任务创建").arg(name));
	}

	/* ---- 延时函数：先找定义，再找调用 ---- */
	static const QRegularExpression reDelayDef(
		QStringLiteral("^[ \\t]*(?:void|VOID)[ \\t]+([A-Za-z_][A-Za-z0-9_]*)[ \\t]*\\("),
		QRegularExpression::MultilineOption);
	QRegularExpressionMatchIterator it = reDelayDef.globalMatch(text);
	while (it.hasNext())
	{
		const QRegularExpressionMatch dm = it.next();
		const QString fn = dm.captured(1);
		const QString lower = fn.toLower();
		if (lower == QLatin1String("delay_ms") || lower == QLatin1String("delayms"))
		{
			note(delayDef, fn, name, lineOf(text, dm.capturedStart(1)),
			     QStringLiteral("%1:%2 里定义了 void %3(...)")
			     .arg(name).arg(lineOf(text, dm.capturedStart(1))).arg(fn));
			break;
		}
	}

	static const QRegularExpression reDelayCall(
		QStringLiteral("\\b(Delay_ms|delay_ms|DelayMs|delayMs)[ \\t]*\\("),
		QRegularExpression::CaseInsensitiveOption);
	m = reDelayCall.match(text);
	if (m.hasMatch())
	{
		note(delayMs, m.captured(1), name, lineOf(text, m.capturedStart()),
		     QStringLiteral("%1:%2 调用了 %3(ms)")
		     .arg(name).arg(lineOf(text, m.capturedStart())).arg(m.captured(1)));
	}

	static const QRegularExpression reHalDelay(QStringLiteral("\\bHAL_Delay[ \\t]*\\("));
	m = reHalDelay.match(text);
	if (m.hasMatch())
	{
		note(delayHal, QStringLiteral("HAL_Delay"), name, lineOf(text, m.capturedStart()),
		     QStringLiteral("%1:%2 调用了 HAL_Delay(ms)")
		     .arg(name).arg(lineOf(text, m.capturedStart())));
	}

	if (reRtosDelay.match(text).hasMatch())
	{
		note(delayRtos, QStringLiteral("vTaskDelay"), name,
		     lineOf(text, reRtosDelay.match(text).capturedStart()),
		     QStringLiteral("%1 里用了 vTaskDelay").arg(name));
	}

	/* ---- 写命令：两个函数 / 一个函数加标志位 ---- */
	static const QRegularExpression reCmd(
		QStringLiteral("\\b([A-Za-z_][A-Za-z0-9_]*WriteCommand)[ \\t]*\\("));
	static const QRegularExpression reData(
		QStringLiteral("\\b([A-Za-z_][A-Za-z0-9_]*WriteData)[ \\t]*\\("));
	static const QRegularExpression reByte(
		QStringLiteral("\\b([A-Za-z_][A-Za-z0-9_]*WR_Byte)[ \\t]*\\("));

	m = reCmd.match(text);
	if (m.hasMatch())
	{
		note(twoCmd, m.captured(1), name, lineOf(text, m.capturedStart()),
		     QStringLiteral("%1：写命令函数 %2").arg(name, m.captured(1)));
	}
	m = reData.match(text);
	if (m.hasMatch())
	{
		note(twoData, m.captured(1), name, lineOf(text, m.capturedStart()),
		     QStringLiteral("%1：写数据函数 %2").arg(name, m.captured(1)));
	}
	m = reByte.match(text);
	if (m.hasMatch())
	{
		note(flagByte, m.captured(1), name, lineOf(text, m.capturedStart()),
		     QStringLiteral("%1：写字节函数 %2（带 cmd 标志位）").arg(name, m.captured(1)));
	}

	/* ---- 字符显示函数 ---- */
	static const QRegularExpression reShowChar(
		QStringLiteral("\\b([A-Za-z_][A-Za-z0-9_]*ShowChar)[ \\t]*\\("));
	m = reShowChar.match(text);
	if (m.hasMatch())
	{
		note(showChar, m.captured(1), name, lineOf(text, m.capturedStart()),
		     QStringLiteral("%1：有字符显示函数 %2").arg(name, m.captured(1)));
	}
}

}   // namespace

Result probeFiles(const QStringList &files)
{
	Result r;
	if (files.isEmpty())
	{
		r.error = QStringLiteral("没有可扫描的源码文件");
		return r;
	}

	int stdScore = 0;
	int halScore = 0;
	Hit delayDef, delayMs, delayHal, delayRtos;
	Hit twoCmd, twoData, flagByte, showChar;
	QStringList evidence;

	for (const QString &f : files)
	{
		scanOne(f, &r, &stdScore, &halScore,
		        &delayDef, &delayMs, &delayHal, &delayRtos,
		        &twoCmd, &twoData, &flagByte, &showChar, &evidence);
	}

	if (stdScore > halScore)
	{
		r.lib = StandardPeriph;
		r.file = twoCmd.file.isEmpty() ? delayDef.file : twoCmd.file;
	}
	else if (halScore > stdScore)
	{
		r.lib = HAL;
	}

	/* 写命令写法：两个函数优先。中景园新版 / 多数 HAL 例程是这样，
	 * 也是头文件注释里的 (A)，和改造前的默认值一致 */
	if (twoCmd.found && twoData.found)
	{
		r.write   = TwoFuncs;
		r.cmdFunc = twoCmd.name;
		r.dataFunc = twoData.name;
		r.file = twoCmd.file;
		r.line = twoCmd.line;
		evidence.prepend(twoCmd.what);
	}
	else if (flagByte.found)
	{
		r.write    = FlagByte;
		r.byteFunc = flagByte.name;
		r.file     = flagByte.file;
		r.line     = flagByte.line;
		evidence.prepend(flagByte.what);
	}

	if (showChar.found)
	{
		r.showAsciiFunc = showChar.name;
	}

	/* 延时：定义 > 工程自己的调用 > HAL > RTOS。
	 * 这个顺序的道理是：工程里真定义了 Delay_ms，就说明它自己有一套；
	 * 只有 HAL 工程才会去调 HAL_Delay。 */
	if (delayDef.found)
	{
		r.delayFunc = delayDef.name;
		r.delayCall = QStringLiteral("%1(ms)").arg(delayDef.name);
		evidence.prepend(delayDef.what);
	}
	else if (delayMs.found)
	{
		r.delayFunc = delayMs.name;
		r.delayCall = QStringLiteral("%1(ms)").arg(delayMs.name);
		evidence.prepend(delayMs.what);
	}
	else if (delayHal.found)
	{
		r.delayFunc = QStringLiteral("HAL_Delay");
		r.delayCall = QStringLiteral("HAL_Delay(ms)");
		evidence.prepend(delayHal.what);
	}
	else if (delayRtos.found && r.freertos)
	{
		r.delayFunc = QStringLiteral("vTaskDelay");
		r.delayCall = QStringLiteral("vTaskDelay(pdMS_TO_TICKS(ms))");
		evidence.prepend(delayRtos.what);
	}

	/* 一行结论，界面上直接显示 */
	QStringList bits;
	if (r.lib != Unknown)
	{
		bits << nameOf(r.lib);
	}
	if (!r.delayCall.isEmpty())
	{
		bits << QStringLiteral("延时 %1").arg(r.delayCall);
	}
	if (r.write == TwoFuncs)
	{
		bits << QStringLiteral("写命令 %1/%2").arg(r.cmdFunc, r.dataFunc);
	}
	else if (r.write == FlagByte)
	{
		bits << QStringLiteral("写命令 %1").arg(r.byteFunc);
	}
	if (r.freertos)
	{
		bits << QStringLiteral("FreeRTOS");
	}
	r.evidenceOneLine = bits.isEmpty()
	        ? QStringLiteral("没能从源码里认出库类型和延时函数")
	        : bits.join(QStringLiteral(" · "));

	r.evidence = evidence;
	if (!r.any())
	{
		r.error = QStringLiteral("工程里没找到能确认库类型的写法");
	}
	return r;
}

Result probe(const QString &root)
{
	Result r;
	if (root.trimmed().isEmpty())
	{
		r.error = QStringLiteral("没有工程目录，跳过识别");
		return r;
	}

	const QFileInfo fi(root);
	if (fi.isFile())
	{
		return probeFiles(QStringList() << fi.absoluteFilePath());
	}
	if (!fi.isDir())
	{
		r.error = QStringLiteral("目录不存在：%1").arg(root);
		return r;
	}

	QStringList files;
	int budget = kMaxFiles;
	walk(fi.absoluteFilePath(), &files, &budget, 0);

	if (files.isEmpty())
	{
		r.error = QStringLiteral("这个目录下没有 .c/.h 源码");
		return r;
	}
	return probeFiles(files);
}

CodeAdapter adapterOf(const Result &r)
{
	CodeAdapter a;
	a.detected = r.any();
	a.stdLib   = (r.lib == StandardPeriph);

	if (r.write == TwoFuncs)
	{
		a.setWrite(QStringLiteral("%1(c)").arg(r.cmdFunc),
		           QStringLiteral("%1(d)").arg(r.dataFunc));
	}
	else if (r.write == FlagByte)
	{
		a.setWrite(QStringLiteral("%1((c), 1)").arg(r.byteFunc),
		           QStringLiteral("%1((d), 0)").arg(r.byteFunc));
	}

	if (!r.showAsciiFunc.isEmpty())
	{
		a.setShowAscii(QStringLiteral("%1((line), (col), (ch))").arg(r.showAsciiFunc));
	}
	if (!r.delayCall.isEmpty())
	{
		a.setDelay(r.delayCall);
	}

	QStringList bits;
	if (r.lib != Unknown)
	{
		bits << nameOf(r.lib);
	}
	if (!r.delayCall.isEmpty())
	{
		bits << QStringLiteral("延时 %1").arg(r.delayCall);
	}
	if (r.write == TwoFuncs)
	{
		bits << QStringLiteral("写命令 %1 / %2").arg(r.cmdFunc, r.dataFunc);
	}
	else if (r.write == FlagByte)
	{
		bits << QStringLiteral("写命令 %1(带 cmd 标志位)").arg(r.byteFunc);
	}
	if (!r.showAsciiFunc.isEmpty())
	{
		bits << QStringLiteral("字符 %1").arg(r.showAsciiFunc);
	}
	a.origin = bits.join(QStringLiteral(" · "));
	return a;
}

QString adapterNote(const CodeAdapter &a)
{
	/* 没探测到就一个字都不加：导出的文件必须和改造前逐字节一致。
	 * 有金样测试卡着这一条，也是老用户不该被这次改动影响的意思。 */
	if (!a.detected || a.origin.isEmpty())
	{
		return QString();
	}

	QString s = QStringLiteral("/*  适配区来源：按你的工程源码识别到 —— %1\n").arg(a.origin);

	/* 探到什么就说什么。只探到延时却让整块看着像"全都认对了"，
	 * 用户就不会去核那几行没认出来的 —— 而没认出来的恰恰是默认值。 */
	QStringList miss;
	if (!a.hasWrite)
	{
		miss << QStringLiteral("写命令 / 写数据");
	}
	if (!a.hasAscii)
	{
		miss << QStringLiteral("字符显示");
	}
	if (!a.hasDelay)
	{
		miss << QStringLiteral("帧间延时");
	}
	if (!miss.isEmpty())
	{
		s += QStringLiteral(" *  没认出来的：%1 —— 下面那几行还是默认值，照上面的 (A)/(B) 核一下。\n")
		         .arg(miss.join(QStringLiteral("、")));
	}

	/* 这条必须单独说：标准库工程里根本不存在 HAL_Delay，
	 * 留着默认值就是"导出成功、编译报错"，而且报错指不到工具头上。 */
	if (a.stdLib && !a.hasDelay)
	{
		s += QStringLiteral(" *  ⚠ 你的工程是标准库，可下面的延时仍是默认的 HAL_Delay ——");
		s += QStringLiteral("标准库里没有这个函数，必须改成你自己的（例如 Delay_ms）。\n");
	}

	s += QStringLiteral(" *  认错了就直接改下面几行；下次导出会重新认一遍，不会记住错的那次。 */\n");
	return s;
}

}   // namespace LibProbe
