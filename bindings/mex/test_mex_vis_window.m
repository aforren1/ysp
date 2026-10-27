function test_mex_vis_window()
%TEST_MEX_VIS_WINDOW  ysp-vis with a real window: local only, not in CI.
%   Opens a 1280 x 720 window with the bundled ANGLE, draws 2 s with the
%   phase A draw path, routes the window calls through the window thread,
%   pauses 6 s without a call (past the 4.1 to 4.6 s after which Windows
%   marks a window that pumps nothing as hung), draws again, then checks the
%   lifetime rule with a window open: a clear of ysp_gfx leaves the window
%   drawing, and the clear of the last thin file closes it. About 15 s. Prints PASS with the
%   number of checks.

    global YSP_VISW_CHECKS %#ok<GVMIS>
    YSP_VISW_CHECKS = 0;
    addpath(fileparts(mfilename('fullpath')));
    clear ysp_screen ysp_gfx ysp_video;
    h = ysp_screen('open', 'windowed', true, 'size', [1280 720]);
    try
        g = ysp_gfx('open', h, 0.25);
        line = ysp_screen('describe', h);
        check(~isempty(strfind(line, '2.1.25848')), 'the bundled ANGLE: %s', line);
        ysp_screen('title', h, 'ysp-vis MEX test');
        ysp_screen('cursor', h, true);
        ysp_screen('grab', h, false);
        frames(h, g, 120, [1 1 1]);
        ysp_screen('wait_flip', h);
        r = ysp_screen('records', h);
        check(numel(r) == 120 && all(ismember([r.path], [1 2 3])) && all(ismember([r.tier], [1 2 3])), ...
              '120 records on a real path');
        fprintf('before the pause: paths %s, tiers %s\n', mat2str(unique([r.path])), mat2str(unique([r.tier])));
        pause(6);                                  % no call: the window thread keeps the window alive
        f = frames(h, g, 60, [0 1 0]);
        ysp_screen('wait_flip', h);
        r = ysp_screen('records', h);
        check(numel(r) == 60 && f.index == 179, '60 frames after the pause');
        fprintf('after the pause: first records [index dropped path tier flags]:\n');
        disp([[r(1:5).index]' [r(1:5).dropped]' [r(1:5).path]' [r(1:5).tier]' [r(1:5).flags]']);
        clear ysp_gfx;
        info = ysp_vis('info');
        check(info.screens == 1 && info.gfx == 1, 'clear ysp_gfx: the window and the gfx stay');
        frames(h, g, 30, [0 0 1]);
        check(true, 'drawing goes on after ysp_gfx loads again');
    catch e
        try, ysp_screen('close', h); catch, end
        rethrow(e);
    end
    clear ysp_screen ysp_gfx ysp_video;            % the last thin file gone: the window closes
    info = ysp_vis('info');
    check(info.users == 0 && info.screens == 0 && info.gfx == 0, 'the last clear closed the window');
    clear ysp_vis;
    fprintf('PASS test_mex_vis_window (%d checks)\n', YSP_VISW_CHECKS);
end

function f = frames(h, g, n, color)
    for i = 1:n
        f = ysp_screen('begin', h);
        ysp_gfx('begin', g);
        ysp_gfx('rect', g, [0 0 200 120], color);
        ysp_gfx('end', g);
        ysp_screen('flip', h);
    end
end

function check(cond, msg, varargin)
    global YSP_VISW_CHECKS %#ok<GVMIS>
    YSP_VISW_CHECKS = YSP_VISW_CHECKS + 1;
    if ~cond
        error('test_mex_vis_window:fail', ['FAIL: ' msg], varargin{:});
    end
end
