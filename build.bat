@echo off
setlocal
set ROOT=%~dp0
set QT=C:\Qt\6.11.0\msvc2022_64
set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat
call "%VCVARS%"
set PATH=%QT%\bin;C:\Program Files\CMake\bin;%PATH%
cd /d "%ROOT%"
cmake --preset windows-release
if errorlevel 1 ( echo CMake failed & pause & exit /b 1 )
cmake --build --preset windows-release
if errorlevel 1 ( echo Build failed & pause & exit /b 1 )
echo Build succeeded!
cd /d "%ROOT%build"
"%QT%\bin\windeployqt.exe" Always.exe --no-translations
copy "C:\libs\mpv\libmpv-2.dll" .
echo Done!
pause
