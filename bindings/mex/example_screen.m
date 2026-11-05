% example_screen.m - a frame loop with ysp-vis: a square that steps across
% the screen for 3 s, then a summary of the flip records.
%
% Build first (build.m: ysp_audio, ysp_timeline, ysp_vis, ysp_screen,
% ysp_gfx, ysp_video), then run it from this directory. Windows only.
% Shift+Esc ends it early. For the full screen, open without 'windowed'.

h = ysp_screen('open', 'windowed', true, 'size', [1280 720]);
try
    g = ysp_gfx('open', h, 0.5);                 % background: mid gray
    ysp_screen('title', h, 'ysp-vis example');   % through the window thread
    disp(ysp_screen('describe', h));
    caps = ysp_screen('caps', h);
    w = caps.mode.w;
    t_end = [];
    while true
        [f, quit] = ysp_screen('begin', h);      % f.onset: when this frame shows
        if ~isempty(quit)
            fprintf('aborted: reasons 0x%x\n', quit.reasons);
            break;
        end
        if isempty(t_end), t_end = f.onset + 3; end
        if f.onset >= t_end
            ysp_screen('flip', h);
            break;
        end
        x = ((f.onset - (t_end - 3)) / 3 - 0.5) * (w - 100);
        ysp_gfx('begin', g);
        ysp_gfx('rect', g, [x 0 100 100], [1 1 1]);   % center from the screen's center, pixels
        ysp_gfx('end', g);
        ysp_screen('flip', h);                   % presents at f.onset
    end
    ysp_screen('wait_flip', h);
    r = ysp_screen('records', h);                % every flip since open, oldest first
    ysp_screen('close', h);                      % the gfx closes with its screen
catch e
    % try/catch, not onCleanup: Octave did not run an onCleanup here
    % (docs/av_probe.md, section 4).
    ysp_screen('close', h);
    rethrow(e);
end
res = abs([r.residual]) * 1e6;
fprintf('%d records, %d missed, %d late targets, residual median %.1f us, max %.1f us\n', ...
        numel(r), sum([r.missed]), sum([r.late]), median(res), max(res));
fprintf('paths: %s (3 = independent flip), tiers: %s\n', mat2str(unique([r.path])), mat2str(unique([r.tier])));
