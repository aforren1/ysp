@echo off
rem Builds gapdiag.exe with MSVC against include\ of this repository.
rem   build.bat SDL3_ROOT ANGLE_DIR OUT_DIR [NAME]
rem SDL3_ROOT holds include\ and lib\x64\ (the SDL3 VC package); SDL3.dll,
rem libEGL.dll and libGLESv2.dll go beside the program.
setlocal
set HERE=%~dp0
set SDL=%~1
set ANGLE=%~2
set OUT=%~3
set NAME=%~4
if "%NAME%"=="" set NAME=gapdiag
if "%OUT%"=="" (echo usage: build.bat SDL3_ROOT ANGLE_DIR OUT_DIR [NAME] & exit /b 2)
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /W4 /WX /O2 /std:c11 /I"%HERE%..\..\..\include" /I"%SDL%\include" "%HERE%gapdiag.c" /Fo"%OUT%\\" /Fe"%OUT%\%NAME%.exe" ^
   /link /LIBPATH:"%SDL%\lib\x64" SDL3.lib user32.lib dwmapi.lib || exit /b 1
copy /y "%SDL%\lib\x64\SDL3.dll" "%OUT%" >nul
copy /y "%ANGLE%\libEGL.dll" "%OUT%" >nul
copy /y "%ANGLE%\libGLESv2.dll" "%OUT%" >nul
