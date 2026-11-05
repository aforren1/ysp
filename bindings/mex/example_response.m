%EXAMPLE_RESPONSE  ysp_response on a scripted participant: a keyboard trial
%   as a Psychtoolbox frame loop would run it, a response box, and an
%   offline rescore. No window and no keyboard: the presses are scripted.
%   Every time is in seconds on one clock; in Psychtoolbox that is GetSecs,
%   the clock of Screen('Flip') and of KbQueueGetEvent.
%
%   With Psychtoolbox, either give the collector key names (KbName's names
%   differ: 'LeftArrow' is 'left' here), or use Psychtoolbox's key codes on
%   both sides: a choice struct('name', 'left', 'control', KbName('f')) and
%   events with control = the event's Keycode. The collector compares the
%   numbers only.

here = fileparts(mfilename('fullpath'));
addpath(here);
K = ysp_response('constants');
frame = 1 / 60;

% 1. A keyboard trial: F or J, 1.5 s from the onset, 0.1 s minimum RT.
h = ysp_response('open', struct('choices', {{'f', 'j'}}, 'duration', 1.5, 'min_rt', 0.1));
% the script: [time down/up keyname], in the shape KbQueueGetEvent gives
script = {10.43, true, 'j'; 10.48, false, 'j'; 10.95, true, 'j'; 11.03, false, 'j'};
t_fix = 10.0;
t_stim = 10.4;
ysp_response('arm', h, t_fix);                 % at the fixation's flip
t = t_fix;
while true
    t = t + frame;                             % t = Screen('Flip', win)
    if abs(t - t_stim) < frame / 2
        ysp_response('set_onset', h, t, struct('src', 'flip'));
    end
    while ~isempty(script) && script{1, 1} <= t   % evt = KbQueueGetEvent
        ysp_response('key', h, script{1, 1}, script{1, 3}, script{1, 2});
        script(1, :) = [];
    end
    if ysp_response('update', h, t) == K.ENDED
        break;
    end
end
% the feedback and the inter-trial interval: keep feeding until the next
% arm, so the response's release still pairs, then finish
for i = 1:size(script, 1)
    ysp_response('key', h, script{i, 1}, script{i, 3}, script{i, 2});
end
[r, row] = ysp_response('finish', h);
fprintf('keyboard: response %s, rt %.3f s, held %.3f s, %d anticipation(s)\n', ...
        r.response, r.rt, r.rt_key_duration, r.n_anticipations);
disp(row);
ysp_response('close', h);

% 2. A response box: one 'feed' per frame with every event the reader
%    queued (a struct array; ysp_box and ysp_device give the same fields).
b = struct('name', {'left', 'right'}, 'kind', 'box', 'control', {1, 2});
src = struct('kind', 'box', 'tier', 1, 'lo', -2e-4, 'hi', 2e-4, 'note', 'device clock fit, loopback checked');
h = ysp_response('open', struct('choices', b, 'duration', 2, 'sources', src));
ysp_response('arm', h, 20);
ysp_response('set_onset', h, 20.25, struct('src', 'flip', 'tier', 1));
ev = struct('t', {20.6123, 20.689}, 'kind', K.KIND_BOX, 'type', {K.PRESS, K.RELEASE}, 'control', 2);
ysp_response('feed', h, ev);
r = ysp_response('finish', h);
fprintf('box: response %s, rt %.4f s, stamp tier %d, bounds %+.4f..%+.4f s\n', ...
        r.response, r.rt, r.stamp_tier, r.stamp_lo, r.stamp_hi);
ysp_response('close', h);

% 3. Offline: one logged trial scored again under other rules.
lg = struct('t', {30.43, 30.47, 30.62, 30.70}, 'type', {1, 2, 1, 2}, 'control', {'f', 'f', 'j', 'j'});
for min_rt = [0.02 0.1 0.25]
    h = ysp_response('open', struct('choices', {{'f', 'j'}}, 'duration', 1.5, 'min_rt', min_rt));
    r = ysp_response('score', h, lg, 30.4, struct('arm', 30.0));
    fprintf('rescore with min_rt %.2f s: response ''%s'', rt %.3f s, %d anticipation(s)\n', ...
            min_rt, r.response, r.rt, r.n_anticipations);
    ysp_response('close', h);
end
