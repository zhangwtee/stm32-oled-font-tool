// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 zhangwtee <https://github.com/zhangwtee>
// 本文件属于「OLED 点阵取模工具」。改动请保留以上两行并注明改了什么（Apache-2.0 §4b）。
/* ScreenProbe 的回归测试。
 *
 * 每种驱动写法都有坑：大小写、注释里的假线索、GBK 编码、只在注释里写型号。
 * 这些都不是造出来的极端情况 —— 都是真实例程里的样子。
 *
 * 构建运行见 tests/run_test.cmd
 */
#include "../src/screenprobe.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <cstdio>

static int g_pass = 0;
static int g_fail = 0;

static void check(const char *what, bool ok, const QString &got)
{
	if (ok)
	{
		++g_pass;
		std::printf("  ok    %s\n", what);
	}
	else
	{
		++g_fail;
		std::printf("  FAIL  %s   -> %s\n", what, got.toLocal8Bit().constData());
	}
}

static void expectFile(const QString &dir, const char *name,
                       ScreenProbe::Controller want)
{
	const QString path = QDir(dir).absoluteFilePath(QString::fromLatin1(name));
	const ScreenProbe::Result r = ScreenProbe::probe(path);
	const QString got = QStringLiteral("%1 (offset %2) [%3]")
	                        .arg(ScreenProbe::nameOf(r.controller))
	                        .arg(r.colOffset)
	                        .arg(r.evidenceOneLine);
	check(name, r.controller == want, got);

	if (want != ScreenProbe::Unknown && r.colOffset != ScreenProbe::offsetOf(want))
	{
		check("  offset", false, QStringLiteral("offset = %1").arg(r.colOffset));
	}
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);

	const QString dir = (argc > 1) ? QString::fromLocal8Bit(argv[1])
	                              : QDir(QCoreApplication::applicationDirPath())
	                                    .absoluteFilePath(QStringLiteral("fixture"));

	std::printf("fixture: %s\n", dir.toLocal8Bit().constData());

	std::printf("[识别各种驱动写法]\n");
	expectFile(dir, "ssd1306_zjy.c",   ScreenProbe::SSD1306);
	expectFile(dir, "sh1106_zjy.c",    ScreenProbe::SH1106);
	expectFile(dir, "ssd1306_atom.c",  ScreenProbe::SSD1306);
	expectFile(dir, "sh1106_note.c",   ScreenProbe::SH1106);
	expectFile(dir, "gbk_sh1106.c",    ScreenProbe::SH1106);

	/* 注释里同时写「兼容 SSD1306 / SH1106」是很常见的做法。
	 * 老版扫到这个文件时先看到 SH1106 就定案了，SSD1306 的屏会被判成 SH1106，
	 * 列偏移变成 2，整幅画面往左错两像素。这份 fixture 两种型号都提到，
	 * 但代码用的是 SSD1306 的列地址高低 4 位写法，所以应该按代码特征判 SSD1306。 */
	std::printf("[两种型号都写到的驱动]\n");
	expectFile(dir, "both_models.c",   ScreenProbe::SSD1306);

	/* 注释里的 0x02 和 +2 不能当证据，否则这是最典型的误判 */
	std::printf("[注释里的假线索不能上当]\n");
	expectFile(dir, "comment_trap.c",  ScreenProbe::SSD1306);

	std::printf("[扫整个目录]\n");
	{
		/* 目录里两种驱动都有，谁分多家；只要不崩、给出一个确定结论即可 */
		const ScreenProbe::Result r = ScreenProbe::probe(dir);
		check("dir probe 有结论", r.controller != ScreenProbe::Unknown,
		      r.evidenceOneLine);
	}

	std::printf("[空目录 / 不存在的路径]\n");
	{
		const ScreenProbe::Result e = ScreenProbe::probe(
			QDir::temp().absoluteFilePath(QStringLiteral("__no_such_dir_9527__")));
		check("不存在 -> 不崩且有提示", !e.error.isEmpty(), e.evidenceOneLine);
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
