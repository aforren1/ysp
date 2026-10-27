function loop_mex(varargin)
%LOOP_MEX  The minimal ysp-vis frame in MATLAB or Octave (tests/probe/vis).
%   loop_mex('seconds', 600, 'windowed', 0, 'idle', 0, 'csv', 'm.csv')
%   Each frame: begin, gfx begin, one rect, gfx end, flip; the frame's
%   completed records kept for the whole run (a preallocated matrix, as a
%   script keeps its data). With idle S the loop calls nothing for S seconds
%   after frame 120 (pause), then goes on. Writes one CSV row per flip
%   record. bindings/mex must be on the path. Shift+Esc ends a run.
    o = struct('seconds', 600, 'windowed', 0, 'idle', 0, 'csv', 'm.csv');
    for i = 1:2:numel(varargin), o.(varargin{i}) = varargin{i + 1}; end
    if o.windowed
        h = ysp_screen('open', 'windowed', true, 'size', [1280 720]);
    else
        h = ysp_screen('open');
    end
    try
        g = ysp_gfx('open', h, 0.5);
        ysp_screen('title', h, 'ysp-vis probe');
        line = ysp_screen('describe', h);
        caps = ysp_screen('caps', h);
        w = caps.mode.w;
        n = ceil((o.seconds + o.idle) * 61) + 200;
        R = zeros(n, 11);              % index target planned onset residual dropped path tier flags draw swap
        k = 0;
        t_end = [];
        while true
            [f, quit] = ysp_screen('begin', h);
            if ~isempty(quit), fprintf('abort 0x%x\n', quit.reasons); break; end
            if isempty(t_end), t_end = f.onset + o.seconds + o.idle; end
            for j = 1:numel(f.done)
                k = k + 1;
                R(k, :) = row(f.done(j));
            end
            if f.onset >= t_end
                ysp_screen('flip', h);
                break;
            end
            x = mod(f.index, 120) / 120 * (w - 100) - (w - 100) / 2;
            ysp_gfx('begin', g);
            ysp_gfx('rect', g, [x 0 100 100], 1.0);
            ysp_gfx('end', g);
            ysp_screen('flip', h);
            if o.idle > 0 && f.index == 119
                pause(o.idle);         % no call into the module
            end
        end
        ysp_screen('wait_flip', h);
        rest = ysp_screen('close', h);
        last = -1;
        if k > 0, last = R(k, 1); end
        for j = 1:numel(rest)
            if rest(j).index > last, k = k + 1; R(k, :) = row(rest(j)); end
        end
    catch e
        % try/catch, not onCleanup: Octave did not run an onCleanup here
        % (docs/av_probe.md, section 4).
        try, ysp_screen('close', h); catch, end
        rethrow(e);
    end
    fid = fopen(o.csv, 'w');
    if exist('OCTAVE_VERSION', 'builtin'), host = 'octave'; else, host = 'matlab'; end
    fprintf(fid, '# %s %s\n', host, line);
    fprintf(fid, 'index,target_ns,planned_ns,onset_ns,residual_ns,dropped,path,tier,flags,draw_ns,swap_ns\n');
    fprintf(fid, '%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n', R(1:k, :)');
    fclose(fid);
    fprintf('%d records -> %s\n', k, o.csv);
end

function v = row(r)
    ph = r.phases * 1e9;
    ph(isnan(ph)) = -1;
    % int64 ns as double: exact below 2^53 ns (104 days of uptime)
    v = [r.index, double(r.target_ns), double(r.planned_ns), double(r.onset_ns), double(r.residual_ns), ...
         r.dropped, r.path, r.tier, r.flags, round(ph(3)), round(ph(5))];
end
