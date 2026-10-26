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
    unsupported = {'ysp_parallel.c', ismac};

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
        args = {['-I' root], srcs{i}};
        if ispc
            % Windows async-pulse workers use Win32 threads, so no thread
            % library. ysp/serial.h does need SetupAPI and the registry for
            % port enumeration, and its #pragma comment(lib) reaches MSVC
            % only: MinGW Octave and MATLAB's MinGW-w64 add-on would fail to
            % link with undefined SetupDi*. mex takes -l<name> on both
            % toolchains (it becomes <name>.lib for MSVC), so one spelling
            % covers them.
            if strcmp(srcs{i}, 'ysp_serial.c')
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
