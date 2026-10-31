#include "mergeproject.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringList>

#include <algorithm>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace MergeProject {

/* ==========================================================================
 *  一、文件读写：只管字节，不管编码
 *
 *  用户的工程文件什么编码都有：Keil 老工程是 GBK/GB2312，STM32CubeMX 生成的
 *  是 UTF-8，还有人两种混着。只要用 QString 读进来再写回去，没识别对的那些
 *  文件里的中文就会永久变成乱码 —— 而且改完才看得见。所以这一层完全不碰编码，
 *  全程 QByteArray，插入的文本按目标文件的编码转成字节再拼。
 * ========================================================================== */

enum Enc { EncUtf8, EncGbk };

static bool isValidUtf8(const QByteArray &b)
{
	/* 严格校验：多字节序列的首字节和续字节的高位必须对得上，
	 * 而且要排除过长编码和代理区。GBK 的中文是两个 0x81~0xFE 的字节，
	 * 第二个字节一般 < 0x80，几乎不可能碰巧构成合法的三字节 UTF-8。 */
	const int n = b.size();
	int i = 0;
	while (i < n)
	{
		const unsigned char c = static_cast<unsigned char>(b.at(i));
		if (c < 0x80)
		{
			++i;
			continue;
		}

		int need = 0;
		if ((c & 0xE0) == 0xC0)
		{
			need = 1;
			if (c < 0xC2)                       /* 过长编码 */
			{
				return false;
			}
		}
		else if ((c & 0xF0) == 0xE0)
		{
			need = 2;
		}
		else if ((c & 0xF8) == 0xF0)
		{
			need = 3;
		}
		else
		{
			return false;
		}

		if (i + need >= n)
		{
			return false;
		}
		for (int k = 1; k <= need; ++k)
		{
			const unsigned char cc = static_cast<unsigned char>(b.at(i + k));
			if ((cc & 0xC0) != 0x80)
			{
				return false;
			}
		}
		i += need + 1;
	}
	return true;
}

static Enc detectEnc(const QByteArray &raw)
{
	return isValidUtf8(raw) ? EncUtf8 : EncGbk;
}

/* 有没有非 ASCII 字节 —— 用来判断"要不要为这个文件补 BOM" */
static bool hasNonAsciiByte(const QByteArray &b)
{
	for (const char c : b)
	{
		if (static_cast<unsigned char>(c) > 127)
		{
			return true;
		}
	}
	return false;
}

SrcIssue judgeSource(const QByteArray &raw, bool hasNonAsciiLiteral)
{
	/* 没有中文字符串的文件不用管：注释里的中文不参与查表，armcc 也不在意
	 * （注释里的字节被吃掉不会改变任何东西，最多换个行的位置） */
	if (!hasNonAsciiLiteral)
	{
		return SrcIssue::None;
	}

	if (raw.startsWith("\xEF\xBB\xBF"))
	{
		return SrcIssue::None;      /* 有 BOM：armcc 按 UTF-8 读，查表也对得上 */
	}

	/* 不是合法 UTF-8，那就是 GBK/ANSI：armcc 读得懂（它本来就按 GBK 读），
	 * 编译能过，但字符串字节和字库的键（UTF-8）对不上，屏幕全空 */
	if (!isValidUtf8(raw))
	{
		return SrcIssue::NotUtf8;
	}

	/* 合法 UTF-8 却没 BOM：查表对得上，armcc 这一关过不去 */
	return SrcIssue::NoBom;
}

/* QString -> 目标编码字节。GBK 走 Win32 代码页 936，不额外依赖 Qt 模块 */
static QByteArray encodeTo(const QString &s, Enc e)
{
	if (e == EncUtf8 || s.isEmpty())
	{
		return s.toUtf8();
	}
#ifdef Q_OS_WIN
	{
		const int n = ::WideCharToMultiByte(936, 0,
		                                    reinterpret_cast<LPCWCH>(s.utf16()),
		                                    s.size(), nullptr, 0, nullptr, nullptr);
		if (n <= 0)
		{
			return s.toUtf8();              /* 转不了就退回 UTF-8，至少不是空 */
		}
		QByteArray out(n, '\0');
		::WideCharToMultiByte(936, 0, reinterpret_cast<LPCWCH>(s.utf16()), s.size(),
		                      out.data(), n, nullptr, nullptr);
		return out;
	}
#else
	return s.toUtf8();
#endif
}

/* QByteArray(目标编码) -> QString */
static QString decodeFrom(const QByteArray &b, Enc e)
{
	QString s;
	if (e == EncUtf8)
	{
		s = QString::fromUtf8(b);
	}
	else
	{
#ifdef Q_OS_WIN
		const int n = ::MultiByteToWideChar(936, 0, b.constData(), b.size(), nullptr, 0);
		if (n <= 0)
		{
			s = QString::fromUtf8(b);
		}
		else
		{
			QVector<ushort> buf(n);
			::MultiByteToWideChar(936, 0, b.constData(), b.size(),
			                      reinterpret_cast<LPWSTR>(buf.data()), n);
			s = QString::fromUtf16(reinterpret_cast<const char16_t *>(buf.constData()), n);
		}
#else
		s = QString::fromUtf8(b);
#endif
	}

	/* UTF-8 BOM 会被 fromUtf8 原样保留成首字符 U+FEFF。
	 * 它挡在行首，`^[ \t]*#` 这类正则就认不出第 1 行的 #include（BOM 既不是
	 * 空格也不是 Tab），插入位置于是算成 0 —— BOM 反而被新内容挤到第二行，
	 * 文件不再以 BOM 开头，下次探测编码会把中文误判成 GBK。
	 * 这里统一摘掉，保存时由 apply 把它补回文件最前面。 */
	if (!s.isEmpty() && s.at(0) == QChar(0xFEFF))
	{
		s.remove(0, 1);
	}
	return s;
}

