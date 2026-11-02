// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 zhangwtee <https://github.com/zhangwtee>
// 本文件属于「OLED 点阵取模工具」。改动请保留以上两行并注明改了什么（Apache-2.0 §4b）。
/* 导出内容的回归测试：三份头文件里的列偏移宏是不是按参数写的。
 *
 * 为什么单独测这个
 * ----------------
 * 三个 *_COL_OFFSET 分散在三份文件里，改了一处漏了两处，症状还各不一样：
 * 字库错位、动画错位、自检帧错位，而且都只在 SH1106 上才看得出来。
 * 所以这里把「参数进去、宏出来」这一环钉死。
 *
 * 顺带把生成结果写到 out/ 下，交给 cl /c 编一遍 —— 语法错没错，
 * 光看字符串是看不出来的。
 */
#include "../src/fontgen.h"
#include "../src/imagegen.h"
#include "../src/selftest.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImageReader>
#include <QRegularExpression>

#include <cstdio>

static int g_pass = 0;
static int g_fail = 0;

static void check(const char *what, bool ok, const QString &detail = QString())
{
	if (ok)
	{
		++g_pass;
		std::printf("  ok    %s\n", what);
	}
	else
	{
		++g_fail;
		std::printf("  FAIL  %s   %s\n", what, detail.toLocal8Bit().constData());
	}
}

/* 从 #define NAME  123 里把 123 读出来 */
static int macroValue(const QString &text, const QString &name)
{
	const QRegularExpression re(QStringLiteral("#define\\s+") + name
	                            + QStringLiteral("\\s+(\\d+)"));
	const QRegularExpressionMatch m = re.match(text);
	return m.hasMatch() ? m.captured(1).toInt() : -999;
}

static bool writeFile(const QString &path, const QString &content)
{
	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
	{
		return false;
	}
	f.write(content.toUtf8());
	f.close();
	return true;
}

