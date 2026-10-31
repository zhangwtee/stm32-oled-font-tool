/*==============================================================================
 *  imagegen.h  ——  图片 / 动画取模（接口）
 *
 *  作者   zhangwtee
 *  GitHub https://github.com/zhangwtee
 *  Gitee  https://gitee.com/z_w_t
 *==============================================================================*/
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
 *     一帧字节数 = W * (H / 8)
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
        bool   invert    = false;   /* true = 黑白对调（反白）：亮的地方变灭，灭的变亮 */
        int    colOffset = 0;       /* SH1106（132 列）填 2，SSD1306 填 0 */
    };

    /*
     * 一帧多少字节。
     *
     * 必须和 packToDisplay 的写入方式完全一致：`out[page * w + x]`，
     * 页数 = h / 8，所以是 w * (h / 8)。
     * 原来写成 (w / 8) * h —— 那其实是"横向像素字节数 × 高"，
     * 只在 w 是 8 的倍数时才和写入布局相等。界面只给 128 宽，所以一直没露头，
     * 但两条公式对着写的代码只要哪天放开宽度就会差出几十字节：
     * 数组开小了、头文件里的 FRAME_SIZE 也跟着错。
     */
    static int bytesPerFrame(int w, int h) { return w * (h / 8); }
    static int flashBytes(int w, int h, int frames) { return bytesPerFrame(w, h) * frames; }

    /* 一份素材在 Flash 里占多少。界面那边每份素材各存各的尺寸和帧数，
     * 这里只取算总量用得上的三个数，不牵扯界面结构。 */
    struct Footprint
    {
        int width  = 0;
        int height = 0;
        int frames = 0;   /* 实际会导出的帧数（抽帧之后），不是 GIF 原始帧数 */
    };

    struct Usage
    {
        int    frames = 0;
        qint64 bytes  = 0;
    };

    /*
     * 列表里全部素材加起来占多少。
     *
     * 必须逐份算完再相加：列表允许混着装 —— 128×64 的动画和 16×16 的图标
     * 可以同时在里头，每帧字节数差 32 倍。拿最后一份的每帧字节乘总帧数，
     * 得到的数看着像那么回事，其实是假的。
     */
    static Usage totalUsage(const QVector<Footprint> &items);

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
     *
     * ident 给多份素材共存时用：传了就把 ANIM_* / Anim_* 这类标识符
     * 换成以它打头的名字（logo → LOGO_WIDTH / Logo_ShowFrame），
     * 几份头文件就能同时 include 进一个工程，不会撞宏定义。
     * 不传（空）时输出和以前逐字节相同。
     */
    static QString renderHeader(const QVector<QVector<quint8>> &frames,
                                const Options &opt,
                                int delayMs,
                                const QString &sourceName,
                                const QString &ident = QString());

    /*
     * 渲染并写盘（UTF-8）。成功返回空字符串，失败返回原因。
     * 单份导出和"每份一个 .h"都走这里，省得两处各写一遍打开/写/关。
     */
    static QString writeHeaderFile(const QString &path,
                                   const QVector<QVector<quint8>> &frames,
                                   const Options &opt,
                                   int delayMs,
                                   const QString &sourceName,
                                   const QString &ident = QString());

    /*
     * 一份素材，给「多份打包进一个 .h」用。
     *
     * 比界面那边的 ImageAsset 少一半字段：列表名、原始帧、抽帧步长都是
     * 界面的东西，导出时只要成品（packed）和它自己的取模参数。
     */
    struct Asset
    {
        QString                  ident;      /* 素材名 → 宏/函数前缀 */
        QString                  sourceName; /* 导入时的文件名，写在点阵上方 */
        QVector<QVector<quint8>> frames;     /* 显存字节 */
        Options                  opt;
        int                      delayMs = 100;
    };

    /*
     * 把多份素材写进**同一个**头文件，每份各自独立 —— 不合成动画。
     *
     * 每份都有自己的 WIDTH / COUNT / Xxx_Frames / Xxx_ShowFrame()，
     * 名字按素材名打头，互不影响；只有「适配区」（写命令 / 写数据 / 延时）
     * 是全文件共用的一份，改一次就行。
     *
     * 每份的点阵数组正上方会注明导入时的文件名，隔了几天回来能认出来
     * 哪个数组是哪张图。
     *
     * title（一般就是文件名）用来生成 include guard：assets.h → __ASSETS_H，
     * 两份打包文件同时 include 也不会互相吃掉。为空时用 __OLED_ASSETS_H。
     */
    static QString renderMultiHeader(const QVector<Asset> &assets,
                                     const QString &title = QString());

    /* 同上的写盘版本 */
    static QString writeMultiHeaderFile(const QString &path,
                                        const QVector<Asset> &assets,
                                        const QString &title = QString());

    /*
     * 把任意字符串收拾成能当 C 标识符的名字：
     * 只留字母数字下划线，其余换成下划线，数字开头补前缀，空的话给个兜底。
     * "我的图标.png" 这类中文名过不了 C 编译器，必须有这一步。
     */
    static QString sanitizeIdentifier(const QString &name);

    /* 在 used 里挑一个没被占用的名字：logo → logo_2 → logo_3 …
     * 比较忽略大小写，因为宏前缀会整体转大写，"Logo" 和 "logo" 一样撞车。 */
    static QString uniqueName(const QStringList &used, const QString &proposed);

    /*
     * 一批图片文件的读取结果。
     *
     * 单张图和"一次多选一堆图"走的是同一条路：前者就是一个元素的列表。
     * 失败的原因要能说清楚，所以 failed 带文件名而不是只给个 bool。
     */
    struct FrameBatch
    {
        QVector<QImage> frames;        /* 读到的帧，已按播放顺序排好 */
        /* 传进来的文件，已按播放顺序排好。注意它包含"一帧都没贡献"的：
         * 读失败的（见 failed）、撞上限没读的（见 skippedFiles）。
         * 想知道哪几个文件真的出了帧，看 frames 和 failed 对不上就够了。 */
        QStringList     files;
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
