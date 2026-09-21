@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

echo ================================================
echo  ChatBox.dll build  (32-bit, C++ restricted subset)
echo ================================================
echo.

rem ---------------------------------------------------------------
rem  Locate toolchain
rem  1) ..\..\NoCopyProtect\tools\w64devkit  (当前布局)
rem  2) ..\..\NoCopyProtect\toolchain        (旧布局)
rem  3) .\toolchain                          (本地自带)
rem ---------------------------------------------------------------
set "TC=%~dp0..\..\NoCopyProtect\tools\w64devkit"
if not exist "%TC%\bin\g++.exe" set "TC=%~dp0..\..\NoCopyProtect\toolchain"
if not exist "%TC%\bin\g++.exe" set "TC=%~dp0toolchain"
if not exist "%TC%\bin\g++.exe" (
    echo [ERROR] toolchain not found.
    echo         expected: NoCopyProtect\tools\w64devkit\bin\g++.exe
    pause
    exit /b 1
)
echo [1/4] toolchain: %TC%

for /f "tokens=*" %%v in ('"%TC%\bin\g++.exe" -dumpmachine') do set "TRIPLE=%%v"
echo       target: !TRIPLE!
echo !TRIPLE! | findstr /i "i686 mingw32" >nul
if errorlevel 1 (
    echo [ERROR] toolchain is not 32-bit. gamemd.exe is a 32-bit process.
    pause
    exit /b 1
)

rem ---------------------------------------------------------------
rem  IMPORTANT: 32-bit gcc uses ANSI APIs to locate its own files.
rem  If the path contains non-ASCII characters (e.g. Chinese), those
rem  characters are lost and gcc cannot find its libs:
rem      ld.exe: cannot find -lkernel32
rem  Workaround: build in a pure-ASCII path (%TEMP%), then copy back.
rem ---------------------------------------------------------------
echo %TC% | findstr /r /c:"^[ -~]*$" >nul
if errorlevel 1 goto :need_temp
echo [2/4] path is pure ASCII - building in place
set "BUILD=%~dp0..\..\_build"
set "SRC=%~dp0"
goto :dobuild

:need_temp
echo [2/4] path contains non-ASCII - staging to %%TEMP%% for build
set "WORK=%TEMP%\chatbox_build"
if exist "%WORK%" rmdir /s /q "%WORK%"
mkdir "%WORK%" 2>nul
echo       copying toolchain (about 70MB, please wait)...
robocopy "%TC%" "%WORK%\toolchain" /E /NFL /NDL /NJH /NJS /NP /R:0 /W:0 >nul
echo       copying source...
robocopy "%~dp0..\..\ChatBox\src" "%WORK%\src" /E /NFL /NDL /NJH /NJS /NP /R:0 /W:0 >nul
set "TC=%WORK%\toolchain"
set "SRC=%WORK%\src"
set "BUILD=%WORK%\out"
mkdir "%BUILD%" 2>nul

:dobuild
echo [3/4] compiling...
set "PATH=%TC%\bin;%PATH%"

rem ---------------------------------------------------------------
rem  C++ 受限子集:
rem    -fno-exceptions -fno-rtti  不引入异常/RTTI 运行时
rem    -nostdlib                  不链接 CRT / libstdc++
rem    代码里不使用 STL、不使用全局对象(全局构造需要 CRT 启动代码)
rem
rem  -Wl,--no-insert-timestamp    去掉 PE 链接时间戳, 让产物可复现
rem ---------------------------------------------------------------
"%TC%\bin\g++.exe" -O2 -shared -fno-exceptions -fno-rtti ^
    -o "%BUILD%\ChatBox.dll" "%SRC%\ChatBox.cpp" ^
    -nostdlib -lkernel32 -luser32 -lgcc ^
    -Wl,--enable-stdcall-fixup -Wl,--no-insert-timestamp -Wl,--entry,_DllMain@12
if errorlevel 1 (
    echo [ERROR] build failed.
    pause
    exit /b 1
)

echo [4/4] collecting output...
copy /y "%BUILD%\ChatBox.dll" "%~dp0..\ChatBox.dll" >nul

echo.
echo ================================================
echo  BUILD OK
echo ================================================
for %%F in ("%~dp0..\ChatBox.dll") do echo    %%~nxF  %%~zF bytes
echo.
echo Deploy to the game folder (where gamemd.exe lives):
echo     ChatBox.dll          ^<- the plugin
echo     ChatBox.dll.inj      ^<- hook declaration, file name must match exactly
echo     ChatBox.ini          ^<- config, optional (defaults apply if absent)
echo.
echo Logs are written to:  ^<game folder^>\MsgLog\YYYY-MM-DD_HH-MM-SS.log
echo.
pause
