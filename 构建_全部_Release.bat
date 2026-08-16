@echo off
setlocal EnableExtensions
chcp 936 >nul
cd /d "%~dp0"

echo ========================================
echo stamky v0.9.3 x86 + x64 Release

echo ========================================
echo.
set "STAMKY_NO_PAUSE=1"
call "构建_x86_Release.bat"
if errorlevel 1 exit /b 1
call "构建_x64_Release.bat"
if errorlevel 1 exit /b 1

echo.
set "STAMKY_NO_PAUSE="
echo x86 与 x64 均已完成。
pause
exit /b 0
