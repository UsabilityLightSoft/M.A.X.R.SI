@echo off
rem usage: msvc.cmd <x86|x64> <file.cpp>
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set "VSPATH=%%i"
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" %1 >nul || exit /b 2
cl /nologo /std:c++20 /EHsc /W4 /WX /c %2 /Fo%TEMP%\exp.obj
exit /b %errorlevel%
