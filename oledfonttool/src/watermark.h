// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 zhangwtee <https://github.com/zhangwtee>
// 本文件属于「OLED 点阵取模工具」。改动请保留以上两行并注明改了什么（Apache-2.0 §4b）。
/*==============================================================================
 *  watermark.h  ——  导出文件里那段出处标记
 *
 *  作者   zhangwtee
 *  GitHub https://github.com/zhangwtee
 *  Gitee  https://gitee.com/z_w_t
 *==============================================================================*/
#ifndef WATERMARK_H
#define WATERMARK_H

#include <QString>

/* 导出的字库 / 单图 / 素材包，文件头都带这一段。
 *
 * 为什么要有：别人真正拿走的是字模，不是这个工具；字模离开工具之后就
 * 跟作者没关系了。把出处写进文件本身，他再扩散出去名字跟着走 ——
 * 改工具源码也去不掉已经发出去的那批字库。
 *
 * 故意不做成可选项：能一键关掉就等于没有。
 *
 * 改这段文本会同时影响三种导出，fixture 里的三份金样要一起更新
 * （header_golden_single.h / header_golden_ident.h / header_golden_dedup.h）。 */
namespace Watermark
{
inline QString text()
{
	return QStringLiteral(
		"/*  本文件由「OLED 点阵取模工具」生成\n"
		" *  https://github.com/zhangwtee/stm32-oled-font-tool\n"
		" *  字模数据可自由用于任何工程；请保留本段出处。 */\n"
		"\n");
}

/* 生成物判定用的特征串。除了各家的 include guard，再加这一条：
 * 以后 guard 改了名也不会把自家产物当成别人的驱动扫。
 *
 * 只能取 ASCII 那一截：探测那边按 Latin-1 逐字节读文件（为了不猜 GBK/UTF-8），
 * 中文水印进去就是乱码，比不中。URL 里的仓库名正好是独一份的 ASCII 标识。 */
inline QString marker()
{
	return QStringLiteral("stm32-oled-font-tool");
}
}

#endif // WATERMARK_H
