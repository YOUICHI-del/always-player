@echo off
setlocal
REM ============================================
REM Always Player v9.0.0 - Installer build script
REM Just double-click this file.
REM ============================================
set WIX_BIN="C:\Program Files (x86)\WiX Toolset v3.14\bin"
REM NOTE: %~dp0 always ends with a trailing backslash.
REM If you wrap it in quotes like "%~dp0", the backslash right
REM before the closing quote escapes that quote, which breaks
REM argument parsing for candle.exe/light.exe (classic Windows
REM command-line gotcha). So we strip the trailing backslash here.
set "SRC=%~dp0"
if "%SRC:~-1%"=="\" set "SRC=%SRC:~0,-1%"
echo ============================================
echo  DEBUG: This script is running from:
echo  %SRC%
echo ============================================
pause
REM If a leftover .wixobj from a previous (possibly failed) run
REM exists, delete it first. Otherwise, if candle silently fails,
REM light could link the old stale .wixobj and produce confusing
REM errors that point to an old/wrong path.
if exist "%SRC%\Always.wixobj" del "%SRC%\Always.wixobj"
echo [1/2] Compiling wxs...
%WIX_BIN%\candle.exe -dSourceDir="%SRC%" -out "%SRC%\Always.wixobj" "%SRC%\Always.wxs"
if errorlevel 1 (
    echo.
    echo ERROR during candle step. See messages above.
    pause
    exit /b 1
)
echo [2/2] Building msi...
%WIX_BIN%\light.exe -ext WixUIExtension -out "%SRC%\AlwaysPlayer_v9.0.0.msi" "%SRC%\Always.wixobj"
if errorlevel 1 (
    echo.
    echo ERROR during light step. See messages above.
    pause
    exit /b 1
)
echo.
echo ============================================
echo  DONE. AlwaysPlayer_v9.0.0.msi has been created.
echo ============================================
pause
