function test_mex_vis_gfx()
%TEST_MEX_VIS_GFX  ysp_gfx in MATLAB or Octave, on the simulated display (no
%   window, no GPU: gfx's null backend draws nothing). Checks the stimuli
%   and their parameters against ysp/gfx.h's constructors and tables, the
%   refusals, draw lists and bulk sets, groups, instances and their
%   elements, dot fields, textures, the timeline bindings (a tween drives a
%   parameter in C; a rewind and a seek replay the same values), hit tests,
%   the gfx started while the screen settles, the frame without records
%   and the row frame, and the lifetime rules (closing a gfx or its screen
%   frees its objects' handles; 'free' refuses what is in use).
%   Build first (build.m: ysp_audio, ysp_timeline, ysp_vis, ysp_screen,
%   ysp_gfx, ysp_video). Windows only. Prints PASS with the number of checks.

    global YSP_GFX_CHECKS %#ok<GVMIS>
    YSP_GFX_CHECKS = 0;
    here = fileparts(mfilename('fullpath'));
    addpath(here);
    P = 0.004;
    sim = {'backend', 'sim', 'sim_period', P};

    % ---- the frame: no records unless asked; the row form
    [bv, gv] = ysp_gfx('version');
    check(strcmp(bv, '1.1.0') && ~isempty(regexp(gv, '^\d+\.\d+\.\d+$', 'once')), 'version: binding %s, gfx.h %s', bv, gv);
    h = ysp_screen('open', sim{:});
    for i = 1:3, ysp_screen('begin', h); ysp_screen('flip', h); end
    [f, quit, done] = ysp_screen('begin', h);
    check(isstruct(f) && isempty(quit) && ~isfield(f, 'done') && ~isfield(f, 'last') && isfield(f, 'n_done'), ...
          'begin: the frame without records');
    check(isstruct(done) && numel(done) == f.n_done && isfield(done, 'onset_ns'), 'begin: the records as the third output');
    ysp_screen('flip', h);
    ysp_screen('close', h);
    h = ysp_screen('open', sim{:}, 'frame', 'row');
    r = ysp_screen('begin', h);
    check(isa(r, 'double') && isequal(size(r), [1 6]) && abs(r(2) - P) < 1e-12 && r(3) == 0, 'frame row: [onset period index vblank n_done done_lost]');
    ysp_screen('flip', h);
    ysp_screen('close', h);
    expect_error('ysp_screen:arg', @() ysp_screen('open', sim{:}, 'frame', 'table'));

    % ---- the gfx and its options
    h = ysp_screen('open', sim{:});
    expect_error('ysp_gfx:arg', @() ysp_gfx('open', h, 'units', 'mm'));
    expect_error('ysp_gfx:arg', @() ysp_gfx('open', h, 'colour', 1));
    g = ysp_gfx('open', h, 'background', 0.5, 'cache', false);
    in = ysp_gfx('info', g);
    check(in.open && isequal(in.size, [800 600]) && isequal(in.center, [400 300]) && isempty(in.cache), 'info: size, center, no cache');
    check(~isempty(strfind(ysp_gfx('describe', g), 'program cache off')), 'describe: cache off');

    % ---- every shape kind
    S = {'rect', {'w', 100, 'h', 50, 'radius', 8}; 'circle', {'w', 40}; 'annulus', {'w', 60, 'inner', 20};
         'line', {'w', 80, 'width', 4}; 'polygon', {'vertices', [-20 -10; 20 -10; 0 20]};
         'cross', {'w', 30, 'width', 4}; 'rrect', {'w', 100, 'h', 60, 'radii', [2 4 6 8]};
         'arc', {'w', 80, 'sweep', 270, 'thickness', 6, 'start', 45}; 'pie', {'w', 80, 'sweep', 90};
         'capsule', {'w', 80, 'radii', [10 6]}; 'ngon', {'w', 60, 'sides', 6}; 'star', {'w', 80, 'points', 5, 'inner', 0.4};
         'ellipse', {'w', 80, 'h', 40}; 'qbezier', {'w', 100, 'h', 60, 'path', [-40 20; 0 -40; 40 20], 'width', 3};
         'polyline', {'w', 100, 'h', 60, 'path', [-40 20; 0 -20; 40 20; 60 0], 'width', 3}};
    hs = zeros(1, size(S, 1), 'uint64');
    for i = 1:size(S, 1)
        hs(i) = ysp_gfx('shape', g, S{i, 1}, 'color', [1 0.5 0], S{i, 2}{:});
        check(strcmp(ysp_gfx('get', hs(i), 'shape'), S{i, 1}) && strcmp(ysp_gfx('get', hs(i), 'kind'), 'shape'), 'shape %s', S{i, 1});
    end
    ysp_screen('begin', h);
    ysp_gfx('frame', g, hs);
    ysp_screen('flip', h);
    check(isequal(ysp_gfx('get', hs(7), 'radii'), [2 4 6 8]), 'rrect radii');
    check(ysp_gfx('get', ysp_gfx('shape', g, 'star', 'w', 80, 'points', 5), 'inner') == 0.5, 'star: inner 0 in the desc is 0.5');
    check(ysp_gfx('get', ysp_gfx('shape', g, 'capsule', 'w', 80, 'radii', [10 6]), 'h') == 20, 'capsule: h from the radii');
    check(ysp_gfx('get', hs(5), 'w') == 40 && ysp_gfx('get', hs(5), 'h') == 40, 'polygon: the vertices'' extent');

    % ---- parameters, set and get
    r = ysp_gfx('shape', g, 'rect', 'w', 100, 'h', 40);
    ysp_gfx('set', r, 'x', 12.5, 'ori', 45, 'color', 0.25, 'anchor', 'top_left', 'edge', 'gaussian', 'edge_width', 1.5);
    check(ysp_gfx('get', r, 'x') == 12.5 && ysp_gfx('get', r, 'ori') == 45, 'set/get: x, ori');
    check(isequal(ysp_gfx('get', r, 'color'), [0.25 0.25 0.25]), 'a gray fills color');
    check(isequal(ysp_gfx('get', r, 'anchor'), [0 0]) && strcmp(ysp_gfx('get', r, 'edge'), 'gaussian'), 'anchor by name; edge by name');
    ysp_gfx('set', r, 'anchor', [0.25 0.75]);
    check(isequal(ysp_gfx('get', r, 'anchor'), [0.25 0.75]), 'anchor as [ax ay]');
    pa = ysp_gfx('get', r);
    check(isstruct(pa) && isfield(pa, 'color') && isfield(pa, 'edge') && ~isfield(pa, 'contrast'), 'get: every parameter of a shape');
    expect_error('ysp_gfx:arg', @() ysp_gfx('set', r, 'contrast', 0.5));      % a modulation parameter
    expect_error('ysp_gfx:arg', @() ysp_gfx('set', r, 'nope', 1));
    expect_error('ysp_gfx:arg', @() ysp_gfx('set', r, 'x', NaN));
    expect_error('ysp_gfx:arg', @() ysp_gfx('set', r, 'color', [1 0]));
    expect_error('ysp_gfx:arg', @() ysp_gfx('set', r, 'edge', 'blurry'));
    expect_error('ysp_gfx:arg', @() ysp_gfx('shape', g, 'hexagon'));
    expect_error('ysp_gfx:arg', @() ysp_gfx('shape', g, 'polygon'));
    expect_error('ysp_gfx:arg', @() ysp_gfx('gabor', g, 'sigma', 10, 'w', 50));
    expect_error('ysp_gfx:arg', @() ysp_gfx('set', r, 'color', '#ff0000'));  % needs a calibration
    expect_error('ysp_gfx:arg', @() ysp_gfx('color', g, 'srgb', [1 0 0]));
    check(isequal(ysp_gfx('color', g, 'rgb', [0.5 0.25 0]), [0.5 0.25 0]), 'color: rgb passes without a calibration');

    % ---- the other kinds
    gr = ysp_gfx('grating', g, 'sf', 0.05, 'w', 200, 'aperture', 'circle', 'edge', 'cosine', 'edge_width', 8, 'contrast', 0.3);
    ga = ysp_gfx('gabor', g, 'sf', 0.05, 'sigma', 10, 'contrast', 0);
    no = ysp_gfx('noise', g, 'w', 64, 'check', 4, 'seed', 7, 'dist', 'binary');
    check(strcmp(ysp_gfx('get', gr, 'aperture'), 'circle') && ysp_gfx('get', gr, 'h') == 200, 'grating');
    check(ysp_gfx('get', ga, 'contrast') == 0 && ysp_gfx('get', ga, 'w') == 0, 'gabor: a zero contrast is zero; w 0 = 8 sigma');
    check(strcmp(ysp_gfx('get', no, 'dist'), 'binary'), 'noise');
    rgb = zeros(32, 64, 3, 'uint8');
    T = ysp_gfx('texture', g, rgb);
    im = ysp_gfx('image', g, T, 'x', 10);
    check(ysp_gfx('get', im, 'w') == 64 && ysp_gfx('get', im, 'h') == 32 && ysp_gfx('get', im, 'tex') == T, 'image: w, h from the texels');
    F = ysp_gfx('texture', g, ones(16, 16, 'single'));
    mo = ysp_gfx('image', g, F, 'modulation', true, 'contrast', 0.5);
    check(ysp_gfx('get', mo, 'modulation') == true, 'image: modulation');
    ysp_gfx('update', T, 255 * ones(32, 64, 3, 'uint8'));
    ysp_gfx('update', T, zeros(2, 2, 4, 'uint8'), [10 10]);
    expect_error('ysp_gfx:arg', @() ysp_gfx('update', T, zeros(2, 2, 4, 'uint8'), [63 0]));
    expect_error('ysp_gfx:arg', @() ysp_gfx('texture', g, zeros(4, 4, 'int8')));
    d = ysp_gfx('dots', g, 'n', 100, 'dot_size', 4, 'color', 1, 'aperture', 'circle', 'w', 200);
    ysp_gfx('set', d, 'positions', rand(60, 2) * 200 - 100);
    check(ysp_gfx('get', d, 'count') == 60 && ysp_gfx('get', d, 'n') == 100, 'dots: positions set the count');
    expect_error('ysp_gfx:arg', @() ysp_gfx('set', d, 'positions', zeros(101, 2)));
    ysp_screen('begin', h);
    ysp_gfx('begin', g);
    expect_error('ysp_gfx:order', @() ysp_gfx('set', d, 'positions', zeros(10, 2)));
    expect_error('ysp_gfx:order', @() ysp_gfx('update', T, rgb));
    ysp_gfx('draw', g, [gr ga no im mo d]);
    ysp_gfx('end', g);
    ysp_screen('flip', h);

    % ---- lists and bulk sets
    items = zeros(1, 100, 'uint64');
    for i = 1:100, items(i) = ysp_gfx('gabor', g, 'sf', 0.05, 'sigma', 8); end
    L = ysp_gfx('list', g, items);
    check(ysp_gfx('count', L) == 100, 'list: 100 items');
    xs = linspace(-300, 300, 100);
    ysp_gfx('set', L, 'x', xs);
    check(max(abs(ysp_gfx('get', L, 'x') - double(single(xs(:))))) == 0, 'list: x of every item in one call');
    ysp_gfx('set', L, 'contrast', 0.25);
    check(all_equal(ysp_gfx('get', L, 'contrast'), 0.25), 'list: one value for all');
    ysp_gfx('set', L, 'dir', repmat([0.1 0.2 0.3], 100, 1));
    dd = ysp_gfx('get', L, 'dir');
    check(isequal(size(dd), [100 3]) && abs(dd(50, 2) - 0.2) < 1e-7, 'list: n x 3');
    expect_error('ysp_gfx:arg', @() ysp_gfx('set', L, 'x', [1 2 3]));
    ysp_screen('begin', h);
    ysp_gfx('frame', g, L);
    ysp_screen('flip', h);
    ysp_gfx('remove', L, 100);
    check(ysp_gfx('count', L) == 99, 'list: remove');
    expect_error('ysp_gfx:busy', @() ysp_gfx('free', items(1)));       % in a list
    ysp_gfx('free', items(100));                                        % not in it any more
    expect_error('ysp_gfx:handle', @() ysp_gfx('get', items(100), 'x'));

    % ---- groups and hit tests
    G = ysp_gfx('group', g, 'x', 100, 'ori', 90, 'scale', 2);
    m = ysp_gfx('shape', g, 'rect', 'w', 10, 'x', 10, 'group', G);
    check(max(abs(ysp_gfx('resolve', m) - [500 320])) < 1e-3, 'group: a member''s anchor through the group');
    ysp_gfx('set', G, 'x', 0);
    check(ysp_gfx('get', G, 'x') == 0 && max(abs(ysp_gfx('resolve', m) - [400 320])) < 1e-3, 'group: set');
    expect_error('ysp_gfx:busy', @() ysp_gfx('free', G));               % a member uses it
    a = ysp_gfx('shape', g, 'rect', 'w', 100, 'h', 100);
    b = ysp_gfx('shape', g, 'circle', 'w', 40, 'x', 30);
    check(ysp_gfx('hit', a, 400, 300) && ~ysp_gfx('hit', a, 460, 300), 'hit: inside, outside');
    H = ysp_gfx('list', g, [a b]);
    check(isequal(ysp_gfx('hit', H, 430, 300), [2 0]) && isequal(ysp_gfx('hit', H, 0, 0), [0 0]), 'hit: the topmost item, 1-based');
    check(isequal(ysp_gfx('bounds', a), [350 250 450 350]), 'bounds');

    % ---- instances and their elements
    t = ysp_gfx('gabor', g, 'sf', 0.05, 'sigma', 5, 'size', 20);
    I = ysp_gfx('instances', g, t, 3, 'fields', {'xy', 'gate'});
    ysp_gfx('elements', I, 'xy', [-100 0; 0 0; 100 0]);
    E = ysp_gfx('elements', I);
    check(isa(E, 'single') && isequal(size(E), [8 3]) && isequal(E(1, :), single([-100 0 100])) && all(E(7, :) == 1), ...
          'elements: 8 x n, x row and gate 1');
    check(ysp_gfx('hit', I, 500, 300) == 3 && ysp_gfx('hit', I, 450, 300) == 0, 'hit: an element, 1-based');
    ysp_gfx('elements', I, 'gate', 0, 3);
    check(ysp_gfx('hit', I, 500, 300) == 0, 'hit: gate 0 is not hit');
    expect_error('ysp_gfx:arg', @() ysp_gfx('elements', I, 'ori', 1));
    expect_error('ysp_gfx:arg', @() ysp_gfx('instances', g, t, 4, 'fields', {'xy', 'color'}));
    ysp_screen('begin', h);
    ysp_gfx('frame', g, I);
    ysp_screen('flip', h);

    % ---- a bound tween drives a parameter; a rewind and a seek replay it
    tl = ysp_timeline('open', struct('channels', 2));
    ysp_timeline('set_keys', tl, 1, 1, [0 0; 0.2 1]);
    ysp_timeline('set_keys', tl, 2, 1, [0 -100; 0.2 100]);
    gb = ysp_gfx('gabor', g, 'sf', 0.05, 'sigma', 10);
    ysp_gfx('bind', g, gb, 'contrast', tl, 1);
    ysp_gfx('bind', g, gb, 'x', tl, 2);
    ysp_gfx('timeline', g, tl, true);
    in = ysp_gfx('info', g);
    check(in.bindings == 2 && in.timelines == 1 && ysp_timeline('pins', tl) == 1, 'bind: two bindings, one pin');
    expect_error('ysp_timeline:busy', @() ysp_timeline('close', tl));
    [t1, v1] = run_tween(h, g, tl, gb, 60);
    k = min(t1 / 0.2, 1);
    check(max(abs(v1(:, 1) - k)) < 1e-6 && max(abs(v1(:, 2) - (-100 + 200 * k))) < 1e-4, ...
          'the stimulus follows the channels, written in C each frame');
    [t2, v2] = run_tween(h, g, tl, gb, 60);
    [c, i1, i2] = intersect(round(t1 * 1e9), round(t2 * 1e9));
    check(numel(c) >= 30 && isequal(v1(i1, :), v2(i2, :)), 'a rewind replays the same values, bit for bit');
    m2 = i1(floor(end / 2));
    f = ysp_screen('begin', h);
    ysp_timeline('skip', tl, 1, f.onset, t1(m2));
    ysp_gfx('frame', g, gb);
    ysp_screen('flip', h);
    check(isequal([ysp_gfx('get', gb, 'contrast') ysp_gfx('get', gb, 'x')], v1(m2, :)), 'a seek gives the same values');
    check(ysp_gfx('unbind', g, gb, 'x') == 1, 'unbind one name');
    ysp_gfx('timeline', g, tl, false);
    check(ysp_gfx('unbind', g, [], '', tl) == 1 && ysp_timeline('pins', tl) == 0, 'unbind the timeline: the pin goes');
    % a list (one channel per item), a group, elements
    LL = ysp_gfx('list', g, [a b]);
    ysp_gfx('bind', g, LL, 'x', tl, [1 2]);
    ysp_gfx('bind', g, G, 'y', tl, 2);
    ysp_gfx('bind', g, I, 'gate', tl, [1 1], 2);
    in = ysp_gfx('info', g);
    check(in.bindings == 5, 'bind: a list, a group, elements');
    expect_error('ysp_gfx:arg', @() ysp_gfx('bind', g, a, 'color', tl, 1));   % 3 channels
    expect_error('ysp_gfx:arg', @() ysp_gfx('bind', g, a, 'edge', tl, 1));    % enums do not bind
    f = ysp_screen('begin', h);
    ysp_timeline('anchor', tl, 1, f.onset);
    ysp_timeline('evaluate', tl, f.onset + 0.1, f.period);
    ysp_gfx('frame', g, LL, I);
    ysp_screen('flip', h);
    E = ysp_gfx('elements', I);
    check(abs(ysp_gfx('get', a, 'x') - 0.5) < 1e-6 && abs(ysp_gfx('get', G, 'y')) < 1e-4 && abs(E(7, 2) - 0.5) < 1e-6, ...
          'the values the script evaluated, written in begin');
    expect_error('ysp_gfx:busy', @() ysp_gfx('free', b));   % in two lists
    ysp_gfx('free', LL);
    ysp_gfx('free', H);
    ysp_gfx('free', b);                                     % its binding goes with it
    in = ysp_gfx('info', g);
    check(in.bindings == 4, 'free: a bound stimulus takes its binding');

    % ---- the parameter tables, capture on the null backend
    p = ysp_gfx('params', 'gabor');
    names = {p.name};
    check(any(strcmp(names, 'sigma')) && ~any(strcmp(names, 'stroke')), 'params: the rows a gabor reads');
    dir0 = tempname();
    ysp_gfx('capture_start', g, 'path', fullfile(dir0, 'f'), 'every', 2);
    for i = 1:4, ysp_screen('begin', h); ysp_gfx('frame', g, a); ysp_screen('flip', h); end
    cs = ysp_gfx('capture_stop', g);
    check(cs.chosen == 2 && cs.written == 2 && numel(dir(fullfile(dir0, 'f*.ppm'))) == 2, 'capture: every 2nd frame to PPM (named by frame index)');
    delete(fullfile(dir0, '*.ppm'));
    rmdir(dir0);

    % ---- closing the gfx frees its objects' handles and gives the pins back
    ysp_gfx('close', g);
    expect_error('ysp_gfx:handle', @() ysp_gfx('get', a, 'x'));
    expect_error('ysp_gfx:handle', @() ysp_gfx('set', L, 'x', 0));
    expect_error('ysp_gfx:handle', @() ysp_gfx('get', G, 'x'));
    expect_error('ysp_gfx:handle', @() ysp_gfx('update', T, rgb));
    expect_error('ysp_gfx:handle', @() ysp_gfx('frame', g, a));
    check(ysp_timeline('pins', tl) == 0, 'close: the pins are given back');
    ysp_timeline('close', tl);

    % ---- the gfx started while the screen settles
    ysp_screen('close', h);
    h = ysp_screen('open', sim{:}, 'gfx', struct('background', 0.25, 'dither', 'ordered'));
    expect_error('ysp_gfx:arg', @() ysp_gfx('open', h, 'background', 0.5));
    g = ysp_gfx('open', h);
    check(~isempty(strfind(ysp_gfx('describe', g), 'dither ordered')), 'gfx option: the options given at the screen''s open');
    s1 = ysp_gfx('shape', g, 'circle', 'w', 10);
    ysp_screen('close', h);                     % the gfx and its objects go with the screen
    expect_error('ysp_gfx:handle', @() ysp_gfx('get', s1, 'x'));
    expect_error('ysp_gfx:handle', @() ysp_gfx('describe', g));
    h = ysp_screen('open', sim{:}, 'gfx', true);
    ysp_screen('close', h);                     % started, never opened: freed with the screen
    expect_error('ysp_screen:arg', @() ysp_screen('open', sim{:}, 'gfx', struct('nope', 1)));

    % ---- the last clear: every gfx, its objects and its pins
    tl = ysp_timeline('open', struct('channels', 1));
    h = ysp_screen('open', sim{:});
    g = ysp_gfx('open', h);
    s1 = ysp_gfx('shape', g, 'circle', 'w', 10);
    ysp_gfx('bind', g, s1, 'x', tl, 1);
    check(ysp_timeline('pins', tl) == 1, 'pinned');
    clear ysp_screen ysp_gfx ysp_video;
    info = ysp_vis('info');
    check(info.users == 0 && info.screens == 0 && info.gfx == 0, 'the last thin file gone: every object closed');
    check(ysp_timeline('pins', tl) == 0, 'and its timeline pins given back');
    ysp_timeline('close', tl);
    clear ysp_screen ysp_gfx ysp_video ysp_vis;

    fprintf('PASS test_mex_vis_gfx (%d checks)\n', YSP_GFX_CHECKS);
end

function [t, v] = run_tween(h, g, tl, gb, n)
    f = ysp_screen('begin', h);
    ysp_timeline('anchor', tl, 1, f.onset);
    t0 = f.onset_ns;
    ysp_gfx('frame', g, gb);
    ysp_screen('flip', h);
    t = zeros(n + 1, 1);
    v = zeros(n + 1, 2);
    v(1, :) = [ysp_gfx('get', gb, 'contrast') ysp_gfx('get', gb, 'x')];
    for i = 1:n
        f = ysp_screen('begin', h);
        ysp_gfx('frame', g, gb);
        ysp_screen('flip', h);
        t(i + 1) = double(f.onset_ns - t0) / 1e9;
        v(i + 1, :) = [ysp_gfx('get', gb, 'contrast') ysp_gfx('get', gb, 'x')];
    end
end

function ok = all_equal(v, x)
    ok = all(v(:) == x);
end

function check(cond, msg, varargin)
    global YSP_GFX_CHECKS %#ok<GVMIS>
    YSP_GFX_CHECKS = YSP_GFX_CHECKS + 1;
    if ~cond
        error('test_mex_vis_gfx:fail', ['FAIL: ' msg], varargin{:});
    end
end

function expect_error(id, f)
    global YSP_GFX_CHECKS %#ok<GVMIS>
    YSP_GFX_CHECKS = YSP_GFX_CHECKS + 1;
    try
        f();
    catch e
        if ~strcmp(e.identifier, id)
            error('test_mex_vis_gfx:fail', 'FAIL: expected %s, got %s: %s', id, e.identifier, e.message);
        end
        return;
    end
    error('test_mex_vis_gfx:fail', 'FAIL: expected %s, nothing was raised', id);
end
