/*==============================================================================
 *  selftest.cpp  ——  自检帧生成
 *
 *  作者   zhangwtee
 *  GitHub https://github.com/zhangwtee
 *  Gitee  https://gitee.com/z_w_t
 *==============================================================================*/
#include "selftest.h"

#include "fontgen.h"
#include "imagegen.h"

namespace
{

/* Format_Mono 每字节 8 个像素，最高位是最左边那个 —— 和 ImageGen 的打包约定一致 */
void setPx(QImage &img, int x, int y)
{
	if (x < 0 || y < 0 || x >= img.width() || y >= img.height())
	{
		return;
	}
	img.scanLine(y)[x >> 3] |= static_cast<uchar>(0x80u >> (x & 7));
}

void hLine(QImage &img, int x0, int x1, int y)
{
	for (int x = x0; x <= x1; ++x)
	{
		setPx(img, x, y);
	}
}

void vLine(QImage &img, int x, int y0, int y1)
{
	for (int y = y0; y <= y1; ++y)
	{
		setPx(img, x, y);
	}
}

void rectOutline(QImage &img, int x0, int y0, int x1, int y1)
{
	hLine(img, x0, x1, y0);
	hLine(img, x0, x1, y1);
	vLine(img, x0, y0, y1);
	vLine(img, x1, y0, y1);
}

void fillRect(QImage &img, int x0, int y0, int x1, int y1)
{
	for (int y = y0; y <= y1; ++y)
	{
		hLine(img, x0, x1, y);
	}
}

QString hex2(quint8 v)
{
	static const char digits[] = "0123456789ABCDEF";
	QString s = QStringLiteral("0x");
	s += QLatin1Char(digits[(v >> 4) & 0x0F]);
	s += QLatin1Char(digits[v & 0x0F]);
	return s;
}

}   /* namespace */

/*
 * 图面设计：
 *
 *   +--------------------------------+
 *   |##                            []|   左上实心 / 右上空心
 *   |##                            []|
 *   |                                |
 *   |           取 模 正 确           |   当前字号，水平垂直居中
 *   |                                |
 *   |XX                            ##|   左下交叉 / 右下棋盘
 *   |XX                            ##|
 *   +--------------------------------+
 *
 * 四角刻意做成四种不同图案。任何"整屏翻转 / 镜像 / 页序颠倒"都会让角标
 * 互相串位，而外框则负责暴露列偏移（比如 SH1106 少接 2 列时右边框会消失）。
 * 中央汉字点阵上下并不对称，位序反了会直接变成镜像字或乱码。
 */
QImage SelfTest::buildImage(const FontGen *gen, const QString &sample, int w, int h)
{
	if (!sizeOk(w, h))
	{
		return QImage();
	}

	QImage img(w, h, QImage::Format_Mono);
	img.setColorCount(2);
	img.setColor(0, qRgb(0, 0, 0));
	img.setColor(1, qRgb(255, 255, 255));
	img.fill(0);

	/* 外框：一条 1px 的整屏矩形 */
	rectOutline(img, 0, 0, w - 1, h - 1);

	/*
	 * 四个角标都往里缩 2px，和外框留一道缝。
	 * 最早是贴着外框画的，结果实心块和左边框、上边框连成一整片，
	 * 看起来只是"边框加粗了"，根本认不出是个方块 —— 角标的意义就没了。
	 *
	 * 边长按屏高自适应：128x64 用 7px 方块看得清；128x32 只有 32 行，
	 * 仍用 7px 的话上下两块加上中间 16px 的字会挤成一团，缩到 4px。
	 */
	const int A = 2;        /* 距外框的距离 */
	const int K = (h >= 48) ? 6 : 3;    /* 图案边长 = K + 1 */

	/* 左上：实心方块 */
	fillRect(img, A, A, A + K, A + K);

	/* 右上：空心方框 */
	rectOutline(img, w - 1 - A - K, A, w - 1 - A, A + K);

	/* 左下：交叉 */
	for (int k = 0; k <= K; ++k)
	{
		const int y = h - 1 - A - K + k;
		setPx(img, A + k, y);
		setPx(img, A + K - k, y);
	}

	/* 右下：棋盘格 */
	for (int y = h - 1 - A - K; y <= h - 1 - A; ++y)
	{
		for (int x = w - 1 - A - K; x <= w - 1 - A; ++x)
		{
			if (((x + y) & 1) == 0)
			{
				setPx(img, x, y);
			}
		}
	}

	/* 中央文字。用 FontGen::glyphBits，和字库是同一份点阵，不另写渲染 */
	if (gen && !sample.isEmpty())
	{
		const int S = gen->size();

		/* 字比屏高还大就整行不画：128x32 屏配 32x32 字号时会正好盖满，
		 * 把外框和四角一起吃掉，自检帧就看不出问题在哪了。 */
		if (S <= h - 4)
		{
			int n = sample.size();
			const int maxN = (w - 4) / S;      /* 留 2px 边距，别贴到外框上 */
			if (n > maxN)
			{
				n = maxN;
			}

			if (n > 0)
			{
				const int y0 = (h - S) / 2;
				const int x0 = (w - n * S) / 2;

				for (int i = 0; i < n; ++i)
				{
					const QVector<QVector<bool>> bits = gen->glyphBits(sample.at(i));
					for (int yy = 0; yy < S && yy < bits.size(); ++yy)
					{
						const QVector<bool> &row = bits.at(yy);
						for (int xx = 0; xx < S && xx < row.size(); ++xx)
						{
							if (row.at(xx))
							{
								setPx(img, x0 + i * S + xx, y0 + yy);
							}
						}
					}
				}
			}
		}
	}

	return img;
}

