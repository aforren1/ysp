% example_gfx_reading.m - reading by rapid serial visual presentation (RSVP)
% with ysp_gfx: a sentence, one word every 0.25 s. Each word is a text
% laid out at setup, centered on fixation. A ysp_timeline channel per
% word, bound to the word's visible, turns it on for its 0.25 s, so the
% stream runs without a MATLAB call per word or per frame. After the
% stream the whole sentence is shown, with each word's box in screen
% pixels ('words'), as an eye tracker's interest areas would take them.
% The flip records give each word's measured onset.
%
% Build first (build.m), then run it from this directory. Windows only.
% For the simulated display (no window): SIM = true; then run it.

if ~exist('SIM', 'var'), SIM = false; end
SENTENCE = 'The quick brown fox jumps over the lazy dog near the riverbank';
SOA = 0.25; SIZE = 48;
words = strsplit(SENTENCE, ' ');
N = numel(words);
if SIM
    h = ysp_screen('open', 'backend', 'sim', 'gfx', struct('background', 0.5));
else
    h = ysp_screen('open', 'windowed', true, 'size', [1280 720], 'gfx', struct('background', 0.5));
end
try
    g = ysp_gfx('open', h);
    font = ysp_gfx('font', g, fullfile(getenv('WINDIR'), 'Fonts', 'segoeui.ttf'));
    T = zeros(1, N, 'uint64');
    for k = 1:N
        T(k) = ysp_gfx('text', g, words{k}, 'font', font, 'size', SIZE, 'color', 0, 'visible', 0);
    end
    shown = ysp_gfx('list', g, T);
    tl = ysp_timeline('open', struct('channels', N));
    for k = 1:N
        ysp_timeline('set_keys', tl, k, 1, [(k - 1) * SOA 1 1; k * SOA 0 1]);   % ease 1: step
    end
    ysp_gfx('bind', g, shown, 'visible', tl, 1:N);   % one channel per word
    in = ysp_gfx('info', g);
    sentence = ysp_gfx('text', g, SENTENCE, 'font', font, 'size', 32, 'color', 0, 'width', 0.9 * in.size(1));
    ysp_gfx('prime', g);
    f = ysp_screen('begin', h);
    start = f.onset + 0.5;
    ysp_timeline('anchor', tl, 1, start);
    R = [];
    while f.onset < start + N * SOA + 0.25
        ysp_timeline('evaluate', tl, f.onset, f.period);
        ysp_gfx('frame', g, shown);
        ysp_screen('flip', h);
        [f, quit, done] = ysp_screen('begin', h);
        if ~isempty(quit), error('example:abort', 'aborted'); end
        R = [R done]; %#ok<AGROW>
    end
    ysp_gfx('frame', g, sentence);
    ysp_screen('flip', h);
    ysp_screen('wait_flip', h);
    R = [R ysp_screen('records', h)];
    [W, w] = ysp_gfx('words', sentence);
    ysp_screen('close', h);
    ysp_timeline('close', tl);
catch err
    try, ysp_screen('close', h); catch, end
    rethrow(err);
end
t = [R.target];
on = R(t >= start - 0.002 & t < start + N * SOA);
k = floor(([on.target] - start + 0.002) / SOA) + 1;
[~, first] = unique(k);                      % each word's first frame
o = [on(first).onset];
fprintf('%d words; measured SOA %.4f to %.4f s; %d missed frames\n', numel(first), min(diff(o)), max(diff(o)), sum([R.missed]));
fprintf('the sentence''s words, screen px [x0 y0 x1 y1]:\n');
for j = 1:numel(w)
    fprintf('  %-10s chars %2d-%2d line %d  [%6.1f %6.1f %6.1f %6.1f]\n', w{j}, W(j, 1), W(j, 2), W(j, 3), W(j, 4:7));
end