/*
 * 切成行（保留原行尾风格）。返回的每行都不含 \r\n。
 *
 * 工程里常见 CRLF 和 LF 混用：本题目标工程就是 OLED.h 全 CRLF、
 * main.c 全 LF（还带 BOM）。统一成一种会把整个文件的行尾全改掉，
 * git diff 会变成"整个文件都改了"，没法审阅。所以按文件自己的主流风格来。
 */
static QList<QByteArray> splitLines(const QByteArray &raw, QByteArray *eol)
{
	const int crlf = raw.count("\r\n");
	const int lf   = raw.count('\n') - crlf;
	*eol = (crlf >= lf && crlf > 0) ? QByteArray("\r\n") : QByteArray("\n");

	QList<QByteArray> lines;
	int start = 0;
	while (start <= raw.size())
	{
		const int p = raw.indexOf('\n', start);
		if (p < 0)
		{
			if (start < raw.size())
			{
				lines << raw.mid(start);
			}
			break;
		}
		int end = p;
		if (end > start && raw.at(end - 1) == '\r')
		{
			--end;
		}
		lines << raw.mid(start, end - start);
		start = p + 1;
	}
	return lines;
}

static QByteArray joinLines(const QList<QByteArray> &lines, const QByteArray &eol)
{
	QByteArray out;
	out.reserve(lines.size() * 40);
	for (const QByteArray &l : lines)
	{
		out += l;
		out += eol;
	}
	return out;
}

/* ==========================================================================
 *  二、找东西：靠内容，不靠文件名
 *
 *  「驱动叫 OLED.c」是最不可靠的假设。用户可能是 lcd.c、display.c、
 *  oled_drv.c，也可能整个驱动塞在 main.c 里。所以一律读文件内容判断。
 * ========================================================================== */

static const QStringList &skipDirs()
{
	static const QStringList k = {
		QStringLiteral("build"),       QStringLiteral("package"),
		QStringLiteral("Output"),      QStringLiteral("Objects"),
		QStringLiteral("Listings"),    QStringLiteral("DebugConfig"),
		QStringLiteral(".git"),        QStringLiteral(".vs"),
		QStringLiteral(".vscode"),     QStringLiteral("node_modules"),
		QStringLiteral("__pycache__"), QStringLiteral("tmp"),
		QStringLiteral("staging"),     QStringLiteral("RTE"),
		QStringLiteral("Drivers/CMSIS"), QStringLiteral("Middlewares")
	};
	return k;
}

/* 递归收集 .c，深度和数量都有上限，避免扫到巨大的 HAL 库时卡住 */
static void collectC(const QDir &dir, QStringList &out, int maxDepth, int maxFiles)
{
	if (maxDepth < 0 || out.size() >= maxFiles)
	{
		return;
	}
	const QFileInfoList files =
		dir.entryInfoList(QStringList() << QStringLiteral("*.c"), QDir::Files, QDir::Name);
	for (const QFileInfo &fi : files)
	{
		if (out.size() >= maxFiles)
		{
			return;
		}
		out << fi.absoluteFilePath();
	}
	const QFileInfoList subs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
	for (const QFileInfo &fi : subs)
	{
		if (skipDirs().contains(fi.fileName(), Qt::CaseInsensitive))
		{
			continue;
		}
		collectC(QDir(fi.absoluteFilePath()), out, maxDepth - 1, maxFiles);
		if (out.size() >= maxFiles)
		{
			return;
		}
	}
}

/* 函数定义：行首（可有空白和 static），返回 名字 和 参数表。
 * 声明（行尾有分号）不算。static 的也不算 —— 声明了外部也调不到。 */
struct FuncDef
{
	QString name;
	QString params;
	bool    isStatic = false;
	bool    inlineBody = false;     /* 左花括号写在同一行 */
};

static bool parseFuncDef(const QString &line, FuncDef *out)
{
	/*
	 * 末尾的 (\{)? 是为了认同行花括号：`void OLED_WR_CMD(u8 cmd) {`。
	 * 以前要求签名后必须只有空白、花括号另起一行，于是这种很常见的写法
	 * 一个都匹配不到，整个"找到驱动"的流程直接失败，报"没找到 OLED 驱动"。
	 * 第 5 组捕获到 { 就说明函数体在同一行，不必再去看下一行。
	 */
	static const QRegularExpression re(
		QStringLiteral("^[ \\t]*(?:(static)\\s+)?(?:void|u8|uint8_t|unsigned\\s+char)\\s+"
		               "([A-Za-z_]\\w*)\\s*\\(([^)]*)\\)\\s*(\\{)?\\s*$"));
	const QRegularExpressionMatch m = re.match(line);
	if (!m.hasMatch())
	{
		return false;
	}
	out->isStatic   = m.captured(1).size() > 0;
	out->name       = m.captured(2);
	out->params     = m.captured(3).trimmed();
	out->inlineBody = m.captured(4).size() > 0;
	return true;
}

