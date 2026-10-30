#ifndef CHIPINFO_H
#define CHIPINFO_H

/*==============================================================================
 *  芯片 Flash 容量读取：这工具到底该按多大空间算占用
 *
 *  为什么要读
 *  ----------
 *  界面上原来写死一句「STM32F103C8T6 共 64KB Flash」。它带来两个问题：
 *
 *    1. 换个芯片就是错话。同一个工具打开 STM32F103ZE 的工程，仍然说 64KB，
 *       而实际是 512KB —— 用户会以为放不下动画，白白压缩帧数。
 *    2. 占用只剩「宽裕 / 注意 / 偏大」三个绝对阈值（8KB、32KB），
 *       和芯片多大无关。64KB 的芯片占 20KB 已经很紧，512KB 的芯片占 20KB
 *       根本不算事，但两边的提示一模一样。
 *
 *  读什么
 *  ------
 *  最准的不是芯片型号，是工程里链接器实际用的 ROM 大小。Keil 的 .uvprojx
 *  里就存着：
 *
 *      <OnChipMemories>
 *        <IROM>
 *          <Type>1</Type>
 *          <StartAddress>0x8000000</StartAddress>
 *          <Size>0x80000</Size>          <- 这个就是 512KB
 *        </IROM>
 *      </OnChipMemories>
 *
 *  它比型号解码可靠，因为：用户可以把 IROM 手动改小（比如留一半给 bootloader），
 *  这时型号还是 ZE，但可用空间已经不是 512KB 了。读 IROM 得到的是"编译器眼里
 *  真正能用的空间"，这才是判断"放不放得下"该用的数字。
 *
 *  读不到怎么办
 *  ------------
 *  按可信度往下退，每一步都记下依据，最后在 tooltip 里说清是用哪条算的：
 *
 *    1. Keil  .uvprojx / .uvproj 里的 <IROM><Size>
 *    2. CubeIDE / Makefile 工程的链接脚本：FLASH (rx) : ORIGIN = ..., LENGTH = 512K
 *    3. IAR   .icf 里的 __ICFEDIT_region_ROM_size__
 *    4. 型号解码：STM32F103ZE 的末位 E = 512KB（ST 命名规则）
 *    5. 都没有 —— 返回 0，不猜。调用方按 64KB 显示，但要明确标"没读到"。
 *
 *  型号解码的规则
 *  --------------
 *  STM32 型号形如 STM32 F 103 Z E [T6]：
 *
 *      第 5 位  Z = 引脚数（T=36 C=48 R=64 V=100 Z=144 I=176）
 *      第 6 位  E = Flash 容量：6=32K 8=64K B=128K C=256K D=384K E=512K F=768K G=1M
 *
 *  所以型号名要先剥掉后面的温度/封装码（C8T6 里的 T6），只取到第 6 位，
 *  不能简单地"取最后一个字母" —— C8T6 取到最后一位是 6，会算成 32KB。
 *============================================================================*/

#include <QString>
#include <QtGlobal>

namespace ChipInfo
{

struct Result
{
	QString device;                 /* 型号，如 STM32F103ZE；可能为空 */
	qint64  flashBytes = 0;         /* 0 = 没读到 */
	QString source;                 /* 依据：Keil IROM / 链接脚本 / 型号解码 / 未读到 */
	QString file;                   /* 依据来自哪个文件（可能为空） */
	QString detail;                 /* 多行说明，给 tooltip */

	bool ok() const { return flashBytes > 0; }
};

/* 在工程根目录下找工程文件并读出容量。projRoot 为空或目录不存在时返回未知。 */
Result read(const QString &projRoot);

/* 只做型号解码，不碰文件。给测试和"用户手填型号"用。
 * 认不出来返回 0。 */
qint64 capacityFromPartNumber(const QString &device);

/* 0x80000 -> "512 KB"；65536 -> "64 KB"；正好 1MB 显示 "1 MB" */
QString formatSize(qint64 bytes);

}   // namespace ChipInfo

#endif  // CHIPINFO_H
