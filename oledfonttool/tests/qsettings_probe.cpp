// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 zhangwtee <https://github.com/zhangwtee>
// 本文件属于「OLED 点阵取模工具」。改动请保留以上两行并注明改了什么（Apache-2.0 §4b）。
/* 一次性诊断：模拟主程序 main() 的设置初始化方式（只设 applicationName，
 * 不设 organizationName），看 QSettings 到底把值存到哪、写不写得进去。
 * 自己写入、自己删掉，跑完不留痕迹。 */
#include <QCoreApplication>
#include <QDebug>
#include <QSettings>

int main(int argc, char *argv[])
{
	QCoreApplication app(argc, argv);
	app.setApplicationName(QStringLiteral("OLED 点阵取模工具"));

	qDebug() << "org     :" << QCoreApplication::organizationName();
	qDebug() << "app     :" << QCoreApplication::applicationName();
	qDebug() << "fileName:" << QSettings().fileName();

	{
		QSettings s;
		qDebug() << "初始 status:" << int(s.status());
		s.setValue(QStringLiteral("zz_probe/key"), QStringLiteral("hello"));
		s.sync();
		qDebug() << "写入后 status:" << int(s.status()) << "(0=NoError 1=AccessError 2=FormatError)";
		qDebug() << "读回:" << QSettings().value(QStringLiteral("zz_probe/key"));
		qDebug() << "现有键:" << QSettings().allKeys();

		/* 清理：只删自己写进去的这一组 */
		QSettings().remove(QStringLiteral("zz_probe"));
		QSettings().sync();
		qDebug() << "清理后键:" << QSettings().allKeys();
	}
	return 0;
}