/* 参数个数（void 算 0 个） */
static int paramCount(const QString &params)
{
	if (params.isEmpty() || params.compare(QLatin1String("void"), Qt::CaseInsensitive) == 0)
	{
		return 0;
	}
	int n = 1;
	for (const QChar c : params)
	{
		if (c == QLatin1Char(','))
		{
			++n;
		}
	}
	return n;
}

struct Driver
{
	QString     cFile;
	QString     hFile;
	QString     cmdFunc;
	QString     dataFunc;
	QStringList initFuncs;      /* 候选，短的排前面 */
};

/* 在 .c 里找「写命令」「写数据」「初始化」三个函数 */
static bool sniffDriver(const QString &cPath, Driver *out)
{
	QFile f(cPath);
	if (!f.open(QIODevice::ReadOnly))
	{
		return false;
	}
	const QByteArray raw = f.readAll();
	f.close();

	const Enc enc = detectEnc(raw);
	QByteArray eol;
	const QList<QByteArray> lines = splitLines(raw, &eol);

	QString cmd, data;
	QStringList inits;          /* 可能不止一个：OLED_Init / OLED_I2C_Init / OLED_SPI_Init */
	bool    foundDriver = false;

	for (int i = 0; i < lines.size(); ++i)
	{
		const QString line = decodeFrom(lines.at(i), enc);
		FuncDef fd;
		if (!parseFuncDef(line, &fd) || fd.isStatic)
		{
			continue;
		}
		/* 函数体开始：要么左花括号已经写在同一行（parseFuncDef 捕获到了），
		 * 要么下一行以 { 开头。两种写法在实际驱动里都很常见。 */
		bool hasBody = fd.inlineBody;
		if (!hasBody && i + 1 < lines.size())
		{
			hasBody = decodeFrom(lines.at(i + 1), enc).trimmed().startsWith(QLatin1Char('{'));
		}
		if (!hasBody)
		{
			continue;
		}

		const QString low = fd.name.toLower();
		const int     np  = paramCount(fd.params);

		if (cmd.isEmpty() && np == 1
		    && (low.contains(QLatin1String("writecommand"))
		        || low.contains(QLatin1String("writecmd"))
		        || low.contains(QLatin1String("write_command"))))
		{
			cmd = fd.name;
			foundDriver = true;
		}
		else if (data.isEmpty() && np == 1
		         && (low.contains(QLatin1String("writedata"))
		             || low.contains(QLatin1String("write_data"))))
		{
			data = fd.name;
			foundDriver = true;
		}
		else if (np == 0 && low.endsWith(QLatin1String("_init"))
		         && (low.contains(QLatin1String("oled")) || low.contains(QLatin1String("lcd"))))
		{
			inits << fd.name;
			foundDriver = true;
		}
	}

	if (!foundDriver)
	{
		return false;
	}

	/* 名字短的排前面：同一份驱动里 OLED_Init 和 OLED_I2C_Init 都在，
	 * main() 里调的一定是前者。按长度排比"谁先出现"可靠得多。 */
	std::sort(inits.begin(), inits.end(), [](const QString &a, const QString &b) {
		if (a.size() != b.size())
		{
			return a.size() < b.size();
		}
		return a < b;
	});
	inits.removeDuplicates();

	out->cFile     = QFileInfo(cPath).absoluteFilePath();
	out->cmdFunc   = cmd;
	out->dataFunc  = data;
	out->initFuncs = inits;

	/* 同目录同名 .h。找不到就是空 —— 那种情况工具不硬塞声明，只提示用户 */
	const QFileInfo ci(cPath);
	const QString   h = ci.absolutePath() + QLatin1Char('/') + ci.completeBaseName()
	                  + QStringLiteral(".h");
	if (QFileInfo::exists(h))
	{
		out->hFile = h;
	}
	return true;
}

static bool findDriver(const QString &root, Driver *out)
{
	QStringList cs;
	collectC(QDir(root), cs, 4, 600);

	QVector<Driver> hits;
	for (const QString &c : cs)
	{
		Driver d;
		if (sniffDriver(c, &d))
		{
			hits << d;
		}
	}
	if (hits.isEmpty())
	{
		return false;
	}

	/* 有写命令+写数据的优先；都有就选文件最短的那个（驱动文件不会太大） */
	int best = -1;
	int bestScore = -1;
	for (int i = 0; i < hits.size(); ++i)
	{
		int score = 0;
		if (!hits.at(i).cmdFunc.isEmpty())
		{
			score += 2;
		}
		if (!hits.at(i).dataFunc.isEmpty())
		{
			score += 2;
		}
		if (!hits.at(i).hFile.isEmpty())
		{
			score += 1;
		}
		if (QFileInfo(hits.at(i).cFile).size() > 40 * 1024)
		{
			score -= 2;                     /* 太大的多半是库文件 */
		}
		if (score > bestScore)
		{
			bestScore = score;
			best = i;
		}
	}
	*out = hits.at(best);
	return true;
}

