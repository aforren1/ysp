% example_pack.m - ysp_pack MEX demo (MATLAB / Octave)
%
% An analysis script that opens a ysp pack, lists it, reads its experiment
% definition and a table, and checks every entry before it uses the data.
% Build first (run build.m), then run this script from this directory. It
% reads the repository's test fixture; set PACK to read another pack.

if ~exist('PACK', 'var')
    PACK = fullfile('..', '..', 'tests', 'pack', 'fixtures', 'small.ysppak');
end

h = ysp_pack('open', PACK);              % mapped read-only; 'use': each entry hashed on first read
info = ysp_pack('info', h);
fprintf('%s\n', info.describe);

L = ysp_pack('list', h);
for i = 1:numel(L)
    fprintf('  %-24s %-10s %8d bytes\n', L(i).name, L(i).kind, L(i).size);
end

if ~isempty(info.experiment)
    def = jsondecode(ysp_pack('definition', h));    % the definition's canonical bytes
    fprintf('experiment %s: format %s, version %d\n', info.experiment, def.format, def.version);
end

[ok, msg] = ysp_pack('verify', h);       % every entry now; the message names a bad one
if ~ok
    ysp_pack('close', h);
    error('the pack is damaged: %s', msg);
end

t = ysp_pack('read', h, 'conditions.pstb');    % uint8 column; ysp/table.h's PSTB block
fprintf('conditions.pstb: %d bytes, magic %s\n', numel(t), char(t(1:4)'));
m = jsondecode(ysp_pack('manifest', h));
fprintf('built by %s %s; pack ID %s\n', m.tool.name, m.tool.version, info.id);
ysp_pack('close', h);
