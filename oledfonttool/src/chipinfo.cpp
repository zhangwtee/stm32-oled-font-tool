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
	 *       |  +-------- 子系列（103）
	 *       +----------- 系列（F / G / L / H / U / W）
	 *
	 * 数字段写 {2,4}：F1/F4/H7 是 3 位（103、407、743），G0/G4 是 4 位（030、431）。
	 * 末尾的 T6（温度+封装）不能参与 —— 直接取最后一个字母的话，
	 * STM32F103C8 会取到 8（对），但 STM32F103C8T6 会取到 T（错），
	 * 所以必须按位置取第 4 个捕获组。
	 */
	static const QRegularExpression re(
		QStringLiteral("STM32([FGLHUW])([0-9]{2,4})([A-Z])([0-9A-Z])"),
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
	const QString uvproj = findFirst(dir, QStringList() << QStringLiteral("*.uvprojx")
	                                                   << QStringLiteral("*.uvproj"));
	if (!uvproj.isEmpty())
	{
		const QString text = readText(uvproj);

		const QRegularExpression reDev(QStringLiteral("<Device>\\s*([^<\\s]+)\\s*</Device>"));
		const QRegularExpressionMatch md = reDev.match(text);
		if (md.hasMatch())
		{
			r.device = md.captured(1).trimmed();
		}

		/* 一块 <IROM> 一个 ROM 区；多目标工程会出现多块，取第一块有 Size 的 */
		static const QRegularExpression reBlock(
			QStringLiteral("<IROM>[\\s\\S]*?</IROM>"),
			QRegularExpression::CaseInsensitiveOption);
		static const QRegularExpression reSize(
			QStringLiteral("<Size>\\s*(0[xX][0-9A-Fa-f]+|[0-9]+)\\s*</Size>"),
			QRegularExpression::CaseInsensitiveOption);

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

		/* 工程里有型号但没 IROM（新建工程时常见）：退回型号解码 */
		if (!r.device.isEmpty())
		{
			const qint64 v = capacityFromPartNumber(r.device);
			if (v > 0)
			{
				r.flashBytes = v;
				r.source     = QStringLiteral("型号解码");
				r.file       = uvproj;
				r.detail     = QStringLiteral("%1\n工程里没写 <IROM><Size>，按型号 %2 的容量码推算：%3")
				                   .arg(uvproj)
				                   .arg(r.device)
				                   .arg(formatSize(v));
				return r;
			}
		}
	}

	/* ---- 2. CubeIDE / Makefile：链接脚本里的 FLASH 区长度 ---- */
	const QString ld = findFirst(dir, QStringList() << QStringLiteral("*.ld"));
	if (!ld.isEmpty())
	{
		const QString text = readText(ld);
		static const QRegularExpression reLd(
			QStringLiteral("FLASH\\s*\\([^)]*\\)\\s*:\\s*ORIGIN\\s*=\\s*0[xX][0-9A-Fa-f]+\\s*,"
			               "\\s*LENGTH\\s*=\\s*([0-9]+)\\s*([KMG])?"),
			QRegularExpression::CaseInsensitiveOption);
		const QRegularExpressionMatch m = reLd.match(text);
		if (m.hasMatch())
		{
			qint64 v = m.captured(1).toLongLong();
			const QString unit = m.captured(2).toUpper();
			if (unit == QLatin1String("K"))
			{
				v *= 1024;
			}
			else if (unit == QLatin1String("M"))
			{
				v *= 1024 * 1024;
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
