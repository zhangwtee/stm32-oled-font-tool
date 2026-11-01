// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 zhangwtee <https://github.com/zhangwtee>
// 本文件属于「OLED 点阵取模工具」。改动请保留以上两行并注明改了什么（Apache-2.0 §4b）。
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
			{ "纯 ASCII 里的 tab 转义",   "a\tb", "a\\tb" },
			{ "中英混排里的 tab 转义",    "a\t\xE5\xBC\xA0", "a\\t\\xE5\\xBC\\xA0" },
		};
		for (const Case &c : cases)
		{
			const QString got = MergeProject::cStringOf(QString::fromUtf8(c.in));
			check(c.what, got == QString::fromUtf8(c.want), got);
		}
	}

	std::printf("[字面量分段：汉字紧跟注释]\n");
	{
		/* cLiteralLines 的输出会被原样粘进 main.c，所以这里要钉死三件事：
		 * (1) 相邻字面量拼起来必须等于 cStringOf(整串) —— 拆段不能改变字节；
		 * (2) 注释里汉字前后都有空格 —— armcc 按 GBK 读时汉字第三字节会
		 *     落单吞掉后一字节，吞空格没事，吞掉 '*' 注释就永不结束；
		 * (3) 折行后每行不超过 maxCols，续行缩进 indent 个空格。 */

		/* 把 "..." 里的内容抠出来，跳过多行里的空白与注释 */
		auto litsJoined = [](const QStringList &lines) {
			QString all;
			for (const QString &ln : lines)
			{
				int i = 0;
				while (i < ln.size())
				{
					if (ln.at(i) != QLatin1Char('"'))
					{
						++i;
						continue;
					}
					++i;
					while (i < ln.size() && ln.at(i) != QLatin1Char('"'))
					{
						if (ln.at(i) == QLatin1Char('\\') && i + 1 < ln.size())
						{
							all += ln.at(i);        /* 转义符和它保护的那个字符都算 */
							++i;
						}
						all += ln.at(i);
						++i;
					}
					++i;
				}
			}
			return all;
		};

		const QString cn = QString::fromUtf8("\xE5\xBC\xA0" "\xE6\xB8\xA9");

		const QStringList one = MergeProject::cLiteralLines(cn, 30);
		check("两个汉字有两段", one.size() == 1, QString::number(one.size()));
		check("首段是「张」的转义", one.value(0).contains(QStringLiteral("\"\\xE5\\xBC\\xA0\"")),
		      one.value(0));
		check("后面紧跟汉字注释", one.value(0).contains(QStringLiteral("/* ") + QString::fromUtf8("\xE5\xBC\xA0") + QStringLiteral(" */")),
		      one.value(0));
		check("注释里汉字两侧有空格", 
		      !one.value(0).contains(QStringLiteral("/*") + QString::fromUtf8("\xE5\xBC\xA0")), one.value(0));

		/* 纯 ASCII：一段，不带注释 */
		const QStringList plain = MergeProject::cLiteralLines(QStringLiteral("Num:"), 30);
		check("纯 ASCII 只有一段", plain.size() == 1, plain.join(QLatin1Char('|')));
		check("纯 ASCII 不加注释", !plain.value(0).contains(QStringLiteral("/*")), plain.value(0));

		/* 中英混排：ASCII 合成一段，汉字各一段，拼接后字节不变 */
		const QString mixed = QStringLiteral("T=") + cn;
		const QStringList ml = MergeProject::cLiteralLines(mixed, 30);
		check("混排拼接后字节不变",
		      litsJoined(ml) == MergeProject::cStringOf(mixed), litsJoined(ml));
		check("混排里 ASCII 段没被逐字符拆开",
		      ml.value(0).contains(QStringLiteral("\"T=\"")), ml.value(0));

		/* 长串折行：17 个汉字，首行前缀 30 列，必然折成多行 */
		QString longText;
		for (int i = 0; i < 17; ++i)
		{
			longText += QChar(0x4E00 + i);
		}
		const QStringList ll = MergeProject::cLiteralLines(longText, 30, 4, 80);
		check("长串折成多行", ll.size() > 1, QString::number(ll.size()));
		bool within = true;
		for (int i = 0; i < ll.size(); ++i)
		{
			if (ll.at(i).size() > 80)
			{
				within = false;
			}
			if (i > 0 && !ll.at(i).startsWith(QStringLiteral("    ")))
			{
				within = false;
			}
		}
		check("每行不超 80 列、续行缩进 4 空格", within, ll.join(QLatin1Char('\n')));
		check("折行后字节仍然不变",
		      litsJoined(ll) == MergeProject::cStringOf(longText), litsJoined(ll));

		/* 空串：没有分段，调用方会补一对空引号 */
		check("空串没有分段", MergeProject::cLiteralLines(QString(), 30).isEmpty(), QString());
	}

	std::printf("[行宽溢出下标]\n");
	{
		/* firstOverflowIndex 直接对应驱动 ShowGlyph 的静默 return：
		 * 累加值一超过屏宽，那个字就显示不出来。这里把边界钉死。 */

		/* 16×16，屏宽 128：恰好 8 个汉字填满（8×16=128），第 9 个溢出 */
		QString eight;
		for (int i = 0; i < 8; ++i)
		{
			eight += QChar(0x4E00 + i);
		}
		check("8 个汉字恰好放得下", MergeProject::firstOverflowIndex(eight, 16, 128) == -1,
		      QString::number(MergeProject::firstOverflowIndex(eight, 16, 128)));

		QString nine = eight + QChar(0x4E09);
		check("第 9 个汉字是第一个溢出的", MergeProject::firstOverflowIndex(nine, 16, 128) == 8,
		      QString::number(MergeProject::firstOverflowIndex(nine, 16, 128)));

		/* ASCII 占 8 像素：16 个 ASCII 填满 128，第 17 个溢出 */
		const QString ascii16 = QStringLiteral("0123456789ABCDEF");
		check("16 个 ASCII 恰好放得下",
		      MergeProject::firstOverflowIndex(ascii16, 16, 128) == -1,
		      QString::number(MergeProject::firstOverflowIndex(ascii16, 16, 128)));
		check("第 17 个 ASCII 溢出",
		      MergeProject::firstOverflowIndex(ascii16 + QStringLiteral("G"), 16, 128) == 16,
		      QString::number(MergeProject::firstOverflowIndex(ascii16 + QStringLiteral("G"), 16, 128)));

		/* 中英混排：7 个汉字(112) + 2 个 ASCII(16) = 128，恰好；再加一个字溢出 */
		QString seven;
		for (int i = 0; i < 7; ++i)
		{
			seven += QChar(0x4E00 + i);
		}
		const QString mixOk = seven + QStringLiteral("AB");
		check("7 汉字 + 2 英文 = 128 恰好",
		      MergeProject::firstOverflowIndex(mixOk, 16, 128) == -1,
		      QString::number(MergeProject::firstOverflowIndex(mixOk, 16, 128)));
		check("再加 1 汉字溢出", MergeProject::firstOverflowIndex(mixOk + QChar(0x4E09), 16, 128) == 9,
		      QString::number(MergeProject::firstOverflowIndex(mixOk + QChar(0x4E09), 16, 128)));

		/* 24×24：5 个汉字 120，第 6 个溢出 */
		QString five;
		for (int i = 0; i < 5; ++i)
		{
			five += QChar(0x4E00 + i);
		}
		check("24×24 放 5 个", MergeProject::firstOverflowIndex(five, 24, 128) == -1,
		      QString::number(MergeProject::firstOverflowIndex(five, 24, 128)));
		check("24×24 第 6 个溢出", MergeProject::firstOverflowIndex(five + QChar(0x4E09), 24, 128) == 5,
		      QString::number(MergeProject::firstOverflowIndex(five + QChar(0x4E09), 24, 128)));
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

	/* ---- 适配区改写 ----
	 * 一键接入手顺带把适配区改成探测到的写法。这段盯两件事：
	 *   1. 没探到的项不能动 —— 只探到延时、没探到写命令写法时，
	 *      用户手改对的 OLED_WR_Byte 不能被改回默认的 OLED_WriteCommand。
	 *   2. 用户自己加的行尾注释不能抹掉。 */
	{
		const QString dir    = root + QStringLiteral("/adapter");
		const QString header = dir + QStringLiteral("/OLED_CFont.h");

		const QByteArray userEdited =
			"#ifndef OLED_CFONT_H\n"
			"#define OLED_CFONT_H\n"
			"#define CF_WRITE_CMD(c)                 OLED_WR_Byte((c), 1)   /* 照我驱动改的 */\n"
			"#define CF_WRITE_DATA(d)                OLED_WR_Byte((d), 0)\n"
			"#define CF_SHOW_ASCII(line, col, ch)    OLED_ShowChar((line), (col), (ch))\n"
			"#endif\n";

		bool    bak  = false;
		QString aerr;

		/* ① 只探到延时，写命令写法没探到 */
		writeFile(header, userEdited);
		LibProbe::CodeAdapter part;
		part.detected = true;
		part.setDelay(QStringLiteral("Delay_ms(ms)"));
		part.origin = QStringLiteral("标准库 · 延时 Delay_ms(ms)");
		const int n1 = MergeProject::applyAdapter(header, part, &bak, &aerr);

		const QByteArray a1 = readFile(header);
		check("没探到写命令 -> 用户改好的那行原样不动",
		      a1 == userEdited, QString::fromUtf8(a1));
		check("只探到延时时一行都不改", n1 == 0, QString::number(n1));
		check("没改动就不留备份", !bak, QString());

		/* ② 探到写命令写法：该改的要改对，注释要留着 */
		const QByteArray stale =
			"#ifndef OLED_CFONT_H\n"
			"#define OLED_CFONT_H\n"
			"#define CF_WRITE_CMD(c)                 OLED_WriteCommand(c)   /* 待改 */\n"
			"#define CF_WRITE_DATA(d)                OLED_WriteData(d)\n"
			"#define CF_SHOW_ASCII(line, col, ch)    OLED_ShowChar((line), (col), (ch))\n"
			"#endif\n";
		writeFile(header, stale);

		LibProbe::CodeAdapter full;
		full.detected = true;
		full.setWrite(QStringLiteral("OLED_WR_Byte((c), 1)"),
		              QStringLiteral("OLED_WR_Byte((d), 0)"));
		full.setShowAscii(QStringLiteral("OLED_ShowChar((line), (col), (ch))"));
		full.setDelay(QStringLiteral("Delay_ms(ms)"));
		full.origin = QStringLiteral("标准库");
		bak = false;
		aerr.clear();
		const int n2 = MergeProject::applyAdapter(header, full, &bak, &aerr);

		const QString s2 = QString::fromUtf8(readFile(header));
		check("探到写命令 -> 换成 WR_Byte 写法",
		      s2.contains(QStringLiteral("OLED_WR_Byte((c), 1)")), s2);
		check("换写法时行尾注释保留",
		      s2.contains(QStringLiteral("/* 待改 */")), s2);
		check("命令 + 数据两行都改了", n2 == 2, QString::number(n2));
		check("改完留了 .oledbak 备份", bak, QString());

		/* ③ CRLF 的文件改完还得是 CRLF，不能变成 \r\r\n */
		QByteArray crlf = stale;
		crlf.replace("\n", "\r\n");
		writeFile(header, crlf);
		MergeProject::applyAdapter(header, full, &bak, &aerr);
		const QByteArray a3 = readFile(header);
		check("CRLF 文件改完还是 CRLF",
		      a3.contains("OLED_WR_Byte((c), 1)") && !a3.contains("\r\r"),
		      QStringLiteral("含 \\r\\r：%1").arg(a3.contains("\r\r") ? QStringLiteral("是")
		                                                               : QStringLiteral("否")));

		/* ④ 没探测到：一个字节都不许动 */
		writeFile(header, stale);
		LibProbe::CodeAdapter none;
		const int n4 = MergeProject::applyAdapter(header, none, &bak, &aerr);
		const QByteArray a4 = readFile(header);
		check("没探测到 -> 一个字节都没动", a4 == stale && n4 == 0, QString::number(n4));
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
