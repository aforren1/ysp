function test_mex_vis_text()
%TEST_MEX_VIS_TEXT  ysp_gfx's text in MATLAB or Octave, on the simulated
%   display (no window, no GPU). Checks fonts (paths, bytes, fallback
%   lists), texts laid out by Skribidi and HarfBuzz (bidi, wrapping,
%   spans), a change of text between trials, non-ASCII strings in and out
%   (MATLAB's UTF-16, Octave's UTF-8 bytes) and the character indices of
%   glyphs, words and lines, hit tests, texts in lists and bindings, the
%   refusals, and the lifetime rules (closing the gfx frees the handles;
%   'free' refuses a font a text uses). Needs Segoe UI, Arial and Microsoft
%   YaHei (Windows's). Build first (build.m). Prints PASS with the number
%   of checks. This file is UTF-8.

    global YSP_TEXT_CHECKS %#ok<GVMIS>
    YSP_TEXT_CHECKS = 0;
    addpath(fileparts(mfilename('fullpath')));
    fonts = fullfile(getenv('WINDIR'), 'Fonts');
    segoe = fullfile(fonts, 'segoeui.ttf');
    arial = fullfile(fonts, 'arial.ttf');
    yahei = fullfile(fonts, 'msyh.ttc');
    has_cjk = exist(yahei, 'file') == 2;   % a machine without YaHei: the fallback checks are skipped
    if ~has_cjk
        yahei = arial;
        disp(['no ' fullfile(fonts, 'msyh.ttc') ': the CJK fallback checks are skipped']);
    end
    is_octave = exist('OCTAVE_VERSION', 'builtin') ~= 0;

    h = ysp_screen('open', 'backend', 'sim', 'sim_period', 0.004);
    g = ysp_gfx('open', h, 'background', 0.5, 'cache', false);

    % ---- a text and its layout
    t = ysp_gfx('text', g, 'Hello, world', 'size', 40, 'color', [1 0 0]);
    check(strcmp(ysp_gfx('get', t, 'kind'), 'text') && strcmp(ysp_gfx('get', t, 'text'), 'Hello, world'), 'kind, text');
    b = ysp_gfx('get', t, 'box');
    check(b(1) > 150 && b(1) < 300 && b(2) > 40 && b(2) < 70, 'box %s', mat2str(b));
    check(ysp_gfx('get', t, 'size') == 40 && ysp_gfx('get', t, 'n_glyphs') == 12 && ysp_gfx('get', t, 'n_lines') == 1 && ...
          ysp_gfx('get', t, 'missing') == 0 && strcmp(ysp_gfx('get', t, 'align'), 'center'), 'size, glyphs, lines, align');
    check(isequal(ysp_gfx('get', t, 'color'), [1 0 0]) && isempty(ysp_gfx('get', t, 'font')), 'color; the default font');
    st = ysp_gfx('get', t, 'stamp');
    check(~isempty(strfind(st, 'harfbuzz=14.6.0')) && ~isempty(strfind(st, 'segoeui.ttf#0=')), 'stamp: %s', st);
    check(ysp_gfx('get', t, 'layout_time') > 0, 'layout_time in seconds');
    bb = ysp_gfx('bounds', t);
    check(abs(bb(1) - (400 - b(1) / 2)) < 1e-3 && abs(bb(4) - (300 + b(2) / 2)) < 1e-3, 'centered on the screen');
    frame(h, g, t);

    % ---- a change between trials
    ysp_gfx('set', t, 'text', 'GREEN', 'color', [0 1 0], 'x', 10);
    check(strcmp(ysp_gfx('get', t, 'text'), 'GREEN') && ysp_gfx('get', t, 'n_glyphs') == 5 && ysp_gfx('get', t, 'x') == 10, 'set: text, color, x');
    ysp_gfx('set', t, 'text', 'Hello, world');
    check(ysp_gfx('get', t, 'new_glyphs') == 0, 'its glyphs are built already');
    ysp_screen('begin', h);
    ysp_gfx('begin', g);
    expect_error('ysp_gfx:order', @() ysp_gfx('set', t, 'text', 'late'));
    ysp_gfx('set', t, 'x', 20);
    ysp_gfx('draw', g, t);
    ysp_gfx('end', g);
    ysp_screen('flip', h);
    check(strcmp(ysp_gfx('get', t, 'text'), 'Hello, world') && ysp_gfx('get', t, 'x') == 20, 'a refused layout leaves the text');

    % ---- non-ASCII in and out; indices in the host's units
    s = 'naïve 中文 😀 end';
    u = ysp_gfx('text', g, s, 'size', 30);
    check(isequal(ysp_gfx('get', u, 'text'), s), 'a non-ASCII string round trip');
    [W, w] = ysp_gfx('words', u);
    check(size(W, 2) == 7 && numel(w) == size(W, 1), 'words: n x 7 and their strings');
    ok = true;
    for i = 1:numel(w)
        ok = ok && isequal(w{i}, s(W(i, 1):W(i, 2)));
    end
    check(ok && isequal(w{1}, 'naïve') && isequal(w{2}, '中文'), 'each word is s(first:last)');
    check(W(2, 1) == strfind(s, '中'), 'a CJK word starts at strfind (%d, %d)', W(2, 1), strfind(s, '中'));
    G = ysp_gfx('glyphs', u);
    check(size(G, 2) == 6 && G(1, 1) == 1 && any(G(:, 1) == strfind(s, '中')), 'glyphs: [char line x0 y0 x1 y1]');
    if is_octave
        check(numel(s) == 22 && W(3, 1) == 20, 'Octave: UTF-8 byte indices');
    else
        check(numel(s) == 15 && W(3, 1) == 13, 'MATLAB: UTF-16 indices (the emoji takes two)');
    end
    ar = 'مرحبا بالعالم';
    a = ysp_gfx('text', g, ar, 'size', 40);
    [Wa, wa] = ysp_gfx('words', a);
    check(strcmp(ysp_gfx('get', a, 'resolved_dir'), 'rtl') && isequal(wa{1}, ar(Wa(1, 1):Wa(1, 2))) && Wa(1, 4) > Wa(2, 6) - 1e-3, ...
          'Arabic: right to left, the first word on the right');

    % ---- wrapping, lines, alignment
    p = ysp_gfx('text', g, repmat('The quick brown fox jumps over the lazy dog ', 1, 3), 'size', 24, 'width', 200, 'align', 'start');
    L = ysp_gfx('lines', p);
    pb = ysp_gfx('get', p, 'box');
    Wp = ysp_gfx('words', p);
    bp = ysp_gfx('bounds', p);
    check(size(L, 1) == ysp_gfx('get', p, 'n_lines') && size(L, 1) > 3 && pb(1) == 200 && all(Wp(:, 6) <= bp(1) + 200 + 1e-3), ...
          'wrapped at 200 px: lines, the box, every word inside');
    check(L(1, 1) == 1 && all(L(2:end, 1) == L(1:end-1, 2) + 1), 'lines cover the text in order');

    % ---- fonts: path, bytes, fallback list
    f = ysp_gfx('font', g, arial);
    fi = ysp_gfx('font_info', f);
    check(strcmp(fi(1).name, 'arial.ttf') && fi(1).units_per_em == 2048 && numel(fi(1).sha256) == 64, 'font_info');
    fid = fopen(arial, 'r');
    raw = fread(fid, Inf, '*uint8');
    fclose(fid);
    fb = ysp_gfx('font', g, raw', 'name', 'Arial bytes');
    fbi = ysp_gfx('font_info', fb);
    check(strcmp(fbi.sha256, fi.sha256) && strcmp(fbi.name, 'Arial bytes'), 'a font from bytes: the same hash');
    ta = ysp_gfx('text', g, 'Arial', 'font', f, 'size', 30);
    tb = ysp_gfx('text', g, 'Arial', 'font', fb, 'size', 30);
    check(isequal(ysp_gfx('get', ta, 'box'), ysp_gfx('get', tb, 'box')) && ysp_gfx('get', ta, 'font') == f, 'the same layout; get font');
    m = ysp_gfx('text', g, 'abc 中文', 'font', segoe, 'size', 30);
    check(ysp_gfx('get', m, 'missing') == 2, 'CJK in Segoe UI: missing');
    fl = ysp_gfx('font', g, {segoe, yahei});
    c = ysp_gfx('text', g, 'abc 中文', 'font', fl, 'size', 30);
    c2 = ysp_gfx('text', g, 'abc 中文', 'font', {segoe, yahei}, 'size', 30);
    check(~has_cjk || (ysp_gfx('get', c, 'missing') == 0 && ysp_gfx('get', c, 'n_runs') == 2 && isequal(ysp_gfx('get', c2, 'box'), ysp_gfx('get', c, 'box'))), ...
          'a fallback list: a handle or a cell of paths');
    expect_error('ysp_gfx:error', @() ysp_gfx('font', g, 'C:/no/such/font.ttf'));
    expect_error('ysp_gfx:arg', @() ysp_gfx('font', g, uint8('not a font, only bytes')));

    % ---- spans: [first last r g b size], host indices, last included
    sp = ysp_gfx('text', g, 'one two three', 'size', 30, 'color', 0.8, 'spans', [1 3 1 0 0 0; 9 13 NaN NaN NaN 45]);
    S = ysp_gfx('get', sp, 'spans');
    check(isequal(S(:, 1:2), [1 3; 9 13]) && isequal(S(1, 3:5), [1 0 0]) && all(isnan(S(2, 3:5))) && S(2, 6) == 45, 'spans round trip');
    check(ysp_gfx('get', sp, 'n_runs') == 2, 'two sizes, two runs');
    expect_error('ysp_gfx:arg', @() ysp_gfx('set', sp, 'spans', [1 5 0 0 0 0; 3 8 0 0 0 0]));
    expect_error('ysp_gfx:arg', @() ysp_gfx('set', sp, 'spans', [1 50 0 0 0 0]));
    check(size(ysp_gfx('get', sp, 'spans'), 1) == 2, 'a refused set leaves the spans');
    us = ysp_gfx('text', g, s, 'spans', [strfind(s, '中') strfind(s, '中') + numel('中文') - 1 0 1 0 0]);
    check(size(ysp_gfx('get', us, 'spans'), 1) == 1, 'a span over non-ASCII characters');

    % ---- glyph and word boxes, hits
    r = ysp_gfx('text', g, 'Read this word', 'size', 40, 'x', -50, 'y', 30);
    Gr = ysp_gfx('glyphs', r);
    [Wr, wr] = ysp_gfx('words', r);
    br = ysp_gfx('bounds', r);
    check(isequal(Gr(:, 1)', 1:14) && isequal(wr', {'Read', 'this', 'word'}) && isequal(Wr(:, 1:2), [1 4; 6 9; 11 14]), 'glyph and word indices');
    check(all(Gr(:, 3) >= br(1) - 1e-3 & Gr(:, 5) <= br(3) + 1e-3) && all(Wr(:, 4) >= br(1) - 1e-3 & Wr(:, 6) <= br(3) + 1e-3), 'inside the bounds');
    k = 2;
    hx = (Gr(k, 3) + Gr(k, 5)) / 2;
    hy = (Gr(k, 4) + Gr(k, 6)) / 2;
    check(ysp_gfx('hit', r, hx, hy) == 2 && ysp_gfx('hit', r, 5, 5) == 0, 'hit: the char index, 0 none');
    Lh = ysp_gfx('list', g, [ysp_gfx('shape', g, 'rect', 'w', 800, 'h', 600, 'color', 0.2) r]);
    hh = ysp_gfx('hit', Lh, hx, hy);
    check(isequal(hh, [2 2]), 'a list: [item char]');

    % ---- lists, bulk sets, bindings
    T = [ysp_gfx('text', g, 'red', 'x', -100), ysp_gfx('text', g, 'green', 'x', 0), ysp_gfx('text', g, 'blue', 'x', 100)];
    LT = ysp_gfx('list', g, T);
    ysp_gfx('set', LT, 'y', [1 2 3]);
    ysp_gfx('set', LT, 'color', [0.2 0.4 0.6]);
    check(ysp_gfx('get', T(3), 'y') == 3 && max(abs(ysp_gfx('get', T(2), 'color') - [0.2 0.4 0.6])) < 1e-6, 'bulk sets');
    expect_error('ysp_gfx:arg', @() ysp_gfx('set', LT, 'size', 20));
    tl = ysp_timeline('open', struct('channels', 4));
    ysp_gfx('bind', g, T(1), 'opacity', tl, 1);
    ysp_gfx('bind', g, T(2), 'x', tl, 2);
    expect_error('ysp_gfx:arg', @() ysp_gfx('bind', g, T(1), 'size', tl, 3));
    fr = ysp_screen('begin', h);
    ysp_timeline('set_keys', tl, 1, 1, [0 0.25]);
    ysp_timeline('set_keys', tl, 2, 1, [0 77]);
    ysp_timeline('anchor', tl, 1, fr.onset);
    ysp_timeline('evaluate', tl, fr.onset, fr.period);
    ysp_gfx('frame', g, LT);
    ysp_screen('flip', h);
    check(ysp_gfx('get', T(1), 'opacity') == 0.25 && ysp_gfx('get', T(2), 'x') == 77, 'bindings write texts in begin');
    ysp_gfx('unbind', g, [], [], tl);
    ysp_timeline('close', tl);

    % ---- refusals
    expect_error('ysp_gfx:arg', @() ysp_gfx('text', g, 'x', 'w', 100));
    expect_error('ysp_gfx:arg', @() ysp_gfx('text', g, 'x', 'align', 'left'));
    expect_error('ysp_gfx:arg', @() ysp_gfx('text', g, 'x', 'colour', 1));
    expect_error('ysp_gfx:arg', @() ysp_gfx('copy', t));
    expect_error('ysp_gfx:arg', @() ysp_gfx('instances', g, t, 3));
    expect_error('ysp_gfx:handle', @() ysp_gfx('words', ysp_gfx('shape', g, 'circle', 'w', 10)));
    if ~is_octave
        expect_error('ysp_gfx:arg', @() ysp_gfx('text', g, char(55296)));   % a lone UTF-16 surrogate
    end
    frame(h, g, [t u a p ta tb m c c2 sp us r]);

    % ---- 1024 texts a frame at the default max_draws: a text without a
    % colored span is one block (ysp/gfx.h 0.17.2; 3 before, when the 342nd
    % failed with "pool or frame full"); the 1025th is refused
    TT = zeros(1, 1025, 'uint64');
    for k = 1:1025
        TT(k) = ysp_gfx('text', g, sprintf('word%d', mod(k, 10)), 'size', 12);
    end
    frame(h, g, ysp_gfx('list', g, TT(1:1024)));
    check(true, '1024 texts in a frame');
    LA = ysp_gfx('list', g, TT);
    ysp_screen('begin', h);
    expect_error('ysp_gfx:error', @() ysp_gfx('frame', g, LA));
    ysp_screen('flip', h);

    % ---- deg units
    h2 = ysp_screen('open', 'backend', 'sim');
    g2 = ysp_gfx('open', h2, 'units', 'deg', 'view', [570 400], 'cache', false);
    d = ysp_gfx('text', g2, 'one degree per em');
    dw = ysp_gfx('text', g2, 'wrapped at five degrees of visual angle', 'size', 0.5, 'width', 5);
    db = ysp_gfx('get', dw, 'box');
    check(ysp_gfx('get', d, 'size') == 1 && db(1) == 5 && ysp_gfx('get', dw, 'n_lines') > 1, 'deg units: size and width in degrees');
    ysp_screen('close', h2);

    % ---- lifetime
    expect_error('ysp_gfx:busy', @() ysp_gfx('free', f));
    ysp_gfx('free', ta);
    ysp_gfx('free', f);
    check(true, 'free: a font once no text uses it');
    ysp_gfx('close', g);
    expect_error('ysp_gfx:handle', @() ysp_gfx('get', t, 'text'));
    expect_error('ysp_gfx:handle', @() ysp_gfx('font_info', fl));
    ysp_screen('close', h);
    fprintf('PASS test_mex_vis_text (%d checks)\n', YSP_TEXT_CHECKS);
end

function frame(h, g, items)
    ysp_screen('begin', h);
    ysp_gfx('frame', g, items);
    ysp_screen('flip', h);
end

function check(cond, msg, varargin)
    global YSP_TEXT_CHECKS %#ok<GVMIS>
    YSP_TEXT_CHECKS = YSP_TEXT_CHECKS + 1;
    if ~cond
        error('test_mex_vis_text:fail', ['FAIL: ' msg], varargin{:});
    end
end

function expect_error(id, f)
    global YSP_TEXT_CHECKS %#ok<GVMIS>
    YSP_TEXT_CHECKS = YSP_TEXT_CHECKS + 1;
    try
        f();
    catch e
        if ~strcmp(e.identifier, id)
            error('test_mex_vis_text:fail', 'FAIL: expected %s, got %s: %s', id, e.identifier, e.message);
        end
        return;
    end
    error('test_mex_vis_text:fail', 'FAIL: expected %s, nothing was raised', id);
end
