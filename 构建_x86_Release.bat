@echo off
setlocal EnableExtensions
chcp 936 >nul
cd /d "%~dp0"
call :build x86 Win32
exit /b %errorlevel%

:build
set "ARCH_NAME=%~1"
set "CMAKE_ARCH=%~2"
set "CMAKE=cmake"
if exist "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "CMAKE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "BUILD_DIR=build\%ARCH_NAME%"
set "OUT_DIR=release\%ARCH_NAME%"
if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
if exist "%OUT_DIR%" rmdir /s /q "%OUT_DIR%"
mkdir "%OUT_DIR%" >nul 2>nul

echo [1/3] 配置 Visual Studio 18 2026 %CMAKE_ARCH% 工程...
"%CMAKE%" -S . -B "%BUILD_DIR%" -G "Visual Studio 18 2026" -A %CMAKE_ARCH%
if errorlevel 1 (
  echo.
  echo VS18 生成器不可用，尝试 Visual Studio 17 2022...
  if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
  "%CMAKE%" -S . -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A %CMAKE_ARCH%
  if errorlevel 1 goto :fail
)

echo [2/3] 编译 Release...
"%CMAKE%" --build "%BUILD_DIR%" --config Release --parallel
if errorlevel 1 goto :fail

echo [3/3] 整理输出...
copy /y "%BUILD_DIR%\bin\Release\stamky.exe" "%OUT_DIR%\stamky.exe" >nul 2>nul
if not exist "%OUT_DIR%\stamky.exe" copy /y "%BUILD_DIR%\bin\stamky.exe" "%OUT_DIR%\stamky.exe" >nul 2>nul
if not exist "%OUT_DIR%\stamky.exe" goto :fail

echo.
echo ========================================
echo 构建完成：%OUT_DIR%\stamky.exe
echo 版本：v0.9.3 %ARCH_NAME%
echo ========================================
echo.
if not defined STAMKY_NO_PAUSE pause
exit /b 0

:fail
echo.
echo ========================================
echo 构建失败。
echo 请复制从第一条 error 开始的完整编译输出。
echo release 目录不会保留旧 EXE 冒充本次结果。
echo ========================================
echo.
if not defined STAMKY_NO_PAUSE pause
exit /b 1
