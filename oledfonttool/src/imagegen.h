#ifndef IMAGEGEN_H
#define IMAGEGEN_H

#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>
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

    /*
     * 一批图片文件的读取结果。
     *
     * 单张图和"一次多选一堆图"走的是同一条路：前者就是一个元素的列表。
     * 失败的原因要能说清楚，所以 failed 带文件名而不是只给个 bool。
     */
    struct FrameBatch
    {
        QVector<QImage> frames;        /* 读到的帧，已按播放顺序排好 */
        QStringList     files;         /* 实际参与的文件，已按播放顺序排好 */
        int             delayMs = 0;   /* 0 = 没有任何文件提供延时（静态图） */
        int             totalFrames = 0; /* 这些文件里本来一共有多少帧 */
        int             limitFrames = 0; /* 本批次用的帧数上限 */
        int             skippedFiles = 0; /* 因为撞上限、整个没被读的文件数 */
        bool            truncated = false;      /* 有文件被截掉了 */
        bool            cappedByCount = false;  /* 原因之一是撞帧数上限 */
        bool            cappedByMemory = false; /* 原因之一是内存预算不够 */
        QStringList     failed;        /* 读不出图像的文件名 */

        bool isEmpty() const { return frames.isEmpty(); }
    };

    /*
     * 按文件名顺序读一批图片，GIF 展开它的所有帧再接下一个文件。
     *
     * maxFrames 和 budgetBytes 是**整个批次**共用的上限：
     * 多选 20 个 300 帧的 GIF 应该是 600 帧封顶，而不是每个文件各 600 帧。
     * 预算按 宽×高×4 估算每帧占用，超了就只读前面一段并置 truncated，
     * 而不是让程序吃到几个 G 内存再被系统杀掉。
     */
    static FrameBatch loadFrames(const QStringList &paths,
                                 int maxFrames = 600,
                                 qint64 budgetBytes = 400LL * 1024 * 1024);
};

#endif // IMAGEGEN_H
