function build(name)
%BUILD  Compile the ysp MEX bindings (MATLAB or Octave).
%
%   build()             builds every ysp_*.c in this directory
%   build('parallel')   builds only ysp_parallel.c
%   build('quest')      builds only ysp_quest.c
%   build('rt')         builds only ysp_rt.c
%
%   Run it from any directory:  run('.../bindings/mex/build.m')
%   Each MEX file is written next to its source as ysp_<name>.<mexext>.
%
%   Octave needs the development package (e.g. `octave-dev`) for `mex`.
%   MATLAB needs a configured C compiler (`mex -setup`).

    here = fileparts(mfilename('fullpath'));
    root = fullfile(here, '..', '..', 'include'); % include/ysp/ holds the headers
    share = fullfile(here, '..', 'share');        % the shared tables (ysp_audio, ysp_timeline)
    % ysp_audio.c needs miniaudio.h 0.11.25 (CMakeLists.txt's YSP_FETCH_MINIAUDIO
    % pin): from YSP_MINIAUDIO_DIR, or in bindings/mex/miniaudio/.
    ma = getenv('YSP_MINIAUDIO_DIR');
    if isempty(ma) || ~exist(fullfile(ma, 'miniaudio.h'), 'file')
        ma = fullfile(here, 'miniaudio');
    end

    if nargin < 1
        d = dir(fullfile(here, 'ysp_*.c'));
        srcs = {d.name};
    else
        srcs = {sprintf('ysp_%s.c', name)};
    end
    if isempty(srcs)
        error('ysp:build', 'no ysp_*.c sources found in %s', here);
    end

    % Per-source platform gates, mirroring YSP_PLATFORMS_<lib> in
    % CMakeLists.txt. A header #errors on a platform it does not support,
    % which would fail the whole build; skip it with a message instead.
    % The adaptive-method sources (ysp_stair.c, ysp_quest.c, ysp_aep.c,
    % ysp_trials.c) are pure computation and build everywhere; ysp_quest.c
    % and ysp_aep.c define YQST_ASYNC / YAEP_ASYNC themselves, so they
    % compile ysp/rt.h and need only the thread library below. ysp_rt.c
    % (ysp/rt.h itself) is the same: no gate, the thread library only.
    % ysp_pack.c compiles ysp/pack.h and the ysp/json.h it requires; it maps
    % files through the OS (Windows, POSIX) and needs no library.
    % ysp_response.c (ysp/response.h with ysp/input.h) is pure computation
    % too and needs no library.
    % ysp-vis (ysp_vis.c, the core, and the thin ysp_screen.c, ysp_gfx.c,
    % ysp_video.c; docs/bindings.md) is Windows only: the other screen
    % backends are stubs.
    unsupported = {'ysp_parallel.c', ismac; 'ysp_vis.c', ~ispc; 'ysp_screen.c', ~ispc; ...
                   'ysp_gfx.c', ~ispc; 'ysp_video.c', ~ispc};
    % ysp_vis.c links a static SDL3 3.4.0 and gets ANGLE's libEGL.dll and
    % libGLESv2.dll beside it, all from third_party/vis/, which
    % tools/vendor_vis.py makes: --sdl msvc for MATLAB, --sdl mingw with
    % Octave's own gcc for Octave.
    vis = fullfile(here, '..', '..', 'third_party', 'vis');
    is_octave = exist('OCTAVE_VERSION', 'builtin') ~= 0;
    if is_octave
        vis_sdl = fullfile(vis, 'sdl3', 'mingw');
        vis_lib = 'libSDL3.a';
        vis_tc = 'mingw --mingw-bin <Octave>/mingw64/bin';
    else
        vis_sdl = fullfile(vis, 'sdl3', 'msvc');
        vis_lib = 'SDL3-static.lib';
        vis_tc = 'msvc';
    end

    % mex writes its output to the current directory; build next to the
    % sources so the result lands in bindings/mex regardless of the caller's cwd.
    old = cd(here);
    cleanup = onCleanup(@() cd(old));   %#ok<NASGU>

    for i = 1:numel(srcs)
        k = find(strcmp(unsupported(:, 1), srcs{i}), 1);
        if ~isempty(k) && unsupported{k, 2}
            fprintf('skipping %s: not supported on this platform\n', srcs{i});
            continue;
        end
        if strcmp(srcs{i}, 'ysp_vis.c') && (~exist(fullfile(vis_sdl, 'lib', vis_lib), 'file') || ...
                                            ~exist(fullfile(vis, 'angle', 'libEGL.dll'), 'file') || ...
                                            ~exist(fullfile(vis, 'pl_mpeg', 'pl_mpeg.h'), 'file'))
            fprintf('skipping ysp_vis.c: no static SDL3, ANGLE or pl_mpeg in %s (tools/vendor_vis.py --write --angle --pl-mpeg --sdl %s)\n', vis, vis_tc);
            continue;
        end
        if strcmp(srcs{i}, 'ysp_audio.c') && ~exist(fullfile(ma, 'miniaudio.h'), 'file')
            fprintf('skipping ysp_audio.c: no miniaudio.h (set YSP_MINIAUDIO_DIR or put it in %s)\n', fullfile(here, 'miniaudio'));
            continue;
        end
        args = {['-I' root], ['-I' share], srcs{i}};
        % The owners of a shared table export ysp_<name>_api_table() too
        % (docs/bindings.md, "Sharing one implementation across modules").
        % MATLAB on Linux and macOS links a MEX file with a list that exports
        % mexFunction alone; this list adds the table function.
        if any(strcmp(srcs{i}, {'ysp_audio.c', 'ysp_timeline.c'})) && ~ispc && ~exist('OCTAVE_VERSION', 'builtin')
            sym = [srcs{i}(1:end-2) '_api_table'];
            lst = [tempname() '.map'];
            fid = fopen(lst, 'w');
            if ismac
                fprintf(fid, '_mexFunction\n_mexfilerequiredapiversion\n_%s\n', sym);
                flag = ['-Wl,-exported_symbols_list,' lst];
            else
                fprintf(fid, 'MEX { global: mexFunction; mexfilerequiredapiversion; %s; local: *; };\n', sym);
                flag = ['-Wl,--version-script,' lst];
            end
            fclose(fid);
            args(end+1:end+2) = {['LINKEXPORT=' flag], ['LINKEXPORTVER=' flag]};   %#ok<AGROW>
        end
        if strcmp(srcs{i}, 'ysp_audio.c')
            args{end+1} = ['-I' ma];   %#ok<AGROW>
            if ~ispc
                % miniaudio loads its backends with dlopen.
                args(end+1:end+2) = {'-ldl', '-lm'};   %#ok<AGROW>
            end
        end
        if strcmp(srcs{i}, 'ysp_vis.c')
            % The system libraries SDL3.dll imports itself (tools/vendor_vis.py).
            sys = {'-luser32', '-lgdi32', '-lwinmm', '-limm32', '-lole32', '-loleaut32', '-lversion', ...
                   '-luuid', '-ladvapi32', '-lsetupapi', '-lshell32', '-ldinput8'};
            args = [args, {['-I' fullfile(vis_sdl, 'include')], ['-I' fullfile(vis, 'pl_mpeg')], ['-L' fullfile(vis_sdl, 'lib')]}];   %#ok<AGROW>
            if is_octave
                % Stripped: about 10 MB to 5 MB (docs/winthread_probe.md, section 6).
                args = [args, {'-lSDL3'}, sys, {'-s'}];   %#ok<AGROW>
            else
                args = [args, {'-lSDL3-static'}, sys];   %#ok<AGROW>
            end
            % Beside the core: ysp/screen.h loads libEGL.dll from the core's own
            % directory (desc.angle_dir), and libEGL loads libGLESv2.dll from its own.
            copyfile(fullfile(vis, 'angle', 'libEGL.dll'), here);
            copyfile(fullfile(vis, 'angle', 'libGLESv2.dll'), here);
            copyfile(fullfile(vis, 'angle', 'LICENSE.ANGLE.txt'), fullfile(here, 'ysp_vis-LICENSE.ANGLE.txt'));
            copyfile(fullfile(vis, 'sdl3', 'LICENSE.txt'), fullfile(here, 'ysp_vis-LICENSE.SDL3.txt'));
            copyfile(fullfile(vis, 'pl_mpeg', 'LICENSE.txt'), fullfile(here, 'ysp_vis-LICENSE.pl_mpeg.txt'));
        end
        if ispc
            % Windows async-pulse workers use Win32 threads, so no thread
            % library. ysp/serial.h does need SetupAPI and the registry for
            % port enumeration, and its #pragma comment(lib) reaches MSVC
            % only: MinGW Octave and MATLAB's MinGW-w64 add-on would fail to
            % link with undefined SetupDi*. mex takes -l<name> on both
            % toolchains (it becomes <name>.lib for MSVC), so one spelling
            % covers them.
            % ysp_device.c and ysp_rigfile.c compile ysp/serial.h too.
            if any(strcmp(srcs{i}, {'ysp_serial.c', 'ysp_device.c', 'ysp_rigfile.c'}))
                args(end+1:end+2) = {'-lsetupapi', '-ladvapi32'};   %#ok<AGROW>
            end
        else
            % The async-pulse workers, the async inference threads of
            % ysp_quest.c and ysp_aep.c, and ysp/rt.h's worker and pump in
            % ysp_rt.c use pthreads on Linux/macOS.
            args{end+1} = '-lpthread';   %#ok<AGROW>
        end
        mex(args{:});
        fprintf('built %s (%s) in %s\n', srcs{i}, mexext, here);
    end
end
