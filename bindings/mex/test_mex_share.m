function test_mex_share()
%TEST_MEX_SHARE  The tables of ysp_audio and ysp_timeline, seen from another
%   MEX file. Builds tests/share_consumer.c into this directory (a consumer
%   made the way the ysp-vis MEX files will be: the headers' declarations
%   and the shims of bindings/share/), then checks that a handle opened by
%   the owner's own command is the object the consumer reaches, that a table
%   the consumer cannot use is refused, and the lifetime rules. Runs in
%   MATLAB and in Octave; build ysp_audio and ysp_timeline first (build.m).
%   Prints PASS with the number of checks.

    global YSP_SH_CHECKS %#ok<GVMIS>
    YSP_SH_CHECKS = 0;
    here = fileparts(mfilename('fullpath'));
    addpath(here);
    old = cd(here);
    restore = onCleanup(@() cd(old));   %#ok<NASGU>
    clear share_consumer;
    args = {['-I' fullfile(here, '..', '..', 'include')], ['-I' fullfile(here, '..', 'share')], ...
            fullfile(here, 'tests', 'share_consumer.c')};
    if ~ispc
        args{end+1} = '-ldl';
    end
    mex(args{:});
    SIM = 4;

    % ---- the tables
    info = share_consumer('import');
    [~, ahv] = ysp_audio('version');
    [~, thv] = ysp_timeline('version');
    check(info.abi == 1 && strcmp(info.audio, ahv) && strcmp(info.timeline, thv) && info.channels == 1024, ...
          'import: both tables, the owners'' header versions');
    cases = {'a_abi', 'ABI'; 'a_version', '0.0.1'; 'a_minor', 'major.minor versions must match'; ...
             'a_sizeof', 'sizeof(yau_audio)'; 'a_host', 'host'; ...
             't_channels', 'YTL_MAX_CHANNELS'; 't_minor', 'major.minor versions must match'};
    for i = 1:size(cases, 1)
        [rc, err] = share_consumer('check', cases{i, 1});
        check(rc == -1 && ~isempty(strfind(err, cases{i, 2})), 'refuses %s: %s', cases{i, 1}, err);
    end
    [rc, err] = share_consumer('check', 'a_none');
    check(rc == 0 && isempty(err), 'accepts the real table');
    for what = {'a_patch', 't_patch'}   % a patch changes no layout and no function a table carries
        [rc, err] = share_consumer('check', what{1});
        check(rc == 0 && isempty(err), 'accepts another patch (%s): %s', what{1}, err);
    end

    % ---- an audio handle opened by ysp_audio, driven from the consumer
    h = ysp_audio('open', struct('backend', 'null'));
    k = share_consumer('hold_audio', h);
    check(ysp_audio('pins', h) == 1, 'the owner sees the pin: one module state');
    ci = share_consumer('info', k);
    check(ci.open && ci.rate == 48000 && ~isempty(strfind(ci.describe, 'backend=Null')), 'the consumer reads the same device');
    t = ysp_audio('now') + 0.1;
    id = share_consumer('play_at', k, t);
    r = share_consumer('wait', k, id);
    ro = ysp_audio('result', h, id);
    check(r.rc == 0 && r.tier == SIM && r.buffer_id == 77 && abs(ro.onset - r.onset) < 1e-9, 'a play from the consumer is in the owner''s records');
    expect_error('ysp_audio:busy', @() ysp_audio('close', h));
    th = ysp_timeline('open', struct('channels', 2));
    expect_error('share_consumer:refused', @() share_consumer('hold_audio', th));
    share_consumer('drop_audio', k);
    check(ysp_audio('pins', h) == 0, 'release drops the pin');
    ysp_audio('close', h);
    expect_error('share_consumer:refused', @() share_consumer('hold_audio', h));

    % ---- clear ysp_audio while pinned: the device stays for the consumer
    h = ysp_audio('open', struct('backend', 'null'));
    k = share_consumer('hold_audio', h);
    clear ysp_audio;
    expect_error('ysp_audio:handle', @() ysp_audio('caps', h));   % the owner's table is empty
    id = share_consumer('play_at', k, 0);
    r = share_consumer('wait', k, id);
    check(r.rc == 0, 'the pinned device still plays after clear ysp_audio');
    share_consumer('drop_audio', k);                               % the last release closes it

    % ---- a timeline built by ysp_timeline, evaluated by the consumer
    q = ysp_timeline('seq', th, 1);
    ysp_timeline('on', q, 1); ysp_timeline('wait', q, 0.5); ysp_timeline('off', q, 1); ysp_timeline('on', q, 2);
    k = share_consumer('hold_timeline', th);
    check(ysp_timeline('pins', th) == 1, 'the timeline is pinned');
    T0 = 10; P = 1 / 60;
    check(share_consumer('anchor', k, 1, T0, 0) == 0, 'anchor through the shim');
    [n, v] = share_consumer('evaluate', k, T0, P, 0, 2);
    check(n == 1 && isequal(v, [1 0]), 'frame 0 through the shim');
    [n, v] = share_consumer('evaluate', k, T0 + 30 * P, P, 30, 2);
    check(n == 2 && isequal(v, [0 1]), 'frame 30 through the shim');
    vv = ysp_timeline('values', th);
    check(isequal(double(vv(:)'), [0 1]), 'the owner sees the values');
    expect_error('ysp_timeline:busy', @() ysp_timeline('close', th));
    b = share_consumer('bench', k, 2e6, 5);
    fprintf('ytl_value: shim %.2f ns, table %.2f ns, inline read %.2f ns\n', b.shim, b.table, b.inline);
    share_consumer('drop_timeline', k);
    ysp_timeline('close', th);

    clear share_consumer;                    % its exit releases and unloads
    fprintf('PASS (%d checks)\n', YSP_SH_CHECKS);
end

function check(cond, msg, varargin)
    global YSP_SH_CHECKS %#ok<GVMIS>
    YSP_SH_CHECKS = YSP_SH_CHECKS + 1;
    if ~cond
        error('test_mex_share:fail', ['FAIL: ' msg], varargin{:});
    end
end

function expect_error(id, f)
    global YSP_SH_CHECKS %#ok<GVMIS>
    YSP_SH_CHECKS = YSP_SH_CHECKS + 1;
    try
        f();
    catch e
        if ~strcmp(e.identifier, id)
            error('test_mex_share:fail', 'FAIL: expected %s, got %s: %s', id, e.identifier, e.message);
        end
        return;
    end
    error('test_mex_share:fail', 'FAIL: expected %s, nothing was raised', id);
end
