@echo off
rem Copy OLED_CFont.h into a Keil project's Hardware folder.
rem
rem The project is located by its project file (*.uvprojx / *.ewp / .cproject),
rem searched upward from this script, so the tool keeps working no matter
rem where it is installed, what the project is called, and whatever the
rem driver source file happens to be named.
rem
rem This file must stay PURE ASCII with no non-ASCII path literals:
rem cmd.exe parses .cmd using the system ANSI codepage.

setlocal enabledelayedexpansion
set "HERE=%~dp0"
set "BUILD=%HERE%..\build"

echo ============================================================
echo  Install OLED_CFont.h into the Keil project
echo ============================================================
echo.

rem ---- 1. walk upward looking for the project folder ----
rem
rem  The project is identified by its PROJECT FILE (*.uvprojx / *.uvproj /
rem  *.ewp / .cproject), not by the driver source name. The driver may be
rem  called OLED.c, lcd.c, display.c ...; the project file is what actually
rem  defines "this is a project".
rem
rem  Fallbacks, strongest first:
rem     a folder holding Hardware\OLED.c   (old rule, kept for compatibility)
rem     a folder holding Hardware\         (weakest, only if nothing else)
set "PROBE=%HERE%"
set "HW="
set "WEAK="
for /l %%i in (1,1,8) do (
    if not defined HW (
        for /d %%d in ("!PROBE!*") do (
            if not defined HW call :probe "%%d"
        )
    )
    if not defined HW (
        pushd "!PROBE!.." 2>nul
        if errorlevel 1 (
            rem reached the drive root, stop climbing
        ) else (
            set "PROBE=!CD!\"
            popd
        )
    )
)
if not defined HW if defined WEAK set "HW=%WEAK%"

goto :probe_done

:probe
rem  %~1 = candidate project folder. Sets HW (strong hit) or WEAK (weak hit).
if defined HW goto :eof
if exist "%~1\*.uvprojx" goto :probe_project
if exist "%~1\*.uvproj"  goto :probe_project
if exist "%~1\*.ewp"     goto :probe_project
if exist "%~1\.cproject" goto :probe_project
if exist "%~1\Hardware\OLED.c" set "HW=%~1\Hardware"
if not defined HW if not defined WEAK if exist "%~1\Hardware" set "WEAK=%~1\Hardware"
goto :eof

:probe_project
if exist "%~1\Hardware" ( set "HW=%~1\Hardware" ) else ( set "HW=%~1" )
goto :eof

:probe_done

if not defined HW (
    echo [ERROR] Could not find the Keil project folder.
    echo.
    echo  Searched upward from this script for a folder containing a
    echo  project file:  *.uvprojx   *.uvproj   *.ewp   .cproject
    echo.
    echo  If your project lives somewhere else, just copy OLED_CFont.h
    echo  into the folder that holds your driver .c file -- it is one file.
    echo.
    pause
    exit /b 1
)
echo  project found: %HW%
echo.

rem ---- 2. locate the font file to install ----
set "SRC="
if exist "%BUILD%\OLED_CFont.h" (
    set "SRC=%BUILD%\OLED_CFont.h"
    echo  source: Qt tool export  ^(oledfonttool\build^)
) else if exist "%HERE%OLED_CFont.h" (
    set "SRC=%HERE%OLED_CFont.h"
    echo  source: Python generator  ^(oledfonttool\src^)
)

if not defined SRC (
    echo [ERROR] no OLED_CFont.h found in either place:
    echo         %BUILD%\OLED_CFont.h
    echo         %HERE%OLED_CFont.h
    echo.
    echo  Export it from the Qt tool, or run build_cfont.py first.
    echo.
    pause
    exit /b 1
)

rem ---- 3. install, keeping a backup of the old file ----
if exist "%HW%\OLED_CFont.h" (
    copy /y "%HW%\OLED_CFont.h" "%HERE%OLED_CFont.old.h" >nul
    echo  old file backed up as oledfonttool\src\OLED_CFont.old.h
)

copy /y "%SRC%" "%HW%\OLED_CFont.h" >nul
if errorlevel 1 (
    echo [FAILED] copy failed - the target folder is not writable.
    echo.
    pause
    exit /b 1
)

echo [OK] installed to: %HW%\OLED_CFont.h
echo      Rebuild the Keil project to take effect.
echo.
pause
endlocal
exit /b 0
