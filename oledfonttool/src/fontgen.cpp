// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 zhangwtee <https://github.com/zhangwtee>
// 本文件属于「OLED 点阵取模工具」。改动请保留以上两行并注明改了什么（Apache-2.0 §4b）。
/*==============================================================================
 *  fontgen.cpp  ——  汉字渲染成 N×N 点阵并编码成 OLED 字节
 *
 *  作者   zhangwtee
 *  GitHub https://github.com/zhangwtee
 *  Gitee  https://gitee.com/z_w_t
 *==============================================================================*/
#include "fontgen.h"

#include "watermark.h"

#include <QFile>
#include <QImage>
#include <QPainter>
#include <QRegularExpression>
#include <QRawFont>
#include <QSet>
#include <QStringConverter>
#include <QTextStream>
#include <algorithm>

FontGen::FontGen(const QFont &font, int threshold, int size)
	: m_font(font), m_threshold(threshold),
	  m_size(isSupportedSize(size) ? size : DEFAULT_SIZE)
{
	if (m_font.family().isEmpty())
	{
		m_font = QFont(QStringLiteral("SimSun"), m_size);   /* 和取模软件的"宋体 N"对应 */
	}
	m_font.setPixelSize(m_size);
	resolveCjkFont();
}

/* 这套字体自己有没有这个字的字形。
 * 用 QRawFont 而不是 QFontMetrics::inFont —— 后者会把 Qt 的"回退字体链"
 * 也算成有，正好把要查的情况盖住。 */
static bool fontHasChar(const QFont &f, QChar ch)
{
	const QRawFont raw = QRawFont::fromFont(f);
	return raw.isValid() && raw.supportsCharacter(ch);
}

/*
 * 决定汉字用哪套字体画。
 *
 * 起因：选 "Bahnschrift SemiBold Condensed"（或 Arial、Consolas 这类西文字体）
 * 以后，取出来的点阵全是加粗的糊字。查下来 Qt 的处理是：
 *
 *     请求 Bahnschrift SemiBold Condensed  ->  原始字体 weight=600
 *     它没有汉字  ->  Qt 回退到 SimSun，但 weight 还是 600  ->  宋体·加粗
 *
 * 16×16 的格子里，加粗宋体笔画两像素宽，相邻笔画连成一片，字就认不出来了。
 *
 * 所以：选中的字体自己就能画汉字，原样用（字重是用户选的，尊重）；
 * 不能画，就换成一款确定含汉字的字体，并【把字重压回 Normal】。
 */
void FontGen::resolveCjkFont()
{
	/* 挑一个几乎每款中文字体都有的字来试探。用"嵌"而不是"一"，
	 * 因为只有一根横的字，某些残缺字库也有。 */
	const QChar probe(u'嵌');

	if (fontHasChar(m_font, probe))
	{
		m_cjkFont = m_font;
		m_cjkSubstituted = false;
		detectCjkNoAa();
		return;
	}

	/* 按常见程度排，中文名在前 —— 界面上要原样显示这个家族名，
	 * 「宋体」比「SimSun」更像给中文用户看的字。系统里没有中文名会退到下一个。 */
	static const char *const cands[] = {
		"宋体", "SimSun",
		"黑体", "SimHei",
		"微软雅黑", "Microsoft YaHei",
		"等线", "DengXian",
		"仿宋", "FangSong",
		"楷体", "KaiTi",
	};

	m_cjkFont = QFont();
	for (const char *name : cands)
	{
		QFont f(QString::fromUtf8(name));
		f.setPixelSize(m_size);
		f.setWeight(QFont::Normal);      /* 这句是关键：不回退成加粗 */
		if (fontHasChar(f, probe))
		{
			m_cjkFont = f;
			m_cjkSubstituted = true;
			detectCjkNoAa();
			return;
		}
	}

	/* 一款中文字体都没有（理论上不可能），退回默认，至少别崩 */
	m_cjkFont = QFont(QStringLiteral("SimSun"));
	m_cjkFont.setPixelSize(m_size);
	m_cjkFont.setWeight(QFont::Normal);
	m_cjkSubstituted = true;
	detectCjkNoAa();
}

/*
 * 这个字体在这个字号下，是"自带点阵"还是"拿轮廓缩放"。
 *
 * 判别办法：照常抗锯齿画一遍，数一数墨迹像素里有多少是中间灰
 * （既不是全黑也不是全白）。自带点阵的字形只有 0 和 255，中间灰占比是 0；
 * 轮廓缩放的笔画宽不到一个像素，边缘全是半灰，占比在 50~60%。
 *
 * 实测（本机，四个探测字取平均）：
 *             16px  24px  32px
 *   华文宋体    60%   58%   51%
 *   宋体         0%   68%   59%      <- 只有 16px 那格是内嵌点阵
 *   微软雅黑    61%   58%   48%
 *   黑体        57%   57%   51%
 *
 * 所以分界取 12%：两头都留着四十多个点的余量，不会在边界上来回跳。
 */
