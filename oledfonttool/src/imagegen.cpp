#include "imagegen.h"

#include <QCollator>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QStringList>
#include <QStringConverter>
#include <QTextStream>

#include <algorithm>

/* 4x4 有序抖动矩阵（Bayer）。值 0..15，用来把灰度转成"疏密不同的点" */
static const int kBay4[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 }
};

/*
 * 缩放并居中。
 *
 * GIF 里可能有透明区域，直接用 convertToFormat 转灰度可能会把透明当成白，
 * 所以先画到黑底画布上再转灰度 —— 透明处就是黑，符合 OLED 的观感。
 */
QImage ImageGen::fitToTarget(const QImage &src, int w, int h, bool stretch)
{
    if (w <= 0 || h <= 0)
    {
        return QImage();
    }
    if (src.isNull())
    {
        QImage empty(w, h, QImage::Format_Grayscale8);
        empty.fill(0);
        return empty;
    }

    const QImage scaled = src.scaled(
        w, h,
        stretch ? Qt::IgnoreAspectRatio : Qt::KeepAspectRatio,
        Qt::SmoothTransformation);

    QImage canvas(w, h, QImage::Format_RGB32);
    canvas.fill(Qt::black);
    {
        QPainter p(&canvas);
        p.drawImage((w - scaled.width()) / 2, (h - scaled.height()) / 2, scaled);
        p.end();
    }
    return canvas.convertToFormat(QImage::Format_Grayscale8);
}

/*
 * 灰度 → 1bit。
 *
 * threshold 作用在"亮度整体平移"上（和文字模式一致：值越小越亮/越粗）；
 * 抖动则在平移后的值上做有序抖动，模拟中间灰。
 */
QImage ImageGen::toMono(const QImage &gray, int threshold, Dither d)
{
    const int w = gray.width();
    const int h = gray.height();

    QImage mono(w, h, QImage::Format_Mono);
    mono.setColorCount(2);
    mono.setColor(0, qRgb(0, 0, 0));
    mono.setColor(1, qRgb(255, 255, 255));
    mono.fill(0);

    const int shift = 128 - threshold;

    for (int y = 0; y < h; ++y)
    {
        const uchar *srcLine = gray.constScanLine(y);
        uchar *dstLine = mono.scanLine(y);

        for (int x = 0; x < w; ++x)
        {
            int v = srcLine[x] + shift;
            if (v < 0)   v = 0;
            if (v > 255) v = 255;

            bool on;
            if (d == Bayer)
            {
                /* 阈值在 8..248 之间跳变，整体平均下来保持原亮度 */
                const int t = kBay4[y & 3][x & 3] * 16 + 8;
                on = (v > t);
            }
            else
            {
                on = (v >= 128);
            }

            if (on)
            {
                dstLine[x >> 3] |= (uchar)(0x80u >> (x & 7));
            }
        }
    }
    return mono;
}

/*
 * 1bit 图 → 显存字节。
 * 一字节 = 某一列的 8 个像素，bit0 是最上面那个（低位在前）。
 */
QVector<quint8> ImageGen::packToDisplay(const QImage &mono)
{
    const int w = mono.width();
    const int h = mono.height();
    QVector<quint8> out(bytesPerFrame(w, h), 0);

    for (int page = 0; page < h / 8; ++page)
    {
        for (int x = 0; x < w; ++x)
        {
            quint8 b = 0;
            for (int i = 0; i < 8; ++i)
            {
                const int y = page * 8 + i;
                const uchar *line = mono.constScanLine(y);
                if ((line[x >> 3] >> (7 - (x & 7))) & 1)
                {
                    b |= (quint8)(1u << i);
                }
            }
            out[page * w + x] = b;
        }
    }
    return out;
}

/*
 * 显存字节 → 1bit 图。
 * 用于预览"真正会被烧进 Flash 的数据"，避免看到的是缩放后而不是单色化后的效果。
 */
QImage ImageGen::unpackFromDisplay(const QVector<quint8> &bytes, int w, int h)
{
    QImage mono(w, h, QImage::Format_Mono);
    mono.setColorCount(2);
    mono.setColor(0, qRgb(0, 0, 0));
    mono.setColor(1, qRgb(255, 255, 255));
    mono.fill(0);

    const int need = bytesPerFrame(w, h);
    if (bytes.size() < need)
    {
        return mono;
    }

    for (int page = 0; page < h / 8; ++page)
    {
        for (int x = 0; x < w; ++x)
        {
            const quint8 b = bytes.at(page * w + x);
            for (int i = 0; i < 8; ++i)
            {
                if (b & (1u << i))
                {
                    mono.scanLine(page * 8 + i)[x >> 3] |=
                        (uchar)(0x80u >> (x & 7));
                }
            }
        }
    }
    return mono;
}

QVector<quint8> ImageGen::convert(const QImage &src, const Options &opt)
{
    const QImage gray = fitToTarget(src, opt.width, opt.height, opt.stretch);
    const QImage mono = toMono(gray, opt.threshold, opt.dither);
    return packToDisplay(mono);
}

/*
 * 生成头文件。
 * 数据按页排列，MCU 侧显示一帧就是"逐页写一整行"，不需要任何转换。
 */
