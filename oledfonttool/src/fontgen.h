#ifndef FONTGEN_H
#define FONTGEN_H

#include <QString>
#include <QStringList>
#include <QVector>
#include <QMap>
#include <QFont>

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

	QFont font() const { return m_font; }
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
	              页地址里灌数据。 */
	QString generateHeader(const QString &text, QString *errorOut = nullptr,
	                       int colOffset = 0, int screenH = 64) const;

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

	QFont m_font;
	int m_threshold;
	int m_size;
	mutable QMap<QChar, QVector<QVector<bool>>> m_cache;   /* const 成员函数里也能写缓存 */
};

#endif /* FONTGEN_H */
