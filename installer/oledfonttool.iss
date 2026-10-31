; ============================================================================
;  OLED 点阵取模工具 —— Inno Setup 安装脚本
;
;  编译方式（在命令行里）：
;      "C:\Users\74406\AppData\Local\Programs\Inno Setup 6\ISCC.exe" oledfonttool.iss
;
;  产出：Output\oledmatrix-<版本>-setup.exe
;
;  源文件取自 package\ 目录（由 oledfonttool\scripts\build.ps1 的 [6/6] 步骤生成）。
; ============================================================================

#define MyAppName        "OLED 点阵取模工具"
#define MyAppVersion     "1.3"
#define MyAppPublisher   "zhangwtee"
#define MyAppURL         "https://github.com/zhangwtee/stm32-oled-font-tool"
#define MyAppExeName     "oledmatrix.exe"

; package 目录的位置（相对本 .iss 文件）
#define SrcDir           "..\package"

[Setup]
; 32 位安装程序可以在 64 位 Windows 上正常安装 64 位应用
AppId={{8F3A2C41-7D5E-4B96-9A18-2C6E4F7B1D33}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}

; 允许用户选择安装路径
DefaultDirName={autopf}\oledfonttool
DisableDirPage=no
DisableProgramGroupPage=yes
AllowNoIcons=yes

; 输出
OutputDir=Output
OutputBaseFilename=oledmatrix-{#MyAppVersion}-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
SetupIconFile=app.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName} {#MyAppVersion}

; 这个程序是 64 位的
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible

; 需要 64 位 Windows 10 及以上
MinVersion=10.0

[Languages]
Name: "chinese"; MessagesFile: "compiler:Default.isl"
; 说明：默认用简体中文界面。如果安装的 Inno Setup 带中文语言包，
; 可以改成 "compiler:Languages\ChineseSimplified.isl"。

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务:"; Flags: checkedonce

[Files]
; package 目录里的所有东西（exe + Qt 运行库 + 插件 + 使用说明）
Source: "{#SrcDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "立即运行 {#MyAppName}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; 程序会在自己目录下生成导出文件，卸载时一并清掉
Type: files; Name: "{app}\OLED_CFont.h"
Type: files; Name: "{app}\export-debug.txt"
