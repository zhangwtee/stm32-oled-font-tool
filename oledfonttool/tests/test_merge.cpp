/* MergeProject（一键接入）的回归测试。
 *
 * 这个模块会真的改用户工程里的文件，所以重点不在"功能对不对"，
 * 而在"改完之后文件还是不是好的"：
 *   · 带 BOM 的文件，插入点算成 0 时 BOM 会不会被挤到第二行
 *   · 插进去的内容有没有落到该落的位置
 *   · 撤销能不能原样回来
 *
 * 另外验证驱动识别能接受同行花括号的写法（void f(u8 x) { ... }）。
 */
#include "../src/mergeproject.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDir>

#include <cstdio>

static int g_pass = 0;
static int g_fail = 0;

static void check(const char *what, bool ok, const QString &got)
{
	if (ok)
	{
		++g_pass;
		std::printf("  ok    %s\n", what);
	}
	else
	{
		++g_fail;
		std::printf("  FAIL  %s   -> %s\n", what, got.toLocal8Bit().constData());
	}
}

static bool writeFile(const QString &path, const QByteArray &data)
{
	QDir().mkpath(QFileInfo(path).absolutePath());
	QFile f(path);
	if (!f.open(QIODevice::WriteOnly))
	{
		return false;
	}
	f.write(data);
	f.close();
	return true;
}

static QByteArray readFile(const QString &path)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
	{
		return QByteArray();
	}
	return f.readAll();
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);

	/* 不用 QTemporaryDir：某些受限环境下它在系统临时目录里建不出来，
	 * 测试会直接跳过。改成 exe 旁边的固定目录，每次跑前清空。 */
	const QString base = QCoreApplication::applicationDirPath()
	                     + QStringLiteral("/merge_tmp");
	QDir(base).removeRecursively();
	if (!QDir().mkpath(base))
	{
		std::printf("建不出临时目录：%s\n", base.toLocal8Bit().constData());
		return 1;
	}

	const QString root = base + QStringLiteral("/mergecase");

	/* ---- 造一个最小工程：驱动用「同行花括号」，main.c 带 UTF-8 BOM ---- */
	const QString oledC = root + QStringLiteral("/Hardware/oled.c");
	const QString oledH = root + QStringLiteral("/Hardware/oled.h");
	const QString mainC = root + QStringLiteral("/User/main.c");
	const QString fontH = root + QStringLiteral("/Hardware/OLED_CFont.h");

	writeFile(oledC,
	          "/* SSD1306 128x64 */\n"
	          "#include \"oled.h\"\n"
	          "\n"
	          "void OLED_WriteCommand(uint8_t cmd)\n"      /* 这种是次行花括号 */
	          "{\n"
	          "    OLED_CMD = cmd;\n"
	          "}\n"
	          "\n"
	          "void OLED_WriteData(uint8_t data) {\n"      /* 这种是同行花括号 */
	          "    OLED_DAT = data;\n"
	          "}\n"
	          "\n"
	          "void OLED_Init(void)\n"
	          "{\n"
	          "    OLED_WriteCommand(0xAE);\n"
	          "}\n");

	writeFile(oledH,
	          "#ifndef __OLED_H\n"
	          "#define __OLED_H\n"
	          "\n"
	          "void OLED_Init(void);\n"
	          "\n"
	          "#endif\n");

	/* BOM + 首行就是 #include：Keil 新建 main.c 的典型样子，
	 * 也正是"插入点算成 0、BOM 被挤走"最容易发生的情形 */
	QByteArray mainBytes("\xEF\xBB\xBF");
	mainBytes.append("#include \"stm32f10x.h\"\n"
	                 "#include \"oled.h\"\n"
	                 "\n"
	                 "int main(void)\n"
	                 "{\n"
	                 "    OLED_Init();\n"
	                 "    while (1) { }\n"
	                 "}\n");
	writeFile(mainC, mainBytes);

	writeFile(fontH,
	          "#ifndef __OLED_CFONT_H\n"
	          "#define __OLED_CFONT_H\n"
	          "\n"
	          "void OLED_CFont_ShowString(int x, int y, const char *s);\n"
	          "\n"
	          "#endif\n");

	/* ---- 1. 驱动识别 ---- */
	std::printf("[驱动识别]\n");
	{
		MergeProject::Scan scan;
		const bool ok = MergeProject::detect(fontH, QStringLiteral("S1"),
		                                    QStringLiteral("S2"), &scan);
		check("detect 成功", ok, scan.error);
		check("认出了写命令函数", scan.cmdFunc == QStringLiteral("OLED_WriteCommand"),
		      scan.cmdFunc);
		check("认出了写数据函数（同行花括号）",
		      scan.dataFunc == QStringLiteral("OLED_WriteData"), scan.dataFunc);
	}

	/* ---- 2. 往带 BOM 的文件里插到最前面 ---- */
	std::printf("[BOM 不被挤走]\n");
	{
		const QString bak = mainC + QStringLiteral(".oledbak");
		QFile::remove(bak);

		MergeProject::Scan scan;
		scan.root = root;

		MergeProject::Step s;
		s.file      = mainC;
		s.afterLine = 0;                    /* 插到文件最前面，最容易踩到 BOM */
		s.text      = QStringLiteral("#include \"OLED_CFont.h\"");
		s.title     = QStringLiteral("测试插入");
		s.checked   = true;
		scan.steps << s;

		QStringList touched;
		QString err;
		const bool ok = MergeProject::apply(scan, &touched, &err);
		check("apply 成功", ok, err);

		const QByteArray after = readFile(mainC);
		const int bomCount = after.count("\xEF\xBB\xBF");

		check("文件以 BOM 开头", after.startsWith("\xEF\xBB\xBF"),
		      QStringLiteral("前 6 字节: %1")
		          .arg(QString::fromLatin1(after.left(6).toHex(' '))));
		check("BOM 只出现一次", bomCount == 1,
		      QStringLiteral("出现 %1 次").arg(bomCount));
		check("新行插在第 1 行",
		      QString::fromUtf8(after.mid(3)).startsWith(
		          QStringLiteral("#include \"OLED_CFont.h\"")),
		      QString::fromUtf8(after.left(80)));
		check("原来的 #include 还在",
		      after.contains("#include \"stm32f10x.h\""), QString());

		/* ---- 3. 撤销还原 ---- */
		std::printf("[撤销]\n");
		QString rerr;
		const bool rok = MergeProject::revert(touched, &rerr);
		check("revert 成功", rok, rerr);

		const QByteArray back = readFile(mainC);
		check("撤销后与原始字节完全一致", back == mainBytes,
		      QStringLiteral("长度 %1 -> %2").arg(mainBytes.size()).arg(back.size()));
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
