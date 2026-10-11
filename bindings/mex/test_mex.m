function test_mex()
%TEST_MEX  Key checks of the ysp_stair, ysp_quest, ysp_aep and ysp_trials MEX
%   functions, and the time units of ysp_parallel (no port needed). Runs in
%   MATLAB and in Octave. Prints PASS, or stops with the error of the first
%   check that failed.
%
%   Build first (run build.m), then from this directory:  test_mex
%
%   Every random draw comes from ysp_trials('splitmix'), so both interpreters
%   see the same stream and the same numbers.

    here = fileparts(mfilename('fullpath'));
    addpath(here);
    global YSP_TEST_STATE %#ok<GVMIS>
    YSP_TEST_STATE = uint64(20260923);

    fprintf('ysp_stair %s, ysp_quest %s, ysp_aep %s, ysp_trials %s\n', ...
        ysp_stair('version'), ysp_quest('version'), ysp_aep('version'), ysp_trials('version'));

    test_stair();
    test_quest();
    test_aep();
    test_trials();
    test_trials_v02();
    test_trials_jitter();
    test_parallel();
    fprintf('PASS\n');
end

% ---------------------------------------------------------------------------

function u = urand()
    global YSP_TEST_STATE %#ok<GVMIS>
    [u, YSP_TEST_STATE] = ysp_trials('splitmix', YSP_TEST_STATE);
end

function check(cond, msg, varargin)
    if ~cond
        error('test_mex:fail', ['FAIL: ' msg], varargin{:});
    end
end

function expect_error(id, f)
    try
        f();
    catch e
        check(strcmp(e.identifier, id), 'expected error %s, got %s (%s)', id, e.identifier, e.message);
        return;
    end
    error('test_mex:fail', 'FAIL: expected error %s, got none', id);
end

function out = failing_model(varargin) %#ok<STOUT>
    error('user:model', 'model failed');
end

function p = weibull_log(x, a, b, g, l)
    p = g + (1 - g - l) .* (1 - exp(-10 .^ (b .* (x - a))));
end

% ---------------------------------------------------------------------------

