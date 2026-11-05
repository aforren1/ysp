function movie_mex(clip, varargin)
%MOVIE_MEX  A movie through the real binding (ysp_video), for the phase B
%   measurements: movie.py's loop and files in MATLAB or Octave.
%
%   movie_mex(clip, 'wav', wav, 'windowed', 0, 'hw', 'auto', 'out', prefix)
%
%   Each display frame: begin, update (timed with tic/toc), the gfx's begin,
%   draw at 1:1, the gfx's end, flip. With 'wav' the soundtrack plays on
%   the default output and the movie clock follows it. Writes
%   PREFIX.frames.csv, PREFIX.flips.csv, PREFIX.update.csv and PREFIX.txt
%   as movie.py does. Shift+Esc ends a run.
    o = struct('wav', '', 'windowed', 0, 'hw', 'auto', 'out', 'movie_mex');
    for k = 1:2:numel(varargin), o.(varargin{k}) = varargin{k + 1}; end
    if o.windowed, opts = {'windowed', true, 'size', [1280 720]}; else, opts = {}; end
    host = 'matlab';
    if exist('OCTAVE_VERSION', 'builtin'), host = 'octave'; end
    a = [];
    if ~isempty(o.wav), a = ysp_audio('open'); end
    h = ysp_screen('open', opts{:});
    lines = {};
    try
        g = ysp_gfx('open', h, 0.45);
        m = ysp_video('open', g, clip, 'hw_decode', o.hw);
        ysp_screen('title', h, 'ysp-vis movie probe');
        lines{end + 1} = [host ' ' ysp_screen('describe', h)];
        lines{end + 1} = ysp_video('describe', m);
        if ~isempty(a)
            ysp_video('soundtrack', m, a, o.wav);
            lines{end + 1} = ysp_audio('describe', a);
        end
        upd = zeros(2, 100000);
        n = 0;
        fl = {};
        ysp_video('play', m);
        while true
            [f, quit] = ysp_screen('begin', h);
            if ~isempty(quit), lines{end + 1} = sprintf('abort 0x%x', quit.reasons); break; end %#ok<AGROW>
            if ~isempty(f.done), fl{end + 1} = f.done; end %#ok<AGROW>
            t0 = tic;
            playing = ysp_video('update', m);
            n = n + 1;
            upd(:, n) = [f.index; toc(t0)];
            ysp_gfx('begin', g);
            ysp_video('draw', m);
            ysp_gfx('end', g);
            ysp_screen('flip', h);
            if ~playing, break; end
        end
        ysp_screen('wait_flip', h);
        rest = ysp_screen('records', h);
        fr = ysp_video('records', m);
        ev = ysp_video('events', m);
        lines{end + 1} = ysp_video('describe', m);
        ysp_video('close', m);
        ysp_screen('close', h);
    catch e
        ysp_screen('close', h);
        if ~isempty(a), ysp_audio('close', a); end
        rethrow(e);
    end
    for k = 1:numel(ev)
        e = ev{k};
        lines{end + 1} = sprintf('event %s at %d ns', e.kind, e.t_ns); %#ok<AGROW>
        if ~isempty(a) && strcmp(e.kind, 'sound') && strcmp(e.action, 'start')
            r = ysp_audio('wait', a, e.play_id, 2.0);
            lines{end + 1} = sprintf('sound onset: target %d ns, onset %d ns, residual %d ns, tier %d, flags 0x%x', ...
                                     r.target_ns, r.onset_ns, r.residual_ns, r.tier, r.flags); %#ok<AGROW>
        end
    end
    if ~isempty(a), ysp_audio('close', a); end

    fid = fopen([o.out '.txt'], 'w');
    fprintf(fid, '%s\n', lines{:});
    fclose(fid);
    fid = fopen([o.out '.frames.csv'], 'w');
    fprintf(fid, 'display,frame,decision,why,flags,onset_ns,due_ns,residual_ns,movie_t_ns,ahead,tier\n');
    dn = {'none', 'shown', 'repeated', 'dropped'};
    wn = {'due', 'cadence', 'display_late', 'decode_late', 'drift', 'seek', 'loop', 'manual'};
    for k = 1:numel(fr)
        r = fr(k);
        fprintf(fid, '%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n', r.display, r.frame, find(strcmp(dn, r.decision)) - 1, ...
                find(strcmp(wn, r.why)) - 1, r.flags, r.onset_ns, r.due_ns, r.residual_ns, r.movie_t_ns, r.ahead, r.tier);
    end
    fclose(fid);
    fid = fopen([o.out '.flips.csv'], 'w');
    fprintf(fid, '# %s\n', lines{1});
    fprintf(fid, 'index,target_ns,planned_ns,onset_ns,residual_ns,dropped,path,tier,flags,draw_ns,swap_ns,upload_ns\n');
    last = -1;
    all = [fl, {rest}];
    for j = 1:numel(all)
        for k = 1:numel(all{j})
            r = all{j}(k);
            if r.index <= last, continue; end
            last = r.index;
            ph = r.phases * 1e9;
            ph(isnan(ph)) = -1;
            fprintf(fid, '%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n', r.index, r.target_ns, r.planned_ns, r.onset_ns, ...
                    r.residual_ns, r.dropped, r.path, r.tier, r.flags, round(ph(3)), round(ph(5)), round(ph(4)));
        end
    end
    fclose(fid);
    fid = fopen([o.out '.update.csv'], 'w');
    fprintf(fid, 'index,update_ns\n');
    fprintf(fid, '%d,%d\n', [upd(1, 1:n); round(upd(2, 1:n) * 1e9)]);
    fclose(fid);
    fprintf('%d frame records -> %s.*\n', numel(fr), o.out);
end
