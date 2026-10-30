/* ChipInfo 的回归测试。
 *
 * 重点是两件事：
 *   1. 真实工程的 IROM 要读对 —— C8T6c 是 64KB，ZET6k 是 512KB。
 *      这条如果错了，界面上就是在说假话，比不显示更糟。
 *   2. 兜底路径不能算错 —— 型号解码最容易错在「取最后一个字母」：
 *      STM32F103C8T6 末位是 6，按位取第 6 位才是 8（64KB）。
 *
 * 构建运行见 build_test.cmd / CMakeLists.txt
 */
#include "../src/chipinfo.h"

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

/* 型号 -> 期望容量（KB），0 表示认不出来 */
static void expectPart(const char *part, qint64 wantKb)
{
	const QString dev = QString::fromLatin1(part);
	const qint64 got = ChipInfo::capacityFromPartNumber(dev);
	const qint64 want = wantKb * 1024;
	check(part, got == want,
	      QStringLiteral("got %1 bytes, want %2").arg(got).arg(want));
}

/* 目录 -> 期望容量（KB），0 表示应该读不到 */
static void expectDir(const QString &dir, qint64 wantKb, const char *what)
{
	const ChipInfo::Result r = ChipInfo::read(dir);
	const qint64 want = wantKb * 1024;
	check(what, r.flashBytes == want,
	      QStringLiteral("got %1 (%2), want %3 KB, detail=%4")
	          .arg(r.flashBytes).arg(r.source).arg(wantKb).arg(r.detail));
}

int main(int argc, char *argv[])
{
	QCoreApplication app(argc, argv);

	const QString fixture = (argc > 1)
	        ? QDir(QString::fromLocal8Bit(argv[1])).absolutePath()
	        : QDir::currentPath();

	std::printf("[型号解码]\n");
	expectPart("STM32F103C8",     64);      /* 中景园最小板 */
	expectPart("STM32F103C8T6",   64);      /* 带后缀，不能算成 32KB */
	expectPart("STM32F103RB",    128);
	expectPart("STM32F103ZE",    512);      /* ZET6k */
	expectPart("STM32F103VET6",  512);
	expectPart("STM32F407ZGT6", 1024);
	expectPart("STM32G030C8T6",   64);      /* 4 位数字子系列 */
	expectPart("STM32H743ZIT6", 2048);      /* I = 2MB */
	expectPart("GD32F103C8",       0);      /* 非 ST，不硬猜 */
	expectPart("",                  0);

	std::printf("[真实工程]\n");
	{
		const QString c8 = QDir(fixture).absoluteFilePath(QStringLiteral("../../../MyFreeRTOS_C8T6c"));
		if (QFileInfo::exists(c8))
		{
			expectDir(c8, 64, "C8T6c 工程 -> 64KB");
			const ChipInfo::Result r = ChipInfo::read(c8);
			check("C8T6c 型号读出来是 STM32F103C8",
			      r.device == QLatin1String("STM32F103C8"),
			      r.device);
		}
		else
		{
			std::printf("  skip  找不到 C8T6c 工程（%s）\n", c8.toLocal8Bit().constData());
		}

		const QString ze = QDir(fixture).absoluteFilePath(QStringLiteral("../../../MyFreeRTOS_ZET6k"));
		if (QFileInfo::exists(ze))
		{
			expectDir(ze, 512, "ZET6k 工程 -> 512KB");
			const ChipInfo::Result r = ChipInfo::read(ze);
			check("ZET6k 型号读出来是 STM32F103ZE",
			      r.device == QLatin1String("STM32F103ZE"),
			      r.device);
		}
		else
		{
			std::printf("  skip  找不到 ZET6k 工程（%s）\n", ze.toLocal8Bit().constData());
		}
	}

	std::printf("[兜底路径]\n");
	expectDir(QDir(fixture).absoluteFilePath(QStringLiteral("chip_noirom")), 512,
	          "只有 Device 没 IROM -> 型号解码 512KB");
	{
		const ChipInfo::Result r = ChipInfo::read(
			QDir(fixture).absoluteFilePath(QStringLiteral("chip_noirom")));
		check("兜底时 source 标明是型号解码",
		      r.source.contains(QStringLiteral("型号")),
		      r.source);
	}
	expectDir(QDir(fixture).absoluteFilePath(QStringLiteral("chip_ld")), 512,
	          "链接脚本 FLASH LENGTH = 512K");
	expectDir(QDir(fixture).absoluteFilePath(QStringLiteral("chip_icf")), 64,
	          "IAR .icf 的 ROM_size = 0x10000");

	std::printf("[读不到]\n");
	expectDir(QDir(fixture).absoluteFilePath(QStringLiteral("chip_empty")), 0,
	          "空目录 -> 不猜，返回 0");
	expectDir(QString(), 0, "空路径 -> 返回 0");
	expectDir(QStringLiteral("Z:/不存在的目录"), 0, "不存在的目录 -> 返回 0");

	std::printf("[显示格式]\n");
	check("65536 -> 64 KB", ChipInfo::formatSize(65536) == QLatin1String("64 KB"),
	      ChipInfo::formatSize(65536));
	check("524288 -> 512 KB", ChipInfo::formatSize(524288) == QLatin1String("512 KB"),
	      ChipInfo::formatSize(524288));
	check("1048576 -> 1 MB", ChipInfo::formatSize(1048576) == QLatin1String("1 MB"),
	      ChipInfo::formatSize(1048576));
	check("0 -> 未知", ChipInfo::formatSize(0) == QStringLiteral("未知"),
	      ChipInfo::formatSize(0));

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
