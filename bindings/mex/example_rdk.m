% example_rdk.m - ysp_rdk MEX demo (MATLAB / Octave)
%
% Run a random dot kinematogram as a rig does, log the inputs of each
% update, then regenerate the trial from the seed and the log, and check
% that the dots are the same, bit for bit. Build first (run build.m).

desc = struct('algorithm', 'MN', 'w', 10, 'count', 100, 'coherence', 0.256, ...
              'direction', 0, 'speed', 5, 'lifetime', 0.2);
seed = uint64(20261025);
t0 = 1.0;                                        % seconds
onsets = t0 + (1:120) / 60;                      % the frames' onsets

% the rig: set the floats, update, log 'get'.last after each update
h = ysp_rdk('open', desc);
ysp_rdk('start', h, seed, t0);
trial_log = [];
for k = 1:numel(onsets)
    ysp_rdk('set', h, 'coherence', 0.256 * (k > 30));   % motion starts at frame 31
    ysp_rdk('update', h, onsets(k));
    s = ysp_rdk('get', h);
    trial_log = [trial_log; s.last]; %#ok<AGROW>
end
shown = ysp_rdk('xy', h);
digest = ysp_rdk('digest', h);
ysp_rdk('close', h);

% the analysis: the same desc, the seed and the log
g = ysp_rdk('open', desc);
[xy, dir, sig] = ysp_rdk('trajectory', g, seed, t0, trial_log);
fprintf('%d updates of %d dots; signal dots on the last update: %d\n', size(xy, 3), size(xy, 1), sum(sig(:, end)));
fprintf('same digest: %d; last positions equal bit for bit: %d\n', ysp_rdk('digest', g) == digest, ...
        isequal(typecast(reshape(xy(:, :, end), [], 1), 'uint32'), typecast(shown(:), 'uint32')));
ysp_rdk('close', g);