/*
 * 找「初始化调用的地方」，同时确认调用的是哪个 Init。
 *
 * 不能只看文件名是不是 main.c —— 有的工程叫 app_main.c、program.c。
 * 判据：这个文件里**调用**了 X_Init()，但**没有定义**它。
 *
 * initFuncs 有多个候选时逐个试（OLED_Init / OLED_I2C_Init / OLED_SPI_Init），
 * 谁真的在某个 .c 里被调用就选中谁 —— 光看名字猜不准哪个是用户调的。
 *
 * 一个都没命中时退回「名字叫 main.c 的文件」，initFunc 留空，
 * 那样只补 #include 不补示例 —— 总比插错地方强。
 */
static QString findMainC(const QString &root, const QStringList &initFuncs,
                         const QString &driverC, QString *hitFunc)
{
	if (hitFunc)
	{
		hitFunc->clear();
	}

	QStringList cs;
	collectC(QDir(root), cs, 4, 600);

	const QString drv = QDir::cleanPath(QFileInfo(driverC).absoluteFilePath());

	QString plainMain;              /* 名字是 main.c 的那个，兜底用 */

	for (const QString &c : cs)
	{
		if (QDir::cleanPath(QFileInfo(c).absoluteFilePath()) == drv)
		{
			continue;
		}
		QFile f(c);
		if (!f.open(QIODevice::ReadOnly))
		{
			continue;
		}
		const QByteArray raw = f.readAll();
		f.close();
		const Enc enc = detectEnc(raw);
		const QString text = decodeFrom(raw, enc);

		const bool isMainName = QFileInfo(c).fileName().toLower() == QLatin1String("main.c");
		if (isMainName && plainMain.isEmpty())
		{
			plainMain = QFileInfo(c).absoluteFilePath();
		}

		for (const QString &fn : initFuncs)
		{
			if (fn.isEmpty() || !text.contains(fn))
			{
				continue;
			}
			const QRegularExpression call(QStringLiteral("\\b%1\\s*\\(\\s*(?:void)?\\s*\\)\\s*;")
			                                  .arg(QRegularExpression::escape(fn)));
			if (!call.match(text).hasMatch())
			{
				continue;
			}
			/* 又没定义 */
			const QRegularExpression def(QStringLiteral("^\\s*void\\s+%1\\s*\\(")
			                                 .arg(QRegularExpression::escape(fn)),
			                             QRegularExpression::MultilineOption);
			if (def.match(text).hasMatch())
			{
				continue;
			}
			if (hitFunc)
			{
				*hitFunc = fn;
			}
			return QFileInfo(c).absoluteFilePath();
		}
	}
	return plainMain;
}

/* ==========================================================================
 *  三、生成插入清单
 * ========================================================================== */

/* 找到最后一个 #include 所在行（1 基）；没有返回 0 */
static int lastIncludeLine(const QList<QByteArray> &lines, Enc enc)
{
	int last = 0;
	static const QRegularExpression re(QStringLiteral("^[ \\t]*#[ \\t]*include\\b"));
	for (int i = 0; i < lines.size(); ++i)
	{
		if (re.match(decodeFrom(lines.at(i), enc)).hasMatch())
		{
			last = i + 1;
		}
	}
	return last;
}

/* 找到最后一个 #endif 所在行（1 基）；没有返回 0 */
static int lastEndifLine(const QList<QByteArray> &lines, Enc enc)
{
	int last = 0;
	static const QRegularExpression re(QStringLiteral("^[ \\t]*#[ \\t]*endif\\b"));
	for (int i = 0; i < lines.size(); ++i)
	{
		if (re.match(decodeFrom(lines.at(i), enc)).hasMatch())
		{
			last = i + 1;
		}
	}
	return last;
}

/* 找 initFunc() 单独成句的那一行（1 基）；没有返回 0 */
static int initCallLine(const QList<QByteArray> &lines, Enc enc, const QString &initFunc)
{
	if (initFunc.isEmpty())
	{
		return 0;
	}
	const QRegularExpression re(
		QStringLiteral("^[ \\t]*%1\\s*\\(\\s*\\)\\s*;")
			.arg(QRegularExpression::escape(initFunc)));
	for (int i = 0; i < lines.size(); ++i)
	{
		if (re.match(decodeFrom(lines.at(i), enc)).hasMatch())
		{
			return i + 1;
		}
	}
	return 0;
}

/*
 * 算 fromDir 到 toFile 的相对路径。
 *
 * 不用 QDir::relativeFilePath —— 实测在含中文的路径上它会把公共前缀判错，
 * 把 "..\Hardware\OLED_CFont.h" 算成 "proj/Hardware/OLED_CFont.h"，
 * 写进 #include 里 Keil 直接找不到文件。自己按路径段比最可靠。
 */
static QString relPath(const QString &fromDir, const QString &toFile)
{
	const QString a = QDir::cleanPath(QDir::fromNativeSeparators(fromDir));
	const QString b = QDir::cleanPath(QDir::fromNativeSeparators(toFile));

	const QStringList as = a.split(QLatin1Char('/'), Qt::SkipEmptyParts);
	const QStringList bs = b.split(QLatin1Char('/'), Qt::SkipEmptyParts);

	int i = 0;
	while (i < as.size() && i < bs.size())
	{
		/* 盘符按大小写不敏感比：C: 和 c: 是同一个盘 */
		const Qt::CaseSensitivity cs = (i == 0) ? Qt::CaseInsensitive : Qt::CaseSensitive;
		if (QString::compare(as.at(i), bs.at(i), cs) != 0)
		{
			break;
		}
		++i;
	}
	if (i == 0)
	{
		return b;                       /* 不同盘、没有公共前缀 → 只能用绝对路径 */
	}

	QStringList out;
	for (int k = i; k < as.size(); ++k)
	{
		out << QStringLiteral("..");
	}
	for (int k = i; k < bs.size(); ++k)
	{
		out << bs.at(k);
	}
	return out.join(QLatin1Char('/'));
}

