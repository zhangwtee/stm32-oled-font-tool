#include "chipinfo.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

namespace
{

/*
 * 读工程文件。
 *
 * 这里**故意**不处理 GBK：需要解析的部分（<IROM> <Size> 0x80000、FLASH LENGTH）
 * 全是 ASCII，GBK 保存的工程只有中文注释会变成乱码，不影响正则匹配。
 * 多写一套编码探测只会多一个出错的地方。
 */
QString readText(const QString &path)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
	{
		return QString();
	}
	return QString::fromUtf8(f.readAll());
}

/* 只在这一层找，工程文件都在工程根上，不往子目录钻 */
QString findFirst(const QDir &dir, const QStringList &patterns)
{
	const QFileInfoList list = dir.entryInfoList(patterns, QDir::Files, QDir::Name);
	return list.isEmpty() ? QString() : list.first().absoluteFilePath();
}

/* "0x80000" / "524288" -> 数值；解析不出来返回 -1 */
qint64 parseNumber(const QString &text)
{
	const QString s = text.trimmed();
	bool ok = false;
	qint64 v = 0;
	if (s.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
	{
		v = s.mid(2).toLongLong(&ok, 16);
	}
	else
	{
		v = s.toLongLong(&ok, 10);
	}
	return ok ? v : -1;
}

/* 型号里的容量码 -> 字节。ST 的命名规则，第 6 位就是 Flash 大小。 */
qint64 capacityFromCode(QChar code)
{
	switch (code.toUpper().unicode())
	{
	case '4': return 16  * 1024;
	case '6': return 32  * 1024;
	case '8': return 64  * 1024;
	case 'B': return 128 * 1024;
	case 'C': return 256 * 1024;
	case 'D': return 384 * 1024;
	case 'E': return 512 * 1024;
	case 'F': return 768 * 1024;
	case 'G': return 1024 * 1024;
	case 'H': return 1536 * 1024;
	case 'I': return 2048 * 1024;      /* H743ZI 的 I = 2MB */
	default:  return 0;
	}
}

}   // namespace

namespace ChipInfo
{

qint64 capacityFromPartNumber(const QString &device)
{
	/*
	 * STM32 F 103 Z E [T6]
	 *       ^ ^^^ ^ ^
	 *       |  |  |  +-- 容量码（E = 512KB）
	 *       |  |  +----- 引脚数（Z = 144）
	 *       |  +-------- 子系列（103 / 030 / 0B1 / 7A3）
	 *       +----------- 系列（F / G / L / H / U / W）
	 *
	 * 子系列固定按 3 个字符收，不能写成"至少两位数字"：
	 *   老型号是纯数字（F103、G030、H743），但新型号的子系列带字母 ——
	 *   G0B1、H7A3、L4P5、U5A5 都是真实存在的型号，只认数字的话这些全解码失败，
	 *   会一路退到最后一步报"没找到工程文件"，而工程文件其实找到了。
	 *   定长 3 位也顺带避开贪婪问题：G0B1RET6 若写成 {1,3} 贪婪，会把 "0B1R"
	 *   整段吃进子系列，容量码就取成了 T（温度/封装），错得更隐蔽。
	 *
	 * 末尾的 T6（温度+封装）不能参与 —— 直接取最后一个字母的话，
	 * STM32F103C8 会取到 8（对），但 STM32F103C8T6 会取到 T（错），
	 * 所以必须按位置取第 4 个捕获组。
	 */
	static const QRegularExpression re(
		QStringLiteral("STM32([FGLHUW])([0-9A-Z]{3})([A-Z])([0-9A-Z])"),
		QRegularExpression::CaseInsensitiveOption);

	const QRegularExpressionMatch m = re.match(device.trimmed());
	if (!m.hasMatch())
	{
		return 0;
	}
	return capacityFromCode(m.captured(4).at(0));
}

QString formatSize(qint64 bytes)
{
	if (bytes <= 0)
	{
		return QStringLiteral("未知");
	}
	if (bytes >= 1024 * 1024)
	{
		return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'g', 3);
	}
	return QStringLiteral("%1 KB").arg(bytes / 1024);
}

