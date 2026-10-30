#ifndef IMAGEGEN_H
#define IMAGEGEN_H

#include <QImage>
#include <QVector>
#include <QString>
#include <QtGlobal>

/*
 * 把图片 / 动画帧转成 OLED 显存格式的点阵数据。
 *
 * 输出格式和汉字字库完全一致：
 *     阴码 / 列行式 / 逆向(低位在前)
 * 也就是：
 *     每字节 = 某一列的连续 8 个像素，bit0 在最上面
 *     按页排列：先第 0 页的 0..W-1 列，再第 1 页……
 *     一帧字节数 = (W/8) * H
 *
 * 这样 MCU 侧不需要任何转换，直接把字节按页写给屏就行。
 */
class ImageGen
{
public:
    /* 单色化方式 */
    enum Dither
    {
        NoDither,       /* 简单阈值：线条图够用，照片会糊成黑块 */
        Bayer           /* 4x4 有序抖动：用疏密模拟灰度，照片也看得过去 */
    };

    struct Options
    {
        int    width     = 128;
        int    height    = 64;
        bool   stretch   = false;   /* false = 等比缩放 + 留白；true = 拉伸填满 */
        int    threshold = 128;     /* 0~255，越小越亮/越粗 */
        Dither dither    = Bayer;
        int    colOffset = 0;       /* SH1106（132 列）填 2，SSD1306 填 0 */
    };

    /* 一帧多少字节 */
    static int bytesPerFrame(int w, int h) { return (w / 8) * h; }
    static int flashBytes(int w, int h, int frames) { return bytesPerFrame(w, h) * frames; }

    /*
     * 把任意尺寸的图片变成目标尺寸的灰度图。
     * stretch=false 时等比缩放后居中留黑边，避免变形。
     */
    static QImage fitToTarget(const QImage &src, int w, int h, bool stretch);

    /* 灰度图 → 1bit 黑白图（可选抖动） */
    static QImage toMono(const QImage &gray, int threshold, Dither d);

    /* 1bit 图 → 显存字节（列行式 / 低位在前 / 阴码） */
    static QVector<quint8> packToDisplay(const QImage &mono);

    /* 反向：显存字节 → 1bit 图，用于"所见即所得"的预览 */
    static QImage unpackFromDisplay(const QVector<quint8> &bytes, int w, int h);

    /* 一步到位：任意图 → 显存字节 */
    static QVector<quint8> convert(const QImage &src, const Options &opt);

    /*
     * 生成 C 头文件的内容。
     * frames 里每个元素是一帧的显存字节（长度必须都等于 bytesPerFrame）。
     */
    static QString renderHeader(const QVector<QVector<quint8>> &frames,
                                const Options &opt,
                                int delayMs,
                                const QString &sourceName);
};

#endif // IMAGEGEN_H
