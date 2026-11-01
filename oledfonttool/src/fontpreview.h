// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 zhangwtee <https://github.com/zhangwtee>
// 本文件属于「OLED 点阵取模工具」。改动请保留以上两行并注明改了什么（Apache-2.0 §4b）。
/*==============================================================================
 *  fontpreview.h  ——  点阵实时预览控件（接口）
 *
 *  作者   zhangwtee
 *  GitHub https://github.com/zhangwtee
 *  Gitee  https://gitee.com/z_w_t
 *==============================================================================*/
#ifndef FONTPREVIEW_H
#define FONTPREVIEW_H

#include <QWidget>
#include <QString>
#include <QVector>
#include <QChar>
#include "fontgen.h"

/*
 * 点阵预览：把每个字的 16x16 点阵画成缩略图网格
 * 这就是"所见即所得"的那块——调阈值、换字体，这里立刻重画
 */
class FontPreview : public QWidget
{
public:
	explicit FontPreview(QWidget *parent = nullptr);

	void setGenerator(FontGen *gen);
	void setText(const QString &text);

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;
	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int w) const override;

	int glyphCount() const { return m_chars.size(); }

protected:
	void paintEvent(QPaintEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;

private:
	int columns() const;
	int cellSize() const;

	FontGen *m_gen;
	QVector<QChar> m_chars;
	int m_gap;
};

#endif /* FONTPREVIEW_H */
