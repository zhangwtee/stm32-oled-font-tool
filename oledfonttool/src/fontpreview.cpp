#include "fontpreview.h"

#include <QPainter>
#include <QSet>
#include <algorithm>

FontPreview::FontPreview(QWidget *parent)
	: QWidget(parent), m_gen(nullptr), m_gap(18)
{
	setMinimumSize(360, 260);
	QPalette pal = palette();
	pal.setColor(QPalette::Window, QColor(0xF7, 0xF8, 0xFA));
	setAutoFillBackground(true);
	setPalette(pal);
}

void FontPreview::setGenerator(FontGen *gen)
{
	m_gen = gen;
	update();
}

void FontPreview::setText(const QString &text)
{
	QSet<QChar> seen;
	m_chars.clear();
	for (int i = 0; i < text.size(); ++i)
	{
		const QChar c = text.at(i);
		if (c.unicode() > 0x7F && !seen.contains(c))
		{
			seen.insert(c);
			m_chars.append(c);
		}
	}
	updateGeometry();
	update();
}

/* 指定宽度下能排几列 */
static int columnsFor(int width, int gap)
{
	const int cell = 64 + gap;                 /* 缩略图边长 + 间距 */
	return std::max(1, (width - gap) / std::max(1, cell));
}

int FontPreview::columns() const
{
	return columnsFor(width(), m_gap);
}

int FontPreview::cellSize() const
{
	/* 缩略图边长。小字固定 64，大字跟着放大，免得 32x32 撑破格子 */
	const int glyph = m_gen ? m_gen->size() : FontGen::DEFAULT_SIZE;
	return std::max(64, glyph * 2);
}

/* 放进 QScrollArea 后必须实现这个，否则滚动区域算不出内容总高度，
 * 要么不出现滚动条，要么把所有格子压扁。 */
int FontPreview::heightForWidth(int w) const
{
	const int cols = columnsFor(w, m_gap);
	const int count = static_cast<int>(m_chars.size());
	const int rows = std::max(1, (count + cols - 1) / cols);
	return m_gap + rows * (cellSize() + m_gap + 20);
}

QSize FontPreview::sizeHint() const
{
	const int w = width() > 0 ? width() : 640;
	return QSize(w, heightForWidth(w));
}

QSize FontPreview::minimumSizeHint() const
{
	return QSize(200, 200);
}

void FontPreview::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	update();
}

void FontPreview::paintEvent(QPaintEvent *event)
{
	Q_UNUSED(event);

	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, false);

	if (!m_gen || m_chars.isEmpty())
	{
		p.setPen(QColor(0x9A, 0xA0, 0xA6));
		p.drawText(rect(), Qt::AlignCenter,
		           QStringLiteral("在这里输入要显示的汉字\n（ASCII 字母数字走原有字库，不用取模）"));
		return;
	}

	const int cols = columnsFor(width(), m_gap);
	const int cell = cellSize();
	const int S = m_gen->size();
	const int grid = std::max(2, cell / S);   /* 一个像素放大成几格 */
	const int draw = grid * S;

	for (int i = 0; i < m_chars.size(); ++i)
	{
		const int col = i % cols;
		const int row = i / cols;
		const int ox = m_gap + col * (cell + m_gap);
		const int oy = m_gap + row * (cell + m_gap + 20);

		if (oy > height())
		{
			break;      /* 超出可视区域就不画了 */
		}

		const QVector<QVector<bool>> bits = m_gen->glyphBits(m_chars.at(i));

		/* 点阵底：黑底白点，和 OLED 实际观感一致 */
		const QRect box(ox, oy, draw, draw);
		p.fillRect(box, Qt::black);

		p.setPen(QColor(0x2A, 0x2E, 0x33));
		for (int k = 0; k <= S; ++k)
		{
			p.drawLine(box.left() + k * grid, box.top(), box.left() + k * grid, box.bottom());
			p.drawLine(box.left(), box.top() + k * grid, box.right(), box.top() + k * grid);
		}

		p.setPen(Qt::NoPen);
		p.setBrush(Qt::white);
		for (int y = 0; y < S; ++y)
		{
			for (int x = 0; x < S; ++x)
			{
				if (bits[y][x])
				{
					p.drawRect(ox + x * grid, oy + y * grid, grid, grid);
				}
			}
		}

		/* 字下面标编码，方便和字库对照 */
		p.setPen(QColor(0x5A, 0x60, 0x66));
		const QByteArray utf8 = QString(m_chars.at(i)).toUtf8();
		QString code;
		for (int k = 0; k < utf8.size(); ++k)
		{
			code += QStringLiteral("%1").arg(static_cast<quint8>(utf8.at(k)), 2, 16, QChar('0')).toUpper();
		}
		p.drawText(QRect(ox, oy + draw + 2, draw + m_gap, 18),
		           Qt::AlignLeft | Qt::AlignVCenter, code);
	}
}
