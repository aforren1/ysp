function run_mex(bundle, varargin)
%RUN_MEX  The ysp-av probe's frame loop in MATLAB or Octave (docs/av_probe.md).
%   run_mex(bundle, 'windowed', 0, 'n', 10, 'seconds', 600, 'csv', path, ...
%           'idle', 0, 'idle_mode', 'pause', 'abort', 0, 'panic', 0, ...
%           'audio', '', 'angle_dir', '', 'preload', '', 'modules', 0)
%   BUNDLE holds ysp_av_probe.<mexext> with SDL3.dll, libEGL.dll and
%   libGLESv2.dll beside it. It goes on the MATLAB path only, not on PATH.
%   idle_mode: 'pause' (pause(s)) or 'java' (java.lang.Thread.sleep).
    o = struct('windowed', 0, 'n', 1, 'seconds', 5, 'csv', '', 'idle', 0, 'idle_mode', 'pause', ...
               'abort', 0, 'panic', 0, 'audio', '', 'angle_dir', '', 'preload', '', 'modules', 0, ...
               'heap', 100000, 'noghost', 0, 'prompt', 0);
    for k = 1:2:numel(varargin)
        o.(varargin{k}) = varargin{k + 1};
    end
    addpath(bundle);
    av = @ysp_av_probe;
    if ~isempty(o.preload)
        fprintf('preload %s: %d\n', o.preload, av('preload', o.preload));
    end
    if o.modules
        fprintf('modules before open:\n%s\n', av('modules'));
    end
    % A heap like an experiment's: a trial table made before the loop.
    trials = struct('trial', num2cell(1:o.heap), 'cond', 0, 'ori', 0, 'rt', [], 'resp', []); %#ok<NASGU>

    av('elevate');
    if o.noghost
        av('noghost');
    end
    fprintf('%s\n', av('open', o.windowed, o.angle_dir, o.panic));
    % try/catch, not onCleanup: in the Octave 10 smoke test an onCleanup
    % did not run at the end of this function and the window stayed open.
    try
        body(av, o);
    catch e
        finish(av, o);
        rethrow(e);
    end
    finish(av, o);
end

function body(av, o)
    av('setup', o.n);
    if ~isempty(o.audio)
        fprintf('audio %s: %s\n', o.audio, av('audio_open', strcmp(o.audio, 'null')));
    end
    if o.modules
        fprintf('modules after open:\n%s\n', av('modules'));
    end
    P = av('params');
    s = struct('av', av, 'n', o.n, 'ori', P(1), 'log', zeros(4096, 8), 'nlog', 0);
    if o.abort || o.panic
        [~, s] = frames(s, 60, Inf);
        av('keys', 1 + 2 * (o.panic ~= 0), 500, 300);
        if o.panic
            fprintf('hanging the interpreter for 10 s; 3 presses follow\n');
            pause(10);
            fprintf('panic did not end the process: %s keys %s', av('wt_state'), mat2str(av('keys_status'))); fprintf('%c', 10);
            [ok, s] = frames(s, Inf, av('now') + 2);
            fprintf('after the panic: loop ended by abort: %d abort_info %s', ~ok, mat2str(av('abort_info'))); fprintf('%c', 10);
        else
            [ok, s] = frames(s, Inf, av('now') + 5);
            fprintf('abort: loop ended by abort: %d abort_info %s keys %s\n', ~ok, ...
                    mat2str(av('abort_info')), mat2str(av('keys_status')));
        end
    elseif o.idle > 0
        [~, s] = frames(s, 120, Inf);
        c = av('counts');
        av('watch_start');
        if strcmp(o.idle_mode, 'pause')
            pause(o.idle);
        else
            java.lang.Thread.sleep(o.idle * 1000);
        end
        fprintf('idle %.1f s (%s), watcher:\n%s', o.idle, o.idle_mode, av('watch_stop'));
        av('watch_start');
        [~, s] = frames(s, 120, Inf);
        fprintf('after idle, watcher:\n%s', av('watch_stop'));
        fprintf('first frame after idle: index %d\n', c(1));
    else
        [~, s] = frames(s, Inf, av('now') + o.seconds);
    end
    fprintf('records in the log %d\n', s.nlog);
end

function [ok, s] = frames(s, count, t_end)
    ok = true;
    av = s.av;
    n = s.n;
    ORI = s.ori;
    k = 0;
    while k < count
        if av('now') >= t_end
            return;
        end
        f = av('begin');
        if isempty(f)
            ok = false;
            return;
        end
        r = av('records');
        m = size(r, 1);
        if s.nlog + m > size(s.log, 1)
            s.log(end + 4096, 8) = 0;   % grow in blocks, as a script would
        end
        s.log(s.nlog + 1:s.nlog + m, :) = r;
        s.nlog = s.nlog + m;
        av('evaluate');
        av('gfx_begin');
        v = mod(f(1), 360);
        for i = 0:n - 1
            av('set', i, ORI, v);
            av('draw', i);
        end
        av('gfx_end');
        av('flip_at', f(2));
        k = k + 1;
    end
end

function finish(av, o)
    av('close');
    if ~isempty(o.csv)
        if exist('OCTAVE_VERSION', 'builtin'), host = ['octave' OCTAVE_VERSION]; else, host = ['matlab' version('-release')]; end
        tag = sprintf('host=%s n=%d windowed=%d seconds=%.0f', host, o.n, o.windowed, o.seconds);
        fprintf('dumped %d frames to %s\n', av('dump', o.csv, tag), o.csv);
    end
    c = av('counts');
    fprintf('frames %d records %d lost %d\n', c(1), c(2), c(3));
    av('free');
end
