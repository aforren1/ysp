function test_mex_rt()
%TEST_MEX_RT  Key checks of the ysp_rt MEX function. Runs in MATLAB and in
%   Octave. Prints PASS, or stops with the error of the first check that
%   failed. Build first (run build.m), then from this directory: test_mex_rt.
%   tests/adapt/rt_test.c checks the header itself; this checks the
%   conversions, the refusals and that the clock is the one the other ysp
%   MEX functions read.

    global YSP_RT_CHECKS %#ok<GVMIS>
    YSP_RT_CHECKS = 0;
    here = fileparts(mfilename('fullpath'));
    addpath(here);
    [bv, hv] = ysp_rt('version');
    fprintf('ysp_rt %s (ysp/rt.h %s)\n', bv, hv);
    check(strcmp(bv, '1.0.0') && strcmp(hv, '0.9.0'), 'versions');

    test_clock();
    test_waits();
    test_virtual();
    test_correlate();
    test_fit();
    test_user_dir();
    fprintf('PASS (%d checks)\n', YSP_RT_CHECKS);
end

function test_clock()
    a = ysp_rt('now_ns'); s = ysp_rt('now'); b = ysp_rt('now_ns');
    check(isa(a, 'int64') && isa(s, 'double'), 'now_ns is int64, now is double');
    check(a <= int64(round(s * 1e9)) + 1 && int64(round(s * 1e9)) <= b + 1, 'now and now_ns agree');
    check(abs(ysp_rt('real_now') - ysp_rt('now')) < 1, 'real_now on the real clock');
    % Each MEX file compiles its own copy of ysp/rt.h. They read one counter:
    % a read of the other function's now lies between two of ours.
    others = {'ysp_serial', 'ysp_parallel'};
    for k = 1:numel(others)
        if exist(others{k}, 'file') ~= 3, continue; end
        ok = true;
        for i = 1:500
            x = ysp_rt('now_ns'); y = int64(round(feval(others{k}, 'now') * 1e9)); z = ysp_rt('now_ns');
            ok = ok && x - 1 <= y && y <= z + 1;
        end
        check(ok, 'same clock as %s', others{k});
    end
    ci = ysp_rt('clock_info');
    check(ci.resolution > 0 && ci.resolution < 1e-3 && islogical(ci.hires_timer), 'clock_info');
    c = ysp_rt('constants');
    check(any(c.default_spin == [0.0012 0.0002]) && c.virtual_t0 == 1000 && c.fit_points == 1024, 'constants');
    check(c.fit_point == 1 && c.fit_refit == 2 && c.fit_restart == 4 && c.fit_rejected == 8 && c.fit_pending == 16, 'fit flags');
    d = ysp_rt('describe');
    check(strncmp(d, 'ysp_rt: policy=NONE', 19), 'describe');
    expect_error('ysp_rt:usage', @() ysp_rt('no_such_command'));
    expect_error('ysp_rt:usage', @() ysp_rt('a_command_name_far_too_long_to_exist'));
    expect_error('ysp_rt:usage', @() ysp_rt(7));
end

function test_waits()
    t = ysp_rt('now') + 0.003;
    late = ysp_rt('sleep_until', t);
    check(late >= 0 && ysp_rt('now') >= t, 'sleep_until');
    check(ysp_rt('sleep_until', ysp_rt('now') + 0.002, 0) >= 0, 'sleep_until without a spin');
    late = ysp_rt('sleep_until', ysp_rt('now') - 0.5);
    check(late >= 0.5, 'a past deadline returns its lateness');
    t0 = ysp_rt('now');
    late = ysp_rt('sleep', 0.002);
    check(ysp_rt('now') - t0 >= 0.002 && late >= 0, 'sleep');
    t = ysp_rt('now') + 0.001;
    check(ysp_rt('spin_until', t) >= 0 && ysp_rt('now') >= t, 'spin_until');
    bad = {-1, NaN, Inf};
    for i = 1:numel(bad)
        expect_error('ysp_rt:arg', @() ysp_rt('sleep_until', bad{i}));
        expect_error('ysp_rt:arg', @() ysp_rt('sleep', bad{i}));
        expect_error('ysp_rt:arg', @() ysp_rt('spin_until', bad{i}));
        expect_error('ysp_rt:arg', @() ysp_rt('sleep_until', 1, bad{i}));
    end
    expect_error('ysp_rt:arg', @() ysp_rt('sleep_until', 1e10));
    expect_error('ysp_rt:usage', @() ysp_rt('sleep_until'));
