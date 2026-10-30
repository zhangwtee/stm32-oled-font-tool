/* 链接脚本：LENGTH 直接写十六进制（STM32CubeIDE 部分版本、手写脚本都这么写） */
MEMORY
{
  RAM   (xrw) : ORIGIN = 0x20000000,   LENGTH = 64K
  FLASH (rx)  : ORIGIN = 0x08000000,   LENGTH = 0x40000
}
_estack = ORIGIN(RAM) + LENGTH(RAM);
