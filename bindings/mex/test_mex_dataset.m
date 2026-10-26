function test_mex_dataset()
%TEST_MEX_DATASET  Key checks of the ysp_dataset MEX function. Runs in MATLAB
%   and in Octave. Prints PASS with the number of checks, or stops with the
%   error of the first check that failed. Build first (run build.m), then
%   from this directory: test_mex_dataset. tests/adapt/dataset_test.c
%   checks the header itself.

    global YSP_DS_CHECKS %#ok<GVMIS>
    YSP_DS_CHECKS = 0;
    here = fileparts(mfilename('fullpath'));
    addpath(here);
    fprintf('ysp_dataset %s\n', ysp_dataset('version'));
    root = tempname();
    mkdir(root);
    cleanup = onCleanup(@() rmdir(root, 's')); %#ok<NASGU>

    cols = {'participant', 'const', 'P07'; 'trial_index', 'int', []; 'response', 'string', []; ...
            'correct', 'bool', []; 'rt_s', 'time', []; 'contrast', 'double', []; 'big', 'int64', []};

    % ---- the folder and session.json at the open
    d = fullfile(root, 'P07');
    h = ysp_dataset('open', struct('dir', d, 'trial_cols', {cols}, ...
                    'session', struct('experiment', 'gabor', 'seed', 20261021)));
    s = session(d);
    check(isequal(s.closed, false) && strcmp(s.format, 'ysp-session') && s.version == 1, 'session.json at the open');
    check(strcmp(s.experiment, 'gabor') && s.seed == 20261021 && ~isfield(s, 'rows'), 'the caller''s keys');
    check(~isempty(regexp(s.start_utc, '^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$', 'once')), 'start_utc');
    check(exist(fullfile(d, 'streams'), 'dir') == 7, 'streams folder');
    info = ysp_dataset('info', h);
    check(info.is_open && ~info.finished && isequal(info.trial_cols, cols(:, 1)'), 'info');
    check(isequal(lines(fullfile(d, 'trials.csv')), {'participant,trial_index,response,correct,rt_s,contrast,big'}), ...
          'trials.csv header');
    fc = ysp_dataset('columns', 'frames');
    check(numel(fc) == 17 && strcmp(fc{5}, 'onset_s'), 'frame columns');
    check(isequal(lines(fullfile(d, 'frames.csv')), {strjoin(fc, ',')}), 'frames.csv header');
    check(numel(ysp_dataset('columns', 'events')) == 20 && numel(ysp_dataset('columns', 'clock')) == 11, 'columns');

    % ---- trial rows: a struct or name-value pairs; [] and NaN are missing
    ysp_dataset('write_trial', h, struct('trial_index', 0, 'response', sprintf('left, "far"\nline'), ...
                'correct', true, 'rt_s', 0.5123456789, 'contrast', 0.25, 'big', int64(2)^62));
    ysp_dataset('write_trial', h, 'trial_index', 1, 'correct', false, 'rt_s', -0.0125, 'contrast', 1e-7);
    ysp_dataset('write_trial', h, 'trial_index', 2, 'response', '', 'correct', [], 'rt_s', NaN, 'contrast', NaN);
    if exist('OCTAVE_VERSION', 'builtin')
        text = ['r' char([195 169]) ' ' char([228 184 173])];   % Octave's char holds UTF-8 bytes
    else
        text = ['r' char(233) ' ' char(20013)];                 % MATLAB's char is UTF-16
    end
    ysp_dataset('write_trial', h, 'trial_index', int32(3), 'rt_s', 1.184516667, 'response', text);
    ysp_dataset('write_trial', h);
    raw = fileread_utf8(fullfile(d, 'trials.csv'));
    nl = char(10);
    want = ['participant,trial_index,response,correct,rt_s,contrast,big' nl ...
            'P07,0,"left, ""far""' nl 'line",1,0.512345679,0.25,4611686018427387904' nl ...
            'P07,1,,0,-0.012500000,1e-07,' nl 'P07,2,,,,,' nl ...
            'P07,3,r' char([195 169]) ' ' char([228 184 173]) ',,1.184516667,,' nl 'P07,,,,,,' nl];
    check(isequal(raw, want), 'trial rows, text as UTF-8 bytes');
    r = ysp_dataset('rows', h);
    check(r.trials == 5 && r.frames == 0, 'rows');

    % ---- refusals: a refused row writes nothing
    expect_error('ysp_dataset:arg', @() ysp_dataset('write_trial', h, 'nope', 1));
    expect_error('ysp_dataset:arg', @() ysp_dataset('write_trial', h, 'participant', 'P08'));
    expect_error('ysp_dataset:arg', @() ysp_dataset('write_trial', h, 'trial_index', 2.5));
    expect_error('ysp_dataset:arg', @() ysp_dataset('write_trial', h, 'trial_index', 2^31));
    expect_error('ysp_dataset:arg', @() ysp_dataset('write_trial', h, 'correct', 2));
    expect_error('ysp_dataset:arg', @() ysp_dataset('write_trial', h, 'rt_s', Inf));
    expect_error('ysp_dataset:arg', @() ysp_dataset('write_trial', h, 'rt_s', '0.5'));
    expect_error('ysp_dataset:arg', @() ysp_dataset('write_trial', h, 'response', 3));
    expect_error('ysp_dataset:arg', @() ysp_dataset('write_trial', h, 'trial_index', 1, 'big', 1.5));
    expect_error('ysp_dataset:usage', @() ysp_dataset('write_trial', h, 'trial_index'));
    r = ysp_dataset('rows', h);
    check(r.trials == 5, 'refused rows write nothing');

    % ---- frames, events, clock
    ysp_dataset('write_frame', h, 'frame', 120, 'trial_index', 3, 'target_s', 10, 'onset_s', 10.000412, ...
                'residual_s', 0.000412, 'dropped', 0, 'path', 1, 'draw_s', 0.0003);
    ysp_dataset('write_frame', h, struct('frame', 121));
    ysp_dataset('write_event', h, 't_s', 10.25, 'trial_index', 3, 'kind', 'keyboard', 'role', 'kb', 'frame', 121, ...
                'code', 37, 'value', 1, 'x', 0.5, 'y', -0.5, 'ticks', int64(2)^40, 'text', 'left');
    ysp_dataset('write_clock', h, 'role', 'box', 'kind', 'fit', 't_s', 1.5, 'ticks', 1000, ...
                'ns_per_tick', 1000.000123, 'ppm', 0.123, 'span_s', 60, 'points', 40, 'epoch', 1);
    fr = lines(fullfile(d, 'frames.csv'));
    check(strcmp(fr{2}, '120,3,10.000000000,,10.000412000,0.000412000,0,1,,,,,,0.000300000,,,'), 'frame row');
    check(strcmp(fr{3}, ['121' repmat(',', 1, 16)]), 'a frame of one field');
    ev = lines(fullfile(d, 'events.csv'));
    check(strcmp(ev{2}, '10.250000000,3,keyboard,kb,,121,,,37,,,1,0.5,-0.5,,,,,1099511627776,left'), 'event row');
    ck = lines(fullfile(d, 'clock.csv'));
    check(strcmp(ck{2}, 'box,fit,1.500000000,1000,,1000.000123,0.123,,60.000000000,40,1'), 'clock row');
    expect_error('ysp_dataset:arg', @() ysp_dataset('write_frame', h, 'onset', 1));
    expect_error('ysp_dataset:arg', @() ysp_dataset('write_event', h, 'kind', 3));

    % ---- streams, by id and by name
    dots = ysp_dataset('stream', h, 'dots', {'t_s', 'time', []; 'coherence', 'double', 3});
    eye = ysp_dataset('stream', h, 'eye', {'t_s', 'time', 6; 'x', 'double', []; 'eye', 'const', 'L'});
    check(dots == 1 && eye == 2, 'stream ids are 1-based');
    ysp_dataset('write_stream', h, dots, 't_s', 0.0166, 'coherence', 0.51234);
    ysp_dataset('write_stream', h, 'dots', struct('t_s', 0.0333));
    ysp_dataset('write_stream', h, 'eye', 't_s', 0.0010004, 'x', 1.5);
    check(isequal(lines(fullfile(d, 'streams', 'dots.csv')), {'t_s,coherence', '0.016600000,0.512', '0.033300000,'}), ...
          'stream rows');
    check(isequal(lines(fullfile(d, 'streams', 'eye.csv')), {'t_s,x,eye', '0.001000,1.5,L'}), 'stream decimals, const');
    expect_error('ysp_dataset:arg', @() ysp_dataset('stream', h, 'DOTS', {'x', 'int'}));
    expect_error('ysp_dataset:arg', @() ysp_dataset('stream', h, 'my dots', {'x', 'int'}));
    expect_error('ysp_dataset:arg', @() ysp_dataset('write_stream', h, 'nope', 'x', 1));
    expect_error('ysp_dataset:arg', @() ysp_dataset('write_stream', h, 3, 'x', 1));
    for i = 1:14
        ysp_dataset('stream', h, sprintf('s%d', i), {'v', 'int'});
    end
    expect_error('ysp_dataset:full', @() ysp_dataset('stream', h, 's99', {'v', 'int'}));

    % ---- session.json: set and push, rewritten atomically
    ysp_dataset('session_set', h, 'fields', struct('participant', 'P07', 'age', 31, 'list', {{1, 2.5, [], true, 'x'}}, ...
                'v', [1 2 3], 'm', [1 2; 3 4], 'u', uint64(18446744073709551615)));
    ysp_dataset('session_push', h, 'changes', struct('key', 'contrast', 'value', 0.2));
    ysp_dataset('session_push', h, 'changes', struct('key', 'contrast', 'value', 0.1));
    txt = fileread_utf8(fullfile(d, 'session.json'));
    s = session(d);
    check(~isempty(strfind(txt, '"list": [1, 2.5, null, true, "x"]')) && isequal(s.fields.m, [1 2; 3 4]), ...
          'session values');
    check(~isempty(strfind(txt, '18446744073709551615')) && ~isempty(strfind(txt, '"v": [1, 2, 3]')), 'session numbers');
    s = session(d);
    check(numel(s.changes) == 2 && isequal(s.closed, false), 'session_push');
    check(exist(fullfile(d, 'session.json.tmp'), 'file') == 0, 'no temporary file');
    expect_error('ysp_dataset:arg', @() ysp_dataset('session_set', h, 'closed', true));
    expect_error('ysp_dataset:arg', @() ysp_dataset('session_set', h, 'rows', 1));
    expect_error('ysp_dataset:arg', @() ysp_dataset('session_push', h, 'fields', 1));
    expect_error('ysp_dataset:arg', @() ysp_dataset('session_set', h, 'x', NaN));
    expect_error('ysp_dataset:arg', @() ysp_dataset('session_set', h, 'x', @sin));

    % ---- an unfinished session reads, while it is open
    t = lines(fullfile(d, 'trials.csv'));
    s = session(d);
    check(numel(t) == 7 && isequal(s.closed, false), 'readable while open');

    % ---- finish, zip, close
    expect_error('ysp_dataset:order', @() ysp_dataset('zip', h));
    r = ysp_dataset('finish', h);
    check(r.trials == 5 && r.frames == 2 && r.events == 1 && r.clock == 1 && r.streams.dots == 2 && r.streams.s14 == 0, ...
          'finish returns the rows');
    s = session(d);
    check(isequal(s.closed, true) && s.rows.trials == 5 && s.rows.streams.eye == 1, 'session.json at the finish');
    r2 = ysp_dataset('rows', h);
    check(isequal(r, r2), 'rows after the finish');
    expect_error('ysp_dataset:closed', @() ysp_dataset('write_trial', h, 'trial_index', 9));
    expect_error('ysp_dataset:closed', @() ysp_dataset('write_frame', h, 'frame', 9));
    expect_error('ysp_dataset:closed', @() ysp_dataset('session_set', h, 'x', 1));
    expect_error('ysp_dataset:closed', @() ysp_dataset('finish', h));
    z = ysp_dataset('zip', h);
    check(isa(z, 'uint8') && isequal(z(1:4), uint8([80 75 3 4])), 'zip bytes');
    ysp_dataset('zip', h, fullfile(root, 'P07.zip'));
    fid = fopen(fullfile(root, 'P07.zip'), 'rb'); z2 = fread(fid, Inf, '*uint8')'; fclose(fid);
    check(isequal(z, z2), 'the zip to a file is the same bytes');
    check(~isempty(strfind(char(z), 'P07/streams/dots.csv')) && ~isempty(strfind(char(z), 'P07/session.json')), 'zip names');
    ysp_dataset('close', h);
    expect_error('ysp_dataset:handle', @() ysp_dataset('rows', h));

    % ---- open refusals leave nothing
    mkdir(fullfile(root, 'there'));
    expect_error('ysp_dataset:exists', @() ysp_dataset('open', struct('dir', fullfile(root, 'there'), 'trial_cols', {{'x', 'int'}})));
    expect_error('ysp_dataset:io', @() ysp_dataset('open', struct('dir', fullfile(root, 'no', 'parent'), 'trial_cols', {{'x', 'int'}})));
    expect_error('ysp_dataset:arg', @() ysp_dataset('open', struct('dir', fullfile(root, 'a'), 'trial_cols', {{'rt', 'time'}})));
    expect_error('ysp_dataset:arg', @() ysp_dataset('open', struct('dir', fullfile(root, 'a'), 'trial_cols', {{'x', 'int'}}, ...
                                                             'session', struct('closed', true))));
    expect_error('ysp_dataset:arg', @() ysp_dataset('open', struct('dir', fullfile(root, 'a'), 'trial_cols', {{'x', 'float32'}})));
    expect_error('ysp_dataset:arg', @() ysp_dataset('open', struct('dir', fullfile(root, 'a'), 'trial_col', {{'x', 'int'}})));
    check(exist(fullfile(root, 'a'), 'dir') == 0, 'a refused open makes nothing');

    % ---- close alone finishes; sync
    h = ysp_dataset('open', struct('dir', fullfile(root, 'b'), 'trial_cols', {{'x', 'int'}}, 'sync', true));
    ysp_dataset('write_trial', h, 'x', 1);
    ysp_dataset('close', h);
    s = session(fullfile(root, 'b'));
    check(isequal(s.closed, true) && s.rows.trials == 1, 'close alone finishes');

    % ---- 'abandon': an abnormal end, for a script's catch block
    d = fullfile(root, 'ab');
    h = ysp_dataset('open', struct('dir', d, 'trial_cols', {{'x', 'int'}}, 'session', struct('experiment', 'ab')));
    try
        ysp_dataset('write_trial', h, 'x', 1);
        ysp_dataset('write_trial', h, 'x', 2);
        error('test_mex_dataset:experiment', 'the experiment failed');
    catch err
        ysp_dataset('abandon', h);
    end
    check(strcmp(err.identifier, 'test_mex_dataset:experiment'), 'the catch block ran');
    unfinished(d, {'x', '1', '2'});
    expect_error('ysp_dataset:handle', @() ysp_dataset('rows', h));
    fid = fopen(fullfile(d, 'trials.csv'), 'a');
    check(fid >= 0, 'abandon closed the table files');
    if fid >= 0, fclose(fid); end

    % ---- seed_text
    check(strcmp(ysp_dataset('seed_text', uint64(18446744073709551615)), 'xffffffffffffffff'), 'seed_text max');
    check(strcmp(ysp_dataset('seed_text', 255), 'x00000000000000ff'), 'seed_text');

    % ---- a crash: a child process writes rows, then is killed; the files read
    crash_check(root, here);

    % ---- trial rows from ysp_trials, when it is built
    trials_check(root);

    % ---- clear ysp_dataset with a handle open: abandoned, not finished
    d = fullfile(root, 'cleared');
    h = ysp_dataset('open', struct('dir', d, 'trial_cols', {{'x', 'int'}}));
    ysp_dataset('write_trial', h, 'x', 7);
    clear ysp_dataset
    unfinished(d, {'x', '7'});
    expect_error('ysp_dataset:handle', @() ysp_dataset('rows', h));   % a new table after the reload

    fprintf('PASS (%d checks)\n', YSP_DS_CHECKS);
end

function crash_check(root, here)
    d = fullfile(root, 'crash');
    ready = fullfile(root, 'ready.txt');
    if ~exist('OCTAVE_VERSION', 'builtin') && strcmp(getenv('GITHUB_ACTIONS'), 'true')
        % CI's MATLAB runs on a batch licensing token; a second MATLAB
        % started from it is not known to start. Octave's job runs this.
        fprintf('skipping the crash check: no second MATLAB on CI\n');
        return;
    end
    if exist('OCTAVE_VERSION', 'builtin')
        exe = fullfile(OCTAVE_HOME, 'bin', 'octave-cli');
        getpid_expr = 'getpid()';
    else
        exe = fullfile(matlabroot, 'bin', 'matlab');
        getpid_expr = 'feature(''getpid'')';
    end
    body = sprintf(['addpath(''%s''); h = ysp_dataset(''open'', struct(''dir'', ''%s'', ''trial_cols'', ' ...
                    '{{''trial_index'', ''int''; ''response'', ''string''}}, ''session'', struct(''experiment'', ''crash''))); ' ...
                    'for i = 0:4, ysp_dataset(''write_frame'', h, ''frame'', i, ''onset_s'', 1 + i / 60); end; ' ...
                    'ysp_dataset(''write_trial'', h, ''trial_index'', 0, ''response'', ''left''); ' ...
                    'ysp_dataset(''session_push'', h, ''changes'', struct(''value'', 0.5)); ' ...
                    'ysp_dataset(''write_trial'', h, ''trial_index'', 1, ''response'', ''right''); ' ...
                    'f = fopen(''%s.tmp'', ''w''); fprintf(f, ''%%d'', %s); fclose(f); movefile(''%s.tmp'', ''%s''); pause(120);'], ...
                   here, d, ready, getpid_expr, ready, ready);
    body = strrep(body, '\', '/');
    ready = strrep(ready, '\', '/');
    if exist('OCTAVE_VERSION', 'builtin')
        cmd = sprintf('"%s" -q --no-gui --eval "%s"', exe, body);
    else
        cmd = sprintf('"%s" -batch "%s"', exe, body);
    end
    if exist('OCTAVE_VERSION', 'builtin')
        system(cmd, false, 'async');
    elseif ispc
        system([cmd ' &']);   % MATLAB: a window of its own, and returns at once
    else
        system([cmd ' > /dev/null 2>&1 &']);
    end
    t0 = tic;
    while exist(ready, 'file') ~= 2 && toc(t0) < 120
        pause(0.2);
    end
    check(exist(ready, 'file') == 2, 'the crash child started');
    pid = str2double(fileread(ready));
    if ispc
        system(sprintf('taskkill /F /PID %d > NUL 2>&1', pid));
    else
        system(sprintf('kill -9 %d', pid));
    end
    pause(1);   % the OS closes the killed process's files
    s = session(d);
    check(isequal(s.closed, false) && ~isfield(s, 'rows') && numel(s.changes) == 1, 'a killed session says closed false');
    check(exist(fullfile(d, 'session.json.tmp'), 'file') == 0, 'no temporary file after the kill');
    check(isequal(lines(fullfile(d, 'trials.csv')), {'trial_index,response', '0,left', '1,right'}), ...
          'every trial row of the killed session reads');
    check(numel(lines(fullfile(d, 'frames.csv'))) == 6, 'every frame row of the killed session reads');
end

function trials_check(root)
    % trials.csv has no session part (desc.trials NULL), so a script writes
    % the trials writer's columns as its own; they must equal ysp_trials'
    % own data line.
    if exist('ysp_trials', 'file') ~= 3
        fprintf('skipping the ysp_trials check: ysp_trials is not built\n');
        return;
    end
    t = ysp_trials('open', struct('n_conditions', 2, 'reps', 2, 'order', 'full_random', 'rng', 1, 'seed', uint64(7), ...
                   'vars', ysp_trials('var_choice', 'side', {'left', 'right'})));
    names = strsplit(strtrim(ysp_trials('format_header', t)), ',');
    types = struct('index', 'int', 'block', 'int', 'rep', 'int', 'condition', 'int', 'track', 'int', 'practice', 'bool', ...
                   'warmup', 'bool', 'requeued', 'bool', 'after_break', 'bool', 'outcome', 'int', 'side', 'string', ...
                   'trial_seed', 'string');
    cols = cell(numel(names), 2);
    for i = 1:numel(names)
        cols(i, :) = {names{i}, types.(names{i})};
    end
    h = ysp_dataset('open', struct('dir', fullfile(root, 'tr'), 'trial_cols', {cols}));
    want = {};
    ti = ysp_trials('next', t);
    while ~isempty(ti)
        ysp_trials('update', t, mod(ti.index, 2));
        line = strtrim(ysp_trials('format_row', t, ti.index));
        want{end + 1} = line; %#ok<AGROW>
        v = strsplit(line, ',');
        row = struct();
        for i = 1:numel(names)
            switch types.(names{i})
                case 'int', row.(names{i}) = str2double(v{i});
                case 'bool', row.(names{i}) = str2double(v{i}) ~= 0;
                otherwise, row.(names{i}) = v{i};
            end
        end
        row.side = ysp_trials('var', t, ti.index, 'side');
        row.trial_seed = ysp_dataset('seed_text', ysp_trials('trial_seed', t, ti.index));
        ysp_dataset('write_trial', h, row);
        ti = ysp_trials('next', t);
    end
    ysp_dataset('close', h);
    ysp_trials('close', t);
    got = lines(fullfile(root, 'tr', 'trials.csv'));
    check(isequal(got(2:end), want), 'trials.csv rows equal ysp_trials'' data line');
end

function unfinished(d, want)
    % What an abnormal end leaves: "closed": false, no rows, every row.
    s = session(d);
    check(isequal(s.closed, false) && ~isfield(s, 'rows'), 'an abnormal end leaves "closed": false (%s)', d);
    check(exist(fullfile(d, 'session.json.tmp'), 'file') == 0, 'no temporary file (%s)', d);
    check(isequal(lines(fullfile(d, 'trials.csv')), want), 'every row reads back (%s)', d);
end

function s = session(d)
    txt = fileread_utf8(fullfile(d, 'session.json'));
    check(txt(end) == 10 && txt(end - 1) == '}' && ~any(txt == 13), 'session.json is one object and an LF');
    s = jsondecode(txt);
end

function t = fileread_utf8(p)
    % The file's bytes as char codes 0..255, so UTF-8 compares as bytes in
    % MATLAB and in Octave.
    fid = fopen(p, 'rb');
    t = char(fread(fid, Inf, 'uint8=>uint8')');
    fclose(fid);
end

function L = lines(p)
    t = fileread_utf8(p);
    if ~isempty(t) && t(end) == 10, t = t(1:end - 1); end
    L = strsplit(t, char(10));
end

function check(cond, msg, varargin)
    global YSP_DS_CHECKS %#ok<GVMIS>
    YSP_DS_CHECKS = YSP_DS_CHECKS + 1;
    if ~cond
        error('test_mex_dataset:fail', ['FAIL: ' msg], varargin{:});
    end
end

function expect_error(id, f)
    global YSP_DS_CHECKS %#ok<GVMIS>
    YSP_DS_CHECKS = YSP_DS_CHECKS + 1;
    try
        f();
    catch e
        if ~strcmp(e.identifier, id)
            error('test_mex_dataset:fail', 'FAIL: expected %s, got %s: %s', id, e.identifier, e.message);
        end
        return;
    end
    error('test_mex_dataset:fail', 'FAIL: expected %s, nothing was raised', id);
end