end

function test_virtual()
    t0 = ysp_rt('virtual_begin');
    cleanup = onCleanup(@() ysp_rt('virtual_end'));   %#ok<NASGU>
    check(ysp_rt('is_virtual') && t0 == 1000 && ysp_rt('now') == 1000, 'virtual clock starts at 1000 s');
    check(ysp_rt('now_ns') == int64(1000) * 1e9, 'virtual now_ns');
    r0 = ysp_rt('real_now');
    check(ysp_rt('sleep_until', t0 + 10) == 0 && ysp_rt('now') == t0 + 10, 'a wait jumps the clock');
    check(ysp_rt('real_now') - r0 < 5, '10 s virtual in less real time');
    expect_error('ysp_rt:state', @() ysp_rt('virtual_begin'));
    if exist('ysp_serial', 'file') == 3   % its own copy of ysp/rt.h: still real
        check(abs(ysp_serial('now') - ysp_rt('real_now')) < 1, 'other MEX files keep the real clock');
    end
    ysp_rt('virtual_end');
    check(~ysp_rt('is_virtual'), 'virtual_end');
    check(ysp_rt('virtual_begin', 5) == 5, 'virtual_begin(t0)');
    ysp_rt('virtual_end');
end

function test_correlate()
    ci = ysp_rt('clock_info');
    res_ns = int64(round(ci.resolution * 1e9));
    c = ysp_rt('correlate', @() ysp_rt('now'));
    check(isa(c.other, 'double') && c.tries == 16 && c.width > 0 && c.width < 0.1, 'correlate a clock in seconds');
    c = ysp_rt('correlate', @() ysp_rt('now_ns'), struct('tries', 8));
    w = int64(round(c.width * 1e9));
    check(isa(c.other, 'uint64') && c.tries == 8, 'correlate a counter');
    check(abs(int64(c.other) - c.t_ns) <= idivide(w, int64(2)) + res_ns, 'our own clock is inside the bracket');
    % A 1 ms clock derived from ours changes exactly at a whole ms.
    c = ysp_rt('correlate', @() idivide(ysp_rt('now_ns'), int64(1000000), 'floor'), ...
               struct('edge', true, 'tries', 2, 'timeout', 5));
    w = int64(round(c.width * 1e9));
    check(c.tries == 2 && abs(int64(c.other) * 1000000 - c.t_ns) <= idivide(w, int64(2)) + res_ns + 1, 'edge mode');
    expect_error('ysp_rt:timeout', @() ysp_rt('correlate', @() 7, struct('edge', true, 'timeout', 0.01)));
    expect_error('my:clock', @() ysp_rt('correlate', @broken_clock));
    expect_error('my:clock', @() ysp_rt('correlate', @broken_clock, struct('edge', true)));
    expect_error('ysp_rt:arg', @() ysp_rt('correlate', @() 'a'));
    expect_error('ysp_rt:arg', @() ysp_rt('correlate', @() -1));
    expect_error('ysp_rt:arg', @() ysp_rt('correlate', 42));
    expect_error('ysp_rt:arg', @() ysp_rt('correlate', @() 1, struct('edges', true)));
end

function [ticks, host, truth] = device(n, rate_ppm, offset, start)
    % A microsecond counter whose reports arrive up to 1.1 ms late, from a
    % fixed sequence so both interpreters see the same pairs.
    k = 1e-6 * (1 + rate_ppm * 1e-6);
    t = start + (0:n-1) * 0.01;
    ticks = round((t - start) / k) + offset;
    host = t + mod((1:n) * 0.6180339887, 1) * 0.0011;
    truth = @(tk) start + (double(tk) - offset) * k;
end

