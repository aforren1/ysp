function test_mex_vis_window()
%TEST_MEX_VIS_WINDOW  ysp-vis with a real window: local only, not in CI.
%   Opens a 1280 x 720 window with the bundled ANGLE, draws 2 s with the
%   phase A draw path, routes the window calls through the window thread,
%   pauses 6 s without a call (past the 4.1 to 4.6 s after which Windows
%   marks a window that pumps nothing as hung), draws again, then checks the
%   lifetime rule with a window open: a clear of ysp_gfx leaves the window
%   drawing, and the clear of the last thin file closes it. Then five
%   stimuli made through ysp_gfx draw the same scene, bit for bit, as the
%   same stimuli made in C with ysp/gfx.h's constructors, and instances
%   match single stimuli. About 20 s. Prints PASS with the
%   number of checks.

    global YSP_VISW_CHECKS %#ok<GVMIS>
    YSP_VISW_CHECKS = 0;
    addpath(fileparts(mfilename('fullpath')));
    clear ysp_screen ysp_gfx ysp_video;
    h = ysp_screen('open', 'windowed', true, 'size', [1280 720]);
    try
        g = ysp_gfx('open', h, 0.25);
        line = ysp_screen('describe', h);
        check(~isempty(strfind(line, '2.1.25848')), 'the bundled ANGLE: %s', line);
        ysp_screen('title', h, 'ysp-vis MEX test');
        ysp_screen('cursor', h, true);
        ysp_screen('grab', h, false);
        frames(h, g, 120, [1 1 1]);
        ysp_screen('wait_flip', h);
        r = ysp_screen('records', h);
        check(numel(r) == 120 && all(ismember([r.path], [1 2 3])) && all(ismember([r.tier], [1 2 3])), ...
              '120 records on a real path');
        fprintf('before the pause: paths %s, tiers %s\n', mat2str(unique([r.path])), mat2str(unique([r.tier])));
        pause(6);                                  % no call: the window thread keeps the window alive
        f = frames(h, g, 60, [0 1 0]);
        ysp_screen('wait_flip', h);
        r = ysp_screen('records', h);
        check(numel(r) == 60 && f.index == 179, '60 frames after the pause');
        fprintf('after the pause: first records [index dropped path tier flags]:\n');
        disp([[r(1:5).index]' [r(1:5).dropped]' [r(1:5).path]' [r(1:5).tier]' [r(1:5).flags]']);
        clear ysp_gfx;
        info = ysp_vis('info');
        check(info.screens == 1 && info.gfx == 1, 'clear ysp_gfx: the window and the gfx stay');
        frames(h, g, 30, [0 0 1]);
        check(true, 'drawing goes on after ysp_gfx loads again');
    catch e
        try, ysp_screen('close', h); catch, end
        rethrow(e);
    end
    clear ysp_screen ysp_gfx ysp_video;            % the last thin file gone: the window closes
    info = ysp_vis('info');
    check(info.users == 0 && info.screens == 0 && info.gfx == 0, 'the last clear closed the window');
    clear ysp_vis;
    pixel_check();
    fprintf('PASS test_mex_vis_window (%d checks)\n', YSP_VISW_CHECKS);
end

function f = frames(h, g, n, color)
    for i = 1:n
        f = ysp_screen('begin', h);
        ysp_gfx('begin', g);
        ysp_gfx('rect', g, [0 0 200 120], color);
        ysp_gfx('end', g);
        ysp_screen('flip', h);
    end
end

function check(cond, msg, varargin)
    global YSP_VISW_CHECKS %#ok<GVMIS>
    YSP_VISW_CHECKS = YSP_VISW_CHECKS + 1;
    if ~cond
        error('test_mex_vis_window:fail', ['FAIL: ' msg], varargin{:});
    end
end

function pixel_check()
    h = ysp_screen('open', 'windowed', true, 'size', [800 600]);
    try
        g = ysp_gfx('open', h, 'background', 0.5);
        tw = [ysp_gfx('gabor', g, 'x', -100, 'y', 40, 'ori', 30, 'sf', 1 / 16, 'phase', 0.25, 'sigma', 12, 'contrast', 0.4), ...
              ysp_gfx('grating', g, 'x', 80, 'y', -30, 'w', 120, 'ori', -20, 'sf', 1 / 20, 'contrast', 0.3, ...
                      'aperture', 'circle', 'edge', 'cosine', 'edge_width', 6), ...
              ysp_gfx('shape', g, 'rect', 'w', 160, 'h', 90, 'ori', 10, 'radius', 12, 'color', [0.9 0.2 0.1], ...
                      'stroke', 4, 'stroke_align', 'inside', 'edge', 'gaussian', 'edge_width', 1), ...
              ysp_gfx('shape', g, 'star', 'place', 'top_left', 'anchor', 'top_left', 'x', 20, 'y', 20, 'w', 100, ...
                      'points', 5, 'inner', 0.4, 'color', [0.1 0.8 0.3], 'opacity', 0.75), ...
              ysp_gfx('noise', g, 'x', 150, 'y', 60, 'w', 64, 'check', 4, 'seed', 7, 'dist', 'binary', ...
                      'contrast', 0.25, 'aperture', 'circle')];
        empty = scene(h, g, []);
        for k = 1:numel(tw)
            c = scene(h, g, -(k - 1));                 % C's scene k - 1
            b = scene(h, g, tw(k));
            check(~isequal(b, empty) && isequal(b, c), 'stimulus %d: the same scene, bit for bit, as the C path', k);
        end
        xy = [-150 -60; 0 0; 120 80];
        oris = [0 30 75];
        s3 = zeros(1, 3, 'uint64');
        for k = 1:3
            s3(k) = ysp_gfx('gabor', g, 'sf', 1 / 12, 'sigma', 8, 'x', xy(k, 1), 'y', xy(k, 2), 'ori', oris(k), 'contrast', 0.4);
        end
        L = ysp_gfx('list', g, s3);
        I = ysp_gfx('instances', g, ysp_gfx('gabor', g, 'sf', 1 / 12, 'sigma', 8, 'contrast', 0.4), 3, 'fields', {'xy', 'ori'});
        ysp_gfx('elements', I, 'xy', xy);
        ysp_gfx('elements', I, 'ori', oris);
        ysp_gfx('prime', g);
        a = scene(h, g, L);
        b = scene(h, g, I);
        check(max(abs(a(:) - b(:))) < 2e-5 && max(abs(a(:) - 0.5)) > 0.1, 'instances match single stimuli (within 2e-5)');
    catch e
        try, ysp_screen('close', h); catch, end
        rethrow(e);
    end
    ysp_screen('close', h);
end

% One frame's scene (h x w x 4 single): what draws it (handles), or C's
% scene -k for k <= 0.
function P = scene(h, g, what)
    ysp_screen('begin', h);
    ysp_gfx('begin', g);
    if isa(what, 'uint64')
        ysp_gfx('draw', g, what);
    elseif ~isempty(what)
        ysp_gfx('_c_scene', g, -what);
    end
    ysp_gfx('end', g);
    P = ysp_gfx('read_scene', g, [0 0 800 600]);
    ysp_screen('flip', h);
end
