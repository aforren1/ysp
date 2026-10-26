function example_dataset(base)
%EXAMPLE_DATASET  A simulated session written as a ysp data set: what a
%   Psychtoolbox script does with ysp_dataset. The flips and the key presses
%   are made up here; in Psychtoolbox, onset_s is Screen('Flip')'s VBL time
%   stamp and t_s a key's time from KbCheck or KbQueueCheck, both GetSecs.
%
%   example_dataset          writes into tempdir
%   example_dataset(base)    writes base/P07_<time> and base/P07_<time>.zip

    if nargin < 1, base = tempdir; end
    here = fileparts(mfilename('fullpath'));
    addpath(here);
    folder = fullfile(base, ['P07_' datestr(now, 'yyyymmddTHHMMSS')]); %#ok<TNOW1,DATST>

    cols = {'participant', 'const', 'P07';
            'trial_index', 'int',    [];
            'contrast',    'double', [];
            'side',        'string', [];
            'response',    'string', [];
            'correct',     'bool',   [];
            'rt_s',        'time',   []};
    session = struct('experiment', 'gabor-2afc', 'seed', 20261021, ...
                     'fields', struct('participant', 'P07', 'age', 31));
    h = ysp_dataset('open', struct('dir', folder, 'trial_cols', {cols}, 'session', session));
    try
        run_session(h);
    catch err
        % An abnormal end: the files close, session.json keeps
        % "closed": false, and the error goes on.
        ysp_dataset('abandon', h);
        rethrow(err);
    end
    rows = ysp_dataset('finish', h);                   % "closed": true
    ysp_dataset('zip', h, [folder '.zip']);
    ysp_dataset('close', h);
    fprintf('wrote %s: %d trials, %d frames, %d events, %d dots rows\n', folder, rows.trials, rows.frames, ...
            rows.events, rows.streams.dots);
    if exist('readtable') %#ok<EXIST>   MATLAB; Octave has no readtable
        T = readtable(fullfile(folder, 'trials.csv'));
        disp(T(1:3, :));
    else
        L = strsplit(fileread(fullfile(folder, 'trials.csv')), char(10));
        fprintf('%s\n', L{1:4});
    end
end

function run_session(h)

    dots = ysp_dataset('stream', h, 'dots', {'t_s', 'time', []; 'coherence', 'double', 3});
    ysp_dataset('write_clock', h, 'role', 'box', 'kind', 'pair', 't_s', 100, 'ticks', 123456789, 'width_s', 0.00002);
    period = 1 / 60;
    t = 100;
    frame = 0;
    rng(20261021);
    sides = {'left', 'right'};
    for trial = 0:9
        contrast = 0.05 * 2^randi([0 2]);
        side = sides{randi(2)};
        for k = 0:29                                   % 0.5 s of frames
            t = t + period;
            vbl = t + 0.0002 * randn();
            ysp_dataset('write_frame', h, 'frame', frame, 'trial_index', trial, 'target_s', t, 'onset_s', vbl);
            if k == 6
                onset = vbl;
                ysp_dataset('write_event', h, 't_s', vbl, 'trial_index', trial, 'kind', 'onset', ...
                            'stimulus', 'gabor', 'frame', frame);
            end
            ysp_dataset('write_stream', h, dots, 't_s', t, 'coherence', 0.5);
            frame = frame + 1;
        end
        rt = 0.25 + 0.2 * rand();
        if rand() < 0.8, response = side; else, response = sides{3 - find(strcmp(sides, side))}; end
        ysp_dataset('write_event', h, 't_s', onset + rt, 'trial_index', trial, 'kind', 'keyboard', ...
                    'role', 'kb', 'text', response);
        ysp_dataset('write_trial', h, struct('trial_index', trial, 'contrast', contrast, 'side', side, ...
                    'response', response, 'correct', strcmp(response, side), 'rt_s', rt));
        if trial == 4                                  % a live change, between trials
            ysp_dataset('session_push', h, 'changes', struct('trial_index', trial, 'key', 'contrast_step', 'value', 0.5));
        end
    end
end
