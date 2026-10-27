% example_gfx_2afc.m - a two-alternative forced-choice grating trial with
% ysp_gfx and ysp_timeline: a fixation dot for 0.5 s, then two gratings for
% 0.2 s with a 50 ms contrast ramp; one has the higher contrast. Answer with
% the Left or Right arrow; the dot turns green or red. The frame loop is
% three calls (ysp_timeline 'evaluate', ysp_gfx 'frame', ysp_screen 'flip'):
% the gratings' gate and contrasts are bound to timeline channels, so no
% MATLAB call sets a stimulus in a frame. Each trial prints the reaction
% time from the stimulus onset that the flip record measured.
%
% Build first (build.m), then run it from this directory. Windows only.
% For the simulated display (random answers, no window): SIM = true; then
% run it.

if ~exist('SIM', 'var'), SIM = false; end
if SIM
    h = ysp_screen('open', 'backend', 'sim', 'gfx', struct('background', 0.5));
else
    h = ysp_screen('open', 'windowed', true, 'size', [1280 720], 'gfx', struct('background', 0.5));
end
try
    g = ysp_gfx('open', h);                      % the programs compiled while the screen settled
    c = ysp_screen('constants');
    LEFT = hex2dec('40000050'); RIGHT = hex2dec('4000004F');   % SDL keycodes
    pair = ysp_gfx('group', g);                  % one gate for both gratings
    left = ysp_gfx('grating', g, 'x', -200, 'w', 200, 'sf', 1 / 20, 'aperture', 'circle', ...
                   'edge', 'cosine', 'edge_width', 12, 'group', pair);
    right = ysp_gfx('copy', left);
    ysp_gfx('set', right, 'x', 200);
    fix = ysp_gfx('shape', g, 'circle', 'w', 10, 'color', 0);
    scene = ysp_gfx('list', g, [left right fix]);
    tl = ysp_timeline('open', struct('channels', 3));   % 1 gate, 2 left contrast, 3 right contrast
    ysp_gfx('bind', g, pair, 'opacity', tl, 1);
    ysp_gfx('bind', g, left, 'contrast', tl, 2);
    ysp_gfx('bind', g, right, 'contrast', tl, 3);
    ysp_gfx('prime', g);
    deltas = [0.15 0.1 0.05 0.1 0.15];
    for trial = 1:numel(deltas)
        side = randi(2);                         % 1 left, 2 right
        ysp_timeline('clear', tl, 1);
        q = ysp_timeline('seq', tl, 1);
        ysp_timeline('set', q, 2, 0.2 + deltas(trial) * (side == 1));
        ysp_timeline('set', q, 3, 0.2 + deltas(trial) * (side == 2));
        ysp_timeline('wait', q, 0.5);
        ysp_timeline('mark', q, 1);              % the stimulus onset
        ysp_timeline('then', q, 1, 1, 'duration', 0.05, 'ease', 'cosine');
        ysp_timeline('wait', q, 0.1);
        ysp_timeline('then', q, 1, 0, 'duration', 0.05, 'ease', 'cosine');
        ysp_timeline('release', q);
        ysp_gfx('set', fix, 'color', 0);
        f = ysp_screen('begin', h);
        ysp_timeline('anchor', tl, 1, f.onset);
        onset_frame = -1; onset = []; answer = [];
        while isempty(answer)
            [~, ev] = ysp_timeline('evaluate', tl, f.onset, f.period);
            if any(strcmp({ev.kind_name}, 'mark')), onset_frame = f.index; end
            ysp_gfx('frame', g, scene);
            ysp_screen('flip', h);
            [f, quit, done] = ysp_screen('begin', h);
            if ~isempty(quit), error('example:abort', 'aborted'); end
            for k = 1:numel(done)
                if done(k).index == onset_frame, onset = done(k); end   % the measured onset
            end
            e = ysp_screen('poll', h);
            for k = 1:numel(e)
                if e(k).kind == c.KIND_KEYBOARD && e(k).type == c.PRESS && any(e(k).code == [LEFT RIGHT]) && ~isempty(onset)
                    answer = [1 + (e(k).code == RIGHT), e(k).t];
                end
            end
            if SIM && ~isempty(onset) && f.onset - onset.onset > 0.4, answer = [randi(2), f.onset]; end
        end
        correct = answer(1) == side;
        ysp_gfx('set', fix, 'color', [0.8 * ~correct, 0.8 * correct, 0]);
        ysp_gfx('frame', g, scene);
        ysp_screen('flip', h);
        words = {'wrong', 'correct'};
        fprintf('trial %d: delta %.2f, side %d, %s, RT %.3f s, onset residual %+.1f us\n', trial, deltas(trial), side, ...
                words{1 + correct}, answer(2) - onset.onset, onset.residual * 1e6);
        for k = 1:30                             % 0.5 s of feedback
            ysp_screen('begin', h);
            ysp_gfx('frame', g, scene);
            ysp_screen('flip', h);
        end
    end
    ysp_screen('close', h);                      % the gfx, its stimuli and its timeline pin go with it
    ysp_timeline('close', tl);
catch err
    % try/catch, not onCleanup: Octave did not run an onCleanup here
    % (docs/av_probe.md, section 4).
    try, ysp_screen('close', h); catch, end
    rethrow(err);
end
