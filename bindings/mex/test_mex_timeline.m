function test_mex_timeline()
%TEST_MEX_TIMELINE  Key checks of the ysp_timeline MEX function. Runs in MATLAB
%   and in Octave. Prints PASS, or stops with the error of the first check
%   that failed. Build first (run build.m), then from this directory:
%   test_mex_timeline. tests/adapt/timeline_test.c checks the header itself;
%   this checks that the binding drives it as the C API does.

    global YSP_TL_CHECKS %#ok<GVMIS>
    YSP_TL_CHECKS = 0;
    here = fileparts(mfilename('fullpath'));
    addpath(here);
    [bv, hv] = ysp_timeline('version');
    fprintf('ysp_timeline %s (ysp/timeline.h %s)\n', bv, hv);

    P = 16666667;            % a 60 Hz period, ns
    T0 = 10e9;               % the first onset, ns
    on = @(i) (T0 + i * P) / 1e9;
    per = P / 1e9;
    FIX = 1; GRATING = 2; CONTRAST = 3; TRIAL = 1;

    % ---- the manual's trial by calls: values and landing records
    h = ysp_timeline('open', struct('channels', 3));
    build_trial(h);
    ysp_timeline('anchor', h, TRIAL, on(0));
    V = zeros(80, 3); EV = cell(80, 1);
    for i = 0:79
        [v, ev] = ysp_timeline('evaluate', h, on(i), per, i);
        V(i + 1, :) = v; EV{i + 1} = ev;
    end
    check(isequal(V(1, 1:2), [1 0]) && isequal(V(30, 1:2), [1 0]) && isequal(V(31, 1:2), [0 1]), 'fix and grating swap on frame 30');
    e = EV{31};
    check(numel(e) == 3 && isequal([e.kind], [3 2 1]) && isequal([e.target], [FIX GRATING 0]) && e(3).code == 12, 'frame 30: offset, onset, trigger');
    check(all([e.frame] == 30) && all([e.residual_ns] == int64(10)) && e(1).onset_ns == int64(T0 + 30 * P), 'landing records, exact ns');
    check(strcmp(e(3).kind_name, 'trigger') && e(1).time == 0.5, 'kind names and seconds');
    for i = 30:36
        u = min(max((i * P - 5e8) / 1e8, 0), 1);
        check(abs(V(i + 1, 3) - 0.5 * (1 - cos(pi * u)) / 2) < 1e-6, 'cosine ramp at frame %d', i);
    end
    check(V(72, 2) == 1 && V(73, 2) == 0 && V(73, 3) == 0, 'grating off at 1.2 s, frame 72');
    rec = ysp_timeline('events', h, TRIAL);
    check(numel(rec) == 5 && all(bitand([rec.flags], 1)), 'the record: every event fired');

    % ---- rewind replays bit for bit
    f1 = ysp_timeline('field', h, [0 0]);
    q = ysp_timeline('seq', h, TRIAL);
    ysp_timeline('tween_to', q, f1, [300 120], 0.4, 'cubic_out');
    ysp_timeline('at', q, 0.45);
    ysp_timeline('animate', q, f1, 'to', [0 0], 'duration', 0.3, 'ease', 'quad_in_out', 'yoyo', true, 'cycles', 2);
    ysp_timeline('at', q, 0.8);
    ysp_timeline('tween_set', q, f1, [5 6]);
    ysp_timeline('anchor', h, TRIAL, on(100));
    A = play(h, @(i) on(100 + i), per, 100);
    ysp_timeline('anchor', h, TRIAL, on(300));
    B = play(h, @(i) on(300 + i), per, 300);
    check(isequal(A.v, B.v) && isequal(A.f, B.f) && isequal(A.k, B.k) && isequal(A.r, B.r), 'a rewind replays values, fields and records');
    sets = ysp_timeline('events', h, TRIAL);
    sets = sets([sets.kind] == 7);
    check(numel(sets) == 1 && sets.user == uint64(f1) && sets.value == 5, 'a set''s record names its field');
    ysp_timeline('close', h);

    % ---- the op table gives the calls' trial; a seek gives continuous play
    a = ysp_timeline('open', struct('channels', 3));
    b = ysp_timeline('open', struct('channels', 3));
    build_trial(a);
    ops = {{'on', FIX}, {'wait', 0.5}, {'off', FIX}, {'on', GRATING}, {'trigger', 12}, ...
           {'to', CONTRAST, 0.5, 'duration', 0.1, 'ease', 'cosine'}, {'wait', 0.6}, ...
           {'then', CONTRAST, 0, 'duration', 0.1, 'ease', 'cosine'}, {'off', GRATING}};
    ysp_timeline('check_ops', b, TRIAL, ops);
    ysp_timeline('run', ysp_timeline('seq', b, TRIAL), ops);
    ysp_timeline('anchor', a, TRIAL, on(0));
    ysp_timeline('anchor', b, TRIAL, on(0));
    A = play(a, on, per, 0); B = play(b, on, per, 0);
    check(isequal(A.v, B.v) && isequal(A.k, B.k) && isequal(A.r, B.r), 'op table = calls');
    for k = [1 30 31 50]
        c = ysp_timeline('open', struct('channels', 3));
        build_trial(c);
        ysp_timeline('skip', c, TRIAL, on(k), k * P / 1e9);
        C = play(c, @(i) on(k + i), per, 0, 90 - k);
        check(isequal(C.v, A.v(k + 1:90, :)), 'a seek to frame %d gives continuous play', k);
        ysp_timeline('close', c);
    end
    bad = ops; bad{6} = {'to', CONTRAST, 0.5, 'duration', 0.1, 'cycles', 'forever'};
    expect_error('ysp_timeline:arg', @() ysp_timeline('check_ops', a, TRIAL, bad));
    try
        ysp_timeline('check_ops', a, TRIAL, bad);
    catch err
        check(~isempty(strfind(err.message, 'op 6')), 'check_ops names the op (1-based)');
    end
    ysp_timeline('close', a); ysp_timeline('close', b);

    % ---- fields: from, fromTo, int, stagger, callbacks as events, kill
    h = ysp_timeline('open', struct('channels', 0, 'fields', 8, 'tweens', 32));
    x = ysp_timeline('field', h, 2);
    k = ysp_timeline('field', h, 0, 'int');
    dots = ysp_timeline('fields', h, 3, 0);
    q = ysp_timeline('seq', h, TRIAL);
    ysp_timeline('tween_to', q, x, 12, 1.0);
    ysp_timeline('tween_to', q, k, 10, 1.0);
    ysp_timeline('animate', q, dots, 'to', 1, 'duration', 0.1, 'stagger', 0.1, 'on_complete', true);
    c = ysp_timeline('cursor', q);
    check(abs(c.prev_end - 0.3) < 1e-12, 'a stagger is one child from its first start to its last end');
    ysp_timeline('anchor', h, TRIAL, on(0));
    [~, ~, F] = ysp_timeline('evaluate', h, on(0), per);
    check(F(1, x) == 2, 'the rest value before the first write');
    [~, ev, F] = ysp_timeline('evaluate', h, on(30), per);
    check(abs(F(1, x) - (2 + 10 * 30 * P / 1e9)) < 1e-4 && F(1, k) == round(10 * 30 * P / 1e9), 'float and int fields');
    check(numel(ev) == 3 && all([ev.kind] == 6), 'on_complete events land in the report');
    check(isequal(ysp_timeline('field_value', h, dots(2)), 1), 'field_value');
    check(ysp_timeline('kill', h, x) == 1, 'kill');
    expect_error('ysp_timeline:arg', @() ysp_timeline('animate', q, dots([1 3]), 'to', 1, 'stagger', 0.1));
    expect_error('ysp_timeline:arg', @() ysp_timeline('tween_to', q, x, [1 2], 0.1));
    ysp_timeline('close', h);

    % ---- tracks, peek, rate, refusals
    h = ysp_timeline('open', struct('channels', 3, 'events', 4));
    ysp_timeline('set_keys', h, 1, TRIAL, [0 0 6 1; 1 10 0 0], [0 0 1 1]);
    ysp_timeline('set_track', h, 2, TRIAL, struct('samples', single(0:120), 'rate', 120));
    ysp_timeline('set_track', h, 3, TRIAL, struct('keys', [0 1 1; 2 * P / 1e9 0 1], 'period', 4 * P / 1e9));
    ysp_timeline('anchor', h, TRIAL, on(0));
    fl = zeros(1, 8);
    for i = 0:60
        v = ysp_timeline('evaluate', h, on(i), per, i);
        if i < 8, fl(i + 1) = v(3); end
    end
    check(abs(v(1) - 10 * 60 * P / 1e9) < 1e-4 && abs(v(2) - 120 * 60 * P / 1e9) < 1e-3, 'keyed (Bezier) and sampled tracks');
    check(isequal(fl, [1 1 0 0 1 1 0 0]), 'a repeating STEP flicker');
    id = ysp_timeline('add', h, struct('time', 1.5, 'kind', 'trigger', 'base', TRIAL, 'code', 4));
    pk = ysp_timeline('peek', h, TRIAL, on(61), on(61) + 1);
    check(numel(pk) == 1 && pk.id == id && pk.onset_ns == int64(T0 + 15e8), 'peek');
    ysp_timeline('rate', h, TRIAL, on(61), 1, 2);
    check(isequal(ysp_timeline('get_rate', h, TRIAL), [1 2]), 'rate');
    check(isempty(ysp_timeline('rt_time', h, 5, 1)), 'a stopped base has no RT time');
    expect_error('ysp_timeline:order', @() ysp_timeline('evaluate', h, on(0), per));
    expect_error('ysp_timeline:arg', @() ysp_timeline('anchor', h, TRIAL, -1));
    expect_error('ysp_timeline:arg', @() ysp_timeline('anchor', h, TRIAL, NaN));
    expect_error('ysp_timeline:bound', @() ysp_timeline('add', h, struct('time', 1, 'kind', 'onset', 'base', TRIAL, 'target', 1)));
    q = ysp_timeline('seq', h, TRIAL);
    expect_error('ysp_timeline:arg', @() ysp_timeline('on', q, 99));
    expect_error('ysp_timeline:arg', @() ysp_timeline('on', q, 1));          % sticky
    ysp_timeline('add', h, struct('time', {2, 3, 4}));
    expect_error('ysp_timeline:full', @() ysp_timeline('add', h, struct('time', 5)));
    ysp_timeline('close', h);
    expect_error('ysp_timeline:handle', @() ysp_timeline('values', h));
    expect_error('ysp_timeline:handle', @() ysp_timeline('on', q, 1));   % its sequences went with it

    fprintf('PASS (%d checks)\n', YSP_TL_CHECKS);
