function test_mex_rdk()
%TEST_MEX_RDK  Key checks of the ysp_rdk MEX function. Runs in MATLAB and in
%   Octave. Prints PASS with the number of checks, or stops with the error of
%   the first check that failed. Build first (run build.m), then from this
%   directory: test_mex_rdk.
%
%   The 8 golden digests are tests/adapt/rdk_test.c's (and ysp.rdk's), and
%   bindings/python/ysp_rdk/tests/c_trial.bin is a trial run by the C header
%   alone (make_c_trial.c beside it): the MEX replays it into arrays equal
%   bit for bit.

    global YSP_RDK_CHECKS %#ok<GVMIS>
    YSP_RDK_CHECKS = 0;
    here = fileparts(mfilename('fullpath'));
    addpath(here);
    [v, vh] = ysp_rdk('version');
    fprintf('ysp_rdk %s (ysp/rdk.h %s)\n', v, vh);

    % the exact pieces: Random123's known answers for philox4x32 10
    check(isequal(ysp_rdk('philox', 0, [0 0 0 0]), uint32([hex2dec('6627E8D5') hex2dec('E169C58D') ...
          hex2dec('BC57AC4C') hex2dec('9B00DBD8')])), 'philox known answer');
    check(ysp_rdk('angle', 90) == uint32(hex2dec('40000000')) && ysp_rdk('angle', 1) == uint32(11930465), 'angle');
    check(isequal(ysp_rdk('sincos', hex2dec('40000000')), int32([2^30 0])), 'sincos');
    c = ysp_rdk('constants');
    check(c.LL == 3 && c.RECT == 1 && c.NOISE_WALK == 2 && c.EV_PLACED == 4, 'constants');

    % ---- the golden digests --------------------------------------------------
    P = 16666667; T0 = 1000000000;
    VC = 1; VD = 2; VS = 4;
    G = {
        'same_dir_circle', struct('w', 10, 'count', 200, 'coherence', 0.3125, 'direction', 30, 'speed', 7.5), ...
            u64('0', '1'), 50, -1, 0, 240, u64('46C7ADD1', '3BEA5555');
        'wn', struct('algorithm', 'WN', 'w', 8, 'density', 2.5, 'coherence', 0.25, 'direction', 200, 'speed', 6), ...
            u64('0', '2'), -1, -1, 0, 240, u64('AB0C3CE3', 'AD1B3C70');
        'mn_frames', struct('algorithm', 'MN', 'w', 12, 'count', 100, 'coherence', 0.5, 'direction', 45, ...
            'speed', 10, 'clock', 'frames', 'frame_period', P / 1e9, 'lifetime_frames', 6), ...
            u64('0', '3'), 70, -1, 0, 300, u64('890DE1AB', '1D616C84');
        'll', struct('algorithm', 'LL', 'w', 9, 'count', 90, 'coherence', 0.6875, 'direction', -30, 'speed', 4), ...
            u64('0', '4'), -1, -1, 0, 300, u64('9FE3C4CA', '6FE8ED6D');
        'bm_rect', struct('algorithm', 'BM', 'aperture', 'rect', 'w', 12, 'h', 5, 'count', 150, ...
            'coherence', 0.4375, 'direction', 100, 'speed', 9, 'stream', 7), ...
            u64('0', '5'), -1, 120, 0, 240, u64('34079EB0', 'FE076C8F');
        'replot_life', struct('signal', 'different', 'noise', 'position', 'edge', 'replot', 'w', 10, ...
            'count', 120, 'lifetime', 0.25, 'coherence', 0.5, 'speed', 12), ...
            u64('0', '6'), -1, -1, VC, 240, u64('D4F62288', '48132C60');
        'walk_rect_life', struct('select', 'bernoulli', 'noise', 'walk', 'aperture', 'rect', 'w', 7, 'h', 11, ...
            'edge', 'replot', 'lifetime_frames', 5, 'frame_period', P / 1e9, 'count', 80, ...
            'coherence', 0.375, 'speed', 5), ...
            u64('0', '7'), -1, -1, bitor(VD, VS), 300, u64('EB8733EF', '39ECA7DA');
        'stall_circle', struct('signal', 'different', 'w', 6, 'count', 64, 'lifetime', 1.3, 'coherence', 0.5, ...
            'direction', 77.25, 'speed', 3.25), ...
            u64('DEADBEEF', 'CAFEF00D'), 40, 60, 0, 200, u64('1749463C', '5A7DD3E8');
    };
    for g = 1:size(G, 1)
        [name, d, seed, drop, stall, vary, updates, digest] = G{g, :};
        steps = zeros(updates, 4);
        for k = 1:updates
            t = T0 + k * P + (mod(k * 7919, 101) - 50) * 1000;
            if drop >= 0 && k >= drop, t = t + P; end
            if stall >= 0 && k >= stall, t = t + 37000000000; end
            cc = field_or(d, 'coherence'); dr = field_or(d, 'direction'); sp = field_or(d, 'speed');
            if bitand(vary, VC), cc = mod(k, 17) / 16; end
            if bitand(vary, VD), dr = mod(k, 8) * 45 + 0.5; end
            if bitand(vary, VS) && k >= 100 && k < 140, sp = -sp; end
            steps(k, :) = [t / 1e9, cc, dr, sp];
        end
        h = ysp_rdk('open', d);
        ysp_rdk('start', h, seed, T0 / 1e9);
        for k = 1:updates
            ysp_rdk('set', h, 'coherence', steps(k, 2), 'direction', steps(k, 3), 'speed', steps(k, 4));
            ysp_rdk('update', h, steps(k, 1));
        end
        check(ysp_rdk('digest', h) == digest, 'golden %s by updates', name);
        ysp_rdk('replay', h, seed, T0 / 1e9, steps);
        check(ysp_rdk('digest', h) == digest, 'golden %s by replay', name);
        xy = ysp_rdk('trajectory', h, seed, T0 / 1e9, steps);
        info = ysp_rdk('get', h);
        check(ysp_rdk('digest', h) == digest && isequal(size(xy), [info.n 2 updates]), 'golden %s by trajectory', name);
        ysp_rdk('close', h);
    end

    % ---- a trial logged by C, replayed bit for bit ---------------------------
    fid = fopen(fullfile(here, '..', 'python', 'ysp_rdk', 'tests', 'c_trial.bin'), 'rb');
    raw = fread(fid, Inf, '*uint8')';
    fclose(fid);
    check(isequal(char(raw(1:4)), 'RDKT') && typecast(raw(5:8), 'uint32') == 1, 'c_trial.bin header');
    seed = typecast(raw(9:16), 'uint64');
    t0 = typecast(raw(17:24), 'int64');
    K = double(typecast(raw(25:28), 'uint32'));
    n = double(typecast(raw(29:32), 'uint32'));
    off = 32;
    rec = reshape(raw(off + 1:off + 24 * K), 24, K);
    off = off + 24 * K;
    st = zeros(K, 4); ns = zeros(K, 1, 'int64');
    for k = 1:K
        ns(k) = typecast(rec(1:8, k)', 'int64');
        st(k, :) = [double(ns(k)) / 1e9, double(typecast(rec(9:20, k)', 'single'))];
    end
    cxy = typecast(raw(off + 1:off + 8 * n * K), 'single'); off = off + 8 * n * K;
    cdir = typecast(raw(off + 1:off + 4 * n * K), 'single'); off = off + 4 * n * K;
    csig = raw(off + 1:off + n * K); off = off + n * K;
    cdigest = typecast(raw(off + 1:off + 8), 'uint64');
    cxy = permute(reshape(cxy, 2, n, K), [2 1 3]);   % C [k][i][xy] -> n x 2 x K
    cdir = reshape(cdir, n, K);
    csig = reshape(csig, n, K);

    cdesc = struct('algorithm', 'LL', 'aperture', 'rect', 'w', 8, 'h', 6, 'count', 60, 'lifetime', 0.3, ...
                   'coherence', 0.5, 'speed', 4);
    h = ysp_rdk('open', cdesc);
    [gxy, gdir, gsig] = ysp_rdk('trajectory', h, seed, double(t0) / 1e9, st);
    check(ysp_rdk('digest', h) == cdigest, 'c_trial digest');
    check(isequal(typecast(gxy(:), 'uint32'), typecast(cxy(:), 'uint32')), 'c_trial x, y bit for bit (%d updates)', K);
    check(isequal(typecast(gdir(:), 'uint32'), typecast(cdir(:), 'uint32')), 'c_trial directions bit for bit');
    check(isequal(gsig, csig), 'c_trial signal flags');

    % a live run's log ('get' .last after each update) has the C log's ns,
    % and replays to the same digest as a struct array
    ysp_rdk('start', h, seed, double(t0) / 1e9);
    log = repmat(struct('t', 0, 'coherence', 0, 'direction', 0, 'speed', 0, 'set', 0, 'ns', int64(0)), K, 1);
    for k = 1:K
        ysp_rdk('set', h, 'coherence', st(k, 2), 'direction', st(k, 3), 'speed', st(k, 4));
        ysp_rdk('update', h, st(k, 1));
        info = ysp_rdk('get', h);
        log(k) = info.last;
    end
    check(ysp_rdk('digest', h) == cdigest, 'c_trial by updates');
    last = ysp_rdk('xy', h);
    check(isequal(typecast(last(:), 'uint32'), typecast(reshape(cxy(:, :, K), [], 1), 'uint32')), 'xy of the last update');
    check(isequal([log.ns]', ns) && all([log.t] == double([log.ns]) / 1e9), 'last: t and its exact ns');
    g2 = ysp_rdk('open', cdesc);
    ysp_rdk('replay', g2, seed, double(t0) / 1e9, log);
    check(ysp_rdk('digest', g2) == cdigest, 'replay of the struct log');
    ysp_rdk('replay', g2, seed, double(t0) / 1e9, rmfield(log, 'ns'));
    check(ysp_rdk('digest', g2) == cdigest, 'replay of the struct log without ns');

    % ---- outputs, snapshots ---------------------------------------------------
    snap = ysp_rdk('snapshot', h);
    ysp_rdk('start', g2, 1, 1);
    ysp_rdk('update', g2, 1.1);
    ysp_rdk('restore', g2, snap);
    check(ysp_rdk('digest', g2) == ysp_rdk('digest', h), 'snapshot and restore');
    check(isequal(size(ysp_rdk('directions', h)), [n 1]) && isa(ysp_rdk('ages', h), 'single') && ...
          isa(ysp_rdk('signals', h), 'uint8') && isequal(ysp_rdk('signals', h), csig(:, K)), 'outputs');
    ev = ysp_rdk('events', h);
    check(isequal(bitand(ev, uint8(1)), csig(:, K)), 'events carry the signal bit');
    info = ysp_rdk('get', h);
    check(info.n == n && info.total == 3 * n && info.stats.updates == K, 'get (LL: 3 sets): n %d, total %d, updates %d', info.n, info.total, info.stats.updates);

    % ---- refusals carry their ids -----------------------------------------------
    expect_error(@() ysp_rdk('open', struct('w', 10, 'count', 10, 'colour', 1)), 'ysp_rdk:arg', 'unknown desc field');
    expect_error(@() ysp_rdk('open', struct('w', 10, 'count', 10, 'algorithm', 'XX')), 'ysp_rdk:arg', 'unknown algorithm');
    expect_error(@() ysp_rdk('open', struct('w', -1, 'count', 10)), 'ysp_rdk:arg', 'header refuses the desc');
    expect_error(@() ysp_rdk('update', h, -1), 'ysp_rdk:arg', 'negative time');
    expect_error(@() ysp_rdk('update', h, NaN), 'ysp_rdk:arg', 'NaN time');
    expect_error(@() ysp_rdk('update', h, 2^23), 'ysp_rdk:arg', 'time at 2^23 s');
    expect_error(@() ysp_rdk('update', h, 1), 'ysp_rdk:order', 'time before the last update');
    expect_error(@() ysp_rdk('replay', h, 1, 1, [1 2 3]), 'ysp_rdk:arg', 'steps of 3 columns');
    bad = log(1:2); bad(2).t = bad(2).t + 1e-9;
    expect_error(@() ysp_rdk('replay', h, seed, double(t0) / 1e9, bad), 'ysp_rdk:arg', 'a row whose t is not ns / 1e9');
    expect_error(@() ysp_rdk('set', h, 'size', 3), 'ysp_rdk:arg', 'set of an unknown float');
    expect_error(@() ysp_rdk('start', h, -1, 0), 'ysp_rdk:arg', 'negative seed');
    expect_error(@() ysp_rdk('restore', h, uint8([1 2 3])), 'ysp_rdk:arg', 'a short snapshot');
    u = ysp_rdk('open', cdesc);
    expect_error(@() ysp_rdk('xy', u), 'ysp_rdk:order', 'output before start');
    ysp_rdk('close', u);
    expect_error(@() ysp_rdk('digest', u), 'ysp_rdk:handle', 'closed handle');
    expect_error(@() ysp_rdk('nonsense'), 'ysp_rdk:usage', 'unknown command');
    ysp_rdk('close', h);
    ysp_rdk('close', g2);

    fprintf('PASS (%d checks)\n', YSP_RDK_CHECKS);
end

function v = u64(hi, lo)
    v = bitor(bitshift(uint64(hex2dec(hi)), 32), uint64(hex2dec(lo)));
end

function v = field_or(s, name)
    v = 0;
    if isfield(s, name), v = s.(name); end
end

function check(ok, fmt, varargin)
    global YSP_RDK_CHECKS %#ok<GVMIS>
    if ~ok
        error('test_mex_rdk:fail', ['FAIL: ' fmt], varargin{:});
    end
    YSP_RDK_CHECKS = YSP_RDK_CHECKS + 1;
end

function expect_error(f, id, what)
    try
        f();
    catch err
        check(strcmp(err.identifier, id), '%s: got %s (%s), expected %s', what, err.identifier, err.message, id);
        return;
    end
    check(false, '%s: no error, expected %s', what, id);
end
