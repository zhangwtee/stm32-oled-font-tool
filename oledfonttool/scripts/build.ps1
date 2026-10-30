# OLED 汉字取模工具 —— 构建脚本
#
# 双击本文件即可构建（.ps1 的文件关联是好的，不像 .bat/.cmd 在你机器上被改坏了）。
# 也可以在命令行里跑： powershell -ExecutionPolicy Bypass -File build.ps1

param(
    # 构建结束后等回车再退出；双击时会自动打开，命令行调用时默认关闭
    [switch]$Wait,

    # 构建类型：Release（默认，体积小、适合分发）或 Debug（带调试信息、便于调试）
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release'
)

# ---------------------------------------------------------------------------
# BOM 自检自愈：Windows PowerShell 5.1 读取没有 BOM 的 UTF-8 文件时，会按系统
# ANSI 代码页(这里是 GBK)解释，中文注释被解成乱码后会凑出非法语法，脚本连
# 解析都过不去，表现就是"窗口一闪而过"。编辑器保存时经常把 BOM 丢掉，
# 所以这里每次启动都检查一遍。
#
# 注意：param() 必须是脚本里第一条语句，所以这段只能放在它后面。
# ---------------------------------------------------------------------------
$__me = $PSCommandPath
if ($__me -and (Test-Path -LiteralPath $__me)) {
    $__bytes = [System.IO.File]::ReadAllBytes($__me)
    $__hasBom = ($__bytes.Length -ge 3 -and $__bytes[0] -eq 0xEF -and
                 $__bytes[1] -eq 0xBB -and $__bytes[2] -eq 0xBF)
    if (-not $__hasBom) {
        Write-Host '正在给本脚本补 UTF-8 BOM（Windows PowerShell 5.1 需要）...'
        $__bom = [byte[]](0xEF, 0xBB, 0xBF)
        $__out = New-Object byte[] ($__bom.Length + $__bytes.Length)
        [System.Array]::Copy($__bom, 0, $__out, 0, $__bom.Length)
        [System.Array]::Copy($__bytes, 0, $__out, $__bom.Length, $__bytes.Length)
        [System.IO.File]::WriteAllBytes($__me, $__out)
        Write-Host '已补上，正在重新启动脚本...'
        & (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') `
            -NoProfile -ExecutionPolicy Bypass -File $__me
        exit $LASTEXITCODE
    }
}

$ErrorActionPreference = 'Stop'

# 控制台切到 UTF-8。
# 默认情况下 PowerShell 按系统 ANSI 代码页（中文系统是 GBK，代码页 936）输出，
# 而工程路径里的某些汉字（例如"课" U+8BFE）在 GBK 里编码不了，
# 会被替换成 '?'，后面的字节就跟着错位，打印出来变成
# "G:/stm32璇惧爞瀹為獙" 这种乱码。切到 65001 就不会了。
try {
    [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
    $OutputEncoding = New-Object System.Text.UTF8Encoding($false)
    & chcp.com 65001 > $null 2>&1
} catch {
    # 改不了就算了，不能让构建因为编码问题失败
}

# 目录约定：
#   本脚本在 oledfonttool\scripts\
#   源码在   oledfonttool\src\      （含 CMakeLists.txt）
#   产物在   oledfonttool\build\
$root   = Split-Path $PSScriptRoot -Parent      # ...\oledfonttool
$src    = Join-Path $root 'src'
$build  = Join-Path $root 'build'
$log    = Join-Path $root 'build-log.txt'

# ---- 工具链路径：先自动探测，再回退到本机默认值 ----
#
# 这几条不能只写死某个人的安装路径，否则别人 clone 下来（Qt 装在 C:\Qt 等）
# 就直接失败。探测顺序：
#   1. 环境变量（Qt 安装器有时会设 QTDIR / QTDIR6，用户自己也能设）
#   2. 常见安装位置逐个找
#   3. 都找不到 → 用下面 $Fallback* 里写的本机路径，并给出明确提示
#
# 找不到时会在 [1/6] 环境自检里报错，并告诉用户改哪几个变量。

function Find-First($candidates, $relative) {
    foreach ($base in $candidates) {
        if (-not $base) { continue }
        $p = Join-Path $base $relative
        if (Test-Path $p) { return (Resolve-Path $p).Path }
    }
    return $null
}

function Get-QtRoots {
    $roots = @()
    if ($env:QTDIR6) { $roots += $env:QTDIR6 }
    if ($env:QTDIR)  { $roots += $env:QTDIR }
    # 常见安装位置 + 其中各个 Qt 版本目录
    foreach ($base in @('C:\Qt', 'D:\Qt', 'E:\Qt', 'C:\Qt6', 'D:\Qt6')) {
        if (-not (Test-Path $base)) { continue }
        $roots += (Get-ChildItem $base -Directory -ErrorAction SilentlyContinue |
                   Where-Object { $_.Name -match '^\d+\.\d+' } |
                   Sort-Object Name -Descending |
                   ForEach-Object { Join-Path $_.FullName 'msvc2022_64' })
    }
    return $roots
}

function Get-VsRoots {
    $roots = @()
    # 用 vswhere 问 Visual Studio 装在哪（最可靠）
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $found = & $vswhere -latest -products * -property installationPath 2>$null
        if ($found) { $roots += $found }
    }
    foreach ($base in @("$env:ProgramFiles\Microsoft Visual Studio", "${env:ProgramFiles(x86)}\Microsoft Visual Studio")) {
        if (-not (Test-Path $base)) { continue }
        $roots += (Get-ChildItem $base -Directory -ErrorAction SilentlyContinue |
                   ForEach-Object {
                       Get-ChildItem $_.FullName -Directory -ErrorAction SilentlyContinue |
                       ForEach-Object { $_.FullName }
                   })
    }
    return $roots
}

# 本机默认值：探测不到时兜底用
$FallbackVcvars = 'D:\Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
$FallbackQt     = 'D:\Qt\6.11.2\msvc2022_64'
$FallbackCmake  = 'D:\Qt\Tools\CMake_64\bin\cmake.exe'
$FallbackNinja  = 'D:\Qt\Tools\Ninja\ninja.exe'

$vcvars = Find-First (Get-VsRoots) 'VC\Auxiliary\Build\vcvars64.bat'
if (-not $vcvars) { $vcvars = $FallbackVcvars }

$qt = $null
foreach ($cand in Get-QtRoots) {
    if (Test-Path (Join-Path $cand 'lib\cmake\Qt6')) { $qt = $cand; break }
}
if (-not $qt) { $qt = $FallbackQt }

# CMake / Ninja 优先找 PATH，其次找 Qt Tools 目录
$cmake = $null
foreach ($cand in @((Get-Command cmake.exe -ErrorAction SilentlyContinue).Source,
                    (Find-First @('D:\Qt\Tools\CMake_64', 'C:\Qt\Tools\CMake_64',
                                  'D:\Qt\Tools\CMake', 'C:\Qt\Tools\CMake') 'bin\cmake.exe'))) {
    if ($cand -and (Test-Path $cand)) { $cmake = $cand; break }
}
if (-not $cmake) { $cmake = $FallbackCmake }

$ninja = $null
foreach ($cand in @((Get-Command ninja.exe -ErrorAction SilentlyContinue).Source,
                    (Find-First @('D:\Qt\Tools\Ninja', 'C:\Qt\Tools\Ninja') 'ninja.exe'))) {
    if ($cand -and (Test-Path $cand)) { $ninja = $cand; break }
}
if (-not $ninja) { $ninja = $FallbackNinja }

# 注意：探测结果不能在这里打印 —— Say() 是在下面才定义的函数，
# PowerShell 顺序执行，在这里调用会报 "Say 不是 cmdlet" 并导致脚本直接退出
# （表现就是窗口一闪而过）。打印放到函数定义之后的"开始"那一段。

function Say($msg) {
    Write-Host $msg
    Add-Content -LiteralPath $log -Value $msg -Encoding UTF8
}

# 一边显示一边写日志。不能用 Tee-Object：PowerShell 5.1 下它会按 UTF-16 追加，
# 和文件开头的 UTF-8 混在一起，日志会变成 "- - C o u l d" 这种带空格的乱码。
function RunLogged([string]$exe, [string[]]$exeArgs) {
    $writer = New-Object System.IO.StreamWriter($log, $true, (New-Object System.Text.UTF8Encoding($false)))
    try {
        $writer.AutoFlush = $true
        & $exe @exeArgs 2>&1 | ForEach-Object {
            Write-Host $_
            $writer.WriteLine($_)
        }
    } finally {
        $writer.Close()
    }
}

function Finish($code) {
    if ($Wait) {
        Write-Host ''
        Read-Host '按回车关闭窗口'
    }
    exit $code
}

function Die($msg) {
    Say ''
    Say '============================================================'
    Say " 构建失败: $msg"
    Say '============================================================'
    Say ''
    Say '--- 日志最后 40 行 ---'
    if (Test-Path $log) {
        Get-Content -LiteralPath $log -Tail 40 | ForEach-Object { Write-Host $_ }
    }
    Say ''
    Say "完整日志: $log"
    Finish 1
}

# ---- 开始 ----
'============================================================' | Set-Content -LiteralPath $log -Encoding UTF8
Say ' OLED 汉字取模工具 - 构建'
Say '============================================================'
Say " 构建类型: $Config"
Say " 源码目录: $src"
Say " 构建目录: $build"
Say " 日志文件: $log"
Say ''
Say ' 工具链探测结果:'
Say "   vcvars : $vcvars"
Say "   Qt     : $qt"
Say "   cmake  : $cmake"
Say "   ninja  : $ninja"
Say ''

# ---- [1/6] 环境自检 ----
Say '[1/6] 环境自检...'
foreach ($item in @(
    @{ n = 'vcvars64.bat'; p = $vcvars },
    @{ n = 'Qt6';          p = (Join-Path $qt 'lib\cmake\Qt6') },
    @{ n = 'cmake.exe';    p = $cmake },
    @{ n = 'ninja.exe';    p = $ninja })) {
    if (-not (Test-Path $item.p)) {
        Die "找不到 $($item.n): $($item.p)`n      请修改 build.ps1 开头的路径配置。"
    }
    Say "      OK  $($item.n)"
}

# ---- [2/6] 载入 MSVC 环境 ----
Say '[2/6] 载入 MSVC 环境...'
$vcOut = & cmd.exe /c "`"$vcvars`" >nul 2>&1 && set" 2>&1
if ($LASTEXITCODE -ne 0) {
    Die "vcvars64.bat 执行失败 (exit=$LASTEXITCODE)"
}
foreach ($line in $vcOut) {
    if ($line -match '^([^=]+)=(.*)$') {
        Set-Item -Path "Env:$($matches[1])" -Value $matches[2] -ErrorAction SilentlyContinue
    }
}
$cl = Get-Command cl.exe -ErrorAction SilentlyContinue
if (-not $cl) {
    Die '载入 MSVC 环境后仍然找不到 cl.exe'
}
Say "      OK  cl.exe -> $($cl.Source)"

# cl.exe 在，不等于 SDK 的工具也在，所以这里必须单独验证 rc.exe。
# 原因：MSVC 的路径是 vcvars 按脚本自身位置推出来的，不碰注册表；而 Windows SDK
# 的路径要靠 vsdevcmd 的 ext 脚本用 reg query 查注册表拿到，再塞进 PATH。那一步
# 一旦失败（reg.exe 被安全策略拦、SDK 装得不全），cl.exe 照样能找到，但 rc.exe /
# mt.exe 不在 PATH 里。CMake 试探编译到最后要编 manifest，找不到 rc 就判定编译器
# 坏了，报成 "The C++ compiler ... is broken" —— 跟 cl.exe 一点关系都没有，
# 对着那句话排查会走很远。宁可在这里当场停下，把话说准。
$rc = Get-Command rc.exe -ErrorAction SilentlyContinue
if (-not $rc) {
    Say '      FAIL 找不到 rc.exe'
    Say ''
    Say '  cl.exe 在，说明 MSVC 本身没问题；缺的是 Windows SDK 的工具目录'
    Say '  （通常在 C:\Program Files (x86)\Windows Kits\10\bin\<版本>\x64）。'
    Say ''
    Say '  可能的原因：'
    Say '    1. Windows SDK 没装，或只装了头文件/库、没装命令行工具'
    Say '    2. vcvars64.bat 查注册表失败 —— 它的输出被 >nul 2>&1 吞掉了，上面看不出来'
    Say '       （安全软件或策略拦截 reg.exe 时就是这种表现）'
    Say '    3. PATH 里有别的 rc.exe 抢先匹配（少见）'
    Say ''
    Say '  排查：开一个普通 cmd 窗口，依次执行'
    Say '        "<vcvars64.bat 的完整路径>"'
    Say '        where rc.exe'
    Say '  能列出来，就说明是本机工具链环境的问题，与本工程代码无关。'
    Say ''
    Die '载入 MSVC 环境后找不到 rc.exe（Windows SDK 未正确载入）'
}
Say "      OK  rc.exe  -> $($rc.Source)"

# mt.exe 只在生成 manifest 时用到，缺了 Ninja 一般也能过，提示一句就够。
$mt = Get-Command mt.exe -ErrorAction SilentlyContinue
if ($mt) {
    Say "      OK  mt.exe  -> $($mt.Source)"
} else {
    Say '      警告 mt.exe 未找到（manifest 工具，一般不影响构建）'
}

# ---- [3/6] CMake 配置 ----
Say '[3/6] CMake 配置...'
Add-Content -LiteralPath $log -Value ''
Add-Content -LiteralPath $log -Value '--- CMake 配置 ---'
RunLogged $cmake @('-S', $src, '-B', $build, '-G', 'Ninja',
    "-DCMAKE_MAKE_PROGRAM=$ninja",
    "-DCMAKE_PREFIX_PATH=$qt",
    "-DCMAKE_BUILD_TYPE=$Config")
if ($LASTEXITCODE -ne 0) {
    Die "CMake 配置失败 (exit=$LASTEXITCODE)"
}

# ---- [4/6] 编译 ----
Say '[4/6] 编译...'
Add-Content -LiteralPath $log -Value ''
Add-Content -LiteralPath $log -Value '--- 编译 ---'
RunLogged $cmake @('--build', $build)
if ($LASTEXITCODE -ne 0) {
    Die "编译失败 (exit=$LASTEXITCODE)"
}

# ---- [5/6] 部署 Qt 运行库 ----
Say '[5/6] 部署 Qt 运行库...'
$exe = Join-Path $build 'oledfonttool.exe'

# windeployqt 会探测 exe 是 Debug 还是 Release，自动拷对应的 DLL
# （Debug 版是 Qt6Widgetsd.dll，带 d）。
# 这里绝对不能写死 --release：Debug 构建配 Release 的 DLL 会直接报
# "找不到 Qt6Widgetsd.dll" 起不来。
#
# 部署前必须把上一次的运行时彻底清掉，否则 Debug/Release 两套会混在一起。
# 注意：只删 build\ 根目录下的 *.dll 是不够的 —— windeployqt 还会往
# platforms\ imageformats\ tls\ 这些子目录放插件，上一次构建留下的
# qwindowsd.dll（调试版）就是这么混进发布包的。所以这里整目录清。
Get-ChildItem $build -Filter 'Qt6*.dll' -Recurse -ErrorAction SilentlyContinue |
    Remove-Item -Force -ErrorAction SilentlyContinue
foreach ($sub in @('platforms', 'imageformats', 'iconengines', 'styles',
                   'tls', 'networkinformation', 'generic')) {
    $p = Join-Path $build $sub
    if (Test-Path $p) {
        Remove-Item $p -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# --compiler-runtime 让 windeployqt 把微软 VC 运行库（msvcp140.dll、
# vcruntime140.dll 等）也一起拷过来。
#
# 为什么必须加：不加的话，程序在"没装过 Visual C++ 运行库"的电脑上会报
# "找不到 msvcp140.dll"直接起不来。windeployqt 默认是不拷这些的。
RunLogged (Join-Path $qt 'bin\windeployqt.exe') @('--no-translations', '--compiler-runtime', $exe)

# 检查真正的那个 DLL 在不在——少了它程序起不来
$widgetsDll = Get-ChildItem $build -File -Filter 'Qt6Widgets.dll' -ErrorAction SilentlyContinue
if (-not $widgetsDll) {
    Die "windeployqt 没有拷来 Qt6Widgets.dll，$exe 运行时会报错。请把上面 windeployqt 的输出发给我。"
}
Say "      OK  已部署 $($widgetsDll.Name)"

# 兜底：确认 VC 运行库真的进来了。有些 windeployqt 版本对 --compiler-runtime
# 处理不一样，所以这里自己再检查一次，缺了就直接从 VS 的 Redist 目录补。
# 注意要用 Redist 目录里的版本（允许随程序分发），不要用 System32 里的。
$crtNeeded = @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
$crtMissing = @($crtNeeded | Where-Object { -not (Test-Path (Join-Path $build $_)) })
if ($crtMissing.Count -gt 0) {
    Say "      VC 运行库缺少 $($crtMissing.Count) 个，从 VS Redist 目录补齐..."
    # 找 VS 根目录。手工数层数很容易错
    # （cl.exe 在 <VS根>\VC\Tools\MSVC\<版本>\bin\Hostx64\x64\ 下，要往上 6 层），
    # 所以用循环逐级上溯，遇到含 VC\Redist\MSVC 的那一层就停。
    $crtSrc = $null
    $probe = (Get-Command cl.exe -ErrorAction SilentlyContinue).Source
    if ($probe) {
        $probe = Split-Path $probe -Parent        # 去掉文件名，从所在目录开始
        for ($i = 0; $i -lt 8 -and $probe; $i++) {
            $redist = Join-Path $probe 'VC\Redist\MSVC'
            if (Test-Path $redist) {
                $crtSrc = Get-ChildItem $redist -Recurse -Directory -Filter 'Microsoft.VC*.CRT' `
                            -ErrorAction SilentlyContinue |
                          Where-Object { $_.FullName -match '\\x64\\' -and $_.FullName -notmatch 'onecore' } |
                          Select-Object -First 1
                if ($crtSrc) { break }
            }
            $parent = Split-Path $probe -Parent
            if ($parent -eq $probe) { break }      # 到盘根了，停
            $probe = $parent
        }
    }
    if ($crtSrc) {
        foreach ($n in $crtMissing) {
            $src = Join-Path $crtSrc.FullName $n
            if (Test-Path $src) {
                Copy-Item $src $build -Force
                Say "        补上 $n"
            } else {
                Say "        找不到 $n（在 $($crtSrc.FullName)）"
            }
        }
    } else {
        Say "        没找到 VS Redist 目录，无法自动补齐。"
        Say "        请手动安装 VC++ 2015-2022 运行库，或者把 msvcp140.dll 等拷进 package\。"
    }
}

