@echo off
setlocal
cd /d "%~dp0"
cmake -S . -B build-release -A x64 -DBUILD_TESTING=OFF
if errorlevel 1 goto :err
cmake --build build-release --config Release
if errorlevel 1 goto :err
copy /Y "build-release\Release\DesktopOrganizer.exe" "%~dp0DesktopOrganizer.exe"
echo.
echo Build finished: DesktopOrganizer.exe
pause
exit /b 0
:err
echo.
echo Build failed. Make sure Visual Studio 2022 Desktop development with C++ and CMake are installed.
pause
exit /b 1
