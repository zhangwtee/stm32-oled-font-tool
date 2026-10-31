/*==============================================================================
 *  selftest.h  ——  自检帧生成（接口）
 *
 *  作者   zhangwtee
 *  GitHub https://github.com/zhangwtee
 *  Gitee  https://gitee.com/z_w_t
 *==============================================================================*/
#ifndef SELFTEST_H
#define SELFTEST_H

#include <QImage>
#include <QString>
#include <QVector>
#include <QtGlobal>

class FontGen;

/*
 * 全屏自检帧。
 *
 * 一张 128x64 的整屏图，打包成和字库完全相同的显存格式（阴码 / 列行式 /
 * 低位在前），导出时附带一个 OLED_SelfTest_Show() 函数。用户把它烧进板子，
 * 整屏刷一次，就能确认"工具吐出来的字节"和"自己的驱动"是同一套格式。
 *
 * 这是唯一能端到端验收的环节：字库里的某个字对不对，肉眼很难判断；
 * 但一整屏的边框、四角、文字对不对劲，一眼就能看出来。
 *
 * 图面刻意做成四角各不相同，这样上下翻转、左右镜像、页序颠倒都会
 * 让四个角标互相串位，不用放大镜也能发现。
 */
class SelfTest
{
public:
	/* 默认尺寸（0.96" 最常见的 128x64）。buildImage/renderHeader 可传别的尺寸，
	 * 0.91" 的 128x32 屏就用 128x32，否则下半屏永远刷不到。 */
	static constexpr int W = 128;
	static constexpr int H = 64;
	static constexpr int PAGES = H / 8;
	static constexpr int SIZE = PAGES * W;      /* 1024 字节 */

	/* 支持的屏尺寸（w, h），h 必须是 8 的倍数 */
	static bool sizeOk(int w, int h) { return w >= 16 && h >= 16 && h % 8 == 0; }
	static int  bytesOf(int w, int h) { return (w / 8) * h; }

	/*
	 * 生成自检帧。
	 * sample 为空时只画几何图形（适合还没装字库、只想验证屏和驱动的情况）。
	 * gen 负责渲染 sample 里的汉字，用的是和字库同一份 glyphBits，
	 * 所以文字部分能真实反映取模结果。
	 */
	static QImage buildImage(const FontGen *gen, const QString &sample,
	                         int w = W, int h = H);

	/* 1bit 图 -> 显存字节。直接走 ImageGen，保证与字库/图片同一套打包逻辑 */
	static QVector<quint8> pack(const QImage &mono);

	/* 生成 C 头文件内容（数组 + 显示函数）
	   colOffset：SH1106（132 列）填 2，SSD1306 填 0。默认 0，导出时由界面传入。
	   w/h：屏尺寸，字节数必须等于 bytesOf(w, h)，否则返回空串。 */
	static QString renderHeader(const QVector<quint8> &bytes,
	                            const QString &sample,
	                            int charSize,
	                            const QString &fontFamily,
	                            int colOffset = 0,
	                            int w = W,
	                            int h = H);
};

#endif /* SELFTEST_H */
