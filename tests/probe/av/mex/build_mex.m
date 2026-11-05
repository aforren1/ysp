function build_mex(outdir, sdl, ma)
%BUILD_MEX  Build the probe MEX into OUTDIR (MATLAB: MSVC; Octave: MinGW).
%   build_mex(outdir, 'C:/sdk/SDL3-3.4.0', miniaudio_dir)
%   Copy SDL3.dll, libEGL.dll and libGLESv2.dll into OUTDIR first.
    here = fileparts(mfilename('fullpath'));
    inc = fullfile(here, '..', '..', '..', '..', 'include');
    src = fullfile(here, 'ysp_av_probe.c');
    args = {['-I' inc], ['-I' fullfile(sdl, 'include')], ['-I' ma], src};
    if exist('OCTAVE_VERSION', 'builtin')
        % MinGW's ld 2.43 aborts on SDL 3.4's MSVC import library
        % (ldlang.c:527), so it links against the SDL3.dll already in
        % OUTDIR: ld finds SDL3.dll for -lSDL3 when no .lib is there.
        mex(args{:}, ['-L' outdir], '-lSDL3', '-luser32', '-o', fullfile(outdir, 'ysp_av_probe'));
    else
        mex(args{:}, ['-L' fullfile(sdl, 'lib', 'x64')], '-lSDL3', '-luser32', '-outdir', outdir);
    end
end
