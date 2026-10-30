/* 链接脚本样例：CubeIDE 生成的样子 */
ENTRY(Reset_Handler)

MEMORY
{
  RAM   (xrw) : ORIGIN = 0x20000000,   LENGTH = 64K
  FLASH (rx)  : ORIGIN = 0x8000000,    LENGTH = 512K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
