@echo off
set WIX=C:\Program Files (x86)\WiX Toolset v3.14\bin
set SRC=C:\Users\jokun\Downloads\Always_App_v5.0\build
set OUT=C:\Users\jokun\Downloads\Always_App_v5.0
cd /d "%OUT%"
echo [1/2] Candle...
"%WIX%\candle.exe" -nologo -dSourceDir="%SRC%" Always.wxs
if errorlevel 1 ( echo Candle failed & pause & exit /b 1 )
echo [2/2] Light...
"%WIX%\light.exe" -nologo -ext WixUIExtension Always.wixobj -out "%USERPROFILE%\Downloads\Always_Player_v5_Setup.msi"
if errorlevel 1 ( echo Light failed & pause & exit /b 1 )
echo Done! Downloads\Always_Player_v5_Setup.msi created.
pause
