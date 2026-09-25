@echo off
rem Always Player v9 リモコンテスト起動用（エラーが出ても画面を閉じない）
powershell -NoProfile -ExecutionPolicy Bypass -NoExit -File "%~dp0remote_test.ps1"