/* 从上往下找「这是一个工程」的硬标记。找不到就返回空。 */
static QString findRootUpward(const QString &startDir)
{
	static const QStringList marks = {
		QStringLiteral("*.uvprojx"), QStringLiteral("*.uvproj"),
		QStringLiteral("*.ewp"),     QStringLiteral("*.cproject"),
		QStringLiteral("*.mxproject"), QStringLiteral("*.ioc"),
		QStringLiteral("platformio.ini")
	};

	QDir d(startDir);
	if (!d.exists())
	{
		return QString();
	}
	for (int up = 0; up < 4; ++up)
	{
		const QFileInfoList hits =
			d.entryInfoList(marks, QDir::Files | QDir::Hidden, QDir::Name);
		if (!hits.isEmpty())
		{
			const QString dir  = d.absolutePath();
			const QString leaf = QFileInfo(dir).fileName().toLower();
			/* .uvprojx 经常躺在 MDK-ARM\ 里，真工程根在上一层；
			 * 但代码和头文件可能就在 MDK-ARM\ 旁边，所以要连上一层一起试 */
			if (leaf == QLatin1String("mdk-arm") || leaf == QLatin1String("mdk")
			    || leaf == QLatin1String("ewarm") || leaf == QLatin1String("project"))
			{
				return QFileInfo(dir).absolutePath();
			}
			return dir;
		}
		if (!d.cdUp())
		{
			break;
		}
	}
	return QString();
}

