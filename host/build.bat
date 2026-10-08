@echo off
rem Compile le banc de test PC du coeur d'emulation (MSVC, Build Tools de Visual Studio).
rem Resultat : host\build\a2host.exe
setlocal
set "HERE=%~dp0"
set "PF86=%ProgramFiles(x86)%"
set "VSDIR="
if exist "%PF86%\Microsoft Visual Studio\Installer\vswhere.exe" (
    for /f "usebackq tokens=*" %%i in (`"%PF86%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VSDIR=%%i"
)
if not defined VSDIR (
    echo Visual Studio Build Tools introuvable.
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist "%HERE%build" mkdir "%HERE%build"
set "CORE=%HERE%..\Apple2_ProjectESP32\src\core"
cl /nologo /O2 /EHsc /std:c++17 /W3 /wd4244 /wd4267 /wd4996 /utf-8 ^
   /I"%CORE%" /I"%HERE%..\Apple2_ProjectESP32\include" ^
   /Fo"%HERE%build\\" /Fe"%HERE%build\a2host.exe" ^
   "%HERE%main.cpp" "%CORE%\Cpu6502.cpp" "%CORE%\A2Machine.cpp" "%CORE%\A2Video.cpp" "%CORE%\A2Disk.cpp" "%CORE%\A2Hdd.cpp" "%CORE%/A2Mockingboard.cpp" "%CORE%/A2Mouse.cpp" "%CORE%/A2IIc.cpp" "%CORE%/A2Disk35.cpp" "%CORE%/A2SmartPort.cpp"
exit /b %ERRORLEVEL%
