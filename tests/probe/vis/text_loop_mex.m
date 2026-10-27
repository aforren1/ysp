function text_loop_mex(varargin)
%TEXT_LOOP_MEX  N texts per frame in MATLAB or Octave (tests/probe/vis).
%   text_loop_mex('mode', 'words', 'n', 100, 'seconds', 60, 'windowed', 0, 'csv', 'm.csv')
%   The scenes of text_loop.py: 'words' (N one-word texts in a list),
%   'page' (one paragraph of 520 characters wrapped at 600 px), 'label' (N
%   words and a label whose text changes every frame). Each frame:
%   ysp_screen('begin'), ysp_gfx('frame', g, scene), ysp_screen('flip'), and
%   in 'label' one ysp_gfx('set', label, 'text', ...). The screen opens with
%   frame='row'. Writes one CSV row per flip record (table.py).
%   bindings/mex must be on the path. Shift+Esc ends a run.
    o = struct('mode', 'words', 'n', 100, 'seconds', 60, 'windowed', 0, 'csv', 'm.csv');
    for i = 1:2:numel(varargin), o.(varargin{i}) = varargin{i + 1}; end
    words = {'reading', 'fixation', 'saccade', 'parafovea', 'lexical', 'syntax', 'meaning', 'context', 'stimulus'};
    lorem = repmat(['Reading research shows text in short lines with clear spacing. The eyes jump from word to word, and ' ...
                    'each fixation takes in a few letters to the left and more to the right. Long words are fixated more ' ...
                    'often, and frequent words are skipped. '], 1, 3);
    if o.windowed
        h = ysp_screen('open', 'windowed', true, 'size', [1280 720], 'frame', 'row', 'gfx', struct('background', 0.5, 'max_draws', 4096));   % a text run takes 3 blocks of max_draws
    else
        h = ysp_screen('open', 'frame', 'row', 'gfx', struct('background', 0.5, 'max_draws', 4096));   % a text run takes 3 blocks of max_draws
    end
    try
        g = ysp_gfx('open', h);
        ysp_screen('title', h, 'ysp-vis text probe');
        font = ysp_gfx('font', g, fullfile(getenv('WINDIR'), 'Fonts', 'segoeui.ttf'));
        in = ysp_gfx('info', g);
        w = in.size(1); hh = in.size(2);
        items = zeros(1, 0, 'uint64');
        label = [];
        if any(strcmp(o.mode, {'words', 'label'}))
            cols = 10; if o.n > 100, cols = 20; end   % 500 words: 25 rows of 20, on the screen
            for i = 0:o.n - 1
                items(end + 1) = ysp_gfx('text', g, words{mod(i, numel(words)) + 1}, 'font', font, 'size', 28, 'color', 0.1, ...
                                         'x', mod(i, cols) * (w / cols) - w / 2 + w / cols / 2, 'y', floor(i / cols) * 40 - hh / 2 + 40); %#ok<AGROW>
            end
        else
            items = ysp_gfx('text', g, lorem(1:520), 'font', font, 'size', 20, 'width', 600, 'align', 'start', 'color', 0.1);
        end
        if strcmp(o.mode, 'label')
            label = ysp_gfx('text', g, 'frame 0123456789', 'font', font, 'size', 40, 'color', 0.9, 'y', hh / 2 - 60);
            items(end + 1) = label;
        end
        scene = ysp_gfx('list', g, items);
        ysp_gfx('prime', g);
        line = sprintf('%s; gfx %s; %s n %d', ysp_screen('describe', h), ysp_gfx('version'), o.mode, o.n);
        n = ceil(o.seconds * 61) + 200;
        R = zeros(n, 15);
        k = 0;
        t_end = [];
        while true
            [f, quit, D] = ysp_screen('begin', h);    % f: [onset period index vblank n_done done_lost]
            if ~isempty(quit), fprintf('abort 0x%x\n', quit.reasons); break; end
            if isempty(t_end), t_end = f(1) + o.seconds; end
            m = size(D, 1);
            if m > 0, R(k + 1:k + m, :) = D; k = k + m; end
            if f(1) >= t_end
                ysp_screen('flip', h);
                break;
            end
            if ~isempty(label), ysp_gfx('set', label, 'text', sprintf('frame %d', f(3))); end
            ysp_gfx('frame', g, scene);
            ysp_screen('flip', h);
        end
        ysp_screen('wait_flip', h);
        D = ysp_screen('records', h);
        last = -1;
        if k > 0, last = R(k, 1); end
        D = D(D(:, 1) > last, :);
        R(k + 1:k + size(D, 1), :) = D;
        k = k + size(D, 1);
        ysp_screen('close', h);
    catch e
        try, ysp_screen('close', h); catch, end
        rethrow(e);
    end
    host = 'matlab';
    if exist('OCTAVE_VERSION', 'builtin'), host = 'octave'; end
    fid = fopen(o.csv, 'w');
    fprintf(fid, '# %s %s\n', host, line);
    fprintf(fid, 'index,target_ns,planned_ns,onset_ns,residual_ns,dropped,path,tier,flags,draw_ns,swap_ns\n');
    for j = 1:k
        r = R(j, :);
        ph = r(10:15);
        dr = -1; sw = -1;
        if ~isnan(ph(3)), dr = round(ph(3) * 1e9); end
        if ~isnan(ph(5)), sw = round(ph(5) * 1e9); end
        fprintf(fid, '%d,%.0f,%.0f,%.0f,%.0f,%d,%d,%d,%d,%d,%d\n', r(1), r(2) * 1e9, r(3) * 1e9, r(4) * 1e9, r(5) * 1e9, ...
                r(6), r(7), r(8), r(9), dr, sw);
    end
    fclose(fid);
    fprintf('%d records -> %s\n', k, o.csv);
end
