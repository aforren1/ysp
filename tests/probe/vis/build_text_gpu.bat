@echo off
rem Builds text_gpu.exe with MSVC. Run from a plain cmd; it calls vcvars64.
rem   build_text_gpu.bat SDL3_ROOT ANGLE_DIR OUT_DIR
rem SDL3_ROOT holds include\ and lib\x64\ (the SDL3 VC development package);
rem ANGLE_DIR holds libEGL.dll and libGLESv2.dll (third_party\vis\angle).
setlocal
set HERE=%~dp0
set SDL=%~1
set ANGLE=%~2
set OUT=%~3
if "%OUT%"=="" (echo usage: build_text_gpu.bat SDL3_ROOT ANGLE_DIR OUT_DIR & exit /b 2)
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /O2 /W3 /MD /I"%HERE%..\..\..\include" /I"%SDL%\include" "%HERE%text_gpu.c" ^
   /Fo"%OUT%\\" /Fe"%OUT%\text_gpu.exe" /link /LIBPATH:"%SDL%\lib\x64" SDL3.lib user32.lib || exit /b 1
copy /y "%SDL%\lib\x64\SDL3.dll" "%OUT%" >nul
copy /y "%ANGLE%\libEGL.dll" "%OUT%" >nul
copy /y "%ANGLE%\libGLESv2.dll" "%OUT%" >nul
