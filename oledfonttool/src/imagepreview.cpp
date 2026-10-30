#include "imagepreview.h"

#include <QPainter>
#include <QPaintEvent>

ImagePreview::ImagePreview(QWidget *parent)
    : QWidget(parent)
{
    m_emptyText = QStringLiteral("选择一张图片或 GIF 后，这里显示实际会烧进 Flash 的点阵");
    setMinimumSize(240, 160);
    setAutoFillBackground(false);
}

void ImagePreview::setFrames(const QVector<QImage> &monoFrames, int delayMs)
{
    m_frames = monoFrames;
    m_current = 0;
    m_delayMs = (delayMs > 0) ? delayMs : 100;

    if (!m_frames.isEmpty())
    {
        m_cell = m_frames.first().size();
        /* 高度固定后，让布局知道我们希望多大 */
        const int wantH = m_cell.height() * 6;
        setMinimumHeight(qMax(160, wantH));
    }
    else
    {
        m_cell = QSize();
    }
    update();
}

void ImagePreview::clearFrames()
{
    m_frames.clear();
    m_current = 0;
    m_cell = QSize();
    setMinimumHeight(160);
    update();
}

void ImagePreview::showNextFrame()
{
    if (m_frames.size() < 2)
    {
        return;
    }
    m_current = (m_current + 1) % m_frames.size();
    update();
}

void ImagePreview::setCurrentFrame(int index)
{
    if (m_frames.isEmpty())
    {
        return;
    }
    m_current = qBound(0, index, m_frames.size() - 1);
    update();
}

QSize ImagePreview::sizeHint() const
{
    if (m_cell.isEmpty())
    {
        return QSize(640, 240);
    }
    return QSize(m_cell.width() * 6, m_cell.height() * 6);
}

QSize ImagePreview::minimumSizeHint() const
{
    return QSize(240, 120);
}

void ImagePreview::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter p(this);
    p.fillRect(rect(), QColor(0x1E, 0x22, 0x28));

    if (m_frames.isEmpty() || m_frames.at(m_current).isNull())
    {
        p.setPen(QColor(0x9A, 0xA3, 0xAF));
        p.drawText(rect().adjusted(16, 16, -16, -16),
                   Qt::AlignCenter | Qt::TextWordWrap, m_emptyText);
        return;
    }

    const QImage &img = m_frames.at(m_current);

    /* 整数倍缩放：每个显示像素对应整数个源像素，方块边缘才干净 */
    const int availW = width() - 24;
    const int availH = height() - 24;
    int scale = qMin(availW / qMax(1, img.width()), availH / qMax(1, img.height()));
    if (scale < 1)
    {
        scale = 1;
    }

    const int dw = img.width() * scale;
    const int dh = img.height() * scale;
    const QRect target((width() - dw) / 2, (height() - dh) / 2, dw, dh);

    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.drawImage(target, img);

    /* 画一圈边框，方便看清屏幕边界 */
    p.setPen(QColor(0x3A, 0x42, 0x4E));
    p.drawRect(target.adjusted(-1, -1, 1, 1));

    if (m_frames.size() > 1)
    {
        p.setPen(QColor(0xC8, 0xD0, 0xDA));
        const QString info = QStringLiteral("%1 / %2").arg(m_current + 1).arg(m_frames.size());
        p.drawText(rect().adjusted(0, 0, -10, -6), Qt::AlignRight | Qt::AlignBottom, info);
    }
}
