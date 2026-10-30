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

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