QString ImageGen::sanitizeIdentifier(const QString &name)
{
    QString out;
    out.reserve(name.size());
    for (const QChar c : name)
    {
        if ((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
            (c >= QLatin1Char('a') && c <= QLatin1Char('z')) ||
            (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) ||
            c == QLatin1Char('_'))
        {
            out.append(c);
        }
        else if (!out.endsWith(QLatin1Char('_')))
        {
            /* 连续的非字母数字只算一个下划线：
             * "a b  c" 变成 a_b_c，而不是 a___b___c 这种读不出原意的名字。 */
            out.append(QLatin1Char('_'));
        }
    }
    while (out.startsWith(QLatin1Char('_')))
    {
        out.remove(0, 1);
    }
    while (out.endsWith(QLatin1Char('_')))
    {
        out.chop(1);
    }

    if (out.isEmpty())
    {
        return QStringLiteral("image");
    }
    /* C 标识符不能数字开头 */
    if (out.at(0).isDigit())
    {
        out.prepend(QStringLiteral("img_"));
    }
    return out;
}

QString ImageGen::uniqueName(const QStringList &used, const QString &proposed)
{
    const QString base = sanitizeIdentifier(proposed);

    auto taken = [&used](const QString &n) {
        for (const QString &u : used)
        {
            if (u.compare(n, Qt::CaseInsensitive) == 0)
            {
                return true;
            }
        }
        return false;
    };

    if (!taken(base))
    {
        return base;
    }
    for (int i = 2; i < 10000; ++i)
    {
        const QString candidate = QStringLiteral("%1_%2").arg(base).arg(i);
        if (!taken(candidate))
        {
            return candidate;
        }
    }
    return base;
}

/* ==========================================================================
 *  头文件生成：单份 / 多份打包
 *
 *  两种输出有三大块是完全一样的：帧去重、点阵的十六进制文本、刷屏函数。
 *  抽在这里共用，免得「单份」和「打包」各写一遍、改一处漏一处。
 *
 *  这些函数都带前缀参数（up = LOGO，camel = Logo）：单份那条路仍然传
 *  ANIM / Anim，和以前一样，最后再整体替换 —— 输出逐字节不变。
 * ========================================================================== */
namespace {

/* 帧去重：完全相同的帧只留一份，map 记录逻辑帧号 → 数据帧号 */
void dedupFrames(const QVector<QVector<quint8>> &frames,
                 QVector<QVector<quint8>> *unique,
                 QVector<int> *map)
{
    unique->clear();
    map->clear();
    unique->reserve(frames.size());
    map->reserve(frames.size());

    for (int f = 0; f < frames.size(); ++f)
    {
        int hit = -1;
        for (int u = 0; u < unique->size(); ++u)
        {
            if (unique->at(u) == frames.at(f))
            {
                hit = u;
                break;
            }
        }
        if (hit < 0)
        {
            hit = unique->size();
            unique->append(frames.at(f));
        }
        map->append(hit);
    }
}

/* 每帧的十六进制初始化体（含"第 N 帧"标题），一直到最后的 }; 之前 */
QString frameHexBlock(const QVector<QVector<quint8>> &unique)
{
    QString s;
    for (int f = 0; f < unique.size(); ++f)
    {
        s += QStringLiteral("    /* ---- 第 %1 帧 ---- */\n    {\n").arg(f);
        const QVector<quint8> &b = unique.at(f);
        for (int i = 0; i < b.size(); i += 16)
        {
            s += QStringLiteral("        ");
            const int end = qMin(i + 16, b.size());
            for (int k = i; k < end; ++k)
            {
                s += QStringLiteral("0x%1").arg(b.at(k), 2, 16, QLatin1Char('0')).toUpper()
                         .replace(QStringLiteral("0X"), QStringLiteral("0x"));
                if (k + 1 < b.size())
                {
                    s += QLatin1Char(',');
                }
            }
            s += QLatin1Char('\n');
        }
        s += QStringLiteral("    },\n");
    }
    return s;
}

/* 逻辑帧号 → 数据帧号的映射表（只有去重时才生成） */
QString mapBlock(const QString &up, const QString &camel, const QString &idxType,
                 const QVector<int> &map, int count)
{
    QString s;
    s += QStringLiteral("/* 逻辑帧号 -> 数据帧号。重复的帧指向同一份数据，逐帧重刷画面一样，\n");
    s += QStringLiteral(" * 所以播放写法和没有重复帧时完全一致。 */\n");
    s += QStringLiteral("static const %1 %2_Map[%3_COUNT] =\n{\n").arg(idxType, camel, up);
    for (int f = 0; f < count; f += 16)
    {
        s += QStringLiteral("    ");
        const int end = qMin(f + 16, count);
        for (int k = f; k < end; ++k)
        {
            s += QStringLiteral("%1").arg(map.at(k), 3);
            if (k + 1 < count)
            {
                s += QLatin1Char(',');
            }
        }
        s += QStringLiteral("   /* 帧 %1..%2 */\n").arg(f).arg(end - 1);
    }
    s += QStringLiteral("};\n\n");
    return s;
}

/* 整屏刷一帧的函数。writeCmd / writeData / unused 是适配区里那三个宏 */
QString showFrameBlock(const QString &up, const QString &camel,
                       const QString &writeCmd, const QString &writeData,
                       const QString &unused, const QString &idxType, bool deduped)
{
    QString s;
    s += QStringLiteral("/* 把第 i 帧整屏刷到屏上。自己发三条定位命令，不依赖驱动里的设光标函数，\n");
    s += QStringLiteral(" * 也就绕开了各种例程参数顺序（页在前 / 列在前）不一致的问题。 */\n");
    s += QStringLiteral("static %1 void %2_ShowFrame(%3 i)\n").arg(unused, camel, idxType);
    s += QStringLiteral("{\n");
    s += QStringLiteral("    const uint8_t *f;\n");
    s += QStringLiteral("    uint8_t page;\n");
    s += QStringLiteral("    uint8_t col;\n");
    s += QStringLiteral("\n");
    s += QStringLiteral("    if (i >= %1_COUNT)\n").arg(up);
    s += QStringLiteral("    {\n");
    s += QStringLiteral("        return;\n");
    s += QStringLiteral("    }\n");
    s += QStringLiteral("    f = &%1_Frames[%2][0];\n")
             .arg(camel, deduped ? QStringLiteral("%1_Map[i]").arg(camel) : QStringLiteral("i"));
    s += QStringLiteral("\n");
    s += QStringLiteral("    for (page = 0; page < %1_PAGES; page++)\n").arg(up);
    s += QStringLiteral("    {\n");
    s += QStringLiteral("        %1((uint8_t)(0xB0 | (page & 0x07)));\n").arg(writeCmd);
    s += QStringLiteral("        %1((uint8_t)(0x10 | ((%2_COL_OFFSET >> 4) & 0x0F)));\n")
             .arg(writeCmd, up);
    s += QStringLiteral("        %1((uint8_t)(0x00 | (%2_COL_OFFSET & 0x0F)));\n")
             .arg(writeCmd, up);
    s += QStringLiteral("\n");
    s += QStringLiteral("        for (col = 0; col < %1_WIDTH; col++)\n").arg(up);
    s += QStringLiteral("        {\n");
    s += QStringLiteral("            %1(f[page * %2_WIDTH + col]);\n").arg(writeData, up);
    s += QStringLiteral("        }\n");
    s += QStringLiteral("    }\n");
    s += QStringLiteral("}\n\n");
    return s;
}

}   // namespace

QString ImageGen::renderHeader(const QVector<QVector<quint8>> &frames,
                               const Options &opt,
                               int delayMs,
                               const QString &sourceName,
                               const QString &ident)
{
    const int frameSize = bytesPerFrame(opt.width, opt.height);
    const int count = frames.size();
    const int pages = opt.height / 8;

    /*
     * 帧去重。
     *
     * GIF 里静止段落很常见：一个人物眨眼，中间十几帧完全一样，各占 1KB
     * 纯属浪费；循环播放的转圈图标首尾帧也往往相同。这里把完全一样的帧
     * 只存一份，再用一张 1 字节/帧的映射表把逻辑帧号指回去。
     *
     * 不做"合并成更长的延时"：那需要每帧独立的延时数组和更绕的播放循环，
     * 而逐帧重刷同一份数据画面完全一致，代码只多一个下标。屏上看不出区别。
     *
     * 比较用 QVector 的 operator==，先比长度再比内容，n 通常几十，开销可以忽略。
     */
    QVector<QVector<quint8>> unique;
    QVector<int> map;
    dedupFrames(frames, &unique, &map);
    const int dataCount = unique.size();
    const bool deduped  = (dataCount < count);

    /*
     * 帧号用多宽的类型。
     *
     * 原来一律写 uint8_t，而界面上最多可以导出 600 帧，于是：
     *   ① Anim_ShowFrame(300) 传进来先被截成 44，第 256 帧以后永远看不到；
     *   ② 头文件里那段示例 for (uint8_t i = 0; i < ANIM_COUNT; i++) 在
     *      ANIM_COUNT=600 时根本退不出去 —— i 到 255 回绕成 0，死循环；
     *   ③ 去重映射表 Anim_Map[] 存的是数据帧号，去重后仍可能超过 255。
     * 帧数不超过 255 时继续用 uint8_t，输出和以前逐字节相同。
     */
    const bool wideIndex = (count > 255) || (dataCount > 255);
    const QString idxType = wideIndex ? QStringLiteral("uint16_t")
                                      : QStringLiteral("uint8_t");

    QString s;
    s += QStringLiteral("/* ============================================================\n");
    s += QStringLiteral(" *  %1\n").arg(
        count > 1 ? QStringLiteral("点阵动画数据 + 显示函数")
                  : QStringLiteral("点阵图片数据 + 显示函数"));
    s += QStringLiteral(" *\n");
    s += QStringLiteral(" *  来源    : %1\n").arg(sourceName);
    s += QStringLiteral(" *  尺寸    : %1 x %2\n").arg(opt.width).arg(opt.height);
    s += QStringLiteral(" *  帧数    : %1\n").arg(count);
    if (deduped)
    {
        s += QStringLiteral(" *  去重    : 重复帧只存一份  %1 -> %2 帧，省 %3 字节\n")
                 .arg(count)
                 .arg(dataCount)
                 .arg((count - dataCount) * frameSize);
    }
    if (delayMs > 0 && count > 1)
    {
        s += QStringLiteral(" *  帧延时  : %1 ms  (约 %2 fps)\n")
                 .arg(delayMs).arg(1000.0 / delayMs, 0, 'f', 1);
    }
    s += QStringLiteral(" *  数据量  : %1 字节 (%2 KB)\n")
             .arg(dataCount * frameSize)
             .arg(dataCount * frameSize / 1024.0, 0, 'f', 1);
    s += QStringLiteral(" *  单色化  : %1, 阈值 %2\n")
             .arg(opt.dither == Bayer ? QStringLiteral("抖动") : QStringLiteral("简单阈值"))
             .arg(opt.threshold);
    s += QStringLiteral(" *\n");
    s += QStringLiteral(" *  数据格式：阴码 / 列行式 / 逆向(低位在前) —— 和汉字字库完全一致\n");
    s += QStringLiteral(" *            每字节 = 一列的 8 个像素，bit0 在最上面\n");
    s += QStringLiteral(" *            按页排列：先第 0 页的 0~%1 列，再第 1 页……\n").arg(opt.width - 1);
    s += QStringLiteral(" *\n");
    s += QStringLiteral(" *  页数    : %1 页，每页 %2 字节\n").arg(pages).arg(opt.width);
    s += QStringLiteral(" *\n");
    s += QStringLiteral(" *  本文件自带 Anim_ShowFrame()，不依赖任何批量写或设光标函数。\n");
    s += QStringLiteral(" *  只要你的驱动有写命令、写数据这两个最底层的函数（初始化时必然用到），\n");
    s += QStringLiteral(" *  把下面【适配区】的宏指向它们就能直接跑。\n");
    s += QStringLiteral(" *\n");
    s += QStringLiteral(" *  用法（只在一个 .c 里 include 本文件，多个 .c 包含会重复定义数组）：\n");
    if (count > 1)
    {
        s += QStringLiteral(" *      %1 i;\n").arg(idxType);
        s += QStringLiteral(" *      while (1) {\n");
        s += QStringLiteral(" *          for (i = 0; i < ANIM_COUNT; i++) {\n");
        s += QStringLiteral(" *              Anim_ShowFrame(i);\n");
        s += QStringLiteral(" *              ANIM_DELAY(ANIM_DELAY_MS);\n");
        s += QStringLiteral(" *          }\n");
        s += QStringLiteral(" *      }\n");
    }
    else
    {
        s += QStringLiteral(" *      Anim_ShowFrame(0);      只有一帧，刷一次就是静态图\n");
    }
    s += QStringLiteral(" * ============================================================ */\n\n");

    s += QStringLiteral("#ifndef __ANIM_FRAMES_H\n");
    s += QStringLiteral("#define __ANIM_FRAMES_H\n\n");
    s += QStringLiteral("#include <stdint.h>\n\n");
    s += QStringLiteral("#define ANIM_WIDTH      %1\n").arg(opt.width);
    s += QStringLiteral("#define ANIM_HEIGHT     %1\n").arg(opt.height);
    s += QStringLiteral("#define ANIM_PAGES      (ANIM_HEIGHT / 8)\n");
    s += QStringLiteral("#define ANIM_FRAME_SIZE %1\n").arg(frameSize);
    s += QStringLiteral("#define ANIM_COUNT      %1\n").arg(count);
    if (deduped)
    {
        s += QStringLiteral("/* 上面是播放帧数。完全相同的帧只存了一份，所以数组里实际只有 %1 帧；\n")
                 .arg(dataCount);
        s += QStringLiteral(" * 中间靠下面的 Anim_Map 把逻辑帧号指回数据帧号，播放时长不变。 */\n");
        s += QStringLiteral("#define ANIM_DATA_COUNT %1\n").arg(dataCount);
    }
    s += QStringLiteral("#define ANIM_DELAY_MS   %1\n\n").arg(delayMs);

    s += QStringLiteral("/* ============================================================\n");
    s += QStringLiteral(" *  【适配区】只改这里，下面的函数体不用动\n");
    s += QStringLiteral(" *\n");
    s += QStringLiteral(" *  驱动的底层只有两个动作：发一条命令、发一个字节数据。\n");
    s += QStringLiteral(" *  这两个函数在不同例程里名字和参数都不同，常见两种写法：\n");
    s += QStringLiteral(" *\n");
    s += QStringLiteral(" *  (A) 命令和数据分成两个函数（中景园新版、多数 HAL 例程）\n");
    s += QStringLiteral(" *      #define ANIM_WRITE_CMD(c)   OLED_WriteCommand(c)\n");
    s += QStringLiteral(" *      #define ANIM_WRITE_DATA(d)  OLED_WriteData(d)\n");
    s += QStringLiteral(" *\n");
    s += QStringLiteral(" *  (B) 一个函数加 cmd 标志位（正点原子、中景园旧版）\n");
    s += QStringLiteral(" *      #define ANIM_WRITE_CMD(c)   OLED_WR_Byte((c), 1)\n");
    s += QStringLiteral(" *      #define ANIM_WRITE_DATA(d)  OLED_WR_Byte((d), 0)\n");
    s += QStringLiteral(" *\n");
    s += QStringLiteral(" *  有些工程把这两个函数只写在 .c 里、没在 .h 里声明，在自己文件里\n");
    s += QStringLiteral(" *  调用会报未声明。把这两行补进对应的 .h 即可：\n");
    s += QStringLiteral(" *      void OLED_WriteCommand(uint8_t cmd);\n");
    s += QStringLiteral(" *      void OLED_WriteData(uint8_t dat);\n");
    s += QStringLiteral(" * ============================================================ */\n");
    s += QStringLiteral("#define ANIM_WRITE_CMD(c)    OLED_WriteCommand(c)\n");
    s += QStringLiteral("#define ANIM_WRITE_DATA(d)   OLED_WriteData(d)\n\n");
    s += QStringLiteral("/* 帧间延时：HAL_Delay(ms) / vTaskDelay(pdMS_TO_TICKS(ms)) / Delay_ms(ms) */\n");
    s += QStringLiteral("#define ANIM_DELAY(ms)       HAL_Delay(ms)\n\n");
    s += QStringLiteral("/* 屏是 SH1106（显存 132 列，可见区从第 2 列开始）时是 2，SSD1306 是 0。\n");
    s += QStringLiteral("   这个值导出时按驱动源码自动判断后填好，一般不用改；\n");
    s += QStringLiteral("   判断错了（右边框缺两列、整幅左移 2 像素）就手改成 2。 */\n");
    s += QStringLiteral("#define ANIM_COL_OFFSET      %1\n\n").arg(opt.colOffset < 0 ? 0 : opt.colOffset);

    s += QStringLiteral("#if defined(__GNUC__)\n");
    s += QStringLiteral("#define ANIM_UNUSED  __attribute__((unused))\n");
    s += QStringLiteral("#else\n");
    s += QStringLiteral("#define ANIM_UNUSED\n");
    s += QStringLiteral("#endif\n\n");

    s += QStringLiteral("const uint8_t Anim_Frames[%1][ANIM_FRAME_SIZE] =\n{\n")
             .arg(deduped ? QStringLiteral("ANIM_DATA_COUNT") : QStringLiteral("ANIM_COUNT"));
    s += frameHexBlock(unique);
    s += QStringLiteral("};\n\n");

    if (deduped)
    {
        s += mapBlock(QStringLiteral("ANIM"), QStringLiteral("Anim"), idxType, map, count);
    }

    s += showFrameBlock(QStringLiteral("ANIM"), QStringLiteral("Anim"),
                        QStringLiteral("ANIM_WRITE_CMD"), QStringLiteral("ANIM_WRITE_DATA"),
                        QStringLiteral("ANIM_UNUSED"), idxType, deduped);

    s += QStringLiteral("#endif /* __ANIM_FRAMES_H */\n");

    /* 换前缀放在最后做：整份文本里 ANIM_* / Anim_* 就是全部对外名字，
     * 逐处改成变量反而容易漏一个（漏了就是两份头文件撞定义，编译期才炸）。
     * 注释里的名字也跟着换，注释才和代码对得上。
     * ident 为空 = 老行为，一个字节都不动。 */
    if (!ident.isEmpty())
    {
        QString obj = sanitizeIdentifier(ident);
        obj[0] = obj.at(0).toUpper();

        /* __ANIM_FRAMES_H 也走这条替换，所以 include guard 天然不撞 */
        s.replace(QStringLiteral("ANIM_"), obj.toUpper() + QLatin1Char('_'));
        s.replace(QStringLiteral("Anim_"), obj + QLatin1Char('_'));
    }
    return s;
}

QString ImageGen::writeHeaderFile(const QString &path,
                                  const QVector<QVector<quint8>> &frames,
                                  const Options &opt,
                                  int delayMs,
                                  const QString &sourceName,
                                  const QString &ident)
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
    ts.setEncoding(QStringConverter::Utf8);
    ts << renderHeader(frames, opt, delayMs, sourceName, ident);

    /* 显式 close 再看错误：磁盘满这类问题在写的时候不报，落盘那一步才报 */
    file.close();
    if (file.error() != QFileDevice::NoError)
    {
        return file.errorString();
    }
    return QString();
}

