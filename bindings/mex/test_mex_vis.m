function test_mex_vis()
%TEST_MEX_VIS  ysp-vis in MATLAB or Octave, on the simulated display (no
%   window, no GPU): the core ysp_vis and the thin ysp_screen, ysp_gfx and
%   ysp_video. Checks the frame loop and its records in seconds and int64
%   ns, the options and their refusals, the order errors, aborts, events,
%   window calls without a window, the phase A draw path on gfx's null
%   backend, the shared tables of ysp_audio and ysp_timeline, and the
%   lifetime rule: one handle table for the three MEX files, and a clear of
%   one MEX file leaves the others' objects open until the last one goes.
%   Build first (build.m: ysp_audio, ysp_timeline, ysp_vis, ysp_screen,
%   ysp_gfx, ysp_video). Windows only. Prints PASS with the number of checks.

    global YSP_VIS_CHECKS %#ok<GVMIS>
    YSP_VIS_CHECKS = 0;
    here = fileparts(mfilename('fullpath'));
    addpath(here);
    clear ysp_screen ysp_gfx ysp_video;
    P = 0.004;                         % a fast simulated display keeps the test short
    sim = {'backend', 'sim', 'sim_period', P};

    % ---- versions and the core
    [bv, hv] = ysp_screen('version');
    check(strcmp(bv, '1.2.0') && ~isempty(regexp(hv, '^\d+\.\d+\.\d+$', 'once')), 'version: binding %s, header %s', bv, hv);
    [~, gv] = ysp_gfx('version');
    [~, vv] = ysp_video('version');
    info = ysp_vis('info');
    check(info.users == 3 && strcmp(info.screen_h, hv) && strcmp(info.gfx_h, gv) && strcmp(info.video_h, vv), ...
          'the core: three thin files attached, the headers'' versions');
    check(strcmp(info.sdl, '3.4.0'), 'static SDL3 3.4.0 (got %s)', info.sdl);
    check(exist(fullfile(info.module_dir, 'libEGL.dll'), 'file') == 2 && ...
          exist(fullfile(info.module_dir, 'libGLESv2.dll'), 'file') == 2, 'ANGLE beside the core: %s', info.module_dir);
    check(exist(fullfile(here, 'SDL3.dll'), 'file') ~= 2, 'no SDL3.dll beside the MEX files');
    o = ysp_screen('options');
    check(isfield(o, 'windowed') && isfield(o, 'backend') && isfield(o, 'settle') && isfield(o, 'window_thread'), 'options');
    c = ysp_screen('constants');
    check(c.TIER_SIM == 4 && c.PATH_SIMULATED == 4 && c.ABORT_PANIC == 32 && c.PRESS == 1, 'constants');

    % ---- the shared tables (ysp_audio and ysp_timeline beside this directory)
    s = ysp_video('shared');
    [~, ahv] = ysp_audio('version');
    [~, thv] = ysp_timeline('version');
    check(s.abi == 1 && strcmp(s.audio, ahv) && strcmp(s.timeline, thv) && s.channels == 1024, ...
          'shared: the owners'' tables (audio %s, timeline %s)', s.audio, s.timeline);

    % ---- refusals
    expect_error('ysp_screen:arg', @() ysp_screen('open', 'backend', 'nope'));
    expect_error('ysp_screen:arg', @() ysp_screen('open', 'no_such_option', 1));
    expect_error('ysp_screen:arg', @() ysp_screen('open', 'backend', 'sim', 'sim_period', -1));
    expect_error('ysp_screen:arg', @() ysp_screen('open', 'backend', 'sim', 'sim_period', NaN));
    expect_error('ysp_screen:arg', @() ysp_screen('open', 'backend', 'sim', 'size', [800 600 3]));
    expect_error('ysp_screen:open', @() ysp_screen('open', 'backend', 'sim', 'lead', 2));
    expect_error('ysp_screen:handle', @() ysp_screen('begin', uint64(7)));
    expect_error('ysp_screen:usage', @() ysp_screen('nope'));

    % ---- the frame loop
    h = ysp_screen('open', sim{:});
    check(isa(h, 'uint64') && h > bitshift(uint64(1), 32), 'open: a uint64 handle of this module');
    caps = ysp_screen('caps', h);
    check(strcmp(caps.backend, 'sim') && caps.period_ns == int64(4000000), 'caps: sim, period');
    check(~isempty(strfind(ysp_screen('describe', h), 'sim')), 'describe');
    on = zeros(1, 30);
    for i = 1:30
        f = ysp_screen('begin', h);
        on(i) = double(f.onset_ns);
        if i == 1, f1 = f; end
        check(f.index == i - 1 && f.period_ns == int64(4000000) && isa(f.onset_ns, 'int64'), 'frame %d', i - 1);
        ysp_screen('flip', h);
    end
    check(all(diff(on) > 0), 'onsets rise');
    r = ysp_screen('wait_flip', h);
    check(r.index == 29, 'wait_flip: the newest record');
    recs = ysp_screen('records', h);
    check(numel(recs) == 30 && isequal([recs.index], 0:29), 'records: every flip, oldest first');
    check(all([recs.path] == c.PATH_SIMULATED) && all([recs.tier] == c.TIER_SIM), 'records: path and tier');
    check(all(arrayfun(@(x) x.residual_ns == x.onset_ns - x.target_ns && x.onset == double(x.onset_ns) / 1e9, recs)), ...
          'records: seconds and exact ns');
    check(recs(1).target_ns == f1.onset_ns, 'flip targets the predicted onset');
    check(numel(recs(1).phases) == 6 && isnan(recs(1).phases(6)), 'phases: GPU unknown');
    check(isempty(ysp_screen('records', h)), 'records drains');

    f = ysp_screen('begin', h);
    ysp_screen('flip_at', h, f.onset + 3 * f.period);
    r = ysp_screen('wait_flip', h);
    check(r.target_ns == f.onset_ns + int64(3 * 4000000) && ~r.late && abs(r.residual) < 1e-6, 'flip_at: three frames on');
    f = ysp_screen('begin', h);
    ysp_screen('flip_at', h, f.onset - 10 * f.period);
    r = ysp_screen('wait_flip', h);
    check(r.late && bitand(r.flags, c.FLIP_LATE_TARGET) ~= 0, 'flip_at in the past: a late target');
    expect_error('ysp_screen:order', @() ysp_screen('flip', h));
    ysp_screen('begin', h);
    expect_error('ysp_screen:order', @() ysp_screen('begin', h));
    expect_error('ysp_screen:arg', @() ysp_screen('flip_at', h, -1));
    expect_error('ysp_screen:arg', @() ysp_screen('mark', h, 'swap'));
    ysp_screen('mark', h, 'evaluate');
    ysp_screen('flip', h);
    r = ysp_screen('wait_flip', h);
    check(~isnan(r.phases(1)), 'mark: the evaluate phase');

    % ---- aborts
    ysp_screen('request_abort');
    [f, quit] = ysp_screen('begin', h);
    check(isempty(f) && bitand(quit.reasons, c.ABORT_REQUEST) ~= 0 && ~quit.panic && isa(quit.t_ns, 'int64'), ...
          'request_abort: [f, quit] returns it');
    ysp_screen('request_abort');
    expect_error('ysp_screen:abort', @() ysp_screen('begin', h));
    [f, quit] = ysp_screen('begin', h);
    check(~isempty(f) && isempty(quit), 'the next begin starts a frame');
    ysp_screen('flip', h);

    % ---- events and window calls without a window
    e = ysp_screen('poll', h);
    check(isstruct(e) && isempty(e) && isfield(e, 't_ns') && isfield(e, 'ticks'), 'poll: the shared event layout, none');
    expect_error('ysp_screen:not_implemented', @() ysp_screen('title', h, 'block 2'));
    expect_error('ysp_screen:not_implemented', @() ysp_screen('cursor', h, true));
    expect_error('ysp_screen:not_implemented', @() ysp_screen('grab', h, false));
    st = ysp_screen('settle', h);
    check(st.result == c.SETTLE_SKIPPED, 'settle: skipped on sim');
    sy = ysp_screen('sync', h);
    check(~sy.untimed, 'sync');

    % ---- gfx on a screen handle from ysp_screen: one table
    g = ysp_gfx('open', h, 0.5);
    check(isa(g, 'uint64') && g ~= h, 'ysp_gfx opens on ysp_screen''s handle');
    expect_error('ysp_gfx:handle', @() ysp_gfx('begin', h));      % a screen handle is not a gfx
    expect_error('ysp_screen:handle', @() ysp_screen('begin', g));
    expect_error('ysp_gfx:order', @() ysp_gfx('begin', g));       % before the screen's begin
    expect_error('ysp_gfx:refused', @() ysp_gfx('open', h));      % one per screen
    for i = 1:3
        ysp_screen('begin', h);
        ysp_gfx('begin', g);
        ysp_gfx('rect', g, [0 0 100 50], [1 0 0]);
        ysp_gfx('rect', g, [10 10 5 5], 0.25);
        if i == 1, expect_error('ysp_screen:order', @() ysp_screen('flip', h)); end
        ysp_gfx('end', g);
        ysp_screen('flip', h);
    end
    check(true, 'gfx: three frames on the null backend');

    % ---- lifetime: a clear of one MEX file leaves the others' objects
    clear ysp_gfx;
    info = ysp_vis('info');
    check(info.users == 2 && info.screens == 1 && info.gfx == 1, 'clear ysp_gfx: the screen and the gfx stay');
    ysp_screen('begin', h);
    ysp_gfx('begin', g);               % ysp_gfx loads again and finds its handle
    ysp_gfx('end', g);
    ysp_screen('flip', h);
    info = ysp_vis('info');
    check(info.users == 3, 'ysp_gfx attached again');
    clear ysp_screen;
    info = ysp_vis('info');
    check(info.users == 2 && info.screens == 1, 'clear ysp_screen: the screen stays for ysp_gfx');
    expect_error('ysp_gfx:order', @() ysp_gfx('begin', g));
    f = ysp_screen('begin', h);        % ysp_screen loads again: the same handle
    ysp_gfx('begin', g);
    ysp_gfx('end', g);
    ysp_screen('flip', h);
    check(~isempty(f), 'the old screen handle works after ysp_screen loads again');

    % ---- close: the gfx goes with its screen; the rest of the records come back
    rest = ysp_screen('close', h);
    check(isstruct(rest) && numel(rest) >= 1, 'close returns the records not read yet (%d)', numel(rest));
    expect_error('ysp_gfx:handle', @() ysp_gfx('begin', g));
    expect_error('ysp_screen:handle', @() ysp_screen('begin', h));
    info = ysp_vis('info');
    check(info.screens == 0 && info.gfx == 0, 'nothing open');

    % ---- the last clear closes what is still open
    h2 = ysp_screen('open', sim{:});
    g2 = ysp_gfx('open', h2);   %#ok<NASGU>
    clear ysp_screen ysp_gfx ysp_video;
    info = ysp_vis('info');
    check(info.users == 0 && info.screens == 0 && info.gfx == 0, 'the last thin file gone: every object closed');
    expect_error('ysp_screen:handle', @() ysp_screen('begin', h2));
    clear ysp_screen ysp_gfx ysp_video ysp_vis;

    fprintf('PASS test_mex_vis (%d checks)\n', YSP_VIS_CHECKS);
end

function check(cond, msg, varargin)
    global YSP_VIS_CHECKS %#ok<GVMIS>
    YSP_VIS_CHECKS = YSP_VIS_CHECKS + 1;
    if ~cond
        error('test_mex_vis:fail', ['FAIL: ' msg], varargin{:});
    end
end

function expect_error(id, f)
    global YSP_VIS_CHECKS %#ok<GVMIS>
    YSP_VIS_CHECKS = YSP_VIS_CHECKS + 1;
    try
        f();
    catch e
        if ~strcmp(e.identifier, id)
            error('test_mex_vis:fail', 'FAIL: expected %s, got %s: %s', id, e.identifier, e.message);
        end
        return;
    end
    error('test_mex_vis:fail', 'FAIL: expected %s, nothing was raised', id);
end
