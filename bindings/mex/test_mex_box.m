function test_mex_box()
%TEST_MEX_BOX  Key checks of the ysp_box MEX function. Runs in MATLAB and in
%   Octave. Prints PASS with the number of checks, or stops with the error of
%   the first check that failed. Build first (run build.m), then from this
%   directory: test_mex_box.
%
%   tests/adapt/box_test.c checks the header itself. Here: each family's
%   frames map to the event and pair fields as the header's manual says, a
%   stream decodes the same whole, byte by byte and in random chunks, random
%   bytes never crash and give only well-formed events, the encoders give
%   the manual's bytes, and bad arguments raise ysp_box:<what>.

    global YSP_BOX_CHECKS %#ok<GVMIS>
    YSP_BOX_CHECKS = 0;
    here = fileparts(mfilename('fullpath'));
    addpath(here);
    [v, vh] = ysp_box('version');
    fprintf('ysp_box %s (ysp/box.h %s)\n', v, vh);
    rand('state', 7); %#ok<RAND> the same call in MATLAB and Octave

    c = ysp_box('constants');
    check(c.XID == 1 && c.PARALLEL == 8 && c.KIND_BOX == 5 && c.KIND_SYNC == 7 && c.PRESS == 1 && ...
          c.SAMPLE == 3 && c.DEVTICKS == 16 && c.TIER_SIM == 4 && c.UNC_MAX == 0.065535 && c.SLOW_WRITE == 0.001, ...
          'constants');
    fields = {'t', 't_ns', 'device', 'control', 'code', 'mods', 'kind', 'type', 'flags', 'stamp', 'unc', ...
              'x', 'y', 'value', 'aux', 'ticks'};

    % ---- frames to fields -------------------------------------------------------
    h = ysp_box('open', 'xid', c.KIND_BOX, 7);
    [ev, pr, info] = ysp_box('decode', h, [xid_key(1, 3, 1, 1234), xid_key(0, 8, 0, 4294967295)], 2.5);
    check(isequal(fieldnames(ev)', fields) && isequal(size(ev), [1 2]), 'event fields in the shared order');
    e = ev(1);
    check(e.t == 2.5 && isa(e.t_ns, 'int64') && e.t_ns == 2500000000 && e.device == 7 && e.control == 3 && ...
          e.code == 1 && e.kind == c.KIND_BOX && e.type == c.PRESS && e.flags == c.DEVTICKS && e.value == 1 && ...
          e.ticks == 1234 && e.unc == 0 && e.stamp == 0, 'XID press fields');
    check(ev(2).control == 8 && ev(2).type == c.RELEASE && ev(2).ticks == 4294967295, 'XID key 0 is 8, release');
    check(isequal(pr.ticks, uint64([1234; 4294967295])) && isequal(pr.probe, [0; 0]) && info.garbage == 0, 'XID pairs');
    s = ysp_box('stats', h);
    check(s.frames == 2 && s.garbage == 0 && strcmp(s.family, 'xid'), 'stats');
    [ev, pr] = ysp_box('decode', h, [uint8('_e5'), le32(777), xid_st2(2, 0, 1, 99)]);
    check(isequal(pr.ticks, uint64([777; 99])) && isequal(pr.probe, [1; 0]) && numel(ev) == 1 && ...
          ev.control == 8 && ev.code == 2 && ev.t == 0, 'timer answer and StimTracker 2');
    [ev, ~, info] = ysp_box('decode', h, [uint8('k'), uint8(12), xid_key(1, 1, 1, 5)]);
    check(info.garbage == 2 && numel(ev) == 1 && ev.ticks == 5, 'a broken XID frame is garbage');
    ysp_box('close', h);

    data = uint8(sprintf('S 100\nE 18446744073709551615 4 1\nA 300 2 -1.5\nQ 9 400\r\nI ysp-line 1 teensy41 left\n# c\nX nonsense\nO 5 9\n'));
    [ev, pr, info] = ysp_box('decode', 'line', data, 1);
    check(isequal(pr.ticks, [uint64(100); intmax('uint64'); 300; 400; 5]) && isequal(pr.probe, [0; 0; 0; 9; 0]), ...
          'line pairs, 64 bits exact');
    check(numel(ev) == 2 && ev(1).ticks == 4294967295 && ev(1).control == 4 && ev(2).type == c.SAMPLE && ...
          ev(2).value == -1.5, 'line events');
    check(isequal(info.text, {'ysp-line 1 teensy41 left'}) && info.garbage == numel('X nonsense') + 1 && ...
          isequal(info.outputs.ticks, uint64(5)) && info.outputs.code == 9, 'line identity, garbage, outputs');
    [~, pr, info] = ysp_box('decode', 'line', uint8(sprintf('I first\nI second\nS 5\nI\n')));
    check(isequal(info.text, {'first'; 'second'; ''}), ...
          'every identity line comes back (ysp/box.h 0.3.1)');
    check(isequal(pr.ticks, uint64(5)), 'the pair after the identity lines');
    [~, pr, info] = ysp_box('decode', 'line', uint8(sprintf('O %d %d\n', [1000:1049; 0:49])));
    check(numel(pr.ticks) == 50 && isequal(info.outputs.code, (0:49)'), 'every O report comes back');
    [ev, pr] = ysp_box('decode', 'photo', [photo(1, 10), photo_sync(20), photo(0, 30)]);
    check(isequal([ev.type], [c.PRESS c.RELEASE]) && isequal([ev.control], [1 1]) && isequal(pr.ticks, uint64([10; 20; 30])), ...
          'photodiode frames');
    fams = {'triggerbox', 'biosemi', 'mmbts', 'lines', 'parallel'};
    for i = 1:numel(fams)
        [ev, pr, info] = ysp_box('decode', fams{i}, uint8(0:255));
        check(isempty(ev) && isempty(pr.ticks) && info.garbage == 256, '%s: every byte is garbage', fams{i});
    end

    % ---- streams split anywhere --------------------------------------------------
    for fam = {'xid', 'line', 'photo'}
        data = stream(fam{1}, 600);
        [w_ev, w_pr, w_n] = chunked(fam{1}, data, numel(data));
        check(numel(w_ev) + numel(w_pr.ticks) > 64 * 4, '%s: many array fills in one call', fam{1});
        [b_ev, b_pr, b_n] = chunked(fam{1}, data, ones(1, numel(data)));
        sizes = chunk_sizes(40, numel(data));
        [r_ev, r_pr, r_n] = chunked(fam{1}, data, sizes);
        check(isequaln(w_ev, b_ev, r_ev) && isequal(w_pr, b_pr, r_pr) && isequal(w_n, b_n, r_n), ...
              '%s: whole, byte by byte and chunked agree', fam{1});
    end

    % ---- random bytes ------------------------------------------------------------
    fams = {'xid', 'line', 'photo', 'triggerbox', 'biosemi', 'mmbts', 'lines', 'parallel'};
    for i = 1:numel(fams)
        data = uint8(floor(rand(1, 200000) * 256));
        if strcmp(fams{i}, 'line')
            alpha = uint8(sprintf('SEAQIO# 0123456789.-\n\n'));
            k = rand(1, numel(data)) < 0.95;
            data(k) = alpha(floor(rand(1, nnz(k)) * numel(alpha)) + 1);
        end
        sizes = chunk_sizes(300, numel(data));
        [ev, pr, n] = chunked(fams{i}, data, sizes);
        ok = n.garbage == n.total && n.total <= numel(data);
        if ~isempty(ev)
            ok = ok && all([ev.flags] == c.DEVTICKS) && all([ev.kind] == c.KIND_BOX) && all([ev.device] == 1) && ...
                 all(ismember([ev.type], [c.PRESS c.RELEASE c.SAMPLE])) && all([ev.ticks] < 2^32) && ...
                 all([ev.control] >= 1 & [ev.control] <= 255) && all([ev.t_ns] == int64(1e9));
        end
        if i <= 3
            [ev2, pr2, n2] = chunked(fams{i}, data, numel(data));
            ok = ok && ~isempty(pr.ticks) && isequaln(ev, ev2) && isequal(pr, pr2) && n.garbage == n2.garbage;
        else
            ok = ok && isempty(ev) && isempty(pr.ticks) && n.garbage == numel(data);
        end
        check(ok, '%s: 200000 random bytes', fams{i});
    end

    % ---- times --------------------------------------------------------------------
    h = ysp_box('open', 'photo');
    ok = true;
    for k = 1:1000
        ns = floor(rand * 2^23) * 1e9 + floor(rand * 1e9);
        ev = ysp_box('decode', h, photo(1, 0), ns / 1e9);
        ok = ok && ev.t_ns == int64(ns) && ev.t == ns / 1e9;
    end
    check(ok, 'a read time ns / 1e9 gives back ns (1000 values below 2^23 s)');
    ev = ysp_box('decode', h, photo(1, 0, 1));
    s = ysp_box('stats', h);
    check(isempty(ev) && s.pending == 3, 'a partial frame waits');
    ysp_box('reset', h);
    [ev, ~, info] = ysp_box('decode', h, uint8([0 0 0]));
    s = ysp_box('stats', h);
    check(isempty(ev) && info.garbage == 3 && s.frames == 0, 'reset forgets it');
    ysp_box('close', h);

    % ---- families and output bytes -------------------------------------------------
    f = ysp_box('family', 'XID');
    check(f.value == 1 && f.caps == 1 + 2 + 8 + 32 && f.code_max == 65535 && f.tick == 0.001 && f.slow_write && ...
          f.baud == 115200 && f.fixed_pulse == 0, 'family xid');
    f = ysp_box('family', 5);
    check(strcmp(f.name, 'biosemi') && f.fixed_pulse == 0.008 && ~f.slow_write, 'family biosemi');
    f = ysp_box('family', 'mmbts');
    g = ysp_box('family', 'line');
    check(f.baud == 9600 && g.tick == 1e-6, 'baud and tick');
    check(isequal(ysp_box('probe', 'xid'), uint8('_e5')) && isequal(ysp_box('probe', 'line', 42), uint8(sprintf('q 42\n'))) && ...
          isempty(ysp_box('probe', 'photo')) && isequal(ysp_box('identify', 'line'), uint8(sprintf('i\n'))), 'queries');
    check(isequal(ysp_box('encode', 'xid', 'set', 4660), [uint8('mh'), 52, 18]), 'xid set');
    check(isequal(ysp_box('encode', 'xid', 'pulse', 3, 0.002), [uint8('mp'), le32(2), uint8('mh'), uint8([3 0])]), 'xid pulse');
    check(isequal(ysp_box('encode', 'line', 'pulse', 5, 0.0015), uint8(sprintf('p 5 1500\n'))), 'line pulse');
    check(isequal(ysp_box('encode', 'triggerbox', 'close'), uint8(255)) && ...
          isequal(ysp_box('encode', 'triggerbox', 'set', 200), uint8(200)) && ...
          isempty(ysp_box('encode', 'biosemi', 'pulse', 7, 0.01)) && isempty(ysp_box('encode', 'lines', 'set', 3)), ...
          'one-byte families');

    % ---- refusals carry their ids ---------------------------------------------------
    expect_error(@() ysp_box('open', 'nope'), 'ysp_box:arg', 'unknown family');
    expect_error(@() ysp_box('open', 0), 'ysp_box:arg', 'family 0');
    expect_error(@() ysp_box('open', 9), 'ysp_box:arg', 'family 9');
    expect_error(@() ysp_box('open', 1.5), 'ysp_box:arg', 'family 1.5');
    expect_error(@() ysp_box('open', {}), 'ysp_box:arg', 'family a cell');
    expect_error(@() ysp_box('open', 'xid', 256), 'ysp_box:arg', 'kind 256');
    expect_error(@() ysp_box('open', 'xid', 5, -1), 'ysp_box:arg', 'device -1');
    expect_error(@() ysp_box('open', 'xid', 5, 2^32), 'ysp_box:arg', 'device 2^32');
    expect_error(@() ysp_box('decode', 'xid', [1 2 3]), 'ysp_box:arg', 'double bytes');
    expect_error(@() ysp_box('decode', 'xid', 'abc'), 'ysp_box:arg', 'char bytes');
    expect_error(@() ysp_box('decode', 'xid', uint8(1), -1), 'ysp_box:arg', 'negative t');
    expect_error(@() ysp_box('decode', 'xid', uint8(1), NaN), 'ysp_box:arg', 'NaN t');
    expect_error(@() ysp_box('decode', 'xid', uint8(1), Inf), 'ysp_box:arg', 'Inf t');
    expect_error(@() ysp_box('decode', 'xid', uint8(1), [1 2]), 'ysp_box:arg', 'two t');
    expect_error(@() ysp_box('decode', 'xid'), 'ysp_box:usage', 'missing bytes');
    expect_error(@() ysp_box('encode', 'xid', 'hold', 1), 'ysp_box:arg', 'unknown op');
    expect_error(@() ysp_box('encode', 'xid', 'set', 65536), 'ysp_box:arg', 'code above code_max');
    expect_error(@() ysp_box('encode', 'xid', 'set', 1.5), 'ysp_box:arg', 'fractional code');
    expect_error(@() ysp_box('encode', 'photo', 'set', 0), 'ysp_box:arg', 'a family without outputs');
    expect_error(@() ysp_box('encode', 'xid', 'pulse', 1, -0.001), 'ysp_box:arg', 'negative width');
    expect_error(@() ysp_box('encode', 'line', 'pulse', 1, 4e-7), 'ysp_box:arg', 'a width below 1 us');
    expect_error(@() ysp_box('probe', 'line', 2^32), 'ysp_box:arg', 'seq 2^32');
    h = ysp_box('open', 'line');
    ysp_box('close', h);
    expect_error(@() ysp_box('decode', h, uint8(1)), 'ysp_box:handle', 'closed handle');
    expect_error(@() ysp_box('stats', uint64(999999)), 'ysp_box:handle', 'unknown handle');
    expect_error(@() ysp_box('nonsense'), 'ysp_box:usage', 'unknown command');
    expect_error(@() ysp_box(), 'ysp_box:usage', 'no command');

    fprintf('PASS (%d checks)\n', YSP_BOX_CHECKS);
end

function b = le32(v)
    b = typecast(uint32(v), 'uint8');
end

function b = xid_key(port, key, pressed, ms)
    b = [uint8('k'), uint8(bitor(bitor(port, pressed * 16), bitshift(mod(key, 8), 5))), le32(ms)];
end

function b = xid_st2(port, key, pressed, ms)
    b = [uint8('o'), uint8([port, mod(key, 8)]), uint8(48 + pressed), le32(ms), uint8(0)];
end

function b = photo(level, us, cut)
    b = [uint8([165 level]), le32(us)];
    if nargin > 2, b = b(1:3); end
end

function b = photo_sync(us)
    b = [uint8([90 0]), le32(us)];
end

function r = chunk_sizes(m, total)
    % random sizes 1..m that sum to at least total
    r = floor(rand(1, ceil(2 * total / m) + 16) * m) + 1;
    while sum(r) < total, r = [r, floor(rand(1, 64) * m) + 1]; end %#ok<AGROW>
end

function data = stream(fam, n)
    parts = cell(1, n);
    for k = 1:n
        u = floor(rand * 2^32);
        switch fam
            case 'xid'
                switch floor(rand * 3)
                    case 0, parts{k} = xid_key(floor(rand * 4), floor(rand * 8), rand < 0.5, u);
                    case 1, parts{k} = xid_st2(floor(rand * 256), floor(rand * 256), rand < 0.5, u);
                    otherwise, parts{k} = [uint8('_e5'), le32(u)];
                end
            case 'line'
                switch floor(rand * 5)
                    case 0, parts{k} = uint8(sprintf('S %d\n', u));
                    case 1, parts{k} = uint8(sprintf('E %d %d %d\n', u, floor(rand * 255) + 1, rand < 0.5));
                    case 2, parts{k} = uint8(sprintf('A %d %d %.3f\n', u, floor(rand * 255) + 1, rand * 10 - 5));
                    case 3, parts{k} = uint8(sprintf('Q %d %d\n', floor(rand * (2^32 - 1)) + 1, u));
                    otherwise, parts{k} = uint8(sprintf('O %d %d\n', u, floor(rand * 256)));
                end
            otherwise
                if rand < 0.7, parts{k} = photo(rand < 0.5, u); else, parts{k} = photo_sync(u); end
        end
        if rand < 0.1, parts{k} = [parts{k}, uint8([1 2 254])]; end
    end
    data = [parts{:}];
end

function [ev, pr, n] = chunked(fam, data, sizes)
    h = ysp_box('open', fam, 5, 1);
    ev = []; pr = struct('ticks', zeros(0, 1, 'uint64'), 'probe', zeros(0, 1));
    n = struct('garbage', 0, 'outputs', zeros(0, 1, 'uint64'), 'total', 0);
    i = 0;
    for s = sizes
        if i >= numel(data), break; end
        [e, p, info] = ysp_box('decode', h, data(i + 1:min(i + s, numel(data))), 1);
        if isempty(ev), ev = e; else, ev = [ev, e]; end %#ok<AGROW>
        pr.ticks = [pr.ticks; p.ticks];
        pr.probe = [pr.probe; p.probe];
        n.garbage = n.garbage + info.garbage;
        n.outputs = [n.outputs; info.outputs.ticks];
        i = i + s;
    end
    st = ysp_box('stats', h);
    n.total = st.garbage;
    ysp_box('close', h);
end

function check(ok, fmt, varargin)
    global YSP_BOX_CHECKS %#ok<GVMIS>
    if ~ok
        error('test_mex_box:fail', ['FAIL: ' fmt], varargin{:});
    end
    YSP_BOX_CHECKS = YSP_BOX_CHECKS + 1;
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
