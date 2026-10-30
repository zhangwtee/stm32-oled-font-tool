#include "screenprobe.h"

#include "mergeproject.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

namespace ScreenProbe
{

QString nameOf(Controller c)
{
	switch (c)
	{
	case SH1106:  return QStringLiteral("SH1106");
	case SSD1306: return QStringLiteral("SSD1306");
	default:      return QStringLiteral("未识别");
	}
}

int offsetOf(Controller c)
{
	return (c == SH1106) ? 2 : 0;
}

Controller fromOffset(int offset)
{
	return (offset >= 2) ? SH1106 : SSD1306;
}

namespace
{

/* 把注释换成空格，长度和换行位置都不变 —— 这样行号还能对上原文件。
 * 顺带跳过双引号字符串里的内容，免得 "http://..." 这种被当成行注释。 */
QString stripComments(const QString &src)
{
	QString out = src;
	const int n = out.size();
	bool inBlock = false;
	bool inLine = false;
	bool inStr = false;

	for (int i = 0; i < n; ++i)
	{
		const QChar c = out.at(i);
		const QChar next = (i + 1 < n) ? out.at(i + 1) : QChar();

		if (c == QLatin1Char('\n'))
		{
			inLine = false;                 /* 行注释到行尾自动结束 */
			continue;
		}
		if (inLine)
		{
			out[i] = QLatin1Char(' ');
			continue;
		}
		if (inBlock)
		{
			if (c == QLatin1Char('*') && next == QLatin1Char('/'))
			{
				out[i] = QLatin1Char(' ');
				out[i + 1] = QLatin1Char(' ');
				++i;
				inBlock = false;
			}
			else
			{
				out[i] = QLatin1Char(' ');
			}
			continue;
		}
		if (inStr)
		{
			if (c == QLatin1Char('\\'))
			{
				++i;                        /* 跳过转义的下一个字符 */
			}
			else if (c == QLatin1Char('"'))
			{
				inStr = false;
			}
			continue;
		}

		if (c == QLatin1Char('/') && next == QLatin1Char('/'))
		{
			out[i] = QLatin1Char(' ');
			out[i + 1] = QLatin1Char(' ');
			++i;
			inLine = true;
		}
		else if (c == QLatin1Char('/') && next == QLatin1Char('*'))
		{
			out[i] = QLatin1Char(' ');
			out[i + 1] = QLatin1Char(' ');
			++i;
			inBlock = true;
		}
		else if (c == QLatin1Char('"'))
		{
			inStr = true;
		}
	}
	return out;
}

QStringList splitLines(const QString &s)
{
	return s.split(QLatin1Char('\n'));
}

QString snippet(const QStringList &lines, int line1based)
{
	if (line1based < 1 || line1based > lines.size())
	{
		return QString();
	}
	QString t = lines.at(line1based - 1).trimmed();
	if (t.size() > 64)
	{
		t = t.left(61) + QStringLiteral("...");
	}
	return t;
}

/* 这些目录里不可能有用户自己的 OLED 驱动，扫了只是浪费时间 */
bool skipDir(const QString &name)
{
	static const char *const bad[] = {
		"build", "Build", "Objects", "Listings", "DebugConfig",
		"RTE", "CMSIS", "Drivers", ".git", ".vscode", ".workbuddy",
		"package", "install", "node_modules"
	};
	for (const char *b : bad)
	{
		if (name.compare(QLatin1String(b), Qt::CaseInsensitive) == 0)
		{
			return true;
		}
	}
	return false;
}

void collectSources(const QString &dir, int depth, QStringList *out, int *budget)
{
	if (depth > 5 || *budget <= 0)
	{
		return;
	}
	QDir d(dir);
	const QFileInfoList entries = d.entryInfoList(
		QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);

	for (const QFileInfo &fi : entries)
	{
		if (--(*budget) <= 0)
		{
			return;
		}
		if (fi.isDir())
		{
			if (!skipDir(fi.fileName()))
			{
				collectSources(fi.absoluteFilePath(), depth + 1, out, budget);
			}
			continue;
		}
		const QString suffix = fi.suffix().toLower();
		if ((suffix == QLatin1String("c") || suffix == QLatin1String("h")
		     || suffix == QLatin1String("cpp") || suffix == QLatin1String("hpp"))
		    && fi.size() > 0 && fi.size() < 512 * 1024)
		{
			out->append(fi.absoluteFilePath());
		}
	}
}

struct Tally
{
	int     sh1106  = 0;
	int     ssd1306 = 0;
	QString file;
	int     line = 0;
	QStringList evidence;
};

void addEvidence(Tally *t, const QString &file, int line, const QString &msg)
{
	t->evidence.append(QStringLiteral("%1:%2  %3")
	                       .arg(QFileInfo(file).fileName())
	                       .arg(line)
	                       .arg(msg));
	if (t->line == 0)
	{
		t->file = file;
		t->line = line;
	}
}

/* 扫一份文件，把分数累加进 tally。只原样加分，不做结论。 */
void scoreFile(const QString &path, Tally *t)
{
	const QString raw = MergeProject::readAsText(path);
	if (raw.isEmpty())
	{
		return;
	}

	const QString code = stripComments(raw);      /* 判代码用 */
	const QStringList codeLines = splitLines(code);
	const QStringList rawLines = splitLines(raw);

	/* ---- 1. 型号字样：注释里写型号是最常见的做法，所以查原文 ---- */
	for (int i = 0; i < rawLines.size(); ++i)
	{
		const QString &ln = rawLines.at(i);
		if (ln.contains(QLatin1String("SH1106"), Qt::CaseInsensitive))
		{
			t->sh1106 += 100;
			addEvidence(t, path, i + 1,
			            QStringLiteral("源码里写明了 SH1106：%1").arg(snippet(rawLines, i + 1)));
			return;                            /* 写明了就不用再猜 */
		}
		if (ln.contains(QLatin1String("SSD1306"), Qt::CaseInsensitive))
		{
			t->ssd1306 += 100;
			addEvidence(t, path, i + 1,
			            QStringLiteral("源码里写明了 SSD1306：%1").arg(snippet(rawLines, i + 1)));
			return;
		}
	}

	/* ---- 2. 设列地址的地方多加了 2：SH1106 最典型的写法 ---- */
	/* 正点原子、中景园旧版用小写 0xb0，所以这里不能区分大小写 */
	static const QRegularExpression rePageCmd(QStringLiteral("0[xX]B[01]"),
	                                          QRegularExpression::CaseInsensitiveOption);
	static const QRegularExpression rePlusTwo(QStringLiteral("\\+\\s*2\\b"));
	static const QRegularExpression reCmd02(QStringLiteral("0[xX]0?2\\b"),
	                                        QRegularExpression::CaseInsensitiveOption);
	static const QRegularExpression reCol83(QStringLiteral("0[xX]83\\b"),
	                                        QRegularExpression::CaseInsensitiveOption);
	static const QRegularExpression reAddrMode(QStringLiteral("0[xX]2[12]\\b"),
	                                           QRegularExpression::CaseInsensitiveOption);
	/* 列地址的高低 4 位命令：0xF0 / 0x0F / >>4。出现它说明这里就是设光标的地方 */
	static const QRegularExpression reColNibble(
		QStringLiteral("0[xX][fF]0\\b|0[xX]0[fF]\\b|>>\\s*4"),
		QRegularExpression::CaseInsensitiveOption);

	bool hitPlus2 = false;
	bool hitCmd02 = false;
	bool hitColNibble = false;

	for (int i = 0; i < codeLines.size(); ++i)
	{
		if (!rePageCmd.match(codeLines.at(i)).hasMatch())
		{
			continue;
		}
		/* 页地址命令附近就是设光标的地方，往前后各看 10 行 */
		const int from = qMax(0, i - 10);
		const int to = qMin(codeLines.size() - 1, i + 10);
		for (int k = from; k <= to; ++k)
		{
			const QString &w = codeLines.at(k);
			if (rePlusTwo.match(w).hasMatch())
			{
				hitPlus2 = true;
				addEvidence(t, path, k + 1,
				            QStringLiteral("设列地址时多加了 2：%1").arg(snippet(codeLines, k + 1)));
			}
			else if (reColNibble.match(w).hasMatch())
			{
				hitColNibble = true;
			}
			else if (reCmd02.match(w).hasMatch())
			{
				hitCmd02 = true;
			}
		}
		if (hitPlus2)
		{
			break;
		}
	}
	if (hitPlus2)
	{
		t->sh1106 += 30;
	}
	else if (hitColNibble)
	{
		/* 设光标的地方直接用列号算高低 4 位、没有加偏移 —— 这是 SSD1306 的写法。
		 * 这条比"没找到任何线索"有用得多：中景园/正点原子的 SSD1306 驱动
		 * 里不会有型号字样，但一定有这段。 */
		t->ssd1306 += 15;
		addEvidence(t, path, 0,
		            QStringLiteral("设列地址时直接用列号，没有加偏移（SSD1306 写法）"));
	}
	else if (hitCmd02)
	{
		t->sh1106 += 10;
		addEvidence(t, path, 0, QStringLiteral("设光标处出现 0x02 起始列"));
	}

	/* ---- 3. 132 列的末列地址 0x83 / 常量 132 ---- */
	for (int i = 0; i < codeLines.size(); ++i)
	{
		if (reCol83.match(codeLines.at(i)).hasMatch())
		{
			t->sh1106 += 20;
			addEvidence(t, path, i + 1,
			            QStringLiteral("出现 0x83（132 列末列）：%1").arg(snippet(codeLines, i + 1)));
			break;
		}
	}

	/* ---- 4. SSD1306 才有的寻址设置命令 0x20/0x21/0x22 ---- */
	int addrModeHits = 0;
	int lastAddrLine = 0;
	for (int i = 0; i < codeLines.size(); ++i)
	{
		if (reAddrMode.match(codeLines.at(i)).hasMatch())
		{
			++addrModeHits;
			lastAddrLine = i + 1;
		}
	}
	if (addrModeHits > 0)
	{
		t->ssd1306 += 20;
		addEvidence(t, path, lastAddrLine,
		            QStringLiteral("使用 0x21/0x22 寻址命令（SSD1306 特性）"));
	}
}

}   // namespace

Result probeFiles(const QStringList &files)
{
	Result r;
	Tally t;

	for (const QString &f : files)
	{
		scoreFile(f, &t);
		/* 型号字样一旦写明就是定案，后面的不用再扫 */
		if (t.sh1106 >= 100 || t.ssd1306 >= 100)
		{
			break;
		}
	}

	r.evidence = t.evidence;
	r.file = t.file;
	r.line = t.line;

	if (t.sh1106 >= 100 && t.sh1106 >= t.ssd1306)
	{
		r.controller = SH1106;
	}
	else if (t.ssd1306 >= 100 && t.ssd1306 > t.sh1106)
	{
		r.controller = SSD1306;
	}
	else if (t.sh1106 > t.ssd1306)
	{
		r.controller = SH1106;
	}
	else if (t.ssd1306 > t.sh1106)
	{
		r.controller = SSD1306;
	}
	else
	{
		r.controller = Unknown;
	}

	r.colOffset = offsetOf(r.controller);

	if (r.controller == Unknown)
	{
		r.evidenceOneLine = QStringLiteral(
			"没在工程里找到能判断控制器的线索，先按最常见的 SSD1306（偏移 0）处理");
	}
	else
	{
		r.evidenceOneLine = QStringLiteral("认出来了：%1，列偏移 %2")
		                        .arg(nameOf(r.controller))
		                        .arg(r.colOffset);
	}
	return r;
}

Result probe(const QString &root)
{
	Result r;
	const QFileInfo fi(root);

	if (!fi.exists())
	{
		r.error = QStringLiteral("路径不存在：%1").arg(root);
		r.evidenceOneLine = r.error;
		return r;
	}

	QStringList files;
	if (fi.isFile())
	{
		files << fi.absoluteFilePath();
	}
	else
	{
		int budget = 4000;              /* 最多看这么多个文件，防止在大目录上卡住 */
		collectSources(fi.absoluteFilePath(), 0, &files, &budget);
	}

	if (files.isEmpty())
	{
		r.evidenceOneLine = QStringLiteral("这个目录下没找到 .c/.h 源码，判断不了屏型号");
		return r;
	}

	/* 名字里有 oled / lcd / disp 的排前面，先看最可能是驱动的 */
	QStringList head, tail;
	for (const QString &f : files)
	{
		const QString n = QFileInfo(f).fileName().toLower();
		if (n.contains(QLatin1String("oled")) || n.contains(QLatin1String("lcd"))
		    || n.contains(QLatin1String("disp")) || n.contains(QLatin1String("sh1106"))
		    || n.contains(QLatin1String("ssd1306")))
		{
			head << f;
		}
		else
		{
			tail << f;
		}
	}

	r = probeFiles(head + tail);
	return r;
}

}   // namespace ScreenProbe
