function build_mex(outdir, sdl, ma, static)
%BUILD_MEX  Build the probe MEX into OUTDIR (MATLAB: MSVC; Octave: MinGW).
%   build_mex(outdir, 'C:/sdk/SDL3-3.4.0', miniaudio_dir)
%   Copy SDL3.dll, libEGL.dll and libGLESv2.dll into OUTDIR first.
%   build_mex(outdir, sdl_static_prefix, miniaudio_dir, true) links a
%   static SDL3 (SDL3-static.lib from MSVC, libSDL3.a from MinGW; see
%   README.md); OUTDIR then needs libEGL.dll and libGLESv2.dll only.
    if nargin < 4, static = false; end
    here = fileparts(mfilename('fullpath'));
    inc = fullfile(here, '..', '..', '..', '..', 'include');
    src = fullfile(here, 'ysp_av_probe.c');
    args = {['-I' inc], ['-I' fullfile(sdl, 'include')], ['-I' ma], src};
    % the system libraries SDL3.dll imports itself (SDL's sdl3.pc, Libs)
    sys = {'-luser32', '-lgdi32', '-lwinmm', '-limm32', '-lole32', '-loleaut32', '-lversion', ...
           '-luuid', '-ladvapi32', '-lsetupapi', '-lshell32', '-ldinput8'};
    if exist('OCTAVE_VERSION', 'builtin')
        if static
            mex(args{:}, ['-L' fullfile(sdl, 'lib')], '-lSDL3', sys{:}, '-o', fullfile(outdir, 'ysp_av_probe'));
        else
            % MinGW's ld 2.43 aborts on SDL 3.4's MSVC import library
            % (ldlang.c:527), so it links against the SDL3.dll already in
            % OUTDIR: ld finds SDL3.dll for -lSDL3 when no .lib is there.
            mex(args{:}, ['-L' outdir], '-lSDL3', '-luser32', '-o', fullfile(outdir, 'ysp_av_probe'));
        end
    elseif static
        mex(args{:}, ['-L' fullfile(sdl, 'lib')], '-lSDL3-static', sys{:}, '-outdir', outdir);
    else
        mex(args{:}, ['-L' fullfile(sdl, 'lib', 'x64')], '-lSDL3', '-luser32', '-outdir', outdir);
    end
end