bool detect(const QString &fontHeaderPath,
            const QString &sample1,
            const QString &sample2,
            Scan *out)
{
	if (!out)
	{
		return false;
	}
	*out = Scan();

	/* 统一成正斜杠：路径可能来自命令行（反斜杠）、QFileDialog（反斜杠）或
	 * QSettings（可能是正斜杠）。混着用会让 relativeFilePath 判不出公共前缀。 */
	/* 一律绝对化：主界面传进来的是绝对路径，但命令行/测试可能传相对路径，
	 * 不绝对化的话后面算 #include 相对路径会拿绝对对相对，直接算不出来。 */
	const QString fontAbs =
		QFileInfo(QDir::cleanPath(QDir::fromNativeSeparators(fontHeaderPath))).absoluteFilePath();
	out->fontHeader = fontAbs;

	if (fontAbs.isEmpty() || !QFileInfo::exists(fontAbs))
	{
		out->error = QStringLiteral("还没有导出字库。先点「导出 OLED_CFont.h」，再来做接入。");
		return false;
	}
	/* 工程根：优先认工程文件（.uvprojx 等），认不到再退回"字库目录的上一层"。
	 * 后者只是启发式 —— 字库放在 User\ 里时会推错，所以放在后面兜底。 */
	out->root = findRootUpward(QFileInfo(fontAbs).absolutePath());
	if (out->root.isEmpty())
	{
		out->root = QFileInfo(fontAbs).absolutePath();
		const QString leaf = QFileInfo(out->root).fileName().toLower();
		if (leaf == QLatin1String("hardware") || leaf == QLatin1String("bsp")
		    || leaf == QLatin1String("user") || leaf == QLatin1String("inc")
		    || leaf == QLatin1String("src"))
		{
			out->root = QFileInfo(out->root).absolutePath();
		}
	}

	/* ---- 1. 驱动 ---- */
	Driver drv;
	if (!findDriver(out->root, &drv))
	{
		out->error = QStringLiteral(
			"在 %1 下面没找到 OLED 驱动。\n\n"
			"判断依据是「哪个 .c 里定义了 OLED_WriteCommand（或类似的写命令函数）」，"
			"不看文件名。如果你确定工程在这儿，可以自己把两行声明加到驱动头文件里：\n\n"
			"void OLED_WriteCommand(uint8_t Command);\n"
			"void OLED_WriteData(uint8_t Data);")
			.arg(QDir::toNativeSeparators(out->root));
		return false;
	}
	out->driverC  = drv.cFile;
	out->driverH  = drv.hFile;
	out->cmdFunc  = drv.cmdFunc;
	out->dataFunc = drv.dataFunc;

	/* ---- 2. 驱动头文件补声明 ---- */
	if (!drv.hFile.isEmpty())
	{
		QFile f(drv.hFile);
		if (f.open(QIODevice::ReadOnly))
		{
			const QByteArray raw = f.readAll();
			f.close();
			const Enc enc = detectEnc(raw);
			const QString text = decodeFrom(raw, enc);
			QByteArray eol;
			const QList<QByteArray> lines = splitLines(raw, &eol);

			QStringList missing;
			const bool hasCmd  = !drv.cmdFunc.isEmpty()
			                     && QRegularExpression(
			                            QStringLiteral("\\bvoid\\s+%1\\s*\\(")
			                                .arg(QRegularExpression::escape(drv.cmdFunc)))
			                            .match(text).hasMatch();
			const bool hasData = !drv.dataFunc.isEmpty()
			                     && QRegularExpression(
			                            QStringLiteral("\\bvoid\\s+%1\\s*\\(")
			                                .arg(QRegularExpression::escape(drv.dataFunc)))
			                            .match(text).hasMatch();

			if (!drv.cmdFunc.isEmpty() && !hasCmd)
			{
				missing << QStringLiteral("void %1(uint8_t Command);").arg(drv.cmdFunc);
			}
			if (!drv.dataFunc.isEmpty() && !hasData)
			{
				missing << QStringLiteral("void %1(uint8_t Data);").arg(drv.dataFunc);
			}

			if (!missing.isEmpty())
			{
				/* 必须插在 #endif **之前**。插在后面就跑到 include guard 外面了，
				 * 头文件被两个 .c 包含时这两个声明会重复出现，直接编译报错。 */
				const int endifLn = lastEndifLine(lines, enc);
				Step s;
				s.file      = drv.hFile;
				s.afterLine = (endifLn > 0) ? endifLn - 1
				                            : static_cast<int>(lines.size());
				s.text   = QStringLiteral("/* 供 OLED_CFont.h / anim.h 调用（函数在 %1 里已定义，"
				                          "只是没在这个头文件里声明） */")
				               .arg(QFileInfo(drv.cFile).fileName())
				           + QLatin1Char('\n') + missing.join(QLatin1Char('\n'));
				s.title  = QStringLiteral("%1：补 %2 行函数声明")
				               .arg(QFileInfo(drv.hFile).fileName())
				               .arg(missing.size());
				s.detail = QStringLiteral("加在 #endif 之前（第 %1 行后）。"
				                          "原来这份头文件只导出了 OLED_Init / ShowChar 这些，"
				                          "写命令和写数据没声明，别的 .c 里调不到。")
				               .arg(s.afterLine);
				out->steps << s;
			}
			else
			{
				out->notes << QStringLiteral("%1 里已经有这两个函数的声明了，不用加。")
				                  .arg(QFileInfo(drv.hFile).fileName());
			}
		}
	}
	else
	{
		/* 注意占位符必须 %1/%2/%3 各出现一次：多参数 arg() 是单次扫描，
		 * 同一个 %1 出现两次会都换成文件名，用户照抄就会得到
		 * "void oled.c(uint8_t Command);" 这种编不过的东西。 */
		out->notes << QStringLiteral(
			"找到了驱动 %1，但它旁边没有同名 .h 文件。\n"
			"请手动把下面两行加到驱动头文件里：\n"
			"    void %2(uint8_t Command);\n    void %3(uint8_t Data);")
		                  .arg(QFileInfo(drv.cFile).fileName(), drv.cmdFunc, drv.dataFunc);
	}

	/* ---- 3. main.c 补 include + 示例 ---- */
	QString initFunc;
	out->mainC = findMainC(out->root, drv.initFuncs, drv.cFile, &initFunc);
	out->initFunc = initFunc;
	if (out->mainC.isEmpty())
	{
		out->notes << QStringLiteral(
			"没找到调用 %1() 的源文件，main.c 那两行需要你自己加。")
		                  .arg(drv.initFuncs.isEmpty() ? QStringLiteral("OLED_Init")
		                                               : drv.initFuncs.first());
	}
	else
	{
		QFile f(out->mainC);
		if (f.open(QIODevice::ReadOnly))
		{
			const QByteArray raw = f.readAll();
			f.close();
			const Enc enc = detectEnc(raw);
			const QString text = decodeFrom(raw, enc);
			QByteArray eol;
			const QList<QByteArray> lines = splitLines(raw, &eol);
			const QString mainName = QFileInfo(out->mainC).fileName();

			/* include：用相对路径，不依赖 Keil 的 Include Path 配没配 */
			const bool hasInclude =
				QRegularExpression(QStringLiteral("#[ \\t]*include\\s*\"[^\"]*OLED_CFont\\.h\""))
					.match(text).hasMatch();

			if (!hasInclude)
			{
				/* 相对路径，不依赖 Keil 的 Include Path 配没配 */
				const QString mainDir = QDir::cleanPath(
					QDir::fromNativeSeparators(QFileInfo(out->mainC).absolutePath()));
				const QString rel = relPath(mainDir, out->fontHeader);

				Step s;
				s.file      = out->mainC;
				s.afterLine = lastIncludeLine(lines, enc);
				s.text      = QStringLiteral("#include \"%1\"").arg(rel);
				s.title     = QStringLiteral("%1：补 #include").arg(mainName);
				s.detail    = QStringLiteral("加在最后一个 #include 之后（第 %1 行后）。\n"
				                             "字库在 %2，main.c 在 %3，所以写成相对路径 %4"
				                             "—— Keil 里不用另外配 Include Path。")
				                  .arg(s.afterLine).arg(out->fontHeader, mainDir, rel);
				out->steps << s;
			}
			else
			{
				out->notes << QStringLiteral("%1 里已经 include 过 OLED_CFont.h 了。").arg(mainName);
			}

			/* 示例调用 */
			const bool hasCall = text.contains(QLatin1String("OLED_CFont_ShowString"));
			const int  initLn  = initCallLine(lines, enc, initFunc);

			if (!hasCall && !sample1.isEmpty() && initLn > 0)
			{
				QStringList body;
				body << QStringLiteral("\tOLED_CFont_ShowString( 1, 1, \"%1\" );").arg(sample1);
				if (!sample2.isEmpty())
				{
					body << QStringLiteral("\tOLED_CFont_ShowString( 2, 1, \"%1\" );").arg(sample2);
				}

				Step s;
				s.file      = out->mainC;
				s.afterLine = initLn;
				s.text      = body.join(QLatin1Char('\n'));
				s.title     = QStringLiteral("%1：补 %2 行示例调用").arg(mainName).arg(body.size());
				s.detail    = QStringLiteral("加在 %1(); 之后（第 %2 行后）。"
				                             "如果下面还有别的 OLED_ShowXxx 会覆盖同一行，"
				                             "把多余的删掉即可。")
				                  .arg(initFunc)
				                  .arg(initLn);
				out->steps << s;
			}
			else if (hasCall)
			{
				out->notes << QStringLiteral("%1 里已经在用 OLED_CFont_ShowString 了，示例就不加了。")
				                  .arg(mainName);
			}
			else if (initLn == 0)
			{
				out->notes << QStringLiteral(
					"%1 里没找到单独成行的 %2();，所以示例调用没敢往里插 —— 插错地方会编译不过。\n"
					"你自己在那句后面加两行就行。")
				                  .arg(mainName, drv.initFuncs.isEmpty()
				                                     ? QStringLiteral("OLED_Init")
				                                     : drv.initFuncs.first());
			}
		}
	}

	return true;
}

