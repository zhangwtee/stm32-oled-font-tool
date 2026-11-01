/* LibProbe（工程底子探测）+ 适配区改写的测试。
 *
 * 这一块的错法很隐蔽：探错了不会崩，只会让导出的头文件在用户机器上
 * 编不过 —— 报的还是「未定义的标识符」，指不到取模工具头上。所以每条
 * 判据都用真实的工程片段试一遍，正反都要过：
 *
 *   · 标准库工程（Library/stm32f10x.h + RCC_APB2PeriphClockCmd + Delay_ms）
 *   · HAL 工程（stm32f1xx_hal.h + HAL_Init + HAL_Delay）
 *   · 正点原子那套（OLED_WR_Byte 带 cmd 标志位）
 *   · 什么都没有的目录 —— 必须老实说不知道，而不是瞎猜
 *
 * 改写部分重点在「改完文件还是不是好的」：BOM、CRLF、对齐空格都不能动。
 */
#include "../src/libprobe.h"
#include "../src/mergeproject.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

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

static void writeFile(const QString &path, const QByteArray &data)
{
	QDir().mkpath(QFileInfo(path).absolutePath());
	QFile f(path);
	if (!f.open(QIODevice::WriteOnly))
	{
		std::printf("  !! 写不了 %s\n", path.toLocal8Bit().constData());
		return;
	}
	f.write(data);
	f.close();
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

/* 标准库工程：最常见的课堂配置 —— Library/ 下是标准外设库，
 * System/Delay.c 自己定义了 Delay_ms，驱动用两个函数收发命令 */
static void testStdLibProject(const QString &root)
{
	const QString dir = root + QStringLiteral("/std");
	writeFile(dir + QStringLiteral("/Library/stm32f10x.h"),
	          "/* 标准外设库 */\n");
	writeFile(dir + QStringLiteral("/System/Delay.c"),
	          "#include \"stm32f10x.h\"\n"
	          "void Delay_us(uint32_t xus)\n{\n}\n"
	          "void Delay_ms(uint32_t xms)\n{\n}\n");
	writeFile(dir + QStringLiteral("/Hardware/OLED.c"),
	          "#include \"stm32f10x.h\"\n"
	          "void OLED_WriteCommand(uint8_t c)\n{\n}\n"
	          "void OLED_WriteData(uint8_t d)\n{\n}\n"
	          "void OLED_ShowChar(uint8_t Line, uint8_t Column, char Char)\n{\n}\n"
	          "void OLED_Init(void)\n{\n"
	          "    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);\n}\n");

	const LibProbe::Result r = LibProbe::probe(dir);
	check("标准库：认出是标准库", r.lib == LibProbe::StandardPeriph,
	      LibProbe::nameOf(r.lib));
	check("标准库：延时用 Delay_ms(ms)", r.delayCall == QStringLiteral("Delay_ms(ms)"),
	      r.delayCall);
	check("标准库：写命令是两个函数", r.write == LibProbe::TwoFuncs,
	      r.cmdFunc + QLatin1Char('/') + r.dataFunc);
	check("标准库：抓到 OLED_WriteCommand",
	      r.cmdFunc == QStringLiteral("OLED_WriteCommand"), r.cmdFunc);
	check("标准库：抓到 OLED_ShowChar",
	      r.showAsciiFunc == QStringLiteral("OLED_ShowChar"), r.showAsciiFunc);

	const LibProbe::CodeAdapter a = LibProbe::adapterOf(r);
	check("标准库：适配区延时不是 HAL_Delay",
	      a.delayCall != QStringLiteral("HAL_Delay(ms)"), a.delayCall);
	check("标准库：适配区写命令对得上",
	      a.writeCmd == QStringLiteral("OLED_WriteCommand(c)"), a.writeCmd);
	check("标准库：注释里写了识别依据", a.origin.contains(QStringLiteral("标准库")),
	      a.origin);
}

/* HAL 工程 */
static void testHalProject(const QString &root)
{
	const QString dir = root + QStringLiteral("/hal");
	writeFile(dir + QStringLiteral("/Core/Inc/stm32f1xx_hal.h"),
	          "/* HAL */\n");
	writeFile(dir + QStringLiteral("/Core/Src/main.c"),
	          "#include \"stm32f1xx_hal.h\"\n"
	          "int main(void)\n{\n"
	          "    HAL_Init();\n"
	          "    __HAL_RCC_GPIOB_CLK_ENABLE();\n"
	          "    HAL_Delay(10);\n"
	          "}\n");

	const LibProbe::Result r = LibProbe::probe(dir);
	check("HAL：认出是 HAL 库", r.lib == LibProbe::HAL, LibProbe::nameOf(r.lib));
	check("HAL：延时用 HAL_Delay(ms)", r.delayCall == QStringLiteral("HAL_Delay(ms)"),
	      r.delayCall);
	check("HAL：没认到写命令就不乱填", r.write == LibProbe::WriteUnknown,
	      QString::number(static_cast<int>(r.write)));
}

/* 正点原子那套：一个函数加 cmd 标志位 */
static void testFlagByteProject(const QString &root)
{
	const QString dir = root + QStringLiteral("/flag");
	writeFile(dir + QStringLiteral("/HARDWARE/OLED/oled.c"),
	          "void OLED_WR_Byte(uint8_t dat, uint8_t cmd)\n{\n}\n"
	          "void OLED_ShowChar(uint8_t Line, uint8_t Column, char Char)\n{\n}\n");

	const LibProbe::Result r = LibProbe::probe(dir);
	check("标志位：认出一个函数加 cmd", r.write == LibProbe::FlagByte, r.byteFunc);

	const LibProbe::CodeAdapter a = LibProbe::adapterOf(r);
	check("标志位：写命令展开成 ((c), 1)",
	      a.writeCmd == QStringLiteral("OLED_WR_Byte((c), 1)"), a.writeCmd);
	check("标志位：写数据展开成 ((d), 0)",
	      a.writeData == QStringLiteral("OLED_WR_Byte((d), 0)"), a.writeData);
}

/* 认不出来时要老实 */
static void testEmptyProject(const QString &root)
{
	const QString dir = root + QStringLiteral("/empty");
	writeFile(dir + QStringLiteral("/main.c"), "int main(void)\n{\n}\n");

	const LibProbe::Result r = LibProbe::probe(dir);
	check("空工程：什么都没认出来", !r.any(), r.evidenceOneLine);

	const LibProbe::CodeAdapter a = LibProbe::adapterOf(r);
	check("空工程：适配区回到老默认值", !a.detected && a.delayCall == QStringLiteral("HAL_Delay(ms)"),
	      a.delayCall);
}

/* 改写：BOM + CRLF + 对齐空格都要保住 */
static void testRewrite(const QString &root)
{
	const QString dir = root + QStringLiteral("/rewrite");
	QDir().mkpath(dir);

	const QString header = dir + QStringLiteral("/OLED_CFont.h");
	const QByteArray original =
	        QByteArray("\xEF\xBB\xBF")                     /* UTF-8 BOM */
	        + "#ifndef __OLED_CFONT_H\r\n"
	          "#define __OLED_CFONT_H\r\n"
	          "#define CF_WRITE_CMD(c)                 OLED_WriteCommand(c)\r\n"
	          "#define CF_WRITE_DATA(d)                OLED_WriteData(d)\r\n"
	          "#define CF_SHOW_ASCII(line, col, ch)    OLED_ShowChar((line), (col), (ch))\r\n"
	          "#endif\r\n";
	writeFile(header, original);

	/* 用「标志位 + Delay_ms」这套结果去改 */
	const LibProbe::Result r = ([] {
		LibProbe::Result x;
		x.write    = LibProbe::FlagByte;
		x.byteFunc = QStringLiteral("OLED_WR_Byte");
		x.showAsciiFunc = QStringLiteral("OLED_ShowChar");
		x.delayCall = QStringLiteral("Delay_ms(ms)");
		return x;
	})();
	const LibProbe::CodeAdapter a = LibProbe::adapterOf(r);

	const QStringList before = MergeProject::adapterMismatch(header, a);
	check("改写前：体检报出不一致", before.size() == 2, QString::number(before.size()));

	bool bak = false;
	QString err;
	const int fixed = MergeProject::applyAdapter(header, a, &bak, &err);
	check("改写：改了两行", fixed == 2, QString::number(fixed) + QLatin1Char(' ') + err);
	check("改写：生成了备份", bak, QString());

	const QByteArray after = readFile(header);
	check("改写：写命令换了",
	      after.contains("OLED_WR_Byte((c), 1)"), QString::fromLatin1(after.left(400)));
	check("改写：写数据换了",
	      after.contains("OLED_WR_Byte((d), 0)"), QString());
	check("改写：BOM 还在最前面", after.startsWith("\xEF\xBB\xBF"), QString());
	check("改写：CRLF 没被改成 LF",
	      after.contains("__OLED_CFONT_H\r\n"), QString());
	check("改写：对齐空格没被吃掉",
	      after.contains("#define CF_WRITE_CMD(c)                 OLED_WR_Byte"),
	      QString());

	const QStringList after2 = MergeProject::adapterMismatch(header, a);
	check("改写后：体检干净", after2.isEmpty(), after2.join(QLatin1Char(';')));

	/* 撤销回到原样 */
	QStringList touched;
	touched << header;
	QString revErr;
	check("撤销成功", MergeProject::revert(touched, &revErr), revErr);
	check("撤销后和原文件逐字节一致", readFile(header) == original, QString());
}

/* 没探测到结果时，一个字节都不许动 */
static void testNoTouchWhenUnknown(const QString &root)
{
	const QString dir = root + QStringLiteral("/notouch");
	QDir().mkpath(dir);

	const QString header = dir + QStringLiteral("/OLED_CFont.h");
	const QByteArray original = "#define CF_WRITE_CMD(c)   OLED_WriteCommand(c)\n";
	writeFile(header, original);

	const LibProbe::CodeAdapter a;      /* detected = false */
	bool bak = false;
	QString err;
	const int fixed = MergeProject::applyAdapter(header, a, &bak, &err);
	check("没探到：不返回改动", fixed == 0, QString::number(fixed));
	check("没探到：不生成备份", !bak, QString());
	check("没探到：文件一个字节没动", readFile(header) == original, QString());
}

/* 工具自己导出的头文件不能当证据。
 * 否则会自我印证：生成的文件里有 CF_SHOW_ASCII → OLED_ShowChar(...)，
 * 下次探测读到自己，就"发现"用户工程里有 OLED_ShowChar。 */
static void testSelfPollution(const QString &root)
{
	const QString dir = root + QStringLiteral("/selfpollute");
	writeFile(dir + QStringLiteral("/main.c"), "int main(void)\n{\n}\n");
	writeFile(dir + QStringLiteral("/Hardware/OLED_CFont.h"),
	          "#ifndef __OLED_CFONT_H\n"
	          "#define __OLED_CFONT_H\n"
	          "#define CF_WRITE_CMD(c)                 OLED_WriteCommand(c)\n"
	          "#define CF_WRITE_DATA(d)                OLED_WriteData(d)\n"
	          "#define CF_SHOW_ASCII(line, col, ch)    OLED_ShowChar((line), (col), (ch))\n"
	          "#endif\n");

	const LibProbe::Result r = LibProbe::probe(dir);
	check("自己生成的头文件不当证据：没认出写命令",
	      r.write == LibProbe::WriteUnknown,
	      QString::number(static_cast<int>(r.write)));
	check("自己生成的头文件不当证据：没认出字符函数",
	      r.showAsciiFunc.isEmpty(), r.showAsciiFunc);
	check("自己生成的头文件不当证据：整体报不知道", !r.any(), r.evidenceOneLine);
}

/* HAL 的招牌头文件在 Drivers/ 下面，目录过滤不能把它一起过滤掉 */
static void testHalInDrivers(const QString &root)
{
	const QString dir = root + QStringLiteral("/hal2");
	writeFile(dir + QStringLiteral("/Drivers/STM32F1xx_HAL_Driver/Inc/stm32f1xx_hal.h"),
	          "/* HAL */\n");
	/* 故意不写 HAL_Init，只留招牌头文件这一条证据 */
	writeFile(dir + QStringLiteral("/Core/Src/main.c"),
	          "int main(void)\n{\n    while (1) { }\n}\n");

	const LibProbe::Result r = LibProbe::probe(dir);
	check("Drivers 下的 stm32f1xx_hal.h 也认", r.lib == LibProbe::HAL,
	      LibProbe::nameOf(r.lib));
}

/* 驱动里定义 OLED_WRITE_CMD(c) 这种带参宏的不算少见（多半是 I2C 地址位），
 * 不能把这种文件当成"工具生成的"跳过 —— 那会把真正的驱动整个漏掉。 */
static void testDriverWithCmdMacro(const QString &root)
{
	const QString dir = root + QStringLiteral("/cmdmacro");
	writeFile(dir + QStringLiteral("/HARDWARE/OLED/oled.h"),
	          "#ifndef __OLED_H\n"
	          "#define __OLED_H\n"
	          "#define OLED_WRITE_CMD(c)   ((c) ? 0x40 : 0x00)\n"
	          "void OLED_WriteCommand(uint8_t cmd);\n"
	          "void OLED_WriteData(uint8_t dat);\n"
	          "#endif\n");

	const LibProbe::Result r = LibProbe::probe(dir);
	check("驱动里的 OLED_WRITE_CMD(c) 不误判成生成物",
	      r.write == LibProbe::TwoFuncs, QString::number(static_cast<int>(r.write)));
}

int main(int argc, char *argv[])
{
	QCoreApplication app(argc, argv);

	/* 带路径跑 = 只打印这个工程的识别结果，给人工核对用。
	 * 例：test_libprobe.exe "C:/Users/xxx/Desktop/5-2 旋转编码器计次" */
	if (argc > 1)
	{
		const QString root = QString::fromLocal8Bit(argv[1]);
		const LibProbe::Result r = LibProbe::probe(root);
		const LibProbe::CodeAdapter a = LibProbe::adapterOf(r);

		std::printf("root : %s\n", root.toUtf8().constData());
		std::printf("lib  : %s\n", LibProbe::nameOf(r.lib).toUtf8().constData());
		std::printf("delay: %s\n", a.delayCall.toUtf8().constData());
		std::printf("cmd  : %s\n", a.writeCmd.toUtf8().constData());
		std::printf("data : %s\n", a.writeData.toUtf8().constData());
		std::printf("ascii: %s\n", a.showAscii.toUtf8().constData());
		std::printf("note : %s\n", a.origin.toUtf8().constData());
		if (!r.error.isEmpty())
		{
			std::printf("error: %s\n", r.error.toUtf8().constData());
		}
		std::printf("--- evidence ---\n");
		for (const QString &e : r.evidence)
		{
			std::printf("  %s\n", e.toUtf8().constData());
		}
		return 0;
	}

	const QString root = QDir::tempPath() + QStringLiteral("/oledfonttool_libprobe");
	QDir(root).removeRecursively();
	QDir().mkpath(root);

	std::printf("=== libprobe ===\n");
	testStdLibProject(root);
	testHalProject(root);
	testFlagByteProject(root);
	testEmptyProject(root);
	testSelfPollution(root);
	testHalInDrivers(root);
	testDriverWithCmdMacro(root);
	testRewrite(root);
	testNoTouchWhenUnknown(root);

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);

	QDir(root).removeRecursively();
	return g_fail == 0 ? 0 : 1;
}
