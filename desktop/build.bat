@echo off
rem Builds SmartKnob.exe (static CRT, no runtime needed). Requires Visual Studio with the C++ workload.
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set VS=
for /f "usebackq delims=" %%i in (`call "%%VSWHERE%%" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (echo Visual Studio C++ tools not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1
if not exist build mkdir build
cl /nologo /O1 /MT /EHsc /std:c++17 /W3 /utf-8 /DUNICODE /D_UNICODE smartknob.cpp /Fo:build\ /Fe:SmartKnob.exe ^
   /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /MANIFEST:EMBED || exit /b 1
echo Built %~dp0SmartKnob.exe
