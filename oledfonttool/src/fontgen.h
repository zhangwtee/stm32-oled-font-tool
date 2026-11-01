/*==============================================================================
 *  fontgen.h  ——  汉字点阵渲染与编码（接口）
 *
 *  作者   zhangwtee
 *  GitHub https://github.com/zhangwtee
 *  Gitee  https://gitee.com/z_w_t
 *==============================================================================*/
#ifndef FONTGEN_H
#define FONTGEN_H

#include <QString>
#include <QStringList>
#include <QVector>
#include <QMap>
#include <QFont>

#include "libprobe.h"

/*
 * 把汉字渲染成 N×N 点阵，并编码成 OLED 驱动要的字节格式。
 *
 * 编码格式（和同目录的 gen_oled_hz.py、生成的 OLED_CFont.h 完全一致）：
 *   阴码 / 列行式 / 逆向(低位在前)
 *   一个汉字 = 每页 N 字节 × (N/8) 页
 *   16x16 -> 32 字节，24x24 -> 72 字节，32x32 -> 128 字节
 *
 * 字号必须是 8 的倍数：编码按"页"走，一页 8 行，
 * 12x12 那种（GB2312 常见小字）除不尽，落不进字节里，所以不接受。
 *
 * 这里的算法必须和 Python 脚本逐字节一致，改一边就要同步改另一边。
 */
class FontGen
{
public:
	static const int DEFAULT_SIZE = 16;

	static bool isSupportedSize(int size)
	{
		return size >= 8 && size <= 64 && (size % 8) == 0;
	}

	/* 一个汉字占多少字节 */
	static int bytesFor(int size)
	{
		return isSupportedSize(size) ? (size / 8) * size : 0;
	}

	/* 字库数组名：16 -> OLED_CF16x16，24 -> OLED_CF24x24 */
	static QString tableName(int size);

	explicit FontGen(const QFont &font = QFont(), int threshold = 110,
	                 int size = DEFAULT_SIZE);

	void setFont(const QFont &font);
	void setThreshold(int threshold);
	void setSize(int size);          /* 换字号：换字体像素大小 + 清缓存 */

	/* 取反（反白）。默认关。
	 *
	 * 打开后每个点对调：原来亮的变不亮、不亮的变亮，屏上就是「亮底暗字」。
	 * 作用在最后成形的点阵上，所以字的形状、粗细、位置、大小都不变，
	 * 只是黑白互换 —— 想在屏上做反白提示（选中行、标题条）时用这个。
	 *
	 * 注意两点：
	 *   1) 空格和缺字那格会从「全灭」变成「全亮」。这是有意的：反白时
	 *      一整行本来就该连成一片亮底，中间豁一个洞反而更怪。
	 *   2) ASCII 不受影响。字母数字走驱动自带的 8×16 字库（CF_SHOW_ASCII），
	 *      压根不进这份数据，所以同一行里汉字反白、字母不反白。
	 *      要整行统一就只用汉字，或者自己在驱动里对 ASCII 取反。 */
	void setInvert(bool on);
	bool invert() const { return m_invert; }

	QFont font() const { return m_font; }

	/* 画汉字实际用的那套字体。
	 *
	 * 选中的字体可能压根没有汉字字形：西文字体（Arial / Consolas）是这样，
	 * Windows 上还有一批「家族名其实是样式名」的，比如
	 * "Bahnschrift SemiBold Condensed"、"Segoe UI Semilight"。
	 * 这两种情况 Qt 都会自己找别的字体顶上，而且【把字重一起带过去】——
	 * 上面那个 SemiBold 带过去以后，汉字是用「宋体 加粗」画的，
	 * 16×16 下笔画全粘成一片，取出来的就是糊字。
	 *
	 * 所以这里不让 Qt 自己挑：主动检测 -> 明确指定一款含汉字的字体
	 * -> 字重压回 Normal。界面上再把这件事告诉用户（见 MainWindow::checkFontExists）。 */
	QFont cjkFont() const { return m_cjkFont; }
	QString cjkFamily() const { return m_cjkFont.family(); }
	bool cjkSubstituted() const { return m_cjkSubstituted; }