void FontGen::detectCjkNoAa()
{
	m_cjkNoAa = false;
	if (m_cjkFont.family().isEmpty())
	{
		return;
	}

	/* 笔画太少的字（一、十）在哪个字号下都近乎纯黑白，测不出东西，
	 * 所以挑三个笔画密、结构复杂的。 */
	static const ushort probes[] = { 0x5D4C, 0x6E29, 0x7F51 };   /* 嵌 温 网 */

	const int S = m_size;
	int mid = 0, ink = 0;
	for (ushort u : probes)
	{
		QImage img(S, S, QImage::Format_Grayscale8);
		img.fill(0);
		{
			QPainter p(&img);
			p.setRenderHint(QPainter::TextAntialiasing, true);
			p.setFont(m_cjkFont);
			p.setPen(Qt::white);
			p.drawText(QRect(0, 0, S, S), Qt::AlignLeft | Qt::AlignTop, QString(QChar(u)));
		}
		for (int y = 0; y < S; ++y)
		{
			const uchar *line = img.constScanLine(y);
			for (int x = 0; x < S; ++x)
			{
				if (line[x] > 0)
				{
					++ink;
					/* 容一点边界：1~24 和 231~254 差一点点也算实心，
					 * 免得量化噪声把这套判断带到另一边去 */
					if (line[x] > 24 && line[x] < 231)
					{
						++mid;
					}
				}
			}
		}
	}

	if (ink > 0)
	{
		m_cjkNoAa = (mid * 100 / ink) > 12;
	}
}

QString FontGen::tableName(int size)
{
	return QStringLiteral("OLED_CF%1x%2").arg(size).arg(size);
}

void FontGen::setFont(const QFont &font)
{
	m_font = font;
	m_font.setPixelSize(m_size);
	resolveCjkFont();
	m_cache.clear();
}

/* 换字号必须连字体像素大小一起换，否则 QPainter 还是按旧尺寸渲染，
 * 画出来会被裁掉或者只占左上角一小块。 */
void FontGen::setSize(int size)
{
	if (!isSupportedSize(size) || size == m_size)
	{
		return;
	}
	m_size = size;
	m_font.setPixelSize(m_size);
	resolveCjkFont();               /* 字号变了，汉字字体的大小也得跟着变 */
	m_cache.clear();
}

void FontGen::setThreshold(int threshold)
{
	if (threshold != m_threshold)
	{
		m_threshold = threshold;
		m_cache.clear();
	}
}

/* 取反。缓存里的点阵也是取反前的，必须跟着清掉，
 * 否则勾上开关后预览还显示旧的那一份。 */
void FontGen::setInvert(bool on)
{
	if (on != m_invert)
	{
		m_invert = on;
		m_cache.clear();
	}
}

/*
 * 渲染：N×N 画布上画字 -> 取墨迹边界 -> 整体平移居中 -> 按阈值二值化
 * 平移这一步不能省：宋体的字形在字身框里天然偏左偏上，不居中的话点阵会贴边
 *
 * 汉字用的是 m_cjkFont（可能是替换过的那套），ASCII 还用用户选的字体：
 * 西文字体画字母本来是正常的，没必要一起换掉。
 *
 * 关不关抗锯齿看 m_cjkNoAa（见 detectCjkNoAa）：
 *   自带点阵的字形（宋体 16×16）本来就只有黑白两种值，开不开都一样，走老路；
 *   轮廓缩放的字体必须关掉，否则笔画边缘全是半灰，阈值切出来的粗细不匀。
 * 关掉以后图里只有 0 / 255，阈值自然不参与（宋体 16×16 本来也是这样）。
 */
