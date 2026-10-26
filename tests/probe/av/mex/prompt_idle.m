function prompt_idle(bundle, idle, windowed, csv)
%PROMPT_IDLE  Leaves MATLAB at its prompt for IDLE s with the window open.
%   Run it with matlab -r (the desktop), not -batch: -batch exits when the
%   script ends. A timer ends the idle, runs 120 frames, closes and exits.
    addpath(bundle);
    av = @ysp_av_probe;
    av('elevate');
    fprintf('%s\n', av('open', windowed, '', 0));
    av('setup', 10);
    P = av('params');
    frames(av, P(1), 120);
    c = av('counts');
    av('watch_start');
    t = timer('StartDelay', idle, 'TimerFcn', @(~, ~) after(av, P(1), c, idle, csv));
    start(t);
    fprintf('at the prompt for %.0f s\n', idle);
end

function after(av, ori, c, idle, csv)
    fprintf('idle %.1f s at the prompt, watcher:\n%s', idle, av('watch_stop'));
    av('watch_start');
    frames(av, ori, 120);
    fprintf('after idle, watcher:\n%s', av('watch_stop'));
    fprintf('first frame after idle: index %d\n', c(1));
    av('close');
    fprintf('dumped %d\n', av('dump', csv, 'host=matlab-prompt'));
    av('free');
    exit;
end

function frames(av, ori, count)
    for k = 1:count
        f = av('begin');
        if isempty(f), return; end
        av('records');
        av('evaluate');
        av('gfx_begin');
        for i = 0:9
            av('set', i, ori, mod(f(1), 360));
            av('draw', i);
        end
        av('gfx_end');
        av('flip_at', f(2));
    end
end
