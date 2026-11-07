@echo off
setlocal
rem DiskPilot - compilazione con Visual Studio 2026 (CMake + Ninja inclusi in VS)
rem Uso: build.bat            -> Release
rem      build.bat debug      -> Debug

set CONFIG=Release
if /i "%1"=="debug" set CONFIG=Debug

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo vswhere.exe non trovato: installa Visual Studio 2026 con "Sviluppo di applicazioni desktop con C++".
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo Strumenti C++ di Visual Studio non trovati.
    exit /b 1
)

call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cd /d "%~dp0"
cmake -S . -B build\%CONFIG% -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% || exit /b 1
cmake --build build\%CONFIG% || exit /b 1

echo.
echo Fatto: build\%CONFIG%\DiskPilot.exe
endlocal