QVector<QVector<bool>> FontGen::renderBits(QChar ch) const
{
	const int S = m_size;
	const bool ascii = (ch.unicode() < 0x80);
	const bool noAa = !ascii && m_cjkNoAa;

	QImage img(S, S, QImage::Format_Grayscale8);
	img.fill(0);

	{
		QFont f = ascii ? m_font : m_cjkFont;
		if (noAa)
		{
			f.setStyleStrategy(QFont::NoAntialias);
		}
		QPainter p(&img);
		p.setRenderHint(QPainter::TextAntialiasing, !noAa);
		p.setFont(f);
		p.setPen(Qt::white);
		p.drawText(QRect(0, 0, S, S), Qt::AlignLeft | Qt::AlignTop, QString(ch));
	}

	/* 墨迹边界 */
	int x0 = S, y0 = S, x1 = -1, y1 = -1;
	for (int y = 0; y < S; ++y)
	{
		const uchar *line = img.constScanLine(y);
		for (int x = 0; x < S; ++x)
		{
			if (line[x] > 0)
			{
				x0 = std::min(x0, x);
				y0 = std::min(y0, y);
				x1 = std::max(x1, x);
				y1 = std::max(y1, y);
			}
		}
	}

	int dx = 0, dy = 0;
	if (x1 >= x0 && y1 >= y0)
	{
		dx = (S - (x1 - x0 + 1)) / 2 - x0;
		dy = (S - (y1 - y0 + 1)) / 2 - y0;
	}

	QImage moved(S, S, QImage::Format_Grayscale8);
	moved.fill(0);
	{
		QPainter p2(&moved);
		p2.drawImage(dx, dy, img);              /* 平移，超出部分自动裁掉 */
	}

	const int thr = noAa ? 1 : m_threshold;
	QVector<QVector<bool>> bits(S, QVector<bool>(S, false));
	for (int y = 0; y < S; ++y)
	{
		const uchar *line = moved.constScanLine(y);
		for (int x = 0; x < S; ++x)
		{
			bits[y][x] = (line[x] >= thr);
		}
	}

	/* 取反放在最后：居中、二值化都是按"墨迹"判的，先取反会把墨迹判成背景，
	 * 字形会被挪到别处去了。 */
	if (m_invert)
	{
		for (int y = 0; y < S; ++y)
		{
			for (int x = 0; x < S; ++x)
			{
				bits[y][x] = !bits[y][x];
			}
		}
	}
	return bits;
}

/* 缓存可变，所以标记 mutable；对外接口仍然是 const */
QVector<QVector<bool>> FontGen::glyphBits(QChar ch) const
{
	QMap<QChar, QVector<QVector<bool>>>::const_iterator it = m_cache.constFind(ch);
	if (it != m_cache.constEnd())
	{
		return it.value();
	}
	QVector<QVector<bool>> bits = renderBits(ch);
	m_cache.insert(ch, bits);
	return bits;
}

/* 列行式 + 逆向(低位在前)：每列取 8 行，低位在上；按页从上往下排 */
QVector<quint8> FontGen::glyphBytes(QChar ch) const
{
	const QVector<QVector<bool>> bits = glyphBits(ch);
	const int S = m_size;
	const int pages = S / 8;

	QVector<quint8> out;
	out.reserve(bytesFor(S));

	for (int page = 0; page < pages; ++page)
	{
		for (int x = 0; x < S; ++x)
		{
			quint8 b = 0;
			for (int i = 0; i < 8; ++i)
			{
				if (bits[page * 8 + i][x])
				{
					b = static_cast<quint8>(b | (1 << i));
				}
			}
			out.append(b);
		}
	}
	return out;
}

/* 把文本里要取模的字符挑出来：去重，保持出现的先后顺序。
 *
 * 预览和导出都走这一个入口 —— 各写一套的话，迟早会一边跳过 4 字节字符
 * 另一边不跳，界面上显示「共 3 字」而导出的字库只有 2 个字。
 *
 * 4 字节字符（emoji、扩展 B 区生僻字如「𠮷」）在 QString 里存成两个代理项。
 * 要是逐 QChar 处理，同一个字会被当成两个不同的字各生成一张点阵 —— 而
 * 生成出来的两张都是无效的（半个代理项画不出东西），显示函数又根本不查
 * 它的表，只是一个字宽两个字宽地跳过去。结果就是白占 Flash，字数统计还
 * 翻倍。这里整个跳过，顺便把个数记下来告诉用户。
 *
 * ASCII 也不进这张表：走驱动自带的 8x16 字库。
 *
 * skipped4 传 nullptr 表示不关心个数。 */
static QVector<QChar> glyphCharsOf(const QString &text, int *skipped4)
{
	QSet<QChar>    set;
	QVector<QChar> list;
	int            skip = 0;

	for (int i = 0; i < text.size(); ++i)
	{
		const QChar c = text.at(i);

		if (c.isHighSurrogate())
		{
			/* 成对的算一个字符，落单的也只算一个 */
			if (i + 1 < text.size() && text.at(i + 1).isLowSurrogate())
			{
				++i;
			}
			++skip;
			continue;
		}
		if (c.isLowSurrogate())
		{
			++skip;
			continue;
		}
		if (c.unicode() > 0x7F && !set.contains(c))
		{
			set.insert(c);
			list.append(c);
		}
	}

	if (skipped4)
	{
		*skipped4 = skip;
	}
	return list;
}

