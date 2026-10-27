% example_video.m - a movie with sound: ysp_video on a ysp_screen, its
% soundtrack on a ysp_audio device, then a summary of the frame records.
%
% It writes its own 3 s movie (a gray gradient and a moving bar, low
% contrast) and a WAV of the same length (a click each second), so it needs
% no clip. For your own clip: an MPEG-1 or MP4 file with its index beside it
% (ysp_video('make_index', clip, '')), and a WAV at the device's rate and
% channel count that lasts as long as the movie, to the sample.
%
% Build first (build.m: ysp_audio, ysp_timeline, ysp_vis, ysp_screen,
% ysp_gfx, ysp_video), then run it from this directory. Windows only.
% Shift+Esc ends it early. For the full screen, open without 'windowed'.
% The audio-video error printed is what the records report (flip onsets
% against the sound's clock), not light against sound.

d = tempname();
mkdir(d);
clip = fullfile(d, 'example.yspseq');
w = 320; hh = 180; n = 90;                         % 3 s at 30 fps
x = 0:w - 1;
fr = zeros(hh, w, 4, n, 'uint8');
for i = 1:n
    v = uint8(112 + floor(x * 32 / w));
    bar = floor((i - 1) * w / n);
    v(x >= bar & x < bar + 8) = 150;
    fr(:, :, 1, i) = repmat(v, hh, 1);
    fr(:, :, 2, i) = fr(:, :, 1, i);
    fr(:, :, 3, i) = fr(:, :, 1, i);
    fr(:, :, 4, i) = 255;
end
ysp_video('write_seq', clip, fr, 30);

a = ysp_audio('open');                             % the default output ('backend', 'null' in a test)
caps = ysp_audio('caps', a);
s = zeros(3 * caps.rate, caps.channels);           % the movie's duration, to the sample
for k = 0:2
    s(k * caps.rate + (1:round(0.005 * caps.rate)), :) = 0.1;   % a 5 ms click each second
end
wav = fullfile(d, 'example.wav');
audiowrite(wav, s, caps.rate);

h = ysp_screen('open', 'windowed', true, 'size', [960 540]);
try
    g = ysp_gfx('open', h, 0.45);
    m = ysp_video('open', g, clip);
    disp(ysp_video('describe', m));
    ysp_video('soundtrack', m, a, wav);            % the movie clock follows the sound
    ysp_video('play', m);
    while true
        [f, quit] = ysp_screen('begin', h);
        if ~isempty(quit), break; end
        playing = ysp_video('update', m);          % the frame due at f.onset
        ysp_gfx('begin', g);
        ysp_video('draw', m);
        ysp_gfx('end', g);
        ysp_screen('flip', h);
        if ~playing, break; end
    end
    ysp_screen('wait_flip', h);
    r = ysp_video('records', m);                   % one per display frame
    info = ysp_video('info', m);
    ysp_video('close', m);                         % gives the audio device back
    ysp_screen('close', h);
catch e
    % try/catch, not onCleanup: Octave did not run an onCleanup here
    % (docs/av_probe.md, section 4).
    ysp_screen('close', h);
    ysp_audio('close', a);
    rethrow(e);
end
ysp_audio('close', a);
rmdir(d, 's');

fprintf('display frames %d; video frames shown %d, repeated %d, dropped %d\n', ...
        numel(r), info.shown, info.repeated, info.dropped);
c = ysp_video('constants');
av = [];
for k = 1:numel(r)
    if bitand(r(k).flags, c.F_AUDIO_CLOCK) && strcmp(r(k).decision, 'shown')
        av(end + 1) = r(k).residual * 1e3; %#ok<SAGROW>
    end
end
if ~isempty(av)
    av = sort(av);
    fprintf('flip onset minus the frame''s time on the audio clock: median %.3f ms, max %.3f ms\n', ...
            av(ceil(end / 2)), av(end));
end