/* ==========================================================================
 *  多份素材打包进一个头文件（各自独立，不合成动画）
 * ========================================================================== */

QString ImageGen::renderMultiHeader(const QVector<Asset> &assets, const QString &title)
{
    /* 空帧的素材直接跳过：C 里没有"长度 0 的数组"，
     * 生成出来只会是一份编不过的文件，不如不写。 */
    QVector<Asset> list;
    for (const Asset &a : assets)
    {
        if (!a.frames.isEmpty())
        {
            list.append(a);
        }
    }
    if (list.isEmpty())
    {
        return QString();
    }

    /*
     * include guard 跟着文件名走：assets.h → __ASSETS_H。
     *
     * 单份头文件早就因为这个吃过大亏（两份都叫 __ANIM_FRAMES_H，
     * 第二份被第一份的 guard 整份吞掉，不报错、只是数据没了）。
     * 打包文件如果统一写死一个 guard，同样的事情会在"两个素材包"上重演。
     */
    const QString base = QFileInfo(title).completeBaseName();
    QString guard = base.isEmpty() ? QStringLiteral("OLED_ASSETS")
                                   : sanitizeIdentifier(base).toUpper();
    guard = QStringLiteral("__%1_H").arg(guard);

    /* 素材名去重：两处都叫 logo 时会生成两套一模一样的宏，
     * 编译器报"重定义"，用户看不出是自己起名撞了。这里按界面那套规则
     * 补 _2 / _3，保证生成的文件一定编得过。 */
    QStringList used;
    QVector<QString> camelOf;
    QVector<QString> upOf;
    camelOf.reserve(list.size());
    upOf.reserve(list.size());
    for (int i = 0; i < list.size(); ++i)
    {
        /* 名字为空时先补个 image 再查重：空串查不出重名，
         * 两份空名字会得到两个 IMAGE_，又撞上了。 */
        const QString proposed = list.at(i).ident.isEmpty() ? QStringLiteral("image")
                                                            : list.at(i).ident;
        QString obj = sanitizeIdentifier(uniqueName(used, proposed));
        used << obj;
        obj[0] = obj.at(0).toUpper();
        camelOf.append(obj);
        upOf.append(obj.toUpper());
    }

    int totalBytes = 0;
    for (const Asset &a : list)
    {
        totalBytes += bytesPerFrame(a.opt.width, a.opt.height) * a.frames.size();
    }

    QString s;

    /* ---------------- 文件抬头 ---------------- */
    s += QStringLiteral("/* ============================================================\n");
    s += QStringLiteral(" *  OLED 点阵素材包：%1 份素材，各存各的\n").arg(list.size());
    s += QStringLiteral(" *\n");
    s += QStringLiteral(" *  这里**没有把素材合成成一个动画**：每份都有自己的宏、数组和显示\n");
    s += QStringLiteral(" *  函数，名字按素材名打头（logo → LOGO_WIDTH / Logo_ShowFrame），\n");
    s += QStringLiteral(" *  互不影响。想刷哪一份就调哪一份的 Xxx_ShowFrame(i)。\n");
    s += QStringLiteral(" *\n");
    s += QStringLiteral(" *  清单（顺序和导出时的素材列表一致）：\n");
    for (int i = 0; i < list.size(); ++i)
    {
        const Asset &a = list.at(i);
        s += QStringLiteral(" *    %1. %2  ——  %3x%4，%5 帧，%6 字节，导入自 %7\n")
                 .arg(i + 1)
                 .arg(camelOf.at(i), -10)
                 .arg(a.opt.width)
                 .arg(a.opt.height)
                 .arg(a.frames.size())
                 .arg(bytesPerFrame(a.opt.width, a.opt.height) * a.frames.size())
                 .arg(a.sourceName.isEmpty() ? QStringLiteral("未记录") : a.sourceName);
    }
    s += QStringLiteral(" *\n");
    s += QStringLiteral(" *  合计    : %1 字节 (%2 KB)\n")
             .arg(totalBytes)
             .arg(totalBytes / 1024.0, 0, 'f', 1);
    s += QStringLiteral(" *\n");
    s += QStringLiteral(" *  数据格式：阴码 / 列行式 / 逆向(低位在前) —— 和汉字字库完全一致\n");
    s += QStringLiteral(" *            每字节 = 一列的 8 个像素，bit0 在最上面\n");
    s += QStringLiteral(" *            按页排列：先第 0 页的 0~W-1 列，再第 1 页……\n");
    s += QStringLiteral(" *\n");
    s += QStringLiteral(" *  用法：只在一个 .c 里 include 本文件（多个 .c 包含会重复定义数组）。\n");
    s += QStringLiteral(" * ============================================================ */\n\n");

    s += QStringLiteral("#ifndef %1\n").arg(guard);
    s += QStringLiteral("#define %1\n\n").arg(guard);
    s += QStringLiteral("#include <stdint.h>\n\n");

    /* ---------------- 适配区（全文件共用一份） ---------------- */
    s += QStringLiteral("/* ============================================================\n");
    s += QStringLiteral(" *  【适配区】多份素材共用，只改这里，下面的函数体不用动\n");
    s += QStringLiteral(" *\n");
    s += QStringLiteral(" *  驱动的底层只有两个动作：发一条命令、发一个字节数据。\n");
    s += QStringLiteral(" *  常见两种写法：\n");
    s += QStringLiteral(" *\n");
    s += QStringLiteral(" *  (A) 命令和数据分成两个函数（中景园新版、多数 HAL 例程）\n");
    s += QStringLiteral(" *      #define OLED_WRITE_CMD(c)   OLED_WriteCommand(c)\n");
    s += QStringLiteral(" *      #define OLED_WRITE_DATA(d)  OLED_WriteData(d)\n");
    s += QStringLiteral(" *\n");
    s += QStringLiteral(" *  (B) 一个函数加 cmd 标志位（正点原子、中景园旧版）\n");
    s += QStringLiteral(" *      #define OLED_WRITE_CMD(c)   OLED_WR_Byte((c), 1)\n");
    s += QStringLiteral(" *      #define OLED_WRITE_DATA(d)  OLED_WR_Byte((d), 0)\n");
    s += QStringLiteral(" *\n");
    s += QStringLiteral(" *  有些工程把这两个函数只写在 .c 里、没在 .h 里声明，在自己文件里\n");
    s += QStringLiteral(" *  调用会报未声明。把这两行补进对应的 .h 即可：\n");
    s += QStringLiteral(" *      void OLED_WriteCommand(uint8_t cmd);\n");
    s += QStringLiteral(" *      void OLED_WriteData(uint8_t dat);\n");
    s += QStringLiteral(" * ============================================================ */\n");
    s += QStringLiteral("#define OLED_WRITE_CMD(c)    OLED_WriteCommand(c)\n");
    s += QStringLiteral("#define OLED_WRITE_DATA(d)   OLED_WriteData(d)\n\n");
    s += QStringLiteral("/* 帧间延时：HAL_Delay(ms) / vTaskDelay(pdMS_TO_TICKS(ms)) / Delay_ms(ms) */\n");
    s += QStringLiteral("#define OLED_DELAY(ms)       HAL_Delay(ms)\n\n");
    s += QStringLiteral("#if defined(__GNUC__)\n");
    s += QStringLiteral("#define OLED_UNUSED  __attribute__((unused))\n");
    s += QStringLiteral("#else\n");
    s += QStringLiteral("#define OLED_UNUSED\n");
    s += QStringLiteral("#endif\n\n");

    /* ---------------- 每份素材各一块 ---------------- */
    for (int i = 0; i < list.size(); ++i)
    {
        const Asset &a      = list.at(i);
        const QString &up   = upOf.at(i);
        const QString &cam  = camelOf.at(i);

        const int frameSize = bytesPerFrame(a.opt.width, a.opt.height);
        const int count     = a.frames.size();
        const int pages     = a.opt.height / 8;

        QVector<QVector<quint8>> unique;
        QVector<int> map;
        dedupFrames(a.frames, &unique, &map);
        const int  dataCount = unique.size();
        const bool deduped   = (dataCount < count);

        const bool wideIndex = (count > 255) || (dataCount > 255);
        const QString idxType = wideIndex ? QStringLiteral("uint16_t")
                                          : QStringLiteral("uint8_t");

        const QString src = a.sourceName.isEmpty() ? QStringLiteral("未记录") : a.sourceName;

        /* 每块的抬头。第一行就写"导入自哪个文件" —— 隔了几天回来，
         * 只看点阵是认不出它原来是哪张图的。 */
        s += QStringLiteral("/* ============================================================\n");
        s += QStringLiteral(" *  素材 %1/%2：%3\n").arg(i + 1).arg(list.size()).arg(cam);
        s += QStringLiteral(" *  导入自  : %1\n").arg(src);
        s += QStringLiteral(" *  尺寸    : %1 x %2（%3 页，每页 %4 字节；一帧 %5 字节）\n")
                 .arg(a.opt.width).arg(a.opt.height).arg(pages).arg(a.opt.width).arg(frameSize);
        if (deduped)
        {
            s += QStringLiteral(" *  帧数    : %1 帧（重复帧只存一份，数据 %2 份）\n")
                     .arg(count).arg(dataCount);
        }
        else
        {
            s += QStringLiteral(" *  帧数    : %1 帧\n").arg(count);
        }
        if (count > 1)
        {
            s += QStringLiteral(" *  帧延时  : %1 ms  (约 %2 fps)\n")
                     .arg(a.delayMs).arg(1000.0 / qMax(1, a.delayMs), 0, 'f', 1);
        }
        s += QStringLiteral(" *  数据量  : %1 字节 (%2 KB)\n")
                 .arg(dataCount * frameSize)
                 .arg(dataCount * frameSize / 1024.0, 0, 'f', 1);
        if (count > 1)
        {
            s += QStringLiteral(" *  刷屏    : %1_ShowFrame(0) … %1_ShowFrame(%2)\n")
                     .arg(cam).arg(count - 1);
        }
        else
        {
            s += QStringLiteral(" *  刷屏    : %1_ShowFrame(0)  只有一帧，刷一次就是静态图\n").arg(cam);
        }
        s += QStringLiteral(" * ============================================================ */\n");

        auto def = [](const QString &name, const QString &value) {
            return QStringLiteral("#define %1 %2\n").arg(name, -20).arg(value);
        };
        s += def(up + QStringLiteral("_WIDTH"), QString::number(a.opt.width));
        s += def(up + QStringLiteral("_HEIGHT"), QString::number(a.opt.height));
        s += def(up + QStringLiteral("_PAGES"),
                 QStringLiteral("(%1_HEIGHT / 8)").arg(up));
        s += def(up + QStringLiteral("_FRAME_SIZE"), QString::number(frameSize));
        s += def(up + QStringLiteral("_COUNT"), QString::number(count));
        if (deduped)
        {
            s += QStringLiteral("/* 上面是播放帧数。完全相同的帧只存了一份，所以数组里实际只有 %1 帧；\n")
                     .arg(dataCount);
            s += QStringLiteral(" * 中间靠下面的 %1_Map 把逻辑帧号指回数据帧号，播放时长不变。 */\n")
                     .arg(cam);
            s += def(up + QStringLiteral("_DATA_COUNT"), QString::number(dataCount));
        }
        s += def(up + QStringLiteral("_DELAY_MS"), QString::number(a.delayMs));
        s += def(up + QStringLiteral("_COL_OFFSET"),
                 QString::number(a.opt.colOffset < 0 ? 0 : a.opt.colOffset));
        s += QStringLiteral("\n");

        /* 点阵正上方再写一次文件名：这段是用户真正要"认数据"的地方，
         * 抬头离得远，翻到数组这里就想直接看到它来自哪个文件。 */
        s += QStringLiteral("/* 点阵数据 —— %1，导入自 %2 */\n").arg(cam, src);
        s += QStringLiteral("const uint8_t %1_Frames[%2][%3_FRAME_SIZE] =\n{\n")
                 .arg(cam,
                      deduped ? up + QStringLiteral("_DATA_COUNT") : up + QStringLiteral("_COUNT"),
                      up);
        s += frameHexBlock(unique);
        s += QStringLiteral("};\n\n");

        if (deduped)
        {
            s += mapBlock(up, cam, idxType, map, count);
        }

        s += showFrameBlock(up, cam,
                            QStringLiteral("OLED_WRITE_CMD"), QStringLiteral("OLED_WRITE_DATA"),
                            QStringLiteral("OLED_UNUSED"), idxType, deduped);
    }

    s += QStringLiteral("#endif /* %1 */\n").arg(guard);
    return s;
}