/* 把一个字写成 C 字符串字面量，形如 "\xE5\xBC\xA0"。
 *
 * 每个字节一律写成 \xNN，绝不用可打印字符代替：这样不管字里有没有引号、
 * 反斜杠、% 号，粘进代码都是安全的。而且每段都自带反斜杠，避免了
 * "\xE5a" 这种 \x 转义把后面紧跟的十六进制字符一起吞掉的经典坑。
 *
 * 数值两位大写，和字模数据的十六进制保持一致，看着整齐。 */
static QString cLiteralOf(const QChar &ch)
{
	static const char digits[] = "0123456789ABCDEF";
	const QByteArray  u8       = QString(ch).toUtf8();

	QString s = QLatin1String("\"");
	for (int i = 0; i < u8.size(); ++i)
	{
		const quint8 v = static_cast<quint8>(u8.at(i));
		s += QStringLiteral("\\x");
		s += QLatin1Char(digits[(v >> 4) & 0x0F]);
		s += QLatin1Char(digits[v & 0x0F]);
	}
	s += QLatin1Char('"');
	return s;
}

QVector<QChar> FontGen::glyphChars(const QString &text)
{
	return glyphCharsOf(text, nullptr);
}

int FontGen::glyphCount(const QString &text)
{
	return glyphCharsOf(text, nullptr).size();
}

qint64 FontGen::flashBytes(int count, int size, bool withAsciiFont)
{
	/* 汉字表 count * 每字字节；ASCII 8x16 字库固定 95*16 字节 */
	return static_cast<qint64>(count) * bytesFor(size) + (withAsciiFont ? 95 * 16 : 0);
}

