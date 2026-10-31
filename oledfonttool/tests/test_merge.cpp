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

	std::printf("[撤销的边界]\n");
	{
		/* (a) 目标文件被人删了、只剩备份。
		 * 老实现先 QFile::remove(f)：文件本来就不在，remove 返回 false，
		 * 于是直接报"删不掉 xxx，可能被 Keil 占用了" —— 备份明明就在旁边，
		 * 却撤销不了，用户只能手动改名。 */
		const QString bak = mainC + QStringLiteral(".oledbak");
		const QString tmp = mainC + QStringLiteral(".oledtmp");

		writeFile(mainC, mainBytes);
		QFile::remove(bak);
		QFile::remove(tmp);
		const bool copied = QFile::copy(mainC, bak);
		check("造出备份", copied, bak);
		QFile::remove(mainC);

		QString e1;
		const bool ok1 = MergeProject::revert(QStringList() << mainC, &e1);
		check("目标被删、只有备份 -> 能还原", ok1, e1);
		check("还原出来的内容和原始字节一致", readFile(mainC) == mainBytes,
		      QStringLiteral("长度 %1").arg(readFile(mainC).size()));
		check("还原后备份已清掉", !QFileInfo::exists(bak), bak);
		check("不留 .oledtmp 残留", !QFileInfo::exists(tmp), tmp);

		/* (b) 没有备份的文件：revert 应该安静跳过，一个字节都不动。
		 * "撤销"是整批处理的，其中一个文件没备份不该让其余的都失败。 */
		QFile::remove(bak);
		const QByteArray keep = readFile(mainC);
		QString e2;
		const bool ok2 = MergeProject::revert(QStringList() << mainC, &e2);
		check("没有备份 -> 返回成功", ok2, e2);
		check("没有备份 -> 文件没被动过", readFile(mainC) == keep, QString());
	}

	std::printf("[UTF-8 无 BOM 时自动补 BOM]\n");
	{
		/* 现场：Keil 里 Edit → Configuration → Encoding 选 UTF-8 之后保存的文件
		 * 是不带 BOM 的。「一键接入」往里写中文示例，armcc 按 GBK 解析，
		 * 汉字三字节的最后一字节把引号吞掉，报 #8: missing closing quote。
		 * 所以写回时看到"UTF-8 + 内容里有非 ASCII + 原来没 BOM"就补一个。 */
		const QByteArray plainNoBom("#include \"stm32f10x.h\"\n"
		                            "\n"
		                            "int main(void)\n"
		                            "{\n"
		                            "    OLED_Init();\n"
		                            "}\n");

		const QString nobom = root + QStringLiteral("/User/main2.c");
		writeFile(nobom, plainNoBom);

		MergeProject::Scan scan;
		scan.root = root;
		MergeProject::Step s;
		s.file      = nobom;
		s.afterLine = 5;
		s.text      = QStringLiteral("\tOLED_CFont_ShowString( 1, 1, \"张\" );");
		s.title     = QStringLiteral("测试插入中文");
		s.checked   = true;
		scan.steps << s;

		QStringList touched;
		QString err;
		const bool ok = MergeProject::apply(scan, &touched, &err);
		check("往无 BOM 的 UTF-8 文件里写中文 -> 成功", ok, err);

		const QByteArray after = readFile(nobom);
		check("写回后补上了 BOM", after.startsWith("\xEF\xBB\xBF"),
		      QString::fromLatin1(after.left(6).toHex(' ')));
		check("BOM 只出现一次", after.count("\xEF\xBB\xBF") == 1,
		      QString::number(after.count("\xEF\xBB\xBF")));
		check("中文按 UTF-8 写入（E5 BC A0）", after.contains("\xE5\xBC\xA0"), QString());
		check("BOM 后面还是原来的第一行",
		      after.mid(3).startsWith("#include \"stm32f10x.h\""),
		      QString::fromUtf8(after.left(40)));

		/* 纯 ASCII 的文件不许动 —— 老工程不该平白多出三个字节 */
		const QString ascii = root + QStringLiteral("/User/main3.c");
		writeFile(ascii, plainNoBom);

		MergeProject::Scan scan2;
		scan2.root = root;
		MergeProject::Step s2;
		s2.file      = ascii;
		s2.afterLine = 0;
		s2.text      = QStringLiteral("#include \"OLED_CFont.h\"");
		s2.title     = QStringLiteral("测试插入 ASCII");
		s2.checked   = true;
		scan2.steps << s2;

		QStringList t2;
		QString e2;
		check("纯 ASCII 文件插入成功", MergeProject::apply(scan2, &t2, &e2), e2);

		const QByteArray a2 = readFile(ascii);
		check("纯 ASCII 文件不加 BOM", !a2.startsWith("\xEF\xBB\xBF"),
		      QString::fromLatin1(a2.left(6).toHex(' ')));
		check("纯 ASCII 文件里新行在第一行",
		      a2.startsWith("#include \"OLED_CFont.h\""), QString::fromUtf8(a2.left(40)));
	}

	std::printf("[源码编码归类]\n");
	{
		/* 两类失败后果完全不同，分错了界面就会给错建议：
		 *   存成 GBK        -> 编译能过、查表对不上、屏幕全空
		 *   UTF-8 但没 BOM  -> 查表对得上、armcc 报 #8 编不过
		 * 这一组把边界钉死。以前这段逻辑就地写在界面函数里，没有测试，
		 * 漏了"缺 BOM"那一支，结果就是"明明编不过，界面还打勾"。 */
		const QByteArray utf8NoBom("#include \"x.h\"\nvoid f(void) { g(\"\xE5\xBC\xA0\"); }\n");
		QByteArray utf8Bom("\xEF\xBB\xBF");
		utf8Bom.append(utf8NoBom);
		const QByteArray gbk("#include \"x.h\"\nvoid f(void) { g(\"\xD5\xC5\"); }\n");
		const QByteArray ascii("#include \"x.h\"\nvoid f(void) { g(\"abc\"); }\n");
		const QByteArray commentOnly("/* \xE5\xBC\xA0\xE6\xB8\xA9 */\nvoid f(void) { }\n");

		using MergeProject::SrcIssue;

		check("UTF-8 无 BOM + 中文字面量 -> NoBom",
		      MergeProject::judgeSource(utf8NoBom, true) == SrcIssue::NoBom, QString());
		check("UTF-8 带 BOM -> 没问题",
		      MergeProject::judgeSource(utf8Bom, true) == SrcIssue::None, QString());
		check("GBK -> NotUtf8（是查表失败，不是缺 BOM）",
		      MergeProject::judgeSource(gbk, true) == SrcIssue::NotUtf8, QString());
		check("纯 ASCII 文件 -> 没问题",
		      MergeProject::judgeSource(ascii, false) == SrcIssue::None, QString());
		check("中文只在注释里 -> 不算问题",
		      MergeProject::judgeSource(commentOnly, false) == SrcIssue::None, QString());
		check("GBK 文件但没中文字面量 -> 不算问题",
		      MergeProject::judgeSource(gbk, false) == SrcIssue::None, QString());
		check("空文件 -> 没问题",
		      MergeProject::judgeSource(QByteArray(), false) == SrcIssue::None, QString());
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