QString ImageGen::writeMultiHeaderFile(const QString &path,
                                       const QVector<Asset> &assets,
                                       const QString &title)
{
    if (path.isEmpty())
    {
        return QStringLiteral("没有指定文件名");
    }

    /* 标题没给就用文件名：include guard 靠它区分不同的素材包 */
    const QString name = title.isEmpty() ? QFileInfo(path).fileName() : title;
    const QString content = renderMultiHeader(assets, name);
    if (content.isEmpty())
    {
        return QStringLiteral("没有可导出的素材（每份素材都至少要有 1 帧）");
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        return file.errorString();
    }

    QTextStream ts(&file);
    ts.setEncoding(QStringConverter::Utf8);
    ts << content;

    /* 同 writeHeaderFile：open 成功不等于写成功，落盘那一步才报错 */
    file.close();
    if (file.error() != QFileDevice::NoError)
    {
        return file.errorString();
    }
    return QString();
}

/* ==========================================================================
 *  读图：单张 / 一次多选
 *
 *  为什么放在这里而不是主窗口里：多选要处理的边界（文件名排序、GIF 展开、
 *  跨文件共用的帧数与内存上限、个别文件读不出来）都不涉及界面，
 *  单独放一处才好写测试。主窗口只管弹对话框和显示结果。
 * ========================================================================== */