function test_stair()
    % A hand-derived 1-up-2-down track: correct, correct steps down; an
    % incorrect steps up and reverses.
    d = struct('start', 10, 'n_down', 2, 'steps', 1, 'stop_trials', 6);
    h = ysp_stair('open', d);
    resp = [1 1 0 1 1 1];
    want = [10 10 9 10 10 9];
    for t = 1:6
        x = ysp_stair('next', h);
        check(x == want(t), 'stair proposal %d: %g, want %g', t, x, want(t));
        ysp_stair('update', h, x, resp(t));
    end
    check(ysp_stair('done', h), 'stair not done after stop_trials');
    check(strcmp(ysp_stair('stop_reason', h), 'trials'), 'stair stop reason');
    check(ysp_stair('n_reversals', h) == 2, 'stair reversal count %d', ysp_stair('n_reversals', h));
    hist = ysp_stair('history', h);
    check(isequal(hist.proposed.', want) && isequal(hist.response.', resp), 'stair history');
    check(isequal(ysp_stair('reversal_trial', h).', [3 5]), 'stair reversal trials');
    check(abs(ysp_stair('estimate', h, 'reversals') - 9.5) < 1e-12, 'stair estimate');
    check(abs(ysp_stair('convergence_p', 1, 2) - sqrt(0.5)) < 1e-12, 'convergence_p');
    check(abs(ysp_stair('weighted_scale', 0.75) - 1/3) < 1e-12, 'weighted_scale');
    expect_error('ysp_stair:arg', @() ysp_stair('update', h, 5, 2));
    ysp_stair('close', h);
    expect_error('ysp_stair:handle', @() ysp_stair('next', h));
    expect_error('ysp_stair:open', @() ysp_stair('open', struct('start', 1, 'steps', 1)));
    expect_error('ysp_stair:arg', @() ysp_stair('open', struct('start', 1, 'steps', 1, 'stop_trial', 3)));
    expect_error('ysp_stair:usage', @() ysp_stair('nosuchcommand'));

    % A 1-up-3-down log staircase against a simulated observer converges.
    d = struct('start', 0.5, 'n_down', 3, 'step_type', 'log', 'steps', [0.3 0.15 0.075], ...
               'min', 0.001, 'max', 1, 'stop_reversals', 14);
    h = ysp_stair('open', d);
    while ~ysp_stair('done', h)
        x = ysp_stair('next', h);
        ysp_stair('update', h, x, ysp_stair('simulate_response', weibull_log(log10(x), -1.5, 3.5, 0.5, 0.02), urand()));
    end
    thr = log10(ysp_stair('estimate', h));
    check(abs(thr + 1.45) < 0.3, 'stair 1-up-3-down estimate %.3f far from the 79%% point', thr);
    ysp_stair('close', h);
    fprintf('ysp_stair: ok\n');
end

% ---------------------------------------------------------------------------

function d = psi_desc()
    d = struct();
    d.stim = {{-3, 0, 31}};
    d.param = {{-3, 0, 61}, {0.5, 6, 12}, 0.5, 0.02};
    d.stop_trials = 100;
end

function test_quest()
    truth = [-1.5 3.5 0.5 0.02];

    % A Psi run recovers its threshold.
    h = ysp_quest('open', psi_desc());
    while ~ysp_quest('done', h)
        [i, x] = ysp_quest('next', h);
        k = ysp_quest('simulate', h, i, truth, urand());
        check(abs(x - ysp_quest('stim_value', h, i)) < 1e-15, 'next second output');
        ysp_quest('update', h, i, k);
    end
    est = ysp_quest('estimate', h, 'mean');
    check(abs(est(1) - truth(1)) < 0.2, 'Psi threshold %.3f, truth %.3f', est(1), truth(1));
    post = ysp_quest('posterior', h);
    check(isequal(size(post), [61 12]), 'posterior shape');
    check(abs(sum(post(:)) - 1) < 1e-9, 'posterior mass');
    m = ysp_quest('marginal', h, 1);
    check(max(abs(m - sum(post, 2))) < 1e-12, 'marginal against the posterior');
    hs = ysp_quest('history', h);
    check(numel(hs.outcome) == 100 && all(hs.stim_index >= 1), 'quest history');
    ysp_quest('close', h);

    % The same model through pf_batch fills the same table.
    d = psi_desc();
    d.stop_trials = 20;
    d2 = d;
    d2.pf_batch = @(s, P) [1 - weibull_log(s(1), P(:,1), P(:,2), P(:,3), P(:,4)), ...
                               weibull_log(s(1), P(:,1), P(:,2), P(:,3), P(:,4))];
    h1 = ysp_quest('open', d);
    h2 = ysp_quest('open', d2);
    for t = 1:20
        i1 = ysp_quest('next', h1);
        i2 = ysp_quest('next', h2);
        check(i1 == i2, 'pf_batch selection differs at trial %d', t);
        k = ysp_quest('simulate', h1, i1, truth, urand());
        ysp_quest('update', h1, i1, k);
        ysp_quest('update', h2, i2, k);
    end
    dp = max(abs(ysp_quest('posterior', h1) - ysp_quest('posterior', h2)));
    check(max(dp(:)) < 1e-6, 'pf_batch posterior differs by %g', max(dp(:)));
    ysp_quest('close', h2);

    % Save between an update and the next selection, resume, and continue:
    % the two sessions agree bit for bit.
    b = ysp_quest('save', h1);
    check(isa(b, 'uint8'), 'save returns uint8');
    h3 = ysp_quest('load', b, d);
    for t = 1:10
        i1 = ysp_quest('next', h1);
        i3 = ysp_quest('next', h3);
        check(i1 == i3, 'resumed selection differs at trial %d', t);
        k = ysp_quest('simulate', h1, i1, truth, urand());
        ysp_quest('update', h1, i1, k);
        ysp_quest('update', h3, i3, k);
    end
    check(isequal(ysp_quest('posterior', h1), ysp_quest('posterior', h3)), 'resumed posterior differs');
    ysp_quest('close', h3);

    % Async: the same responses give the same posterior as the plain loop.
    ha = ysp_quest('open', d);
    hs = ysp_quest('open', d);
    ysp_quest('async_start', ha, struct('queue_depth', 1));
    snap = ysp_quest('async_poll', ha);
    check(snap.seq == 0, 'first snapshot seq');
    for t = 1:8
        is = ysp_quest('next', hs);
        check(snap.proposed == is, 'async proposal differs at trial %d', t);
        k = ysp_quest('simulate', hs, is, truth, urand());
        ysp_quest('update', hs, is, k);
        seq = ysp_quest('async_submit', ha, snap.proposed, k);
        snap = ysp_quest('async_wait', ha, seq, 5);
        check(snap.seq >= seq && snap.n_trials == t, 'async wait');
    end
    expect_error('ysp_quest:busy', @() ysp_quest('next', ha));
    ysp_quest('async_stop', ha);
    check(isequal(ysp_quest('posterior', ha), ysp_quest('posterior', hs)), 'async posterior differs');
    ysp_quest('close', hs);

    % A MATLAB rng refuses the async layer; an integer seed does not.
    d3 = d; d3.rng = @() rand(); d3.subset_size = 8;
    hr = ysp_quest('open', d3);
    expect_error('ysp_quest:arg', @() ysp_quest('async_start', hr));
    ysp_quest('close', hr);
    d3.rng = 7;
    hr = ysp_quest('open', d3);
    ysp_quest('async_start', hr);
    ysp_quest('async_stop', hr);
    ysp_quest('close', hr);

    % Error ids.
    expect_error('ysp_quest:arg', @() ysp_quest('update', ha, 0, 1));
    expect_error('ysp_quest:arg', @() ysp_quest('update', ha, 1, 5));
    d4 = d; d4.pf_batch = @failing_model;
    expect_error('user:model', @() ysp_quest('open', d4));
    d4.pf_batch = @(s, P) ones(3, 3);
    expect_error('ysp_quest:callback', @() ysp_quest('open', d4));
    expect_error('ysp_quest:open', @() ysp_quest('open', rmfield(d, 'stop_trials')));
    expect_error('ysp_quest:load', @() ysp_quest('load', uint8([1 2 3]), d));
    ysp_quest('close', ha);
    ysp_quest('close', h1);
    expect_error('ysp_quest:handle', @() ysp_quest('next', h1));
    fprintf('ysp_quest: ok\n');
end

% ---------------------------------------------------------------------------

function test_aep()
    % 1-D Bernoulli detection, threshold at -1.5 log units for p = 0.75
    % with guess 0.5: truth p(x) = 0.5 + 0.5 * Phi((x + 1.5) / 0.3).
    ptrue = @(x) 0.5 + 0.5 * 0.5 * erfc(-((x + 1.5) / 0.3) / sqrt(2));
    d = struct('lo', -3, 'hi', 0, 'guess', 0.5, 'target_p', 0.75, 'n_init', 6, ...
               'n_candidates', 61, 'fit', true, 'fit_every', 10, 'stop_trials', 60);
    h = ysp_aep('open', d);
    for t = 1:40
        [x, idx] = ysp_aep('next', h); %#ok<ASGLU>
        ysp_aep('update', h, x, double(urand() < ptrue(x)));
    end
    b = ysp_aep('save', h);
    h2 = ysp_aep('load', b, d);
    while ~ysp_aep('done', h)
        x = ysp_aep('next', h);
        x2 = ysp_aep('next', h2);
        check(isequal(x, x2), 'resumed GP proposal differs');
        y = double(urand() < ptrue(x));
        ysp_aep('update', h, x, y);
        ysp_aep('update', h2, x2, y);
    end
    [thr, lo, hi] = ysp_aep('threshold', h);
    check(abs(thr + 1.5) < 0.35, 'GP threshold %.3f, truth -1.5', thr);
    check(lo <= thr && thr <= hi, 'GP band does not contain the threshold');
    [thr2] = ysp_aep('threshold', h2);
    check(thr == thr2, 'resumed GP threshold differs');
    p = ysp_aep('predict_p_many', h, [-3; -1.5; 0]);
    check(numel(p) == 3 && p(1) < p(3), 'predict_p_many');
    hy = ysp_aep('get_hyper', h);
    check(isfield(hy, 'lengthscale') && hy.lengthscale > 0, 'get_hyper');
    hist = ysp_aep('history', h);
    check(size(hist.x, 1) == 60 && islogical(hist.init), 'GP history');
    expect_error('ysp_aep:arg', @() ysp_aep('update', h, [0 0], 1));
    expect_error('ysp_aep:arg', @() ysp_aep('open', struct('lo', 0, 'hi', 1, 'stop_trials', 5, 'lik', 'nosuch')));
    ysp_aep('close', h);
    ysp_aep('close', h2);

    % Async with fit_in_idle off equals the plain loop.
    d.fit = false; d.stop_trials = 12;
    ha = ysp_aep('open', d);
    hs = ysp_aep('open', d);
    ysp_aep('async_start', ha);
    snap = ysp_aep('async_poll', ha);
    for t = 1:10
        xs = ysp_aep('next', hs);
        check(isequal(xs, snap.x), 'GP async proposal differs at trial %d', t);
        y = double(urand() < ptrue(xs));
        ysp_aep('update', hs, xs, y);
        seq = ysp_aep('async_submit', ha, snap.x, y);
        snap = ysp_aep('async_wait', ha, seq, 10);
    end
    ysp_aep('async_stop', ha);
    check(ysp_aep('log_marginal', ha) == ysp_aep('log_marginal', hs), 'GP async posterior differs');
    ysp_aep('close', ha);
    ysp_aep('close', hs);
    fprintf('ysp_aep: ok\n');
end

% ---------------------------------------------------------------------------

function test_trials()
    % Method of constant stimuli: 2 x 3 factorial, constrained order.
    d = struct('reps', 6, 'order', 'constrained', 'rng', uint64(42));
    d.factors = {{'ori', 2}, {'con', 3}};
    d.constraints = ysp_trials('max_run', 'ori', 'any', 2);
    h = ysp_trials('open', d);
    check(ysp_trials('n_conditions', h) == 6, 'factorial size');
    check(ysp_trials('condition_from_levels', h, [2 3]) == 6, 'condition_from_levels');
    check(isequal(ysp_trials('levels', h, 4), [2 1]), 'levels');
    for t = 1:15
        ti = ysp_trials('next', h);
        ysp_trials('update', h, double(urand() < 0.7));
    end
    % Save mid-session, resume, and finish both.
    b = ysp_trials('save', h);
    d2 = d; d2.rng = ysp_trials('rng_state', h);
    h2 = ysp_trials('load', b, d2);
    while true
        ti = ysp_trials('next', h);
        ti2 = ysp_trials('next', h2);
        check(isequal(ti, ti2), 'resumed trial differs');
        if isempty(ti), break; end
        y = double(urand() < 0.7);
        ysp_trials('update', h, y);
        ysp_trials('update', h2, y);
    end
    H = ysp_trials('history', h);
    check(isequal(H, ysp_trials('history', h2)), 'resumed history differs');
    ori = zeros(numel(H.condition), 1);
    for t = 1:numel(ori), ori(t) = ysp_trials('level', h, H.condition(t), 1); end
    run = 1;
    for t = 2:numel(ori)
        if ori(t) == ori(t-1), run = run + 1; else, run = 1; end
        check(run <= 2, 'max_run broken at trial %d', t);
    end
    check(numel(H.condition) == 36 && all(H.done), 'MOCS trial count');
    hdr = ysp_trials('format_header', h);
    check(ischar(hdr) && hdr(end) == sprintf('\n'), 'format_header');
    expect_error('ysp_trials:order', @() ysp_trials('update', h, 1));
    ysp_trials('close', h);
    ysp_trials('close', h2);

    % Three staircases and a catch condition, never two catch trials in a
    % row; the level of each staircase trial is the 8-byte record.
    stairs = cell(1, 3);
    for k = 1:3
        stairs{k} = ysp_stair('open', struct('start', 0.3, 'n_down', k + 1, 'step_type', 'log', ...
            'steps', [0.2 0.1], 'min', 0.001, 'max', 1, 'stop_reversals', 6));
    end
    d = struct('n_conditions', 1, 'reps', 10, 'order', 'constrained', ...
               'track_rate', 0.9, 'rng', 7, 'record_size', 8);
    d.constraints = ysp_trials('min_gap', 'condition', 1, 1);
    d.tracks = {{'stair', stairs{1}}, {'stair', stairs{2}}, {'stair', stairs{3}}};
    h = ysp_trials('open', d);
    levels = [];
    while true
        ti = ysp_trials('next', h);
        if isempty(ti), break; end
        if ti.track > 0
            s = stairs{ti.track};
            x = ysp_stair('next', s);
            r = ysp_stair('simulate_response', weibull_log(log10(x), -1.5, 3.5, 0.5, 0.02), urand());
            ysp_stair('update', s, x, r);
            ysp_trials('update', h, r, x);
            levels(end+1) = x; %#ok<AGROW>
        else
            ysp_trials('update', h, double(urand() < 0.5));
        end
    end
    check(ysp_trials('done', h), 'trials not done');
    for k = 1:3, check(ysp_stair('done', stairs{k}), 'staircase %d not done', k); end
    H = ysp_trials('history', h);
    catchs = find(H.condition == 1);
    check(numel(catchs) == 10, 'catch trial count %d', numel(catchs));
    check(all(diff(catchs) > 1) || any(H.violation), 'two catch trials in a row');
    tr = find(H.track > 0);
    check(numel(tr) == numel(levels), 'track trial count');
    for j = 1:numel(tr)
        check(typecast(ysp_trials('record', h, tr(j)), 'double') == levels(j), 'record %d', j);
    end
    check(all(ysp_trials('record', h, catchs(1)) == 0), 'a catch trial record is zeroed');
    % A closed staircase handle surfaces as that module's error, from next.
    d.tracks = {{'stair', stairs{1}}};
    d.n_conditions = 1; d.track_rate = 1; d.constraints = [];
    ysp_stair('close', stairs{1});
    h3 = ysp_trials('open', d);
    expect_error('ysp_stair:handle', @() ysp_trials('next', h3));
    ysp_trials('close', h3);
    for k = 2:3, ysp_stair('close', stairs{k}); end
    expect_error('ysp_trials:open', @() ysp_trials('open', struct('n_conditions', 2)));
    expect_error('ysp_trials:arg', @() ysp_trials('open', struct('n_conditions', 2, 'reps', 1, 'nosuch', 1)));
    ysp_trials('close', h);
    fprintf('ysp_trials: ok\n');
end

% ---------------------------------------------------------------------------
% ysp_trials v0.2: tables, names, rules, units, balance, groups, Latin squares.

function test_trials_v02()
    csv = sprintf(['target,contrast,word\n' ...
                   'a,0.25,cat\na,0.5,dog\nb,0.25,"new york"\nb,0.5,ox\ncatch,0,emu\ncatch,0,yak\n']);
    info = ysp_trials('table', csv);
    check(isequal(info.columns, {'target', 'contrast', 'word'}), 'table columns');
    check(isequal(info.types, {'string', 'number', 'string'}), 'table types');
    check(info.n_rows == 6 && strcmp(info.values{3, 3}, 'new york'), 'table values');
    check(isequal(info.levels{1}, {'a', 'b', 'catch'}), 'table levels');
    expect_error('ysp_trials:table', @() ysp_trials('table', struct('csv', sprintf('x,y\n1,a\n"0,5",b\n'), 'types', struct('x', 'number'))));
    % The same design by names and by rules gives one schedule.
    d = struct('table', csv, 'reps', 6, 'order', 'constrained', 'rng', 99, 'block_size', 12);
    d.constraints = [ysp_trials('max_run', 'target', 'any', 2), ...
                     ysp_trials('max_in_window', 'target', 'catch', 2, 1), ...
                     ysp_trials('first_not', 'target', 'catch')];
    h = ysp_trials('open', d);
    r = struct('table', csv, 'rng', 99, 'rules', sprintf(['order constrained\nreps 6\n' ...
               'max_run target 2\nmax_in_window target=catch 2 1\nfirst_not target=catch\nblock_size 12\n']));
    h2 = ysp_trials('open', r);
    check(isequal(ysp_trials('schedule', h), ysp_trials('schedule', h2)), 'rules and names give different schedules');
    r.rules = ysp_trials('format_rules', h2);
    h3 = ysp_trials('open', r);
    check(isequal(ysp_trials('schedule', h3), ysp_trials('schedule', h2)), 'format_rules does not paste back');
    row = ysp_trials('values', h, 3);
    check(strcmp(row.word, 'new york') && row.contrast == 0.25, 'values');
    ysp_trials('close', h); ysp_trials('close', h2); ysp_trials('close', h3);
    expect_error('ysp_trials:rules', @() ysp_trials('open', struct('table', csv, 'rules', 'maxrep target 2')));
    % Units: every prime directly followed by a target.
    u = sprintf('kind\nprime\ntarget\nfill\nfill\n');
    h = ysp_trials('open', struct('table', u, 'reps', 5, 'order', 'constrained', 'rng', 4, ...
                                  'constraints', ysp_trials('followed_by', 'kind', 'prime', 'target')));
    s = ysp_trials('schedule', h);
    for k = 1:numel(s)
        if s(k) == 1, check(k < numel(s) && s(k + 1) == 2, 'a prime not followed by a target'); end
    end
    ysp_trials('close', h);
    % Balance: every ordered pair twice, a flagged lead-in.
    h = ysp_trials('open', struct('table', sprintf('lvl\nA\nB\nC\n'), 'reps', 6, 'order', 'constrained', ...
                                  'rng', 12, 'constraints', ysp_trials('balance', 'lvl')));
    s = ysp_trials('schedule', h);
    check(numel(s) == 19, 'balance length');
    cnt = zeros(3);
    for k = 2:numel(s), cnt(s(k - 1), s(k)) = cnt(s(k - 1), s(k)) + 1; end
    check(all(cnt(:) == 2), 'balance pair counts');
    while ~isempty(ysp_trials('next', h)), ysp_trials('update', h, 1); end
    H = ysp_trials('history', h);
    check(H.leadin(1) && ~any(H.leadin(2:end)), 'lead-in flag');
    ysp_trials('close', h);
    % Groups in Williams order; Latin squares.
    g = sprintf('block,item\nA,1\nA,2\nB,1\nB,2\nC,1\nC,2\nD,1\nD,2\n');
    h = ysp_trials('open', struct('table', g, 'reps', 1, 'order', 'full_random', 'rng', 2, ...
        'groups', struct('factor', 'block', 'mode', 'blocked', 'order', 'balanced_latin', 'participant', 1)));
    s = ysp_trials('schedule', h);
    want = ysp_trials('latin', 4, 1, true);
    blocks = ceil(s / 2);
    check(isequal(blocks(1:2:end).', want), 'groups in Williams order');
    ysp_trials('close', h);
    [w, rows] = ysp_trials('latin', 5, 7, true);
    check(rows == 10 && isequal(sort(w), 1:5), 'latin rows and permutation');
    h = ysp_trials('open', struct('n_conditions', 4, 'order', 'list', 'order_list', [4 1 1 3]));
    check(isequal(ysp_trials('schedule', h).', [4 1 1 3]), 'order list');
    ysp_trials('close', h);
    fprintf('ysp_trials v0.2: ok\n');
end

% ---------------------------------------------------------------------------

function test_trials_jitter()
    % Jitter (v0.2.1): drawn per trial, logged, replayed by a fresh open.
    d = struct('n_conditions', 3, 'reps', 4, 'order', 'full_random', 'rng', 31);
    d.jitters = [ysp_trials('uniform', 'iti', 0.8, 1.2, [60000 1001]), ...
                 ysp_trials('exponential', 'fp', 0.5, 2.0, 0.4), ...
                 ysp_trials('choice', 'soa', [0.1 0.2 0.4])];
    h = ysp_trials('open', d);
    check(isequal(ysp_trials('jitter_names', h), {'iti', 'fp', 'soa'}), 'jitter names');
    k = 0;
    ti = ysp_trials('next', h);
    while ~isempty(ti)
        k = k + 1;
        v = ysp_trials('jitter', h, ti.index, 'iti');
        check(v.frames >= 48 && v.frames <= 71, 'iti frames');
        check(v.ns == int64(round(double(v.frames) * 1001e9 / 60000)), 'iti ns');
        v = ysp_trials('jitter', h, ti.index, 2);
        check(v.s >= 0.5 && v.s <= 2.0 && v.frames == -1, 'fp bounds');
        ysp_trials('update', h, 1);
        ti = ysp_trials('next', h);
    end
    check(k == 12, 'jitter session length');
    hdr = ysp_trials('format_header', h);
    check(~isempty(strfind(hdr, ',iti,fp,soa')), 'jitter columns in the header');
    h2 = ysp_trials('open', d);
    ysp_trials('restore', h2, ones(1, 12));
    for i = 1:12
        check(strcmp(ysp_trials('format_row', h, i), ysp_trials('format_row', h2, i)), 'jitter replay');
    end
    ysp_trials('close', h); ysp_trials('close', h2);
    v = ysp_trials('jitter_map', ysp_trials('uniform', 'x', 0.8, 1.2, [60000 1001]), 0);
    check(v.frames == 48 && v.ns == int64(800800000), 'jitter_map');
    expect_error('ysp_trials:arg', @() ysp_trials('jitter_map', ysp_trials('uniform', 'x', 1.2, 0.8), 0.5));
    expect_error('ysp_trials:open', @() ysp_trials('open', struct('n_conditions', 1, 'reps', 1, ...
        'jitters', ysp_trials('uniform', 'x', 0.801, 0.81, 60))));
    fprintf('ysp_trials v0.2.1 jitter: ok\n');
end

% ---------------------------------------------------------------------------

function test_parallel()
    % Times are seconds. No port is needed: the times in the open struct are
    % checked before the open, and without a port the open fails with
    % ysp_parallel:open. With a port, the pulse widths are checked too.
    if exist('ysp_parallel', 'file') ~= 3
        fprintf('ysp_parallel: not built on this platform, skipped\n');
        return;
    end
    t0 = ysp_parallel('now');
    pause(0.05);
    t1 = ysp_parallel('now');
    check(t1 - t0 > 0.04 && t1 - t0 < 1, 'ysp_parallel(''now'') is not in seconds: %g', t1 - t0);
    expect_error('ysp_parallel:usage', @() ysp_parallel('now_us'));
    expect_error('ysp_parallel:arg', @() ysp_parallel('open', struct('runtime', -1e-3)));
    expect_error('ysp_parallel:arg', @() ysp_parallel('open', struct('deadline', Inf)));
    expect_error('ysp_parallel:arg', @() ysp_parallel('open', struct('period', NaN)));
    expect_error('ysp_parallel:arg', @() ysp_parallel('open', struct('period', 1e10)));
    expect_error('ysp_parallel:arg', @() ysp_parallel('open', struct('runtime_ns', 5e5)));
    try
        h = ysp_parallel('open', struct('runtime', 5e-4, 'deadline', 2e-3, 'period', 2e-3));
    catch e
        check(strcmp(e.identifier, 'ysp_parallel:open'), ...
            'expected ysp_parallel:open without a port, got %s (%s)', e.identifier, e.message);
        fprintf('ysp_parallel seconds: ok (no port)\n');
        return;
    end
    closer = onCleanup(@() ysp_parallel('close', h));   %#ok<NASGU>
    s = ysp_parallel('sched', h);
    check(s.runtime > 0 && s.runtime <= s.deadline && s.deadline <= s.period && s.period < 1, ...
        'sched is not in seconds: %g %g %g', s.runtime, s.deadline, s.period);
    expect_error('ysp_parallel:arg', @() ysp_parallel('pulse', h, 0, -1e-3));
    expect_error('ysp_parallel:arg', @() ysp_parallel('pulse', h, 0, Inf));
    expect_error('ysp_parallel:arg', @() ysp_parallel('pulseasync', h, 0, NaN));
    ysp_parallel('pulse', h, 0, 1e-4);
    fprintf('ysp_parallel seconds: ok (port open)\n');
end
