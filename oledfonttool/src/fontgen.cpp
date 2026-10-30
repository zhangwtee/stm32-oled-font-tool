#include "fontgen.h"

#include <QFile>
#include <QImage>
#include <QPainter>
#include <QRegularExpression>
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
}

QString FontGen::tableName(int size)
{
	return QStringLiteral("OLED_CF%1x%2").arg(size).arg(size);
}

void FontGen::setFont(const QFont &font)
{
	m_font = font;
	m_font.setPixelSize(m_size);
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

/*
 * 渲染：N×N 画布上画字 -> 取墨迹边界 -> 整体平移居中 -> 按阈值二值化
 * 平移这一步不能省：宋体的字形在字身框里天然偏左偏上，不居中的话点阵会贴边
 */
QVector<QVector<bool>> FontGen::renderBits(QChar ch) const
{
	const int S = m_size;
	QImage img(S, S, QImage::Format_Grayscale8);
	img.fill(0);

	QPainter p(&img);
	p.setRenderHint(QPainter::TextAntialiasing, true);
	p.setFont(m_font);
	p.setPen(Qt::white);
	p.drawText(QRect(0, 0, S, S), Qt::AlignLeft | Qt::AlignTop, QString(ch));
	p.end();

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
	QPainter p2(&moved);
	p2.drawImage(dx, dy, img);              /* 平移，超出部分自动裁掉 */
	p2.end();

	QVector<QVector<bool>> bits(S, QVector<bool>(S, false));
	for (int y = 0; y < S; ++y)
	{
		const uchar *line = moved.constScanLine(y);
		for (int x = 0; x < S; ++x)
		{
			bits[y][x] = (line[x] >= m_threshold);
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

int FontGen::glyphCount(const QString &text)
{
	QSet<QChar> set;
	for (int i = 0; i < text.size(); ++i)
	{
		if (text.at(i).unicode() > 0x7F)         /* ASCII 走原字库，不进这个表 */
		{
			set.insert(text.at(i));
		}
	}
	return set.size();
}

qint64 FontGen::flashBytes(int count, int size, bool withAsciiFont)
{
	/* 汉字表 count * 每字字节；ASCII 8x16 字库固定 95*16 字节 */
	return static_cast<qint64>(count) * bytesFor(size) + (withAsciiFont ? 95 * 16 : 0);
}

QString FontGen::generateHeader(const QString &text, QString *errorOut, int colOffset,
                               int screenH) const
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
	{
		QSet<QChar> set;
		QVector<QChar> list;
		for (int i = 0; i < text.size(); ++i)
		{
			const QChar c = text.at(i);
			if (c.unicode() > 0x7F && !set.contains(c))
			{
				set.insert(c);
				list.append(c);
			}
		}
		/* 按 UTF-8 编码排序，和 Python 脚本产出一致，方便 diff */
		std::sort(list.begin(), list.end(), [](const QChar &a, const QChar &b) {
			return QString(a).toUtf8() < QString(b).toUtf8();
		});
		n = list.size();
		chars = new QChar[n];
		for (int i = 0; i < n; ++i)
		{
			chars[i] = list.at(i);
		}
	}

	if (n == 0)
	{
		delete[] chars;
		if (errorOut)
		{
			*errorOut = QStringLiteral("没有可生成的汉字（ASCII 字符走原有字库，不需要取模）");
		}
		return QString();
	}

	const int S = m_size;
	const int per = bytesFor(S);          /* 一个汉字多少字节 */
	const int pages = S / 8;              /* 分几页 */
	const int stride = 3 + per;           /* 字库里一行多少字节 */
	const QString tbl = tableName(S);     /* 数组名，比如 OLED_CF24x24 */

	QString out;
	out += QStringLiteral(
		"#ifndef __OLED_CFONT_H\n"
		"#define __OLED_CFONT_H\n"
		"\n"
		"/*==============================================================================\n"
		" *  汉字字模库  %1x%1 点阵，每个汉字 %2 字节\n"
		" *\n"
		" *  取模格式（与 OLED.c 的 OLED_ShowChinese 对应）：\n"
		" *      阴码 / 列行式 / 逆向(低位在前) / C51格式\n"
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
		"#define OLED_CFONT_W       %1\n"
		"#define OLED_CFONT_H       %1\n"
		"#define OLED_CFONT_BYTES   %2\n"
		"#define OLED_CFONT_STRIDE  %4\n"
		"\n"
		"/* 汉字码表：每行 %4 字节 = UTF-8 三字节编码 + %1x%1 点阵(%2 字节) */\n"
		"const uint8_t %5[][%4] =\n"
		"{\n")
		.arg(S)
		.arg(per)
		.arg(pages)
		.arg(stride)
		.arg(tbl);

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

		out += QStringLiteral("\t/* %1 */\n\t%2,\n").arg(QString(chars[i]), hex);
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
#define CF_WRITE_CMD(c)                 OLED_WriteCommand(c)
#define CF_WRITE_DATA(d)                OLED_WriteData(d)
#define CF_SHOW_ASCII(line, col, ch)    OLED_ShowChar((line), (col), (ch))

/* 屏是 SH1106（显存 132 列，可见区从第 2 列开始）时是 2，SSD1306 是 0。
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
