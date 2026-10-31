/*==============================================================================
 *  imagepreview.h  ——  动画帧预览控件（接口）
 *
 *  作者   zhangwtee
 *  GitHub https://github.com/zhangwtee
 *  Gitee  https://gitee.com/z_w_t
 *==============================================================================*/
#ifndef IMAGEPREVIEW_H
#define IMAGEPREVIEW_H

#include <QWidget>
#include <QVector>
#include <QImage>
#include <QString>

/*
 * 动画帧预览。
 *
 * 显示的是已经"单色化并按显存格式打包后再还原"的图，
 * 也就是真正会被烧进 Flash 的那份数据 —— 所见即所得。
 *
 * 用整数倍放大 + 最近邻插值，保证每个像素是方正的方块，
 * 不会被平滑成一团灰。
 */
class ImagePreview : public QWidget
{
    Q_OBJECT

public:
    explicit ImagePreview(QWidget *parent = nullptr);

    /* monoFrames: 每帧都是目标尺寸的 1bit 图 */
    void setFrames(const QVector<QImage> &monoFrames, int delayMs);
    void clearFrames();

    int  frameCount() const { return m_frames.size(); }
    int  currentFrame() const { return m_current; }

public slots:
    void showNextFrame();
    void setCurrentFrame(int index);

protected:
    void paintEvent(QPaintEvent *event) override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

private:
    QVector<QImage> m_frames;
    int             m_current = 0;
    int             m_delayMs = 100;
    QString         m_emptyText;
    QSize           m_cell;      /* 单帧的原始像素尺寸 */
};

#endif // IMAGEPREVIEW_H
