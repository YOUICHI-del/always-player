@echo off
set WIX=C:\Program Files (x86)\WiX Toolset v3.14\bin
set SRC=%~dp0build
set OUT=%~dp0
cd /d "%OUT%"
echo [1/2] Candle...
"%WIX%\candle.exe" -nologo -dSourceDir="%SRC%" Always.wxs
if errorlevel 1 ( echo Candle failed & pause & exit /b 1 )
echo [2/2] Light...
"%WIX%\light.exe" -nologo -ext WixUIExtension Always.wixobj -out "%OUT%Always_Player_v6_Setup.msi"
if errorlevel 1 ( echo Light failed & pause & exit /b 1 )
echo Done! Always_Player_v6_Setup.msi created.
pause