function ptb_movie(movie, out_csv, pixel_format, max_threads, special_flags1, volume, seconds, screen_no)
% ptb_movie - Psychtoolbox's movie playback on Linux, for comparison with
% examples/video/soundtrack.c (ysp/video.h) against the kernel's vblank
% times (../ptb_linux/vbl_log.c). README.md in this folder says how to run it.
%
%   ptb_movie(movie, out_csv, pixel_format, max_threads, special_flags1,
%             volume, seconds, screen_no)
%
% The loop of PTB's PlayMoviesDemo: GetMovieImage blocks until GStreamer
% releases the next frame on the pipeline's clock (the audio sink's when
% the movie has sound), then DrawTexture at 1:1 in the center (as
% yvid_stim's default), then Flip for the next vblank. Priority(MaxPriority)
% as the demos do: SCHED_FIFO under the psychtoolbox group's rtprio grant,
% as video_soundtrack --elevate.
%
% pixel_format 4 (RGBA8) and max_threads -1 are PTB's defaults on x86;
% 6 (I420, converted by a shader) is the manual's advice for
% high-resolution movies. Its other advice, max_threads 0, and every
% explicit count tried (1, 4, 8) hung OpenMovie here, so run.sh keeps -1:
% the codec's own default, which for avdec_h264 is its automatic count.
% The decoder is GStreamer's choice;
% run.sh steers it with GST_PLUGIN_FEATURE_RANK and logs what was made.
% seconds 0 plays to the end.
%
% Writes one line per flip:
%   i,pts,t_get,t_got,vbl,onset,flip_ts,missed
% in GetSecs seconds (CLOCK_REALTIME on Linux): t_get just before
% GetMovieImage, t_got just after, pts the frame's presentation time in the
% movie, vbl/onset/flip_ts/missed as Flip returns them.

if nargin < 3 || isempty(pixel_format), pixel_format = 4; end
if nargin < 4 || isempty(max_threads), max_threads = -1; end
if nargin < 5 || isempty(special_flags1), special_flags1 = 0; end
if nargin < 6 || isempty(volume), volume = 10 ^ (-30 / 20); end
if nargin < 7 || isempty(seconds), seconds = 0; end
if nargin < 8 || isempty(screen_no), screen_no = 0; end
AssertOpenGL;
% Verbosity 4 prints the audio sink actually used when the movie closes.
Screen('Preference', 'Verbosity', 4);
KbName('UnifyKeyNames');
esc = KbName('ESCAPE');
w = Screen('OpenWindow', screen_no, 0);
try
    ifi = Screen('GetFlipInterval', w);
    Priority(MaxPriority(w));
    HideCursor(w);
    t0 = GetSecs;
    [mv, dur, fps, mw, mh] = Screen('OpenMovie', w, movie, 0, 1, special_flags1, pixel_format, max_threads);
    t_open = GetSecs - t0;
    n_max = ceil(dur * fps) + 10;
    rows = zeros(n_max, 8);
    dst = CenterRect([0 0 mw mh], Screen('Rect', w));
    Screen('Flip', w);
    c0 = cputime;
    Screen('PlayMovie', mv, 1, 0, volume);
    i = 0;
    while i < n_max
        t_get = GetSecs;
        [tex, pts] = Screen('GetMovieImage', w, mv, 1);
        t_got = GetSecs;
        if tex <= 0, break; end
        % Filter 0 (nearest), as yvid_stim's default at 1:1.
        Screen('DrawTexture', w, tex, [], dst, 0, 0);
        [vbl, onset, flip_ts, missed] = Screen('Flip', w);
        Screen('Close', tex);
        i = i + 1;
        rows(i, :) = [i, pts, t_get, t_got, vbl, onset, flip_ts, missed];
        if seconds > 0 && pts >= seconds, break; end
        % Polled every 30th frame: KbCheck costs time on the frame thread.
        if mod(i, 30) == 0
            [down, ~, kc] = KbCheck(-1);
            if down && kc(esc), break; end
        end
    end
    dropped = Screen('PlayMovie', mv, 0);
    cpu_s = cputime - c0;
    Screen('CloseMovie', mv);
    Priority(0);
    ShowCursor(w);
    sca;
catch e
    Priority(0);
    sca;
    rethrow(e);
end
rows = rows(1:i, :);
f = fopen(out_csv, 'w');
fprintf(f, '# ifi %.9f fps %.6f dur %.6f w %d h %d open_s %.3f pixel_format %d max_threads %d special_flags1 %d dropped %d cpu_s %.2f priority %d\n', ...
        ifi, fps, dur, mw, mh, t_open, pixel_format, max_threads, special_flags1, dropped, cpu_s, MaxPriority(0));
fprintf(f, 'i,pts,t_get,t_got,vbl,onset,flip_ts,missed\n');
fprintf(f, '%d,%.6f,%.7f,%.7f,%.7f,%.7f,%.7f,%.6f\n', rows');
fclose(f);
end
