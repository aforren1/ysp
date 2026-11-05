% example_timeline.m - ysp_timeline on a simulated 60 Hz display: a trial
% built as a sequence, evaluated at each frame's predicted onset, then run
% again by a rewind. Build first (run build.m), then run this file.
%
% In Psychtoolbox the onset is vbl + ifi from the last Screen('Flip'), and
% the period is ifi; nothing here reads a clock.

TRIAL = 1; FIX = 1; GRATING = 2; CONTRAST = 3;
ifi = 1 / 60;
t0 = 100;                                         % the first flip's onset, s

h = ysp_timeline('open', struct('channels', 3));
dot = ysp_timeline('field', h, [0 0]);            % a position the timeline writes

q = ysp_timeline('seq', h, TRIAL);
ysp_timeline('on', q, FIX);
ysp_timeline('wait', q, 0.5);
ysp_timeline('off', q, FIX);
ysp_timeline('on', q, GRATING);
ysp_timeline('trigger', q, 12);
ysp_timeline('to', q, CONTRAST, 0.5, 'duration', 0.1, 'ease', 'cosine');
ysp_timeline('tween_to', q, dot, [300 0], 0.6, 'cubic_out');
ysp_timeline('wait', q, 0.6);
ysp_timeline('then', q, CONTRAST, 0, 'duration', 0.1, 'ease', 'cosine');
ysp_timeline('off', q, GRATING);

for first = [0 200]
    ysp_timeline('anchor', h, TRIAL, t0 + first * ifi);   % trial time 0
    for i = first:first + 79
        [v, ev, f] = ysp_timeline('evaluate', h, t0 + i * ifi, ifi, i);
        % Screen('DrawTexture', ...) with v(GRATING), v(CONTRAST), f(1:2, dot)
        for e = ev'
            fprintf('frame %3d  %-7s ch %d code %2d  %.1f us late\n', e.frame, e.kind_name, ...
                    e.target, e.code, e.residual * 1e6);
        end
        if any(i - first == [30 33 36 60 72])
            fprintf('frame %3d  contrast %.4f  dot x %.2f\n', i, v(CONTRAST), f(1, dot));
        end
    end
    if first == 0, fprintf('-- the same trial again, anchored at frame 200 --\n'); end
end
ev = ysp_timeline('events', h, TRIAL);
fprintf('%d events in the record\n', numel(ev));
ysp_timeline('close', h);
