% example_box.m - ysp_box MEX demo (MATLAB / Octave)
%
% Decode a line board's bytes, read in chunks, as an analysis decodes a raw
% capture of a session. No device is needed: the bytes are made here, split
% where a serial read could split them. Build first (run build.m).

reads = {                         % read time (s), the bytes of that read
    10.000, sprintf('I ysp-line 1 teensy41 left\nS 1000000\n');
    10.050, sprintf('E 1049000 1 1\nE 10');      % a line split across two reads
    10.070, sprintf('62000 1 0\nQ 7 1070500\n');
    10.100, sprintf('S 1100000\nO 1100200 5\n')};

c = ysp_box('constants');
h = ysp_box('open', 'line', c.KIND_BOX, 1);
pairs = zeros(0, 3);
for k = 1:size(reads, 1)
    [ev, pr, info] = ysp_box('decode', h, uint8(reads{k, 2}), reads{k, 1});
    for e = ev'
        fprintf('read at %.3f s: type %d of channel %d at board tick %d\n', e.t, e.type, e.control, e.ticks);
    end
    for j = 1:numel(info.outputs.code)
        fprintf('the board set its outputs to %d at tick %d\n', info.outputs.code(j), info.outputs.ticks(j));
    end
    for j = 1:numel(info.text)
        fprintf('identity: %s\n', info.text{j});
    end
    pairs = [pairs; repmat(reads{k, 1}, numel(pr.probe), 1), double(pr.ticks), pr.probe]; %#ok<AGROW>
end
disp('clock pairs (read time, board ticks, probe):');
disp(pairs);
s = ysp_box('stats', h);
f = ysp_box('family', 'line');
fprintf('one tick is %g s; %d frames, %d garbage bytes\n', f.tick, s.frames, s.garbage);
ysp_box('close', h);

% The bytes a host writes: a trigger code on a Brain Products TriggerBox
% and a 2 ms pulse that a line board times itself.
disp(ysp_box('encode', 'triggerbox', 'set', 12));
disp(char(ysp_box('encode', 'line', 'pulse', 5, 0.002)));
