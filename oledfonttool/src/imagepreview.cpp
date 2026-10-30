#include "imagepreview.h"

#include <QFont>
#include <QFontMetrics>
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

    /* 放大到 4 倍以上时画像素网格。
     *
     * 这个预览是整数倍最近邻放大，64×64 的图在几百像素的面板里会放大 7 倍，
     * 抖动出来的 4×4 周期就被拉成 28×28 的大方块 —— 看着像花屏，
     * 实际屏上（0.96" 屏像素间距约 0.15mm）根本看不出来。
     * 画上格子，一眼就能看出"这是一格一格的像素"，而不是一片噪点。
     *
     * 颜色取中性灰半透明：白底上显浅灰、黑底上显深灰，两边都看得见，
     * 又不会把点阵本身的疏密盖住。放大倍数越小格子越密，透明度跟着降低，
     * 免得整片糊成灰雾。 */
    if (scale >= 4)
    {
        /* 4 倍时格子只有 4 像素宽，线再深就糊成灰雾；6 倍以上给足对比度，
         * 白底上约 230、黑底上约 30，两种底色都看得出来。 */
        const int alpha = (scale >= 6) ? 55 : 38;
        p.setPen(QPen(QColor(140, 140, 140, alpha), 1));
        for (int i = 1; i < img.width(); ++i)
        {
            const int x = target.left() + i * scale;
            p.drawLine(x, target.top(), x, target.bottom() - 1);
        }
        for (int j = 1; j < img.height(); ++j)
        {
            const int y = target.top() + j * scale;
            p.drawLine(target.left(), y, target.right() - 1, y);
        }
    }

    /* 画一圈边框，方便看清屏幕边界 */
    p.setPen(QColor(0x3A, 0x42, 0x4E));
    p.drawRect(target.adjusted(-1, -1, 1, 1));

    /* 标出真实点阵尺寸和当前放大倍数。不写这一行，用户看到的是"一张 448×448
     * 的图"，很难意识到那其实是 64×64 放大 7 倍的结果，于是把放大后的方块
     * 当成实际显示效果。
     *
     * 位置放在图片上方／下方的空白里，不放图片内部 —— 压在点阵上会盖掉一块
     * 图案，128×64 只放大 3 倍、图又窄的时候尤其明显。 */
    QFont tagFont = font();
    tagFont.setPixelSize(12);
    /* 显式给一个中文字体：默认字体在没有字体配置的环境里解析出来可能根本
     * 不含汉字，画出来是一排方框。Windows 上雅黑必然存在，缺了 Qt 会自己回退。 */
    tagFont.setFamily(QStringLiteral("Microsoft YaHei"));
    p.setFont(tagFont);

    const QString tag = (scale > 1)
            ? QStringLiteral("%1×%2 · 放大 %3×")
                  .arg(img.width()).arg(img.height()).arg(scale)
            : QStringLiteral("%1×%2").arg(img.width()).arg(img.height());
    const QFontMetrics fm(tagFont);
    const int tagW = fm.horizontalAdvance(tag) + 12;
    const int tagH = fm.height() + 4;

    int tagY;
    if (target.top() - tagH - 6 >= 0)
    {
        tagY = target.top() - tagH - 6;
    }
    else if (target.bottom() + 6 + tagH <= height())
    {
        tagY = target.bottom() + 6;
    }
    else
    {
        tagY = target.top() + 2;        /* 上下都没地方了，只能压在图里 */
    }
    const int tagX = qBound(4, target.left(), qMax(4, width() - tagW - 4));

    const QRect tagRect(tagX, tagY, tagW, tagH);
    p.fillRect(tagRect, QColor(0, 0, 0, 170));
    p.setPen(QColor(0xC8, 0xD0, 0xDA));
    p.drawText(tagRect, Qt::AlignCenter, tag);

    if (m_frames.size() > 1)
    {
        p.setPen(QColor(0xC8, 0xD0, 0xDA));
        const QString info = QStringLiteral("%1 / %2").arg(m_current + 1).arg(m_frames.size());
        p.drawText(rect().adjusted(0, 0, -10, -6), Qt::AlignRight | Qt::AlignBottom, info);
    }
}
