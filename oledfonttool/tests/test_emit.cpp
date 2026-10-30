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

#include <QDir>
#include <QFile>
#include <QGuiApplication>
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

		writeFile(QDir(outDir).absoluteFilePath(QStringLiteral("OLED_CFont32.h")), h32);

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

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
