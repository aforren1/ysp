function test_mex_device()
%TEST_MEX_DEVICE  Key checks of the ysp_device and ysp_rigfile MEX functions
%   on simulated boards (ysp_device('sim_open', ...)): no hardware. Runs in
%   MATLAB and in Octave. Prints PASS with the number of checks, or stops
%   with the error of the first check that failed. Build first (run
%   build.m), then from this directory: test_mex_device.

    global YSP_DEVICE_CHECKS %#ok<GVMIS>
    YSP_DEVICE_CHECKS = 0;
    here = fileparts(mfilename('fullpath'));
    addpath(here);
    [bv, hv] = ysp_device('version');
    [rv, rhv] = ysp_rigfile('version');
    fprintf('ysp_device %s (ysp/device.h %s), ysp_rigfile %s (ysp/rigfile.h %s)\n', bv, hv, rv, rhv);
    tol = 0.005;   % a stamp's error on the real clock, with the reader's wake under load

    c = ysp_device('constants');
    check(c.LINE == 2 && c.TRIGGERBOX == 4 && c.RUNNING == 2 && c.KIND_BOX == 5 && c.PRESS == 1 && ...
          c.RELEASE == 2 && c.DEVTICKS == 16 && c.TIER_1 == 1 && c.OUT_RULE == 512, 'constants');
    [n, name] = ysp_device('family', 'xid');
    check(n == 1 && strcmp(name, 'xid'), 'family by name');
    check(ysp_device('key_match', 'serial:16C0:0483::', 'serial:16c0:0483:A:1-1'), 'key_match');
    check(isstruct(ysp_device('list')), 'list');
    t0 = ysp_device('now');
    check(t0 > 0 && abs(ysp_device('now') - t0) < 1, 'now');

    % ---- a line board: events on the ysp_rt clock ----
    s = ysp_device('sim_open', 'line');
    h = ysp_device('open', struct('role', 'photo', 'family', 'line', 'key', 'serial:16C0:0483::', 'sim', s, ...
                                  'no_elevate', true));
    % Inf is INT64_MAX ns: ysp/device.h 0.5.0 timed out at once on it (now + INT64_MAX overflowed)
    check(ysp_device('wait_running', h, Inf), 'line: RUNNING, waited with Inf');
    [st, sn] = ysp_device('state', h);
    check(st == c.RUNNING && strcmp(sn, 'running'), 'state');
    info = ysp_device('info', h);
    check(strcmp(info.role, 'photo') && info.family == c.LINE && info.kind == c.KIND_BOX && info.device >= 1, 'info');
    roles = ysp_device('roles');
    check(any(strcmp(roles, 'photo')), 'roles');
    stats = ysp_device('stats', h);
    check(strncmp(stats.ident, 'ysp-line 1', 10), 'identified');
    tp = ysp_device('now') + 0.2;
    truth = [];
    for i = 0:9
        ysp_device('sim_press', s, tp + i * 0.05, 0.02, 3);
        truth = [truth, tp + i * 0.05, tp + i * 0.05 + 0.02]; %#ok<AGROW>
    end
    ev = collect(h, 20);
    check(numel(ev) == 20, 'line: 20 events, got %d', numel(ev));
    check(all([ev.type] == repmat([c.PRESS c.RELEASE], 1, 10)), 'press and release');
    check(all([ev.control] == 3) && all([ev.device] == info.device) && all([ev.kind] == c.KIND_BOX), 'fields');
    check(all(abs([ev.t] - truth) < tol), 'stamps within %g s: worst %g', tol, max(abs([ev.t] - truth)));
    check(isa(ev(1).t_ns, 'int64') && abs(double(ev(1).t_ns) / 1e9 - ev(1).t) < 1e-6, 't_ns');
    check(all(bitand([ev.flags], c.DEVTICKS) > 0) && all([ev.unc] >= 0), 'devticks, unc');
    names = fieldnames(ev);
    check(isequal(names', {'t', 't_ns', 'device', 'control', 'code', 'mods', 'kind', 'type', 'flags', 'stamp', ...
                           'unc', 'x', 'y', 'value', 'aux', 'ticks'}), 'event field order');
    e0 = ysp_device('poll', h);
    check(isstruct(e0) && isempty(e0) && isfield(e0, 't'), 'poll: empty struct with the fields');
    tic; e1 = ysp_device('wait', h, 0.2); w = toc;
    check(isempty(e1) && w >= 0.15 && w < 2, 'wait times out');
    ysp_device('sim_press', s, ysp_device('now') + 0.02, 0.005);
    pause(0.3);
    check(ysp_device('pending', h) == 2, 'pending');
    check(numel(ysp_device('poll', h, 1)) == 1 && ysp_device('clear', h) == 1, 'poll max, clear');
    src = ysp_device('source', h);
    check(src.tier == c.TIER_UNKNOWN, 'source: tier UNKNOWN without a profile');
    stats = ysp_device('stats', h);
    check(stats.events >= 22 && stats.answers > 0 && stats.fit.ready && stats.queue_dropped == 0, 'stats');
    ysp_device('pulse', h, 7, 0.002);
    lo = ysp_device('last_out', h);
    check(lo.code == 7 && bitand(lo.flags, c.OUT_DEVICE_TIMED) > 0, 'line pulse is device-timed');
    expect_error('ysp_device:arg', @() ysp_device('wait', h, -1));
    expect_error('ysp_device:arg', @() ysp_device('pulse', h, 1, NaN));
    ysp_device('close', h);
    expect_error('ysp_device:handle', @() ysp_device('stats', h));

    % ---- a trigger box: outputs ----
    ts = ysp_device('sim_open', 'triggerbox');
    t = ysp_device('open', struct('role', 'trig', 'family', 'triggerbox', 'key', ysp_device('sim_key', ts), ...
                                  'sim', ts, 'pulse', 0.005));
    check(ysp_device('wait_running', t, 2), 'trig: RUNNING');
    ysp_device('pulse', t, 5, 0.002);
    lo = ysp_device('last_out', t);
    check(lo.code == 5 && bitand(lo.flags, c.OUT_PULSE) > 0 && abs(lo.width - 0.002) < 1e-9 && lo.result == 0, 'last_out');
    pause(0.05);
    wr = ysp_device('sim_writes', ts);
    check(numel(wr) >= 3 && isequal(wr(2).data, uint8(5)) && isequal(wr(3).data, uint8(0)), 'trig: code then 0');
    check(wr(3).t - wr(2).t > 0.0019 && wr(3).t - wr(2).t < 0.02, 'trig: host-timed width');
    ysp_device('set', t, 9);
    ysp_device('mark', t, 3, 'stimulus on');
    expect_error('ysp_device:arg', @() ysp_device('set', t, 256));

    % ---- a response rule ----
    r = ysp_device('rule_open', t, struct('kind', {5, 5}, 'control', {1, 2}, 'code', {101, 102}));
    bs = ysp_device('sim_open', 'line', struct('key', 'serial:16C0:0483:RULEBOX:1-9'));
    b = ysp_device('open', struct('role', 'resp', 'family', 'line', 'key', 'serial:16C0:0483:RULEBOX:', ...
                                  'sim', bs, 'rule', r));
    check(ysp_device('wait_running', b, 3), 'box: RUNNING');
    now = ysp_device('now');
    ysp_device('rule_arm', r, now, Inf);
    ysp_device('sim_press', bs, now + 0.1, 0.03, 2);
    ev = collect(b, 2);
    check(numel(ev) == 2, 'rule: 2 events');
    pause(0.05);
    ri = ysp_device('rule_info', r);
    check(ri.fired == 1 && ri.code == 102 && ri.choice == 2 && abs(ri.t_event - ev(1).t) < 1e-6, 'rule fired');
    rf = ysp_device('rule_fires', r);
    check(numel(rf) == 1 && rf(1).code == 102, 'rule fires');
    wr = ysp_device('sim_writes', ts);
    check(any(arrayfun(@(x) isequal(x.data, uint8(102)), wr)), 'the rule wrote the code');
    expect_error('ysp_device:order', @() ysp_device('close', t));
    expect_error('ysp_device:order', @() ysp_device('rule_close', r));
    expect_error('ysp_device:order', @() ysp_device('sim_close', bs));
    ysp_device('close', b);
    ysp_device('rule_close', r);
    ysp_device('close', t);
    wr = ysp_device('sim_writes', ts);
    check(isequal(wr(end).data, uint8(255)), 'close byte 0xFF');
    ysp_device('sim_close', ts);
    ysp_device('sim_close', bs);
    ysp_device('sim_close', s);

    % ---- a wrong device fails; a key that matches nothing times out ----
    ws = ysp_device('sim_open', 'line', struct('identify', false));
    wh = ysp_device('open', struct('role', 'w', 'family', 'line', 'key', ysp_device('sim_key', ws), 'sim', ws));
    expect_error('ysp_device:failed', @() ysp_device('wait_running', wh, 5));
    ysp_device('close', wh);
    kh = ysp_device('open', struct('role', 'k', 'family', 'line', 'key', 'serial:16C0:0483:OTHER:', 'sim', ws));
    check(~ysp_device('wait_running', kh, 0.2), 'no match: timeout');
    expect_error('ysp_device:state', @() ysp_device('pulse', kh, 1, 0.002));
    ysp_device('close', kh);
    ysp_device('sim_close', ws);
    expect_error('ysp_device:arg', @() ysp_device('open', struct('role', 'x', 'family', 'line', 'key', 'k', 'bogus', 1)));
    expect_error('ysp_device:open', @() ysp_device('open', struct('role', 'x', 'family', 'line', 'key', 'COM1')));

    % ---- the rig profile ----
    p = ysp_rigfile('new', 'booth-2');
    ysp_rigfile('bind', p, 'resp', 'line', 'serial:16C0:0483:B1:');
    ysp_rigfile('bind', p, 'trig', 'triggerbox', 'serial:0403:6001:TB0123:');
    ysp_rigfile('set_view', p, 570, 518.4, 1920);
    pi_ = ysp_rigfile('info', p);
    check(strcmp(pi_.rig, 'booth-2') && numel(pi_.roles) == 2 && abs(pi_.ppd - 36.8496) < 1e-4 && ...
          numel(pi_.hash) == 64, 'profile info');
    text = ysp_rigfile('text', p);
    q = ysp_rigfile('parse', text);
    check(strcmp(ysp_rigfile('hash', q), pi_.hash), 'parse round trip keeps the hash');
    d = ysp_rigfile('desc', p, 'resp');
    check(strcmp(d.role, 'resp') && d.family == c.LINE && ~isfield(d, 'pulse'), 'desc');
    expect_error('ysp_rigfile:missing', @() ysp_rigfile('desc', p, 'nope'));
    ysp_rigfile('bind', p, 'eeg', 'lsl', 'lsl:EEG:EEG:abc:host');
    expect_error('ysp_rigfile:format', @() ysp_rigfile('desc', p, 'eeg'));
    expect_error('ysp_rigfile:json', @() ysp_rigfile('parse', '{'));
    rs = ysp_device('sim_open', 'line', struct('key', 'serial:16C0:0483:B1:1-1'));
    d.sim = rs;
    rh = ysp_device('open', d);
    check(ysp_device('wait_running', rh, 3), 'a role from the profile: RUNNING');
    src = ysp_rigfile('source', p, ysp_device('info', rh));
    check(src.tier == c.TIER_UNKNOWN && ~isempty(src.note), 'source: no loopback, no tier');
    src = ysp_rigfile('source', p, 'resp');
    check(src.tier == c.TIER_UNKNOWN, 'source by role');
    ysp_device('close', rh);
    ysp_device('sim_close', rs);
    f = [tempname() '.json'];
    ysp_rigfile('save_file', p, f);
    l = ysp_rigfile('load_file', f);
    li = ysp_rigfile('info', l);
    check(strcmp(li.hash, ysp_rigfile('hash', p)) && isempty(li.notes), 'save_file and load_file');
    delete(f);
    check(ysp_rigfile('remove', p, 'eeg') && ~ysp_rigfile('remove', p, 'eeg'), 'remove');
    ysp_rigfile('close', p);
    ysp_rigfile('close', q);
    ysp_rigfile('close', l);
    expect_error('ysp_rigfile:handle', @() ysp_rigfile('info', p));

    % ---- clear mex closes what is left, in order ----
    os = ysp_device('sim_open', 'triggerbox');
    o = ysp_device('open', struct('role', 'gc_out', 'family', 'triggerbox', 'key', ysp_device('sim_key', os), 'sim', os));
    ysp_device('wait_running', o, 2);
    gr = ysp_device('rule_open', o, struct('code', 1));
    gs = ysp_device('sim_open', 'line');
    ysp_device('open', struct('role', 'gc_box', 'family', 'line', 'key', ysp_device('sim_key', gs), 'sim', gs, 'rule', gr));
    clear ysp_device
    check(true, 'clear mex with a box, a rule and an output open');

    fprintf('PASS (%d checks)\n', YSP_DEVICE_CHECKS);
end

function ev = collect(h, n)
    ev = ysp_device('poll', h);
    t = tic;
    while numel(ev) < n && toc(t) < 5
        ev = [ev, ysp_device('wait', h, 0.2)]; %#ok<AGROW>
    end
end

function check(cond, msg, varargin)
    global YSP_DEVICE_CHECKS %#ok<GVMIS>
    YSP_DEVICE_CHECKS = YSP_DEVICE_CHECKS + 1;
    if ~cond
        error('test_mex_device:fail', ['FAIL: ' msg], varargin{:});
    end
end

function expect_error(id, f)
    global YSP_DEVICE_CHECKS %#ok<GVMIS>
    YSP_DEVICE_CHECKS = YSP_DEVICE_CHECKS + 1;
    try
        f();
    catch e
        if ~strcmp(e.identifier, id)
            error('test_mex_device:fail', 'FAIL: expected %s, got %s: %s', id, e.identifier, e.message);
        end
        return;
    end
    error('test_mex_device:fail', 'FAIL: expected %s, nothing was raised', id);
end