QString FontGen::generateHeader(const QString &text, QString *errorOut, int colOffset,
                               int screenH, const LibProbe::CodeAdapter &adapter) const
{
	/* 只可能是 0 或 2，但别让传进来的野值把生成的宏写坏 */
	if (colOffset < 0)
	{
		colOffset = 0;
	}
	if (colOffset > 8)
	{
		colOffset = 8;
	}

	/* 只认屏高 32 / 64，其它值按 64 处理：乱值会让显示函数把行算到屏外 */
	if (screenH != 32)
	{
		screenH = 64;
	}

	QChar *chars = nullptr;
	int n = 0;
	int skipped4 = 0;
	{
		QVector<QChar> list = glyphCharsOf(text, &skipped4);
		/* 按 UTF-8 编码排序，和 Python 脚本产出一致，方便 diff。
		 * 预览那边要的是输入顺序，所以排序放在这里，不放进 glyphCharsOf。 */
		std::sort(list.begin(), list.end(), [](const QChar &a, const QChar &b) {
			return QString(a).toUtf8() < QString(b).toUtf8();
		});
		n = list.size();
		if (n > 0)
		{
			chars = new QChar[n];
			for (int i = 0; i < n; ++i)
			{
				chars[i] = list.at(i);
			}
		}
	}

	if (n == 0)
	{
		delete[] chars;
		if (errorOut)
		{
			*errorOut = skipped4 > 0
			        ? QStringLiteral("只有 4 字节字符（emoji、扩展区生僻字），字库不收这些")
			        : QStringLiteral("没有可生成的汉字（ASCII 字符走原有字库，不需要取模）");
		}
		return QString();
	}

	const int S = m_size;
	const int per = bytesFor(S);          /* 一个汉字多少字节 */
	const int pages = S / 8;              /* 分几页 */
	const int stride = 3 + per;           /* 字库里一行多少字节 */
	const QString tbl = tableName(S);     /* 数组名，比如 OLED_CF24x24 */

	/* 勾了「取反」就把这件事写进文件里。不写的话文件头的「阴码」两个字
	 * 就是在说假话，用户过两个月回来看会以为自己哪里搞错了。 */
	const QString invNote = m_invert
	        ? QStringLiteral(" *      【本字库已取反】每一位都反了，屏上是亮底暗字（反白）。\n"
	                         " *      要改回普通白字黑底，把取反开关关掉重新导出即可。\n")
	        : QString();

	/* 跳过的 4 字节字符要说一声。不说的话，用户在输入框里明明打了 emoji，
	 * 导出后既没有报错也不在字库里，只会以为工具漏字了。 */
	const QString skipNote = (skipped4 > 0)
	        ? QStringLiteral(" *  （另有 %1 个 4 字节字符未收录：emoji、扩展区生僻字。\n"
	                         " *    显示函数遇到它们会直接跳过，不占 Flash。）\n")
	                  .arg(skipped4)
	        : QString();

	/* 出处标记放在最前面：字模离开这个工具之后，只有这行能说明它从哪来 */
	QString out = Watermark::text();
	out += QStringLiteral(
		"#ifndef __OLED_CFONT_H\n"
		"#define __OLED_CFONT_H\n"
		"\n"
		"/*==============================================================================\n"
		" *  汉字字模库  %1x%1 点阵，每个汉字 %2 字节\n"
		" *\n"
		" *  取模格式（与 OLED.c 的 OLED_ShowChinese 对应）：\n"
		" *      阴码 / 列行式 / 逆向(低位在前) / C51格式\n"
		"%6"
		" *      一个汉字 = %3 页 x %1 字节 = %2 字节（每页对应 8 行像素）\n"
		" *\n"
		" *  每行的排布：前 3 字节是该字的 UTF-8 编码，后面 %2 字节是点阵，\n"
		" *  所以字库里一行是 %4 字节。\n"
		" *\n"
		" *  这里全部写成十六进制字节，汉字只放在注释里——因为把字符串字面量\n"
		" *  (\"\\xNN\\xNN\\xNN\") 和字节混在同一个初始化列表里，从第二行起编译器会\n"
		" *  把字符串当成指针值，报 initializer element is not computable at load time。\n"
		" *\n"
		" *  注意：本文件必须是 UTF-8 编码保存（Keil 里 Edit -> Configuration ->\n"
		" *        Encoding 选 UTF-8）。\n"
		" *\n"
		" *  下面几个宏给驱动用，别把数字写死——换字号时驱动不用改：\n"
		" *      OLED_CFONT_W / H       点阵宽高\n"
		" *      OLED_CFONT_BYTES       一个汉字占多少字节\n"
		" *      OLED_CFONT_STRIDE      字库里一行多少字节（3 + 点阵）\n"
		" *============================================================================*/\n"
		"\n"
		"/* 只用到 uint8_t。自己 include 才不挑顺序——否则得靠调用方先把芯片头\n"
		"   文件放在前面，挪一下位置就报 unknown type name 'uint8_t'。 */\n"
		"#include <stdint.h>\n"
		"\n"
		"#define OLED_CFONT_W       %1\n"
		"#define OLED_CFONT_H       %1\n"
		"#define OLED_CFONT_BYTES   %2\n"
		"#define OLED_CFONT_STRIDE  %4\n"
		"\n"
		"/* 汉字码表：每行 %4 字节 = UTF-8 三字节编码 + %1x%1 点阵(%2 字节)。\n"
		"   每个字后面的注释里带一串 \"\\xNN\\xNN\\xNN\"，那是这个字的 C 字符串，\n"
		"   连引号一起复制进代码就能用，不必回来查编码：\n"
		"       OLED_CFont_ShowString(1, 1, <把注释里那串粘在这儿>); */\n"
		"const uint8_t %5[][%4] =\n"
		"{\n")
		.arg(S)
		.arg(per)
		.arg(pages)
		.arg(stride)
		.arg(tbl)
		.arg(invNote + skipNote);

	for (int i = 0; i < n; ++i)
	{
		const QVector<quint8> data = glyphBytes(chars[i]);

		/* 前 3 字节是 UTF-8 编码，后面 32 字节是点阵 */
		QVector<quint8> vals;
		const QByteArray utf8 = QString(chars[i]).toUtf8();
		for (int k = 0; k < utf8.size(); ++k)
		{
			vals.append(static_cast<quint8>(utf8.at(k)));
		}
		vals += data;

		QString hex;
		for (int k = 0; k < vals.size(); ++k)
		{
			if (k)
			{
				hex += QLatin1Char(',');
			}
			/* 注意：不能对 "0x%1" 整串调 toUpper()，那会把前缀里的 x 也变成 X，
			 * 数据会写成 0XE4 这种形式——C++ 能编译，但和字库约定的大小写不一致，
			 * 自检也会因为 startsWith("0x") 匹配不上而误报。
			 * 这里只把两位数值转成大写。 */
			const quint8 v = vals.at(k);
			const char digits[] = "0123456789ABCDEF";
			hex += QLatin1Char('0');
			hex += QLatin1Char('x');
			hex += QLatin1Char(digits[(v >> 4) & 0x0F]);
			hex += QLatin1Char(digits[v & 0x0F]);
		}

		/* 注释里除了字本身，再带上这个字的 C 字符串字面量。
		 * 目的很实在：要显示某几个字时，不用回工具里重新点一遍、也不用记
		 * 编码，直接从这儿连引号一起复制就能粘进代码。
		 * 放在注释里而不是单独一列，是因为它不能出现在初始化列表里——
		 * 字符串和字节混排会让编译器从第二行起把字符串当成指针值。 */
		out += QStringLiteral("\t/* %1  %2 */\n\t%3,\n")
		           .arg(QString(chars[i]), cLiteralOf(chars[i]), hex);
	}

	out += QStringLiteral(
		"};\n"
		"\n"
		"/* 汉字个数 */\n"
		"#define OLED_CFONT_TABLE   %1\n"
		"#define OLED_CFONT_NUM     (sizeof(OLED_CFONT_TABLE) / sizeof(OLED_CFONT_TABLE[0]))\n")
		.arg(tbl);

	/* 16x16 时代驱动里写的是 OLED_CF16x16_NUM，留着这行老工程照样能编译 */
	if (S == 16)
	{
		out += QStringLiteral(
			"#define OLED_CF16x16_NUM   OLED_CFONT_NUM\n");
	}

	/*
	 * 自带的显示函数。
	 * 这段文本和 Python 版 gen_oled_hz.py 里的 SHOW_FUNC_TAIL 必须逐字符一致，
	 * 所以里面不插任何尺寸数字——全从上面的宏来。改一边就要同步另一边。
	 */
	out += QStringLiteral(R"CF(
/* ============================================================================
 *  适配区：只改这里，下面的函数不用动
 *
 *  这里不假设你工程里已经写好 OLED_ShowChinese —— 很多工程没有，或者参数
 *  顺序、页数算法各不相同。本文件只跟驱动交换两个最底层的动作：
 *  写一条命令、写一个字节。这两个函数初始化时必然要用到，不可能不存在。
 *
 *  (A) 命令和数据分成两个函数（中景园新版、多数 HAL 例程）
 *      #define CF_WRITE_CMD(c)   OLED_WriteCommand(c)
 *      #define CF_WRITE_DATA(d)  OLED_WriteData(d)
 *
 *  (B) 一个函数加 cmd 标志位（正点原子、中景园旧版）
 *      #define CF_WRITE_CMD(c)   OLED_WR_Byte((c), 1)
 *      #define CF_WRITE_DATA(d)  OLED_WR_Byte((d), 0)
 *
 *  有些工程把这两个函数只写在 .c 里、没在 .h 声明，在自己文件里调用会报
 *  未声明。把这两行补进对应的 .h 即可：
 *      void OLED_WriteCommand(uint8_t cmd);
 *      void OLED_WriteData(uint8_t dat);
 *
 *  CF_SHOW_ASCII 用来显示英文字符。没有自己的字符显示函数就改成
 *      #define CF_SHOW_ASCII(line, col, ch)  ((void)0)
 *  这样英文字符被跳过，汉字位置仍然连续。
 * ========================================================================== */
)CF");

	/* 适配区这几个宏原来写死在这里。现在改成按工程源码算 —— 标准库工程里
	 * 没有 HAL_Delay，正点原子那套驱动也没有 OLED_WriteCommand，默认值填
	 * 错了就是「导出成功、编译报错」，而报错只指到「未定义的标识符」，
	 * 指不到取模工具头上。探测不出来时 adapter 里还是老默认值，行为不变。 */
	out += LibProbe::adapterNote(adapter);
	out += QStringLiteral("#define CF_WRITE_CMD(c)                 %1\n").arg(adapter.writeCmd);
	out += QStringLiteral("#define CF_WRITE_DATA(d)                %1\n").arg(adapter.writeData);
	out += QStringLiteral("#define CF_SHOW_ASCII(line, col, ch)    %1\n\n").arg(adapter.showAscii);

	out += QStringLiteral(R"CF(/* 屏是 SH1106（显存 132 列，可见区从第 2 列开始）时是 2，SSD1306 是 0。
   这个值是导出时按驱动源码自动判断后填进来的，一般不用改；
   万一判断错了（右边框缺两列、整幅左移 2 像素），把它改成 2 或 0 即可。 */
#define CF_COL_OFFSET                   %1

/* 屏幕像素宽度，只用来判断一行写满后不再往后写 */
#define CF_SCREEN_W                     128

/* 屏幕像素高度，只用来判断行号是否超出屏幕。
   0.91" 的 128x32 屏导出时这里是 32 —— 那种屏只有 4 页，
   按 64 算会让第 3 行文字往不存在的页地址里写。 */
#define CF_SCREEN_H                     %2

/* 一个汉字占几页 */
#define OLED_CFONT_PAGES                (OLED_CFONT_H / 8)

#if defined(__GNUC__) || defined(__CC_ARM)
#define CF_UNUSED  __attribute__((unused))
#else
#define CF_UNUSED
#endif

/* 定位到某一页、某一像素列。自己发命令，不调用驱动里的设光标函数，
   免得碰上各例程参数顺序不一致（有的页在前，有的列在前） */
#define CF_SET_POS(page, col)                                          \
    do {                                                               \
        CF_WRITE_CMD((uint8_t)(0xB0 | ((page) & 0x07)));                \
        CF_WRITE_CMD((uint8_t)(0x10 | (((col) >> 4) & 0x0F)));          \
        CF_WRITE_CMD((uint8_t)(0x00 | ((col) & 0x0F)));                 \
    } while (0)

/* 在码表里找这个字，返回点阵起始地址；找不到返回 0。
   utf8 指向该字 3 字节 UTF-8 编码的第一个字节。

   码表在生成时就按 UTF-8 编码升序排好了，所以这里用二分查。
   线性扫描在 70 个字时无所谓，但几百个字时刷一屏 4 行 32 个字就是上万次比较，
   帧率会肉眼可见地掉；二分 500 个字也只要 9 次比较。

   不用先转成 Unicode 码点：UTF-8 的字节序和码点序完全一致，
   逐字节比大小就等于比码点。 */
static CF_UNUSED const uint8_t *OLED_CFont_Find(const char *utf8)
{
    uint16_t lo = 0;
    uint16_t hi = (uint16_t)OLED_CFONT_NUM;
    const uint8_t b0 = (uint8_t)utf8[0];
    const uint8_t b1 = (uint8_t)utf8[1];
    const uint8_t b2 = (uint8_t)utf8[2];

    while (lo < hi)
    {
        const uint16_t mid = (uint16_t)(lo + (hi - lo) / 2);
        const uint8_t *row = OLED_CFONT_TABLE[mid];

        if (row[0] < b0)        { lo = (uint16_t)(mid + 1); }
        else if (row[0] > b0)   { hi = mid; }
        else if (row[1] < b1)   { lo = (uint16_t)(mid + 1); }
        else if (row[1] > b1)   { hi = mid; }
        else if (row[2] < b2)   { lo = (uint16_t)(mid + 1); }
        else if (row[2] > b2)   { hi = mid; }
        else                    { return &OLED_CFONT_TABLE[mid][3]; }
    }
    return 0;
}

/* 把一个字的点阵写到屏上。
   line   : 第几行文字（从 1 开始），一行占 OLED_CFONT_PAGES 页
   xPixel : 起始像素列（0 起），不是"第几个字" */
static CF_UNUSED void OLED_CFont_ShowGlyph(uint8_t line, uint16_t xPixel,
                                           const uint8_t *glyph)
{
    uint8_t  p;
    uint8_t  x;
    uint16_t base;

    if (glyph == 0 || (uint16_t)(xPixel + OLED_CFONT_W) > CF_SCREEN_W)
    {
        return;
    }

    if (line == 0)
    {
        return;                 /* line 从 1 起。传 0 会让 (line-1) 回绕成 65534，
                                   下面的页范围检查跟着失效，数据写到第 6/7 页上 */
    }

    base = (uint16_t)((line - 1) * OLED_CFONT_PAGES);
    if ((uint16_t)(base + OLED_CFONT_PAGES) > (uint16_t)(CF_SCREEN_H / 8))
    {
        return;                 /* 整行放不下了，不写。否则页地址会绕回第 0 页 */
    }

    for (p = 0; p < OLED_CFONT_PAGES; p++)
    {
        CF_SET_POS(base + p, (uint16_t)(xPixel + CF_COL_OFFSET));
        for (x = 0; x < OLED_CFONT_W; x++)
        {
            CF_WRITE_DATA(glyph[(uint16_t)p * OLED_CFONT_W + x]);
        }
    }
}

/* 显示一串 UTF-8 编码的文字（汉字 + 英文 + 数字混排）。
   line : 第几行文字（从 1 开始）
   col  : 从第几列开始，单位是 8 像素，和 OLED_ShowString 的 Column 一致。
          所以 16x16 的汉字占 2 列，24x24 占 3 列，32x32 占 4 列。

   注意：英文字符委托给 CF_SHOW_ASCII，它按 8x16 字符计算行位置。
   字号不是 16 时，英文和汉字的行高不一致，混排会错开，这种情况自己调 line。 */
static CF_UNUSED void OLED_CFont_ShowString(uint8_t line, uint8_t col,
                                            const char *utf8)
{
    const uint8_t *g;
    uint16_t xPixel;
    uint8_t  c;

    if (utf8 == 0)
    {
        return;
    }
    if (col < 1)
    {
        col = 1;
    }
    xPixel = (uint16_t)((col - 1) * 8);

    while (*utf8 != '\0')
    {
        c = (uint8_t)(*utf8);

        if (c < 0x80)                       /* ASCII */
        {
            if ((uint16_t)(xPixel + 8) <= CF_SCREEN_W)
            {
                CF_SHOW_ASCII(line, (uint8_t)(xPixel / 8 + 1), (char)c);
            }
            xPixel = (uint16_t)(xPixel + 8);
            utf8++;
        }
        else if ((c & 0xF0) == 0xE0)        /* 三字节：本字库收录的汉字 */
        {
            g = OLED_CFont_Find(utf8);
            if (g != 0)
            {
                OLED_CFont_ShowGlyph(line, xPixel, g);
            }
            xPixel = (uint16_t)(xPixel + OLED_CFONT_W);
            utf8 += 3;
        }
        else if ((c & 0xF8) == 0xF0)        /* 四字节：扩展区汉字，本库不收 */
        {
            xPixel = (uint16_t)(xPixel + OLED_CFONT_W * 2);
            utf8 += 4;
        }
        else if ((c & 0xE0) == 0xC0)        /* 二字节：拉丁扩展等，本库不收 */
        {
            xPixel = (uint16_t)(xPixel + 8);
            utf8 += 2;
        }
        else                                /* 非法字节，跳过一个继续 */
        {
            utf8++;
        }
    }
}
)CF").arg(colOffset).arg(screenH);

	out += QStringLiteral(
		"\n"
		"#endif\n");

	delete[] chars;
	return out;
}

