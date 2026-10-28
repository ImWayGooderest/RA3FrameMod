@echo off
rem Builds bin\dsound.dll (32-bit). Needs Visual Studio 2019+ or the VS Build Tools with the C++ workload.
setlocal
set ROOT=%~dp0
if /i "%VSCMD_ARG_TGT_ARCH%"=="x86" goto build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto novs
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto novs
call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1 || exit /b 1
:build
if not exist "%ROOT%bin" mkdir "%ROOT%bin"
if not exist "%ROOT%build" mkdir "%ROOT%build"
cl /nologo /O2 /MT /W3 /EHsc /LD /Fo"%ROOT%build\\" "%ROOT%src\dllmain.cpp" ^
   /link /DEF:"%ROOT%src\dsound.def" /OUT:"%ROOT%bin\dsound.dll" /IMPLIB:"%ROOT%build\dsound.lib" /SAFESEH:NO user32.lib kernel32.lib || exit /b 1
copy /y "%ROOT%src\RA3FrameMod.ini" "%ROOT%bin\RA3FrameMod.ini" >nul
echo Built %ROOT%bin\dsound.dll
exit /b 0
:novs
echo Visual Studio with the C++ build tools was not found.
exit /b 1
