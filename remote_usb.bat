@echo off
chcp 65001 > nul
rem ============================================================
rem  Always Link (PCリモコン) USB接続用
rem  スマホをUSBでつないでから、このファイルをダブルクリックします。
rem  スマホのリモコン画面で 127.0.0.1 に接続すると、USB経由でPCを操作できます。
rem ============================================================

set "ADB=%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe"
if not exist "%ADB%" set "ADB=adb"

"%ADB%" devices
"%ADB%" reverse tcp:50505 tcp:50505
if errorlevel 1 goto NG

echo.
echo [OK] 準備ができました。
echo スマホのPCリモコン画面で、IPアドレス欄に 127.0.0.1 と入れて「接続」を押してください。
echo ※ USBケーブルを抜き差ししたら、もう一度このファイルを実行してください。
goto END

:NG
echo.
echo [失敗] スマホが見つかりません。
echo  - USBケーブルでつながっているか
echo  - スマホの「開発者向けオプション」で「USBデバッグ」がONか
echo  - スマホに「USBデバッグを許可しますか？」が出ていたら「許可」を押したか
echo を確認して、もう一度このファイルを実行してください。

:END
echo.
pause
