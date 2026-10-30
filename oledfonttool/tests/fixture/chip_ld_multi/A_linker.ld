/* 只描述 RAM 的那份（CubeIDE 的 _RAM.ld，或旧版遗留）。
 * 按文件名排在最前面 —— 老实现只看第一个 .ld，于是整个链接脚本检查直接失效。 */
MEMORY
{
  RAM (xrw) : ORIGIN = 0x20000000, LENGTH = 64K
}
