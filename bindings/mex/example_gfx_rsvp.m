% example_gfx_rsvp.m - a rapid serial visual presentation (RSVP) stream
% with ysp_gfx: 20 items at 10 per second; press Space at the vertical
% grating. The items are 64 x 64 patches side by side in one texture (an
% atlas); one image shows one patch at a time through its source
% rectangle. A ysp_timeline track steps the rectangle's x to the next patch
% every 0.1 s (STEP keys), bound to the image's src_x, so the stream runs
% without a MATLAB call per item or per frame. The flip records then give
% the measured onset of each item.
%
% Build first (build.m), then run it from this directory. Windows only.
% For the simulated display (no window): SIM = true; then run it.

if ~exist('SIM', 'var'), SIM = false; end
N = 20; SIDE = 64; SOA = 0.1; SPACE = 32;    % SDL keycode of Space
target = randi([9 16]);
[xx, yy] = meshgrid((0:SIDE - 1) - SIDE / 2 + 0.5);
ap = hypot(xx, yy) < SIDE / 2 - 1;
atlas = zeros(SIDE, N * SIDE);
for i = 1:N
    ori = 0;
    if i ~= target, ori = 20 + 140 * rand(); end   % degrees from vertical
    gr = cos(2 * pi * (xx * cosd(ori) + yy * sind(ori)) / 8);
    p = 0.5 * ones(SIDE);
    p(ap) = 0.5 + 0.4 * gr(ap);
    atlas(:, (i - 1) * SIDE + (1:SIDE)) = p;
end
if SIM
    h = ysp_screen('open', 'backend', 'sim', 'gfx', struct('background', 0.5));
else
    h = ysp_screen('open', 'windowed', true, 'size', [1280 720], 'gfx', struct('background', 0.5));
end
try
    g = ysp_gfx('open', h);
    c = ysp_screen('constants');
    tex = ysp_gfx('texture', g, uint8(round(repmat(atlas, 1, 1, 3) * 255)));   % gray, linear values
    item = ysp_gfx('image', g, tex, 'src', [0 0 SIDE SIDE], 'w', 4 * SIDE, 'h', 4 * SIDE);
    fix = ysp_gfx('shape', g, 'circle', 'w', 6, 'color', 0);
    scene = ysp_gfx('list', g, [item fix]);
    tl = ysp_timeline('open', struct('channels', 2));      % 1 src_x, 2 on
    ysp_timeline('set_keys', tl, 1, 1, [(0:N - 1)' * SOA, (0:N - 1)' * SIDE, ones(N, 1) * 1]);   % ease 1: step
    ysp_timeline('set_keys', tl, 2, 1, [0 1 1; N * SOA 0 1]);
    ysp_gfx('bind', g, item, 'src_x', tl, 1);
    ysp_gfx('bind', g, item, 'visible', tl, 2);
    ysp_gfx('prime', g);
    f = ysp_screen('begin', h);
    start = f.onset + 0.5;
    ysp_timeline('anchor', tl, 1, start);
    R = []; presses = [];
    while f.onset < start + N * SOA + 0.5
        ysp_timeline('evaluate', tl, f.onset, f.period);
        ysp_gfx('frame', g, scene);
        ysp_screen('flip', h);
        [f, quit, done] = ysp_screen('begin', h);
        if ~isempty(quit), error('example:abort', 'aborted'); end
        R = [R done]; %#ok<AGROW>
        e = ysp_screen('poll', h);
        for k = 1:numel(e)
            if e(k).kind == c.KIND_KEYBOARD && e(k).type == c.PRESS && e(k).code == SPACE, presses(end + 1) = e(k).t; end %#ok<AGROW>
        end
    end
    ysp_gfx('frame', g, scene);
    ysp_screen('flip', h);
    ysp_screen('wait_flip', h);
    R = [R ysp_screen('records', h)];
    ysp_screen('close', h);
    ysp_timeline('close', tl);
catch err
    try, ysp_screen('close', h); catch, end
    rethrow(err);
end
t = [R.target];
shown = R(t >= start - 0.002 & t < start + N * SOA);
k = floor(([shown.target] - start + 0.002) / SOA) + 1;
[~, first] = unique(k);                      % each item's first frame
on = [shown(first).onset];
fprintf('%d items, target at %d; measured SOA %.4f to %.4f s; %d missed frames\n', numel(first), target, ...
        min(diff(on)), max(diff(on)), sum([shown.missed]));
if ~isempty(presses) && numel(on) >= target
    fprintf('Space %.3f s after the target''s onset\n', presses(1) - on(target));
end