function test_fit()
    expect_error('ysp_rt:arg', @() ysp_rt('fit_open', struct('mode', 'late')));
    expect_error('ysp_rt:arg', @() ysp_rt('fit_open', struct('mode', 'fast', 'tick', 1e-3)));
    expect_error('ysp_rt:arg', @() ysp_rt('fit_open', struct('tick', 1e-3, 'bits', 65)));
    expect_error('ysp_rt:arg', @() ysp_rt('fit_open', struct('tick', 1e-3, 'tik', 1)));

    h = ysp_rt('fit_open', struct('mode', 'late', 'tick', 1e-6));
    expect_error('ysp_rt:state', @() ysp_rt('fit_map', h, 0));
    [ticks, host, truth] = device(3000, 100, 12345678, 100);
    flags = ysp_rt('fit_add', h, ticks, host);
    check(isequal(size(flags), size(ticks)) && all(flags >= 0), 'fit_add takes vectors');
    info = ysp_rt('fit_info', h);
    check(info.ready && info.slope && info.epoch == 0 && abs(info.ppm - 100) < 5, 'fit_info: ready, slope, ppm');
    [s, ns] = ysp_rt('fit_map', h, info.ticks0);
    err = s - truth(info.ticks0);
    check(err > -0.0002 && err < 0.0015, 'the map is within the delays of the truth (%g s)', err);
    check(isa(ns, 'int64') && abs(double(ns) - s * 1e9) < 2, 'fit_map gives ns too');
    m = ysp_rt('fit_map', h, [double(info.ticks0) - 1000; double(info.ticks0)]);
    check(isequal(size(m), [2 1]) && abs(m(2) - m(1) - 1e-3) < 1e-7, 'fit_map keeps the shape');
    expect_error('ysp_rt:arg', @() ysp_rt('fit_add', h, 1.5, 1));
    expect_error('ysp_rt:arg', @() ysp_rt('fit_add', h, 1, -1));
    expect_error('ysp_rt:arg', @() ysp_rt('fit_add', h, [1 2], 1));
    ysp_rt('fit_close', h);
    expect_error('ysp_rt:handle', @() ysp_rt('fit_info', h));

    % a 32-bit counter that wraps 5 s in
    h = ysp_rt('fit_open', struct('tick', 1e-6, 'bits', 32));
    [ticks, host, truth] = device(1000, -80, 2^32 - 5e6, 100);
    ysp_rt('fit_add', h, uint64(mod(ticks, 2^32)), host);
    u = ysp_rt('fit_unwrap', h, 1000);
    check(isa(u, 'uint64') && u == uint64(2^32 + 1000), 'fit_unwrap');
    check(abs(ysp_rt('fit_map', h, 1000) - truth(u)) < 0.0015, 'the map across the wrap');
    ysp_rt('fit_close', h);

    % brackets wider than max_width are refused
    h = ysp_rt('fit_open', struct('mode', 'bracket', 'tick', 1e-3, 'max_width', 0.002));
    f = ysp_rt('fit_add', h, [5 6], [10 10.001], [0.0005 0.003]);
    info = ysp_rt('fit_info', h);
    check(bitand(f(1), 1) && f(2) == 8 && info.rejected == 1, 'bracket refuses a wide pair');
    ysp_rt('fit_close', h);

    % a counter reset starts a new epoch
    h = ysp_rt('fit_open', struct('tick', 1e-3));
    i = 0:999;
    tk = i * 10; tk(501:end) = (i(501:end) - 500) * 10;
    f = ysp_rt('fit_add', h, tk, 50 + i * 0.01);
    info = ysp_rt('fit_info', h);
    check(any(bitand(f, 4)) && info.epoch == 1, 'restart after a reset');
    ysp_rt('fit_close', h);
end

function test_user_dir()
    d = ysp_rt('user_dir', 'config');
    check(isempty(d) || strcmp(d(end-2:end), 'ysp'), 'user_dir config');
    d = ysp_rt('user_dir', 'cache', 'rig/x');
    check(isempty(d) || ~isempty(regexp(strrep(d, '\', '/'), 'ysp/rig/x$', 'once')), 'user_dir with a subdir');
    check(isempty(ysp_rt('user_dir', 'config', '..')), 'a refused subdir gives ''''');
    expect_error('ysp_rt:arg', @() ysp_rt('user_dir', 'temp'));
end

function v = broken_clock() %#ok<STOUT>
    error('my:clock', 'broken');
end

function check(cond, msg, varargin)
    global YSP_RT_CHECKS %#ok<GVMIS>
    YSP_RT_CHECKS = YSP_RT_CHECKS + 1;
    if ~cond
        error('test_mex_rt:fail', ['FAIL: ' msg], varargin{:});
    end
end

function expect_error(id, f)
    try
        f();
    catch e
        check(strcmp(e.identifier, id), 'expected %s, got %s: %s', id, e.identifier, e.message);
        return;
    end
    check(false, 'expected %s, got no error', id);
end
