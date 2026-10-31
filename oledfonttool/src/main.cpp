#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QCollator>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPixmap>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QSizePolicy>
#include <QSlider>
#include <QSpinBox>
#include <QSignalBlocker>
#include <QScreen>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringConverter>
#include <QTabWidget>
#include <QTextCursor>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

#include "fontgen.h"
#include "fontpreview.h"
#include "imagegen.h"
#include "imagepreview.h"
#include "chipinfo.h"
#include "mergeproject.h"
#include "screenprobe.h"
#include "selftest.h"

/* 诊断用：读进程令牌，判断是否被 AppContainer / 低完整性限制 */
#ifdef Q_OS_WIN
#  include <windows.h>
#  include <shlobj.h>
#endif

/*
 * OLED 点阵取模工具
 *
 * 关键约束：生成的字模必须和工程里 Hardware/OLED.c 的
 * OLED_ShowChinese() 对得上（列行式 / 逆向 / 低位在前 / 每字按页排列）。
 * 每字字节数随字号变：16×16 = 32，24×24 = 72，32×32 = 128。
 * 编码逻辑在 fontgen.cpp，那份算法和 gen_oled_hz.py 是等价的，
 * 改任何一边都要同步改另一边，否则字会花屏。
 */

/*
 * 找本工具自己的 src 目录（里面有 install-cfont.cmd 等脚本）。
 * exe 在 oledfonttool\build\ 下，src 在 oledfonttool\src\，所以向上找。
 * 之所以不写死 "../src"：工具被拷到别处时仍然能找到。
 */
static QString findSrcDir()
{
	QDir dir(QCoreApplication::applicationDirPath());
	for (int up = 0; up <= 5; ++up)
	{
		const QString candidate = dir.absoluteFilePath(QStringLiteral("src"));
		if (QFile::exists(QDir(candidate).absoluteFilePath(QStringLiteral("install-cfont.cmd"))))
		{
			return candidate;
		}
		if (!dir.cdUp())
		{
			break;
		}
	}
	return QString();
}

/*
 * ---- 源码编码检查 ----
 *
 * 为什么需要：
 *   Keil(ARMCC 5.x) 按系统 ANSI 代码页读源文件，而它【不做任何转码】——
 *   源文件里是什么字节，字符串字面量里就是什么字节。
 *   而字库里的键是固定的 UTF-8 三字节。两边编码不一致时：
 *       编译通过、程序能跑、屏幕上什么都没有
 *   这种失败没有任何报错，最难查。所以这里主动检查并提醒。
 */

/* 扫描时要跳过的目录：构建产物、IDE 缓存、包管理器目录。
 * 扫这些既慢又没有意义，而且 build/ 下常常有同名副本，容易认错。 */
static const QStringList &heavyDirs()
{
	static const QStringList kSkip = {
		QStringLiteral("build"),        QStringLiteral("package"),
		QStringLiteral("Output"),       QStringLiteral("Objects"),
		QStringLiteral("Listings"),     QStringLiteral("DebugConfig"),
		QStringLiteral(".git"),         QStringLiteral(".vs"),
		QStringLiteral("node_modules"), QStringLiteral("__pycache__"),
		QStringLiteral("tmp"),          QStringLiteral("staging"),
		QStringLiteral("RTE"),          QStringLiteral(".settings")
	};
	return kSkip;
}

/* path 是否落在 excl 这棵树里（含自身） */
static bool isUnder(const QString &path, const QString &excl)
{
	if (excl.isEmpty())
	{
		return false;
	}
	const QString here = QDir::cleanPath(path);
	const QString root = QDir::cleanPath(excl);
	return here.compare(root, Qt::CaseInsensitive) == 0 ||
	       here.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive);
}

/*
 * 在 base 目录下（含自身）递归找某个文件名，最多找 maxDepth 层。
 * 找到就返回它所在的目录。
 *
 * excludePrefix 用来排除一整棵子树 —— 主要是把工具自己的目录排掉，
 * 否则会先找到 oledfonttool\src\OLED_CFont.h（工具自带的那份），
 * 而不是用户工程里的。
 */
static QString findFileUnder(const QString &base, const QString &fileName, int maxDepth,
                             const QString &excludePrefix = QString())
{
	if (maxDepth < 0)
	{
		return QString();
	}
	QDir d(base);
	if (!d.exists() || isUnder(d.absolutePath(), excludePrefix))
	{
		return QString();
	}

	if (QFile::exists(d.absoluteFilePath(fileName)))
	{
		return d.absolutePath();
	}

	const QFileInfoList subs =
		d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
	for (const QFileInfo &fi : subs)
	{
		if (heavyDirs().contains(fi.fileName(), Qt::CaseInsensitive))
		{
			continue;
		}
		const QString hit = findFileUnder(fi.absoluteFilePath(), fileName,
		                                  maxDepth - 1, excludePrefix);
		if (!hit.isEmpty())
		{
			return hit;
		}
	}
	return QString();
}

/* 从字库路径反推工程根目录：字库在 <工程>\Hardware\ 下，往上一级就是工程根。
 * 不依赖工程叫什么名字。 */
static QString findProjectRoot(const QString &headerPath)
{
	if (headerPath.isEmpty())
	{
		return QString();
	}
	QDir d(QFileInfo(headerPath).absolutePath());   // ...\Hardware
	if (d.cdUp())                                  // ...\<工程>
	{
		return d.absolutePath();
	}
	return QString();
}

/* ============================================================================
 * 定位用户工程 —— 靠"工程文件"，不靠驱动文件名
 *
 * 为什么换：
 *   最早写死目录名 MyFreeRTOS_C8T6，改名就失效；
 *   后来改成找 Hardware\OLED.c，可用的是"驱动文件叫 OLED.c"这个巧合，
 *   别人的驱动叫 lcd.c / ssd1306.c / Display.c / my_screen.c，一样认不出。
 *   把这种"借用户的命名习惯"当判据，换个工程就崩，跟写死本质是一回事。
 *
 * 真正的判据是工程文件本身：
 *   .uvprojx / .uvproj / .ewp / .cproject / platformio.ini / .mxproject
 *   这些东西是"这是一个工程"的定义，跟源码文件怎么起名完全无关。
 *
 * 找到工程根之后，字库放哪一层？
 *   字库是纯数据头文件，只要所在目录在 include path 里就能用，
 *   所以不需要精确识别"驱动在哪个文件"，只要挑个用户装惯的地方：
 *   已有字库的目录 → 常见源码目录名 → 工程根。
 * ==========================================================================*/

/* 各 IDE 用来标记"这里是工程"的文件。带 * 的通配。 */
static QStringList projectMarkers()
{
	return QStringList {
		QStringLiteral("*.uvprojx"),       /* Keil MDK 5 */
		QStringLiteral("*.uvproj"),        /* Keil MDK 4 */
		QStringLiteral("*.ewp"),           /* IAR EWARM */
		QStringLiteral(".cproject"),       /* STM32CubeIDE / Eclipse CDT */
		QStringLiteral(".mxproject"),      /* STM32CubeMX 生成 */
		QStringLiteral("platformio.ini")   /* PlatformIO */
	};
}

/*
 * 在 base 下（含自身）按层找工程文件，返回同一层里的【所有】工程根。
 *
 * 为什么返回列表而不是一个：
 *   一个父目录下常常并排放着好几个工程 —— 比如 D:\STM32\ 下面就有
 *   LED闪烁实验、MyFreeRTOS_C8T6c、MyFreeRTOS_ZET6k…… 只取第一个
 *   等于按字母序瞎猜，猜错了用户还不知道为什么。
 *   收集起来才能挑，也才能老实告诉用户"这一层还有别的工程"。
 *
 * 只收同一层：浅层的工程比深层更可能是当前在用的，
 * 而且一旦某层命中就不再往里钻（工程根内部不会再有工程根）。
 *
 * maxDirs 是硬上限：从盘根开始找时，几十万目录会卡死，到上限就放弃。
 */
static QStringList findProjectRootsUnder(const QString &base, int maxDepth,
                                         const QString &excludePrefix = QString(),
                                         int maxCount = 12)
{
	QStringList result;
	if (base.isEmpty() || maxDepth < 0)
	{
		return result;
	}

	const QStringList patterns = projectMarkers();
	QStringList level { base };
	int visited = 0;
	const int kMaxDirs = 1500;

	for (int depth = 0; depth <= maxDepth && !level.isEmpty(); ++depth)
	{
		QStringList next;
		QStringList hits;

		for (const QString &p : level)
		{
			if (++visited > kMaxDirs)
			{
				break;
			}
			if (isUnder(p, excludePrefix))
			{
				continue;
			}
			const QDir d(p);
			if (!d.exists())
			{
				continue;
			}

			/* 这一层有工程文件 → 命中。
			 * 必须带 QDir::Hidden：.cproject / .mxproject 是点开头的隐藏文件，
			 * 不带这个参数 entryList 根本列不出来。 */
			if (!d.entryList(patterns, QDir::Files | QDir::Hidden).isEmpty())
			{
				hits << QDir::cleanPath(d.absolutePath());
				if (hits.size() >= maxCount)
				{
					return hits;
				}
				continue;       /* 已是工程根，不必再往里找 */
			}

			const QFileInfoList subs = d.entryInfoList(
				QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);
			for (const QFileInfo &fi : subs)
			{
				if (!heavyDirs().contains(fi.fileName(), Qt::CaseInsensitive))
				{
					next << fi.absoluteFilePath();
				}
			}
		}

		if (!hits.isEmpty())
		{
			return hits;        /* 这一层有就到此为止，不往深了收 */
		}
		level = next;
	}
	return result;
}

/*
 * 多个候选里挑一个默认值。规则从强到弱：
 *   1. 里面已经有 OLED_CFont.h 的 —— 之前就在这个工程里导出过
 *   2. 目录修改时间最新的 —— 最近在动的那个通常就是正在做的
 *   3. 第一个（按名字序，保证结果稳定可复现）
 *
 * 这只是"默认值"。UI 会把"这一层还有几个别的工程"写出来，
 * 用户一看不对就能一键换 —— 暗中选择又不提示，才是真的坑。
 */
static QString pickBestProject(const QStringList &roots)
{
	if (roots.isEmpty())
	{
		return QString();
	}
	if (roots.size() == 1)
	{
		return roots.first();
	}

	for (const QString &r : roots)
	{
		if (!findFileUnder(r, QStringLiteral("OLED_CFont.h"), 2).isEmpty())
		{
			return r;
		}
	}

	QString best = roots.first();
	QDateTime bestTime = QFileInfo(best).lastModified();
	for (int i = 1; i < roots.size(); ++i)
	{
		const QDateTime t = QFileInfo(roots.at(i)).lastModified();
		if (t > bestTime)
		{
			best = roots.at(i);
			bestTime = t;
		}
	}
	return best;
}

/*
 * 工程根定了，字库写哪层。
 *
 * strongOnly = true ：只认"已有字库"和"常见源码目录名"，都不中就返回空，
 *                     交给调用方换一个候选根再试。这样"上一层才是真工程根"
 *                     的情况（.uvprojx 放在 MDK-ARM\ 里）能自动纠正。
 * strongOnly = false：都找不到就退回工程根本身。
 */
static QString pickFontDirIn(const QString &projRoot, bool strongOnly,
                             const QString &excludePrefix = QString())
{
	if (projRoot.isEmpty())
	{
		return QString();
	}
	const QDir root(projRoot);
	if (!root.exists())
	{
		return QString();
	}

	/* 1) 工程里已经有字库 → 原地更新。
	 *    绝不能换个目录再写一份，否则工程里两个 OLED_CFont.h，谁生效看运气。 */
	{
		const QString hit = findFileUnder(projRoot, QStringLiteral("OLED_CFont.h"), 3, excludePrefix);
		if (!hit.isEmpty())
		{
			return hit;
		}
	}

	/* 2) 常见放法，按顺序取第一个存在的 */
	static const QStringList kCommon = {
		QStringLiteral("Hardware"),   QStringLiteral("User"),
		QStringLiteral("BSP"),        QStringLiteral("Drivers"),
		QStringLiteral("App"),        QStringLiteral("Core/Src"),
		QStringLiteral("Src"),        QStringLiteral("Source"),
		QStringLiteral("Inc"),        QStringLiteral("Source/Hardware")
	};
	for (const QString &rel : kCommon)
	{
		const QDir d(root.absoluteFilePath(rel));
		if (d.exists())
		{
			return QDir::cleanPath(d.absolutePath());
		}
	}

	if (strongOnly)
	{
		return QString();
	}

	/* 3) 都不像 → 工程根。位置不理想，但至少落在工程里，用户自己搬一步就行 */
	return QDir::cleanPath(root.absolutePath());
}

/*
 * 从"含工程文件的那一层"推出最终写字库的目录。
 *
 * 同一层先试，不中再试上一层 —— 因为 .uvprojx 经常被放在 MDK-ARM\、
 * EWARM\、Project\ 这类子目录里，真工程根在上一层。
 */
/*
 * 把长路径截成「…\末两级」，只用于面板里的那行提示。
 *
 * 完整路径往往有 40~60 个字符（D:\STM32\MyFreeRTOS_C8T6c\Hardware\OLED_CFont.h），
 * 面板宽度只有 300 出头，一行放不下就得折成两行，白吃 20 像素高度。
 * 真正需要全文的场景（复制、核对）交给 tooltip。
 */
static QString tailPath(const QString &full, int keep = 2)
{
	const QString clean = QDir::toNativeSeparators(QDir::cleanPath(full));
	const QStringList parts = clean.split(QLatin1Char('\\'), Qt::SkipEmptyParts);
	if (parts.size() <= keep)
	{
		return clean;
	}
	return QStringLiteral("…\\") + parts.mid(parts.size() - keep).join(QLatin1Char('\\'));
}

static QString resolveFontDir(const QString &markerDir, const QString &excludePrefix = QString())
{
	if (markerDir.isEmpty())
	{
		return QString();
	}

	QStringList candidates;
	candidates << QDir::cleanPath(markerDir);
	{
		const QString up = QFileInfo(markerDir).absolutePath();
		if (!up.isEmpty() && !isUnder(up, excludePrefix))
		{
			candidates << QDir::cleanPath(up);
		}
	}

	for (const QString &c : candidates)
	{
		const QString hit = pickFontDirIn(c, true, excludePrefix);
		if (!hit.isEmpty())
		{
			return hit;
		}
	}
	/* 都没有强命中 → 就在工程文件那一层 */
	return QDir::cleanPath(markerDir);
}

/*
 * 递归收集 .c / .h 文件。用于编码检查。
 *
 * 同样不依赖子目录叫什么名字 —— 有些工程叫 User/Hardware，
 * 有些叫 Core/Src/App，都无所谓。
 * 但有深度和数量上限，避免扫到巨大的第三方库时卡住。
 */
static void collectSourceFiles(const QDir &dir, QStringList &out, int maxDepth, int maxFiles)
{
	if (maxDepth < 0 || out.size() >= maxFiles)
	{
		return;
	}

	const QFileInfoList files =
		dir.entryInfoList(QStringList() << QStringLiteral("*.c") << QStringLiteral("*.h"),
		                  QDir::Files, QDir::Name);
	for (const QFileInfo &fi : files)
	{
		if (out.size() >= maxFiles)
		{
			return;
		}
		out << fi.absoluteFilePath();
	}

	const QFileInfoList subs =
		dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
	for (const QFileInfo &fi : subs)
	{
		if (heavyDirs().contains(fi.fileName(), Qt::CaseInsensitive))
		{
			continue;
		}
		collectSourceFiles(QDir(fi.absoluteFilePath()), out, maxDepth - 1, maxFiles);
		if (out.size() >= maxFiles)
		{
			return;
		}
	}
}

/* 检测一个源文件的编码。
 * 判据：UTF-8 的多字节序列有严格规则，GBK 的中文字节几乎不可能碰巧是合法 UTF-8，
 *       所以"能否整体按 UTF-8 解码"就是很可靠的判据。 */
static QString detectSourceEncoding(const QString &path, QString *outText,
                                   QByteArray *outRaw = nullptr)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
	{
		return QString();
	}
	const QByteArray raw = f.readAll();
	f.close();

	if (outRaw)
	{
		*outRaw = raw;          /* 给 judgeSource 判"缺不缺 BOM"用 */
	}

	QString text;
	QString enc;

	if (raw.startsWith("\xEF\xBB\xBF"))
	{
		enc = QStringLiteral("UTF-8 BOM");
		text = QString::fromUtf8(raw.mid(3));
	}
	else
	{
		QStringDecoder dec(QStringDecoder::Utf8);
		text = dec(raw);
		if (dec.hasError())
		{
			enc = QStringLiteral("GBK/ANSI");
			/* 用系统代码页解码，好让用户看清乱码的样子 */
			text = QString::fromLocal8Bit(raw);
		}
		else
		{
			enc = QStringLiteral("UTF-8");
		}
	}

	if (outText)
	{
		*outText = text;
	}
	return enc;
}

/* 从 C 源码里抽出双引号字符串字面量的内容（跳过注释）。
 * 只做够用的解析：识别行注释与块注释、处理反斜杠转义。 */
static QStringList collectStringLiterals(const QString &src)
{
	QStringList out;
	QString cur;
	bool inStr = false;
	bool inLineComment = false;
	bool inBlockComment = false;
	bool escape = false;

	for (int i = 0; i < src.size(); ++i)
	{
		const QChar c = src.at(i);
		const QChar n = (i + 1 < src.size()) ? src.at(i + 1) : QChar();

		if (inLineComment)
		{
			if (c == QLatin1Char('\n')) inLineComment = false;
			continue;
		}
		if (inBlockComment)
		{
			if (c == QLatin1Char('*') && n == QLatin1Char('/')) { inBlockComment = false; ++i; }
			continue;
		}
		if (inStr)
		{
			if (escape)          { cur.append(c); escape = false; }
			else if (c == QLatin1Char('\\')) { escape = true; }
			else if (c == QLatin1Char('"'))  { inStr = false; out << cur; cur.clear(); }
			else                 { cur.append(c); }
			continue;
		}

		if (c == QLatin1Char('/') && n == QLatin1Char('/')) { inLineComment = true; ++i; continue; }
		if (c == QLatin1Char('/') && n == QLatin1Char('*')) { inBlockComment = true; ++i; continue; }
		if (c == QLatin1Char('"')) { inStr = true; cur.clear(); continue; }
	}
	return out;
}

