% example_gfx_stroop.m - the Stroop task with ysp_gfx: name the ink color of
% a color word. 12 trials; each one: a fixation cross for 0.5 s, then RED,
% GREEN or BLUE in red, green or blue ink until a key or 2 s. Press R for
% red ink, G for green, B for blue. Half of the trials are congruent.
%
% One text shows every word: between trials its text and color change,
% which lays it out again (Skribidi and HarfBuzz; the glyphs of all three
% words are built at setup, so a change builds none). The script prints
% what each change cost. The word's onset is the flip record's (measured),
% and a reaction time is the key's time minus that onset. Colors are linear
% device RGB, not calibrated.
%
% Build first (build.m), then run it from this directory. Windows only.
% For the simulated display (no window, no keys): SIM = true; then run it.

if ~exist('SIM', 'var'), SIM = false; end
names = {'red', 'green', 'blue'};
ink = [0.6 0.05 0.05; 0.05 0.45 0.05; 0.08 0.1 0.7];
keys = double('rgb');                        % SDL keycodes of R, G, B
FIX = 0.5; LIMIT = 2.0;
rng(7);
[wi, ii] = meshgrid(1:3, 1:3);
trials = [[1:3 1:3]' [1:3 1:3]'; wi(wi ~= ii) ii(wi ~= ii)];   % 6 congruent, 6 not: [word ink]
trials = trials(randperm(size(trials, 1)), :);
if SIM
    h = ysp_screen('open', 'backend', 'sim', 'gfx', struct('background', 0.5));
else
    h = ysp_screen('open', 'windowed', true, 'size', [1280 720], 'gfx', struct('background', 0.5));
end
rows = zeros(0, 6);                          % word ink response rt_s change_s layout_s
try
    g = ysp_gfx('open', h);
    c = ysp_screen('constants');
    fix = ysp_gfx('shape', g, 'cross', 'w', 24, 'h', 24, 'width', 3, 'color', 0);
    word = ysp_gfx('text', g, 'RED GREEN BLUE', 'size', 64);   % builds every glyph the trials use
    ysp_gfx('prime', g);
    for n = 1:size(trials, 1)
        w = trials(n, 1); k = trials(n, 2);
        t0 = tic;
        ysp_gfx('set', word, 'text', upper(names{w}), 'color', ink(k, :));   % a layout, between frames
        change = toc(t0);
        f = ysp_screen('begin', h);
        end_fix = f.onset + FIX;
        while f.onset < end_fix - f.period / 2
            ysp_gfx('frame', g, fix);
            ysp_screen('flip', h);
            f = ysp_screen('begin', h);
        end
        ysp_screen('poll', h);               % drop the keys of the fixation
        ysp_gfx('frame', g, word);
        ysp_screen('flip', h);
        r = ysp_screen('wait_flip', h);
        onset = r.onset;
        resp = 0; rt = NaN;
        while true
            [f, quit] = ysp_screen('begin', h);
            if ~isempty(quit), error('example:abort', 'aborted'); end
            e = ysp_screen('poll', h);
            for j = 1:numel(e)
                if resp == 0 && e(j).kind == c.KIND_KEYBOARD && e(j).type == c.PRESS && any(e(j).code == keys)
                    resp = find(e(j).code == keys); rt = e(j).t - onset;
                end
            end
            if resp > 0 || f.onset > onset + LIMIT, break; end
            ysp_gfx('frame', g, word);
            ysp_screen('flip', h);
        end
        ysp_gfx('frame', g);                 % a blank frame ends the trial
        ysp_screen('flip', h);
        rows(end + 1, :) = [w k resp rt change ysp_gfx('get', word, 'layout_time')]; %#ok<SAGROW>
    end
    ysp_screen('close', h);
catch err
    try, ysp_screen('close', h); catch, end
    rethrow(err);
end
fprintf('trial word  ink    response rt_s   change_ms layout_ms\n');
for n = 1:size(rows, 1)
    rs = '-'; if rows(n, 3) > 0, rs = names{rows(n, 3)}; end
    fprintf('%5d %-5s %-6s %-8s %6.3f %9.3f %9.3f\n', n, names{rows(n, 1)}, names{rows(n, 2)}, rs, rows(n, 4), ...
            1e3 * rows(n, 5), 1e3 * rows(n, 6));
end
ok = rows(:, 3) == rows(:, 2);
cong = rows(:, 1) == rows(:, 2);
fprintf('median RT, correct trials: congruent %.3f s, incongruent %.3f s\n', median(rows(ok & cong, 4)), median(rows(ok & ~cong, 4)));
