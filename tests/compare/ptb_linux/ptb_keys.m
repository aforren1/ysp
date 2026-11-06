function ptb_keys(helper, n, out_csv, log_csv)
% ptb_keys - Psychtoolbox's KbQueue key times on Linux against the write()
% to a uinput keyboard and the kernel's evdev time (uinput_keys.c).
%
%   ptb_keys(helper, n, out_csv, log_csv)
%
% Starts the helper (it makes the keyboard, waits 3 s, then sends n taps
% and writes log_csv), finds the new keyboard among PTB's keyboards, opens a
% KbQueue on it alone and writes one line per press:
%   i,keycode,time
% with time as KbQueue gives it (GetSecs, CLOCK_REALTIME on Linux).

KbName('UnifyKeyNames');
system(sprintf('%s %d 3 %s &', helper, n, log_csv));
idx = [];
t0 = GetSecs;
while isempty(idx) && GetSecs - t0 < 3
    WaitSecs(0.2);
    [ids, names] = GetKeyboardIndices;
    k = find(strcmp(names, 'ysp uinput_keys'));
    if ~isempty(k), idx = ids(k(1)); end
end
if isempty(idx), error('ptb_keys: the uinput keyboard did not appear'); end
KbQueueCreate(idx);
KbQueueStart(idx);
rows = zeros(n, 3);
got = 0;
t_end = GetSecs + 3 + n * 0.06 + 5;
while got < n && GetSecs < t_end
    [ev, ~] = KbEventGet(idx, 0.1);
    if ~isempty(ev) && ev.Pressed
        got = got + 1;
        rows(got, :) = [got - 1, ev.Keycode, ev.Time];
    end
end
KbQueueStop(idx);
KbQueueRelease(idx);
WaitSecs(1);   % the helper writes its log
f = fopen(out_csv, 'w');
fprintf(f, 'i,keycode,time\n');
fprintf(f, '%d,%d,%.7f\n', rows(1:got, :)');
fclose(f);
fprintf('ptb_keys: %d of %d presses, device %d\n', got, n, idx);
end