/*
 * 导入：把字库文件里的汉字读回来，拼成字符串，供用户继续编辑。
 * 只认字模数据行，也就是以 0x 开头的那种。
 *
 * 注意：字库每一行是 35 个字节 —— 前 3 个是该字的 UTF-8 编码，后面 32 个是点阵，
 * 所以取这一行的前 3 个字节就能还原出这个汉字。
 *
 * 历史原因：早期版本写成 {"\xNN\xNN\xNN",0x..}，那种写法编译器过不去
 * （字符串和字节混在一个初始化列表里），已经全部改成纯字节表。
 * 这里顺带保留对旧格式的兼容，免得遇到老文件读不出来。
 */
QString FontGen::parseHeader(const QString &headerContent)
{
	QString result;
	QSet<QChar> seen;
	const QStringList lines = headerContent.split(QLatin1Char('\n'));

	for (int i = 0; i < lines.size(); ++i)
	{
		const QString line = lines.at(i).trimmed();

		QByteArray code;

		if (line.startsWith(QLatin1String("0x")))
		{
			/* 新格式：纯十六进制字节表，取前 3 个字节 */
			static const QRegularExpression re(QStringLiteral("0x([0-9A-Fa-f]{2})"));
			QRegularExpressionMatchIterator it = re.globalMatch(line);
			while (it.hasNext() && code.size() < 3)
			{
				code.append(static_cast<char>(it.next().captured(1).toInt(nullptr, 16)));
			}
		}
		else if (line.startsWith(QLatin1String("{\"\\x")))
		{
			/* 旧格式：{"\xE4\xB8\x8A",0x..} */
			for (int k = 0; k + 3 < line.size() && code.size() < 3; ++k)
			{
				if (line.at(k) == QLatin1Char('\\') && line.at(k + 1) == QLatin1Char('x'))
				{
					bool ok = false;
					const int v = line.mid(k + 2, 2).toInt(&ok, 16);
					if (ok)
					{
						code.append(static_cast<char>(v));
					}
				}
			}
		}
		else
		{
			continue;
		}

		if (code.size() != 3)
		{
			continue;
		}

		const QString ch = QString::fromUtf8(code);
		if (ch.isEmpty())
		{
			continue;
		}
		if (!seen.contains(ch.at(0)))
		{
			seen.insert(ch.at(0));
			result += ch.at(0);
		}
	}
	return result;
}

QString FontGen::writeTextFile(const QString &path, const QString &text)
{
	if (path.isEmpty())
	{
		return QStringLiteral("没有指定文件名");
	}

	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
	{
		return file.errorString();
	}

	QTextStream ts(&file);
	ts.setEncoding(QStringConverter::Utf8);        /* Keil 读 UTF-8 才不会花 */
	ts << text;
	ts.flush();

	/* close() 才是落盘那一步：前面 write 进的是缓冲，磁盘满 / 网络盘断开
	 * 都在这时候才报。imagegen 的 writeHeaderFile 里踩过同一个坑。 */
	file.close();
	if (file.error() != QFileDevice::NoError)
	{
		return file.errorString();
	}
	return QString();
}