/* 判断 s 的第 i 位开始是不是 "0x"/"0X" + 两个十六进制数字（也就是一个字节的字面量） */
static bool isHexPair(const QString &s, int i)
{
	if (i + 3 >= s.size())            return false;
	if (s.at(i) != QLatin1Char('0'))  return false;
	if (s.at(i + 1) != QLatin1Char('x') && s.at(i + 1) != QLatin1Char('X')) return false;

	for (int k = i + 2; k <= i + 3; ++k)
	{
		const QChar c = s.at(k);
		const bool hex = (c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
		                 (c >= QLatin1Char('a') && c <= QLatin1Char('f')) ||
		                 (c >= QLatin1Char('A') && c <= QLatin1Char('F'));
		if (!hex)
		{
			return false;
		}
	}
	return true;
}

static const char *kQss =
	/* 注意：这里不写全局 color。
	 * 写了之后 QMessageBox 会继承深色文字，而它在深色主题下是深色背景，
	 * 结果弹窗文字完全看不见（踩过这个坑）。颜色一律写进具体控件。 */
	"QWidget { font-family: 'Microsoft YaHei UI','Microsoft YaHei',sans-serif; font-size: 12px; }"
	"QMainWindow, QSplitter { background: #FFFFFF; }"
	"#SidePanel { background: #FAFBFC; border-right: 1px solid #E4E7EB; }"
	"#Title { color: #1F2328; font-size: 16px; font-weight: 600; }"
	"#SubTitle, #Hint { color: #6B7280; font-size: 11px; }"
	"#Count { color: #1F6FEB; font-size: 12px; font-weight: 600; }"
	"#Warn { color: #B45309; background: #FEF3C7; border: 1px solid #FDE68A;"
	"        border-radius: 4px; padding: 4px 6px; font-size: 11px; }"
	"#PathLabel { color: #6B7280; font-size: 11px; }"
	"QLabel { color: #1F2328; }"
	"QCheckBox { color: #1F2328; }"
	"QGroupBox { color: #374151; border: 1px solid #E4E7EB; border-radius: 6px; margin-top: 14px; padding: 10px 8px 8px 8px; background: #FFFFFF; }"
	"QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: #374151; font-weight: 600; }"
	"QPlainTextEdit, QComboBox, QLineEdit { color: #1F2328; border: 1px solid #D7DBE0; border-radius: 4px; padding: 4px 6px; background: #FFFFFF; }"
	"QPlainTextEdit:focus, QComboBox:focus, QLineEdit:focus { border-color: #3B82F6; }"
	"QPushButton { color: #1F2328; border: 1px solid #D7DBE0; border-radius: 4px; padding: 6px 12px; background: #FFFFFF; }"
	"QPushButton:hover { background: #F3F4F6; }"
	"QPushButton#Primary { background: #3B82F6; border-color: #3B82F6; color: #FFFFFF; font-weight: 600; }"
	"QPushButton#Primary:hover { background: #2F6FE0; }"
	"QSlider::groove:horizontal { height: 4px; background: #E4E7EB; border-radius: 2px; }"
	"QSlider::handle:horizontal { width: 14px; margin: -6px 0; background: #3B82F6; border-radius: 7px; }"
	/* 弹窗显式配色，别依赖系统主题 */
	"QMessageBox { background: #FFFFFF; }"
	"QMessageBox QLabel { color: #1F2328; min-width: 320px; }";

/* 一份素材 = 一张图或一个 GIF。
 *
 * 之所以要这个结构：工程里常常同时要开机图、logo、几个小图标，
 * 它们尺寸不同、阈值不同、导出成不同的头文件。原先图片页只有一个槽位，
 * 导完一张换下一张，前一张的参数就丢了，来回改很折磨人。
 *
 * mono / packed 是当前参数下的计算结果，缓存在这里而不是每次重算：
 * 从列表切回来时不用再跑一遍缩放和抖动。
 */
struct ImageAsset
{
	QString                    name;        /* 列表名，也是导出文件名和标识前缀 */
	QStringList                files;       /* 参与的文件（GIF 就一个） */
	QString                    sourceName;  /* 写进头文件「来源」那行 */
	QVector<QImage>            source;      /* 原始帧 */
	QVector<QImage>            mono;        /* 单色化后（预览用） */
	QVector<QVector<quint8>>   packed;      /* 显存字节（导出用） */
	int                        delayMs = 100;
	ImageGen::Options          opt;         /* 每份素材自己的取模设置 */
	int                        step = 1;    /* 抽帧步长 */
	QString                    note;        /* 导入时的提醒：截断 / 读不出 */
};

class MainWindow : public QMainWindow
{
public:
	MainWindow();

private:
	void refreshPreview();
	/* askForPath = false 表示调用方已经让用户选过保存位置了，
	 * 不要再弹一次对话框（「另存为…」就是这么调用的）。 */
	void exportHeader(bool askForPath = true);
	void importHeader();

	void buildUi();
	void updateStatus();
	/* 侧栏副标题跟着「取反」开关变：勾上以后屏上是亮底暗字，
	 * 再挂「阴码」两个字就自相矛盾了。 */
	void updateSubTitle();
	void checkFontExists();
	void checkSourceEncoding(bool popupOnProblem);
	void writeEncodingLog(const QStringList &lines) const;
	QString defaultHeaderPath() const;
	/* 让用户自己指定工程目录，并重新推算字库落点。
	 * 自动定位猜不准时用它 —— 手动指过一次就记住，比任何猜测都可靠。 */
	void chooseProjectDir();

	/* ---- 图片 / 动画模式 ---- */
	QWidget *buildImagePage();
	void loadImageFile();           /* 添加素材：单个文件一份，或多个文件按模式处理 */
	void recomputeImage();          /* 重新缩放+单色+打包，并刷新预览 */
	void exportAnimHeader();        /* 导出当前素材 */
	void exportAllAssets();         /* 每份素材各导一个 .h */
	void exportAssetsToSingleFile(); /* 全部素材写进一个 .h，但各存各的 */
	void togglePlayback();
	void updateImageInfo();
	ImageGen::Options imageOptions() const;
	/* 抽帧步长（每 N 帧取 1）。抽掉帧以后每帧该停留的时间要跟着放大，
	 * 否则 30 fps 的 GIF 抽成每 4 帧取 1 会变成 120 fps 一样快。 */
	int frameStep() const;
	int effectiveDelayMs() const;
	/* 「屏」下拉选 32 行时，把图片页的目标尺寸跟到 128×32 */
	void syncImageSizeToScreen();

	/* ---- 素材列表 ---- */
	ImageAsset       *currentAsset();
	const ImageAsset *currentAsset() const;
	void addAssets(const QStringList &paths);   /* 读文件 → 追加一份或合成一份 */
	void selectAsset(int row);                  /* 切换选中，控件跟着换 */
	void removeCurrentAsset();
	void refreshAssetList();                    /* 重建列表项文字 */
	void refreshAssetItem(int row);
	void showAsset(int row);                    /* 把某份素材摆到预览和参数区 */
	void storeWidgetsToAsset(int row);          /* 控件当前值写回素材 */

	/* ---- 烧录自检帧 ---- */
	void showSelfTest();

	/* ---- 一键接入工程 ---- */
	/* 把「驱动头文件补声明」「main.c 补 include 和示例调用」直接做掉。
	 * 只插入不改动，写盘前给预览，每个文件留 .oledbak 可撤销。
	 *
	 * assets = false 接字库（文字页），true 接素材包（图片/动画页）。
	 * 两条路只差「接哪个头文件」和「示例写什么」，其余完全共用。 */
	void mergeIntoProject(bool assets = false);

	/* ---- 屏控制器（决定列偏移） ---- */
	/* 从工程里的驱动源码读出这屏是 SSD1306 还是 SH1106。
	 * 三份导出文件（字库 / 动画 / 自检帧）里的 *_COL_OFFSET 都由它决定。 */
	void reprobeScreen();
	void updateScreenNote();
	int  currentColOffset() const;
	int  currentScreenH() const;

	/* ---- 芯片 Flash 容量 ---- */
	/* 从工程的 .uvprojx（IROM）或链接脚本读容量，读不到退回型号解码。
	 * 放启动和换工程时做，状态栏和图片页的占比都靠它。 */
	void   readChip();
	void   updateChipNote();
	qint64 flashCapacity() const;
	/* 换工程（手动指定、导入字库、导出时临时选目录）之后，把所有"按工程读一次"
	 * 的结论统一重算：屏控制器、芯片容量、状态栏占比、图片页的容量提示。 */
	void   refreshProjectFacts();

	FontGen m_gen;
	QPlainTextEdit *m_text;
	QComboBox *m_fontBox;
	QComboBox *m_sizeBox;           /* 点阵字号 16 / 24 / 32 */
	QLabel *m_subTitle;             /* 副标题：随取反开关在「阴码 / 反白」之间切 */
	QLabel *m_fontWarn;
	QSlider *m_threshold;
	QLabel *m_thresholdValue;
	QCheckBox *m_invert;            /* 点阵取反：屏上反白显示 */
	QCheckBox *m_asciiFont;
	FontPreview *m_preview;
	QLabel *m_status;
	QLabel *m_countLabel;
	QLabel *m_hint;
	QLabel *m_pathLabel;
	QLabel *m_projWarn;             /* 同一层找到多个工程时的提醒，单行 */
	QLabel *m_encStatus;            /* 源码编码检查结果 */
	QCheckBox *m_askEveryTime;      /* 勾上则每次导出都弹对话框选位置 */
	QString m_headerPath;
	/* 上次「全部存进一个 .h」写到哪儿。图片页的一键接入认这个路径，
	 * 省得再让用户选一次。空了就退回设置里存的那份。 */
	QString m_assetsHeaderPath;

	/* 屏控制器探测结果。offset 0 = SSD1306，2 = SH1106（132 列，可见区从第 2 列起） */
	QComboBox          *m_screenBox;
	QComboBox          *m_screenHBox;      /* 屏高 64 / 32，写进字库的 CF_SCREEN_H */
	ScreenProbe::Result m_probe;
	bool                m_probeDone = false;

	/* 芯片 Flash 容量。以前写死 64KB，打开 512KB 的工程就在说假话。
	 * m_chip 是自动读到的结果，m_flashBox 让用户能覆盖它（同一个芯片也可以
	 * 手动把 IROM 改小，这时读到的值仍然对，但用户可能想按更小的算）。 */
	ChipInfo::Result    m_chip;
	QComboBox          *m_flashBox;
	QLabel             *m_flashInfo;

	/* defaultHeaderPath() 是 const 的，但要把"这一层发现的所有工程"带出来
	 * 给界面提示用（只发现一个的话不必多说）。所以是 mutable。 */
	mutable QStringList m_projectCandidates;

	/* 图片 / 动画模式的状态：每份素材各存一份参数，互不影响 */
	QVector<ImageAsset>          m_assets;
	int                          m_imgCurrent = -1;   /* 列表里选中的那一份 */
	QString                      m_imgPath;           /* 最近导入的路径，决定对话框起始目录 */
	ImagePreview                *m_imgPreview;
	QListWidget                 *m_imgList;
	QLabel                      *m_imgFileLabel;
	QLabel                      *m_imgTotal;    /* 列表合计：几份素材一共占多少 Flash */
	QLabel                      *m_imgInfo;
	QComboBox                   *m_imgSize;
	QComboBox                   *m_imgScale;
	QComboBox                   *m_imgStep;     /* 抽帧：每 N 帧取 1 */
	QSlider                     *m_imgThreshold;
	QLabel                      *m_imgThresholdValue;
	QCheckBox                   *m_imgDither;
	QCheckBox                   *m_imgInvert;   /* 取反：黑白对调（反白） */
	QCheckBox                   *m_imgMerge;    /* 多选时合成一个动画（不勾＝各存一份） */
	QPushButton                 *m_imgPlayBtn;
	QTimer                      *m_imgTimer;
};

MainWindow::MainWindow()
	: m_gen(QFont(QStringLiteral("SimSun"), 16), 110)
{
	buildUi();

	/* buildUi 里 m_headerPath 就定下来了，紧接着读芯片容量：
	 * refreshPreview → updateStatus 要用它算占比，顺序反了会先显示一遍 64KB 再改。 */
	readChip();
	refreshPreview();

	/* 界面搭好了再探屏：探测要读工程里的驱动源码，这时才知道工程在哪。
	 * 放在后面而不是导出时，是为了让用户一打开就能看见判断结果。 */
	reprobeScreen();
}

void MainWindow::buildUi()
{
	setWindowTitle(QStringLiteral("OLED 点阵取模工具"));

	/* 890 是 1080p 上刚好不出现滚动条的高度，但副屏常常只有 900 高：
	 * 客户区 890 + 标题栏约 31 = 921，底边会被屏幕切掉，最下面那个按钮点不到。
	 * 所以按当前屏的可用工作区收一下高度，放不下的部分交给滚动区。
	 * 上下限：640 是再小就什么都看不见，890 是"内容本来就够"的上界。 */
	{
		const QScreen *scr = QGuiApplication::primaryScreen();
		const int availH = scr ? scr->availableGeometry().height() : 900;
		resize(1060, qBound(640, availH - 90, 890));
	}

	QWidget *side = new QWidget(this);
	side->setObjectName(QStringLiteral("SidePanel"));
	/* 不要用 setFixedWidth：固定宽度会让分隔线拖不动。
	 * 给个最小宽度让内容不挤，剩下的交给 QSplitter —— 用户可以自由拖动。 */
	side->setMinimumWidth(320);
	side->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

	QVBoxLayout *sideLayout = new QVBoxLayout(side);
	sideLayout->setContentsMargins(16, 16, 16, 12);
	sideLayout->setSpacing(10);

	QLabel *title = new QLabel(QStringLiteral("OLED 点阵取模工具"), side);
	title->setObjectName(QStringLiteral("Title"));
	m_subTitle = new QLabel(side);
	m_subTitle->setObjectName(QStringLiteral("SubTitle"));
	sideLayout->addWidget(title);
	sideLayout->addWidget(m_subTitle);

	/* 四个分组叠起来比 1080p 还高一点，窗口一缩小最下面的按钮就掉出去了。
	 * 所以中间这一段放滚动区；编码状态和字数统计固定在底部，不跟着滚。 */
	QWidget *inner = new QWidget(side);
	QVBoxLayout *innerLayout = new QVBoxLayout(inner);
	innerLayout->setContentsMargins(0, 0, 0, 0);
	innerLayout->setSpacing(10);

	/* ---- 取模设置 ---- */
	QGroupBox *boxSet = new QGroupBox(QStringLiteral("取模设置"), side);
	QFormLayout *form = new QFormLayout(boxSet);
	form->setLabelAlignment(Qt::AlignLeft);
	form->setSpacing(8);

	m_fontBox = new QComboBox(boxSet);
	const QStringList families = QFontDatabase::families();
	const QStringList prefer = { QStringLiteral("SimSun"), QStringLiteral("宋体"),
	                             QStringLiteral("SimHei"), QStringLiteral("黑体"),
	                             QStringLiteral("Microsoft YaHei"), QStringLiteral("微软雅黑") };
	for (int i = 0; i < prefer.size(); ++i)
	{
		if (families.contains(prefer.at(i)))
		{
			m_fontBox->addItem(prefer.at(i));
		}
	}
	for (int i = 0; i < families.size(); ++i)
	{
		if (m_fontBox->findText(families.at(i)) < 0)
		{
			m_fontBox->addItem(families.at(i));
		}
	}
	form->addRow(QStringLiteral("字体"), m_fontBox);

	/* 字号。只放 8 的倍数：编码按页走，一页 8 行，24 和 32 都能整除，
	 * 12x12 那种除不尽，这套格式装不下，所以不提供。 */
	m_sizeBox = new QComboBox(boxSet);
	m_sizeBox->addItem(QStringLiteral("16×16  小字（每字 32 字节）"), 16);
	m_sizeBox->addItem(QStringLiteral("24×24  中字（每字 72 字节）"), 24);
	m_sizeBox->addItem(QStringLiteral("32×32  大字（每字 128 字节）"), 32);
	m_sizeBox->setToolTip(QStringLiteral(
		"点阵越大越清楚，也越占 Flash。\n"
		"16×16 和原本的驱动完全兼容；换成 24/32 后，\n"
		"驱动里的数组名和查表步长会自动变化，用导出文件里的宏即可。"));
	form->addRow(QStringLiteral("字号"), m_sizeBox);

	/* 字体名不存在时 Qt 会静默回退，字形就变了，必须提醒 */
	m_fontWarn = new QLabel(boxSet);
	m_fontWarn->setObjectName(QStringLiteral("Warn"));
	m_fontWarn->setWordWrap(true);
	m_fontWarn->setVisible(false);
	form->addRow(QString(), m_fontWarn);


	QWidget *thrRow = new QWidget(boxSet);
	QHBoxLayout *thrLayout = new QHBoxLayout(thrRow);
	thrLayout->setContentsMargins(0, 0, 0, 0);
	thrLayout->setSpacing(6);
	m_threshold = new QSlider(Qt::Horizontal, thrRow);
	m_threshold->setRange(40, 200);
	m_threshold->setValue(110);
	/* 有些字体/字号下点阵里只有黑白两种值（宋体 16×16 自带点阵位图，
	 * 其余字体小字号改走整像素渲染），这时候没有"边缘灰度"可切，
	 * 滑块拖到底点阵也不会变。不说清楚会被当成坏了。 */
	m_threshold->setToolTip(QStringLiteral(
	    "笔画边缘的灰度门槛：调小让笔画变粗，调大让笔画变细。\n"
	    "字体在这个字号下自带点阵位图（例如宋体 16×16），或汉字是按整像素\n"
	    "渲染的时候，点阵只有黑和白两种值，这个滑块就不参与。"));
	m_thresholdValue = new QLabel(QStringLiteral("110"), thrRow);
	m_thresholdValue->setFixedWidth(28);
	thrLayout->addWidget(m_threshold);
	thrLayout->addWidget(m_thresholdValue);
	QLabel *thrLabel = new QLabel(QStringLiteral("阈值"), boxSet);
	thrLabel->setMinimumWidth(30);        /* 不设的话窄面板里会被滑块压成半截 */
	form->addRow(thrLabel, thrRow);

	/* 取反。作用在成形的点阵上，和阈值是两件独立的事，所以单独一行，
	 * 不塞进阈值那一行里。 */
	m_invert = new QCheckBox(QStringLiteral("取反色（反白显示）"), boxSet);
	m_invert->setToolTip(QStringLiteral(
		"把点阵每一位对调：原来亮的变不亮，屏上就是「亮底暗字」。\n"
		"字的形状、粗细、位置都不变，只是黑白互换 —— 做反白标题条、\n"
		"选中行这类效果时用。\n\n"
		"两点注意：\n"
		"· 空格和缺的字那格会变成一整块亮的（反白时本来就该连成一片）\n"
		"· 字母数字走驱动自带的 ASCII 字库，不在这份数据里，不会跟着反白"));
	form->addRow(QString(), m_invert);

	innerLayout->addWidget(boxSet);

	/* ---- 显示内容 ---- */
	QGroupBox *boxText = new QGroupBox(QStringLiteral("① 要显示的文字（在这里加字）"), side);
	QVBoxLayout *textLayout = new QVBoxLayout(boxText);
	m_text = new QPlainTextEdit(boxText);
	m_text->setPlaceholderText(QStringLiteral("直接打字，或点下面的「导入已有字库」"));
	m_text->setPlainText(QStringLiteral("嵌入式开发网中文测试你好世界温度湿度"));
	m_text->setFixedHeight(88);
	textLayout->addWidget(m_text);

	/* 字数就地显示在输入框下面，改一个字立刻能看到 */
	m_countLabel = new QLabel(boxText);
	m_countLabel->setObjectName(QStringLiteral("Count"));
	/* 必须能换行，否则这一整行的最小宽度会把滚动区内容撑得比视口还宽，
	 * 右边一截被切掉（横向滚动条是关的），看起来像字被截断。 */
	m_countLabel->setWordWrap(true);
	m_countLabel->setMinimumWidth(1);
	textLayout->addWidget(m_countLabel);

	m_hint = new QLabel(boxText);
	m_hint->setObjectName(QStringLiteral("Hint"));
	m_hint->setWordWrap(true);
	m_hint->setMinimumWidth(1);
	m_hint->setText(QStringLiteral("· 改文字就加字，右边实时预览\n"
	                               "· 「导入已有字库」= 读回并合并，不覆盖\n"
	                               "· 字母数字走 ASCII；缺字显示空白，不花屏"));
	textLayout->addWidget(m_hint);
	innerLayout->addWidget(boxText);

	/* ---- 导出 ---- */
	QGroupBox *boxOut = new QGroupBox(QStringLiteral("导出"), side);
	QVBoxLayout *outLayout = new QVBoxLayout(boxOut);

	/* 屏控制器决定列偏移：SSD1306 显存 128 列，列地址就从 0 开始；SH1106 显存
	 * 132 列、可见区从第 2 列开始，每条列地址都要 +2，否则整幅左移 2 像素、
	 * 右边少两列。这个差别不体现在数据里，只体现在写哪个列地址。
	 *
	 * 以前要用户自己翻驱动、再把三份导出文件里的宏各改一遍（漏改一处的表现
	 * 还各不相同）。现在从驱动源码里读，读不准也能手动指定。 */
	QHBoxLayout *screenRow = new QHBoxLayout();
	screenRow->setContentsMargins(0, 0, 0, 0);
	screenRow->setSpacing(6);
	QLabel *screenLabel = new QLabel(QStringLiteral("屏"), boxOut);
	screenLabel->setMinimumWidth(20);
	m_screenBox = new QComboBox(boxOut);
	m_screenBox->addItem(QStringLiteral("自动判断"), 0);
	m_screenBox->addItem(QStringLiteral("SSD1306 · 偏移 0"), 1);
	m_screenBox->addItem(QStringLiteral("SH1106 · 偏移 2"), 2);
	m_screenBox->setToolTip(QStringLiteral(
		"决定导出文件里 CF_COL_OFFSET / ANIM_COL_OFFSET / OLED_SELFTEST_COL_OFFSET 的值。\n"
		"自动判断：在工程里找驱动源码，看设列地址的地方有没有多加了 2、"
		"有没有写型号。\n"
		"判断错了的表现：整幅左移 2 像素、右边框缺两列 —— 手动选另一项即可。"));

	/* 屏高只影响字库：CF_SCREEN_H 决定"第几行还放得下"。
	 * 128×32 的 0.91" 屏只有 4 页，按 64 算会往不存在的页里写。 */
	m_screenHBox = new QComboBox(boxOut);
	m_screenHBox->addItem(QStringLiteral("64 行"), 64);
	m_screenHBox->addItem(QStringLiteral("32 行"), 32);
	m_screenHBox->setFixedWidth(70);
	m_screenHBox->setToolTip(QStringLiteral(
		"屏的像素高度。0.96\" 是 128×64（64 行），0.91\" 是 128×32（32 行）。\n"
		"选 32 时导出的字库会把 CF_SCREEN_H 写成 32，"
		"一行 16×16 最多显示 2 行，24×24 超过 1 行就自动跳过不写。"));

	screenRow->addWidget(screenLabel);
	screenRow->addWidget(m_screenBox, 1);
	screenRow->addWidget(m_screenHBox);
	outLayout->addLayout(screenRow);

	/* 明确停在「自动判断」。updateScreenNote 会改第 0 项的文字，
	 * 万一哪次改动让它落到别的项上，用户会以为工具擅自选了手动值。 */
	m_screenBox->setCurrentIndex(0);
	connect(m_screenBox, &QComboBox::currentIndexChanged, this,
	        &MainWindow::updateScreenNote);

	/* 屏高变了，图片页的目标尺寸也要跟过去（两张页面的尺寸是同一件事） */
	connect(m_screenHBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
	        this, [this](int) { syncImageSizeToScreen(); });

	m_asciiFont = new QCheckBox(QStringLiteral("统计时包含 ASCII 字库(1520B)"), boxOut);
	m_asciiFont->setChecked(true);
	outLayout->addWidget(m_asciiFont);

	/* 路径和「换个目录」并排一行。原来按钮单独占一行，白吃 32 像素，
	 * 而这个按钮一年也用不上一次 —— 缩成一个小按钮跟在路径后面就够。 */
	QHBoxLayout *pathRow = new QHBoxLayout();
	pathRow->setSpacing(6);
	m_pathLabel = new QLabel(boxOut);
	m_pathLabel->setObjectName(QStringLiteral("PathLabel"));
	/* 只显示路径末两级，不用换行。完整路径在 tooltip 里。 */
	m_pathLabel->setWordWrap(false);
	pathRow->addWidget(m_pathLabel, 1);

	/* 自动定位总有猜不准的时候（工程在别的盘、用了不常见的目录结构）。
	 * 与其让用户对着"未找到工程"发愁，不如给一个明确的入口自己指一次。 */
	QPushButton *projBtn = new QPushButton(QStringLiteral("更改"), boxOut);
	projBtn->setToolTip(QStringLiteral(
		"工具靠工程文件（.uvprojx / .ewp / .cproject 等）自动找工程。\n"
		"找不到、或找到了别的工程时，点这里手动指定一次，之后会记住。"));
	pathRow->addWidget(projBtn);
	outLayout->addLayout(pathRow);
	connect(projBtn, &QPushButton::clicked, this, &MainWindow::chooseProjectDir);

	/* 同一层有多个工程时，默认值是挑出来的，必须说出来 ——
	 * 暗中选择又不提示，用户会发现字库莫名其妙进了别的工程。
	 * 单独一行、只写结论，名单塞进 tooltip。 */
	m_projWarn = new QLabel(boxOut);
	m_projWarn->setObjectName(QStringLiteral("Warn"));
	m_projWarn->setWordWrap(true);
	m_projWarn->setMinimumWidth(1);
	m_projWarn->setVisible(false);
	outLayout->addWidget(m_projWarn);

	/* 默认不勾：正常情况下一键导出，不打扰。
	 * 勾上后每次导出都弹对话框，方便导出到别处。 */
	m_askEveryTime = new QCheckBox(QStringLiteral("每次导出都询问保存位置"), boxOut);
	m_askEveryTime->setChecked(false);
	outLayout->addWidget(m_askEveryTime);

	QPushButton *exportBtn = new QPushButton(QStringLiteral("导出 OLED_CFont.h"), boxOut);
	exportBtn->setObjectName(QStringLiteral("Primary"));
	QPushButton *importBtn = new QPushButton(QStringLiteral("导入已有字库"), boxOut);
	QPushButton *saveAsBtn = new QPushButton(QStringLiteral("另存为…"), boxOut);

	outLayout->addWidget(exportBtn);
	QHBoxLayout *btnRow = new QHBoxLayout();
	btnRow->addWidget(importBtn);
	btnRow->addWidget(saveAsBtn);
	outLayout->addLayout(btnRow);

	/* 自检帧和字库是两条独立的路：字库验证"这个字对不对"，自检帧验证
	 * "工具吐出来的字节和你的驱动是不是同一套"。后者是唯一能一眼看穿的验收手段，
	 * 所以单独给一个入口，不和导出字库混在一起。 */
	QPushButton *selfTestBtn = new QPushButton(QStringLiteral("烧录自检帧…"), boxOut);
	selfTestBtn->setToolTip(QStringLiteral(
		"生成一张整屏图（带显示函数），烧进板子刷一次。\n"
		"128×64 和 128×32 两种屏都能选。\n"
		"四边完整、四角各不同、中间有字 = 格式对上了。\n"
		"这是确认取模结果和驱动匹配最快的办法。"));
	outLayout->addWidget(selfTestBtn);
	connect(selfTestBtn, &QPushButton::clicked, this, &MainWindow::showSelfTest);

	/* 导出完字库，用户手里还是"一坨数据"。这一步把它接进工程：
	 * 驱动头文件补声明、main.c 补 include 和示例。省掉手工改文件的活。 */
	QPushButton *mergeBtn = new QPushButton(QStringLiteral("一键接入工程…"), boxOut);
	mergeBtn->setToolTip(QStringLiteral(
		"自动在驱动头文件里补上写命令/写数据的声明，"
		"在 main.c 里补 #include 和两行示例调用。\n"
		"只插入，不改你已有的任何一行；写盘前会给你看改什么，"
		"每个文件留一份 .oledbak 随时能撤销。"));
	outLayout->addWidget(mergeBtn);
	connect(mergeBtn, &QPushButton::clicked, this, &MainWindow::mergeIntoProject);

	innerLayout->addWidget(boxOut);
	innerLayout->addStretch(1);

	QScrollArea *sideScroll = new QScrollArea(side);
	sideScroll->setWidget(inner);
	sideScroll->setWidgetResizable(true);
	sideScroll->setFrameShape(QFrame::NoFrame);
	sideScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	sideLayout->addWidget(sideScroll, 1);

	/* 源码编码检查结果。
	 * 单独放一个标签，因为它关系到"字库能不能被查到"，比普通状态重要。
	 * 放在滚动区外面：窗口再小也要能一眼看到。 */
	m_encStatus = new QLabel(side);
	m_encStatus->setObjectName(QStringLiteral("Hint"));
	m_encStatus->setWordWrap(true);
	m_encStatus->setMinimumWidth(1);
	sideLayout->addWidget(m_encStatus);

	m_status = new QLabel(side);
	m_status->setObjectName(QStringLiteral("Hint"));
	/* 必须开换行：状态文字比较长（例如"已导入 17 个汉字并与原文合并（原有 17 个，现共 17 个）"），
	 * 不开的话超出面板宽度的部分会被直接切掉，看起来像显示不全。 */
	m_status->setWordWrap(true);
	m_status->setMinimumWidth(1);      /* 允许被压缩，配合上面的换行才不会撑开面板 */

	/* 第二行：芯片 + 容量 + 占比。容量可以手动改 ——
	 * 自动读的是链接器里的 IROM，多数情况不用管；但有人会把 IROM 改小
	 * （留一半给 bootloader），或者工程文件读不出来，这时就需要能自己选。 */
	QHBoxLayout *flashRow = new QHBoxLayout();
	flashRow->setSpacing(6);
	flashRow->setContentsMargins(0, 0, 0, 0);
	m_flashInfo = new QLabel(side);
	m_flashInfo->setObjectName(QStringLiteral("Hint"));
	m_flashInfo->setWordWrap(true);
	m_flashInfo->setMinimumWidth(1);
	flashRow->addWidget(m_flashInfo, 1);

	m_flashBox = new QComboBox(side);
	m_flashBox->addItem(QStringLiteral("自动"), 0);
	m_flashBox->addItem(QStringLiteral("32 KB"),   32);
	m_flashBox->addItem(QStringLiteral("64 KB"),   64);
	m_flashBox->addItem(QStringLiteral("128 KB"), 128);
	m_flashBox->addItem(QStringLiteral("256 KB"), 256);
	m_flashBox->addItem(QStringLiteral("512 KB"), 512);
	m_flashBox->addItem(QStringLiteral("1 MB"),  1024);
	m_flashBox->setToolTip(QStringLiteral(
		"手动指定芯片的 Flash 容量，用来算「字库占多少」。\n"
		"默认「自动」= 读工程里的 IROM（链接器实际用的 ROM 大小）。"));
	/* 下拉只显示「自动 / 32 KB / …」，具体读到的容量在左边那行里，
	 * 这里再写一遍反而被截成「自动 64」。 */
	m_flashBox->setFixedWidth(76);
	m_flashBox->setFixedHeight(22);
	flashRow->addWidget(m_flashBox);

	/* 两行贴在一起：中间按 10px 走的话，底部这一块会比改造前高 8px，
	 * 面板刚好挤出竖直滚动条（上面几十个像素是省出来的，不能又还回去）。 */
	QVBoxLayout *statusBox = new QVBoxLayout();
	statusBox->setContentsMargins(0, 0, 0, 0);
	statusBox->setSpacing(2);
	statusBox->addWidget(m_status);
	statusBox->addLayout(flashRow);
	sideLayout->addLayout(statusBox);
	connect(m_flashBox, &QComboBox::currentIndexChanged, this, [this]() {
		updateStatus();
		updateImageInfo();          /* 图片页那句「放不下几秒」也跟着变 */
	});

	/* ---- 右侧预览：套一层滚动区域，字多了 / 窗口缩小了都能滚 ---- */
	m_preview = new FontPreview(this);
	m_preview->setGenerator(&m_gen);

	QScrollArea *scroll = new QScrollArea(this);
	scroll->setWidget(m_preview);
	scroll->setWidgetResizable(true);          /* 宽度跟随窗口，高度由 heightForWidth 决定 */
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	scroll->setFrameShape(QFrame::NoFrame);

	QSplitter *splitter = new QSplitter(Qt::Horizontal, this);
	splitter->addWidget(side);
	splitter->addWidget(scroll);
	splitter->setStretchFactor(0, 0);
	splitter->setStretchFactor(1, 1);
	splitter->setChildrenCollapsible(false);
	splitter->setSizes(QList<int>() << 360 << 700);

	/* 两种模式并排放进标签页：文字模式是原来那套界面，一个字都不动；
	 * 图片/动画模式是独立一页，互不影响。 */
	QTabWidget *tabs = new QTabWidget(this);
	tabs->addTab(splitter, QStringLiteral("文字"));
	tabs->addTab(buildImagePage(), QStringLiteral("图片 / 动画"));
	setCentralWidget(tabs);

	m_headerPath = defaultHeaderPath();
	if (m_headerPath.isEmpty())
	{
		m_pathLabel->setText(QStringLiteral("未找到工程目录"));
		m_pathLabel->setToolTip(QStringLiteral(
			"没在本工具附近找到 .uvprojx / .ewp / .cproject。\n"
			"导出时会让你选一次工程目录，或点右边「更改」。"));
	}
	else
	{
		m_pathLabel->setText(QStringLiteral("导出到：%1").arg(tailPath(m_headerPath, 2)));
		m_pathLabel->setToolTip(m_headerPath);

		/* 同一层有多个工程时，默认值是挑出来的，必须说出来。
		 * 暗中选择又不提示 —— 用户会发现字库莫名其妙进了别的工程。 */
		if (m_projectCandidates.size() > 1 && m_projWarn)
		{
			QStringList names;
			QString used;
			for (const QString &p : m_projectCandidates)
			{
				names << QFileInfo(p).fileName();
				if (m_headerPath.startsWith(QDir::cleanPath(p) + QLatin1Char('/')))
				{
					used = QFileInfo(p).fileName();
				}
			}
			m_projWarn->setText(QStringLiteral("⚠ 同层 %1 个工程，默认 %2")
			                    .arg(m_projectCandidates.size())
			                    .arg(used.isEmpty() ? QStringLiteral("一个") : used));
			m_projWarn->setToolTip(QStringLiteral("这一层找到的工程：%1\n"
			                                      "要换成别的，点上面的「更改」。")
			                       .arg(names.join(QStringLiteral("、"))));
			m_projWarn->setVisible(true);
		}
	}

	/* 启动时做一次源码编码检查（不弹窗，只写进状态标签） */
	checkSourceEncoding(false);

	connect(m_text, &QPlainTextEdit::textChanged, this, &MainWindow::refreshPreview);
	connect(m_fontBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
	        this, &MainWindow::refreshPreview);
	connect(m_sizeBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
	        this, &MainWindow::refreshPreview);
	connect(m_threshold, &QSlider::valueChanged, this, [this](int v) {
		m_thresholdValue->setText(QString::number(v));
		refreshPreview();
	});
	/* 取反只影响成形后的点阵，字体/字号/阈值都不用重算，走同一条刷新路径即可 */
	connect(m_invert, &QCheckBox::toggled, this, &MainWindow::refreshPreview);
	connect(m_asciiFont, &QCheckBox::toggled, this, &MainWindow::updateStatus);
	connect(exportBtn, &QPushButton::clicked, this, &MainWindow::exportHeader);
	connect(importBtn, &QPushButton::clicked, this, &MainWindow::importHeader);
	connect(saveAsBtn, &QPushButton::clicked, this, [this]() {
		const QString path = QFileDialog::getSaveFileName(
			this, QStringLiteral("另存为"), m_headerPath,
			QStringLiteral("C 头文件 (*.h)"));
		if (!path.isEmpty())
		{
			m_headerPath = path;
			/* 存到别处可能就换到另一个工程了，屏型号和容量要跟着重判 */
			refreshProjectFacts();
			/* 传 false：路径刚在这里选过，exportHeader 不要再弹对话框 */
			exportHeader(false);
		}
	});
}

/* ==========================================================================
 *  图片 / 动画模式
 * ========================================================================== */

QWidget *MainWindow::buildImagePage()
{
	QWidget *side = new QWidget(this);
	side->setObjectName(QStringLiteral("SidePanel"));
	side->setMinimumWidth(300);
	side->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

	QVBoxLayout *sideLayout = new QVBoxLayout(side);
	sideLayout->setContentsMargins(16, 16, 16, 12);
	sideLayout->setSpacing(10);

	QLabel *title = new QLabel(QStringLiteral("图片 / 动画取模"), side);
	title->setObjectName(QStringLiteral("Title"));
	sideLayout->addWidget(title);

	QGroupBox *boxFile = new QGroupBox(QStringLiteral("素材"), side);
	QVBoxLayout *fileLayout = new QVBoxLayout(boxFile);

	QPushButton *openBtn = new QPushButton(QStringLiteral("添加图片 / 动图…"), boxFile);
	fileLayout->addWidget(openBtn);

	/* 一次选多个文件时怎么处理，交给用户决定：
	 *   勾上  → 当成一个动画的连续帧（原有用法：一堆 PNG 合成动画）
	 *   不勾  → 每个文件单独占一条，各导各的头文件（工程里放 logo、图标）
	 * 默认不勾 —— 图片页本来就该能同时存好几份素材，
	 * 想合动画的是少数场景，勾一下很快。 */
	m_imgMerge = new QCheckBox(QStringLiteral("多选时合成一个动画"), boxFile);
	m_imgMerge->setToolTip(QStringLiteral(
		"勾上：一次选中的多张图片按文件名顺序拼成一段动画（frame2 排在 frame10 前）。\n"
		"不勾：每个文件各自成为一条素材，分别预览、分别设置、分别导出，互不影响。\n"
		"GIF 无论勾不勾都按它自己的多帧处理。"));
	fileLayout->addWidget(m_imgMerge);

	/* 素材列表：改参数、导出都只作用于选中的那条 */
	m_imgList = new QListWidget(boxFile);
	m_imgList->setObjectName(QStringLiteral("AssetList"));
	m_imgList->setMinimumHeight(84);
	m_imgList->setMaximumHeight(132);
	m_imgList->setAlternatingRowColors(false);
	m_imgList->setSelectionMode(QAbstractItemView::SingleSelection);
	fileLayout->addWidget(m_imgList);

	QHBoxLayout *listBtnRow = new QHBoxLayout();
	QPushButton *removeBtn = new QPushButton(QStringLiteral("移除"), boxFile);
	QPushButton *clearBtn  = new QPushButton(QStringLiteral("清空"), boxFile);
	removeBtn->setToolTip(QStringLiteral("删掉列表里选中的这一份"));
	clearBtn->setToolTip(QStringLiteral("删掉全部素材"));
	listBtnRow->addWidget(removeBtn);
	listBtnRow->addWidget(clearBtn);
	listBtnRow->addStretch(1);
	fileLayout->addLayout(listBtnRow);

	/* 列表合计：单独占一行，紧贴着列表 —— 一次导入十来张图的时候，
	 * 要回答的是"这些一共占多少"，而不是"选中的这一张占多少"。
	 * 只有一份素材时不显示：那一份的数字下面的详情里已经写了，
	 * 再挂一行一模一样的数会让人以为算了两次。 */
	m_imgTotal = new QLabel(boxFile);
	m_imgTotal->setObjectName(QStringLiteral("Hint"));
	m_imgTotal->setWordWrap(true);
	m_imgTotal->setMinimumWidth(1);
	fileLayout->addWidget(m_imgTotal);

	m_imgFileLabel = new QLabel(QStringLiteral("（还没有素材）"), boxFile);
	m_imgFileLabel->setObjectName(QStringLiteral("PathLabel"));
	m_imgFileLabel->setWordWrap(true);
	fileLayout->addWidget(m_imgFileLabel);

	sideLayout->addWidget(boxFile);

	QGroupBox *boxOpt = new QGroupBox(QStringLiteral("取模设置"), side);
	QFormLayout *form = new QFormLayout(boxOpt);

	m_imgSize = new QComboBox(boxOpt);
	m_imgSize->addItem(QStringLiteral("128 × 64（0.96\" 全屏）"), QPoint(128, 64));
	m_imgSize->addItem(QStringLiteral("128 × 32（0.91\" 屏）"),   QPoint(128, 32));
	m_imgSize->addItem(QStringLiteral("64 × 64"),                 QPoint(64, 64));
	m_imgSize->addItem(QStringLiteral("32 × 32"),                 QPoint(32, 32));
	m_imgSize->addItem(QStringLiteral("16 × 16"),                 QPoint(16, 16));
	form->addRow(QStringLiteral("目标尺寸"), m_imgSize);

	m_imgScale = new QComboBox(boxOpt);
	m_imgScale->addItem(QStringLiteral("等比缩放 + 留白"), 0);
	m_imgScale->addItem(QStringLiteral("拉伸填满"),       1);
	form->addRow(QStringLiteral("缩放方式"), m_imgScale);

	m_imgStep = new QComboBox(boxOpt);
	m_imgStep->addItem(QStringLiteral("不抽帧"),   1);
	m_imgStep->addItem(QStringLiteral("每 2 帧取 1"), 2);
	m_imgStep->addItem(QStringLiteral("每 3 帧取 1"), 3);
	m_imgStep->addItem(QStringLiteral("每 4 帧取 1"), 4);
	m_imgStep->addItem(QStringLiteral("每 6 帧取 1"), 6);
	m_imgStep->addItem(QStringLiteral("每 8 帧取 1"), 8);
	form->addRow(QStringLiteral("抽帧"), m_imgStep);

	QHBoxLayout *thrRow = new QHBoxLayout();
	m_imgThreshold = new QSlider(Qt::Horizontal, boxOpt);
	m_imgThreshold->setRange(0, 255);
	m_imgThreshold->setValue(128);
	m_imgThresholdValue = new QLabel(QStringLiteral("128"), boxOpt);
	m_imgThresholdValue->setMinimumWidth(36);
	thrRow->addWidget(m_imgThreshold);
	thrRow->addWidget(m_imgThresholdValue);
	form->addRow(QStringLiteral("亮度阈值"), thrRow);

	m_imgDither = new QCheckBox(QStringLiteral("抖动（照片/渐变建议开）"), boxOpt);
	m_imgDither->setChecked(true);
	form->addRow(QString(), m_imgDither);

	m_imgInvert = new QCheckBox(QStringLiteral("取反色（反白显示）"), boxOpt);
	m_imgInvert->setToolTip(QStringLiteral(
	    "把整幅图每一位对调：原来亮的地方不亮，屏上是亮底暗图。\n"
	    "和阈值、抖动互不影响，是在它们之后统一做的。\n"
	    "logo、图标这类深色背景的图，反白后更省电、也更清楚。"));
	form->addRow(QString(), m_imgInvert);

	sideLayout->addWidget(boxOpt);

	m_imgInfo = new QLabel(side);
	m_imgInfo->setObjectName(QStringLiteral("Hint"));
	m_imgInfo->setWordWrap(true);
	m_imgInfo->setMinimumWidth(1);
	sideLayout->addWidget(m_imgInfo);

	sideLayout->addStretch(1);

	QHBoxLayout *btnRow = new QHBoxLayout();
	m_imgPlayBtn = new QPushButton(QStringLiteral("播放"), side);
	QPushButton *exportBtn = new QPushButton(QStringLiteral("导出这份 .h"), side);
	exportBtn->setObjectName(QStringLiteral("Primary"));
	exportBtn->setToolTip(QStringLiteral("只导出列表里选中的这一份素材"));
	btnRow->addWidget(m_imgPlayBtn);
	btnRow->addWidget(exportBtn);
	sideLayout->addLayout(btnRow);

	QPushButton *exportAllBtn = new QPushButton(QStringLiteral("全部导出…（每份一个 .h）"), side);
	exportAllBtn->setToolTip(QStringLiteral(
		"选一个目录，列表里每份素材各写一个 <名字>.h。\n"
		"文件名和里面的宏、数组名都按素材名区分，可以同时 include 进一个工程。"));
	sideLayout->addWidget(exportAllBtn);

	QPushButton *exportPackBtn = new QPushButton(QStringLiteral("全部存进一个 .h…"), side);
	exportPackBtn->setToolTip(QStringLiteral(
		"把列表里所有素材写进同一个头文件，但每份仍然是独立的宏、数组和显示函数\n"
		"（不合成一个动画）。每份点阵上方会注明导入时的文件名。"));
	sideLayout->addWidget(exportPackBtn);

	/* 和文字页那个「一键接入工程」是同一套（补声明 / 补 include / 补示例 / 可撤销），
	 * 只是接的是素材包头，示例换成「刷第 0 帧」 */
	QPushButton *mergeAssetBtn = new QPushButton(QStringLiteral("一键接入工程…"), side);
	mergeAssetBtn->setToolTip(QStringLiteral(
		"把上次导出的素材包头接进工程：驱动头补函数声明、main.c 补 #include，\n"
		"再补一行「刷第 0 帧」的调用（插在 OLED_Init 之后，不动 while 循环）。\n"
		"只插入不改已有行，写盘前能逐条预览，每个文件留 .oledbak 可撤销。"));
	sideLayout->addWidget(mergeAssetBtn);

	/* 右侧预览 */
	m_imgPreview = new ImagePreview(this);
	QScrollArea *scroll = new QScrollArea(this);
	scroll->setWidget(m_imgPreview);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setMinimumWidth(320);

	QSplitter *splitter = new QSplitter(Qt::Horizontal, this);
	splitter->addWidget(side);
	splitter->addWidget(scroll);
	splitter->setStretchFactor(0, 0);
	splitter->setStretchFactor(1, 1);
	splitter->setChildrenCollapsible(false);
	splitter->setSizes(QList<int>() << 340 << 720);

	m_imgTimer = new QTimer(this);
	connect(m_imgTimer, &QTimer::timeout, m_imgPreview, &ImagePreview::showNextFrame);

	connect(openBtn, &QPushButton::clicked, this, &MainWindow::loadImageFile);
	connect(exportBtn, &QPushButton::clicked, this, &MainWindow::exportAnimHeader);
	connect(exportAllBtn, &QPushButton::clicked, this, &MainWindow::exportAllAssets);
	connect(exportPackBtn, &QPushButton::clicked, this, &MainWindow::exportAssetsToSingleFile);
	connect(mergeAssetBtn, &QPushButton::clicked, this, [this] { mergeIntoProject(true); });
	connect(m_imgPlayBtn, &QPushButton::clicked, this, &MainWindow::togglePlayback);
	connect(removeBtn, &QPushButton::clicked, this, &MainWindow::removeCurrentAsset);
	connect(clearBtn, &QPushButton::clicked, this, [this] {
		if (m_assets.isEmpty())
		{
			return;
		}
		const auto ret = QMessageBox::question(
			this, QStringLiteral("清空素材"),
			QStringLiteral("把 %1 份素材全部删掉？文件本身不会动。").arg(m_assets.size()),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		if (ret != QMessageBox::Yes)
		{
			return;
		}
		m_assets.clear();
		m_imgCurrent = -1;
		m_imgList->clear();
		m_imgPreview->clearFrames();
		m_imgFileLabel->setText(QStringLiteral("（还没有素材）"));
		updateImageInfo();
	});

	/* 切换素材时先把当前控件值写回上一份（否则改完参数点走一下就丢了），
	 * 再按新选中那份的参数重摆控件。 */
	connect(m_imgList, &QListWidget::currentRowChanged, this, [this](int row) {
		selectAsset(row);
	});

	/* 参数一改就落到当前素材上，然后只重算这一份 */
	auto onParamChanged = [this] {
		storeWidgetsToAsset(m_imgCurrent);
		recomputeImage();
	};
	connect(m_imgSize, QOverload<int>::of(&QComboBox::currentIndexChanged),
	        this, [onParamChanged](int) { onParamChanged(); });
	connect(m_imgScale, QOverload<int>::of(&QComboBox::currentIndexChanged),
	        this, [onParamChanged](int) { onParamChanged(); });
	connect(m_imgStep, QOverload<int>::of(&QComboBox::currentIndexChanged),
	        this, [onParamChanged](int) { onParamChanged(); });
	connect(m_imgDither, &QCheckBox::toggled, this, [onParamChanged](bool) { onParamChanged(); });
	connect(m_imgInvert, &QCheckBox::toggled, this, [onParamChanged](bool) { onParamChanged(); });
	connect(m_imgThreshold, &QSlider::valueChanged, this, [this, onParamChanged](int v) {
		m_imgThresholdValue->setText(QString::number(v));
		onParamChanged();
	});

	/* 初始尺寸跟着「屏」下拉走（0.91" 屏时默认就是 128×32） */
	syncImageSizeToScreen();

	updateImageInfo();

	return splitter;
}

ImageGen::Options MainWindow::imageOptions() const
{
	ImageGen::Options opt;
	const QPoint sz = m_imgSize->currentData().toPoint();
	opt.width     = sz.x();
	opt.height    = sz.y();
	opt.stretch   = (m_imgScale->currentData().toInt() == 1);
	opt.threshold = m_imgThreshold->value();
	opt.dither    = m_imgDither->isChecked() ? ImageGen::Bayer : ImageGen::NoDither;
	opt.invert    = m_imgInvert->isChecked();
	opt.colOffset = currentColOffset();
	return opt;
}

int MainWindow::frameStep() const
{
	return m_imgStep ? qMax(1, m_imgStep->currentData().toInt()) : 1;
}

/* 一帧该停留多久。
 * 抽帧把 4 帧合成 1 帧，这 1 帧就得停 4 倍时间，否则整体播放速度会变成 4 倍。
 * 原来导出时直接写 GIF 的原始延时，抽帧越多动画越快 —— 帧数和时长都对不上，
 * 而用户看到的是"导出以后动画快得离谱"，很难联想到是抽帧这一步。 */
int MainWindow::effectiveDelayMs() const
{
	const ImageAsset *a = currentAsset();
	const int base = (a && a->delayMs > 0) ? a->delayMs : 100;
	return qMax(1, base) * frameStep();
}

/* 「屏」下拉在文字页，图片页的目标尺寸在另一页，两边本来就该一致。
 * 0.91" 的屏是 128×32，图片页却固定默认 128×64，导出的 anim.h 会往后
 * 4 页灌数据 —— 烧进去只显示上半截，看着像"驱动少写了半屏"。
 * 所以选 32 行时把图片页尺寸跟过去。
 * 只在当前正好是另一档 128 宽时才跟：用户特意选了 64×64 / 16×16 是在做图标，
 * 不该被抢掉。 */
void MainWindow::syncImageSizeToScreen()
{
	if (!m_imgSize)
	{
		return;
	}
	const QPoint want = (currentScreenH() == 32) ? QPoint(128, 32) : QPoint(128, 64);
	const QPoint cur  = m_imgSize->currentData().toPoint();
	if (cur == want || cur.x() != 128)
	{
		return;
	}
	const int idx = m_imgSize->findData(want);
	if (idx >= 0)
	{
		m_imgSize->setCurrentIndex(idx);     /* 会触发 recomputeImage */
	}
}

/* 一份素材的说明文字：文件名、帧数、原尺寸，加上导入时的提醒。
 * 列表里只放名字和关键数字，详细内容摆在这里。 */
static QString assetDetailText(const ImageAsset &a)
{
	/* 帧数报"实际会导出的帧数"，不是原始帧数。
	 * 抽帧以后两者不一样，而列表那行显示的是导出帧数 ——
	 * 这里再报原始帧数，用户会看到"列表 2 帧、详情 4 帧"这种自相矛盾。
	 * 抽帧时把原始帧数补在后面，用户才知道少的帧去哪了。 */
	const int shown = a.packed.isEmpty() ? a.source.size() : a.packed.size();
	const QString stepNote =
		(a.step > 1 && shown != a.source.size())
			? QStringLiteral("；原始 %1 帧，每 %2 帧取 1").arg(a.source.size()).arg(a.step)
			: QString();

	QString s;
	if (a.files.isEmpty())
	{
		s = a.name;
	}
	else if (a.files.size() == 1)
	{
		s = QStringLiteral("%1\n（%2 帧，原尺寸 %3×%4%5）")
		        .arg(QFileInfo(a.files.first()).fileName())
		        .arg(shown)
		        .arg(a.source.isEmpty() ? 0 : a.source.first().width())
		        .arg(a.source.isEmpty() ? 0 : a.source.first().height())
		        .arg(stepNote);
	}
	else
	{
		/* 多文件时尺寸可能各不相同，报第一张就行，别编一个"平均尺寸" */
		s = QStringLiteral("%1 等 %2 个文件\n（按文件名顺序 → 共 %3 帧，首帧 %4×%5%6）")
		        .arg(QFileInfo(a.files.first()).fileName())
		        .arg(a.files.size())
		        .arg(shown)
		        .arg(a.source.isEmpty() ? 0 : a.source.first().width())
		        .arg(a.source.isEmpty() ? 0 : a.source.first().height())
		        .arg(stepNote);
	}
	if (!a.note.isEmpty())
	{
		s += QLatin1Char('\n') + a.note;
	}
	return s;
}

/* 字节数说成人话。素材合计常见是几百字节到几十 KB：
 * 一律报 KB 会出现「0.4 KB」这种不好比的数，一律报字节又长又难读。 */
static QString byteSizeText(qint64 bytes)
{
	if (bytes < 1024)
	{
		return QStringLiteral("%1 字节").arg(bytes);
	}
	if (bytes < 1024 * 1024)
	{
		return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
	}
	return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 2);
}

/* 导入时被截断或读不出文件，要说清楚为什么。
 * 只说"只读了前 N 帧"而不说原因，用户会以为文件本身就这么几帧。 */
static QString batchNote(const ImageGen::FrameBatch &batch)
{
	QString note;
	if (batch.truncated)
	{
		QStringList why;
		if (batch.cappedByCount)
		{
			why << QStringLiteral("达到 %1 帧上限").arg(batch.limitFrames);
		}
		if (batch.cappedByMemory)
		{
			why << QStringLiteral("内存预算不够");
		}
		if (why.isEmpty())
		{
			why << QStringLiteral("超出可用上限");
		}
		note = QStringLiteral("⚠ 只收了前 %1 帧：%2")
		           .arg(batch.frames.size())
		           .arg(why.join(QStringLiteral("，")));
		if (batch.skippedFiles > 0)
		{
			note += QStringLiteral("（另有 %1 个文件没读）").arg(batch.skippedFiles);
		}
	}
	if (!batch.failed.isEmpty())
	{
		if (!note.isEmpty())
		{
			note += QLatin1Char('\n');
		}
		note += QStringLiteral("⚠ 有 %1 个文件读不出图像：%2")
		            .arg(batch.failed.size())
		            .arg(batch.failed.join(QStringLiteral("、")));
	}
	return note;
}

/* 按素材自己的参数重算预览帧和导出字节 */
static void rebuildAsset(ImageAsset &a)
{
	a.mono.clear();
	a.packed.clear();

	const int step = qMax(1, a.step);
	for (int i = 0; i < a.source.size(); i += step)
	{
		const QVector<quint8> bytes = ImageGen::convert(a.source.at(i), a.opt);
		a.packed.append(bytes);
		/* 预览用"从显存字节还原出来的图"，保证看到的就是要烧进去的数据 */
		a.mono.append(ImageGen::unpackFromDisplay(bytes, a.opt.width, a.opt.height));
	}
}

ImageAsset *MainWindow::currentAsset()
{
	if (m_imgCurrent < 0 || m_imgCurrent >= m_assets.size())
	{
		return nullptr;
	}
	return &m_assets[m_imgCurrent];
}

const ImageAsset *MainWindow::currentAsset() const
{
	if (m_imgCurrent < 0 || m_imgCurrent >= m_assets.size())
	{
		return nullptr;
	}
	return &m_assets.at(m_imgCurrent);
}

void MainWindow::loadImageFile()
{
	/* 起始目录：没导入过就传空，让系统用它记住的上次目录。
	 * 一次永远允许多选 —— 选几个由用户定，怎么处理由「合成一个动画」决定。 */
	const QString startDir = m_imgPath.isEmpty() ? QString()
	                                             : QFileInfo(m_imgPath).absolutePath();
	const QString filter = QStringLiteral("图片 (*.gif *.png *.jpg *.jpeg *.bmp);;所有文件 (*.*)");

	const QStringList paths = QFileDialog::getOpenFileNames(
		this, QStringLiteral("选择图片或动图（可多选）"), startDir, filter);
	if (paths.isEmpty())
	{
		return;
	}
	addAssets(paths);
}

void MainWindow::addAssets(const QStringList &paths)
{
	if (paths.isEmpty())
	{
		return;
	}

	/* 先把当前控件值写回选中的那份，避免待会儿切到新素材时把改动丢了 */
	storeWidgetsToAsset(m_imgCurrent);

	const bool merge = (m_imgMerge && m_imgMerge->isChecked());
	const int  firstRow = m_assets.size();

	/* 新素材的名字取文件名（去扩展名），转成能当 C 标识符的样子，重名加 _2。
	 * 这个名字三处共用：列表显示、导出文件名、头文件里的宏/函数前缀 ——
	 * 只维护一套名字，用户不用记两套对应关系。 */
	QStringList used;
	used.reserve(m_assets.size() + paths.size());
	for (const ImageAsset &a : m_assets)
	{
		used << a.name;
	}

	/* 新素材沿用当前这一份的取模设置：一次导十个图标通常想要同一档尺寸和阈值，
	 * 每张都从头调一遍太烦。 */
	const ImageGen::Options seedOpt  = imageOptions();
	const int               seedStep = frameStep();

	auto addOne = [&](const ImageGen::FrameBatch &batch, const QString &proposed) {
		ImageAsset a;
		a.name    = ImageGen::uniqueName(used, proposed);
		a.files   = batch.files;
		a.source  = batch.frames;
		a.delayMs = (batch.delayMs > 0) ? batch.delayMs : 100;
		a.sourceName = (batch.files.size() == 1)
		                   ? QFileInfo(batch.files.first()).fileName()
		                   : QStringLiteral("%1 等 %2 个文件（合成一个动画）")
		                         .arg(QFileInfo(batch.files.first()).fileName())
		                         .arg(batch.files.size());
		a.opt  = seedOpt;
		a.step = seedStep;
		a.note = batchNote(batch);
		rebuildAsset(a);

		m_assets.append(a);
		used << a.name;
	};

	QStringList failed;

	if (merge)
	{
		/* 一个批次：GIF 展开、按文件名排序、上限按整批算 */
		const ImageGen::FrameBatch batch = ImageGen::loadFrames(paths);
		if (batch.isEmpty())
		{
			QMessageBox::warning(this, QStringLiteral("打不开"),
			                     QStringLiteral("选中的文件都读不出图像：\n%1\n\n"
			                                    "支持 GIF / PNG / JPG / BMP。")
			                         .arg(batch.failed.join(QStringLiteral("\n"))));
			return;
		}
		/* 名字和默认目录都取排好序的第一个文件：
		 * 用户是从对话框里乱选一通的，拿 paths.first() 会得到 a3，
		 * 而「来源」写的是 a1.png 等 3 个文件，两边对不上。 */
		const QString first = batch.files.isEmpty() ? paths.first() : batch.files.first();
		addOne(batch, QFileInfo(first).completeBaseName());
		m_imgPath = first;
	}
	else
	{
		/* 每个文件单独读、单独成一份。各份之间没有帧数关系，
		 * 所以上限按单个文件算就对了。
		 * 先按文件名自然排序，列表里的顺序才和资源管理器里看到的一致
		 * （对话框返回的顺序取决于系统，frame10 也可能排在 frame2 前面）。 */
		QStringList ordered = paths;
		QCollator collator;
		collator.setNumericMode(true);
		collator.setCaseSensitivity(Qt::CaseInsensitive);
		std::stable_sort(ordered.begin(), ordered.end(),
		                 [&collator](const QString &a, const QString &b) {
			                 return collator.compare(QFileInfo(a).fileName(),
			                                         QFileInfo(b).fileName()) < 0;
		                 });

		for (const QString &p : ordered)
		{
			const ImageGen::FrameBatch batch = ImageGen::loadFrames(QStringList() << p);
			if (batch.isEmpty())
			{
				failed << QFileInfo(p).fileName();
				continue;
			}
			addOne(batch, QFileInfo(p).completeBaseName());
			m_imgPath = p;
		}

		if (m_assets.size() == firstRow)
		{
			QMessageBox::warning(this, QStringLiteral("打不开"),
			                     QStringLiteral("这些文件都读不出图像：\n%1\n\n"
			                                    "支持 GIF / PNG / JPG / BMP。")
			                         .arg(failed.join(QStringLiteral("\n"))));
			return;
		}
	}

	/* 选中最新加进来的那份，用户刚导完就想看到它 */
	m_imgCurrent = m_assets.size() - 1;
	refreshAssetList();
	showAsset(m_imgCurrent);

	if (!failed.isEmpty())
	{
		QMessageBox::warning(this, QStringLiteral("有文件读不出来"),
		                     QStringLiteral("下面这些读不出图像，已跳过：\n%1")
		                         .arg(failed.join(QStringLiteral("\n"))));
	}
}

void MainWindow::selectAsset(int row)
{
	/* 先把上一份的参数存回去 —— 用户改完参数直接点列表里另一份，
	 * 不存的话这些改动就白改了。 */
	storeWidgetsToAsset(m_imgCurrent);

	if (row < 0 || row >= m_assets.size())
	{
		m_imgCurrent = -1;
		showAsset(-1);
		return;
	}
	m_imgCurrent = row;
	showAsset(row);
}

void MainWindow::removeCurrentAsset()
{
	if (!currentAsset())
	{
		return;
	}

	const int row = m_imgCurrent;
	m_assets.remove(row);

	if (m_assets.isEmpty())
	{
		m_imgCurrent = -1;
		QSignalBlocker block(m_imgList);
		m_imgList->clear();
		m_imgPreview->clearFrames();
		m_imgFileLabel->setText(QStringLiteral("（还没有素材）"));
		updateImageInfo();
		return;
	}

	m_imgCurrent = qMin(row, m_assets.size() - 1);
	refreshAssetList();          /* 重建列表（信号已挡，选中不会自动触发） */
	showAsset(m_imgCurrent);
}

void MainWindow::refreshAssetItem(int row)
{
	if (!m_imgList || row < 0 || row >= m_assets.size())
	{
		return;
	}
	QListWidgetItem *item = m_imgList->item(row);
	if (!item)
	{
		return;
	}

	const ImageAsset &a = m_assets.at(row);
	const QString kind = (a.packed.size() > 1)
	                         ? QStringLiteral("%1 帧").arg(a.packed.size())
	                         : QStringLiteral("静态");
	item->setText(QStringLiteral("%1   ·   %2   ·   %3×%4")
	                  .arg(a.name, kind)
	                  .arg(a.opt.width)
	                  .arg(a.opt.height));

	QString tip = QStringLiteral("%1 帧 × %2 字节 = %3")
	                  .arg(a.packed.size())
	                  .arg(ImageGen::bytesPerFrame(a.opt.width, a.opt.height))
	                  .arg(byteSizeText(qint64(ImageGen::bytesPerFrame(a.opt.width, a.opt.height))
	                                    * a.packed.size()));
	tip += QLatin1Char('\n') + a.files.join(QStringLiteral("\n"));
	if (!a.note.isEmpty())
	{
		tip += QLatin1Char('\n') + a.note;
	}
	item->setToolTip(tip);
}

void MainWindow::refreshAssetList()
{
	if (!m_imgList)
	{
		return;
	}
	/* 重建过程中不要触发 currentRowChanged：那会把参数往还没准备好的素材上写 */
	const QSignalBlocker block(m_imgList);

	m_imgList->clear();
	for (int i = 0; i < m_assets.size(); ++i)
	{
		m_imgList->addItem(new QListWidgetItem());
		refreshAssetItem(i);
	}
	if (m_imgCurrent >= 0 && m_imgCurrent < m_assets.size())
	{
		m_imgList->setCurrentRow(m_imgCurrent);
	}
}

void MainWindow::storeWidgetsToAsset(int row)
{
	if (row < 0 || row >= m_assets.size() || !m_imgSize)
	{
		return;
	}
	m_assets[row].opt  = imageOptions();
	m_assets[row].step = frameStep();
}

void MainWindow::showAsset(int row)
{
	m_imgTimer->stop();
	m_imgPlayBtn->setText(QStringLiteral("播放"));

	const ImageAsset *a = (row >= 0 && row < m_assets.size()) ? &m_assets.at(row) : nullptr;
	if (!a)
	{
		m_imgPreview->clearFrames();
		m_imgPlayBtn->setEnabled(false);
		m_imgFileLabel->setText(QStringLiteral("（还没有素材）"));
		updateImageInfo();
		return;
	}

	/* 参数控件跟着换。挡信号是为了别把"换显示"当成"用户改了参数"，
	 * 那会触发一次多余的重算，还会把改动写回错误的那一份。 */
	{
		const QSignalBlocker b1(m_imgSize);
		const QSignalBlocker b2(m_imgScale);
		const QSignalBlocker b3(m_imgStep);
		const QSignalBlocker b4(m_imgThreshold);
		const QSignalBlocker b5(m_imgDither);
		const QSignalBlocker b6(m_imgInvert);

		int idx = m_imgSize->findData(QPoint(a->opt.width, a->opt.height));
		if (idx >= 0)
		{
			m_imgSize->setCurrentIndex(idx);
		}
		m_imgScale->setCurrentIndex(a->opt.stretch ? 1 : 0);
		idx = m_imgStep->findData(a->step);
		if (idx >= 0)
		{
			m_imgStep->setCurrentIndex(idx);
		}
		m_imgThreshold->setValue(a->opt.threshold);
		m_imgThresholdValue->setText(QString::number(a->opt.threshold));
		m_imgDither->setChecked(a->opt.dither == ImageGen::Bayer);
		m_imgInvert->setChecked(a->opt.invert);
	}

	const int delay = qMax(20, effectiveDelayMs());
	if (a->mono.size() > 1)
	{
		m_imgTimer->setInterval(delay);
	}
	m_imgPreview->setFrames(a->mono, delay);
	m_imgPlayBtn->setEnabled(a->mono.size() > 1);

	m_imgFileLabel->setText(assetDetailText(*a));
	updateImageInfo();
}

void MainWindow::recomputeImage()
{
	m_imgTimer->stop();
	m_imgPlayBtn->setText(QStringLiteral("播放"));

	ImageAsset *a = currentAsset();
	if (!a)
	{
		m_imgPreview->clearFrames();
		m_imgPlayBtn->setEnabled(false);
		updateImageInfo();
		return;
	}

	rebuildAsset(*a);

	const int delay = qMax(20, effectiveDelayMs());
	if (a->mono.size() > 1)
	{
		m_imgTimer->setInterval(delay);
	}
	m_imgPreview->setFrames(a->mono, delay);
	m_imgPlayBtn->setEnabled(a->mono.size() > 1);

	m_imgFileLabel->setText(assetDetailText(*a));
	refreshAssetItem(m_imgCurrent);     /* 帧数/尺寸可能变了，列表那行跟着更新 */
	updateImageInfo();
}

void MainWindow::updateImageInfo()
{
	/* ---- 列表合计 ----
	 * 一次导入十来张图时，要回答的是「这些一共占多少」，不是「选中的这张占多少」。
	 * 所以这一行不看选中项，只要列表里有素材就重算 —— 加、删、清空、改尺寸、
	 * 改抽帧都会走到这里。
	 * 不到两份就不显示：那一份的数字下面的详情里已经写了，再挂一行
	 * 一模一样的数会让人以为算了两次。 */
	{
		QVector<ImageGen::Footprint> items;
		items.reserve(m_assets.size());
		for (const ImageAsset &x : m_assets)
		{
			const ImageGen::Footprint f = { x.opt.width, x.opt.height,
			                                static_cast<int>(x.packed.size()) };
			items.append(f);
		}
		const ImageGen::Usage all = ImageGen::totalUsage(items);

		if (m_imgTotal)
		{
			if (m_assets.size() < 2)
			{
				/* 藏起来而不是只清文字：空 QLabel 仍然占一行高度，
				 * 单份素材时按钮和文件名之间就多出一道说不清来历的空白。 */
				m_imgTotal->clear();
				m_imgTotal->setToolTip(QString());
				m_imgTotal->setVisible(false);
			}
			else
			{
				const qint64 cap = flashCapacity();
				const double pct = (cap > 0) ? (all.bytes * 100.0 / cap) : 0.0;
				/* 分成两行写：侧栏只有 340 像素宽，拼成一行会剩一个「下」字
				 * 单独掉到第二排，比多占一行还难看。 */
				QString s = QStringLiteral("共 %1 份素材 · %2 帧 · %3")
				                .arg(m_assets.size())
				                .arg(all.frames)
				                .arg(byteSizeText(all.bytes));
				/* 超出容量时不再说「占 187%」—— 那句话帮不上忙，直接说装不下 */
				if (cap > 0 && all.bytes > cap)
				{
					s += QLatin1Char('\n')
					     + QStringLiteral("⚠ 超出 %1，装不下").arg(ChipInfo::formatSize(cap));
				}
				else if (cap > 0)
				{
					s += QLatin1Char('\n')
					     + QStringLiteral("占 %1 的 %2%")
					           .arg(ChipInfo::formatSize(cap))
					           .arg(pct, 0, 'f', 1);
					if (pct >= 40.0)
					{
						s += QStringLiteral(" ⚠");
					}
				}
				m_imgTotal->setText(s);
				m_imgTotal->setVisible(true);
				m_imgTotal->setToolTip(QStringLiteral(
					"按每份素材各自的尺寸算完再相加：128×64 一帧 1024 字节，\n"
					"16×16 一帧 32 字节，列表里可以混着放。\n"
					"改哪一份的尺寸或抽帧，这里都跟着变。"));
			}
		}
	}

	const ImageAsset *a = currentAsset();

	if (!a || a->packed.isEmpty())
	{
		const qint64 cap = flashCapacity();
		QString tip = (m_assets.isEmpty())
		                  ? QStringLiteral("添加一张图片或动图开始。")
		                  : QStringLiteral("在左边列表里选一份素材。");
		if (cap > 0)
		{
			/* 按实际容量和当前选的尺寸举例：512KB 的芯片能放 512 帧，
			 * 说"放不下几秒"就是在吓人；128×32 一帧也只有 512 字节。 */
			const QPoint sz = m_imgSize->currentData().toPoint();
			const int per  = ImageGen::bytesPerFrame(sz.x(), sz.y());
			const int full = per > 0 ? static_cast<int>(cap / per) : 0;
			tip += QStringLiteral("\n\n提示：%1×%2 一帧 %3 字节，%4 的芯片最多放 %5 帧。")
			           .arg(sz.x())
			           .arg(sz.y())
			           .arg(per)
			           .arg(ChipInfo::formatSize(cap))
			           .arg(full);
		}
		m_imgInfo->setText(tip);
		return;
	}

	const int frames = a->packed.size();
	const int per    = ImageGen::bytesPerFrame(a->opt.width, a->opt.height);
	const int total  = per * frames;

	/* 阈值按占芯片容量的比例算，不再用 8KB / 32KB 这种绝对数：
	 * 同样是 20KB，在 64KB 的芯片上占 31%（要提醒），在 512KB 上只占 3.9%（无所谓）。 */
	const qint64 cap = flashCapacity();
	const double pct = (cap > 0) ? (total * 100.0 / cap) : 0.0;
	QString hint;
	QString color;
	if (pct <= 15.0)
	{
		color = QStringLiteral("#15803D");     /* 绿 */
		hint  = QStringLiteral("Flash 占用宽裕");
	}
	else if (pct <= 40.0)
	{
		color = QStringLiteral("#B45309");     /* 橙 */
		hint  = QStringLiteral("注意 Flash 占用");
	}
	else
	{
		color = QStringLiteral("#B91C1C");     /* 红 */
		hint  = QStringLiteral("占用偏大，建议减小尺寸或加大抽帧");
	}

	const int delay = effectiveDelayMs();
	const double fps = (delay > 0) ? (1000.0 / delay) : 0.0;
	const double secs = (fps > 0) ? (frames / fps) : 0.0;

	/* 分几段拼，不用 %10 %11 —— arg() 对两位数占位符的处理容易踩到
	 * "%1" 把 "%10" 的前两个字符吃掉，分成短串就没这个风险。 */
	QString html = QStringLiteral("<span style='color:%1'>%2</span><br/>"
	                              "%3 帧 × %4 字节 = <b>%5 字节</b>")
	                   .arg(color, hint)
	                   .arg(frames)
	                   .arg(per)
	                   .arg(total);
	html += QStringLiteral("（%1 KB，占 %2 的 %3）<br/>")
	            .arg(total / 1024.0, 0, 'f', 1)
	            .arg(ChipInfo::formatSize(cap))
	            .arg(QStringLiteral("%1%").arg(pct, 0, 'f', 1));
	html += QStringLiteral("帧延时 %1 ms（约 %2 fps），可播 %3 秒")
	            .arg(delay)
	            .arg(fps, 0, 'f', 1)
	            .arg(secs, 0, 'f', 1);
	if (frameStep() > 1 && frames > 1)
	{
		/* 抽帧以后延时是放大过的，把原始值也说清楚，免得用户以为工具算错。
		 * 只有一帧时不提 —— 静态图没有"播放快慢"这回事。 */
		html += QStringLiteral("<br/>已抽帧：每 %1 帧取 1，延时按 ×%1 放大（GIF 原始 %2 ms）")
		            .arg(frameStep())
		            .arg(a->delayMs);
	}

	/* 合计挪到列表下面单独一行（见函数开头），这里不再重复说一遍 */

	m_imgInfo->setText(html);
}

void MainWindow::togglePlayback()
{
	const ImageAsset *a = currentAsset();
	if (!a || a->mono.size() < 2)
	{
		return;
	}
	if (m_imgTimer->isActive())
	{
		m_imgTimer->stop();
		m_imgPlayBtn->setText(QStringLiteral("播放"));
	}
	else
	{
		m_imgTimer->start(qMax(20, effectiveDelayMs()));
		m_imgPlayBtn->setText(QStringLiteral("暂停"));
	}
}

void MainWindow::exportAnimHeader()
{
	const ImageAsset *a = currentAsset();
	if (!a || a->packed.isEmpty())
	{
		QMessageBox::information(this, QStringLiteral("还没有素材"),
		                         QStringLiteral("先添加一张图片或动图。"));
		return;
	}

	const QString baseName = a->name + QStringLiteral(".h");
	QString start = QFileInfo(m_imgPath).absolutePath() + QLatin1Char('/') + baseName;
	if (!m_headerPath.isEmpty())
	{
		start = QFileInfo(m_headerPath).absolutePath() + QLatin1Char('/') + baseName;
	}

	const QString path = QFileDialog::getSaveFileName(
		this, QStringLiteral("导出素材头文件"), start,
		QStringLiteral("C 头文件 (*.h)"));
	if (path.isEmpty())
	{
		return;
	}

	/* 标识符按素材名加前缀（logo → LOGO_WIDTH / Logo_ShowFrame），
	 * 多份素材的头文件才能同时进一个工程。 */
	const QString err = ImageGen::writeHeaderFile(
		path, a->packed, a->opt, effectiveDelayMs(),
		a->sourceName.isEmpty() ? QStringLiteral("未记录") : a->sourceName,
		a->name);
	if (!err.isEmpty())
	{
		QMessageBox::critical(this, QStringLiteral("写不进去"),
		                      QStringLiteral("%1\n\n%2").arg(path, err));
		return;
	}

	const int total = a->packed.size() * ImageGen::bytesPerFrame(a->opt.width, a->opt.height);
	const QString prefix = ImageGen::sanitizeIdentifier(a->name);

	QMessageBox::information(this, QStringLiteral("导出成功"),
		QStringLiteral("已写入：\n%1\n\n%2 帧，共 %3 字节（%4 KB）\n\n"
		               "宏和函数以 %5_ 打头（%6_WIDTH、%6_ShowFrame），"
		               "和别的素材头文件放进同一个工程不会撞定义。\n"
		               "把【适配区】两个宏指向你的写命令、写数据函数，"
		               "再循环调用 %6_ShowFrame(i) 就行。")
		.arg(path)
		.arg(a->packed.size())
		.arg(total)
		.arg(total / 1024.0, 0, 'f', 1)
		.arg(prefix.toUpper())
		.arg(prefix));
}

void MainWindow::exportAllAssets()
{
	if (m_assets.isEmpty())
	{
		QMessageBox::information(this, QStringLiteral("还没有素材"),
		                         QStringLiteral("先添加图片或动图。"));
		return;
	}

	const QString start = !m_headerPath.isEmpty()
	                          ? QFileInfo(m_headerPath).absolutePath()
	                          : (m_imgPath.isEmpty() ? QDir::homePath()
	                                                 : QFileInfo(m_imgPath).absolutePath());

	const QString dir = QFileDialog::getExistingDirectory(
		this, QStringLiteral("选择目录（每份素材各写一个 .h）"), start);
	if (dir.isEmpty())
	{
		return;
	}

	/* 素材名唯一（导进来时就去重了），所以文件名也唯一。
	 * 覆盖前问一次：全部导出的按钮很容易误点，闷头覆盖太危险。 */
	QStringList targets;
	QStringList conflicts;
	for (const ImageAsset &a : m_assets)
	{
		const QString p = QDir(dir).filePath(a.name + QStringLiteral(".h"));
		targets << p;
		if (QFileInfo::exists(p))
		{
			conflicts << a.name + QStringLiteral(".h");
		}
	}
	if (!conflicts.isEmpty())
	{
		const auto ret = QMessageBox::question(
			this, QStringLiteral("有同名文件"),
			QStringLiteral("%1 里已经有 %2 个同名文件：\n%3\n\n覆盖吗？")
				.arg(dir)
				.arg(conflicts.size())
				.arg(conflicts.join(QStringLiteral("、"))),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		if (ret != QMessageBox::Yes)
		{
			return;
		}
	}

	QStringList written;
	QStringList errors;
	for (int i = 0; i < m_assets.size(); ++i)
	{
		const ImageAsset &a = m_assets.at(i);
		const QString err = ImageGen::writeHeaderFile(
			targets.at(i), a.packed, a.opt,
			qMax(1, a.delayMs) * qMax(1, a.step),
			a.sourceName.isEmpty() ? QStringLiteral("未记录") : a.sourceName,
			a.name);
		if (!err.isEmpty())
		{
			errors << QStringLiteral("%1：%2").arg(a.name + QStringLiteral(".h"), err);
			continue;
		}
		written << a.name + QStringLiteral(".h");
	}

	QString msg = QStringLiteral("已写入 %1 个文件到：\n%2\n\n%3")
	                  .arg(written.size())
	                  .arg(dir)
	                  .arg(written.join(QStringLiteral("、")));
	if (!errors.isEmpty())
	{
		msg += QStringLiteral("\n\n失败 %1 个：\n%2")
		           .arg(errors.size())
		           .arg(errors.join(QStringLiteral("\n")));
	}
	QMessageBox::information(this, QStringLiteral("导出完成"), msg);
}

/* ==========================================================================
 *  全部素材写进一个 .h —— 但每份各存各的
 *
 *  和「全部导出…」的区别只有一个：文件数。那边是 N 份素材 → N 个 .h，
 *  这边是 N 份素材 → 1 个 .h，里面每份仍然有自己的宏、数组、显示函数，
 *  名字按素材名打头，没有合并成一段动画数据。
 * ========================================================================== */

void MainWindow::exportAssetsToSingleFile()
{
	if (m_assets.isEmpty())
	{
		QMessageBox::information(this, QStringLiteral("还没有素材"),
		                         QStringLiteral("先添加图片或动图。"));
		return;
	}

	const QString dir = !m_headerPath.isEmpty()
	                        ? QFileInfo(m_headerPath).absolutePath()
	                        : (m_imgPath.isEmpty() ? QDir::homePath()
	                                               : QFileInfo(m_imgPath).absolutePath());

	const QString path = QFileDialog::getSaveFileName(
		this, QStringLiteral("全部存进一个头文件"),
		QDir(dir).filePath(QStringLiteral("oled_assets.h")),
		QStringLiteral("C 头文件 (*.h)"));
	if (path.isEmpty())
	{
		return;
	}

	/* 延时统一按"抽帧步长 × 单帧延时"折算，和「每份一个 .h」保持一致，
	 * 否则同一份素材换个导出方式播放速度就变了。 */
	QVector<ImageGen::Asset> list;
	for (const ImageAsset &a : m_assets)
	{
		if (a.packed.isEmpty())
		{
			continue;
		}
		ImageGen::Asset one;
		one.ident      = a.name;
		one.sourceName = a.sourceName;
		one.frames     = a.packed;
		one.opt        = a.opt;
		one.delayMs    = qMax(1, a.delayMs) * qMax(1, a.step);
		list.append(one);
	}
	if (list.isEmpty())
	{
		QMessageBox::information(this, QStringLiteral("没有可导出的素材"),
		                         QStringLiteral("列表里的素材都还没有帧。"));
		return;
	}

	const QString err = ImageGen::writeMultiHeaderFile(path, list, QFileInfo(path).fileName());
	if (!err.isEmpty())
	{
		QMessageBox::critical(this, QStringLiteral("写不进去"),
		                      QStringLiteral("%1\n\n%2").arg(path, err));
		return;
	}

	/* 记下来：图片页的「一键接入工程」直接认这个路径，不用再选一次 */
	m_assetsHeaderPath = path;
	QSettings().setValue(QStringLiteral("export/assetPack"), path);

	/* 把每份的宏前缀列出来：用户接下来要去 .c 里照着这个名字调函数，
	 * 让他不用回头翻头文件。 */
	QStringList lines;
	int total = 0;
	for (const ImageGen::Asset &a : list)
	{
		QString camel = ImageGen::sanitizeIdentifier(a.ident);
		camel[0] = camel.at(0).toUpper();
		const int bytes = ImageGen::bytesPerFrame(a.opt.width, a.opt.height) * a.frames.size();
		total += bytes;
		lines << QStringLiteral("  %1 · %2 帧 · %3×%4 · %5_ShowFrame(i)")
		             .arg(camel)
		             .arg(a.frames.size())
		             .arg(a.opt.width)
		             .arg(a.opt.height)
		             .arg(camel);
	}

	QMessageBox::information(this, QStringLiteral("导出成功"),
		QStringLiteral("已写入：\n%1\n\n%2 份素材，共 %3 字节（%4 KB）\n\n"
		               "每份各自独立（没有合成动画），点阵上方注明了导入时的文件名：\n%5\n\n"
		               "把【适配区】三个宏指向你的写命令、写数据、延时函数，"
		               "再调对应的 Xxx_ShowFrame(i) 就行。\n\n"
		               "驱动头声明和 main.c 的 #include 可以让工具代做："
		               "点左边的「一键接入工程…」。")
		.arg(path)
		.arg(list.size())
		.arg(total)
		.arg(total / 1024.0, 0, 'f', 1)
		.arg(lines.join(QLatin1Char('\n'))));
}

/* ==========================================================================
 *  烧录自检帧
 *
 *  字库能验证的只有"这个字取模对不对"，而对不对的前提是格式已经匹配。
 *  格式一错，整个字库都是花的，但花在哪、为什么花，光看一个字根本判断不出来。
 *  所以需要一张整屏图：边框负责暴露列偏移，四角负责暴露翻转和页序，
 *  中间的汉字负责暴露位序。一次烧录，全部结论都能拿到。
 * ========================================================================== */

void MainWindow::showSelfTest()
{
	QDialog dlg(this);
	dlg.setWindowTitle(QStringLiteral("烧录自检帧"));
	dlg.setMinimumWidth(920);

	QVBoxLayout *root = new QVBoxLayout(&dlg);
	root->setContentsMargins(18, 16, 18, 14);
	root->setSpacing(12);

	QLabel *head = new QLabel(QStringLiteral(
		"整屏一张图，按页整行写进显存。烧进板子刷一次，就能确认"
		"「工具吐出来的字节」和「你的驱动」是不是同一套格式。"), &dlg);
	head->setWordWrap(true);
	root->addWidget(head);

	/* 屏尺寸要能选：128×32 的 0.91" 屏如果刷 128×64 的帧，
	 * 后 4 页的列命令会落到不存在的页上，屏上只显示上半截，
	 * 看起来像"驱动少写了半屏"，其实帧和屏对不上。 */
	QHBoxLayout *sizeRow = new QHBoxLayout();
	sizeRow->setContentsMargins(0, 0, 0, 0);
	sizeRow->setSpacing(8);
	sizeRow->addWidget(new QLabel(QStringLiteral("屏尺寸"), &dlg));
	QComboBox *sizeBox = new QComboBox(&dlg);
	sizeBox->addItem(QStringLiteral("128 × 64（0.96\" 全屏）"), QPoint(128, 64));
	sizeBox->addItem(QStringLiteral("128 × 32（0.91\" 屏）"),   QPoint(128, 32));
	sizeBox->setCurrentIndex(currentScreenH() == 32 ? 1 : 0);   /* 跟导出组那个「屏」下拉保持一致 */
	sizeRow->addWidget(sizeBox);
	sizeRow->addStretch(1);
	root->addLayout(sizeRow);

	QHBoxLayout *mid = new QHBoxLayout();
	mid->setSpacing(18);

	/* 放大 4 倍显示。必须用最近邻，插值放大会把点阵糊成灰块，就不是点阵了。
	 * 尺寸多留 2px 是给 QLabel 自己的 1px 边框 —— 用 styleSheet 的 border 会
	 * 吃掉内容区，pixmap 边缘被裁掉半个像素，外框看着就不完整了。 */
	QLabel *screen = new QLabel(&dlg);
	screen->setFixedSize(SelfTest::W * 4 + 2, SelfTest::H * 4 + 2);
	screen->setFrameShape(QFrame::Box);
	screen->setStyleSheet(QStringLiteral("background:#000;"));
	screen->setAlignment(Qt::AlignCenter);
	mid->addWidget(screen, 0, Qt::AlignTop | Qt::AlignHCenter);

	QLabel *tips = new QLabel(&dlg);
	tips->setWordWrap(true);
	tips->setMinimumWidth(300);
	tips->setTextFormat(Qt::RichText);
	tips->setText(QStringLiteral(
		"<p style='margin:0 0 8px 0'><b>正常应该看到</b></p>"
		"<p style='margin:0 0 4px 0'>· 四边完整的外框，左右对称</p>"
		"<p style='margin:0 0 4px 0'>· 左上实心 / 右上空心 / 左下交叉 / 右下棋盘</p>"
		"<p style='margin:0 0 12px 0'>· 中间一行「取模正确」</p>"
		"<p style='margin:0 0 8px 0'><b>对不上时看现象</b></p>"
		"<p style='margin:0 0 4px 0'>右边框少两列、整幅左移 → 屏是 SH1106，驱动要偏移 2 列</p>"
		"<p style='margin:0 0 4px 0'>画面上下颠倒 → 页序反了（0xC8 ↔ 0xC0）</p>"
		"<p style='margin:0 0 4px 0'>变成白底黑点 → 极性反了（阳码 / 阴码）</p>"
		"<p style='margin:0'>字是花条纹、上下错位 → 位序反了（高位 / 低位在前）</p>"));
	mid->addWidget(tips, 1, Qt::AlignTop);

	root->addLayout(mid);

	QCheckBox *withText = new QCheckBox(&dlg);
	withText->setChecked(true);
	withText->setText(QStringLiteral("包含中文（%1×%1 点阵，样例「%2」）")
	                  .arg(m_gen.size())
	                  .arg(QStringLiteral("取模正确")));
	root->addWidget(withText);

	QHBoxLayout *btns = new QHBoxLayout();
	btns->addStretch(1);
	QPushButton *toProject = new QPushButton(QStringLiteral("保存到工程"), &dlg);
	toProject->setObjectName(QStringLiteral("Primary"));
	QPushButton *saveAs = new QPushButton(QStringLiteral("另存为…"), &dlg);
	QPushButton *closeBtn = new QPushButton(QStringLiteral("关闭"), &dlg);
	btns->addWidget(toProject);
	btns->addWidget(saveAs);
	btns->addWidget(closeBtn);
	root->addLayout(btns);

	const QString sampleText = QStringLiteral("取模正确");

	/* 自检帧验的是"工具吐出来的字节和驱动对不对"，取的是工具的原始点阵。
	 * 用户勾了取反也不该影响它：反白的自检帧看起来正好像"极性反了"，
	 * 这张图就失去诊断作用了。所以这里用一份不取反的副本。 */
	FontGen stGen(m_gen);
	stGen.setInvert(false);

	auto sample = [&]() {
		return withText->isChecked() ? sampleText : QString();
	};

	auto curSize = [&]() {
		const QPoint p = sizeBox->currentData().toPoint();
		return p.isNull() ? QPoint(SelfTest::W, SelfTest::H) : p;
	};

	auto rebuild = [&]() {
		const QPoint sz = curSize();
		screen->setFixedSize(sz.x() * 4 + 2, sz.y() * 4 + 2);

		const QImage img = SelfTest::buildImage(&stGen, sample(), sz.x(), sz.y());
		screen->setPixmap(QPixmap::fromImage(
			img.scaled(sz.x() * 4, sz.y() * 4,
			           Qt::IgnoreAspectRatio, Qt::FastTransformation)));
	};
	rebuild();

	connect(withText, &QCheckBox::toggled, &dlg, [&](bool) { rebuild(); });
	connect(sizeBox, &QComboBox::currentIndexChanged, &dlg, [&](int) { rebuild(); });

	/* 写文件单独抽出来，因为"保存到工程"和"另存为"只有路径不同 */
	auto saveTo = [&](const QString &path) {
		if (path.isEmpty())
		{
			return;
		}

		const QPoint sz = curSize();
		const QImage img = SelfTest::buildImage(&stGen, sample(), sz.x(), sz.y());
		const QString content = SelfTest::renderHeader(
			SelfTest::pack(img), sample(), m_gen.size(), m_fontBox->currentText(),
			currentColOffset(), sz.x(), sz.y());

		/* 打开成功不等于写成功：磁盘满、U 盘拔掉、杀软拦下写入这些情况，
		 * open() 是成功的，失败要等 close() 落盘才暴露。交给统一的一处判断。 */
		const QString err = FontGen::writeTextFile(path, content);
		if (!err.isEmpty())
		{
			QMessageBox::critical(&dlg, QStringLiteral("写不进去"),
			                      QStringLiteral("%1\n\n%2").arg(path, err));
			return;
		}

		QMessageBox::information(&dlg, QStringLiteral("自检帧已生成"),
			QStringLiteral("已写入：\n%1\n\n"
			               "用法：\n"
			               "1) 这个文件放到 OLED.c 同一目录（比如 Hardware\\）\n"
			               "2) main.c 里 #include \"OLED_SelfTest.h\"\n"
			               "3) OLED_Init();\n"
			               "   OLED_SelfTest_Show();\n"
			               "   while (1);\n\n"
			               "屏上四边完整、四角各不同、中间有字，就说明格式对上了。")
			.arg(path));
	};

	connect(toProject, &QPushButton::clicked, &dlg, [&]() {
		if (m_headerPath.isEmpty())
		{
			QMessageBox::information(&dlg, QStringLiteral("还不知道工程在哪"),
				QStringLiteral("点「另存为…」自己挑位置，"
				               "或回主界面点「设置工程目录…」指一次。"));
			return;
		}
		saveTo(QFileInfo(m_headerPath).absolutePath()
		       + QStringLiteral("/OLED_SelfTest.h"));
	});

	connect(saveAs, &QPushButton::clicked, &dlg, [&]() {
		const QString start = m_headerPath.isEmpty()
		        ? QDir::homePath() + QStringLiteral("/OLED_SelfTest.h")
		        : QFileInfo(m_headerPath).absolutePath() + QStringLiteral("/OLED_SelfTest.h");
		const QString path = QFileDialog::getSaveFileName(
			&dlg, QStringLiteral("自检帧保存到"), start,
			QStringLiteral("C 头文件 (*.h)"));
		if (!path.isEmpty())
		{
			saveTo(path);
		}
	});

	connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

	dlg.exec();
}

/* ==========================================================================
 *  一键接入工程
 *
 *  导出字库之后，用户手上还差三步手工活：驱动头文件补声明、main.c 补 include、
 *  补几行示例。这三步没有任何创造性，全是照抄，但每一步都有坑（函数没声明、
 *  参数顺序反了、Include Path 没配）。工具已经知道工程在哪了，没有理由让用户
 *  自己抄。
 *
 *  但改别人的源码是高危动作，所以这一版的三条底线：
 *      1) 只插入，绝不修改已有行
 *      2) 写盘前把每一处插入的文件、行号、内容全摆出来
 *      3) 每个文件留 .oledbak，一键撤销
 * ========================================================================== */
void MainWindow::mergeIntoProject(bool assets)
{
	QDialog dlg(this);
	dlg.setWindowTitle(assets ? QStringLiteral("一键接入动画")
	                          : QStringLiteral("一键接入工程"));
	dlg.resize(940, 640);

	QVBoxLayout *root = new QVBoxLayout(&dlg);

	/* ---- 检测结果 ---- */
	QLabel *head = new QLabel(&dlg);
	head->setObjectName(QStringLiteral("Hint"));
	head->setWordWrap(true);
	head->setTextInteractionFlags(Qt::TextSelectableByMouse);
	root->addWidget(head);

	/* ---- 示例：字库写文字，素材包挑一份来刷帧 ---- */
	QHBoxLayout *txtRow = new QHBoxLayout();
	QLineEdit *line1 = nullptr;
	QLineEdit *line2 = nullptr;
	QComboBox *showBox = nullptr;

	/* 素材包头：本次会话导出过就用它，否则用上次记住的路径 */
	QString packPath = m_assetsHeaderPath;
	if (packPath.isEmpty() || !QFileInfo::exists(packPath))
	{
		packPath = QSettings().value(QStringLiteral("export/assetPack")).toString();
	}

	if (assets)
	{
		txtRow->addWidget(new QLabel(QStringLiteral("示例用哪一份："), &dlg));
		showBox = new QComboBox(&dlg);
		showBox->addItems(MergeProject::listAssetShowFuncs(packPath));
		showBox->setToolTip(QStringLiteral(
			"从素材包头里读出来的显示函数。选中的这个会以「刷第 0 帧」的形式"
			"插到 main() 里。"));
		txtRow->addWidget(showBox, 1);
		QLabel *note = new QLabel(QStringLiteral("插在 OLED_Init 之后刷第 0 帧"), &dlg);
		note->setObjectName(QStringLiteral("Hint"));
		txtRow->addWidget(note);
	}
	else
	{
		txtRow->addWidget(new QLabel(QStringLiteral("示例第 1 行："), &dlg));
		line1 = new QLineEdit(QStringLiteral("温度正常"), &dlg);
		txtRow->addWidget(line1, 1);
		txtRow->addWidget(new QLabel(QStringLiteral("第 2 行："), &dlg));
		line2 = new QLineEdit(QStringLiteral("T=25C 湿度60%"), &dlg);
		txtRow->addWidget(line2, 1);
	}
	root->addLayout(txtRow);

	/* ---- 左边勾选清单，右边预览 ---- */
	QListWidget *stepList = new QListWidget(&dlg);
	stepList->setMinimumWidth(300);
	QPlainTextEdit *prev = new QPlainTextEdit(&dlg);
	prev->setReadOnly(true);
	prev->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
	prev->setLineWrapMode(QPlainTextEdit::NoWrap);

	QSplitter *split = new QSplitter(Qt::Horizontal, &dlg);
	split->addWidget(stepList);
	split->addWidget(prev);
	split->setStretchFactor(0, 0);
	split->setStretchFactor(1, 1);
	split->setSizes(QList<int>() << 320 << 600);
	root->addWidget(split, 1);

	QLabel *warn = new QLabel(&dlg);
	warn->setObjectName(QStringLiteral("Hint"));
	warn->setWordWrap(true);
	root->addWidget(warn);

	/* ---- 按钮 ---- */
	QHBoxLayout *btns = new QHBoxLayout();
	QPushButton *undoBtn = new QPushButton(QStringLiteral("撤销上次接入…"), &dlg);
	btns->addWidget(undoBtn);
	btns->addStretch(1);
	QPushButton *doBtn = new QPushButton(QStringLiteral("执行"), &dlg);
	doBtn->setObjectName(QStringLiteral("Primary"));
	QPushButton *closeBtn = new QPushButton(QStringLiteral("关闭"), &dlg);
	btns->addWidget(doBtn);
	btns->addWidget(closeBtn);
	root->addLayout(btns);

	MergeProject::Scan scan;

	auto showStep = [&](int row) {
		if (row < 0 || row >= scan.steps.size())
		{
			prev->clear();
			return;
		}
		const MergeProject::Step &s = scan.steps.at(row);
		const QString headText = QStringLiteral("// %1\n// %2\n\n").arg(s.title, s.detail);
		const QVector<MergeProject::Step> one = { s };
		prev->setPlainText(headText + MergeProject::previewOf(s.file, one, 3));
		prev->moveCursor(QTextCursor::Start);
	};

	/* 重扫：工程结构不会在执行前后大变，但示例文字会变 */
	auto rescan = [&]() {
		QSet<QString> keep;         /* 保住用户取消过的勾 */
		for (int i = 0; i < stepList->count() && i < scan.steps.size(); ++i)
		{
			if (stepList->item(i)->checkState() == Qt::Unchecked)
			{
				keep.insert(scan.steps.at(i).title);
			}
		}

		scan = MergeProject::Scan();
		bool ok = false;
		if (assets)
		{
			if (packPath.isEmpty() || !QFileInfo::exists(packPath))
			{
				head->setText(QStringLiteral(
					"还没有导出素材包。先在图片页点「全部存进一个 .h…」，再来做接入。"));
				stepList->clear();
				prev->clear();
				doBtn->setEnabled(false);
				return;
			}
			ok = MergeProject::detectAssets(packPath,
			                                showBox ? showBox->currentText() : QString(),
			                                &scan);
		}
		else
		{
			if (m_headerPath.isEmpty())
			{
				head->setText(QStringLiteral(
					"还没有导出字库。先点「导出 OLED_CFont.h」，再来做接入。"));
				stepList->clear();
				prev->clear();
				doBtn->setEnabled(false);
				return;
			}
			ok = MergeProject::detect(m_headerPath, line1->text().trimmed(),
			                          line2->text().trimmed(), &scan);
		}

		if (!ok)
		{
			head->setText(QStringLiteral("没找到能自动改的地方：\n\n") + scan.error);
			stepList->clear();
			prev->clear();
			doBtn->setEnabled(false);
			return;
		}

		QStringList h;
		if (assets)
		{
			h << QStringLiteral("素材包：%1").arg(QDir::toNativeSeparators(packPath));
		}
		h << QStringLiteral("工程：%1").arg(QDir::toNativeSeparators(scan.root));
		if (!scan.driverC.isEmpty())
		{
			h << QStringLiteral("驱动：%1（写命令 %2 / 写数据 %3）")
			         .arg(QDir::toNativeSeparators(scan.driverC))
			         .arg(scan.cmdFunc.isEmpty() ? QStringLiteral("没找到") : scan.cmdFunc)
			         .arg(scan.dataFunc.isEmpty() ? QStringLiteral("没找到") : scan.dataFunc);
		}
		if (!scan.mainC.isEmpty())
		{
			h << QStringLiteral("主程序：%1").arg(QDir::toNativeSeparators(scan.mainC));
		}
		head->setText(h.join(QLatin1Char('\n')));

		stepList->clear();
		for (const MergeProject::Step &s : scan.steps)
		{
			QListWidgetItem *it = new QListWidgetItem(s.title, stepList);
			it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
			it->setCheckState(keep.contains(s.title) ? Qt::Unchecked : Qt::Checked);
			it->setToolTip(s.detail);
		}

		QStringList w = scan.notes;
		if (!scan.notes.isEmpty())
		{
			w << QString();
		}
		w << QStringLiteral("执行前会把这些文件各备份一份（<文件名>.oledbak），"
		                    "点了执行之后随时能用「撤销上次接入」还原。");
		warn->setText(w.join(QLatin1Char('\n')));

		doBtn->setEnabled(!scan.steps.isEmpty());
		if (stepList->count() > 0)
		{
			stepList->setCurrentRow(0);
		}
		else
		{
			prev->setPlainText(QStringLiteral("没有需要改的地方 —— 该补的都补过了。"));
		}
	};

	/* 必须先连好再 rescan：rescan 里有 setCurrentRow(0)，
	 * 信号没连上的话第一次打开预览区是空的。 */
	if (assets)
	{
		connect(showBox, &QComboBox::currentTextChanged, &dlg,
		        [&](const QString &) { rescan(); });
	}
	else
	{
		connect(line1, &QLineEdit::textChanged, &dlg, [&](const QString &) { rescan(); });
		connect(line2, &QLineEdit::textChanged, &dlg, [&](const QString &) { rescan(); });
	}
	connect(stepList, &QListWidget::currentRowChanged, &dlg, [&](int row) { showStep(row); });

	rescan();

	/* ---- 撤销 ---- */
	connect(undoBtn, &QPushButton::clicked, &dlg, [&]() {
		if (scan.root.isEmpty())
		{
			QMessageBox::information(&dlg, QStringLiteral("没有可撤销的"),
			                         QStringLiteral("先让工具找到工程。"));
			return;
		}
		QStringList files;
		QDirIterator it(scan.root, QStringList() << QStringLiteral("*.oledbak"),
		                QDir::Files, QDirIterator::Subdirectories);
		while (it.hasNext())
		{
			it.next();
			const QString bak = it.filePath();
			files << bak.left(bak.size() - 8);      /* 去掉 ".oledbak" */
		}
		files = MergeProject::revertable(files);
		if (files.isEmpty())
		{
			QMessageBox::information(&dlg, QStringLiteral("没有可撤销的"),
			                         QStringLiteral("这个工程里没找到 .oledbak 备份。"));
			return;
		}

		const int r = QMessageBox::question(
			&dlg, QStringLiteral("撤销接入"),
			QStringLiteral("会把下面这些文件还原成接入之前的样子：\n\n%1\n\n"
			               "接入之后你自己又改过的内容会一起丢掉。继续吗？")
				.arg(QDir::toNativeSeparators(files.join(QLatin1Char('\n')))),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		if (r != QMessageBox::Yes)
		{
			return;
		}

		QString err;
		if (!MergeProject::revert(files, &err))
		{
			QMessageBox::critical(&dlg, QStringLiteral("撤销失败"), err);
			return;
		}
		QMessageBox::information(&dlg, QStringLiteral("已还原"),
		                         QStringLiteral("还原了 %1 个文件，备份已删除。")
		                             .arg(files.size()));
		rescan();
	});

	/* ---- 执行 ---- */
	connect(doBtn, &QPushButton::clicked, &dlg, [&]() {
		for (int i = 0; i < stepList->count() && i < scan.steps.size(); ++i)
		{
			scan.steps[i].checked = (stepList->item(i)->checkState() == Qt::Checked);
		}

		QStringList what;
		for (const MergeProject::Step &s : scan.steps)
		{
			if (s.checked)
			{
				what << QStringLiteral("· %1").arg(s.title);
			}
		}
		if (what.isEmpty())
		{
			QMessageBox::information(&dlg, QStringLiteral("没勾选任何改动"), QString());
			return;
		}

		const int r = QMessageBox::question(
			&dlg, QStringLiteral("确认改这些文件"),
			QStringLiteral("%1\n\n每个文件会先备份成 .oledbak。继续吗？")
				.arg(what.join(QLatin1Char('\n'))),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
		if (r != QMessageBox::Yes)
		{
			return;
		}

		QString err;
		QStringList touched;
		if (!MergeProject::apply(scan, &touched, &err))
		{
			QMessageBox::critical(&dlg, QStringLiteral("没改成"), err);
			return;
		}

		QMessageBox::information(
			&dlg, QStringLiteral("接好了"),
			QStringLiteral("改了 %1 个文件：\n\n%2\n\n"
			               "接下来在 Keil 里 Rebuild 就行。\n"
			               "要退回去就点「撤销上次接入」。")
				.arg(touched.size())
				.arg(QDir::toNativeSeparators(touched.join(QLatin1Char('\n')))));
		rescan();
	});

	connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

	dlg.exec();
}

/*
 * 找字库该导出到哪里。
 *
 * **判据是工程文件，不是驱动文件名。**
 * 工程里一定有 .uvprojx / .ewp / .cproject 中的一个，这是"这是一个工程"
 * 的硬定义；而驱动叫 OLED.c 还是 lcd.c 纯属个人习惯，拿来当判据等于
 * 把"猜"换了个名字。
 *
 * 优先级：
 *   1. 用户点过「设置工程目录」明确指定的（记在设置里，最高优先）
 *   2. 上次导出用过的目录
 *   3. 从 exe 往上逐层，在每层向下找工程文件（.uvprojx 等），
 *      找到后在其所在层/上一层挑字库落点
 *   4. 退一步：找已有的 OLED_CFont.h（老工程、或工程文件不在附近时）
 *   5. 环境变量 OLED_FONT_DIR
 *   6. 都没有 → 空，导出时让用户选目录（选完记住）
 *
 * 找不到是正常情况，不猜也不乱写。
 */
QString MainWindow::defaultHeaderPath() const
{
	/* 1) 用户明确指定过的工程目录 —— 一切猜测都要让位 */
	{
		const QString root = QSettings().value(QStringLiteral("project/root")).toString();
		if (!root.isEmpty() && QDir(root).exists())
		{
			const QString dir = pickFontDirIn(root, false);
			if (!dir.isEmpty())
			{
				return QDir(dir).absoluteFilePath(QStringLiteral("OLED_CFont.h"));
			}
		}
	}

	/* 2) 上次用过的目录 —— 下次启动就不用再猜 */
	{
		const QString last = QSettings().value(QStringLiteral("export/lastDir")).toString();
		if (!last.isEmpty() && QDir(last).exists())
		{
			return QDir(last).absoluteFilePath(QStringLiteral("OLED_CFont.h"));
		}
	}

	/* 工具自己的根目录：exe 在 oledfonttool\build\ 或 \package\ 下时，
	 * 上一级才是。整棵子树都不许扫 —— 否则会命中
	 * oledfonttool\src\OLED_CFont.h（工具自带的样例），而不是用户工程的。 */
	const QDir appDir(QCoreApplication::applicationDirPath());
	QString toolRoot = appDir.absolutePath();
	{
		const QString leaf = QFileInfo(toolRoot).fileName().toLower();
		if (leaf == QLatin1String("build") || leaf == QLatin1String("package"))
		{
			toolRoot = QFileInfo(toolRoot).absolutePath();     /* -> ...\oledfonttool */
		}
	}

	/* 3) 先找工程文件。深度 3 足够：工程文件要么在根，要么在
	 *    Project\ / MDK-ARM\ / EWARM\ 这一层。 */
	{
		QDir up(appDir.absolutePath());
		for (int step = 0; step <= 6; ++step)
		{
			const QStringList roots = findProjectRootsUnder(up.absolutePath(), 3, toolRoot);
			if (!roots.isEmpty())
			{
				/* 把候选带出去，界面好提示"还有别的工程" */
				m_projectCandidates = roots;

				const QString best = pickBestProject(roots);
				const QString fontDir = resolveFontDir(best, toolRoot);
				if (!fontDir.isEmpty() && QDir(fontDir).exists())
				{
					return QDir(fontDir).absoluteFilePath(QStringLiteral("OLED_CFont.h"));
				}
			}
			if (!up.cdUp())
			{
				break;
			}
		}
	}

	/* 4) 没有工程文件（比如只是拷了源码、或者工程文件在别处）→
	 *    找已有的字库文件，从它所在目录继续 */
	{
		QDir up(appDir.absolutePath());
		for (int step = 0; step <= 6; ++step)
		{
			const QString hit = findFileUnder(up.absolutePath(), QStringLiteral("OLED_CFont.h"),
			                                  3, toolRoot);
			if (!hit.isEmpty())
			{
				return QDir(hit).absoluteFilePath(QStringLiteral("OLED_CFont.h"));
			}
			if (!up.cdUp())
			{
				break;
			}
		}
	}

	/* 5) 环境变量：工具装在 C 盘、工程在 D 盘时，自己设一个就行 */
	const QByteArray envDir = qgetenv("OLED_FONT_DIR");
	if (!envDir.isEmpty())
	{
		const QDir d(QString::fromLocal8Bit(envDir));
		if (d.exists())
		{
			return d.absoluteFilePath(QStringLiteral("OLED_CFont.h"));
		}
	}

	/* 6) 猜不准 → 空字符串，让调用方弹对话框让用户自己选 */
	return QString();
}

/*
 * 手动指定工程目录。
 *
 * 存在的意义：自动定位再聪明也是猜，而用户心里非常清楚工程在哪。
 * 指一次就存进设置，之后 defaultHeaderPath() 第一优先级就是它，
 * 所有猜测都让位 —— 这比把启发式堆得越来越复杂要可靠。
 */
void MainWindow::chooseProjectDir()
{
	QString start = QSettings().value(QStringLiteral("project/root")).toString();
	if (start.isEmpty() || !QDir(start).exists())
	{
		start = m_headerPath.isEmpty()
		        ? QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
		              .absolutePath()
		        : QFileInfo(m_headerPath).absolutePath();
	}

	const QString picked = QFileDialog::getExistingDirectory(
		this,
		QStringLiteral("选择 STM32 工程目录（含 .c 源码的那一层）"),
		start,
		QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
	if (picked.isEmpty())
	{
		return;
	}

	/* 存"用户选的"这一层，不存推算结果。以后工程结构变了还能重新推。 */
	QSettings().setValue(QStringLiteral("project/root"), picked);
	m_projectCandidates.clear();     /* 已手动指定，不再需要"还有别的工程"的提示 */

	const QString dir = pickFontDirIn(picked, false);
	const QString target = dir.isEmpty() ? picked : dir;

	m_headerPath = QDir(target).absoluteFilePath(QStringLiteral("OLED_CFont.h"));
	QSettings().setValue(QStringLiteral("export/lastDir"), target);
	m_pathLabel->setText(QStringLiteral("导出到：%1").arg(tailPath(m_headerPath, 2)));
	m_pathLabel->setToolTip(m_headerPath);
	if (m_projWarn)
	{
		m_projWarn->setVisible(false);
	}

	/* 换工程了，编码检查的对象也变了，重新跑一次 */
	checkSourceEncoding(false);

	/* 屏型号、芯片容量同样是按工程判的，换了工程就得重判，
	 * 否则还在按上一个工程的 Flash 算占比 */
	refreshProjectFacts();
}

void MainWindow::refreshPreview()
{
	/* 先换字号再换字体：setSize 会把字体像素大小一起改掉，
	 * 顺序反了的话 setFont 用的还是旧字号 */
	m_gen.setSize(m_sizeBox->currentData().toInt());

	QFont f(m_fontBox->currentText());
	f.setPixelSize(m_gen.size());
	m_gen.setFont(f);
	m_gen.setThreshold(m_threshold->value());
	m_gen.setInvert(m_invert->isChecked());

	updateSubTitle();
	checkFontExists();
	m_preview->setText(m_text->toPlainText());
	updateStatus();
}

/* QFont 在字体名不存在时会静默回退到默认字体，那样字形就不是你要的那个了。
 *
 * 第二种情况更隐蔽：名字是真的，字体也在，但它里面没有汉字字形
 * （Arial、Consolas 这类西文字体，还有 Windows 上一批把样式名当家族名报出来的
 *  "Bahnschrift SemiBold Condensed"）。Qt 会自己找宋体顶上，而且把 SemiBold
 * 这个字重一起带过去 —— 16×16 取出来就是加粗的糊字。只看 family 名字的话，
 * 这里会被判成"字体找到了"，界面上一点提示都没有。
 * 所以改成问 FontGen：汉字最后是用哪套字体画的。 */
void MainWindow::checkFontExists()
{
	const QString want = m_fontBox->currentText();
	const QFontInfo info{QFont(want)};
	const bool sameFamily = info.family().compare(want, Qt::CaseInsensitive) == 0;

	if (sameFamily && !m_gen.cjkSubstituted())
	{
		m_fontWarn->setVisible(false);
		return;
	}

	if (m_gen.cjkSubstituted())
	{
		m_fontWarn->setText(QStringLiteral("⚠「%1」里没有汉字字形，汉字实际用「%2」画，"
		                                   "字形和这个字体不一样")
		                        .arg(want, m_gen.cjkFamily()));
	}
	else
	{
		m_fontWarn->setText(QStringLiteral("⚠ 找不到「%1」，实际用的是「%2」，字形会不一样")
		                        .arg(want, info.family()));
	}
	m_fontWarn->setVisible(true);
}

/*
 * 检查工程源码的编码，和字库的键（固定 UTF-8）是否匹配。
 *
 * 背景：Keil 按系统 ANSI 代码页读源文件且【不转码】，源文件里是什么字节，
 *       字符串字面量里就是什么字节。字库的键是 UTF-8 三字节，
 *       所以源码若是 GBK，两边对不上 —— 编译能过、程序能跑、屏幕全空。
 *       这种失败没有任何报错，主动检查一次能省掉大量排查时间。
 *
 * popupOnProblem = true 时，发现问题会弹窗（导出前调用）；
 *                = false 时只更新状态标签（启动时调用，不打扰）。
 */
void MainWindow::checkSourceEncoding(bool popupOnProblem)
{
	const QString root = findProjectRoot(m_headerPath);

	/* 把检查过程写进日志文件。
	 * 这类"静默失效"的问题光看界面看不出来（根目录找错了也只是显示"没找到"），
	 * 落盘之后可以直接读文件排查，不用来回截图。 */
	QStringList log;
	log << QStringLiteral("=== 源码编码检查 ===");
	log << QStringLiteral("时间: %1").arg(QDateTime::currentDateTime().toString(Qt::ISODate));
	log << QStringLiteral("程序目录: %1").arg(QCoreApplication::applicationDirPath());
	log << QStringLiteral("字库路径: %1").arg(m_headerPath.isEmpty() ? QStringLiteral("(空)") : m_headerPath);
	log << QStringLiteral("推算的工程根: %1").arg(root.isEmpty() ? QStringLiteral("(空)") : root);

	if (root.isEmpty())
	{
		m_encStatus->setText(QStringLiteral("源码编码：未找到工程目录，跳过检查"));
		log << QStringLiteral("结果: 未找到工程目录");
		writeEncodingLog(log);
		return;
	}

	QStringList checked;        /* 有中文字符串的文件 */
	QStringList mismatch;       /* 编码不匹配的文件 */
	QStringList noBom;          /* 是 UTF-8 但没带 BOM 的文件 */
	QStringList details;

	/* 递归找工程里的 .c / .h —— 不依赖子目录叫什么名字。
	 * 只读文件判断编码，不解析语义，所以扫全一点也没关系；
	 * 上限 1200 个文件、深度 4 层，够覆盖正常工程。 */
	QStringList sources;
	collectSourceFiles(QDir(root), sources, 4, 1200);
	log << QStringLiteral("扫描到 %1 个 .c/.h 文件").arg(sources.size());

	for (const QString &path : sources)
	{
			const QString name = QFileInfo(path).fileName();
			QString text;
			QByteArray raw;
			const QString enc = detectSourceEncoding(path, &text, &raw);
			if (enc.isEmpty())
			{
				log << QStringLiteral("    %1: 打不开").arg(name);
				continue;
			}

			/* 只看有没有"含中文的字符串字面量" —— 中文注释不影响查表 */
			bool hasChineseLiteral = false;
			QString sample;
			const QStringList literals = collectStringLiterals(text);
			for (const QString &lit : literals)
			{
				bool hasNonAscii = false;
				for (const QChar &c : lit)
				{
					if (c.unicode() > 127) { hasNonAscii = true; break; }
				}
				if (hasNonAscii)
				{
					hasChineseLiteral = true;
					if (sample.isEmpty()) sample = lit;
				}
			}
			log << QStringLiteral("    %1: 编码=%2 字面量=%3 含中文=%4")
			       .arg(name, enc).arg(literals.size())
			       .arg(hasChineseLiteral ? QStringLiteral("是") : QStringLiteral("否"));
			if (!hasChineseLiteral)
			{
				continue;
			}

			checked << name;
			details << QStringLiteral("%1  %2").arg(name, enc);

			/* 归类交给 judgeSource 统一判（有测试兜着）。以前这里是就地用
			 * enc 字符串拼条件，少写一个分支就是"明明编不过，界面还打勾"。 */
			switch (MergeProject::judgeSource(raw, hasChineseLiteral))
			{
			case MergeProject::SrcIssue::NotUtf8:
				mismatch << QStringLiteral("%1（%2）示例：\"%3\"").arg(name, enc, sample);
				break;
			case MergeProject::SrcIssue::NoBom:
				noBom << name;
				break;
			case MergeProject::SrcIssue::None:
				break;
			}
	}

	if (checked.isEmpty())
	{
		/* 把扫描的目录写出来，方便判断"是真的没有"还是"根目录找错了" */
		m_encStatus->setText(QStringLiteral("源码编码：%1 下没找到含中文的字符串字面量")
		                     .arg(QDir::toNativeSeparators(root)));
		log << QStringLiteral("结果: 没有含中文字面量的文件");
		writeEncodingLog(log);
		return;
	}

	if (mismatch.isEmpty() && noBom.isEmpty())
	{
		m_encStatus->setText(QStringLiteral("源码编码：✓ 正常（%1）")
		                     .arg(details.join(QStringLiteral("、"))));
		log << QStringLiteral("结果: 正常");
		writeEncodingLog(log);
		return;
	}

	/* 是 UTF-8 但没 BOM —— 查表没问题，armcc 解析就过不去 */
	const QString bomHint = QStringLiteral(
		"这些源文件是 UTF-8，但没带 BOM：\n\n%1\n\n"
		"armcc（Keil AC5）不看内容判编码，一律按系统代码页（GBK）当双字节解析。\n"
		"汉字 UTF-8 三字节的最后一位常落在 GBK 的引导字节区间，会把后面那个\n"
		"引号当成它的一半吞掉，于是编译报：\n\n"
		"    error:  #8: missing closing quote\n"
		"    error:  #18: expected a \")\"\n\n"
		"解决办法（任选一个）：\n"
		"  1. 给这些文件补 UTF-8 BOM（scripts\\add_bom.py 可以批量补；\n"
		"     用本工具「一键接入工程」写回时也会自动补）\n"
		"  2. 或者工程 C/C++ → Misc Controls 加 --locale=english，\n"
		"     整工程改成按直通字节处理，不再套本地代码页\n\n"
		"工程用 AC6（armclang）的话默认就按 UTF-8 读源码，这条可以忽略。")
		.arg(noBom.join(QStringLiteral("、")));

	if (mismatch.isEmpty())
	{
		m_encStatus->setText(QStringLiteral("⚠ %1 是 UTF-8 但缺 BOM，AC5 会报 #8")
		                     .arg(noBom.join(QStringLiteral("、"))));
		log << QStringLiteral("结果: 缺 BOM -> %1").arg(noBom.join(QStringLiteral("; ")));
		writeEncodingLog(log);

		if (popupOnProblem)
		{
			QMessageBox::warning(this, QStringLiteral("源码是 UTF-8 但缺 BOM"), bomHint);
		}
		return;
	}

	/* 有不匹配的 */
	const QString msg = QStringLiteral("源码编码和字库不一致：\n\n%1\n\n"
		"字库的键固定是 UTF-8 三字节，而这些源文件不是 UTF-8。\n"
		"Keil 读源文件时不做转码，字符串会按源文件的编码进入程序，\n"
		"和字库的键对不上 —— 编译能通过、程序也能跑，但屏幕上不会显示汉字。\n\n"
		"解决办法（任选一个）：\n"
		"  1. 把这些源文件另存成 UTF-8\n"
		"  2. 并且在 Keil 里 Edit → Configuration → Editor → Encoding 选 UTF-8")
		.arg(mismatch.join(QStringLiteral("\n")));

	m_encStatus->setText(QStringLiteral("⚠ 源码编码不匹配：%1")
	                     .arg(mismatch.join(QStringLiteral("、"))));

	log << QStringLiteral("结果: 不匹配 -> %1").arg(mismatch.join(QStringLiteral("; ")));
	if (!noBom.isEmpty())
	{
		log << QStringLiteral("另有缺 BOM -> %1").arg(noBom.join(QStringLiteral("; ")));
	}
	writeEncodingLog(log);

	if (popupOnProblem)
	{
		/* 两件事一起说：只说编码不对，用户改完 UTF-8 还会撞上缺 BOM 这一关 */
		const QString full = noBom.isEmpty() ? msg : msg + QStringLiteral("\n\n") + bomHint;
		QMessageBox::warning(this, QStringLiteral("源码编码和字库不一致"), full);
	}
}

/* 把编码检查的过程写到程序目录下的 encoding-check.txt */
void MainWindow::writeEncodingLog(const QStringList &lines) const
{
	const QString path = QDir(QCoreApplication::applicationDirPath())
		.absoluteFilePath(QStringLiteral("encoding-check.txt"));
	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
	{
		return;
	}
	QTextStream ts(&f);
	ts.setEncoding(QStringConverter::Utf8);
	for (const QString &l : lines)
	{
		ts << l << "\n";
	}
	f.close();
}

/* 从工程源码里判断这屏是 SSD1306 还是 SH1106。
 * 判不出来不是错误 —— 按最常见的 SSD1306（偏移 0）走，并在界面上说清楚。 */
void MainWindow::reprobeScreen()
{
	m_probeDone = true;
	m_probe = ScreenProbe::Result();

	QString base = m_headerPath;
	if (base.isEmpty())
	{
		base = defaultHeaderPath();
	}

	QString root = findProjectRoot(base);
	if (root.isEmpty() && !base.isEmpty())
	{
		/* 字库还没落进工程时，从它所在目录往下找 —— 驱动一般就在旁边 */
		root = QFileInfo(base).absolutePath();
	}

	if (root.isEmpty())
	{
		m_probe.evidenceOneLine = QStringLiteral(
			"还没确定工程位置，先按 SSD1306（偏移 0）导出");
		updateScreenNote();
		return;
	}

	m_probe = ScreenProbe::probe(root);
	updateScreenNote();
}

/* 把探测结论写进下拉的第一项。
 *
 * 单独占一行说明会把「导出」组撑高，1366×768 的屏上最下面的按钮会被顶出去，
 * 所以结论直接写进选项文字里，判定依据放 tooltip。 */
void MainWindow::updateScreenNote()
{
	if (!m_screenBox)
	{
		return;
	}

	/* 改 item 文字会发 currentIndexChanged，不挡住会递归回来 */
	const QSignalBlocker block(m_screenBox);

	if (m_probe.ok())
	{
		m_screenBox->setItemText(
			0, QStringLiteral("自动判断 · %1").arg(ScreenProbe::nameOf(m_probe.controller)));
	}
	else
	{
		m_screenBox->setItemText(0, QStringLiteral("自动判断 · 未识别（先按 SSD1306）"));
	}

	const QString tip = m_probe.evidence.isEmpty()
	        ? m_probe.evidenceOneLine
	        : m_probe.evidenceOneLine + QLatin1Char('\n') + QLatin1Char('\n')
	          + m_probe.evidence.join(QLatin1Char('\n'));
	m_screenBox->setItemData(0, tip, Qt::ToolTipRole);
	m_screenBox->setToolTip(tip);
}

/* 三份导出文件里那个 *_COL_OFFSET 该填几 */
int MainWindow::currentColOffset() const
{
	const int sel = m_screenBox ? m_screenBox->currentData().toInt() : 0;
	if (sel == 1)
	{
		return 0;
	}
	if (sel == 2)
	{
		return 2;
	}
	return m_probe.colOffset;
}

int MainWindow::currentScreenH() const
{
	const int h = m_screenHBox ? m_screenHBox->currentData().toInt() : 64;
	return (h == 32) ? 32 : 64;
}

/*
 * 副标题跟着「取反」开关走。
 *
 * 「阴码」说的是数据位 1 对应点亮的像素。勾了取反以后，亮的像素全反到了背景上，
 * 屏上是白底黑字 —— 这时候还挂「阴码」两个字就是自相矛盾，用户看到文件头里
 * 写「已取反」也会犯嘀咕。所以这一格换成「反白」。
 *
 * 另外三项（列行式 / 低位在前 / 字号）跟取反没有关系，取反只对调每一位的值，
 * 不改排列方式和位序，原样留着。
 *
 * setText 前先比一次：refreshPreview 每次敲键盘都会调到这儿，
 * 文字没变还调 setText 会让 QLabel 重新算布局，闪一下。
 */
void MainWindow::updateSubTitle()
{
	if (!m_subTitle)
	{
		return;
	}

	const bool inv = (m_invert && m_invert->isChecked());
	const QString want = inv
	        ? QStringLiteral("反白 · 列行式 · 低位在前 · 字号可选")
	        : QStringLiteral("阴码 · 列行式 · 低位在前 · 字号可选");

	if (m_subTitle->text() != want)
	{
		m_subTitle->setText(want);
	}

	m_subTitle->setToolTip(inv
	        ? QStringLiteral("已取反：数据位对调，屏上是亮底暗字（反白）。\n"
	                         "「列行式」「低位在前」不受取反影响。")
	        : QString());
}

void MainWindow::updateStatus()
{
	const int n = m_preview->glyphCount();
	const int size = m_gen.size();
	const qint64 bytes = FontGen::flashBytes(n, size, m_asciiFont->isChecked());

	/* 底下状态栏已经有完整版了，这里只留能一行放下的短版 —— 折成两行白吃 18 像素 */
	m_countLabel->setText(QStringLiteral("%1 个汉字 · %2×%2 · %3 字节（%4 KB）")
	                      .arg(n)
	                      .arg(size)
	                      .arg(bytes)
	                      .arg(QString::number(bytes / 1024.0, 'f', 1)));

	m_status->setText(QStringLiteral("汉字 %1 个 · %2×%2 · 每字 %3 字节 · 字库 %4 字节（%5 KB）")
	                  .arg(n)
	                  .arg(size)
	                  .arg(m_gen.glyphSize())
	                  .arg(bytes)
	                  .arg(QString::number(bytes / 1024.0, 'f', 1)));

	/* 容量那行：只写容量 + 占比，不摆芯片型号。
	 * 这工具不是只服务 STM32F103 —— 一行「STM32F103C8 · 64 KB」摆在状态栏上，
	 * 看起来像「只认这个型号」。型号和它的来源都放在 tooltip 里，
	 * 需要核对时鼠标一停就看得见，平时不占地方。
	 * （容量本身照旧按工程读，换 512KB 的工程这里就是 512 KB。） */
	const qint64 cap = flashCapacity();

	/* 没读到、又没手动选，就明说是按 64KB 估的，别让它看起来像真读到了 */
	const bool guessed = (!m_chip.ok() && (m_flashBox ? m_flashBox->currentData().toInt() == 0 : true));

	QString flash;
	if (guessed)
	{
		flash = QStringLiteral("Flash 按 64 KB 估 · 占 %1%")
		            .arg(bytes * 100.0 / cap, 0, 'f', 1);
	}
	else
	{
		flash = QStringLiteral("Flash %1 · 占 %2%")
		            .arg(ChipInfo::formatSize(cap))
		            .arg(bytes * 100.0 / cap, 0, 'f', 1);
	}

	/* 占比高到影响使用才变色：15% 以下随便放，40% 以上该提醒了 */
	if (bytes * 100.0 / cap >= 40.0)
	{
		flash += QStringLiteral(" ⚠");
	}
	m_flashInfo->setText(flash);

	/* 型号从标签上撤下来了，但没丢：tooltip 第一行就是它 */
	QString tip = m_chip.detail;
	if (!m_chip.device.isEmpty())
	{
		tip = QStringLiteral("工程芯片：%1\n%2").arg(m_chip.device, m_chip.detail);
	}
	m_flashInfo->setToolTip(tip);
}

/* 从工程文件读 Flash 容量。换工程要重读 —— 这是唯一一个按工程变、
 * 又不在「导出」组里的信息。 */
void MainWindow::readChip()
{
	QString root = findProjectRoot(m_headerPath);
	if (root.isEmpty() && !m_headerPath.isEmpty())
	{
		root = QFileInfo(m_headerPath).absolutePath();
	}
	m_chip = ChipInfo::read(root);
	updateChipNote();
}

/* 换工程之后，"按工程读一次"的结论全部失效：屏控制器、芯片容量、
 * 状态栏占比、图片页的容量提示。之前只在构造函数里做一遍，
 * 用户中途换工程（手动指定 / 导入别人工程的字库 / 导出时选别的目录）
 * 之后，界面还在拿上一个工程的数字说话，而且没有任何提示。 */
void MainWindow::refreshProjectFacts()
{
	m_probeDone = false;
	reprobeScreen();
	readChip();
	updateStatus();
	updateImageInfo();
}

/* 下拉第 0 项的文字跟着自动结果走，选它就是"用读到的值" */
void MainWindow::updateChipNote()
{
	if (!m_flashBox)
	{
		return;
	}
	const QSignalBlocker block(m_flashBox);

	if (m_chip.ok())
	{
		m_flashBox->setItemData(0, m_chip.detail, Qt::ToolTipRole);
	}
	else
	{
		m_flashBox->setItemData(
			0,
			m_chip.detail + QLatin1Char('\n') + QStringLiteral("\n没读到就按 64 KB 算。"),
			Qt::ToolTipRole);
	}
}

/* 当前按多大的 Flash 算。手动选的优先，其次自动读到的，最后 64KB 兜底。 */
qint64 MainWindow::flashCapacity() const
{
	const int sel = m_flashBox ? m_flashBox->currentData().toInt() : 0;
	if (sel > 0)
	{
		return static_cast<qint64>(sel) * 1024;
	}
	if (m_chip.ok())
	{
		return m_chip.flashBytes;
	}
	return 64 * 1024;
}

void MainWindow::exportHeader(bool askForPath)
{
	const QString srcText = m_text->toPlainText();
	QString err;

	/* 面板上那个字节数是按「是否包含 ASCII 字库」的勾选算的，导出提示里必须同口径。
	 * 原来这里写死 false（不含），于是同一个屏幕上两个数字差 1520 字节，
	 * 用户会以为哪个算错了。 */
	const qint64 totalBytes = FontGen::flashBytes(m_preview->glyphCount(), m_gen.size(),
	                                             m_asciiFont->isChecked());

	/* 第一次导出前才去读驱动源码 —— 构造函数里读的话，工程路径可能还没定 */
	if (!m_probeDone)
	{
		reprobeScreen();
	}

	const QString content = m_gen.generateHeader(srcText, &err, currentColOffset(),
	                                              currentScreenH());

	/* 诊断：每次导出都把中间结果落盘。
	 * 生成失败时靠猜没有意义，直接留下证据（文字、生成内容、逐行统计）。 */
	{
		const QString diagPath = QDir(QCoreApplication::applicationDirPath())
			.absoluteFilePath(QStringLiteral("export-debug.txt"));
		QFile dbg(diagPath);
		if (dbg.open(QIODevice::WriteOnly | QIODevice::Text))
		{
			QTextStream ts(&dbg);
			ts.setEncoding(QStringConverter::Utf8);
			ts << "=== 输入文字 ===\n";
			ts << "字符数: " << srcText.size() << "\n";
			ts << "内容: [" << srcText << "]\n\n";
			ts << "=== 汉字统计 ===\n";
			ts << "FontGen::glyphCount: " << FontGen::glyphCount(srcText) << "\n";
			ts << "content 是否为空: " << (content.isEmpty() ? "是" : "否") << "\n";
			ts << "content 长度: " << content.size() << "\n\n";
			ts << "=== 生成内容逐行统计 ===\n";

			const QStringList lines = content.split(QLatin1Char('\n'));
			ts << "总行数: " << lines.size() << "\n";
			for (int i = 0; i < lines.size(); ++i)
			{
				const QString tr = lines.at(i).trimmed();
				ts << QStringLiteral("[%1] 前6字符=<%2> 以0x开头=%3 长度=%4\n")
					.arg(i)
					.arg(tr.left(6))
					.arg(tr.startsWith(QLatin1String("0x")) ? "是" : "否")
					.arg(tr.size());
			}
			ts << "\n=== 原始内容(前 2000 字符) ===\n";
			ts << content.left(2000) << "\n";
			dbg.close();
		}
	}

	if (content.isEmpty())
	{
		QMessageBox::warning(this, QStringLiteral("无法导出"),
			err + QStringLiteral("\n\n(诊断信息已写入 export-debug.txt)"));
		return;
	}

	/* 写出前先自检：这个格式一旦写错，贴进 Keil 就是编译报错或者花屏，
	 * 而且肉眼很难看出来（我们已经在字库格式上栽过一次）。
	 *
	 * 这里不依赖 QString::count(QRegularExpression) 的语义——不同 Qt 版本对
	 * "数匹配个数"还是"数匹配到的字符总数"有歧义，曾经导致误报。
	 * 改成手动逐段扫："0x" + 两位十六进制 算一个字节。 */
	{
		const QStringList lines = content.split(QLatin1Char('\n'));
		/* 一行多少字节要跟着字号走：3 字节 UTF-8 编码 + 一个字的点阵。
		 * 这里原来写死 35（= 3 + 16×16/8），选 24/32 时会误判成"格式错误"并拒绝导出。 */
		const int stride = m_gen.glyphSize() + 3;
		int rows = 0;
		for (int i = 0; i < lines.size(); ++i)
		{
			const QString line = lines.at(i).trimmed();
			/* 注意：只以长度 + 前两字符判断，大小写都认（0x / 0X）。
			 * 曾经因为生成端写成 0X 而这里严格比 "0x"，导致误报"没有数据行"。 */
			if (line.size() < 5 ||
			    line.at(0) != QLatin1Char('0') ||
			    (line.at(1) != QLatin1Char('x') && line.at(1) != QLatin1Char('X')))
			{
				continue;
			}
			++rows;

			int bytes = 0;      /* 数 "0xNN" 的个数 */
			for (int k = 0; k + 3 < line.size(); )
			{
				if (isHexPair(line, k))
				{
					++bytes;
					k += 4;
				}
				else
				{
					++k;
				}
			}

			if (bytes != stride)
			{
				QString preview = line;
				if (preview.size() > 80)
				{
					preview = preview.left(80) + QStringLiteral(" ...");
				}
				QMessageBox::critical(this, QStringLiteral("导出前自检失败"),
					QStringLiteral("第 %1 行数出 %2 个字节，期望 %3 个"
					               "（%4×%5 点阵 = 3 字节编码 + %6）。\n\n该行开头：\n%7")
					.arg(i + 1)
					.arg(bytes)
					.arg(stride)
					.arg(m_gen.size())
					.arg(m_gen.size())
					.arg(m_gen.glyphSize())
					.arg(preview));
				return;
			}
		}
		if (rows == 0)
		{
			QMessageBox::critical(this, QStringLiteral("导出前自检失败"),
				QStringLiteral("没有生成任何字模数据行。\n"
				               "当前文字里似乎没有汉字（只有 ASCII 字符？）\n\n"
				               "诊断信息已写入程序目录下的 export-debug.txt，请把它发给我。"));
			return;
		}
	}

	/* 什么时候先问用户保存位置：
	 *   1. 用户勾了"每次导出都询问"
	 *   2. 没自动找到工程目录（猜不准，绝不能默默写到别处）
	 * 否则直接写到自动推断出来的路径，保持一键导出的便利。
	 *
	 * 例外：调用方（「另存为…」）刚让用户选完路径，这时 askForPath 为 false，
	 * 绝不能再弹一次 —— 否则用户要点两次"保存/替换"，第二次才真正写文件。 */
	const bool needAsk = askForPath &&
	                     (m_askEveryTime->isChecked() || m_headerPath.isEmpty());
	if (needAsk)
	{
		/* 两种情况问的东西不一样：
		 *   没找到工程 → 问"你的工程在哪"（选目录）
		 *   勾了每次询问 → 问"存到哪个文件"（选文件，允许改名）
		 * 混用会让人困惑：明明只想把文件放进工程，却要先给文件起个名。 */
		if (m_headerPath.isEmpty())
		{
			const QString pickedDir = QFileDialog::getExistingDirectory(
				this,
				QStringLiteral("选择你的 STM32 工程目录（含 .c 源码的那一层）"),
				QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
					.absolutePath(),
				QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
			if (pickedDir.isEmpty())
			{
				return;     /* 用户取消了 */
			}

			/* 大多数人选的是工程根，而字库通常该放进 Hardware / User 这类子目录。
			 * 这里顺着往下找一层，省掉"导出完还得自己搬"那一步。 */
			QString target = pickFontDirIn(pickedDir, false);
			if (target.isEmpty())
			{
				target = pickedDir;
			}

			/* 记的是用户选的"工程根"，不是推算出来的字库层 ——
			 * 下次换字号、换目录结构还能重新推算，不会把旧路径钉死。 */
			QSettings().setValue(QStringLiteral("project/root"), pickedDir);
			m_headerPath = QDir(target).absoluteFilePath(QStringLiteral("OLED_CFont.h"));
		}
		else
		{
			const QString picked = QFileDialog::getSaveFileName(
				this, QStringLiteral("导出到"), m_headerPath,
				QStringLiteral("C 头文件 (*.h)"));
			if (picked.isEmpty())
			{
				return;     /* 用户取消了 */
			}
			m_headerPath = picked;
		}

		/* 目录是这一刻才定下来的，所以芯片容量、屏控制器都要按新工程重来一遍。
		 * 否则状态栏还在按上一个工程（或者 64KB 估算）说话，
		 * 而用户刚选完目录，看到的正是那一行。 */
		refreshProjectFacts();

		/* 导出过的目录记下来，下次启动直接用它 */
		QSettings().setValue(QStringLiteral("export/lastDir"),
		                     QFileInfo(m_headerPath).absolutePath());
		m_pathLabel->setText(QStringLiteral("导出到：%1").arg(tailPath(m_headerPath, 2)));
		m_pathLabel->setToolTip(m_headerPath);
	}

	QFile file(m_headerPath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
	{
		/* 先判断到底是什么原因：Windows 把"文件被占用"和"权限不足"
		 * 都报成"拒绝访问"，光看错误信息分不出来。
		 * 这里的办法是同目录下建一个临时文件试试：
		 *   能建 → 目录可写，问题在那个文件本身（多半被 Keil/编辑器打开了）
		 *   不能建 → 目录不可写（权限问题） */
		const QString dirPath = QFileInfo(m_headerPath).absolutePath();
		const QString probePath = QDir(dirPath).absoluteFilePath(QStringLiteral("write-probe.tmp"));
		bool dirWritable = false;
		{
			QFile probe(probePath);
			if (probe.open(QIODevice::WriteOnly))
			{
				dirWritable = true;
				probe.close();
				probe.remove();
			}
		}

		/* 这个程序进程可能被限制了写入范围（实测：连系统临时目录都写不进去，
		 * 只有程序自己的目录可以）。所以失败后不再弹"另存为"，
		 * 而是自动重试到确定可写的位置，再告诉用户怎么搬进工程。 */
		const QString srcDir = findSrcDir();
		QStringList fallbacks;
		fallbacks << QDir(QCoreApplication::applicationDirPath())
		             .absoluteFilePath(QStringLiteral("OLED_CFont.h"));
		if (!srcDir.isEmpty())
		{
			fallbacks << QDir(srcDir).absoluteFilePath(QStringLiteral("OLED_CFont.h"));
		}

		for (int i = 0; i < fallbacks.size(); ++i)
		{
			/* 写成功才认，写不完就换下一个位置：原来只要 open 成功就
			 * 弹"导出成功（需要手动搬运）"，磁盘满时搬过去的是个空文件。 */
			if (!FontGen::writeTextFile(fallbacks.at(i), content).isEmpty())
			{
				continue;
			}
			const QString savedTo = fallbacks.at(i);

			m_headerPath = savedTo;
			m_pathLabel->setText(QStringLiteral("导出到：%1").arg(tailPath(savedTo, 2)));
			m_pathLabel->setToolTip(savedTo);

			const QString installer = srcDir.isEmpty()
				? QStringLiteral("install-cfont.cmd")
				: QDir(srcDir).absoluteFilePath(QStringLiteral("install-cfont.cmd"));

			QMessageBox::information(this, QStringLiteral("导出成功（需要手动搬运）"),
				QStringLiteral("程序没有权限直接写入工程目录，已改为导出到：\n%1\n\n"
				               "下一步：双击\n%2\n"
				               "把字库搬进工程的 Hardware 目录（会自动备份旧文件）。\n\n"
				               "汉字 %3 个，%4 字节。")
				.arg(savedTo)
				.arg(installer)
				.arg(m_preview->glyphCount())
				.arg(totalBytes));
			return;
		}

		QString reason;
		if (dirWritable)
		{
			reason = QStringLiteral(
				"目录本身可以写，但目标文件打不开。\n"
				"最常见的原因是它正被别的程序占用——\n"
				"请关掉 Keil / VSCode / 记事本里打开的 OLED_CFont.h 再试。");
		}
		else
		{
			reason = QStringLiteral(
				"这个程序进程不能写那个目录。\n"
				"（实测连系统临时目录都写不进去，只有程序自己的目录可以，\n"
				"  属于进程被限制了写入范围，不是文件权限问题。）");
		}

		/* 诊断：把程序自己的运行环境和逐项测试结果落盘。
		 * 用户用 cmd 测试同一个目录是成功的，只有本程序失败，
		 * 所以必须看清楚程序进程到底在什么环境下跑。 */
		{
			const QString diagPath = QDir(QCoreApplication::applicationDirPath())
				.absoluteFilePath(QStringLiteral("export-debug.txt"));
			QFile dbg(diagPath);
			if (dbg.open(QIODevice::WriteOnly | QIODevice::Text))
			{
				QTextStream ts(&dbg);
				ts.setEncoding(QStringConverter::Utf8);
				ts << "=== 导出失败诊断 ===\n";
				ts << "时间: " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n\n";
				ts << "--- 运行环境 ---\n";
				ts << "用户名: " << qgetenv("USERNAME") << "\n";
				ts << "计算机名: " << qgetenv("COMPUTERNAME") << "\n";
				ts << "工作目录: " << QDir::currentPath() << "\n";
				ts << "程序目录: " << QCoreApplication::applicationDirPath() << "\n";
				ts << "TEMP 环境变量: " << qgetenv("TEMP") << "\n";
				ts << "TMP  环境变量: " << qgetenv("TMP") << "\n";
				ts << "Qt 认为的临时目录: " << QDir::tempPath() << "\n";
				ts << "是否以管理员运行: "
				   << (IsUserAnAdmin() ? "是" : "否") << "\n";

				/* 打印进程令牌里的组和完整性级别：
				 * AppContainer / 受限令牌会在这里露出特征（低完整性、AppContainer SID）。 */
				HANDLE hTok = nullptr;
				if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hTok))
				{
					DWORD len = 0;
					GetTokenInformation(hTok, TokenGroups, nullptr, 0, &len);
					if (len)
					{
						QByteArray buf(static_cast<int>(len), 0);
						if (GetTokenInformation(hTok, TokenGroups, buf.data(), len, &len))
						{
							TOKEN_GROUPS *groups = reinterpret_cast<TOKEN_GROUPS *>(buf.data());
							ts << "令牌组数: " << static_cast<int>(groups->GroupCount) << "\n";
							for (DWORD g = 0; g < groups->GroupCount; ++g)
							{
								WCHAR name[256] = {0};
								WCHAR domain[256] = {0};
								DWORD nLen = 256, dLen = 256;
								SID_NAME_USE use;
								if (LookupAccountSidW(nullptr, groups->Groups[g].Sid,
								                      name, &nLen, domain, &dLen, &use))
								{
									ts << "  " << QString::fromWCharArray(domain) << "\\"
									   << QString::fromWCharArray(name)
									   << (groups->Groups[g].Attributes & SE_GROUP_USE_FOR_DENY_ONLY
									           ? "  [仅用于拒绝]" : "") << "\n";
								}
							}
						}
					}

					DWORD ilen = 0;
					GetTokenInformation(hTok, TokenIntegrityLevel, nullptr, 0, &ilen);
					if (ilen)
					{
						QByteArray ibuf(static_cast<int>(ilen), 0);
						if (GetTokenInformation(hTok, TokenIntegrityLevel, ibuf.data(), ilen, &ilen))
						{
							TOKEN_MANDATORY_LABEL *label =
								reinterpret_cast<TOKEN_MANDATORY_LABEL *>(ibuf.data());
							const DWORD rid = *GetSidSubAuthority(
								label->Label.Sid,
								*GetSidSubAuthorityCount(label->Label.Sid) - 1);
							ts << "完整性级别 RID: " << static_cast<int>(rid) << "  (";
							if (rid < SECURITY_MANDATORY_MEDIUM_RID)      ts << "低于 Medium —— 受限！";
							else if (rid == SECURITY_MANDATORY_MEDIUM_RID) ts << "Medium（正常）";
							else                                           ts << "高于 Medium";
							ts << ")\n";
						}
					}
					CloseHandle(hTok);
				}
				ts << "\n";
				ts << "--- 目标 ---\n";
				ts << "m_headerPath: [" << m_headerPath << "]\n";
				ts << "absolutePath: [" << dirPath << "]\n";
				ts << "目录存在: " << (QDir(dirPath).exists() ? "是" : "否") << "\n";
				ts << "文件存在: " << (QFile::exists(m_headerPath) ? "是" : "否") << "\n";
				ts << "文件大小: " << QFileInfo(m_headerPath).size() << "\n\n";
				ts << "--- 逐项测试 ---\n";
				ts << "1) QFile 打开目标文件(WriteOnly): " << file.errorString() << "\n";

				QFile t1(probePath);
				ts << "2) QFile 在目标目录建 probe.tmp: "
				   << (t1.open(QIODevice::WriteOnly) ? "成功" : ("失败 - " + t1.errorString())) << "\n";
				if (t1.isOpen()) { t1.close(); t1.remove(); }

				QFile t2(QDir::temp().absoluteFilePath(QStringLiteral("oled-probe.tmp")));
				ts << "3) QFile 在系统临时目录建文件: "
				   << (t2.open(QIODevice::WriteOnly) ? "成功" : ("失败 - " + t2.errorString())) << "\n";
				if (t2.isOpen()) { t2.close(); t2.remove(); }

				QFile t3(QDir(QCoreApplication::applicationDirPath())
					.absoluteFilePath(QStringLiteral("oled-probe.tmp")));
				ts << "4) QFile 在程序目录建文件: "
				   << (t3.open(QIODevice::WriteOnly) ? "成功" : ("失败 - " + t3.errorString())) << "\n";
				if (t3.isOpen()) { t3.close(); t3.remove(); }

				QFile t4(QDir(QDir::currentPath()).absoluteFilePath(QStringLiteral("oled-probe.tmp")));
				ts << "5) QFile 在工作目录建文件: "
				   << (t4.open(QIODevice::WriteOnly) ? "成功" : ("失败 - " + t4.errorString())) << "\n";
				if (t4.isOpen()) { t4.close(); t4.remove(); }

				dbg.close();
			}
		}

		QMessageBox::critical(this, QStringLiteral("导出失败"),
			QStringLiteral("无法写入：\n%1\n\n%2\n\n系统报错：%3\n\n"
			               "诊断已写入 export-debug.txt（在程序目录下）。")
			.arg(m_headerPath, reason, file.errorString()));
		return;
	}

	QTextStream ts(&file);
	ts.setEncoding(QStringConverter::Utf8);        /* 必须是 UTF-8，Keil 才不会读花 */
	ts << content;
	ts.flush();
	file.close();

	/* 落盘也要确认。这一行原来是缺的：写进缓冲就报"导出成功"，
	 * 磁盘满 / U 盘拔掉时用户拿到的是半截文件，而界面说成功了。 */
	if (file.error() != QFileDevice::NoError)
	{
		QMessageBox::critical(this, QStringLiteral("导出失败"),
			QStringLiteral("文件已经打开，但内容没写完：\n%1\n\n%2\n\n"
			               "最常见的原因是磁盘满了，或者文件正被别的程序写入。\n"
			               "请腾出空间后重新导出。")
				.arg(m_headerPath, file.errorString()));
		return;
	}

	/* 记住这个目录：下次启动直接用它，不用再猜（也不依赖工程名） */
	QSettings().setValue(QStringLiteral("export/lastDir"),
	                     QFileInfo(m_headerPath).absolutePath());

	/* 写完把路径显示同步一下，避免提示里写着 A、标签上还显示 B */
	m_pathLabel->setText(QStringLiteral("导出到：%1").arg(tailPath(m_headerPath, 2)));
	m_pathLabel->setToolTip(m_headerPath);

	/* 换了字号，驱动那边不是"重新编译一下"就完事的：
	 * 数组名变了，查表的步长也变了。这里必须说清楚，否则屏幕上会错位。 */
	const QString sizeNote = (m_gen.size() == 16)
		? QString()
		: QStringLiteral("\n\n注意：这次导出的是 %1×%1 字库，数组名是 %2。\n"
		                 "驱动里原来写死的 16 / 32 要换成文件开头的\n"
		                 "OLED_CFONT_W / H / BYTES / STRIDE 这几个宏，\n"
		                 "否则字会错位或只显示一部分。")
		  .arg(m_gen.size())
		  .arg(FontGen::tableName(m_gen.size()));

	QMessageBox::information(this, QStringLiteral("导出成功"),
	                         QStringLiteral("已写入：\n%1\n\n%2×%2 点阵，汉字 %3 个，%4 字节\n"
	                                        "在 Keil 里 Rebuild 即可生效。%5\n\n"
	                                        "头文件里已带 OLED_CFont_ShowString()，"
	                                        "工程里没有中文显示函数也能直接用：\n"
	                                        "把文件里【适配区】的宏指向你的写命令、写数据函数，"
	                                        "然后调用\n"
	                                        "    OLED_CFont_ShowString(行, 列, \"要显示的文字\");")
	                         .arg(m_headerPath)
	                         .arg(m_gen.size())
	                         .arg(m_preview->glyphCount())
	                         .arg(totalBytes)
	                         .arg(sizeNote));

	/* 导出成功后顺便检查一下工程源码的编码。
	 * 编码不一致时字库能写进去、Keil 也能编译，但屏幕上不会显示汉字，
	 * 这里提醒一次，免得用户对着一片空白找原因。 */
	checkSourceEncoding(true);
}

void MainWindow::importHeader()
{
	const QString path = QFileDialog::getOpenFileName(
		this, QStringLiteral("选择已有的 OLED_CFont.h"), m_headerPath,
		QStringLiteral("C 头文件 (*.h)"));
	if (path.isEmpty())
	{
		return;
	}

	QFile file(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
	{
		QMessageBox::critical(this, QStringLiteral("读取失败"), file.errorString());
		return;
	}
	const QString text = FontGen::parseHeader(QString::fromUtf8(file.readAll()));
	file.close();

	if (text.isEmpty())
	{
		QMessageBox::warning(this, QStringLiteral("没读到汉字"),
		                     QStringLiteral("这个文件里没有可识别的字模行。"));
		return;
	}

	/* 合并而不是替换：导入已有字库通常是为了"在它的基础上加字"，
	 * 直接替换会把原来几十个字全丢掉。 */
	const QString current = m_text->toPlainText();
	QString merged;
	QSet<QChar> seen;
	for (int pass = 0; pass < 2; ++pass)
	{
		const QString &src = (pass == 0) ? current : text;
		for (int i = 0; i < src.size(); ++i)
		{
			const QChar c = src.at(i);
			if (!seen.contains(c))
			{
				seen.insert(c);
				merged += c;
			}
		}
	}

	m_text->setPlainText(merged);
	m_pathLabel->setText(QStringLiteral("导出到：%1").arg(tailPath(path, 2)));
	m_pathLabel->setToolTip(path);
	m_headerPath = path;

	/* 导入的可能是别的工程的字库 —— 屏控制器、芯片容量、源码编码都跟着变。
	 * 这些以前要重启程序才更新，用户会以为"工具认的还是原来那个工程"。 */
	refreshProjectFacts();
	checkSourceEncoding(false);

	const int before = FontGen::glyphCount(current);
	const int after = FontGen::glyphCount(merged);
	const int added = FontGen::glyphCount(text);

	if (after < before)
	{
		/* 合并逻辑理论上只增不减；真出现减少说明用户清空了输入框之类，
		 * 提醒一句，别把好不容易攒的字丢了 */
		QMessageBox::information(this, QStringLiteral("注意字数变化"),
		    QStringLiteral("合并后是 %1 个汉字，比原来的 %2 个少。\n"
		                   "如果这不是你要的，请先撤销（Ctrl+Z）。").arg(after).arg(before));
	}

	m_status->setText(QStringLiteral("已导入 %1 字，合并后共 %2 字")
	                  .arg(added)
	                  .arg(after));
}

int main(int argc, char *argv[])
{
	QApplication app(argc, argv);

	/* 组织名必须先设。
	 * QSettings 的默认构造函数用 organizationName() + applicationName()
	 * 拼存储路径（Windows 是 HKEY_CURRENT_USER\Software\<组织>\<应用>）。
	 * 组织名为空时 Qt 直接判定为 AccessError —— fileName() 返回空字符串，
	 * setValue() 静默丢弃、value() 永远读回空值，不报错也不警告。
	 *
	 * 这曾经导致「点了『更改』指定工程目录，下次启动又忘」「记住上次导出目录」
	 * 两个功能完全失效，而且失效得没有任何痕迹：代码逻辑看着都对，
	 * 只是值存不进去。实测确认：没有这一行时 status=1(AccessError)、
	 * 写进去读不回来；加上之后 status=0、读写正常。 */
	app.setOrganizationName(QStringLiteral("OLEDFontTool"));

	/* 应用名从「OLED 汉字取模工具」改成「OLED 点阵取模工具」，
	 * 存储路径 ...\OLEDFontTool\<应用名> 跟着变，旧键里的「上次导出目录」
	 * 「工程目录」读不到了。不做旧键迁移：这个键在本机从来没写进去过
	 * （见上），迁移代码只会是永远走不到的死分支。 */
	app.setApplicationName(QStringLiteral("OLED 点阵取模工具"));

	/* 强制用 Fusion 风格 + 浅色调色板。
	 *
	 * 原因是踩过坑：Windows 深色主题下，QMessageBox 会用深色背景，
	 * 而样式表里的深色文字在深色背景上完全看不见——弹窗看起来"只有标题、
	 * 没有内容"，实际是文字隐形了，白折腾好几轮。
	 * 固定成浅色以后，弹出框、菜单这些非自绘控件都不会再出现隐形文字。 */
	app.setStyle(QStringLiteral("Fusion"));

	QPalette pal;
	pal.setColor(QPalette::Window, QColor(0xF0, 0xF0, 0xF0));
	pal.setColor(QPalette::WindowText, QColor(0x1F, 0x23, 0x28));
	pal.setColor(QPalette::Base, Qt::white);
	pal.setColor(QPalette::AlternateBase, QColor(0xF7, 0xF8, 0xFA));
	pal.setColor(QPalette::Text, QColor(0x1F, 0x23, 0x28));
	pal.setColor(QPalette::Button, QColor(0xF0, 0xF0, 0xF0));
	pal.setColor(QPalette::ButtonText, QColor(0x1F, 0x23, 0x28));
	pal.setColor(QPalette::ToolTipBase, Qt::white);
	pal.setColor(QPalette::ToolTipText, QColor(0x1F, 0x23, 0x28));
	pal.setColor(QPalette::Highlight, QColor(0x3B, 0x82, 0xF6));
	pal.setColor(QPalette::HighlightedText, Qt::white);
	pal.setColor(QPalette::PlaceholderText, QColor(0x9A, 0xA0, 0xA6));
	app.setPalette(pal);

	/* 不要设置全局 color：那会让 QMessageBox 这类系统控件继承到深色文字，
	 * 在深色背景下看不见。 */
	app.setStyleSheet(QString::fromUtf8(kQss));

	MainWindow w;
	w.show();
	return app.exec();
}