	/* 汉字是否要走"无抗锯齿"渲染。
	 *
	 * 有些字体在小字号下自带点阵位图（宋体 16×16 就是），Qt 直接取位图，
	 * 笔画是整像素的，干净。其余字体（华文宋体、微软雅黑、黑体，以及宋体的
	 * 24/32 号）都是拿轮廓去缩放，笔画只有 0.7 像素宽，抗锯齿之后边缘全是
	 * 半灰 —— 阈值切在哪边，笔画就忽粗忽细，看上去就是糊的。
	 *
	 * 这种情况关掉抗锯齿让 Qt 按整像素出图，笔画才连得起来。
	 * 见 FontGen::detectCjkNoAa。 */
	bool cjkNoAa() const { return m_cjkNoAa; }

	int threshold() const { return m_threshold; }
	int size() const { return m_size; }
	int glyphSize() const { return bytesFor(m_size); }   /* 一个字多少字节 */

	/* 取一个字的点阵；调过阈值/字体/字号后会自动重算 */
	QVector<QVector<bool>> glyphBits(QChar ch) const;

	/* 一个字的点阵字节 */
	QVector<quint8> glyphBytes(QChar ch) const;

	/* 整个字符串的 C 头文件内容。
	   colOffset：SH1106（132 列）填 2，SSD1306 填 0。默认 0，导出时由界面传入。
	   screenH  ：屏的像素高度（64 / 32）。写进 CF_SCREEN_H，显示函数用它判断
	              第几行还放得下 —— 128×32 的屏只有 4 页，按 64 写会往不存在的
	              页地址里灌数据。
	   adapter  ：适配区那四个宏填什么（写命令 / 写数据 / 字符 / 延时）。
	              由 LibProbe 按工程源码算出来；不传就是老默认值，
	              所以老的调用点行为完全不变。 */
	QString generateHeader(const QString &text, QString *errorOut = nullptr,
	                       int colOffset = 0, int screenH = 64,
	                       const LibProbe::CodeAdapter &adapter = LibProbe::CodeAdapter()) const;

	/* 统计 */
	static int glyphCount(const QString &text);
	static qint64 flashBytes(int glyphCount, int size, bool withAsciiFont = true);

	/* 从已有的头文件里把汉字编码读回来（导入用） */
	static QString parseHeader(const QString &headerContent);

	/* 把一段文本写成 UTF-8 文件，成功返回空串，失败返回原因。
	 *
	 * 为什么不能只判断 open()：磁盘满、U 盘拔了、网络盘断开、杀软拦下写入
	 * 这些情况，open 是成功的，write() 也可能"成功"（写进缓冲），
	 * 真正的失败在 close() 落盘那一步才出现。不查这一次，
	 * 界面就会弹"导出成功"，而用户手上的文件是空的或者缺了半截。 */
	static QString writeTextFile(const QString &path, const QString &text);

private:
	QVector<QVector<bool>> renderBits(QChar ch) const;

	/* 按当前字体/字号重新判断"汉字该用哪套字体画" */
	void resolveCjkFont();

	/* 判断汉字要不要走无抗锯齿渲染（这个字体在这个字号下有没有点阵位图） */
	void detectCjkNoAa();

	QFont m_font;
	QFont m_cjkFont;                 /* 画汉字用的；m_font 本身有汉字时就是它自己 */
	bool  m_cjkSubstituted = false;  /* true = 选了字体但没有汉字，已换成别的 */
	bool  m_cjkNoAa = false;         /* true = 汉字按整像素渲染，不走抗锯齿 */
	bool  m_invert = false;          /* true = 点阵取反，屏上反白 */
	int m_threshold;
	int m_size;
	mutable QMap<QChar, QVector<QVector<bool>>> m_cache;   /* const 成员函数里也能写缓存 */
};

#endif /* FONTGEN_H */
