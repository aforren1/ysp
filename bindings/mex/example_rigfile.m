% example_rigfile.m - ysp_rigfile MEX demo (MATLAB / Octave)
%
% Build first:   run build.m
%   example_rigfile()          % make a profile, write it to a temporary file, read it back
%   example_rigfile('user')    % print the user's own rig profile (read only)

function example_rigfile(mode)
    if nargin > 0 && strcmp(mode, 'user')
        p = ysp_rigfile('load');
    else
        p = ysp_rigfile('new', 'booth-2');
        ysp_rigfile('bind', p, 'resp', 'line', 'serial:16C0:0483:B1:');
        ysp_rigfile('bind', p, 'trig', 'triggerbox', 'serial:0403:6001:TB0123:');
        ysp_rigfile('set_view', p, 570, 518.4, 1920);
        f = [tempname() '.json'];
        ysp_rigfile('save_file', p, f);
        ysp_rigfile('close', p);
        p = ysp_rigfile('load_file', f);
        delete(f);
    end
    info = ysp_rigfile('info', p);
    fprintf('rig %s, written %s\nhash %s\n', info.rig, info.written, info.hash);
    for i = 1:numel(info.notes), fprintf('note: %s\n', info.notes{i}); end
    if ~isempty(info.view)
        fprintf('view: %g mm, %g mm across %d px: %.4f px/deg\n', info.view.distance_mm, ...
                info.view.width_mm, info.view.width_px, info.ppd);
    end
    for i = 1:numel(info.roles)
        r = info.roles(i);
        src = ysp_rigfile('source', p, r.name);
        fprintf('%-6s %-10s %-28s tier %d\n', r.name, r.family, r.key, src.tier);
    end
    d = ysp_rigfile('desc', p, 'trig');
    fprintf('ysp_device(''open'', ...) gets: role %s, family %d, key %s\n', d.role, d.family, d.key);
    ysp_rigfile('close', p);
end