end

function build_trial(h)
    q = ysp_timeline('seq', h, 1);
    ysp_timeline('on', q, 1);
    ysp_timeline('wait', q, 0.5);
    ysp_timeline('off', q, 1);
    ysp_timeline('on', q, 2);
    ysp_timeline('trigger', q, 12);
    ysp_timeline('to', q, 3, 0.5, 'duration', 0.1, 'ease', 'cosine');
    ysp_timeline('wait', q, 0.6);
    ysp_timeline('then', q, 3, 0, 'duration', 0.1, 'ease', 'cosine');
    ysp_timeline('off', q, 2);
    ysp_timeline('release', q);
end

function R = play(h, on, per, index0, n)
    if nargin < 5, n = 90; end
    R.v = []; R.f = []; R.k = []; R.r = [];
    for i = 0:n - 1
        [v, ev, f] = ysp_timeline('evaluate', h, on(i), per, index0 + i);
        R.v(end + 1, :) = v;
        R.f(end + 1, :) = f(:)';
        for e = ev'
            R.k(end + 1, :) = [i, e.kind, e.code, e.target, e.frame - index0];
            R.r(end + 1, 1) = e.residual_ns;
        end
    end
end

function check(cond, msg, varargin)
    global YSP_TL_CHECKS %#ok<GVMIS>
    YSP_TL_CHECKS = YSP_TL_CHECKS + 1;
    if ~cond
        error('test_mex_timeline:fail', ['FAIL: ' msg], varargin{:});
    end
end

function expect_error(id, f)
    global YSP_TL_CHECKS %#ok<GVMIS>
    YSP_TL_CHECKS = YSP_TL_CHECKS + 1;
    try
        f();
    catch e
        if ~strcmp(e.identifier, id)
            error('test_mex_timeline:fail', 'FAIL: expected %s, got %s: %s', id, e.identifier, e.message);
        end
        return;
    end
    error('test_mex_timeline:fail', 'FAIL: expected %s, nothing was raised', id);
end
