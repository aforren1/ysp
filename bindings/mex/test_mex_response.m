function test_mex_response()
%TEST_MEX_RESPONSE  Key checks of the ysp_response MEX function. Runs in
%   MATLAB and in Octave. Prints PASS with the number of checks, or stops
%   with the error of the first check that failed. Build first (run
%   build.m), then from this directory: test_mex_response.
%   tests/adapt/response_test.c checks the header's rules themselves; here
%   each rule is checked once through the binding, with its units.

    global YSP_RESPONSE_CHECKS %#ok<GVMIS>
    YSP_RESPONSE_CHECKS = 0;
    here = fileparts(mfilename('fullpath'));
    addpath(here);
    [v, vin] = ysp_response('version');
    fprintf('ysp_response %s (ysp/response.h %s)\n', v, vin);
    K = ysp_response('constants');
    check(K.ENDED == 2 && K.KIND_BOX == 5 && K.R_SENT_MISMATCH == 1024 && K.UNC_MAX_US == 65535, 'constants');
    check(ysp_response('scancode', 'F') == 9 && isempty(ysp_response('scancode', 'nokey')), 'scancode');
    check(strcmp(ysp_response('key_name', 80), 'left'), 'key_name');

    % an event: defaults, key names, units
    ev = ysp_response('event', struct('t', 12.3456789014, 'control', 'f', 'unc', 0.0004));
    check(ev.t_ns == int64(12345678901) && isa(ev.t_ns, 'int64') && ev.control == 9 && ev.type == 1, 'event fields');
    ev = ysp_response('event', struct('t', 1, 'type', K.PRESS, 'control', 'f', 'unc', 1));
    check(ev.value == 1 && ev.unc == 0.065535, 'event value and unc');
    names = {'t', 't_ns', 'device', 'control', 'code', 'mods', 'kind', 'type', 'flags', 'stamp', 'unc', 'x', 'y', ...
             'value', 'aux', 'ticks'};
    check(isequal(fieldnames(ev)', names), 'event field order');

    % a keyboard trial: RT from the onset, classes, release
    h = ysp_response('open', struct('choices', {{'f', 'j'}}, 'duration', 1.5, 'min_rt', 0.1));
    ysp_response('arm', h, 10.0);
    check(ysp_response('key', h, 10.2, 'f') == K.OPEN, 'early press');
    ysp_response('key', h, 10.25, 'f', false);
    st = ysp_response('set_onset', h, 10.5, struct('src', 'flip', 'tier', 1, 'residual', -2e-5, 'frame', 31));
    check(st == K.OPEN, 'onset');
    ysp_response('key', h, 10.55, 'j');
    ysp_response('key', h, 10.555, 'j', 0);
    check(ysp_response('key', h, 10.9, 'j') == K.ENDED, 'response ends the window');
    ysp_response('key', h, 10.97, 'j', false);
    [r, row] = ysp_response('finish', h);
    check(strcmp(r.response, 'j') && r.choice == 2 && abs(r.rt - 0.4) < 1e-12 && r.rt_ns == int64(400000000), 'rt and response');
    check(r.rt_key_duration_ns == int64(70000000) && r.t_onset_ns == int64(10500000000), 'hold duration, onset ns');
    check(r.onset_src == K.ONSET_FLIP && r.onset_tier == 1 && abs(r.onset_residual + 2e-5) < 1e-15 && r.onset_frame == 31, 'onset fields');
    check(r.n_early == 1 && r.n_anticipations == 1 && r.n_entries == 3 && bitand(r.flags, K.R_RESPONDED), 'counts');
    check(isequal(fieldnames(row)', {'response', 'rt_s', 'rt_key_duration_s', 'onset_src', 'onset_tier', ...
          'onset_residual_s', 'landing_residual_s', 'onset_frame', 'rsp_tier', 'rsp_flags', 'n_anticipations', 'n_early'}), 'data row columns');
    check(strcmp(row.response, 'j') && abs(row.rt_s - 0.4) < 1e-12 && isnan(row.landing_residual_s) && row.rsp_tier == 0, 'data row values');
    e = ysp_response('entries', h);
    check(numel(e) == 3 && e(3).choice == 2 && bitand(e(1).flags, K.E_EARLY) && bitand(e(2).flags, K.E_ANTICIPATION), 'entries');
    check(e(3).t_release_ns == int64(10970000000) && isequaln(ysp_response('data_row', h), row), 'entry release, data_row');

    % timeout, no deadline
    ysp_response('arm', h, 1.0);
    ysp_response('set_onset', h, 1.0);
    check(ysp_response('update', h, 2.4999) == K.OPEN && ysp_response('update', h, 2.5) == K.ENDED, 'deadline');
    r = ysp_response('finish', h);
    check(bitand(r.flags, K.R_TIMEOUT) && isnan(r.rt) && r.rt_ns == intmin('int64') && r.choice == 0 && isempty(r.response), 'timeout');
    ysp_response('close', h);
    h = ysp_response('open', struct('choices', {{'f'}}, 'duration', Inf));
    ysp_response('arm', h, 0);
    ysp_response('set_onset', h, 0);
    check(ysp_response('update', h, 1e5) == K.OPEN, 'Inf is no deadline');
    ysp_response('close', h);

    % plan, then a final onset a frame late
    h = ysp_response('open', struct('choices', {{'f', 'j'}}, 'duration', 1.5, 'min_rt', 0.1));
    ysp_response('arm', h, 0);
    ysp_response('set_onset', h, 1.0, struct('final', false, 'src', 'plan', 'residual', 0.001, 'frame', 60));
    check(ysp_response('plan_frame', h) == 60, 'plan frame');
    check(ysp_response('key', h, 1.11, 'f') == K.ENDED, 'valid against the plan');
    ysp_response('set_onset', h, 1.0167, struct('src', 'flip', 'tier', 1, 'frame', 60));
    check(isempty(ysp_response('plan_frame', h)), 'final onset');
    r = ysp_response('finish', h);
    check(bitand(r.flags, K.R_ENDED_EARLY) && isnan(r.rt) && abs(r.plan_residual - 0.001) < 1e-15, 'ended early');
    ysp_response('close', h);

    % a response box, a source, the event struct array of one frame
    sb = struct('name', {'left', 'right'}, 'kind', {'box', 'box'}, 'control', {1, 2}, 'device', {2, 2});
    src = struct('kind', 'box', 'tier', 1, 'partial', true, 'lo', -2e-4, 'hi', 2e-4, 'note', 'loopback');
    h = ysp_response('open', struct('choices', sb, 'duration', 2, 'sources', src));
    ysp_response('arm', h, 20);
    ysp_response('set_onset', h, 20.25);
    evs = struct('t', {20.4, 20.6123, 20.7}, 'kind', K.KIND_BOX, 'type', {K.PRESS, K.PRESS, K.RELEASE}, ...
                 'control', {2, 2, 2}, 'device', {3, 2, 2});
    check(ysp_response('feed', h, evs) == K.ENDED, 'feed a struct array');
    r = ysp_response('finish', h);
    check(strcmp(r.response, 'right') && abs(r.rt - 0.3623) < 1e-12 && r.stamp_tier == 1 && r.stamp_partial, 'box response');
    check(abs(r.stamp_lo + 2e-4) < 1e-15 && abs(r.rt_key_duration - 0.0877) < 1e-12, 'source bounds, release');
    check(ysp_response('feed', h, struct('t', {})) == K.ENDED, 'empty feed gives the status');
    ysp_response('close', h);

    % ALL mode: the keyboard only; 'all' takes every kind but SYNC
    h = ysp_response('open', struct());
    ysp_response('arm', h, 0);
    ysp_response('set_onset', h, 0);
    check(ysp_response('feed', h, struct('t', 0.2, 'kind', 'mouse', 'type', 1, 'control', 1)) == K.OPEN, 'a click is no key');
    check(ysp_response('key', h, 0.3, 'q') == K.ENDED, 'any key');
    ysp_response('close', h);
    h = ysp_response('open', struct('kinds', 'all'));
    ysp_response('arm', h, 0);
    ysp_response('set_onset', h, 0);
    check(ysp_response('feed', h, struct('t', 0.1, 'kind', 'sync', 'type', 1, 'control', 1)) == K.OPEN, 'sync is not a response');
    ysp_response('close', h);
    h = ysp_response('open', struct('kinds', {{'sync'}}));
    ysp_response('arm', h, 0);
    ysp_response('set_onset', h, 0);
    check(ysp_response('feed', h, struct('t', 0.1, 'kind', 'sync', 'type', 1, 'control', 1)) == K.ENDED, 'wait for the scanner');
    ysp_response('close', h);

    % a crossing and the trace
    g = struct('name', 'trigger', 'kind', 'gamepad', 'cross', 'rising', 'control', K.AXIS_GAMEPAD + 5, 'level', 0.5, 'hysteresis', 0.2);
    h = ysp_response('open', struct('choices', g, 'trace_cap', 2, 'trace_kinds', {{'gamepad'}}));
    ysp_response('arm', h, 0);
    ysp_response('set_onset', h, 0);
    s = struct('t', {0.1, 0.25, 0.3}, 'kind', 'gamepad', 'type', K.SAMPLE, 'control', K.AXIS_GAMEPAD + 5, 'value', {0.3, 0.6, 0.7});
    check(ysp_response('feed', h, s) == K.ENDED, 'crossing');
    r = ysp_response('finish', h);
    tr = ysp_response('trace', h);
    check(r.rt_ns == int64(250000000) && numel(tr) == 2 && bitand(r.flags, K.R_TRACE_FULL) && abs(tr(2).value - 0.6) < 1e-7, 'trace');
    ysp_response('close', h);

    % codes sent at the presses
    h = ysp_response('open', struct('choices', {{'f', 'j'}}, 'duration', 1.5, 'min_rt', 0.1));
    ysp_response('arm', h, 0);
    ysp_response('set_onset', h, 0);
    ysp_response('key', h, 0.4, 'j');
    ysp_response('set_sent', h, struct('t', 0.4, 'kind', 0, 'device', 0, 'control', 'j', 'choice', 2, 'written', true));
    r = ysp_response('finish', h);
    check(~bitand(r.flags, K.R_SENT_MISMATCH) && r.n_sent == 1, 'codes agree');
    ysp_response('set_sent', h, struct('t', 0.4, 'kind', 0, 'device', 0, 'control', 'j', 'choice', 1, 'written', true));
    r = ysp_response('finish', h);
    check(bitand(r.flags, K.R_SENT_MISMATCH) ~= 0, 'another choice''s code');

    % offline: score a logged stream; the same as the live loop
    lg = struct('t', {0.5, 0.6, 1.05, 1.62, 1.70, 1.75}, 'type', {1, 2, 1, 1, 2, 2}, ...
                'control', {'f', 'f', 'j', 'f', 'f', 'j'});
    ysp_response('close', h);
    h = ysp_response('open', struct('choices', {{'f', 'j'}}, 'duration', 1.5, 'min_rt', 0.1));
    ysp_response('feed', h, lg(1:2));
    ysp_response('arm', h, 0.9);
    ysp_response('set_onset', h, 1.0);
    ysp_response('feed', h, lg(3:end));
    a = ysp_response('finish', h);
    ysp_response('close', h);
    h = ysp_response('open', struct('choices', {{'f', 'j'}}, 'duration', 1.5, 'min_rt', 0.1));
    [b, brow] = ysp_response('score', h, lg, 1.0, struct('arm', 0.9));
    check(isequaln(a, b) && strcmp(b.response, 'f') && b.rt_ns == int64(620000000) && b.n_anticipations == 1, 'score is the live loop');
    check(strcmp(brow.response, 'f'), 'score gives the row');
    ysp_response('close', h);
    h = ysp_response('open', struct('choices', {{'f', 'j'}}, 'duration', 1.5, 'min_rt', 0.03));
    b = ysp_response('score', h, lg, 1.0, struct('arm', 0.9));
    check(strcmp(b.response, 'j') && b.rt_ns == int64(50000000), 'rescore with other rules');
    ysp_response('close', h);

    % seconds keep their ns at a day of uptime
    h = ysp_response('open', struct('choices', {{'f'}}));
    ysp_response('arm', h, 86400);
    ysp_response('set_onset', h, 86400.000000001);
    ysp_response('key', h, 86400.4000000004, 'f');
    r = ysp_response('finish', h);
    check(r.t_onset_ns == int64(86400000000001) && r.t_response_ns == int64(86400400000000), 'ns at a day');

    % errors carry their id
    expect_error('ysp_response:arg', @() ysp_response('arm', h, -1));
    expect_error('ysp_response:arg', @() ysp_response('set_onset', h, NaN));
    expect_error('ysp_response:arg', @() ysp_response('key', h, 1, 'nokey'));
    expect_error('ysp_response:arg', @() ysp_response('feed', h, struct('t', 1, 'type', 0)));
    expect_error('ysp_response:arg', @() ysp_response('feed', h, struct('kind', 0)));
    expect_error('ysp_response:arg', @() ysp_response('open', struct('choices', {{'nokey'}})));
    expect_error('ysp_response:arg', @() ysp_response('open', struct('choices', 'fj')));
    expect_error('ysp_response:arg', @() ysp_response('open', struct('choices', struct('controll', 4))));
    expect_error('ysp_response:arg', @() ysp_response('open', struct('choises', {{'f'}})));
    expect_error('ysp_response:arg', @() ysp_response('open', struct('min_rt', 2, 'duration', 1)));
    expect_error('ysp_response:arg', @() ysp_response('open', struct('max_hold', Inf)));
    expect_error('ysp_response:arg', @() ysp_response('open', struct('kinds', {{'nope'}})));
    ysp_response('close', h);
    expect_error('ysp_response:handle', @() ysp_response('finish', h));
    expect_error('ysp_response:usage', @() ysp_response('nocommand', ysp_response('open')));

    fprintf('PASS (%d checks)\n', YSP_RESPONSE_CHECKS);
end

function check(cond, msg, varargin)
    global YSP_RESPONSE_CHECKS %#ok<GVMIS>
    YSP_RESPONSE_CHECKS = YSP_RESPONSE_CHECKS + 1;
    if ~cond
        error('test_mex_response:fail', ['FAIL: ' msg], varargin{:});
    end
end

function expect_error(id, f)
    global YSP_RESPONSE_CHECKS %#ok<GVMIS>
    YSP_RESPONSE_CHECKS = YSP_RESPONSE_CHECKS + 1;
    try
        f();
    catch e
        if ~strcmp(e.identifier, id)
            error('test_mex_response:fail', 'FAIL: expected %s, got %s: %s', id, e.identifier, e.message);
        end
        return;
    end
    error('test_mex_response:fail', 'FAIL: expected %s, nothing was raised', id);
end
