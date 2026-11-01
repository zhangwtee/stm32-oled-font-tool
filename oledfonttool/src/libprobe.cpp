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
#include <QVector>

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

/* 一个候选函数。同一个名字可能在 .h 里声明、在 .c 里定义，记两条 ——
 * 挑的时候要看它跟字符显示函数是不是同一套驱动，只留一条就把判据丢了。 */
struct Cand
{
	QString name;
	QString file;       /* 文件名，给用户看 */
	QString path;       /* 绝对路径，判断是不是同一个文件 */
	int     line = 0;
};

/* 写命令 / 写数据 / 字符显示都可能不止一套（工程里同时挂 LCD 和 OLED） */
struct Bag
{
	QVector<Cand> cmd;      /* XxxWriteCommand(c) */
	QVector<Cand> data;     /* XxxWriteData(d) */
	QVector<Cand> byte;     /* XxxWR_Byte(d, cmd) */
	QVector<Cand> show;     /* XxxShowChar(line, col, ch) */
};

void addCand(QVector<Cand> *v, const QString &name, const QString &file,
             const QString &path, int line)
{
	for (const Cand &c : *v)
	{
		if (c.name == name && c.path == path)
		{
			return;         /* 同文件同名只记一条 */
		}
	}
	Cand c;
	c.name = name;
	c.file = file;
	c.path = path;
	c.line = line;
	v->append(c);
}

/* 两个名字的公共前缀长度（不分大小写）。
 * OLED_WriteCommand 和 OLED_ShowChar 共 5 个字符，跟 LCD_WriteCommand 一个都不共。 */
int commonPrefixLen(const QString &a, const QString &b)
{
	int n = 0;
	while (n < a.size() && n < b.size()
	       && a.at(n).toLower() == b.at(n).toLower())
	{
		++n;
	}
	return n;
}

/*
 * 「这套函数是不是给 OLED 用的」。
 *
 * 课设工程里同时挂 LCD 和 OLED 两套驱动不算少见，两套都有 XxxWriteCommand
 * 和 XxxShowChar。光按目录名顺序取第一个，LCD.c 排在 OLED.c 前面，就把
 * LCD_WriteCommand 填进 OLED 字库了 —— 编过、能跑、屏幕不亮，最难查的一种。
 * 这个工具是给 OLED 用的，认驱动时优先名字里带 OLED 的那套，其次带控制器
 * 型号的；都没有就按「跟字符显示函数是不是同一套」来定。
 */
int driverBias(const QString &name)
{
	const QString l = name.toLower();
	if (l.contains(QLatin1String("oled")))
	{
		return 100;
	}
	if (l.contains(QLatin1String("ssd1306")) || l.contains(QLatin1String("sh1106")))
	{
		return 90;
	}
	return 0;
}

/* 挑最像的一组里最像的那一条：同文件加权最高，其次是名字公共前缀。
 * 全都一样时按扫描顺序取第一个（NaN 情况下的老行为不变）。 */