/* ==========================================================================
 *  四、预览 / 写盘 / 撤销
 * ========================================================================== */

static QVector<Step> stepsOf(const QVector<Step> &all, const QString &file)
{
	QVector<Step> v;
	for (const Step &s : all)
	{
		if (s.checked && QDir::cleanPath(s.file) == QDir::cleanPath(file))
		{
			v << s;
		}
	}
	/* 从后往前插，前面插入的行号才不会被打乱 */
	std::sort(v.begin(), v.end(), [](const Step &a, const Step &b) {
		return a.afterLine > b.afterLine;
	});
	return v;
}

QString previewOf(const QString &file, const QVector<Step> &steps, int context)
{
	QFile f(file);
	if (!f.open(QIODevice::ReadOnly))
	{
		return QStringLiteral("（打不开 %1）").arg(file);
	}
	const QByteArray raw = f.readAll();
	f.close();
	const Enc enc = detectEnc(raw);
	QByteArray eol;
	const QList<QByteArray> lines = splitLines(raw, &eol);

	const QVector<Step> mine = stepsOf(steps, file);
	if (mine.isEmpty())
	{
		return QString();
	}

	QString out;
	for (const Step &s : mine)
	{
		out += QStringLiteral("── 插在第 %1 行之后 ──\n").arg(s.afterLine);

		auto dumpAdd = [&]() {
			const QStringList add = s.text.split(QLatin1Char('\n'));
			for (const QString &a : add)
			{
				out += QStringLiteral("+ %1\n").arg(a);
			}
		};

		if (s.afterLine <= 0)
		{
			dumpAdd();                          /* 插到文件最前面 */
		}

		const int from = qMax(1, s.afterLine - context);
		const int to   = qMin(static_cast<int>(lines.size()), qMax(s.afterLine, 1) + context);
		for (int i = from; i <= to; ++i)
		{
			out += QStringLiteral("  %1\n").arg(decodeFrom(lines.at(i - 1), enc));
			if (i == s.afterLine)
			{
				dumpAdd();
			}
		}
		out += QLatin1Char('\n');
	}
	return out;
}