int main(int argc, char **argv)
{
	QGuiApplication app(argc, argv);

	const QString outDir = (argc > 1) ? QString::fromLocal8Bit(argv[1])
	                                  : QDir::current().absoluteFilePath(QStringLiteral("out"));
	QDir().mkpath(outDir);

	/* ---------------- 字库 ---------------- */
	std::printf("[字库 OLED_CFont.h]\n");
	{
		FontGen gen(QFont(QStringLiteral("SimSun"), 16), 110, 16);
		QString err;

		const QString a = gen.generateHeader(QStringLiteral("温度正常"), &err, 0);
		const QString b = gen.generateHeader(QStringLiteral("温度正常"), &err, 2);
		const QString d = gen.generateHeader(QStringLiteral("温度正常"), &err);   /* 默认参数 */

		check("offset 传 0 写成 0", macroValue(a, QStringLiteral("CF_COL_OFFSET")) == 0,
		      QStringLiteral("got %1").arg(macroValue(a, QStringLiteral("CF_COL_OFFSET"))));
		check("offset 传 2 写成 2", macroValue(b, QStringLiteral("CF_COL_OFFSET")) == 2,
		      QStringLiteral("got %1").arg(macroValue(b, QStringLiteral("CF_COL_OFFSET"))));
		check("不传参默认 0（老调用点不受影响）",
		      macroValue(d, QStringLiteral("CF_COL_OFFSET")) == 0,
		      QStringLiteral("got %1").arg(macroValue(d, QStringLiteral("CF_COL_OFFSET"))));

		/* 除了这个数字，两份内容必须完全一样 —— 否则说明偏移量漏到别处去了 */
		QString a2 = a, b2 = b;
		const QRegularExpression re(QStringLiteral("#define CF_COL_OFFSET\\s+\\d+"));
		a2.replace(re, QStringLiteral("#define CF_COL_OFFSET X"));
		b2.replace(re, QStringLiteral("#define CF_COL_OFFSET X"));
		check("改动只落在这一行", a2 == b2);

		/* 每个字后面的注释里要带这个字的 C 字符串，连引号复制出来就能粘进代码。
		 * 这里逐字核对：按约定拼出的那一行必须原样出现在文件里 —— 既钉住了
		 * "注释里得有"，也钉住了写法（每个字节一律 \xNN 两位大写，不许用
		 * 可打印字符代替，否则碰到引号、反斜杠时粘过去就是错的）。 */
		{
			static const char hexd[] = "0123456789ABCDEF";
			bool    okLine = true;
			QString firstBad;

			for (const QString &ch : { QStringLiteral("温"), QStringLiteral("度"),
			                           QStringLiteral("正"), QStringLiteral("常") })
			{
				const QByteArray u8 = ch.toUtf8();
				QString lit = QStringLiteral("\"");
				for (int i = 0; i < u8.size(); ++i)
				{
					const quint8 v = static_cast<quint8>(u8.at(i));
					lit += QStringLiteral("\\x");
					lit += QLatin1Char(hexd[(v >> 4) & 0x0F]);
					lit += QLatin1Char(hexd[v & 0x0F]);
				}
				lit += QStringLiteral("\"");

				const QString want = QStringLiteral("\t/* %1  %2 */").arg(ch, lit);
				if (!a.contains(want))
				{
					okLine = false;
					firstBad = want;
					break;
				}
			}
			check("每个字后面带可直接复制的 C 字符串", okLine, firstBad);
		}

		writeFile(QDir(outDir).absoluteFilePath(QStringLiteral("OLED_CFont.h")), b);

		/* 屏高：128×32 的屏只有 4 页，字库里的 CF_SCREEN_H 必须跟着变，
		 * 否则第 3 行文字会往不存在的页地址里写。 */
		const QString h32 = gen.generateHeader(QStringLiteral("温度正常"), &err, 0, 32);
		check("屏高传 32 写成 32",
		      macroValue(h32, QStringLiteral("CF_SCREEN_H")) == 32,
		      QStringLiteral("got %1").arg(macroValue(h32, QStringLiteral("CF_SCREEN_H"))));
		check("屏高不传默认 64",
		      macroValue(a, QStringLiteral("CF_SCREEN_H")) == 64,
		      QStringLiteral("got %1").arg(macroValue(a, QStringLiteral("CF_SCREEN_H"))));
		check("屏高传野值（48）按 64 处理",
		      macroValue(gen.generateHeader(QStringLiteral("温度正常"), &err, 0, 48),
		                 QStringLiteral("CF_SCREEN_H")) == 64);

		QString a3 = a, h3 = h32;
		const QRegularExpression reH(QStringLiteral("#define CF_SCREEN_H\\s+\\d+"));
		a3.replace(reH, QStringLiteral("#define CF_SCREEN_H X"));
		h3.replace(reH, QStringLiteral("#define CF_SCREEN_H X"));
		check("屏高也只改这一行", a3 == h3);

		/* 适配区：不传 adapter 时必须还是改造前那几个死值 —— 老调用点逐字节不变 */
		check("默认适配区仍是 OLED_WriteCommand",
		      a.contains(QStringLiteral("#define CF_WRITE_CMD(c)                 OLED_WriteCommand(c)"))
		          && a.contains(QStringLiteral("#define CF_WRITE_DATA(d)                OLED_WriteData(d)")),
		      QString());
		check("默认适配区不加任何来源注释（老输出逐字节不变）",
		      !a.contains(QStringLiteral("适配区来源")), QString());

		/* 传了 adapter 就按工程探测结果填（标准库 + 正点原子那套驱动） */
		LibProbe::Result lr;
		lr.lib         = LibProbe::StandardPeriph;
		lr.write       = LibProbe::FlagByte;
		lr.byteFunc    = QStringLiteral("OLED_WR_Byte");
		lr.showAsciiFunc = QStringLiteral("OLED_ShowChar");
		lr.delayCall   = QStringLiteral("Delay_ms(ms)");
		const LibProbe::CodeAdapter ad = LibProbe::adapterOf(lr);

		const QString withAd = gen.generateHeader(QStringLiteral("温度正常"), &err, 0, 64, ad);
		check("适配区：写命令换成标志位写法",
		      withAd.contains(QStringLiteral("#define CF_WRITE_CMD(c)                 OLED_WR_Byte((c), 1)")),
		      QString());
		check("适配区：写数据也跟着换",
		      withAd.contains(QStringLiteral("#define CF_WRITE_DATA(d)                OLED_WR_Byte((d), 0)")),
		      QString());
		check("适配区：不再出现 HAL_Delay",
		      !withAd.contains(QStringLiteral("HAL_Delay")), QString());
		check("适配区：注释里写了识别依据",
		      withAd.contains(QStringLiteral("适配区来源")), QString());

		/* 换了适配区，点阵数据本身一个字节都不能变。字模表在适配区之前，
		 * 所以比「从第一个 0x 到适配区注释」这一段。 */
		const QString dataA = a.mid(a.indexOf(QStringLiteral("0x")),
		                            a.indexOf(QStringLiteral("适配区：只改这里"))
		                                - a.indexOf(QStringLiteral("0x")));
		const QString dataB = withAd.mid(withAd.indexOf(QStringLiteral("0x")),
		                                 withAd.indexOf(QStringLiteral("适配区：只改这里"))
		                                     - withAd.indexOf(QStringLiteral("0x")));
		check("适配区换了但字模数据不变", dataA == dataB,
		      QStringLiteral("%1 vs %2 字节").arg(dataA.size()).arg(dataB.size()));

		writeFile(QDir(outDir).absoluteFilePath(QStringLiteral("OLED_CFont32.h")), h32);

		/* 头文件自己 include <stdint.h>：不写的话它用 uint8_t 全靠调用方
		 * 把芯片头放在前面，include 顺序一换就报 unknown type name 'uint8_t'。
		 * 素材包头 oled_assets.h 早就有这一行，字库这份以前漏了。 */
		check("字库头自带 #include <stdint.h>",
		      a.contains(QStringLiteral("#include <stdint.h>")), QString());
		check("stdint 在数组定义之前",
		      a.indexOf(QStringLiteral("#include <stdint.h>"))
		          < a.indexOf(QStringLiteral("const uint8_t ")),
		      QString::number(a.indexOf(QStringLiteral("#include <stdint.h>"))));

		/* 单片机端改成了二分查表，前提是码表按 UTF-8 升序。
		 * 顺序一旦坏了，表现是"某些字突然显示不出来"，编译链接都不报错，
		 * 所以这里直接从生成的头文件里把每行的前 3 字节抠出来验一遍。 */
		{
			const QRegularExpression reRow(QStringLiteral(
				"^\\t0x([0-9A-F]{2}),0x([0-9A-F]{2}),0x([0-9A-F]{2}),"));
			const QStringList rows = b.split(QLatin1Char('\n'));

			QVector<QByteArray> keys;
			for (const QString &line : rows)
			{
				const QRegularExpressionMatch m = reRow.match(line);
				if (m.hasMatch())
				{
					keys.append(QByteArray::fromHex(
						(m.captured(1) + m.captured(2) + m.captured(3)).toLatin1()));
				}
			}

			auto keyLess = [](const QByteArray &x, const QByteArray &y) {
				for (int i = 0; i < 3; ++i)
				{
					const int a = static_cast<quint8>(x.at(i));
					const int c = static_cast<quint8>(y.at(i));
					if (a != c)
					{
						return a < c;
					}
				}
				return false;
			};

			bool sorted = (keys.size() == 4);
			for (int i = 1; i < keys.size(); ++i)
			{
				if (!keyLess(keys.at(i - 1), keys.at(i)))
				{
					sorted = false;
				}
			}
			check("码表严格升序（二分查表的前提）", sorted,
			      QStringLiteral("抠到 %1 行").arg(keys.size()));
		}
	}

	/* ---------------- 帧字节数：公式必须和写入布局一致 ---------------- */
	std::printf("[帧字节数]\n");
	{
		/* 128 宽只是"8 的倍数"的特例。
		 * packToDisplay 按 page * w + x 写，真实长度是 w * (h / 8)；
		 * bytesPerFrame 原来写成 (w / 8) * h，两者只在 w % 8 == 0 时相等。
		 * 界面现在只给 128 宽，所以这个错一直没露头，但头文件里的
		 * ANIM_FRAME_SIZE 和数组维数都靠它 —— 哪天放开宽度就是数组错位。 */
		const struct { int w; int h; } cases[] = {
			{128, 64}, {128, 32}, {64, 64}, {100, 60}, {130, 64}, {99, 55}
		};

		for (const auto &c : cases)
		{
			QImage mono(c.w, c.h, QImage::Format_Mono);
			mono.setColorCount(2);
			mono.setColor(0, qRgb(0, 0, 0));
			mono.setColor(1, qRgb(255, 255, 255));
			mono.fill(0);

			const QVector<quint8> packed = ImageGen::packToDisplay(mono);
			const int want = c.w * (c.h / 8);
			const QByteArray tag = QStringLiteral("点阵 %1x%2").arg(c.w).arg(c.h).toUtf8();

			check(tag.constData(), packed.size() == want,
			      QStringLiteral("packToDisplay 出来 %1 字节，按页排布应该是 %2")
			          .arg(packed.size()).arg(want));
			check(tag.constData(), ImageGen::bytesPerFrame(c.w, c.h) == want,
			      QStringLiteral("bytesPerFrame 给 %1，实际写入 %2")
			          .arg(ImageGen::bytesPerFrame(c.w, c.h)).arg(want));

			/* 头文件里的 FRAME_SIZE 必须跟着真实长度走，否则数组维数就错了 */
			ImageGen::Options o;
			o.width  = c.w;
			o.height = c.h;
			QVector<QVector<quint8>> one;
			one.append(packed);
			const QString hc = ImageGen::renderHeader(one, o, 100, QStringLiteral("t.png"));
			check(tag.constData(),
			      macroValue(hc, QStringLiteral("ANIM_FRAME_SIZE")) == want,
			      QStringLiteral("头文件里是 %1，实际每帧 %2 字节")
			          .arg(macroValue(hc, QStringLiteral("ANIM_FRAME_SIZE"))).arg(want));
		}
	}

	/* ---------------- 动画 ---------------- */
	std::printf("[动画 anim.h]\n");
	{
		ImageGen::Options opt;
		opt.width = 128;
		opt.height = 64;
		opt.colOffset = 2;

		QVector<QVector<quint8>> frames;
		frames.append(QVector<quint8>(ImageGen::bytesPerFrame(128, 64), 0xA5));

		const QString content = ImageGen::renderHeader(frames, opt, 100, QStringLiteral("test.gif"));
		check("offset 传 2 写成 2",
		      macroValue(content, QStringLiteral("ANIM_COL_OFFSET")) == 2,
		      QStringLiteral("got %1").arg(macroValue(content, QStringLiteral("ANIM_COL_OFFSET"))));

		opt.colOffset = 0;
		const QString c0 = ImageGen::renderHeader(frames, opt, 100, QStringLiteral("test.gif"));
		check("offset 传 0 写成 0",
		      macroValue(c0, QStringLiteral("ANIM_COL_OFFSET")) == 0,
		      QStringLiteral("got %1").arg(macroValue(c0, QStringLiteral("ANIM_COL_OFFSET"))));

		writeFile(QDir(outDir).absoluteFilePath(QStringLiteral("anim.h")), content);

		/* 适配区：动图这份以前写死 HAL_Delay，标准库工程导出来直接编不过 */
		check("动图默认适配区仍是 HAL_Delay",
		      content.contains(QStringLiteral("#define ANIM_DELAY(ms)       HAL_Delay(ms)")),
		      QString());

		LibProbe::Result lr;
		lr.lib       = LibProbe::StandardPeriph;
		lr.delayCall = QStringLiteral("Delay_ms(ms)");
		lr.write     = LibProbe::FlagByte;
		lr.byteFunc  = QStringLiteral("OLED_WR_Byte");
		const LibProbe::CodeAdapter ad = LibProbe::adapterOf(lr);

		const QString withAd = ImageGen::renderHeader(frames, opt, 100,
		                                              QStringLiteral("test.gif"),
		                                              QString(), ad);
		check("动图适配区：延时换成 Delay_ms",
		      withAd.contains(QStringLiteral("#define ANIM_DELAY(ms)       Delay_ms(ms)")),
		      QString());
		check("动图适配区：不再把 HAL_Delay 当默认值",
		      !withAd.contains(QStringLiteral("#define ANIM_DELAY(ms)       HAL_Delay(ms)")),
		      QString());

		/* 帧数据不受适配区影响 */
		const QString fa = content.mid(content.indexOf(QStringLiteral("0x")));
		const QString fb = withAd.mid(withAd.indexOf(QStringLiteral("0x")));
		check("动图适配区换了但帧数据不变", fa == fb, QString());

		/* 多素材打包那份是另一个生成器，也要按 adapter 填 */
		ImageGen::Asset asset;
		asset.ident  = QStringLiteral("logo");
		asset.frames = frames;
		asset.opt    = opt;
		const QString multi = ImageGen::renderMultiHeader(QVector<ImageGen::Asset>() << asset,
		                                                  QStringLiteral("assets.h"), ad);
		check("多素材适配区：延时换成 Delay_ms",
		      multi.contains(QStringLiteral("#define OLED_DELAY(ms)       Delay_ms(ms)")),
		      QString());
	}

	/* ---------------- 动画帧去重 ---------------- */
	std::printf("[动画帧去重]\n");
	{
		ImageGen::Options opt;
		opt.width = 128;
		opt.height = 64;

		const int per = ImageGen::bytesPerFrame(128, 64);

		/* 第 3 帧和第 1 帧相同（不相邻），第 4 帧和第 3 帧相同（相邻） */
		QVector<QVector<quint8>> f;
		f.append(QVector<quint8>(per, 0x11));
		f.append(QVector<quint8>(per, 0x22));
		f.append(QVector<quint8>(per, 0x11));
		f.append(QVector<quint8>(per, 0x11));

		const QString dc = ImageGen::renderHeader(f, opt, 100, QStringLiteral("loop.gif"));
		check("逻辑帧数不变（播放时长靠它）",
		      macroValue(dc, QStringLiteral("ANIM_COUNT")) == 4,
		      QStringLiteral("got %1").arg(macroValue(dc, QStringLiteral("ANIM_COUNT"))));
		check("数据帧数只剩 2",
		      macroValue(dc, QStringLiteral("ANIM_DATA_COUNT")) == 2,
		      QStringLiteral("got %1").arg(macroValue(dc, QStringLiteral("ANIM_DATA_COUNT"))));
		check("数组按 ANIM_DATA_COUNT 声明",
		      dc.contains(QStringLiteral("Anim_Frames[ANIM_DATA_COUNT]")));
		check("生成映射表", dc.contains(QStringLiteral("Anim_Map[ANIM_COUNT]")));
		check("取帧走映射", dc.contains(QStringLiteral("Anim_Frames[Anim_Map[i]]")));

		/* 映射内容必须是 0,1,0,0 —— 顺序错了帧序就乱，但画面每帧都对，
		 * 只有连着看才看得出，属于最难查的那种。 */
		const QRegularExpression reMap(
			QStringLiteral("Anim_Map\\[ANIM_COUNT\\]\\s*=\\s*\\{([^}]*)\\}"));
		const QRegularExpressionMatch mm = reMap.match(dc);
		QString seq;
		if (mm.hasMatch())
		{
			const QString body = QString(mm.captured(1)).remove(QRegularExpression(
				QStringLiteral("/\\*.*?\\*/"), QRegularExpression::DotMatchesEverythingOption));
			const QRegularExpression reNum(QStringLiteral("\\d+"));
			auto it = reNum.globalMatch(body);
			while (it.hasNext())
			{
				if (!seq.isEmpty())
				{
					seq += QLatin1Char(',');
				}
				seq += it.next().captured(0);
			}
		}
		check("映射序列是 0,1,0,0", seq == QStringLiteral("0,1,0,0"), seq);

		/* 只有一帧内容时不该白占 4 份数据 */
		QVector<QVector<quint8>> same;
		for (int i = 0; i < 5; ++i)
		{
			same.append(QVector<quint8>(per, 0x7E));
		}
		const QString sc = ImageGen::renderHeader(same, opt, 100, QStringLiteral("still.gif"));
		check("5 帧全同 -> 只存 1 帧",
		      macroValue(sc, QStringLiteral("ANIM_DATA_COUNT")) == 1
		          && macroValue(sc, QStringLiteral("ANIM_COUNT")) == 5);

		/* 没有重复帧时输出必须和以前完全一样：老工程重新导出不该被搅动 */
		QVector<QVector<quint8>> uniq;
		uniq.append(QVector<quint8>(per, 0x11));
		uniq.append(QVector<quint8>(per, 0x22));
		const QString uc = ImageGen::renderHeader(uniq, opt, 100, QStringLiteral("a.gif"));
		check("无重复时不定义 ANIM_DATA_COUNT",
		      !uc.contains(QStringLiteral("ANIM_DATA_COUNT")));
		check("无重复时不生成映射表", !uc.contains(QStringLiteral("Anim_Map")));
		check("无重复时数组维度仍是 ANIM_COUNT",
		      uc.contains(QStringLiteral("Anim_Frames[ANIM_COUNT]")));

		writeFile(QDir(outDir).absoluteFilePath(QStringLiteral("anim_dedup.h")), dc);
	}

	/* ---------------- 帧号位宽（超过 255 帧） ---------------- */
	std::printf("[超过 255 帧的索引类型]\n");
	{
		ImageGen::Options opt;
		opt.width = 128;
		opt.height = 32;                     /* 一帧 512 字节，300 帧也就 150KB */
		const int per = ImageGen::bytesPerFrame(128, 32);

		QVector<QVector<quint8>> many;
		for (int i = 0; i < 300; ++i)
		{
			/* 首两字节带上帧号，保证 300 帧两两不同 —— 否则 256 帧会和 0 帧
			 * 撞成一样，去重插进来，测的就不是"帧号位宽"这件事了 */
			QVector<quint8> one(per, quint8(i));
			one[0] = quint8(i & 0xFF);
			one[1] = quint8((i >> 8) & 0xFF);
			many.append(one);
		}
		const QString wc = ImageGen::renderHeader(many, opt, 100, QStringLiteral("long.gif"));
		check("300 帧：帧号用 uint16_t",
		      wc.contains(QStringLiteral("Anim_ShowFrame(uint16_t i)")));
		check("300 帧：示例循环也用 uint16_t",
		      wc.contains(QStringLiteral("uint16_t i;")));
		check("300 帧：不该再出现 uint8_t 帧号",
		      !wc.contains(QStringLiteral("uint8_t i")));
		check("ANIM_COUNT 是 300",
		      macroValue(wc, QStringLiteral("ANIM_COUNT")) == 300);

		/* 不超过 255 帧时，输出必须和以前逐字节一样 —— 老工程重新导出不该被搅动 */
		QVector<QVector<quint8>> few;
		few.append(QVector<quint8>(per, 0x11));
		few.append(QVector<quint8>(per, 0x22));
		const QString fc = ImageGen::renderHeader(few, opt, 100, QStringLiteral("a.gif"));
		check("2 帧：仍然是 uint8_t",
		      fc.contains(QStringLiteral("Anim_ShowFrame(uint8_t i)"))
		          && fc.contains(QStringLiteral("uint8_t i;")));
		check("2 帧：不出现 uint16_t",
		      !fc.contains(QStringLiteral("uint16_t")));

		/* 映射表里存的是"数据帧号"，也是同一个位宽问题 */
		QVector<QVector<quint8>> blink;
		for (int i = 0; i < 300; ++i)
		{
			blink.append(QVector<quint8>(per, (i % 2) ? 0x33 : 0x44));
		}
		const QString mc = ImageGen::renderHeader(blink, opt, 100, QStringLiteral("blink.gif"));
		check("去重后 300 帧：映射表也是 uint16_t",
		      mc.contains(QStringLiteral("static const uint16_t Anim_Map[ANIM_COUNT]")));
		check("去重后数据帧只剩 2",
		      macroValue(mc, QStringLiteral("ANIM_DATA_COUNT")) == 2);

		writeFile(QDir(outDir).absoluteFilePath(QStringLiteral("anim_300.h")), wc);
	}

	/* ---------------- 自检帧 ---------------- */
	std::printf("[自检帧 OLED_SelfTest.h]\n");
	{
		QVector<quint8> bytes(SelfTest::SIZE, 0x5A);

		const QString content = SelfTest::renderHeader(
			bytes, QStringLiteral("取模正确"), 16, QStringLiteral("SimSun"), 2);
		check("offset 传 2 写成 2",
		      macroValue(content, QStringLiteral("OLED_SELFTEST_COL_OFFSET")) == 2,
		      QStringLiteral("got %1")
		          .arg(macroValue(content, QStringLiteral("OLED_SELFTEST_COL_OFFSET"))));

		/* 自检帧以前调 OLED_SetCursor(page, 0)，那个函数各例程参数顺序不一致，
		 * 24×24 混排时还会被走样。现在改成自己发三条命令，和字库/动画一致。
		 * 注意只禁「调用」，注释里提它没关系 —— 带左括号才算调用。 */
		check("不再调用 OLED_SetCursor",
		      !content.contains(QStringLiteral("OLED_SetCursor(")));
		check("自带写命令宏", content.contains(QStringLiteral("OLED_SELFTEST_WRITE_CMD")));
		check("自带写数据宏", content.contains(QStringLiteral("OLED_SELFTEST_WRITE_DATA")));
		check("定位命令自己发", content.contains(QStringLiteral("0xB0 | (page & 0x07)")));

		/* 传 0 时也要是 0，不能因为默认值写反了变成 2 */
		const QString c0 = SelfTest::renderHeader(
			bytes, QStringLiteral("取模正确"), 16, QStringLiteral("SimSun"), 0);
		check("offset 传 0 写成 0",
		      macroValue(c0, QStringLiteral("OLED_SELFTEST_COL_OFFSET")) == 0);

		writeFile(QDir(outDir).absoluteFilePath(QStringLiteral("OLED_SelfTest.h")), content);

		/* ---- 128×32（0.91" 屏）---- */
		const QVector<quint8> b32(SelfTest::bytesOf(128, 32), 0x3C);
		const QString s32 = SelfTest::renderHeader(
			b32, QStringLiteral("取模正确"), 16, QStringLiteral("SimSun"), 0, 128, 32);
		check("32 行版本 W 仍是 128",
		      macroValue(s32, QStringLiteral("OLED_SELFTEST_W")) == 128);
		check("32 行版本 H 是 32",
		      macroValue(s32, QStringLiteral("OLED_SELFTEST_H")) == 32,
		      QStringLiteral("got %1").arg(macroValue(s32, QStringLiteral("OLED_SELFTEST_H"))));
		check("32 行版本 4 页",
		      macroValue(s32, QStringLiteral("OLED_SELFTEST_PAGES")) == 4);
		check("32 行版本共 512 字节",
		      macroValue(s32, QStringLiteral("OLED_SELFTEST_SIZE")) == 512,
		      QStringLiteral("got %1").arg(macroValue(s32, QStringLiteral("OLED_SELFTEST_SIZE"))));
		check("32 行版本不空", !s32.isEmpty());

		/* 字节数和尺寸对不上就得拒绝，不能生成一份长度不符的数组 */
		check("尺寸和字节数不符时返回空",
		      SelfTest::renderHeader(bytes, QStringLiteral("取模正确"), 16,
		                             QStringLiteral("SimSun"), 0, 128, 32).isEmpty());
		check("h 不是 8 的倍数时返回空",
		      SelfTest::renderHeader(bytes, QString(), 16, QString(), 0, 128, 30).isEmpty());

		/* 图面：32 行版本的外框必须四条边都在，底边最容易因为 H 用错而丢 */
		{
			FontGen g16(QFont(QStringLiteral("SimSun"), 16), 110, 16);
			const QImage img32 = SelfTest::buildImage(&g16, QStringLiteral("取模正确"), 128, 32);
			check("128×32 图尺寸正确",
			      img32.width() == 128 && img32.height() == 32,
			      QStringLiteral("%1x%2").arg(img32.width()).arg(img32.height()));

			const QVector<quint8> p32 = SelfTest::pack(img32);
			check("128×32 打包 512 字节", p32.size() == 512,
			      QStringLiteral("got %1").arg(p32.size()));

			bool topOk = (p32.size() == 512);
			bool botOk = (p32.size() == 512);
			for (int x = 0; x < 128 && p32.size() == 512; ++x)
			{
				/* 阴码低位在前：一页里 bit0 是最上面那行，bit7 是最下面那行 */
				if ((p32.at(x) & 0x01) == 0)
				{
					topOk = false;
				}
				if ((p32.at(3 * 128 + x) & 0x80) == 0)
				{
					botOk = false;
				}
			}
			check("32 行版本顶边整行点亮", topOk);
			check("32 行版本底边整行点亮（底边没丢）", botOk);

			QDir().mkpath(outDir + QStringLiteral("/selftest32"));
			writeFile(outDir + QStringLiteral("/selftest32/OLED_SelfTest.h"), s32);
		}
	}

	/* ---------------- 多选读图 ImageGen::loadFrames ----------------
	 *
	 * 一次选一堆图拼动画，出错的都是些不显眼的地方：
	 * 排序用字符串比会把 frame10 排到 frame2 前面（动画顺序乱），
	 * 上限按文件算而不是按批次算（20 个 300 帧的 GIF 能撑爆内存），
	 * 一个坏文件把整批都拖垮（用户看到"什么都没导入"）。
	 * 这几条都在这里钉住。
	 */
	std::printf("[多选读图 loadFrames]\n");
	{
		const QString dir = outDir + QStringLiteral("/frames");
		QDir().mkpath(dir);

		auto makePng = [&dir](const QString &name, QRgb color) {
			QImage img(8, 8, QImage::Format_ARGB32);
			img.fill(color);
			return img.save(dir + QStringLiteral("/") + name, "PNG");
		};
		const bool made = makePng(QStringLiteral("frame1.png"),  qRgb(255, 0, 0))
		               && makePng(QStringLiteral("frame2.png"),  qRgb(0, 255, 0))
		               && makePng(QStringLiteral("frame10.png"), qRgb(0, 0, 255));
		check("能造出 PNG 素材", made);

		/* 故意按 10 → 2 → 1 的顺序传，模拟"用户点选顺序和文件名顺序不一致" */
		const QStringList raw = {dir + QStringLiteral("/frame10.png"),
		                         dir + QStringLiteral("/frame2.png"),
		                         dir + QStringLiteral("/frame1.png")};

		const ImageGen::FrameBatch b = ImageGen::loadFrames(raw);
		check("三张图各读成一帧", b.frames.size() == 3,
		      QStringLiteral("got %1").arg(b.frames.size()));

		const bool orderOk = (b.frames.size() == 3)
		                     && b.frames.at(0).pixelColor(0, 0).red()   == 255
		                     && b.frames.at(1).pixelColor(0, 0).green() == 255
		                     && b.frames.at(2).pixelColor(0, 0).blue()  == 255;
		check("按文件名自然排序（1 → 2 → 10，不是 10 → 2 → 1）", orderOk);

		check("files 也按播放顺序给回",
		      b.files.size() == 3
		          && QFileInfo(b.files.first()).fileName() == QStringLiteral("frame1.png")
		          && QFileInfo(b.files.last()).fileName()  == QStringLiteral("frame10.png"));
		check("totalFrames 是文件里真实总数", b.totalFrames == 3,
		      QStringLiteral("got %1").arg(b.totalFrames));
		check("静态图没给延时（由界面用默认值）", b.delayMs == 0);
		check("好好的文件不算截断", !b.truncated);
		check("没有失败文件", b.failed.isEmpty());

		/* 上限按批次算：3 个文件一共只收 2 帧 */
		const ImageGen::FrameBatch capped = ImageGen::loadFrames(raw, 2);
		check("maxFrames 按批次算（3 个文件只收 2 帧）", capped.frames.size() == 2,
		      QStringLiteral("got %1").arg(capped.frames.size()));
		check("撞上限算截断", capped.truncated);
		check("截断原因是帧数上限", capped.cappedByCount && !capped.cappedByMemory);
		check("记下有几个文件整个没读", capped.skippedFiles == 1,
		      QStringLiteral("got %1").arg(capped.skippedFiles));
		check("批次帧数上限回填给界面", capped.limitFrames == 2,
		      QStringLiteral("got %1").arg(capped.limitFrames));

		/* 一个坏文件不该把整批拖垮 */
		QStringList withBad = raw;
		withBad << dir + QStringLiteral("/zz_nope.png");
		const ImageGen::FrameBatch bad = ImageGen::loadFrames(withBad);
		check("坏文件不拖累其它文件", bad.frames.size() == 3,
		      QStringLiteral("got %1").arg(bad.frames.size()));
		check("坏文件名进 failed",
		      bad.failed.size() == 1
		          && bad.failed.first() == QStringLiteral("zz_nope.png"),
		      bad.failed.join(QStringLiteral(",")));

		/* 全坏：界面要能判定"一张都没读进来" */
		const ImageGen::FrameBatch none = ImageGen::loadFrames(
		    QStringList() << dir + QStringLiteral("/nope_a.png")
		                  << dir + QStringLiteral("/nope_b.png"));
		check("全失败时 isEmpty", none.isEmpty());
		check("全失败时都记在 failed", none.failed.size() == 2);

		/* 预算不够时别硬塞：8×8 一帧 256 字节，给 100 字节预算 */
		const ImageGen::FrameBatch poor = ImageGen::loadFrames(raw, 600, 100);
		check("预算不足时一帧都不收", poor.isEmpty(),
		      QStringLiteral("got %1").arg(poor.frames.size()));
		check("预算不足的原因记成内存", poor.cappedByMemory && !poor.cappedByCount);

		/* 预算不足不等于"文件读不出来"。
		 * 原来一帧都没收就一律塞进 failed，界面上于是出现
		 * "下面这些读不出图像，已跳过：frame1.png、frame2.png、frame10.png" ——
		 * 三张好好的图被说成坏的，用户拿着图去找为什么打不开。
		 * 真正的原因（预算）其实写在另一行，两句话互相打脸。 */
		check("预算不足不记成读不出图像", poor.failed.isEmpty(),
		      poor.failed.join(QStringLiteral(",")));

		/* 坏文件 + 预算不足混在一起：failed 里只能有那个真的坏的 */
		QStringList poorAndBad = raw;
		poorAndBad << dir + QStringLiteral("/zz_nope.png");
		const ImageGen::FrameBatch mixed = ImageGen::loadFrames(poorAndBad, 600, 100);
		check("预算不足时 failed 只留真坏的文件",
		      mixed.failed.size() == 1
		          && mixed.failed.first() == QStringLiteral("zz_nope.png"),
		      mixed.failed.join(QStringLiteral(",")));
		check("预算不足时仍然标记成内存原因", mixed.cappedByMemory,
		      QStringLiteral("cappedByMemory=%1").arg(mixed.cappedByMemory));

		/* GIF：一个文件展开成多帧，并带回它自己的延时 */
		const QString gif = QDir(QCoreApplication::applicationDirPath())
		                        .absoluteFilePath(QStringLiteral("fixture/move.gif"));
		QImageReader gr(gif);
		const int gifCount = gr.imageCount();
		check("找到 GIF 素材且多于 1 帧", gifCount > 1,
		      QStringLiteral("%1 帧 / %2").arg(gifCount).arg(gif));

		const ImageGen::FrameBatch gb = ImageGen::loadFrames(QStringList() << gif);
		check("GIF 展开成它的全部帧", gb.frames.size() == gifCount,
		      QStringLiteral("%1 vs %2").arg(gb.frames.size()).arg(gifCount));
		check("GIF 带回自己的延时", gb.delayMs > 0,
		      QStringLiteral("got %1").arg(gb.delayMs));

		/* 一张图 + 一个 GIF：顺序仍按文件名，帧数累加 */
		const ImageGen::FrameBatch mix = ImageGen::loadFrames(
		    QStringList() << raw.at(0) << gif);
		check("图片与 GIF 混选时帧数累加", mix.frames.size() == 1 + gifCount,
		      QStringLiteral("%1 vs %2").arg(mix.frames.size()).arg(1 + gifCount));
		check("混选时延时取 GIF 的", mix.delayMs == gb.delayMs);
	}

	/* ---------------- 多份素材：名字与标识前缀 ---------------- */
	std::printf("[多份素材]\n");
	{
		/* 文件名直接拿来当标题，得先过成合法 C 标识符 */
		check("中文名退回兜底名",
		      ImageGen::sanitizeIdentifier(QStringLiteral("图标")) == QStringLiteral("image"),
		      ImageGen::sanitizeIdentifier(QStringLiteral("图标")));
		check("空格和标点变下划线",
		      ImageGen::sanitizeIdentifier(QStringLiteral("my logo-1")) == QStringLiteral("my_logo_1"),
		      ImageGen::sanitizeIdentifier(QStringLiteral("my logo-1")));
		check("数字开头补前缀",
		      ImageGen::sanitizeIdentifier(QStringLiteral("2nd_icon")) == QStringLiteral("img_2nd_icon"),
		      ImageGen::sanitizeIdentifier(QStringLiteral("2nd_icon")));
		check("下划线不堆叠",
		      ImageGen::sanitizeIdentifier(QStringLiteral("a b  c")) == QStringLiteral("a_b_c"),
		      ImageGen::sanitizeIdentifier(QStringLiteral("a b  c")));
		check("首尾下划线去掉",
		      ImageGen::sanitizeIdentifier(QStringLiteral("_logo_")) == QStringLiteral("logo"),
		      ImageGen::sanitizeIdentifier(QStringLiteral("_logo_")));
		check("本来就合法的不动",
		      ImageGen::sanitizeIdentifier(QStringLiteral("logo32")) == QStringLiteral("logo32"),
		      ImageGen::sanitizeIdentifier(QStringLiteral("logo32")));

		/* 同名文件从两个目录一起导进来很常见 */
		QStringList used;
		used << QStringLiteral("logo");
		check("重名加 _2", ImageGen::uniqueName(used, QStringLiteral("logo")) == QStringLiteral("logo_2"));
		used << QStringLiteral("logo_2");
		check("再重名加 _3", ImageGen::uniqueName(used, QStringLiteral("logo")) == QStringLiteral("logo_3"));
		check("大小写也算撞名",
		      ImageGen::uniqueName(used, QStringLiteral("LOGO")) == QStringLiteral("LOGO_3"),
		      ImageGen::uniqueName(used, QStringLiteral("LOGO")));
		check("没撞名就用原名",
		      ImageGen::uniqueName(used, QStringLiteral("icon")) == QStringLiteral("icon"));

		/* 头文件里的宏和函数都要跟着素材名走，否则两份 .h 不能同时 include */
		ImageGen::Options opt;
		opt.width  = 128;
		opt.height = 32;
		QVector<QVector<quint8>> frames;
		frames.append(QVector<quint8>(ImageGen::bytesPerFrame(128, 32), 0x3C));

		const QString def  = ImageGen::renderHeader(frames, opt, 80, QStringLiteral("a.gif"));
		const QString logo = ImageGen::renderHeader(frames, opt, 80, QStringLiteral("a.gif"),
		                                            QStringLiteral("logo"));

		check("默认输出不带前缀（老行为不变）",
		      def.contains(QStringLiteral("#define ANIM_WIDTH      128")) &&
		          def.contains(QStringLiteral("void Anim_ShowFrame")),
		      QStringLiteral("默认输出被改动了"));
		check("指定 ident 后宏换前缀",
		      logo.contains(QStringLiteral("#define LOGO_WIDTH      128")) &&
		          logo.contains(QStringLiteral("#define LOGO_COL_OFFSET")),
		      QStringLiteral("没找到 LOGO_WIDTH"));
		check("指定 ident 后函数/数组换前缀",
		      logo.contains(QStringLiteral("Logo_ShowFrame")) &&
		          logo.contains(QStringLiteral("const uint8_t Logo_Frames")),
		      QStringLiteral("没找到 Logo_ShowFrame"));
		check("include guard 也分开",
		      logo.contains(QStringLiteral("__LOGO_FRAMES_H")) &&
		          def.contains(QStringLiteral("__ANIM_FRAMES_H")),
		      QStringLiteral("guard 撞了"));
		check("老的 ANIM_ 一个不剩",
		      !logo.contains(QStringLiteral("ANIM_")),
		      QStringLiteral("还有残留的 ANIM_"));
		check("两份头文件的宏不重叠",
		      !logo.contains(QStringLiteral("#define ANIM_WIDTH")),
		      QStringLiteral("LOGO 头里还有 ANIM_WIDTH"));

		/* 名字全是标点时兜底成 image，不能生成空的宏前缀 */
		const QString odd = ImageGen::renderHeader(frames, opt, 80, QStringLiteral("x"),
		                                           QStringLiteral("图标"));
		check("中文 ident 退回 image 前缀",
		      odd.contains(QStringLiteral("#define IMAGE_WIDTH")),
		      QStringLiteral("没有退回 IMAGE_"));

		/* 写盘：内容要和 renderHeader 一致，失败要给出原因而不是静默 */
		const QString one = QDir(outDir).absoluteFilePath(QStringLiteral("logo.h"));
		const QString werr = ImageGen::writeHeaderFile(
		    one, frames, opt, 80, QStringLiteral("logo.png"), QStringLiteral("logo"));
		check("写盘成功时返回空", werr.isEmpty(), werr);

		QFile wf(one);
		const bool opened = wf.open(QIODevice::ReadOnly);
		QString written = opened ? QString::fromUtf8(wf.readAll()) : QString();
		wf.close();
		/* 写盘走 QIODevice::Text，Windows 上换行会变成 CRLF，
		 * 比较前先归一化 —— 内容本身必须和 renderHeader 一模一样 */
		check("写出来的内容就是 renderHeader 的结果",
		      written.count(QLatin1Char('\n')) > 0 &&
		          written.replace(QStringLiteral("\r\n"), QStringLiteral("\n"))
		              == ImageGen::renderHeader(frames, opt, 80,
		                                        QStringLiteral("logo.png"),
		                                        QStringLiteral("logo")),
		      QStringLiteral("%1 字节").arg(written.size()));
		check("写出来的文件带前缀", written.contains(QStringLiteral("#define LOGO_WIDTH")));

		const QString bad = ImageGen::writeHeaderFile(
		    QDir(outDir).absoluteFilePath(QStringLiteral("no_such_dir/x.h")),
		    frames, opt, 80, QStringLiteral("x"), QStringLiteral("x"));
		check("目录不存在时返回原因", !bad.isEmpty(), QStringLiteral("返回空"));
		check("空路径被挡住",
		      !ImageGen::writeHeaderFile(QString(), frames, opt, 80,
		                                 QStringLiteral("x")).isEmpty());
	}

	/* ---------------- 单份头文件：格式金样 ----------------
	 * renderHeader 为了和「多份打包」共用代码，内部拆成了几个小函数。
	 * 拆分最容易出的错是"输出悄悄变了"：多一个空格、少一行注释，用户那边的
	 * diff 就是上百行。这三份是拆分**之前**生成的，逐字节比。 */
	{
		const QString dir = QDir(QCoreApplication::applicationDirPath())
		                        .absoluteFilePath(QStringLiteral("fixture"));
		auto slurp = [](const QString &p) {
			QFile f(p);
			if (!f.open(QIODevice::ReadOnly))
			{
				return QString();
			}
			QString t = QString::fromUtf8(f.readAll());
			f.close();
			/* 金样是以 QIODevice::Text 写出来的，Windows 上是 CRLF */
			return t.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
		};

		ImageGen::Options o64;
		o64.width     = 128;
		o64.height    = 64;
		o64.threshold = 128;
		o64.dither    = ImageGen::Bayer;

		QVector<QVector<quint8>> one;
		QVector<quint8> f(ImageGen::bytesPerFrame(128, 64));
		for (int i = 0; i < f.size(); ++i)
		{
			f[i] = quint8((i * 37 + 11) & 0xFF);
		}
		one.append(f);

		const QString gSingle = slurp(QDir(dir).absoluteFilePath(
		    QStringLiteral("header_golden_single.h")));
		const QString gIdent = slurp(QDir(dir).absoluteFilePath(
		    QStringLiteral("header_golden_ident.h")));
		const QString gDedup = slurp(QDir(dir).absoluteFilePath(
		    QStringLiteral("header_golden_dedup.h")));
		check("金样读得进来（3 份都在）",
		      !gSingle.isEmpty() && !gIdent.isEmpty() && !gDedup.isEmpty());

		const QString gotSingle =
		    ImageGen::renderHeader(one, o64, 0, QStringLiteral("logo.png"));
		check("单帧 / 不带前缀：和拆分前逐字节一致", gotSingle == gSingle,
		      QStringLiteral("%1 vs %2 字节").arg(gotSingle.size()).arg(gSingle.size()));

		const QString gotIdent = ImageGen::renderHeader(one, o64, 120,
		                                                QStringLiteral("logo.png"),
		                                                QStringLiteral("logo"));
		check("单帧 / 带前缀：和拆分前逐字节一致", gotIdent == gIdent,
		      QStringLiteral("%1 vs %2 字节").arg(gotIdent.size()).arg(gIdent.size()));

		ImageGen::Options o32;
		o32.width     = 128;
		o32.height    = 32;
		o32.dither    = ImageGen::NoDither;
		o32.threshold = 96;
		QVector<QVector<quint8>> four;
		for (int k = 0; k < 2; ++k)
		{
			QVector<quint8> g(ImageGen::bytesPerFrame(128, 32));
			for (int i = 0; i < g.size(); ++i)
			{
				g[i] = quint8((i * (k + 3) + 7) & 0xFF);
			}
			four.append(g);
		}
		four.append(four.at(0));
		four.append(four.at(1));
		const QString gotDedup = ImageGen::renderHeader(four, o32, 80,
		                                                QStringLiteral("blink.gif"),
		                                                QStringLiteral("blink"));
		check("去重 + 带前缀：和拆分前逐字节一致", gotDedup == gDedup,
		      QStringLiteral("%1 vs %2 字节").arg(gotDedup.size()).arg(gDedup.size()));
	}

	/* ---------------- 多份素材写进一个 .h（各存各的，不合成动画） ----------------
	 * 用户的真实需求：一次导入好几张图，想要**一个**头文件，但不是一段动画，
	 * 而是各自独立的数组，而且点阵上方要能看出它当初是从哪个文件来的。 */
	{
		ImageGen::Asset a1;
		a1.ident      = QStringLiteral("logo");
		a1.sourceName = QStringLiteral("logo.png");
		a1.opt.width  = 128;
		a1.opt.height = 64;
		a1.delayMs    = 100;
		QVector<quint8> f1(ImageGen::bytesPerFrame(128, 64));
		for (int i = 0; i < f1.size(); ++i)
		{
			f1[i] = quint8((i * 37 + 11) & 0xFF);
		}
		a1.frames.append(f1);

		ImageGen::Asset a2;
		a2.ident      = QStringLiteral("blink");
		a2.sourceName = QStringLiteral("blink.gif");
		a2.opt.width  = 128;
		a2.opt.height = 32;
		a2.opt.dither = ImageGen::NoDither;
		a2.opt.threshold = 96;
		a2.delayMs    = 80;
		for (int k = 0; k < 2; ++k)
		{
			QVector<quint8> g(ImageGen::bytesPerFrame(128, 32));
			for (int i = 0; i < g.size(); ++i)
			{
				g[i] = quint8((i * (k + 3) + 7) & 0xFF);
			}
			a2.frames.append(g);
		}
		a2.frames.append(a2.frames.at(0));      /* 重复帧 → 这一份自己会去重 */
		a2.frames.append(a2.frames.at(1));

		QVector<ImageGen::Asset> list;
		list << a1 << a2;

		const QString h = ImageGen::renderMultiHeader(list, QStringLiteral("assets.h"));
		check("打包头文件非空", !h.isEmpty());

		/* 各自一套名字，互不覆盖 */
		check("两份素材各有自己的宏",
		      h.contains(QStringLiteral("#define LOGO_WIDTH")) &&
		          h.contains(QStringLiteral("#define BLINK_WIDTH")));
		check("两份素材各有自己的数组",
		      h.contains(QStringLiteral("const uint8_t Logo_Frames[LOGO_COUNT]")) &&
		          h.contains(QStringLiteral("const uint8_t Blink_Frames[BLINK_DATA_COUNT]")));
		check("两份素材各有自己的显示函数",
		      h.contains(QStringLiteral("void Logo_ShowFrame(")) &&
		          h.contains(QStringLiteral("void Blink_ShowFrame(")));

		/* 关键：没有变成"一个动画"。老的单份写法里全是 ANIM_* / Anim_* */
		check("没有拼成一段动画（ANIM_ 一个不剩）",
		      !h.contains(QStringLiteral("ANIM_")) &&
		          !h.contains(QStringLiteral("Anim_")),
		      QStringLiteral("还能找到 ANIM_"));

		/* 每份的帧数、尺寸各自独立 */
		check("帧数各报各的",
		      h.contains(QStringLiteral("#define LOGO_COUNT           1")) &&
		          h.contains(QStringLiteral("#define BLINK_COUNT          4")));
		check("去重只作用在自己那一份上",
		      h.contains(QStringLiteral("#define BLINK_DATA_COUNT     2")) &&
		          !h.contains(QStringLiteral("#define LOGO_DATA_COUNT")));

		/* 适配区一份就够，不能每份素材各来一遍让用户改 N 次 */
		check("适配区全文件只有一份",
		      h.count(QStringLiteral("\n#define OLED_WRITE_CMD(c)")) == 1 &&
		          h.count(QStringLiteral("\n#define OLED_WRITE_DATA(d)")) == 1 &&
		          h.count(QStringLiteral("\n#define OLED_DELAY(ms)")) == 1);

		/* include guard 跟着文件名走，两个素材包同时 include 也不会互相吃掉 */
		check("guard 取自文件名（assets.h → __ASSETS_H）",
		      h.contains(QStringLiteral("#ifndef __ASSETS_H")) &&
		          h.contains(QStringLiteral("#endif /* __ASSETS_H */")));

		/* 用户明确要的：点阵数据正上方写着导入时的文件名 */
		check("点阵正上方写了导入时的文件名（logo）",
		      h.contains(QStringLiteral("/* 点阵数据 —— Logo，导入自 logo.png */\n"
		                                "const uint8_t Logo_Frames[")));
		check("点阵正上方写了导入时的文件名（blink）",
		      h.contains(QStringLiteral("/* 点阵数据 —— Blink，导入自 blink.gif */\n"
		                                "const uint8_t Blink_Frames[")));
		check("抬头里也有来源那行",
		      h.contains(QStringLiteral("导入自  : logo.png")) &&
		          h.contains(QStringLiteral("导入自  : blink.gif")));

		/* 顺序跟着列表走 */
		const int p1 = h.indexOf(QStringLiteral("素材 1/2：Logo"));
		const int p2 = h.indexOf(QStringLiteral("素材 2/2：Blink"));
		check("块顺序和列表一致", p1 >= 0 && p2 > p1,
		      QStringLiteral("p1=%1 p2=%2").arg(p1).arg(p2));

		/* 名称撞车：两份都叫 logo 也必须编得过（自动补 _2），而不是生成两份一样的宏 */
		ImageGen::Asset dup = a2;
		dup.ident = QStringLiteral("logo");
		QVector<ImageGen::Asset> dupList;
		dupList << a1 << dup;
		const QString dh = ImageGen::renderMultiHeader(dupList, QStringLiteral("x.h"));
		check("重名自动补 _2，不会生成两套一样的宏",
		      dh.contains(QStringLiteral("#define LOGO_WIDTH")) &&
		          dh.contains(QStringLiteral("#define LOGO_2_WIDTH")) &&
		          dh.count(QStringLiteral("#define LOGO_WIDTH")) == 1);

		/* 空帧素材直接跳过 —— 生成 [0][N] 的数组，C 里编不过 */
		ImageGen::Asset empty;
		empty.ident = QStringLiteral("nothing");
		QVector<ImageGen::Asset> withEmpty;
		withEmpty << a1 << empty << a2;
		const QString eh = ImageGen::renderMultiHeader(withEmpty, QStringLiteral("y.h"));
		check("空帧素材被跳过，也不影响其它两份",
		      !eh.contains(QStringLiteral("Nothing_Frames")) &&
		          eh.contains(QStringLiteral("素材 1/2：Logo")) &&
		          eh.contains(QStringLiteral("素材 2/2：Blink")));

		check("全都是空帧时返回空串（不生成编不过的文件）",
		      ImageGen::renderMultiHeader(QVector<ImageGen::Asset>() << empty).isEmpty());

		/* 不给文件名时的兜底 guard */
		check("没给文件名时 guard 退回 __OLED_ASSETS_H",
		      ImageGen::renderMultiHeader(list).contains(
		          QStringLiteral("#ifndef __OLED_ASSETS_H")));

		/* 写盘 */
		const QString packPath = QDir(outDir).absoluteFilePath(QStringLiteral("assets_pack.h"));
		const QString perr = ImageGen::writeMultiHeaderFile(packPath, list,
		                                                    QStringLiteral("assets_pack.h"));
		check("打包写盘成功时返回空", perr.isEmpty(), perr);

		QFile pf(packPath);
		QString pw;
		if (pf.open(QIODevice::ReadOnly))
		{
			pw = QString::fromUtf8(pf.readAll());
			pf.close();
		}
		check("写出来的就是 renderMultiHeader 的结果",
		      !pw.isEmpty() &&
		          pw.replace(QStringLiteral("\r\n"), QStringLiteral("\n")) ==
		              ImageGen::renderMultiHeader(list, QStringLiteral("assets_pack.h")));
		check("写出来的文件里带文件名注释",
		      pw.contains(QStringLiteral("导入自 logo.png")));
		check("打包空路径被挡住",
		      !ImageGen::writeMultiHeaderFile(QString(), list).isEmpty());
		check("打包目录不存在时返回原因",
		      !ImageGen::writeMultiHeaderFile(
		           QDir(outDir).absoluteFilePath(QStringLiteral("nope/x.h")), list)
		           .isEmpty());
	}

	/* ---------------- 写文本文件（三条导出路径共用） ----------------
	 * 字库导出、自检帧、图片导出都是"生成一段文本 → 写进文件"，
	 * 而且都踩过同一个坑：open() 成功就报"导出成功"，磁盘满时
	 * 用户拿到半截文件。失败那一半（磁盘满）在测试里造不出来，
	 * 能测的是"失败必须给出原因"和"成功时内容/编码一字不差"。 */
	{
		const QString p = QDir(outDir).absoluteFilePath(QStringLiteral("text_utf8.h"));
		const QString body = QStringLiteral("/* 汉字 */\n#define X 1\n");
		const QString err = FontGen::writeTextFile(p, body);
		check("写文本文件成功时返回空", err.isEmpty(), err);

		QFile rf(p);
		QByteArray raw;
		if (rf.open(QIODevice::ReadOnly))
		{
			raw = rf.readAll();
			rf.close();
		}
		QString got = QString::fromUtf8(raw);
		check("写出来的内容原样（按 UTF-8 读回）",
		      got.replace(QStringLiteral("\r\n"), QStringLiteral("\n")) == body,
		      QStringLiteral("%1 字节").arg(got.size()));
		/* Keil 里汉字变花/变空的根因就是编码：必须是 UTF-8 三字节，不是 GBK */
		check("中文按 UTF-8 落盘（不是 GBK）",
		      raw.contains(QByteArray::fromHex("e6b189")),
		      QStringLiteral("没找到「汉」的 UTF-8 字节"));

		check("空路径被挡住", !FontGen::writeTextFile(QString(), body).isEmpty());
		check("目录不存在时返回原因",
		      !FontGen::writeTextFile(
		           QDir(outDir).absoluteFilePath(QStringLiteral("nope/x.h")), body)
		           .isEmpty());
		/* 目标是个已存在的目录：open 必须失败，不能当"写成功" */
		check("目标是目录时返回原因", !FontGen::writeTextFile(outDir, body).isEmpty());
	}

	/* ---------------- 字体里没有汉字的时候 ----------------
	 * 选 "Bahnschrift SemiBold Condensed"（西文字体，以及 Windows 上那批
	 * 把样式名当家族名报出来的字体）以后，点阵全是加粗的糊字。
	 * 查下来是 Qt 的处理：字体里没有汉字 -> 回退到宋体，但把 weight=600
	 * 一起带过去 -> 实际用「宋体·加粗」画 -> 16px 下笔画粘成一片。
	 * 这里钉三件事：认出没有汉字、换上的字体字重是 Normal、点阵和正经宋体一致。 */
	{
		FontGen plain(QFont(QStringLiteral("SimSun"), 16), 110, 16);
		check("宋体自己有汉字，不该被替换", !plain.cjkSubstituted(), plain.cjkFamily());
		check("没替换时汉字字体就是选的那个",
		      plain.cjkFamily().compare(QStringLiteral("SimSun"), Qt::CaseInsensitive) == 0,
		      plain.cjkFamily());

		/* Arial 每台 Windows 都有，且一定没有汉字 */
		FontGen latin(QFont(QStringLiteral("Arial"), 16), 110, 16);
		check("西文字体被认出「没有汉字」", latin.cjkSubstituted(), latin.cjkFamily());
		check("换上的字体不是空的", !latin.cjkFamily().isEmpty(), latin.cjkFamily());
		check("换上的字体字重是 Normal（不继承被选字体的字重）",
		      latin.cjkFont().weight() == QFont::Normal,
		      QString::number(static_cast<int>(latin.cjkFont().weight())));

		const QVector<QVector<bool>> ref = plain.glyphBits(QChar(u'嵌'));
		check("换字体后点阵和正经宋体逐格一致（不是加粗糊字）",
		      latin.glyphBits(QChar(u'嵌')) == ref);

		/* 用户报的就是这支：样式名当家族名、自带 weight=600。
		 * 机器上没装就跳过，不硬失败。 */
		const QString semiName = QStringLiteral("Bahnschrift SemiBold Condensed");
		if (QFontDatabase::families().contains(semiName))
		{
			FontGen semi(QFont(semiName), 110, 16);
			check("半粗体样式名也被认出没有汉字", semi.cjkSubstituted(), semi.cjkFamily());
			check("半粗体也压回 Normal",
			      semi.cjkFont().weight() == QFont::Normal,
			      QString::number(static_cast<int>(semi.cjkFont().weight())));
			check("半粗体的点阵同样和宋体一致", semi.glyphBits(QChar(u'嵌')) == ref);
		}

		/* 换字号后汉字字体的大小要跟着换，否则小字号用大像素画，整块被裁掉 */
		FontGen small(QFont(QStringLiteral("Arial"), 16), 110, 16);
		small.setSize(8);
		check("换字号后汉字字体也跟着换到 8px",
		      small.cjkFont().pixelSize() == 8,
		      QString::number(small.cjkFont().pixelSize()));
		check("8px 的点阵还是 8×8", small.glyphBits(QChar(u'嵌')).size() == 8);
	}

	/* ---------------- 小字号笔画糊：抗锯齿 vs 整像素 ----------------
	 * 用户报「选华文宋体取模，字是糊的」。查下来不是回退的问题，是渲染：
	 *
	 *   宋体 16×16 自带点阵位图，Qt 直接取位图，笔画是整像素的 —— 中间灰占 0%
	 *   宋体 24/32、华文宋体、微软雅黑、黑体，全是拿轮廓缩放 —— 中间灰占 50~60%
	 *
	 * 笔画只有 0.7 像素宽的时候，抗锯齿会把它摊成一片半灰，阈值切在哪儿
	 * 笔画就在哪儿断、在哪儿变粗，看上去就是糊的。所以这类情况要关掉抗锯齿。
	 *
	 * 这里钉四件事：什么时候该关、换字号后会重算、关掉后阈值不参与、
	 * 以及关掉后点阵里不该再有孤立单点（那正是糊的直观表现）。 */
	{
		FontGen s16(QFont(QStringLiteral("SimSun"), 16), 110, 16);
		check("宋体 16×16 自带点阵，不该关抗锯齿", !s16.cjkNoAa());

		FontGen s24(QFont(QStringLiteral("SimSun"), 16), 110, 24);
		check("宋体 24×24 是轮廓缩放，要关抗锯齿", s24.cjkNoAa());

		FontGen s32(QFont(QStringLiteral("SimSun"), 16), 110, 32);
		check("宋体 32×32 也是轮廓缩放，要关抗锯齿", s32.cjkNoAa());

		/* 换字号必须重新判断，否则 24 号切回 16 号还按整像素画 */
		s24.setSize(16);
		check("从 24 换回 16，判断跟着回到「自带点阵」", !s24.cjkNoAa());

		/* 整像素渲染后图里只有 0 和 255，阈值没有可切的位置 */
		FontGen thr(QFont(QStringLiteral("SimSun"), 16), 110, 24);
		const QVector<QVector<bool>> before = thr.glyphBits(QChar(u'温'));
		thr.setThreshold(30);
		check("整像素渲染时改阈值不改变点阵", thr.glyphBits(QChar(u'温')) == before);
		thr.setThreshold(220);
		check("阈值拉高也一样（不是被夹到某一边）", thr.glyphBits(QChar(u'温')) == before);

		/* 关抗锯齿只针对汉字，字母数字还走原来的字体和抗锯齿 */
		FontGen asc(QFont(QStringLiteral("SimSun"), 16), 110, 24);
		const QVector<QVector<bool>> letter = asc.glyphBits(QChar(u'A'));
		int inkA = 0;
		for (int y = 0; y < letter.size(); ++y)
		{
			for (int x = 0; x < letter[y].size(); ++x)
			{
				if (letter[y][x])
				{
					++inkA;
				}
			}
		}
		check("字母 A 仍然取得到点阵（ASCII 没被一起关掉）", inkA > 8,
		      QStringLiteral("亮 %1 点").arg(inkA));

		/* 有这几款字体就一并验证：它们在任何字号下都没有内嵌点阵 */
		const QStringList fams = QFontDatabase::families();
		const QStringList noBitmap = { QString::fromUtf8("华文宋体"),
		                               QString::fromUtf8("微软雅黑"),
		                               QString::fromUtf8("黑体") };
		for (const QString &name : noBitmap)
		{
			if (!fams.contains(name))
			{
				continue;
			}
			FontGen g(QFont(name, 16), 110, 16);
			const QByteArray noBitmapMsg =
			    QStringLiteral("「%1」16×16 没有点阵，要关抗锯齿").arg(name).toUtf8();
			check(noBitmapMsg.constData(), g.cjkNoAa(), g.cjkFamily());
			const QByteArray selfMsg =
			    QStringLiteral("「%1」汉字字体就是它自己").arg(name).toUtf8();
			check(selfMsg.constData(), !g.cjkSubstituted());
		}

		/* 糊字最直观的表现就是点阵里散着一堆孤立单点：笔画断开，
		 * 只留下一个点。整像素渲染后不该再出现。 */
		if (fams.contains(QString::fromUtf8("华文宋体")))
		{
			FontGen hw(QFont(QString::fromUtf8("华文宋体"), 16), 110, 16);
			const QString probe = QString::fromUtf8("\u5d4c\u7f51\u6e29\u5ea6");  /* 嵌网温度 */
			int lonely = 0;
			for (int i = 0; i < probe.size(); ++i)
			{
				const QVector<QVector<bool>> b = hw.glyphBits(probe.at(i));
				const int n = b.size();
				for (int y = 1; y + 1 < n; ++y)
				{
					for (int x = 1; x + 1 < n; ++x)
					{
						if (!b[y][x])
						{
							continue;
						}
						bool neighbour = false;
						for (int j = -1; j <= 1 && !neighbour; ++j)
						{
							for (int i2 = -1; i2 <= 1; ++i2)
							{
								if ((i2 || j) && b[y + j][x + i2])
								{
									neighbour = true;
									break;
								}
							}
						}
						if (!neighbour)
						{
							++lonely;
						}
					}
				}
			}
			check("华文宋体 16×16 四个字里没有孤立单点（笔画不断）", lonely == 0,
			      QStringLiteral("%1 个").arg(lonely));
		}
	}

	/* ---------------- 取反（反白） ----------------
	 * 取反是"成形之后整体对调"，所以只需盯三件事：
	 * 每一格是不是都反了、字形有没有被挪动、开关来回拨能不能复原。
	 */
	std::printf("[取反（反白）]\n");
	{
		FontGen plain(QFont(QStringLiteral("SimSun"), 16), 110, 16);
		check("默认不取反", !plain.invert());

		const QChar c = QChar(u'温');
		const QVector<QVector<bool>> base = plain.glyphBits(c);

		/* 复制一份再开取反：同一个对象上改会一起把缓存清掉，
		 * 那样就分不清"算错了"还是"缓存没清"了。 */
		FontGen inv(plain);
		inv.setInvert(true);
		check("开了以后 invert() 为真", inv.invert());

		const QVector<QVector<bool>> flipped = inv.glyphBits(c);

		bool cellOk = (flipped.size() == base.size());
		int litFlipped = 0, litBase = 0;
		for (int y = 0; y < base.size() && cellOk; ++y)
		{
			if (flipped.at(y).size() != base.at(y).size())
			{
				cellOk = false;
				break;
			}
			for (int x = 0; x < base.at(y).size(); ++x)
			{
				if (flipped.at(y).at(x) != !base.at(y).at(x))
				{
					cellOk = false;
					break;
				}
				if (flipped.at(y).at(x))
				{
					++litFlipped;
				}
				if (base.at(y).at(x))
				{
					++litBase;
				}
			}
		}
		check("每一格都和原来相反", cellOk);

		/* 互补就说明没有多补一个边、也没有把背景算成墨迹挪位置 */
		check("亮点的总数正好互补（16×16）", litFlipped + litBase == 16 * 16,
		      QStringLiteral("%1 + %2").arg(litFlipped).arg(litBase));
		check("原字不是全黑也不是全亮（这条测试本身有效）",
		      litBase > 0 && litBase < 16 * 16, QStringLiteral("%1").arg(litBase));

		const QVector<quint8> bytesPlain = plain.glyphBytes(c);
		const QVector<quint8> bytesInv   = inv.glyphBytes(c);
		bool xorOk = (bytesPlain.size() == bytesInv.size()) && !bytesPlain.isEmpty();
		for (int i = 0; xorOk && i < bytesPlain.size(); ++i)
		{
			if (static_cast<quint8>(bytesPlain.at(i) ^ bytesInv.at(i)) != 0xFFu)
			{
				xorOk = false;
			}
		}
		check("32 个字节逐字节都是异或 0xFF", xorOk,
		      QStringLiteral("%1 字节").arg(bytesPlain.size()));

		FontGen back(inv);
		back.setInvert(false);
		check("关掉以后点阵回到原样", back.glyphBits(c) == base);

		/* 缓存：同一个对象先算一次，再拨开关，必须拿到新的那份 */
		FontGen live(QFont(QStringLiteral("SimSun"), 16), 110, 16);
		const QVector<QVector<bool>> first = live.glyphBits(c);
		live.setInvert(true);
		check("拨开关后缓存被清掉（不是还在返回旧的）", live.glyphBits(c) != first);

		QString err;
		const QString h0 = plain.generateHeader(QStringLiteral("温度"), &err);
		const QString h1 = inv.generateHeader(QStringLiteral("温度"), &err);
		check("不取反时文件头不提反白", !h0.contains(QStringLiteral("已取反")));
		check("取反时文件头写明已取反", h1.contains(QStringLiteral("已取反")));
		check("两版文件头里的点阵字节数一样（只是值反了）",
		      macroValue(h0, QStringLiteral("OLED_CFONT_BYTES"))
		          == macroValue(h1, QStringLiteral("OLED_CFONT_BYTES")));
	}

	/* 图片这条路是打包成字节以后再逐字节求反的，单独验一次 */
	{
		ImageGen::Options o;
		o.width = 32;
		o.height = 16;
		o.threshold = 128;
		o.dither = ImageGen::NoDither;

		QImage src(64, 32, QImage::Format_Grayscale8);
		src.fill(255);
		for (int y = 0; y < 16; ++y)          /* 留一块全黑，保证黑白都有 */
		{
			uchar *line = src.scanLine(y);
			for (int x = 0; x < 16; ++x)
			{
				line[x] = 0;
			}
		}

		const QVector<quint8> p0 = ImageGen::convert(src, o);
		ImageGen::Options oi = o;
		oi.invert = true;
		const QVector<quint8> p1 = ImageGen::convert(src, oi);

		bool imgXorOk = (p0.size() == p1.size()) && !p0.isEmpty();
		int on0 = 0, on1 = 0;
		for (int i = 0; i < p0.size(); ++i)
		{
			if (static_cast<quint8>(p0.at(i) ^ p1.at(i)) != 0xFFu)
			{
				imgXorOk = false;
			}
			for (int b = 0; b < 8; ++b)
			{
				if (p0.at(i) & (1u << b))
				{
					++on0;
				}
				if (p1.at(i) & (1u << b))
				{
					++on1;
				}
			}
		}
		check("图片取反：每个字节都是补码", imgXorOk,
		      QStringLiteral("%1 字节").arg(p0.size()));
		check("图片取反：亮的点数互补（32×16 = 512）", on0 + on1 == 32 * 16,
		      QStringLiteral("%1 + %2").arg(on0).arg(on1));

		QVector<QVector<quint8>> one0, one1;
		one0.append(p0);
		one1.append(p1);
		const QString ih0 = ImageGen::renderHeader(one0, o, 100, QStringLiteral("a.png"));
		const QString ih1 = ImageGen::renderHeader(one1, oi, 100, QStringLiteral("a.png"));
		check("图片不反白时文件头不提反白", !ih0.contains(QStringLiteral("反白")));
		check("图片反白时文件头写明", ih1.contains(QStringLiteral("反白")));
	}

	/* ---------------- 多份素材合计 ----------------
	 * 这一行是拿去判断「装不装得下」的，算错了用户会带着超容量的字库去烧。
	 * 列表允许混装不同尺寸，所以盯的是「每份按自己的尺寸算」这一条。
	 */
	std::printf("[多份素材合计]\n");
	{
		const ImageGen::Usage empty = ImageGen::totalUsage(QVector<ImageGen::Footprint>());
		check("没有素材时合计为 0", empty.frames == 0 && empty.bytes == 0);

		QVector<ImageGen::Footprint> items;
		items.append({ 128, 64, 24 });    /* 1024 字节/帧 */
		items.append({ 64, 64, 1 });      /*  512 字节/帧 */
		items.append({ 16, 16, 30 });     /*   32 字节/帧 */
		const ImageGen::Usage u = ImageGen::totalUsage(items);
		check("合计帧数 = 各份帧数相加", u.frames == 55, QString::number(u.frames));
		const qint64 want = 128 * 8 * 24 + 64 * 8 * 1 + 16 * 2 * 30;
		check("合计字节 = 每份按自己的尺寸算完再相加", u.bytes == want,
		      QStringLiteral("%1，应为 %2").arg(u.bytes).arg(want));

		/* 反例：拿最大那份的每帧字节乘总帧数会得到 55 × 1024 = 56320，
		 * 比真实值大得多。这条就是防有人图省事写成"取一份的每帧字节" */
		check("合计不等于「某一帧字节 × 总帧数」", u.bytes != qint64(128 * 8) * u.frames);

		QVector<ImageGen::Footprint> zero;
		zero.append({ 128, 64, 0 });      /* 抽帧抽到一帧不剩，或者读图失败 */
		const ImageGen::Usage z = ImageGen::totalUsage(zero);
		check("帧数为 0 的素材不占字节", z.frames == 0 && z.bytes == 0);

		/* 合并成一个动画的只是其中一种用法；各存一份时总帧数一样、尺寸可能不一样，
		 * 所以同样帧数下字节数只由尺寸决定 */
		QVector<ImageGen::Footprint> one;
		one.append({ 128, 64, 10 });
		QVector<ImageGen::Footprint> two;
		two.append({ 128, 64, 5 });
		two.append({ 128, 64, 5 });
		check("同样帧数、同样尺寸，拆成两份与合成一份占地相同",
		      ImageGen::totalUsage(one).bytes == ImageGen::totalUsage(two).bytes);
	}

	/* ---------------- 4 字节字符 ----------------
	 * 「𠮷」这种扩展 B 区生僻字和 emoji，UTF-8 是 4 字节，在 QString 里存成
	 * 两个代理项。逐 QChar 处理的话会各生成一张点阵，而两张都是无效的
	 * （半个代理项画不出东西），显示函数又根本不查它的表 —— 白占 Flash。 */
	std::printf("[4 字节字符]\n");
	{
		FontGen gen(QFont(QStringLiteral("SimSun"), 16), 110, 16);

		/* 用一个代理对拼出 4 字节字符：C++ 的 \u 不允许直接写代理项，
		 * 只好按 UTF-8 字节还原。U+20BB7 是 F0 A0 AE B7。 */
		auto fourByte = [](const char *utf8) { return QString::fromUtf8(utf8); };

		const QString mixed = QStringLiteral("张") + fourByte("\xF0\xA0\xAE\xB7");   /* 张 + 𠮷 */
		check("生僻字不拆成两个字形", FontGen::glyphCount(mixed) == 1,
		      QString::number(FontGen::glyphCount(mixed)));

		QString err;
		const QString h = gen.generateHeader(mixed, &err);
		check("含生僻字时仍然生成得出来", !h.isEmpty(), err);
		check("文件头说明跳过了几个 4 字节字符",
		      h.contains(QStringLiteral("4 字节字符未收录")), QString());

		/* 纯 emoji：一个字形都不该生成，且要说清为什么 */
		const QString emoji = fourByte("\xF0\x9F\x98\x80");                        /* U+1F600 */
		QString err2;
		const QString h2 = gen.generateHeader(emoji, &err2);
		check("纯 emoji 不生成字库", h2.isEmpty(), QString::number(h2.size()));
		check("纯 emoji 时说清了原因", err2.contains(QStringLiteral("4 字节")), err2);
		check("纯 emoji 时字数是 0", FontGen::glyphCount(emoji) == 0,
		      QString::number(FontGen::glyphCount(emoji)));
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
