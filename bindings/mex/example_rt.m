%EXAMPLE_RT  ysp_rt: frames on the ysp clock, another clock mapped to it,
%   and a device clock fit. Build first (run build.m). The device in the
%   last part is simulated, so its numbers describe the fit, not a device.

disp(ysp_rt('describe'));   % log this line beside timing data

% 200 frames of 5 ms against a running deadline, so lateness cannot add up.
late = zeros(200, 1);
t = ysp_rt('now');
for i = 1:200
    t = t + 0.005;
    late(i) = ysp_rt('sleep_until', t);
end
late = sort(late);
fprintf('lateness over 200 waits: median %.1f us, p99 %.1f us, max %.1f us\n', ...
        late(101) * 1e6, late(199) * 1e6, late(end) * 1e6);

% Another clock as one pair of readings. With Psychtoolbox, @GetSecs.
try
    c = ysp_rt('correlate', @GetSecs);
    fprintf('GetSecs %.6f s was ysp time %.6f s, +- %.1f us\n', c.other, c.t, c.width / 2 * 1e6);
catch e
    fprintf('no GetSecs here (%s)\n', e.identifier);
end

% A board's micros() clock, 32 bits, its reports up to 1 ms late. It runs
% 50 ppm fast, so its tick is 50 ppm short: info.ppm is -50.
h = ysp_rt('fit_open', struct('mode', 'late', 'tick', 1e-6, 'bits', 32));
host = 100 + (0:1999) * 0.01;
us = mod(round((host - 100) * 1e6 * (1 + 50e-6)) + 4e9, 2^32);
ysp_rt('fit_add', h, uint64(us), host + mod((1:2000) * 0.6180339887, 1) * 0.001);
info = ysp_rt('fit_info', h);
fprintf('fit: %d points over %.1f s, %+.1f ppm, spread %.0f us\n', info.points, info.span, info.ppm, info.spread * 1e6);
fprintf('an event at micros() = 4010000500 happened at ysp time %.6f s (truth 110 s)\n', ...
        ysp_rt('fit_map', h, 4010000500));
ysp_rt('fit_close', h);
