function test_mex_audio()
%TEST_MEX_AUDIO  Key checks of the ysp_audio MEX function on miniaudio's null
%   device (no sound hardware). Runs in MATLAB and in Octave. Prints PASS,
%   or stops with the error of the first check that failed. Build first
%   (run build.m), then from this directory: test_mex_audio.
%   tests/adapt/audio_test.c checks the header itself; this checks that the
%   binding drives it as the C API does.

    global YSP_AU_CHECKS %#ok<GVMIS>
    YSP_AU_CHECKS = 0;
    here = fileparts(mfilename('fullpath'));
    addpath(here);
    [bv, hv] = ysp_audio('version');
    fprintf('ysp_audio %s (ysp/audio.h %s)\n', bv, hv);
    % The null device's fit is of callback times on the real clock; a loaded
    % machine moves it by more than a frame. The plumbing checks allow 5 ms.
    TOL = 5e6; TOLF = 240;                       % ns, frames
    UNCONFIRMED = 64; LATE = 2; CANCELED = 4; STOPPED = 8; SIM = 4;

    % ---- module commands
    [t, tns] = ysp_audio('now');
    check(isa(tns, 'int64') && abs(double(tns) / 1e9 - t) < 1e-3, 'now in seconds and int64 ns');
    check(abs(ysp_audio('db', -20) - 0.1) < 1e-6, 'db');
    d = ysp_audio('devices', 'null');
    check(numel(d) == 1 && d(1).default && ~isempty(strfind(d(1).name, 'NULL')), 'the null device list');
    check(isstruct(ysp_audio('devices')), 'the default device list');
    expect_error('ysp_audio:arg', @() ysp_audio('devices', 'nosuch'));

    % ---- open
    h = ysp_audio('open', struct('backend', 'null'));
    check(isa(h, 'uint64') && ysp_audio('is_open', h), 'open the null device');
    c = ysp_audio('caps', h);
    check(strcmp(c.backend, 'null') && c.rate == 48000 && c.channels == 2 && c.period_frames == 480, 'caps');
    check(abs(c.period - 0.01) < 1e-12 && strcmp(c.pos_source, 'callback') && isempty(c.os_latency), 'caps in seconds, no claim');
    check(~isempty(strfind(ysp_audio('describe', h), 'backend=Null')), 'describe');
    [l, lns] = ysp_audio('lead', h);
    % At most two periods, and below 0 while the next callback is more than
    % a period late on the fit: the null device's callbacks wait in 10 ms
    % sleeps (docs/audio.md, "The lead on the null device").
    check(isa(lns, 'int64') && lns ~= 0 && l == double(lns) / 1e9, 'lead in seconds and int64 ns');
    check(l > -0.5 && l <= 2 * c.period + TOL / 1e9, 'lead: at most two periods, maybe negative');
    expect_error('ysp_audio:open', @() ysp_audio('open', struct('device', 'no such device name')));
    expect_error('ysp_audio:arg', @() ysp_audio('open', struct('backend', 'null', 'chanels', 2)));

    % ---- synthesis
    b = ysp_audio('tone', h, struct('hz', 1000, 'duration', 0.1, 'peak', 0.25, 'ramp', 0.005, 'id', 9));
    x = ysp_audio('samples', b);
    check(isa(x, 'single') && isequal(size(x), [4800 1]), 'a tone is 4800 x 1 single');
    k = [1000 2000 2345];
    check(max(abs(double(x(k + 1)) - 0.25 * sin(2 * pi * 1000 * k' / 48000))) < 1e-6, 'tone samples: the formula');
    check(abs(x(1)) < 1e-6, 'the ramp starts at 0');
    n1 = ysp_audio('samples', ysp_audio('noise', h, struct('duration', 0.05, 'rms', 0.05, 'seed', 7)));
    n2 = ysp_audio('samples', ysp_audio('noise', h, struct('duration', 0.05, 'rms', 0.05, 'seed', uint64(7))));
    check(isequal(n1, n2) && abs(sqrt(mean(double(n1) .^ 2)) - 0.05) < 0.005, 'noise: seed and rms');
    expect_error('ysp_audio:arg', @() ysp_audio('noise', h, struct('duration', 1, 'rms', 0.4, 'seed', 1)));
    cl = ysp_audio('samples', ysp_audio('click', h, struct('peak', 0.5)));
    check(isequal(cl, single(0.5)), 'a click of one frame');

    % ---- buffers from MATLAB data
    m = [zeros(480, 1), 0.5 * ones(480, 1)];
    bb = ysp_audio('buffer', h, m, 3);
    check(isequal(ysp_audio('samples', bb), single(m)), 'a frames x channels buffer comes back');
    check(isequal(size(ysp_audio('samples', ysp_audio('buffer', h, single([0.1 0.2 0.3])))), [3 1]), 'a row vector is one channel');
    expect_error('ysp_audio:arg', @() ysp_audio('buffer', h, [0 NaN]));
    expect_error('ysp_audio:arg', @() ysp_audio('buffer', h, []));

    % ---- play at a time: the record
    beep = ysp_audio('tone', h, struct('hz', 1000, 'duration', 0.02, 'peak', 0.03, 'id', 4));
    t = ysp_audio('now') + 0.15;
    id = ysp_audio('play_at', h, beep, t);
    r0 = ysp_audio('result', h, id);
    check(r0.pending && bitand(r0.flags, 1), 'pending before the onset');
    r = ysp_audio('wait', h, id, Inf);   % INT64_MAX ns: overflowed before ysp/audio.h 0.7.2
    check(~r.pending && r.tier == SIM && r.buffer_id == 4 && bitand(r.flags, UNCONFIRMED), 'the record: tier SIM, unconfirmed');
    check(isa(r.target_ns, 'int64') && abs(double(r.target_ns) - t * 1e9) <= 1, 'target, exact ns');
    check(r.residual_ns == r.onset_ns - r.target_ns && abs(double(r.residual_ns)) <= TOL && ~bitand(r.flags, LATE), 'on its time');
    f = ysp_audio('frame_at', h, r.onset);
    check(abs(f - r.start_frame) <= TOLF, 'frame_at');
    [s, sns] = ysp_audio('time_of', h, f);
    check(abs(double(sns - r.onset_ns)) <= TOL && abs(s - double(sns) / 1e9) < 1e-6, 'time_of');
    id = ysp_audio('play', h, bb, struct('at', ysp_audio('now') + 0.12, 'db', -6, 'channels', [2], 'offset', 48, 'frames', 96));
    r = ysp_audio('wait', h, id, 2);
    check(r.sample == 48 && r.buffer_id == 3, 'play with options');
    r = ysp_audio('wait', h, ysp_audio('play_at', h, beep, ysp_audio('now') - 0.1));
    check(bitand(r.flags, LATE) ~= 0, 'already past: LATE');

    % ---- cancel, stop, gain
    id = ysp_audio('play_at', h, beep, ysp_audio('now') + 1);
    ysp_audio('cancel', h, id);
    r = ysp_audio('wait', h, id);
    check(bitand(r.flags, CANCELED) && r.start_frame == -1, 'cancel');
    long = ysp_audio('tone', h, struct('hz', 300, 'duration', 0.2, 'peak', 0.03));
    id = ysp_audio('play', h, long, struct('at', ysp_audio('now') + 0.1, 'loops', -1));
    ysp_audio('gain', h, id, -12, ysp_audio('now') + 0.15, 0.01);
    ysp_audio('stop', h, id, ysp_audio('now') + 0.3, 0.02);
    ysp_audio('wait', h, id);
    t0 = tic;
    r = ysp_audio('result', h, id);
    while r.end_frame == 0 && toc(t0) < 3
        pause(0.01);
        ysp_audio('update', h);
        r = ysp_audio('result', h, id);
    end
    check(bitand(r.flags, STOPPED) && r.end_frame > r.start_frame, 'stop with a ramp');

    % ---- refusals
    expect_error('ysp_audio:arg', @() ysp_audio('play_at', h, beep, -1));
    expect_error('ysp_audio:arg', @() ysp_audio('play_at', h, beep, NaN));
    expect_error('ysp_audio:not_found', @() ysp_audio('result', h, 1e9));
    expect_error('ysp_audio:handle', @() ysp_audio('play_at', h, h, 0));
    id = ysp_audio('play_at', h, beep, ysp_audio('now') + 5);
    expect_error('ysp_audio:timeout', @() ysp_audio('wait', h, id, 0.02));
    ysp_audio('cancel', h, id);
    r = ysp_audio('wait', h, id, Inf);
    check(~r.pending && bitand(r.flags, CANCELED), 'wait with Inf on a canceled sound');
    ysp_audio('wait', h, id);
    ysp_audio('arena_reset', h);
    expect_error('ysp_audio:arg', @() ysp_audio('play_at', h, beep, 0));

    % ---- a stream fed from MATLAB
    st = ysp_audio('stream_open', h, struct('channels', 1, 'ring', 0.5, 'id', 3));
    i0 = ysp_audio('stream_info', st);
    check(strcmp(i0.state, 'idle'), 'a stream starts idle');
    check(ysp_audio('stream_write', st, 0.01 * ones(4800, 1)) == 4800, 'stream_write');
    t = ysp_audio('now') + 0.15;
    id = ysp_audio('play', h, st, struct('at', t));
    r = ysp_audio('wait', h, id);
    check(r.buffer_id == 3 && r.sample == 0 && abs(double(r.residual_ns)) <= TOL, 'a stream plays at its time');
    i1 = ysp_audio('stream_info', st);
    check(i1.started && i1.origin == r.start_frame && abs(ysp_audio('sample_at', st, r.onset)) <= TOLF, 'origin and sample_at');
    ysp_audio('stream_end', st);
    ysp_audio('stream_close', st);
    expect_error('ysp_audio:handle', @() ysp_audio('stream_write', st, 0));

    % ---- WAV files
    p = [tempname() '.wav'];
    audiowrite(p, 0.03 * ones(4800, 1), 48000, 'BitsPerSample', 16);
    info = ysp_audio('wav_info', p);
    check(info.rate == 48000 && info.frames == 4800 && info.channels == 1, 'wav_info');
    w = ysp_audio('wav_open', h, p, struct('id', 11));
    check(ysp_audio('wav_wants', w) && ysp_audio('wav_feed', w) == 4800, 'wav_feed');
    t = ysp_audio('now') + 0.15;
    r = ysp_audio('wait', h, ysp_audio('play', h, w, struct('at', t)));
    check(r.buffer_id == 11 && abs(double(r.residual_ns)) <= TOL, 'a WAV stream plays at its time');
    ysp_audio('wav_close', w);
    lb = ysp_audio('load_wav', h, p, 2);
    lx = ysp_audio('samples', lb);
    check(size(lx, 1) == 4800 && abs(double(lx(10)) - round(0.03 * 32767) / 32768) < 1e-6, 'load_wav');
    q = [tempname() '.wav'];
    audiowrite(q, zeros(441, 1), 44100);
    expect_error('ysp_audio:format', @() ysp_audio('wav_open', h, q));
    delete(p); delete(q);

    % ---- close
    ysp_audio('close', h);
    expect_error('ysp_audio:handle', @() ysp_audio('caps', h));
    expect_error('ysp_audio:closed', @() ysp_audio('samples', lb));

    % ---- capture of the null input
    cp = ysp_audio('capture_open', struct('backend', 'null', 'channels', 1, 'ring', 0.5));
    ci = ysp_audio('capture_info', cp);
    check(ci.tier == SIM && ci.rate == 48000 && ci.channels == 1, 'capture_open');
    pause(0.1);
    [cx, first] = ysp_audio('capture_read', cp);
    check(isa(cx, 'single') && size(cx, 2) == 1 && size(cx, 1) > 0 && all(cx == 0) && first >= 0, 'capture_read');
    [ct, ctns] = ysp_audio('capture_time_of', cp, first);
    check(isa(ctns, 'int64') && abs(ysp_audio('capture_frame_at', cp, ct) - first) <= 1, 'capture times');
    ysp_audio('capture_close', cp);
    expect_error('ysp_audio:handle', @() ysp_audio('capture_info', cp));

    fprintf('PASS (%d checks)\n', YSP_AU_CHECKS);
end

function check(cond, msg, varargin)
    global YSP_AU_CHECKS %#ok<GVMIS>
    YSP_AU_CHECKS = YSP_AU_CHECKS + 1;
    if ~cond
        error('test_mex_audio:fail', ['FAIL: ' msg], varargin{:});
    end
end

function expect_error(id, f)
    global YSP_AU_CHECKS %#ok<GVMIS>
    YSP_AU_CHECKS = YSP_AU_CHECKS + 1;
    try
        f();
    catch e
        if ~strcmp(e.identifier, id)
            error('test_mex_audio:fail', 'FAIL: expected %s, got %s: %s', id, e.identifier, e.message);
        end
        return;
    end
    error('test_mex_audio:fail', 'FAIL: expected %s, nothing was raised', id);
end
