# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 zhangwtee <https://github.com/zhangwtee>
# 本文件属于「OLED 点阵取模工具」。改动请保留以上两行并注明改了什么（Apache-2.0 §4b）。
# Creates the BUILD.lnk shortcut that builds the OLED font tool.
#
# ASCII only: Windows PowerShell 5.1 reads BOM-less UTF-8 files as the system
# ANSI codepage (GBK on this machine), which would mangle any non-ASCII text
# here -- including filenames it generates.
#
# Layout assumption:
#   oledfonttool\scripts\build.ps1   <- the real build script
#   oledfonttool\BUILD.lnk           <- this shortcut, in the parent folder

$here   = $PSScriptRoot                       # ...\oledfonttool\scripts
$root   = Split-Path $here -Parent           # ...\oledfonttool
$lnk    = Join-Path $root 'BUILD.lnk'
$ps     = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$script = Join-Path $here 'build.ps1'

if (-not (Test-Path $ps))     { throw "powershell.exe not found: $ps" }
if (-not (Test-Path $script)) { throw "build.ps1 not found: $script" }

$shell = New-Object -ComObject WScript.Shell
$sc = $shell.CreateShortcut($lnk)
$sc.TargetPath       = $ps
$sc.Arguments        = '-NoProfile -ExecutionPolicy Bypass -File "' + $script + '" -Wait'
$sc.WorkingDirectory = $here
$sc.Description      = 'Build oledfonttool'
$sc.IconLocation     = "$ps,0"
$sc.Save()

Write-Host "Shortcut written : $lnk"
Write-Host "  target         : $ps"
Write-Host "  arguments      : $($sc.Arguments)"
