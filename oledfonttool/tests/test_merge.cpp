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

	std::printf("[示例串按字节转义，源码里不留中文]\n");
	{
		/* 现场：Keil 里 Edit → Configuration → Encoding 选 UTF-8，保存出来的
		 * 文件不带 BOM；在 Keil 里改一下示例行的坐标再存，连原来补好的 BOM
		 * 也会被去掉。带中文的字符串字面量这种文件 armcc 按 GBK 解析，
		 * 汉字三字节的最后一字节把引号吞掉 -> #8: missing closing quote。
		 * 所以示例串一律写成 "\xE5\xBC\xA0" 这种字节转义：
		 * 屏幕上的字节还是 UTF-8 三字节，但源码里一个中文字节都没有，
		 * BOM 丢不丢都不影响编译。 */
		const QString bak = mainC + QStringLiteral(".oledbak");
		QFile::remove(bak);
		/* 用「Keil 存出来的样子」——没有 BOM —— 做底稿，
		 * 这样才能验证"写回去也不会平白多出 BOM" */
		QByteArray plainNoBom = mainBytes;
		plainNoBom.remove(0, 3);
		writeFile(mainC, plainNoBom);

		MergeProject::Scan scan;
		const bool ok = MergeProject::detect(fontH, QStringLiteral("张"),
		                                     QStringLiteral("李"), &scan);
		check("detect 成功（带中文示例）", ok, scan.error);

		QString sampleStep;
		for (const MergeProject::Step &s : scan.steps)
		{
			if (s.text.contains(QLatin1String("OLED_CFont_ShowString")))
			{
				sampleStep = s.text;
				break;
			}
		}
		check("找到了示例行", !sampleStep.isEmpty(), sampleStep);

		bool asciiOnly = true;
		for (const QChar &c : sampleStep)
		{
			if (c.unicode() > 127) { asciiOnly = false; break; }
		}
		check("示例行全是 ASCII", asciiOnly, sampleStep);
		check("「张」写成了 \\xE5\\xBC\\xA0",
		      sampleStep.contains(QLatin1String("\"\\xE5\\xBC\\xA0\"")), sampleStep);
		check("「李」写成了 \\xE6\\x9D\\x8E",
		      sampleStep.contains(QLatin1String("\"\\xE6\\x9D\\x8E\"")), sampleStep);

		QStringList touched;
		QString err;
		check("apply 成功", MergeProject::apply(scan, &touched, &err), err);

		/* 落盘后文件必须是纯 ASCII（BOM 都不该补），但程序里的字符串字节不变 */
		const QByteArray after = readFile(mainC);
		bool fileAscii = true;
		for (const char ch : after)
		{
			if (static_cast<unsigned char>(ch) > 127) { fileAscii = false; break; }
		}
		check("写回后 main.c 里没有非 ASCII 字节", fileAscii,
		      QStringLiteral("长度 %1").arg(after.size()));
		check("纯 ASCII 文件不加 BOM", !after.startsWith("\xEF\xBB\xBF"),
		      QString::fromLatin1(after.left(6).toHex(' ')));
		check("转义后仍是 E5 BC A0 这段文本",
		      after.contains("\\xE5\\xBC\\xA0"), QString::fromUtf8(after.left(200)));

		/* 中文后面紧跟十六进制字符是转义的天敌："\xA0B" 会被读成 0xA0B，
		 * 必须先把字符串断开。这里用「张A」把这条钉死。 */
		writeFile(mainC, plainNoBom);       /* 上一轮已经写进示例行，先还原 */
		MergeProject::Scan s3;
		const bool ok3 = MergeProject::detect(fontH, QStringLiteral("张A"), QString(), &s3);
		QString step3;
		for (const MergeProject::Step &s : s3.steps)
		{
			if (s.text.contains(QLatin1String("OLED_CFont_ShowString"))) step3 = s.text;
		}
		check("detect 成功（中文后面跟 ASCII）", ok3, s3.error);
		check("转义后紧跟 hex 字符时断开字符串",
		      step3.contains(QLatin1String("\"\\xE5\\xBC\\xA0\"\"A\"")), step3);

		QFile::remove(bak);
		writeFile(mainC, mainBytes);
	}

	std::printf("[C 字符串转义]\n");
	{
		/* 界面上的「复制为 C 字符串」直接调这个函数，所以它得自己保证
		 * 结果能原样粘进 .c：中文逐字节转义、引号/反斜杠转义、
		 * \x 后面紧跟十六进制字符时要断开字符串。 */
		struct Case
		{
			const char *what;
			const char *in;
			const char *want;
		};
		const Case cases[] = {
			{ "纯 ASCII 原样返回",     "AB12",  "AB12" },
			{ "一个汉字",              "\xE5\xBC\xA0", "\\xE5\\xBC\\xA0" },
			{ "中英混排",              "A\xE5\xBC\xA0", "A\\xE5\\xBC\\xA0" },
			{ "中文后跟十六进制字符",  "\xE5\xBC\xA0" "A", "\\xE5\\xBC\\xA0" "\"\"A" },
			{ "双引号要转义",          "a\"b", "a\\\"b" },
			{ "反斜杠要转义",          "a\\b", "a\\\\b" },
		};
		for (const Case &c : cases)
		{
			const QString got = MergeProject::cStringOf(QString::fromUtf8(c.in));
			check(c.what, got == QString::fromUtf8(c.want), got);
		}
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

	std::printf("[一键接入：include 写法跟工程一致]\n");
	{
		/* 现场的困惑：「main.c 里写了 #include "oled.h"，工具又塞进来一行
		 * #include "../Hardware/OLED_CFont.h"，看着像重复了两遍。」
		 *
		 * 判据就在 main.c 自己身上：它有一条不带路径的短名 include，而那个
		 * 头文件就在字库同目录 —— 这条能编过，说明该目录在 Include Path 上，
		 * 字库也用短名就对了。没有这条依据时仍写相对路径（一定编得过）。
		 */
		writeFile(mainC, mainBytes);        /* mainBytes 里有 #include "oled.h" */

		MergeProject::Scan scan;
		const bool ok = MergeProject::detect(fontH, QStringLiteral("A"),
		                                     QStringLiteral("B"), &scan);
		check("detect 成功", ok, scan.error);

		QString inc;
		for (const MergeProject::Step &s : scan.steps)
		{
			if (s.title.contains(QStringLiteral("补 #include")))
			{
				inc = s.text;
			}
		}
		check("同目录头文件已用短名 -> 字库也用短名",
		      inc == QStringLiteral("#include \"OLED_CFont.h\""), inc);
	}

	std::printf("[一键接入：没有短名依据时退回相对路径]\n");
	{
		/* 另造一个工程：main.c 里全是带路径的 include，说明 Include Path 没配
		 * 或者不包含 Hardware —— 这时候必须写相对路径，短名会直接找不到文件。 */
		const QString root2 = base + QStringLiteral("/mergecase2");
		QDir().mkpath(root2 + QStringLiteral("/Hardware"));
		QDir().mkpath(root2 + QStringLiteral("/User"));

		const QString c2 = root2 + QStringLiteral("/Hardware/oled.c");
		const QString h2 = root2 + QStringLiteral("/Hardware/oled.h");
		const QString m2 = root2 + QStringLiteral("/User/main.c");
		const QString f2 = root2 + QStringLiteral("/Hardware/OLED_CFont.h");

		writeFile(c2,
		          "#include \"../Hardware/oled.h\"\n"
		          "void OLED_WriteCommand(uint8_t cmd)\n"
		          "{\n"
		          "    (void)cmd;\n"
		          "}\n"
		          "void OLED_WriteData(uint8_t d)\n"
		          "{\n"
		          "    (void)d;\n"
		          "}\n"
		          "void OLED_Init(void)\n"
		          "{\n"
		          "}\n");
		writeFile(h2, "#ifndef __OLED_H\n#define __OLED_H\nvoid OLED_Init(void);\n#endif\n");
		writeFile(m2,
		          "#include \"../Hardware/oled.h\"\n"
		          "\n"
		          "int main(void)\n"
		          "{\n"
		          "    OLED_Init();\n"
		          "    while (1) { }\n"
		          "}\n");
		writeFile(f2, "#ifndef __OLED_CFONT_H\n#define __OLED_CFONT_H\n#endif\n");

		MergeProject::Scan scan;
		const bool ok = MergeProject::detect(f2, QString(), QString(), &scan);
		check("detect 成功（第二个工程）", ok, scan.error);

		QString inc;
		for (const MergeProject::Step &s : scan.steps)
		{
			if (s.title.contains(QStringLiteral("补 #include")))
			{
				inc = s.text;
			}
		}
		check("没有短名依据 -> 写相对路径",
		      inc == QStringLiteral("#include \"../Hardware/OLED_CFont.h\""), inc);
	}

	std::printf("[素材包接入]\n");
	{
		const QString pack = root + QStringLiteral("/Hardware/oled_assets.h");
		writeFile(pack,
		          "#ifndef __OLED_ASSETS_H\n"
		          "#define __OLED_ASSETS_H\n"
		          "#include <stdint.h>\n"
		          "static void Logo_ShowFrame(uint8_t i) { (void)i; }\n"
		          "static void Miao_ShowFrame(uint8_t i) { (void)i; }\n"
		          "#endif\n");

		const QStringList funcs = MergeProject::listAssetShowFuncs(pack);
		check("列出素材包里的显示函数", funcs.size() == 2,
		      funcs.join(QStringLiteral(", ")));
		check("认出两个 _ShowFrame（不会把写命令函数也算进来）",
		      funcs.contains(QStringLiteral("Logo_ShowFrame"))
		          && funcs.contains(QStringLiteral("Miao_ShowFrame")),
		      funcs.join(QStringLiteral(", ")));

		writeFile(mainC, mainBytes);        /* 回到干净现场 */
		const int initLn = 6;               /* mainBytes：1-2 include，3 空，4 main，5 {，6 Init */

		MergeProject::Scan scan;
		const bool ok = MergeProject::detectAssets(pack, QStringLiteral("Miao_ShowFrame"),
		                                           &scan);
		check("detectAssets 成功", ok, scan.error);

		QString inc;
		int     showLine = -1;
		QString showText;
		for (const MergeProject::Step &s : scan.steps)
		{
			if (s.title.contains(QStringLiteral("补 #include")))
			{
				inc = s.text;
			}
			else if (s.title.contains(QStringLiteral("示例")))
			{
				showLine = s.afterLine;
				showText = s.text;
			}
		}
		check("素材包也用短名（和 oled.h 一致）",
		      inc == QStringLiteral("#include \"oled_assets.h\""), inc);
		check("示例插在 OLED_Init() 之后", showLine == initLn, QString::number(showLine));
		check("示例是刷第 0 帧",
		      showText.contains(QStringLiteral("Miao_ShowFrame(0)")), showText);

		/* 幂等：已经接过一次（include 和调用都在），就不该再插第二遍 */
		QByteArray done("\xEF\xBB\xBF");
		done.append("#include \"stm32f10x.h\"\n"
		            "#include \"oled.h\"\n"
		            "#include \"oled_assets.h\"\n"
		            "\n"
		            "int main(void)\n"
		            "{\n"
		            "    OLED_Init();\n"
		            "    Miao_ShowFrame(0);\n"
		            "    while (1) { }\n"
		            "}\n");
		writeFile(mainC, done);

		MergeProject::Scan scan2;
		const bool ok2 = MergeProject::detectAssets(pack, QStringLiteral("Miao_ShowFrame"),
		                                            &scan2);
		check("已接过一次时仍能检测成功", ok2, scan2.error);

		bool hasInc = false;
		bool hasShow = false;
		for (const MergeProject::Step &s : scan2.steps)
		{
			if (s.title.contains(QStringLiteral("补 #include")))
			{
				hasInc = true;
			}
			if (s.title.contains(QStringLiteral("示例")))
			{
				hasShow = true;
			}
		}
		check("已接过 -> 不再补 include", !hasInc, QString());
		check("已接过 -> 不再补示例", !hasShow, QString());

		/* 素材包不存在：要说清楚下一步做什么，而不是抛个空错误 */
		MergeProject::Scan scan3;
		const bool ok3 = MergeProject::detectAssets(root + QStringLiteral("/没有这个.h"),
		                                            QString(), &scan3);
		check("素材包不存在 -> 返回失败", !ok3, QString());
		check("失败时给了人话提示",
		      scan3.error.contains(QStringLiteral("全部存进一个")), scan3.error);
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
