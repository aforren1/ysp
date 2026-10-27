function gfx_loop_mex(varargin)
%GFX_LOOP_MEX  N gabors with a bound tween in MATLAB or Octave (tests/probe/vis).
%   gfx_loop_mex('n', 1000, 'mode', 'list', 'seconds', 600, 'windowed', 0, 'csv', 'm.csv')
%   Each frame: ysp_screen('begin'), ysp_gfx('frame', g, scene),
%   ysp_screen('flip'): three calls, whatever N. The scene is N gabors
%   (sigma 6 px, 48 px across) on a grid, as one list of N stimuli ('list')
%   or one instances stimulus of N elements ('instances'). A ysp_timeline
%   drives the contrast (a yoyo tween, 0.1 to 0.5 in 1 s) and the phase (1
%   cycle/s) of every gabor through ysp_gfx('bind'), and the gfx evaluates
%   it at each frame's predicted onset: no call per stimulus. The screen
%   opens with frame='row': begin gives a row and the records as a matrix,
%   kept for the run as a script keeps its data. Writes one CSV row per flip
%   record (table.py). bindings/mex must be on the path. Shift+Esc ends a run.
    o = struct('n', 100, 'mode', 'list', 'seconds', 600, 'windowed', 0, 'csv', 'm.csv');
    for i = 1:2:numel(varargin), o.(varargin{i}) = varargin{i + 1}; end
    sp = 48;
    if o.windowed
        h = ysp_screen('open', 'windowed', true, 'size', [1280 720], 'frame', 'row', 'gfx', struct('background', 0.5));
    else
        h = ysp_screen('open', 'frame', 'row', 'gfx', struct('background', 0.5));
    end
    try
        g = ysp_gfx('open', h);
        ysp_screen('title', h, 'ysp-vis gfx probe');
        in = ysp_gfx('info', g);
        w = in.size(1); hh = in.size(2);
        cols = floor(w / sp);
        k = (0:o.n - 1)';
        xy = [mod(k, cols) * sp - w / 2 + sp / 2, floor(k / cols) * sp - hh / 2 + sp / 2];
        tl = ysp_timeline('open', struct('channels', 2));
        ysp_timeline('tween', tl, 1, 1, 0.5, 'start', 0, 'duration', 1, 'from', 0.1, 'cycles', Inf, 'yoyo', true, 'ease', 'cosine');
        ysp_timeline('tween', tl, 2, 1, o.seconds + 60, 'start', 0, 'duration', o.seconds + 60, 'from', 0, 'ease', 'linear');
        if strcmp(o.mode, 'list')
            hs = zeros(1, o.n, 'uint64');
            for i = 1:o.n, hs(i) = ysp_gfx('gabor', g, 'sf', 1 / 12, 'sigma', 6, 'x', xy(i, 1), 'y', xy(i, 2)); end
            scene = ysp_gfx('list', g, hs);
            ysp_gfx('bind', g, scene, 'contrast', tl, 1);   % one channel for every item
            ysp_gfx('bind', g, scene, 'phase', tl, 2);
        else
            t = ysp_gfx('gabor', g, 'sf', 1 / 12, 'sigma', 6);
            scene = ysp_gfx('instances', g, t, o.n, 'fields', {'xy'});
            ysp_gfx('elements', scene, 'xy', xy);
            ysp_gfx('bind', g, scene, 'contrast', tl, 1);   % the template's: every element
            ysp_gfx('bind', g, scene, 'phase', tl, 2);
        end
        ysp_gfx('timeline', g, tl, true);
        ysp_gfx('prime', g);
        line = sprintf('%s; gfx %s; n %d %s', ysp_screen('describe', h), ysp_gfx('version'), o.n, o.mode);
        n = ceil(o.seconds * 61) + 200;
        R = zeros(n, 15);
        k = 0;
        t_end = [];
        while true
            [f, quit, D] = ysp_screen('begin', h);    % f: [onset period index vblank n_done done_lost]
            if ~isempty(quit), fprintf('abort 0x%x\n', quit.reasons); break; end
            if isempty(t_end)
                ysp_timeline('anchor', tl, 1, f(1));
                t_end = f(1) + o.seconds;
            end
            m = size(D, 1);
            if m > 0, R(k + 1:k + m, :) = D; k = k + m; end
            if f(1) >= t_end
                ysp_screen('flip', h);
                break;
            end
            ysp_gfx('frame', g, scene);
            ysp_screen('flip', h);
        end
        ysp_screen('wait_flip', h);
        D = ysp_screen('records', h);
        last = -1;
        if k > 0, last = R(k, 1); end
        D = D(D(:, 1) > last, :);
        R(k + 1:k + size(D, 1), :) = D;
        k = k + size(D, 1);
        ysp_screen('close', h);
        ysp_timeline('close', tl);
    catch e
        % try/catch, not onCleanup: Octave did not run an onCleanup here
        % (docs/av_probe.md, section 4).
        try, ysp_screen('close', h); catch, end
        rethrow(e);
    end
    host = 'matlab';
    if exist('OCTAVE_VERSION', 'builtin'), host = 'octave'; end
    fid = fopen(o.csv, 'w');
    fprintf(fid, '# %s %s\n', host, line);
    fprintf(fid, 'index,target_ns,planned_ns,onset_ns,residual_ns,dropped,path,tier,flags,draw_ns,swap_ns\n');
    % the matrix gives seconds; ns from them (exact below 2^53 ns)
    for j = 1:k
        r = R(j, :);
        ph = r(10:15);
        dr = -1; sw = -1;
        if ~isnan(ph(3)), dr = round(ph(3) * 1e9); end
        if ~isnan(ph(5)), sw = round(ph(5) * 1e9); end
        fprintf(fid, '%d,%.0f,%.0f,%.0f,%.0f,%d,%d,%d,%d,%d,%d\n', r(1), r(2) * 1e9, r(3) * 1e9, r(4) * 1e9, r(5) * 1e9, ...
                r(6), r(7), r(8), r(9), dr, sw);
    end
    fclose(fid);
    fprintf('%d records -> %s\n', k, o.csv);
end
