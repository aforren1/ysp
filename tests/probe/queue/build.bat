@echo off
rem Builds probe_queue.dll with MSVC against the probe copy of ysp/screen.h
rem (include\ here comes before the repository's include\), and copies
rem SDL3.dll beside it.
rem   build.bat SDL3_ROOT OUT_DIR
rem SDL3_ROOT holds include\ and lib\x64\ (the SDL3 VC development package).
setlocal
set HERE=%~dp0
set SDL=%~1
set OUT=%~2
if "%OUT%"=="" (echo usage: build.bat SDL3_ROOT OUT_DIR & exit /b 2)
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /O2 /W3 /MD /LD /I"%HERE%include" /I"%HERE%..\..\..\include" /I"%SDL%\include" ^
   "%HERE%probe_queue.c" /Fo"%OUT%\\" /Fe"%OUT%\probe_queue.dll" /link ^
   /LIBPATH:"%SDL%\lib\x64" SDL3.lib user32.lib dxgi.lib d3d11.lib dxguid.lib psapi.lib || exit /b 1
copy /y "%SDL%\lib\x64\SDL3.dll" "%OUT%\" >nul || exit /b 1
