%EXAMPLE_AUDIO  ysp_audio: a tone at a time on the ysp_rt clock, and where it landed.
%   Run from this directory after build.m. backend 'null' plays nothing; leave
%   it out for the OS default output (-40 dBFS tones). In Psychtoolbox, t is
%   the flip time you want the sound on: put Screen's clock on the ysp_rt
%   clock (ysp_rt('correlate', @GetSecs)) or take t from ysp_audio('now').

opts = struct('backend', 'null');                 % struct() for the default output
d = ysp_audio('devices', opts.backend);
fprintf('outputs: %s\n', strjoin({d.name}, ', '));
h = ysp_audio('open', opts);
disp(ysp_audio('describe', h));
beep = ysp_audio('tone', h, struct('hz', 1000, 'duration', 0.1, 'peak', ysp_audio('db', -40), 'ramp', 0.005, 'id', 1));

% A sound must be handed over at least 'lead' before its time.
t = ysp_audio('now') + ysp_audio('lead', h) + 0.05;
ids = zeros(1, 5);
for k = 1:5
    ids(k) = ysp_audio('play_at', h, beep, t + 0.25 * (k - 1));
end
r = ysp_audio('wait', h, ids(end));
fprintf('onset %.6f s, residual %d ns, tier %d, flags 0x%x\n', r.onset, r.residual_ns, r.tier, r.flags);
for k = 1:5
    r = ysp_audio('result', h, ids(k));
    fprintf('  play %d: start frame %d, residual %+6d ns\n', r.id, r.start_frame, r.residual_ns);
end
ysp_audio('close', h);