/* 把一个图片文件读成帧，追加到 out。
 *
 *   budgetLeft  剩余内存预算（字节），读完扣减，跨文件共享
 *   maxFrames   本批次还能收几帧，同样跨文件共享
 *   fileTotal   回填"这个文件里本来有多少帧"
 *   delayMs     进来是 0 时按文件里的 GIF 延时填；已经填过就不覆盖
 *               —— 多张静态图拼动画时整个序列用同一档延时，不会一张快一张慢
 *   truncated   这个文件没读完
 *   byCount / byMemory  截断的原因：撞帧数上限 / 内存预算不够。
 *               界面要靠它说清楚"为什么只读了这么多"，
 *               否则只能拼出"共 N 帧，只读了前 N 帧"这种自相矛盾的话。
 *
 *  返回实际追加的帧数。0 表示这个文件一帧都没贡献（坏了，或预算已耗尽）。 */
static int readOneFile(const QString &path, QVector<QImage> &out,
                       qint64 &budgetLeft, int maxFrames,
                       int &fileTotal, int &delayMs,
                       bool &truncated, bool &byCount, bool &byMemory)
{
    fileTotal = 0;
    truncated = false;
    byCount   = false;
    byMemory  = false;

    if (maxFrames <= 0)
    {
        truncated = true;
        return 0;
    }

    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);

    /* 静态图片只有 1 帧，GIF 才有 imageCount() > 1 */
    const QSize rawSize = reader.size();
    const qint64 perFrame = (rawSize.width() > 0 && rawSize.height() > 0)
                                ? qint64(rawSize.width()) * qint64(rawSize.height()) * 4
                                : 0;

    const int count = reader.imageCount();
    if (count > 1)
    {
        fileTotal = count;
        int limit = qMin(count, maxFrames);
        byCount = (count > maxFrames);

        if (perFrame > 0)
        {
            const qint64 fit = budgetLeft / perFrame;
            if (fit < limit)
            {
                /* 预算连一帧都放不下时也留一帧：让用户至少看到画面，
                 * 而不是"选了文件什么都没发生"。截断标记照常置上。 */
                limit = int(qBound<qint64>(qint64(1), fit, qint64(limit)));
                byMemory = (limit < count);
            }
        }
        truncated = (limit < count);

        int added = 0;
        for (int i = 0; i < limit; ++i)
        {
            const QImage img = reader.read();
            if (img.isNull())
            {
                break;
            }
            if (delayMs <= 0)
            {
                const int d = reader.nextImageDelay();
                if (d > 0)
                {
                    delayMs = d;
                }
            }
            out.append(img);
            ++added;
            /* 实际尺寸可能和 size() 报的不一样（局部帧），扣预算按真图算 */
            const qint64 real = qint64(img.width()) * qint64(img.height()) * 4;
            budgetLeft -= qMax(real, qint64(1));
        }
        return added;
    }

    /* 单帧：整张读一次 */
    const QImage img = reader.read();
    if (img.isNull())
    {
        return 0;
    }
    fileTotal = 1;

    const qint64 need = qint64(img.width()) * qint64(img.height()) * 4;
    if (need > budgetLeft)
    {
        truncated = true;
        byMemory  = true;
        return 0;
    }
    out.append(img);
    budgetLeft -= qMax(need, qint64(1));
    return 1;
}

