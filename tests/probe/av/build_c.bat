@echo off
rem Builds probe_c.exe with MSVC. Run from a plain cmd; it calls vcvars64.
rem   build_c.bat SDL3_ROOT MINIAUDIO_DIR OUT_DIR
rem SDL3_ROOT holds include\ and lib\x64\ (the SDL3 VC development package).
setlocal
set HERE=%~dp0
set SDL=%~1
set MA=%~2
set OUT=%~3
if "%OUT%"=="" (echo usage: build_c.bat SDL3_ROOT MINIAUDIO_DIR OUT_DIR & exit /b 2)
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /O2 /W3 /MD /I"%HERE%..\..\..\include" /I"%SDL%\include" /I"%MA%" "%HERE%probe_c.c" ^
   /Fo"%OUT%\\" /Fe"%OUT%\probe_c.exe" /link /LIBPATH:"%SDL%\lib\x64" SDL3.lib user32.lib || exit /b 1