QVector<quint8> SelfTest::pack(const QImage &mono)
{
	return ImageGen::packToDisplay(mono);
}

QString SelfTest::renderHeader(const QVector<quint8> &bytes,
                               const QString &sample,
                               int charSize,
                               const QString &fontFamily,
                               int colOffset,
                               int w,
                               int h)
{
	if (!sizeOk(w, h) || bytes.size() != bytesOf(w, h))
	{
		return QString();
	}
	if (colOffset < 0)
	{
		colOffset = 0;
	}
	if (colOffset > 8)
	{
		colOffset = 8;
	}

	const int pages = h / 8;
	const int size  = bytes.size();

	QString s;

	s += QStringLiteral(
		"/* ==========================================================================\n"
		" *  OLED 全屏自检帧   %1 x %2   共 %3 字节\n"
		" *\n"
		" *  用途：烧进板子整屏刷一次，确认「取模工具的输出格式」和「你的驱动」\n"
		" *        是同一套。字库里的单个字对不对肉眼很难判断，但一整屏的边框、\n"
		" *        四角、文字对不对劲，一眼就能看出来。\n"
		" *\n"
		" *  正常应该看到（示意图按 128 x 64 画的，你的屏行数按实际看）：\n"
		" *\n"
		" *      +------------------------------------------+\n"
		" *      |##                                      []|\n"
		" *      |##                                      []|\n"
		" *      |                                          |\n"
		" *      |               取 模 正 确                 |\n"
		" *      |                                          |\n"
		" *      |XX                                      ##|\n"
		" *      |XX                                      ##|\n"
		" *      +------------------------------------------+\n"
		" *\n"
		" *      左上实心 / 右上空心 / 左下交叉 / 右下棋盘 —— 四个角刻意不一样，\n"
		" *      所以整屏翻转、镜像、页序颠倒都会让角标串位，不用放大镜也能发现。\n"
		" *\n"
		" *  对不上的话，按现象查原因：\n"
		" *      右边框缺两列 / 整幅左移 2 像素   -> 屏是 SH1106(132 列)，把 OLED_SELFTEST_COL_OFFSET 改成 2\n"
		" *      画面上下颠倒                     -> 页序反了(0xC8 与 0xC0 对调)\n"
		" *      黑底白点变成白底黑点             -> 极性反了(阳码 / 阴码)\n"
		" *      字是花条纹或上下错位             -> 位序反了(高位在前 / 低位在前)\n"
		" *      有边框但中间没字                 -> 生成时没勾「包含中文」\n"
		" *      字的上下两半各偏了一点           -> 驱动写页时页地址算错了(少乘一次)\n"
		" *      下半屏整块黑、或内容重复一遍     -> 屏其实是 %4 行，自检帧按 %2 行生成的\n"
		" * ========================================================================== */\n"
		"\n")
		.arg(w)
		.arg(h)
		.arg(size)
		.arg(h == 64 ? 32 : 64);

	if (!sample.isEmpty())
	{
		s += QStringLiteral(
			"/* 中间的样例文字：%1（%2x%2 点阵，%3） */\n\n")
			.arg(sample.mid(0, (w - 4) / qMax(1, charSize)))
			.arg(charSize)
			.arg(fontFamily.isEmpty() ? QStringLiteral("宋体") : fontFamily);
	}
	else
	{
		s += QStringLiteral("/* 本帧不含汉字，只有外框和四角 —— 用于在装字库之前先验证屏和驱动 */\n\n");
	}

	s += QStringLiteral(
		"#ifndef __OLED_SELFTEST_H\n"
		"#define __OLED_SELFTEST_H\n"
		"\n"
		"#include <stdint.h>\n"
		"\n"
		"#define OLED_SELFTEST_W       %1\n"
		"#define OLED_SELFTEST_H       %2\n"
		"#define OLED_SELFTEST_PAGES   %3\n"
		"#define OLED_SELFTEST_SIZE    %4\n"
		"#define OLED_SELFTEST_COL_OFFSET %5\n"
		"\n"
		"/* ============================================================\n"
		" *  【适配区】只改这两行，下面的代码不用动\n"
		" *\n"
		" *  和字库、动画头文件一样，本文件只跟驱动交换「写一条命令」「写一个\n"
		" *  字节数据」两个动作，不调用 OLED_SetCursor —— 那个函数各例程的参数\n"
		" *  顺序不一致（有的页在前、有的列在前），绕开它就不用对着改。\n"
		" *\n"
		" *  (A) 命令和数据分成两个函数（中景园新版、多数 HAL 例程）\n"
		" *      #define OLED_SELFTEST_WRITE_CMD(c)   OLED_WriteCommand(c)\n"
		" *      #define OLED_SELFTEST_WRITE_DATA(d)  OLED_WriteData(d)\n"
		" *\n"
		" *  (B) 一个函数加 cmd 标志位（正点原子、中景园旧版）\n"
		" *      #define OLED_SELFTEST_WRITE_CMD(c)   OLED_WR_Byte((c), 1)\n"
		" *      #define OLED_SELFTEST_WRITE_DATA(d)  OLED_WR_Byte((d), 0)\n"
		" * ============================================================ */\n"
		"#define OLED_SELFTEST_WRITE_CMD(c)    OLED_WriteCommand(c)\n"
		"#define OLED_SELFTEST_WRITE_DATA(d)   OLED_WriteData(d)\n"
		"\n"
		"/* 数据格式：阴码 / 列行式 / 逆向(低位在前) —— 和汉字字库完全一致\n"
		" * 每字节 = 一列的 8 个像素，bit0 在最上面；按页排列，每页 %1 字节\n"
		" *\n"
		" * 加 static 是为了让多个 .c 都能包含这个文件而不报重复定义。\n"
		" * 代价是每个包含它的 .c 各占一份 Flash；只在一个 .c 里用就把 static 去掉。 */\n"
		"static const uint8_t OLED_SelfTest[OLED_SELFTEST_PAGES][OLED_SELFTEST_W] =\n"
		"{\n")
		.arg(w)
		.arg(h)
		.arg(pages)
		.arg(size)
		.arg(colOffset);

	for (int p = 0; p < pages; ++p)
	{
		s += QStringLiteral("\t{   /* 第 %1 页   y = %2..%3 */\n")
		         .arg(p)
		         .arg(p * 8)
		         .arg(p * 8 + 7);

		for (int i = 0; i < w; i += 16)
		{
			const int end = qMin(i + 16, w);
			s += QStringLiteral("\t\t");
			for (int k = i; k < end; ++k)
			{
				s += hex2(bytes.at(p * w + k));
				if (k < w - 1)
				{
					s += QLatin1Char(',');
				}
			}
			s += QLatin1Char('\n');
		}
		s += QStringLiteral("\t},\n");
	}

	s += QStringLiteral(
		"};\n"
		"\n"
		"/*\n"
		" * 整屏显示一次。\n"
		" *\n"
		" * 定位命令自己发（0xB0|页、列地址高低 4 位），不调用 OLED_SetCursor，\n"
		" * 只依赖【适配区】指向的那两个最底层的函数。\n"
		" *\n"
		" * 用法：\n"
		" *     1) 本文件放到 OLED.c 所在目录（比如 Hardware\\）\n"
		" *     2) main.c 里 #include \"OLED_SelfTest.h\"\n"
		" *     3) OLED_Init();\n"
		" *        OLED_SelfTest_Show();\n"
		" *        while (1);\n"
		" */\n"
		"static void OLED_SelfTest_Show(void)\n"
		"{\n"
		"\tuint8_t page, col;\n"
		"\n"
		"\tfor (page = 0; page < OLED_SELFTEST_PAGES; page++)\n"
		"\t{\n"
		"\t\tOLED_SELFTEST_WRITE_CMD((uint8_t)(0xB0 | (page & 0x07)));\n"
		"\t\tOLED_SELFTEST_WRITE_CMD((uint8_t)(0x10 | ((OLED_SELFTEST_COL_OFFSET >> 4) & 0x0F)));\n"
		"\t\tOLED_SELFTEST_WRITE_CMD((uint8_t)(0x00 | (OLED_SELFTEST_COL_OFFSET & 0x0F)));\n"
		"\n"
		"\t\tfor (col = 0; col < OLED_SELFTEST_W; col++)\n"
		"\t\t{\n"
		"\t\t\tOLED_SELFTEST_WRITE_DATA(OLED_SelfTest[page][col]);\n"
		"\t\t}\n"
		"\t}\n"
		"}\n"
		"\n"
		"#endif /* __OLED_SELFTEST_H */\n");

	return s;
}