ImageGen::FrameBatch ImageGen::loadFrames(const QStringList &paths,
                                         int maxFrames,
                                         qint64 budgetBytes)
{
    FrameBatch batch;

    QStringList ordered = paths;

    /* 按文件名自然排序 —— frame2 要排在 frame10 前面。
     * 对话框返回的顺序取决于系统和点选次序，指望不上；
     * 资源管理器里看着是排好的，直接用字符串比较又会把 10 排到 2 前面。 */
    if (ordered.size() > 1)
    {
        QCollator collator;
        collator.setNumericMode(true);
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        std::stable_sort(ordered.begin(), ordered.end(),
                         [&collator](const QString &a, const QString &b) {
                             return collator.compare(QFileInfo(a).fileName(),
                                                     QFileInfo(b).fileName()) < 0;
                         });
    }

    qint64 budget = budgetBytes;
    batch.files = ordered;
    batch.limitFrames = maxFrames;

    for (int i = 0; i < ordered.size(); ++i)
    {
        if (batch.frames.size() >= maxFrames)
        {
            /* 撞到帧数上限：剩下的文件整个不读。
             * "没读的文件数"要记下来，否则界面只能说"只读了前 N 帧"，
             * 用户不知道还有多少被跳过了。 */
            batch.truncated     = true;
            batch.cappedByCount = true;
            batch.skippedFiles  = ordered.size() - i;
            break;
        }

        int  fileTotal = 0;
        bool trunc     = false;
        bool byCount   = false;
        bool byMemory  = false;
        const int added = readOneFile(ordered.at(i), batch.frames, budget,
                                      maxFrames - batch.frames.size(),
                                      fileTotal, batch.delayMs,
                                      trunc, byCount, byMemory);
        batch.totalFrames += fileTotal;
        if (trunc)
        {
            batch.truncated = true;
        }
        batch.cappedByCount  = batch.cappedByCount  || byCount;
        batch.cappedByMemory = batch.cappedByMemory || byMemory;

        if (added == 0)
        {
            /* 一帧都没收进来有两种原因，界面上的话完全不同：
             *   文件坏了      → "读不出图像"，用户该换一张图
             *   内存预算不够  → 文件没问题，只是这批读不下，用户该少选几张
             * 原来两种都塞进 failed，于是界面把好好的文件列进
             * "下面这些读不出图像，已跳过"，用户拿着图去找为什么打不开，
             * 而真正的原因（预算）只写在另一行的"只收了前 N 帧"里。
             * 预算这类不算失败，交给 cappedByMemory 说。 */
            if (!byMemory)
            {
                batch.failed << QFileInfo(ordered.at(i)).fileName();
            }
            if (budget <= 0)
            {
                break;      /* 预算见底，后面的文件也读不进来 */
            }
        }
    }

    return batch;
}
