function test_mex_video()
%TEST_MEX_VIDEO  ysp_video (ysp-vis) in MATLAB or Octave, on the simulated
%   display (no window, no GPU) and miniaudio's null device (no sound card).
%   Checks the frame sequence writer and probe, a movie played to its end
%   with one record per display frame (seconds and int64 ns), each frame
%   against the due rule of its movie time, the controls (pause, seek,
%   seek_frame, show) and their results, the order errors, a movie closed
%   with its gfx, a movie from bytes, the soundtrack on a ysp_audio handle
%   and its pin (ysp_audio('close') is ysp_audio:busy while the movie holds
%   it), a ysp_timeline handle as the movie base, and MPEG-1 when ffmpeg is
%   on the path. Build first (build.m: ysp_audio, ysp_timeline, ysp_vis,
%   ysp_screen, ysp_gfx, ysp_video). Windows only. Prints PASS with the
%   number of checks.

    global YSP_VV_CHECKS %#ok<GVMIS>
    YSP_VV_CHECKS = 0;
    here = fileparts(mfilename('fullpath'));
    addpath(here);
    P = 0.004;                                   % a fast simulated display keeps the test short
    d = tempname();
    mkdir(d);
    cleanup = onCleanup(@() rmdir(d, 's'));      %#ok<NASGU>

    % ---- versions
    [bv, hv] = ysp_video('version');
    check(strcmp(bv, '1.2.0') && ~isempty(regexp(hv, '^\d+\.\d+\.\d+$', 'once')), 'version: binding %s, header %s', bv, hv);
    o = ysp_video('options');
    check(isfield(o, 'loop') && isfield(o, 'hw_decode') && isfield(o, 'timeline') && isfield(o, 'records'), 'options');
    c = ysp_video('constants');
    check(c.F_AUDIO_CLOCK == 16 && c.F_PENDING == 1, 'constants');

    % ---- the frame sequence writer and probe
    p30 = fullfile(d, 's30.yspseq');
    fr = zeros(36, 64, 4, 90, 'uint8');
    for i = 1:90, fr(:, :, 1, i) = i - 1; fr(:, :, 4, i) = 255; end
    check(ysp_video('write_seq', p30, fr, 30) == 90, 'write_seq: 90 frames');
    pr = ysp_video('probe', p30);
    check(pr.w == 64 && pr.h == 36 && pr.fps_num == 30 && pr.frames == 90 && pr.duration == 3 && ...
          pr.duration_ns == int64(3e9) && strcmp(pr.codec, 'seq') && strcmp(pr.format, 'rgba8'), 'probe');
    p2398 = fullfile(d, 's2398.yspseq');
    check(ysp_video('write_seq', p2398, fr(:, :, :, 1:48), [24000 1001], 'qoi') == 48, 'write_seq: 24000/1001, QOI');
    pr = ysp_video('probe', p2398);
    check(pr.fps_num == 24000 && pr.fps_den == 1001 && pr.duration_ns == int64(2002000000), 'probe: t(N) = 2.002 s');
    check(ysp_video('write_seq', fullfile(d, 'r8.yspseq'), zeros(8, 16, 5, 'uint8'), 60) == 5, 'write_seq: R8 [h w n]');
    pr = ysp_video('probe', fullfile(d, 'r8.yspseq'));
    check(strcmp(pr.format, 'r8') && pr.w == 16 && pr.h == 8, 'probe: R8, w and h from the array');
    check(ysp_video('write_seq', fullfile(d, 'r16.yspseq'), zeros(8, 16, 3, 'uint16'), 60) == 3, 'write_seq: R16');
    check(ysp_video('write_seq', fullfile(d, 'f.yspseq'), zeros(8, 16, 4, 2, 'single'), 60) == 2, 'write_seq: RGBA32F');
    expect_error('ysp_video:arg', @() ysp_video('write_seq', fullfile(d, 'x.yspseq'), zeros(8, 16, 3, 'int32'), 60));
    expect_error('ysp_video:arg', @() ysp_video('write_seq', fullfile(d, 'x.yspseq'), zeros(8, 16, 3, 'uint8'), 60, 'qoi'));
    expect_error('ysp_video:arg', @() ysp_video('write_seq', fullfile(d, 'x.yspseq'), fr, 0));
    expect_error('ysp_video:not_found', @() ysp_video('probe', fullfile(d, 'missing.yspseq')));

    % ---- a movie played to its end
    h = ysp_screen('open', 'backend', 'sim', 'sim_period', P);
    g = ysp_gfx('open', h, 0.5);
    expect_error('ysp_video:not_found', @() ysp_video('open', g, fullfile(d, 'missing.yspseq')));
    expect_error('ysp_video:arg', @() ysp_video('open', g, p30, 'light', 'nosuch'));
    expect_error('ysp_video:arg', @() ysp_video('open', g, p30, 'nosuch', 1));
    expect_error('ysp_video:open', @() ysp_video('open', g, p30, 'lead', 2));
    expect_error('ysp_video:handle', @() ysp_video('update', h));
    m = ysp_video('open', g, p30);
    check(isa(m, 'uint64'), 'open: a uint64 handle');
    in = ysp_video('info', m);
    check(in.frames == 90 && in.fps == 30 && strcmp(in.backend, 'seq') && strcmp(in.path, 'upload') && ...
          isfield(in.repeats_by, 'cadence'), 'info');
    check(~isempty(strfind(ysp_video('describe', m), 'frame sequence')), 'describe');
    check(isempty(ysp_video('last', m)), 'last: none before an update');
    pid = ysp_video('play', m);
    check(pid > 0 && isempty(ysp_video('result', m, pid)), 'play: an id, its result pending');
    expect_error('ysp_video:order', @() ysp_video('update', m));      % before the screen's begin
    n = play_all(h, g, m, 10000);
    r = ysp_video('records', m);
    check(numel(r) == n - 1 && r(end).frame == 89, 'records: one per display frame but the last (%d of %d)', numel(r), n);
    check(isa(r(1).onset_ns, 'int64') && r(1).onset == double(r(1).onset_ns) / 1e9 && ...
          r(1).residual_ns == r(1).onset_ns - r(1).due_ns, 'records: seconds and int64 ns');
    check(strcmp(r(1).decision, 'shown') && strcmp(r(1).why, 'due') && r(1).frame == 0, 'records: the first frame shown, due');
    lead = int64(P * 1e9 / 2);
    ok = true;
    for k = 1:numel(r)
        due = idivide((r(k).movie_t_ns + lead) * int64(30), int64(1e9), 'floor');
        if r(k).frame ~= double(due) && strcmp(r(k).why, 'due'), ok = false; end
    end
    check(ok, 'records: each frame is the due frame of its movie time (lead 0.5 period)');
    res = ysp_video('result', m, pid);
    check(strcmp(res.kind, 'play') && res.id == pid && res.frame == 0, 'result: the PLAY record');
    e = ysp_video('events', m);
    check(iscell(e) && strcmp(e{1}.kind, 'open') && strcmp(e{end}.kind, 'end') && e{end}.frame == 89, 'events: open ... end');
    check(isempty(ysp_video('records', m)), 'records: drained');
    in = ysp_video('info', m);
    check(in.shown + in.repeated == numel(r) && in.hash_mismatch == 0, 'info: the counts');
    ysp_video('close', m);
    expect_error('ysp_video:handle', @() ysp_video('update', m));

    % ---- the order errors and the draw
    m = ysp_video('open', g, p30, 'inline_decode', true);
    ysp_video('play', m);
    f = ysp_screen('begin', h); %#ok<NASGU>
    check(ysp_video('update', m), 'update: true while playing');
    expect_error('ysp_video:order', @() ysp_video('update', m));      % once a display frame
    expect_error('ysp_video:order', @() ysp_video('draw', m));        % outside the gfx's frame
    ysp_gfx('begin', g);
    expect_error('ysp_video:order', @() ysp_video('update', m));      % a texture changes only between frames
    ysp_video('draw', m, [10 -5 32 18], 15, 0.5, true);
    expect_error('ysp_video:arg', @() ysp_video('draw', m, [1 2 3]));
    ysp_gfx('end', g);
    ysp_screen('flip', h);

    % ---- the controls
    play_all(h, g, m, 10);
    pz = ysp_video('pause', m);
    play_all(h, g, m, 5);
    res = ysp_video('result', m, pz);
    check(strcmp(res.kind, 'pause') && res.movie_t > 0, 'pause: its result');
    ysp_video('records', m);
    sk = ysp_video('seek', m, 2.0, 'paused');
    play_all(h, g, m, 10);
    res = ysp_video('result', m, sk);
    check(strcmp(res.kind, 'seek') && res.target == 60, 'seek: target frame 60');
    check(abs(ysp_video('movie_time', m, ysp_screen('now')) - 2.0) < 1e-9, 'movie_time: paused at t(60)');
    ysp_video('play', m);
    play_all(h, g, m, 10);
    r = ysp_video('records', m);
    check(r(1).frame == 60, 'play after the paused seek goes on from frame 60');
    sf = ysp_video('seek_frame', m, 15);
    play_all(h, g, m, 10);
    res = ysp_video('result', m, sf);
    check(res.target == 15, 'seek_frame: target 15');
    ysp_screen('begin', h);
    ysp_video('show', m, 40);
    ysp_video('update', m);
    ysp_gfx('begin', g); ysp_video('draw', m); ysp_gfx('end', g);
    ysp_screen('flip', h);
    l = ysp_video('last', m);
    check(l.frame == 40 && strcmp(l.why, 'manual'), 'show: frame 40, manual');
    expect_error('ysp_video:arg', @() ysp_video('seek', m, -1));
    expect_error('ysp_video:arg', @() ysp_video('seek', m, 1, 'later'));
    expect_error('ysp_video:arg', @() ysp_video('play', m, 0));
    expect_error('ysp_video:not_found', @() ysp_video('result', m, 10000));

    % ---- a gfx that closes closes its movies
    ysp_gfx('close', g);
    expect_error('ysp_video:closed', @() ysp_video('update', m));
    check(isstruct(ysp_video('records', m)), 'records: readable after the close');
    ysp_video('close', m);
    g = ysp_gfx('open', h, 0.5);

    % ---- bytes: data and index
    fid = fopen(p30, 'r'); bytes = fread(fid, Inf, '*uint8'); fclose(fid);
    m = ysp_video('open', g, '', 'data', bytes, 'records', 16, 'inline_decode', true);
    ysp_video('play', m);
    n = play_all(h, g, m, 10000);
    [r, lost] = ysp_video('records', m);
    check(numel(r) == 16 && lost == n - 1 - 16 && r(end).frame == 89, 'data: a movie from bytes; records keeps 16');
    ysp_video('close', m);
    expect_error('ysp_video:arg', @() ysp_video('open', g, p30, 'data', bytes));

    % ---- the soundtrack on a ysp_audio handle, pinned
    a = ysp_audio('open', struct('backend', 'null'));
    wav = fullfile(d, 's30.wav');
    audiowrite(wav, zeros(3 * 48000, 2), 48000);       % 90 frames at 30 fps: 3 s, to the sample
    short = fullfile(d, 'short.wav');
    audiowrite(short, zeros(3 * 48000 - 1, 2), 48000);
    m = ysp_video('open', g, p30);
    expect_error('ysp_video:refused', @() ysp_video('soundtrack', m, a, short));
    ysp_audio('close', a);                              % the refusal kept no pin
    a = ysp_audio('open', struct('backend', 'null'));
    expect_error('ysp_video:arg', @() ysp_video('soundtrack', m, uint64(5), wav));
    ysp_video('soundtrack', m, a, wav, 'ramp', 0.002);
    expect_error('ysp_video:order', @() ysp_video('soundtrack', m, a, wav));
    expect_error('ysp_audio:busy', @() ysp_audio('close', a));
    ysp_video('play', m);
    play_all(h, g, m, 10000);
    r = ysp_video('records', m);
    fl = arrayfun(@(x) bitand(x.flags, c.F_AUDIO_CLOCK) ~= 0, r);
    check(r(end).frame == 89 && any(fl), 'soundtrack: played; the movie clock followed the sound');
    e = ysp_video('events', m);
    snd = e(cellfun(@(x) strcmp(x.kind, 'sound'), e));
    check(~isempty(snd) && strcmp(snd{1}.action, 'start') && snd{1}.sample == 0, 'soundtrack: the SOUND start');
    ysp_video('follow_audio', m, []);
    ysp_video('follow_audio', m, a);
    ysp_video('close', m);
    ysp_audio('close', a);                              % the pins went back with the movie
    check(true, 'soundtrack: the Audio closes once the movie has');

    % ---- a ysp_timeline handle as the movie base
    tl = ysp_timeline('open', struct('channels', 1));
    m = ysp_video('open', g, p30, 'timeline', tl, 'base', 1, 'inline_decode', true);
    expect_error('ysp_timeline:busy', @() ysp_timeline('close', tl));
    ysp_video('play', m);
    play_all(h, g, m, 20);
    t = ysp_screen('now');
    check(abs(ysp_timeline('base_time', tl, 1, t) - ysp_video('movie_time', m, t)) < 1e-9, 'timeline: the base is the movie clock');
    ysp_video('close', m);
    ysp_timeline('close', tl);

    % ---- MPEG-1, when ffmpeg is on the path
    [st, ~] = system('ffmpeg -version');
    if st == 0
        mp = fullfile(d, 'm30.mpg');
        system(sprintf('ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=160x96:rate=30 -frames:v 60 -c:v mpeg1video -q:v 4 -g 15 -bf 0 -f mpeg "%s"', mp));
        check(ysp_video('make_index', mp) == 60, 'make_index: 60 MPEG-1 frames');
        pr = ysp_video('probe', mp);
        check(strcmp(pr.codec, 'mpeg1') && strcmp(pr.format, 'i420') && strcmp(pr.matrix, 'bt601'), 'probe: MPEG-1');
        m = ysp_video('open', g, mp);
        in = ysp_video('info', m);
        check(strcmp(in.backend, 'plmpeg'), 'MPEG-1 through pl_mpeg');
        ysp_video('play', m);
        play_all(h, g, m, 10000);
        r = ysp_video('records', m);
        in = ysp_video('info', m);
        check(r(end).frame == 59 && in.hash_mismatch == 0 && in.ts_mismatch == 0, 'MPEG-1: played, every hash the index''s');
        ysp_video('close', m);
    else
        fprintf('skipped MPEG-1: ffmpeg is not on the path\n');
    end

    ysp_screen('close', h);
    fprintf('PASS test_mex_video (%d checks)\n', YSP_VV_CHECKS);
end

function n = play_all(h, g, m, limit)
    n = 0;
    while n < limit
        ysp_screen('begin', h);
        playing = ysp_video('update', m);
        ysp_gfx('begin', g);
        ysp_video('draw', m);
        ysp_gfx('end', g);
        ysp_screen('flip', h);
        n = n + 1;
        if ~playing, break; end
    end
    ysp_screen('wait_flip', h);
end

function check(cond, msg, varargin)
    global YSP_VV_CHECKS %#ok<GVMIS>
    YSP_VV_CHECKS = YSP_VV_CHECKS + 1;
    if ~cond
        error('test_mex_video:fail', ['FAIL: ' msg], varargin{:});
    end
end

function expect_error(id, f)
    global YSP_VV_CHECKS %#ok<GVMIS>
    YSP_VV_CHECKS = YSP_VV_CHECKS + 1;
    try
        f();
    catch e
        if ~strcmp(e.identifier, id)
            error('test_mex_video:fail', 'FAIL: expected %s, got %s: %s', id, e.identifier, e.message);
        end
        return;
    end
    error('test_mex_video:fail', 'FAIL: expected %s, nothing was raised', id);
end