Result read(const QString &projRoot)
{
	Result r;
	if (projRoot.isEmpty())
	{
		r.source = QStringLiteral("未读到");
		r.detail = QStringLiteral("还没确定工程目录。");
		return r;
	}

	const QDir dir(projRoot);
	if (!dir.exists())
	{
		r.source = QStringLiteral("未读到");
		r.detail = QStringLiteral("目录不存在：%1").arg(projRoot);
		return r;
	}

	/* ---- 1. Keil：<IROM><Size> 是链接器真正用的 ROM 大小，最可信 ---- */
	/*
	 * 一个目录里可能躺着好几个工程文件（Bootloader + App 两个目标、
	 * 升级前后各留一份）。以前只取按名字排在最前的那个：赶上它没写
	 * <IROM>（新建工程、旧 Keil 保存）就整段放弃，直接往下退 ——
	 * 明明同目录另一个工程写得清清楚楚。
	 * 现在逐个试，谁有 IROM 用谁；都没有才拿第一个能读出型号的兜底。
	 */
	static const QRegularExpression reDev(QStringLiteral("<Device>\\s*([^<\\s]+)\\s*</Device>"));
	/* 一块 <IROM> 一个 ROM 区；多目标工程会出现多块，取第一块有 Size 的 */
	static const QRegularExpression reBlock(
		QStringLiteral("<IROM>[\\s\\S]*?</IROM>"),
		QRegularExpression::CaseInsensitiveOption);
	static const QRegularExpression reSize(
		QStringLiteral("<Size>\\s*(0[xX][0-9A-Fa-f]+|[0-9]+)\\s*</Size>"),
		QRegularExpression::CaseInsensitiveOption);

	const QFileInfoList uvFiles = dir.entryInfoList(
		QStringList() << QStringLiteral("*.uvprojx") << QStringLiteral("*.uvproj"),
		QDir::Files, QDir::Name);

	QString deviceFile;      /* 最后兜底：第一个能读出型号的工程 */
	QString deviceName;

	for (const QFileInfo &fi : uvFiles)
	{
		const QString uvproj = fi.absoluteFilePath();
		const QString text   = readText(uvproj);

		const QRegularExpressionMatch md = reDev.match(text);
		const QString device = md.hasMatch() ? md.captured(1).trimmed() : QString();
		if (deviceFile.isEmpty() && !device.isEmpty())
		{
			deviceFile = uvproj;
			deviceName = device;
		}

		QRegularExpressionMatchIterator it = reBlock.globalMatch(text);
		while (it.hasNext())
		{
			const QRegularExpressionMatch block = it.next();
			const QRegularExpressionMatch ms = reSize.match(block.captured(0));
			if (!ms.hasMatch())
			{
				continue;
			}
			const qint64 v = parseNumber(ms.captured(1));
			if (v > 0)
			{
				r.device     = device;
				r.flashBytes = v;
				r.source     = QStringLiteral("Keil 工程 IROM");
				r.file       = uvproj;
				r.detail     = QStringLiteral("%1\n<IROM><Size> = %2（%3）")
				                   .arg(uvproj)
				                   .arg(ms.captured(1))
				                   .arg(formatSize(v));
				return r;
			}
		}
	}

	/* 工程里有型号但没 IROM（新建工程时常见）：退回型号解码 */
	if (!deviceFile.isEmpty())
	{
		const qint64 v = capacityFromPartNumber(deviceName);
		if (v > 0)
		{
			r.device     = deviceName;
			r.flashBytes = v;
			r.source     = QStringLiteral("型号解码");
			r.file       = deviceFile;
			r.detail     = QStringLiteral("%1\n工程里没写 <IROM><Size>，按型号 %2 的容量码推算：%3")
			                   .arg(deviceFile)
			                   .arg(deviceName)
			                   .arg(formatSize(v));
			return r;
		}
	}

	/* ---- 2. CubeIDE / Makefile：链接脚本里的 FLASH 区长度 ---- */
	/*
	 * 同样可能有多个 .ld：CubeIDE 生成 _FLASH.ld + _RAM.ld，有的工程还留着
	 * 旧版链接脚本，其中只描述 RAM。排在前面的那个可能根本没有 FLASH 区，
	 * 所以不能只看第一个文件，要逐个找"带 FLASH 且长度大于 0"的。
	 *
	 * LENGTH 可能写成十进制、带 K/M 单位，也可能直接写十六进制
	 * （0x80000）。原来只认十进制，遇到十六进制会匹配到开头的 "0"，
	 * 算出 0 又不报错，等于悄悄忽略了一个完全合格的链接脚本。
	 */
	static const QRegularExpression reLd(
		QStringLiteral("FLASH\\s*\\([^)]*\\)\\s*:\\s*ORIGIN\\s*=\\s*0[xX][0-9A-Fa-f]+\\s*,"
		               "\\s*LENGTH\\s*=\\s*(0[xX][0-9A-Fa-f]+|[0-9]+)\\s*([KMG])?"),
		QRegularExpression::CaseInsensitiveOption);

	const QFileInfoList ldFiles = dir.entryInfoList(QStringList() << QStringLiteral("*.ld"),
	                                                QDir::Files, QDir::Name);
	for (const QFileInfo &fi : ldFiles)
	{
		const QString ld = fi.absoluteFilePath();
		const QRegularExpressionMatch m = reLd.match(readText(ld));
		if (!m.hasMatch())
		{
			continue;
		}

		qint64 v = parseNumber(m.captured(1));
		const QString unit = m.captured(2).toUpper();
		if (unit == QLatin1String("K"))
		{
			v *= 1024;
		}
		else if (unit == QLatin1String("M"))
		{
			v *= 1024 * 1024;
		}
		else if (unit == QLatin1String("G"))
		{
			v *= 1024LL * 1024 * 1024;
		}
		if (v > 0)
		{
			r.flashBytes = v;
			r.source     = QStringLiteral("链接脚本 FLASH");
			r.file       = ld;
			r.detail     = QStringLiteral("%1\nFLASH 区 LENGTH = %2%3")
			                   .arg(ld)
			                   .arg(m.captured(1))
			                   .arg(m.captured(2));
			return r;
		}
	}

	/* ---- 3. IAR：.icf 里的 ROM 区大小 ---- */
	const QString icf = findFirst(dir, QStringList() << QStringLiteral("*.icf"));
	if (!icf.isEmpty())
	{
		const QString text = readText(icf);
		static const QRegularExpression reIcf(
			QStringLiteral("__ICFEDIT_region_ROM_size__\\s*=\\s*(0[xX][0-9A-Fa-f]+|[0-9]+)"));
		const QRegularExpressionMatch m = reIcf.match(text);
		if (m.hasMatch())
		{
			const qint64 v = parseNumber(m.captured(1));
			if (v > 0)
			{
				r.flashBytes = v;
				r.source     = QStringLiteral("IAR 链接配置");
				r.file       = icf;
				r.detail     = QStringLiteral("%1\n__ICFEDIT_region_ROM_size__ = %2")
				                   .arg(icf)
				                   .arg(m.captured(1));
				return r;
			}
		}
	}

	/* ---- 4. IAR 工程文件里的型号 ---- */
	const QString ewp = findFirst(dir, QStringList() << QStringLiteral("*.ewp"));
	if (!ewp.isEmpty())
	{
		const QString text = readText(ewp);
		static const QRegularExpression reEwp(
			QStringLiteral("(STM32[FGLHUW][0-9]{2,4}[A-Z][0-9A-Z])"),
			QRegularExpression::CaseInsensitiveOption);
		const QRegularExpressionMatch m = reEwp.match(text);
		if (m.hasMatch())
		{
			r.device = m.captured(1).toUpper();
			const qint64 v = capacityFromPartNumber(r.device);
			if (v > 0)
			{
				r.flashBytes = v;
				r.source     = QStringLiteral("IAR 工程型号");
				r.file       = ewp;
				r.detail     = QStringLiteral("%1\n工程里的型号 %2 → %3")
				                   .arg(ewp)
				                   .arg(r.device)
				                   .arg(formatSize(v));
				return r;
			}
		}
	}

	/* ---- 5. 都没读到：不猜 ---- */
	r.source = QStringLiteral("未读到");
	r.detail = QStringLiteral("在 %1 下没找到能读出 Flash 容量的工程文件"
	                          "（.uvprojx / .ld / .icf）。").arg(projRoot);
	return r;
}

}   // namespace ChipInfo