bool apply(const Scan &scan, QStringList *touched, QString *err)
{
	if (touched)
	{
		touched->clear();
	}

	/* 按文件分组 */
	QStringList files;
	for (const Step &s : scan.steps)
	{
		if (s.checked && !files.contains(s.file))
		{
			files << s.file;
		}
	}
	if (files.isEmpty())
	{
		if (err)
		{
			*err = QStringLiteral("没有勾选任何改动。");
		}
		return false;
	}

	for (const QString &file : files)
	{
		QFile f(file);
		if (!f.open(QIODevice::ReadOnly))
		{
			if (err)
			{
				*err = QStringLiteral("打不开 %1：%2").arg(file, f.errorString());
			}
			return false;
		}
		const QByteArray raw = f.readAll();
		f.close();

		const Enc enc = detectEnc(raw);

		/* 把 UTF-8 BOM 摘出来单独保管，写完再补回最前面。
		 * 留在第 1 行内容里的话，一旦插入点算成 0，BOM 就被挤到第二行行首：
		 * 文件头成了新插入的那行，BOM 落到中间，armcc/MSVC 会报非法字符，
		 * 而且文件不再以 BOM 开头，下次本工具探测编码会把中文误判成 GBK。 */
		QByteArray bom;
		QByteArray body = raw;
		if (body.startsWith("\xEF\xBB\xBF"))
		{
			bom = body.left(3);
			body.remove(0, 3);
		}

		QByteArray eol;
		QList<QByteArray> lines = splitLines(body, &eol);

		/* 备份，只在第一次改这个文件时做。固定名 .oledbak：
		 * 带时间戳会堆一地文件，而重复点「接入」本来就几乎不改东西。 */
		const QString bak = file + QStringLiteral(".oledbak");
		if (!QFileInfo::exists(bak))
		{
			if (!QFile::copy(file, bak))
			{
				if (err)
				{
					*err = QStringLiteral("备份失败，为了安全不再继续：\n%1").arg(bak);
				}
				return false;
			}
		}

		const QVector<Step> mine = stepsOf(scan.steps, file);
		for (const Step &s : mine)
		{
			QList<QByteArray> add;
			const QStringList parts = s.text.split(QLatin1Char('\n'));
			for (const QString &p : parts)
			{
				add << encodeTo(p, enc);
			}
			int at = qBound(0, s.afterLine, static_cast<int>(lines.size()));
			for (int k = add.size() - 1; k >= 0; --k)
			{
				lines.insert(at, add.at(k));
			}
		}

		QSaveFile out(file);
		/* 先写临时文件再改名，能保证"要么全写成，要么原样不动"。
		 * 但在某些中文路径的目录里建临时文件会被拒（实测 Win11 上就有），
		 * 所以打开 directWriteFallback：拿不到临时文件就直接写目标文件。
		 * 反正前面已经留了 .oledbak，最坏情况也能撤回来。 */
		out.setDirectWriteFallback(true);
		if (!out.open(QIODevice::WriteOnly))
		{
			if (err)
			{
				*err = QStringLiteral("写不进 %1：%2").arg(file, out.errorString());
			}
			return false;
		}
		QByteArray bytes = joinLines(lines, eol);

		/* 原来就有 BOM 的，照旧补回去。
		 * 原来没有 BOM、但这次写进去的内容里有中文（UTF-8 字节），就补一个：
		 * armcc（Keil AC5）不看文件内容判编码，一律按系统代码页（GBK）当双字节走。
		 * 汉字的 UTF-8 三字节最后一位常落在 GBK 的引导字节区间，
		 * 会把后面那个引号当成第二字节吞掉，编译直接报：
		 *     error:  #8: missing closing quote
		 * 文件开头带 BOM，armcc 才改按 UTF-8 解析（实测 AC5.06u5）。
		 * 纯 ASCII 的源文件不动 —— 没必要给老工程平白多加三个字节。 */
		if (!bom.isEmpty())
		{
			bytes.prepend(bom);
		}
		else if (enc == EncUtf8 && hasNonAsciiByte(bytes))
		{
			bytes.prepend("\xEF\xBB\xBF");
		}

		out.write(bytes);
		if (!out.commit())
		{
			if (err)
			{
				*err = QStringLiteral("保存 %1 失败：%2").arg(file, out.errorString());
			}
			return false;
		}

		if (touched)
		{
			*touched << file;
		}
	}
	return true;
}

QStringList revertable(const QStringList &files)
{
	QStringList out;
	for (const QString &f : files)
	{
		if (QFileInfo::exists(f + QStringLiteral(".oledbak")))
		{
			out << f;
		}
	}
	return out;
}

bool revert(const QStringList &files, QString *err)
{
	for (const QString &f : files)
	{
		const QString bak = f + QStringLiteral(".oledbak");
		if (!QFileInfo::exists(bak))
		{
			continue;
		}

		/* 目标被人删了、只剩备份：直接把备份放回去。
		 * 老实现先 remove(f) 再 copy，f 不存在时 remove 就失败，
		 * 于是"文件被删了"反而撤销不了，明明备份还在。 */
		if (!QFileInfo::exists(f))
		{
			if (!QFile::rename(bak, f))
			{
				if (err)
				{
					*err = QStringLiteral("还原失败：%1 -> %2").arg(bak, f);
				}
				return false;
			}
			continue;
		}

		/* 先改名再还原，不要「先删后拷」。
		 * 老写法 QFile::remove(f) 成功、QFile::copy(bak, f) 失败
		 * （磁盘满、杀软锁目录、备份被占用）时，用户原来的文件已经没了，
		 * 备份又没拷回来 —— 一次撤销把源码删干净。
		 * 改名不丢数据：把原文件挪到 .oledtmp 占着位置，备份 rename 上来；
		 * 中间任何一步失败都还能把 .oledtmp 改回去。 */
		const QString tmp = f + QStringLiteral(".oledtmp");
		QFile::remove(tmp);        /* 上一轮崩在中间留下的残留 */
		if (!QFile::rename(f, tmp))
		{
			if (err)
			{
				*err = QStringLiteral("改不动 %1，可能被 Keil 占用了").arg(f);
			}
			return false;
		}
		if (!QFile::rename(bak, f))
		{
			QFile::rename(tmp, f); /* 还原失败就把原文件放回去，别让人两头空 */
			if (err)
			{
				*err = QStringLiteral("还原失败：%1 -> %2（原文件已放回）").arg(bak, f);
			}
			return false;
		}
		QFile::remove(tmp);
	}
	return true;
}

QString readAsText(const QString &path)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
	{
		return QString();
	}
	const QByteArray raw = f.readAll();
	f.close();
	return decodeFrom(raw, detectEnc(raw));
}

}   // namespace MergeProject