const Cand *pickBest(const QVector<Cand> &v, const Cand *anchor)
{
	if (v.isEmpty())
	{
		return nullptr;
	}
	const Cand *best = &v.at(0);
	int bestScore = -1;
	for (const Cand &c : v)
	{
		int s = driverBias(c.name);
		if (anchor)
		{
			if (c.path == anchor->path)
			{
				s += 50;
			}
			s += commonPrefixLen(c.name, anchor->name);
		}
		if (s > bestScore)
		{
			bestScore = s;
			best = &c;
		}
	}
	return best;
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

QStringList projectMarkers()
{
	/* 这些文件是「这里是一个工程」的硬定义，跟源码怎么起名无关。
	 * 判据不能是"有没有 OLED.c"—— 别人叫 lcd.c / ssd1306.c 一样是驱动。 */
	return QStringList {
		QStringLiteral("*.uvprojx"),       /* Keil MDK 5 */
		QStringLiteral("*.uvproj"),        /* Keil MDK 4 */
		QStringLiteral("*.ewp"),           /* IAR EWARM */
		QStringLiteral(".cproject"),       /* STM32CubeIDE / Eclipse CDT */
		QStringLiteral(".mxproject"),      /* STM32CubeMX 生成 */
		QStringLiteral("platformio.ini")   /* PlatformIO */
	};
}

namespace
{

bool hasProjectMarker(const QDir &d)
{
	if (!d.exists())
	{
		return false;
	}
	return !d.entryList(projectMarkers(), QDir::Files | QDir::Hidden).isEmpty();
}

bool hasSourceFiles(const QDir &d)
{
	static const QStringList pat = {
		QStringLiteral("*.c"), QStringLiteral("*.h"),
		QStringLiteral("*.cpp"), QStringLiteral("*.hpp")
	};
	return d.exists() && !d.entryList(pat, QDir::Files).isEmpty();
}

}   // namespace

QString projectRootOf(const QString &anyPath)
{
	if (anyPath.trimmed().isEmpty())
	{
		return QString();
	}

	const QFileInfo fi(anyPath);
	QDir d = fi.isDir() ? QDir(anyPath) : QDir(fi.absolutePath());
	if (!d.exists())
	{
		return QString();
	}

	/* 1) 往上找最近的、带工程文件的目录。
	 *    Hardware\OLED_CFont.h  → 工程根
	 *    OLED_CFont.h（在根）    → 工程根自己，而不是装着所有课的上一层 */
	QDir cur = d;
	for (int up = 0; up < 3; ++up)
	{
		if (hasProjectMarker(cur))
		{
			return cur.absolutePath();
		}
		if (!cur.cdUp())
		{
			break;
		}
	}

	/* 2) 没有工程文件：这一层直接放着源码，就算工程的一部分，并到
	 *    上一层不再直接放源码为止。有人把驱动和 main.c 整个拷成一个
	 *    文件夹，没有 .uvprojx，也得能用。 */
	if (hasSourceFiles(d))
	{
		QDir top = d;
		for (int up = 0; up < 2; ++up)
		{
			QDir parent = top;
			if (!parent.cdUp() || !hasSourceFiles(parent))
			{
				break;
			}
			top = parent;
		}
		return top.absolutePath();
	}

	/* 3) 什么都不像 —— 返回空。宁可这次不认，也不能认到隔壁工程头上：
	 *    写错了不报错、只是编不过或者屏幕不亮，没人会想到是取模工具干的。 */
	return QString();
}

namespace
{

/* 一次扫描把所有要认的东西都收齐。认内容不认文件名 */
void scanOne(const QString &path, Result *r, int *stdScore, int *halScore,
             Hit *delayDef, Hit *delayMs, Hit *delayHal, Hit *delayRtos,
             Bag *bag, QStringList *evidence)
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
		addCand(&bag->cmd, m.captured(1), name, path, lineOf(text, m.capturedStart()));
	}
	m = reData.match(text);
	if (m.hasMatch())
	{
		addCand(&bag->data, m.captured(1), name, path, lineOf(text, m.capturedStart()));
	}
	m = reByte.match(text);
	if (m.hasMatch())
	{
		addCand(&bag->byte, m.captured(1), name, path, lineOf(text, m.capturedStart()));
	}

	/* ---- 字符显示函数 ---- */
	static const QRegularExpression reShowChar(
		QStringLiteral("\\b([A-Za-z_][A-Za-z0-9_]*ShowChar)[ \\t]*\\("));
	m = reShowChar.match(text);
	if (m.hasMatch())
	{
		addCand(&bag->show, m.captured(1), name, path, lineOf(text, m.capturedStart()));
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
	Bag bag;
	QStringList evidence;

	for (const QString &f : files)
	{
		scanOne(f, &r, &stdScore, &halScore,
		        &delayDef, &delayMs, &delayHal, &delayRtos,
		        &bag, &evidence);
	}

	if (stdScore > halScore)
	{
		r.lib = StandardPeriph;
	}
	else if (halScore > stdScore)
	{
		r.lib = HAL;
	}

	/* 先挑字符显示函数：它是这套驱动的主锚点，写命令 / 写数据都按
	 * 「跟它是不是同一套」来选，这样 LCD 和 OLED 同时存在时不会拼出
	 * LCD_WriteCommand + OLED_ShowChar 这种根本不存在的组合。 */
	const Cand *show = pickBest(bag.show, nullptr);
	if (show)
	{
		r.showAsciiFunc = show->name;
		r.file = show->file;
		r.line = show->line;
	}

	/* 写命令写法：两个函数优先。中景园新版 / 多数 HAL 例程是这样，
	 * 也是头文件注释里的 (A)，和改造前的默认值一致 */
	if (!bag.cmd.isEmpty() && !bag.data.isEmpty())
	{
		const Cand *c = pickBest(bag.cmd, show);
		const Cand *d = pickBest(bag.data, c);
		if (c && d)
		{
			r.write    = TwoFuncs;
			r.cmdFunc  = c->name;
			r.dataFunc = d->name;
			r.file     = c->file;
			r.line     = c->line;
			evidence.prepend(QStringLiteral("%1：写命令函数 %2")
			                 .arg(c->file, c->name));
		}
	}
	else if (!bag.byte.isEmpty())
	{
		const Cand *b = pickBest(bag.byte, show);
		if (b)
		{
			r.write    = FlagByte;
			r.byteFunc = b->name;
			r.file     = b->file;
			r.line     = b->line;
			evidence.prepend(QStringLiteral("%1：写字节函数 %2（带 cmd 标志位）")
			                 .arg(b->file, b->name));
		}
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
