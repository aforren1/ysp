function ptb_flips(out_csv, n, hold, screen_no)
% ptb_flips - Psychtoolbox's Flip on Linux, for comparison with
% examples/screen/flipstats.c (ysp/screen.h, GLX_OML) against the kernel's
% vblank times (vbl_log.c). README.md in this folder says how to run both.
%
%   ptb_flips(out_csv, n, hold, screen_no)
%
% n flips of a full-screen gray window. hold 0: each Flip for the next
% vblank. hold 1: each Flip asks for the vblank 1 to 5 frames after the
% last one, with the manual's idiom when = vbl + (k - 0.5) * ifi, as
% flipstats --hold asks for the predicted vblank plus 0 to 4 frames.
% Priority(MaxPriority(w)) as the PTB demos do: SCHED_FIFO under the
% psychtoolbox group's rtprio grant, as flipstats' frame thread.
%
% Writes one line per flip:
%   i,k,t_call,when,vbl,onset,flip_ts,missed
% in GetSecs seconds (CLOCK_REALTIME on Linux): t_call just before Flip,
% when the time asked for (0 = next vblank), vbl/onset/flip_ts/missed as
% Flip returns them, k the frames ahead asked for (1 = the next vblank).

if nargin < 2, n = 600; end
if nargin < 3, hold = 0; end
if nargin < 4, screen_no = 0; end
AssertOpenGL;
Screen('Preference', 'Verbosity', 3);
rand('seed', 1); %#ok<RAND>
t_open = GetSecs;
w = Screen('OpenWindow', screen_no, 64);
t_opened = GetSecs;
ifi = Screen('GetFlipInterval', w);
Priority(MaxPriority(w));
HideCursor(w);
rows = zeros(n, 8);
vbl = Screen('Flip', w);
for i = 1:n
    Screen('FillRect', w, 64);
    if hold
        k = 1 + floor(rand * 5);
        when = vbl + (k - 0.5) * ifi;
    else
        k = 1;
        when = 0;
    end
    t_call = GetSecs;
    [vbl, onset, flip_ts, missed] = Screen('Flip', w, when);
    rows(i, :) = [i, k, t_call, when, vbl, onset, flip_ts, missed];
end
Priority(0);
ShowCursor(w);
sca;
f = fopen(out_csv, 'w');
fprintf(f, '# ifi %.9f open_s %.3f priority %d\n', ifi, t_opened - t_open, MaxPriority(0));
fprintf(f, 'i,k,t_call,when,vbl,onset,flip_ts,missed\n');
fprintf(f, '%d,%d,%.7f,%.7f,%.7f,%.7f,%.7f,%.6f\n', rows');
fclose(f);
end
