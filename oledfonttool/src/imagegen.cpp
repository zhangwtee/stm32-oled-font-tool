#include "imagegen.h"

#include <QPainter>
#include <QStringList>

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
QString ImageGen::renderHeader(const QVector<QVector<quint8>> &frames,
                               const Options &opt,
                               int delayMs,
                               const QString &sourceName)
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
    unique.reserve(count);
    map.reserve(count);
    for (int f = 0; f < count; ++f)
    {
        int hit = -1;
        for (int u = 0; u < unique.size(); ++u)
        {
            if (unique.at(u) == frames.at(f))
            {
                hit = u;
                break;
            }
        }
        if (hit < 0)
        {
            hit = unique.size();
            unique.append(frames.at(f));
        }
        map.append(hit);
    }
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

    for (int f = 0; f < dataCount; ++f)
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

    s += QStringLiteral("};\n\n");

    if (deduped)
    {
        s += QStringLiteral("/* 逻辑帧号 -> 数据帧号。重复的帧指向同一份数据，逐帧重刷画面一样，\n");
        s += QStringLiteral(" * 所以播放写法和没有重复帧时完全一致。 */\n");
        s += QStringLiteral("static const %1 Anim_Map[ANIM_COUNT] =\n{\n").arg(idxType);
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
    }

    s += QStringLiteral("/* 把第 i 帧整屏刷到屏上。自己发三条定位命令，不依赖驱动里的设光标函数，\n");
    s += QStringLiteral(" * 也就绕开了各种例程参数顺序（页在前 / 列在前）不一致的问题。 */\n");
    s += QStringLiteral("static ANIM_UNUSED void Anim_ShowFrame(%1 i)\n").arg(idxType);
    s += QStringLiteral("{\n");
    s += QStringLiteral("    const uint8_t *f;\n");
    s += QStringLiteral("    uint8_t page;\n");
    s += QStringLiteral("    uint8_t col;\n");
    s += QStringLiteral("\n");
    s += QStringLiteral("    if (i >= ANIM_COUNT)\n");
    s += QStringLiteral("    {\n");
    s += QStringLiteral("        return;\n");
    s += QStringLiteral("    }\n");
    s += QStringLiteral("    f = &Anim_Frames[%1][0];\n")
             .arg(deduped ? QStringLiteral("Anim_Map[i]") : QStringLiteral("i"));
    s += QStringLiteral("\n");
    s += QStringLiteral("    for (page = 0; page < ANIM_PAGES; page++)\n");
    s += QStringLiteral("    {\n");
    s += QStringLiteral("        ANIM_WRITE_CMD((uint8_t)(0xB0 | (page & 0x07)));\n");
    s += QStringLiteral("        ANIM_WRITE_CMD((uint8_t)(0x10 | ((ANIM_COL_OFFSET >> 4) & 0x0F)));\n");
    s += QStringLiteral("        ANIM_WRITE_CMD((uint8_t)(0x00 | (ANIM_COL_OFFSET & 0x0F)));\n");
    s += QStringLiteral("\n");
    s += QStringLiteral("        for (col = 0; col < ANIM_WIDTH; col++)\n");
    s += QStringLiteral("        {\n");
    s += QStringLiteral("            ANIM_WRITE_DATA(f[page * ANIM_WIDTH + col]);\n");
    s += QStringLiteral("        }\n");
    s += QStringLiteral("    }\n");
    s += QStringLiteral("}\n\n");

    s += QStringLiteral("#endif /* __ANIM_FRAMES_H */\n");
    return s;
}
