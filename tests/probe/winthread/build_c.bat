@echo off
rem Builds probe_c.exe with MSVC against the probe copy of ysp/screen.h
rem (include\ here comes before the repository's include\).
rem   build_c.bat SDL3_ROOT MINIAUDIO_DIR OUT_DIR [static]
rem SDL3_ROOT holds include\ and lib\x64\ (the SDL3 VC development package),
rem or, with "static", include\ and lib\SDL3-static.lib (tools in README.md).
setlocal
set HERE=%~dp0
set SDL=%~1
set MA=%~2
set OUT=%~3
set MODE=%~4
if "%OUT%"=="" (echo usage: build_c.bat SDL3_ROOT MINIAUDIO_DIR OUT_DIR [static] & exit /b 2)
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
set INC=/I"%HERE%include" /I"%HERE%..\..\..\include" /I"%SDL%\include" /I"%MA%"
if /i "%MODE%"=="static" (
  rem Static SDL3 needs the system libraries its DLL imports itself.
  cl /nologo /O2 /W3 /MD %INC% "%HERE%probe_c.c" /Fo"%OUT%\\" /Fe"%OUT%\probe_c.exe" /link ^
     "%SDL%\lib\SDL3-static.lib" user32.lib gdi32.lib winmm.lib imm32.lib ole32.lib oleaut32.lib version.lib ^
     uuid.lib advapi32.lib setupapi.lib shell32.lib dinput8.lib || exit /b 1
) else (
  cl /nologo /O2 /W3 /MD %INC% "%HERE%probe_c.c" /Fo"%OUT%\\" /Fe"%OUT%\probe_c.exe" /link ^
     /LIBPATH:"%SDL%\lib\x64" SDL3.lib user32.lib || exit /b 1
)
