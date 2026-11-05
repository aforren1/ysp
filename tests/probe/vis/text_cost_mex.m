function text_cost_mex(reps)
%TEXT_COST_MEX  The cost of a text's layout through ysp_gfx, on the simulated
%   display (tests/probe/vis/text_cost.py's cases, in MATLAB or Octave).
%   text_cost_mex(2000). For a word, a sentence, a paragraph (wrapped at 600
%   px) and an Arabic sentence, one text is set alternately to two strings
%   of the same class (their glyphs built at setup), reps times each: the
%   mean wall time of ysp_gfx('set', t, 'text', s) over a loop of reps
%   calls (one tic and toc: Octave's toc on Windows resolves about 1 ms),
%   then in a second loop the layout's own time per call ('layout_time',
%   the C clock): median, p90, max. Microseconds. bindings/mex must be on
%   the path. This file is UTF-8.
    if nargin < 1, reps = 2000; end
    fonts = fullfile(getenv('WINDIR'), 'Fonts');
    lorem = ['Reading research shows text in short lines with clear spacing. The eyes jump from word to word, and ' ...
             'each fixation takes in a few letters to the left and more to the right. Long words are fixated more ' ...
             'often, and frequent words are skipped. '];
    lorem = repmat(lorem, 1, 3);
    cases = {'word', {'size', 64}, 'GREEN', 'BLUE';
             'sentence', {'size', 32}, 'The quick brown fox jumps over the lazy dog.', 'A lazy dog sleeps under the old brown tree.';
             'paragraph', {'size', 20, 'width', 600, 'align', 'start'}, lorem(1:520), lorem(41:560);
             'arabic', {'size', 32}, 'مرحبا بالعالم وأهلا بكم في القراءة', 'القراءة السريعة للكلمات في الجملة'};
    host = 'matlab';
    if exist('OCTAVE_VERSION', 'builtin'), host = 'octave'; end
    h = ysp_screen('open', 'backend', 'sim');
    g = ysp_gfx('open', h, 'cache', false);
    f = ysp_gfx('font', g, fullfile(fonts, 'segoeui.ttf'));
    fprintf('%s: %-10s %5s  %-12s %-28s\n', host, 'text', 'lines', 'call us mean', 'layout_time us p50/p90/max');
    for c = 1:size(cases, 1)
        kw = cases{c, 2};
        s1 = cases{c, 3};
        s2 = cases{c, 4};
        t = ysp_gfx('text', g, s1, 'font', f, kw{:});
        ysp_gfx('set', t, 'text', s2);
        t0 = tic;
        for i = 1:2:reps
            ysp_gfx('set', t, 'text', s1);
            ysp_gfx('set', t, 'text', s2);
        end
        call = toc(t0) / (2 * ceil(reps / 2));
        lay = zeros(reps, 1);
        for i = 1:reps
            ysp_gfx('set', t, 'text', cases{c, 3 + mod(i - 1, 2)});
            lay(i) = ysp_gfx('get', t, 'layout_time');
        end
        b = st(lay);
        fprintf('%s: %-10s %5d  %12.1f  %8.1f %8.1f %9.1f\n', host, cases{c, 1}, ysp_gfx('get', t, 'n_lines'), 1e6 * call, 1e6 * b);
    end
    ysp_screen('close', h);
end

function v = st(x)
    x = sort(x);
    v = [median(x) x(floor(0.9 * (numel(x) - 1)) + 1) x(end)];
end