# ---- [6/6] 打包成可分发文件夹 ----
# 目的：产出一个"拷到任何 Windows 机器都能双击运行"的干净目录，
# 里面只有 exe + 必需的 Qt DLL + 说明，没有 .obj/.pdb/.ninja 这些构建杂物。
Say '[6/6] 打包可分发文件夹...'
$pkg = Join-Path $root 'package'
if (Test-Path $pkg) {
    Remove-Item $pkg -Recurse -Force -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Path $pkg -Force | Out-Null

Copy-Item $exe $pkg -Force

# 会不会有调试版混进来，用【明确的黑名单】判断，不能用 "以 d 结尾" 这种模式。
# 反例：qcertonlybackend.dll / qschannelbackend.dll 是正常的发布版插件
#       （结尾的 d 属于 backend 这个词），按模式匹配会被误删。
#
# 真调试版的特征：
#   Qt6Cored.dll             Qt 的调试库
#   qwindowsd.dll            平台插件的调试版
#   msvcp140d.dll            微软 C 运行库调试版（不可再分发，必须剔除）
#   msvcp140_2d.dll          同上，注意版本和 d 之间还夹着 _1 / _2
#   vcruntime140_1d.dll      同上
#   vcruntime140_threadsd.dll 同上
#   vccorlib140d.dll         C++/CX 运行库调试版
function Test-DebugRuntime($name) {
    # Qt 调试库：Qt6<模块>d.dll
    if ($name -match '^Qt6[A-Za-z]+d\.dll$') { return $true }
    # 平台/图片格式插件的调试版
    if ($name -match '^q(windows|svgicon|jpeg|gif|ico|modernwindowsstyle|offscreen|minimal|direct2d)d\.dll$') { return $true }
    # 微软运行库的调试版。
    # 不要试图穷举文件名 —— 这些名字的形态很多：
    #   msvcp140d.dll              以 d.dll 结尾
    #   msvcp140_1d.dll            同上（_1 在 d 前面）
    #   vcruntime140_threadsd.dll  同上（_threads 在 d 前面）
    #   msvcp140d_atomic_wait.dll  d 后面还有模块名 —— 这种以 d_ 为特征
    #   msvcp140d_codecvt_ids.dll  同上
    #   vccorlib140d.dll / concrt140d.dll
    #
    # 只需要两条规则就能全覆盖：
    #   a) 以 "d.dll" 结尾
    #   b) 名字里含 "d_"（调试标记后面跟着模块名）
    if ($name -match '^(msvcp|vcruntime|vccorlib|concrt|ucrtbased)') {
        if ($name.EndsWith('d.dll') -or $name.Contains('d_')) {
            return $true
        }
    }
    return $false
}

# Qt 运行库。跳过调试版（双保险：上面已经清理过，这里再挡一道）
$skipped = @()
Get-ChildItem $build -File -Filter '*.dll' | ForEach-Object {
    if ($_.Name -like 'oledfonttool*') { return }
    if (Test-DebugRuntime $_.Name) { $skipped += $_.Name; return }
    Copy-Item $_.FullName -Destination $pkg -Force
}

# 插件子目录（platforms 少了程序起不来）
foreach ($sub in @('platforms', 'imageformats', 'iconengines', 'styles',
                   'tls', 'networkinformation', 'generic')) {
    $sp = Join-Path $build $sub
    if (-not (Test-Path $sp)) { continue }
    $dp = Join-Path $pkg $sub
    New-Item -ItemType Directory -Path $dp -Force | Out-Null
    Get-ChildItem $sp -File -Filter '*.dll' | ForEach-Object {
        if (Test-DebugRuntime $_.Name) { $skipped += $_.Name; return }
        Copy-Item $_.FullName -Destination $dp -Force
    }
    if (-not (Get-ChildItem $dp -File -ErrorAction SilentlyContinue)) {
        Remove-Item $dp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

if ($skipped.Count -gt 0) {
    Say ("      已剔除调试版文件 {0} 个: {1}" -f $skipped.Count, ($skipped -join ', '))
}

# 这个 exe 只用到 Core/Gui/Widgets/Svg，连网络都不用，所以 Qt6Network
# 和它的 TLS 后端插件（qcertonlybackend / qschannelbackend / qnetworklistmanager）
# 在发布包里是多余的，整块去掉。
foreach ($extra in @(
    'Qt6Network.dll',
    'networkinformation\qnetworklistmanager.dll',
    'tls\qcertonlybackend.dll',
    'tls\qschannelbackend.dll')) {
    $p = Join-Path $pkg $extra
    if (Test-Path $p) { Remove-Item $p -Force -ErrorAction SilentlyContinue }
}
Remove-Item (Join-Path $pkg 'tls') -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $pkg 'networkinformation') -Recurse -Force -ErrorAction SilentlyContinue

# 附一份使用说明，别人拿到就能用
$readme = @"
OLED 汉字取模工具  ($Config 版)
================================

双击 oledfonttool.exe 运行。不需要安装 Qt，也不需要编译器。

用法
----
1. 左边输入要显示的汉字（多行也可以，会自动去重）
2. 选字号：16x16（默认，最省 Flash）、24x24、32x32（大字）
3. 右边实时显示点阵，调阈值看效果
4. 点「导出 OLED_CFont.h」生成字库
5. 把生成的文件放进 Keil 工程的 Hardware 目录，Rebuild 即可

注意
----
字库格式固定为：
    阴码 / 列行式 / 逆向(低位在前)
    每行 = 3 字节 UTF-8 编码 + 点阵字节
    点阵字节 = 16x16 的 32 字节 / 24x24 的 72 字节 / 32x32 的 128 字节

换字号后，驱动里的数组名和每行字节数会变：
    16x16  ->  数组 OLED_CF16x16   每行 35 字节
    24x24  ->  数组 OLED_CF24x24   每行 75 字节
    32x32  ->  数组 OLED_CF32x32   每行 131 字节
导出的头文件开头有 OLED_CFONT_W / H / BYTES / STRIDE 四个宏，
驱动里改用这几个宏，以后换字号就不用再改驱动。

如果导出时提示"需要手动搬运"，说明本程序没有直接写入工程目录的权限，
按提示把文件复制进 Hardware 目录也行。
"@
Set-Content -LiteralPath (Join-Path $pkg '使用说明.txt') -Value $readme -Encoding UTF8

$pkgSize = (Get-ChildItem $pkg -Recurse -File | Measure-Object Length -Sum).Sum
$pkgFiles = (Get-ChildItem $pkg -Recurse -File | Measure-Object).Count

Say ''
Say '============================================================'
Say ' 构建成功！'
Say ''
Say " 开发用（带调试信息）: $exe"
Say " 分发用（可拷走）    : $pkg"
Say ("   共 {0} 个文件, {1:N1} MB" -f $pkgFiles, ($pkgSize / 1MB))
Say '============================================================'
Finish 0
