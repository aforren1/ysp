function test_mex_pack()
%TEST_MEX_PACK  Checks of the ysp_pack MEX function. Runs in MATLAB and in
%   Octave. Prints PASS with the number of checks, or stops with the error
%   of the first check that failed. Build first (run build.m), then from
%   this directory: test_mex_pack. It reads tests/pack/fixtures/small.ysppak,
%   a pack the pack tool built from tests/pack/fixtures/small/, and damaged
%   copies of it.

    global YSP_PACK_CHECKS %#ok<GVMIS>
    YSP_PACK_CHECKS = 0;
    here = fileparts(mfilename('fullpath'));
    addpath(here);
    fixture = fullfile(here, '..', '..', 'tests', 'pack', 'fixtures', 'small.ysppak');
    sources = fullfile(here, '..', '..', 'tests', 'pack', 'fixtures', 'small');
    [v, hv] = ysp_pack('version');
    fprintf('ysp_pack %s (ysp/pack.h %s)\n', v, hv);
    check(ischar(v) && ischar(hv), 'version');

    % ---- the tool's pack, mapped ----
    h = ysp_pack('open', fixture);
    check(isa(h, 'uint64'), 'open returns a uint64 handle');
    info = ysp_pack('info', h);
    check(strcmp(info.id, 'sha256:f6495cf8d6a581d5db26ea89f76d4cba3498be132e91aad5e65ae4ad288d090f'), 'the pack ID');
    check(info.format == 1 && info.entries == 5 && info.chunks == 0 && info.zip_start == 0, 'info');
    check(strcmp(info.policy, 'use') && strcmp(info.experiment, 'experiment/main.json'), 'policy, experiment');
    check(~isempty(strfind(info.describe, info.id)), 'describe');
    L = ysp_pack('list', h);
    check(isstruct(L) && isequal(size(L), [5 1]), 'list is N x 1');
    check(isequal({L.name}, {'ysp/manifest.json', 'conditions.pstb', 'experiment/main.json', 'readme.txt', 'ysp/chunks'}), 'names in pack order');
    check(isequal([L.index], 1:5), 'indices are 1-based');
    check(isequal({L.kind}, {'manifest', 'table', 'experiment', 'file', 'chunks'}), 'kinds');
    check(all(mod([L.data_off], 4096) == 0) && all(~[L.stream]), 'aligned data');
    check(isa(L(1).xxh64, 'uint64') && isa(L(1).crc32, 'uint32'), 'hash classes');

    readme = read_file(fullfile(sources, 'readme.txt'));
    b = ysp_pack('read', h, 'readme.txt');
    check(isa(b, 'uint8') && iscolumn(b) && isequal(b, readme), 'read: the source bytes');
    check(isequal(ysp_pack('read', h, 'readme.txt', 2, 5), readme(3:7)), 'read a range');
    check(isequal(ysp_pack('read', h, 'readme.txt', 50, Inf), readme(51:end)), 'read to the end');
    check(isempty(ysp_pack('read', h, 'readme.txt', 1000, 4)), 'read past the end');
    check(ysp_pack('has', h, 'readme.txt') && ~ysp_pack('has', h, 'README.TXT'), 'has');
    e = ysp_pack('entry', h, 'conditions.pstb');
    check(e.size == 400 && strcmp(e.kind, 'table') && e.index == 2, 'entry');

    % the manifest and the definition's canonical bytes
    m = jsondecode(ysp_pack('manifest', h));
    check(strcmp(m.format, 'ysp-pack') && m.version == 1 && strcmp(m.experiment.entry, info.experiment), 'manifest');
    d = jsondecode(ysp_pack('definition', h));
    check(strcmp(d.format, 'ysp-experiment') && strcmp(d.note, 'the bindings test fixture'), 'definition');
    for i = 1:numel(m.entries)
        me = m.entries(i);
        if iscell(m.entries), me = m.entries{i}; end
        bytes = ysp_pack('read', h, me.name);
        check(numel(bytes) == me.size, 'size of %s', me.name);
        check(ysp_pack('entry_hash', bytes) == hex64(me.xxh64), 'xxh64 of %s against the manifest', me.name);
    end
    check(ysp_pack('xxh64', uint8('abc')) == hex64('44bc2cf5ad770999'), 'XXH64 known answer');
    check(ysp_pack('xxh64', uint8([])) == hex64('ef46db3751d8e999'), 'XXH64 of nothing');

    ysp_pack('verify', h);
    ysp_pack('verify', h, 'conditions.pstb');
    [ok, msg] = ysp_pack('verify', h);
    check(ok && isempty(msg), 'verify');
    info = ysp_pack('info', h);
    check(info.hashed_bytes >= sum([L.size]) - L(1).size, 'every entry hashed');

    % ---- bad arguments ----
    expect_error('ysp_pack:not_found', @() ysp_pack('read', h, 'nope'), 'no entry ''nope''');
    expect_error('ysp_pack:not_found', @() ysp_pack('read', h, 'README.TXT'), 'case-sensitive');
    expect_error('ysp_pack:arg', @() ysp_pack('read', h, 'readme.txt', -1));
    expect_error('ysp_pack:arg', @() ysp_pack('read', h, 'readme.txt', 1.5));
    expect_error('ysp_pack:arg', @() ysp_pack('read', h, 'readme.txt', 0, -1));
    expect_error('ysp_pack:arg', @() ysp_pack('read', h, 5));
    expect_error('ysp_pack:arg', @() ysp_pack('open', fixture, 'later'));
    expect_error('ysp_pack:arg', @() ysp_pack('open', 42));
    expect_error('ysp_pack:arg', @() ysp_pack('xxh64', 'abc'));
    expect_error('ysp_pack:io', @() ysp_pack('open', fullfile(tempdir, 'no_such_pack.ysppak')), 'cannot open');
    expect_error('ysp_pack:usage', @() ysp_pack('frobnicate', h));
    expect_error('ysp_pack:usage', @() ysp_pack('read', h));
    ysp_pack('close', h);
    expect_error('ysp_pack:handle', @() ysp_pack('read', h, 'readme.txt'));
    expect_error('ysp_pack:handle', @() ysp_pack('close', h));
    expect_error('ysp_pack:handle', @() ysp_pack('info', uint64(123456)));
    check(isequal(b, readme), 'read results outlive close');

    % ---- from memory, the three policies, damage ----
    raw = read_file(fixture);
    h = ysp_pack('open', raw, 'open');
    info = ysp_pack('info', h);
    check(strcmp(info.policy, 'open') && isequal(ysp_pack('read', h, 'readme.txt'), readme), 'open from uint8');
    ysp_pack('close', h);

    bad = raw;
    k = L(4).data_off + 3;                  % a byte of readme.txt (0-based offset)
    bad(k + 1) = bitxor(bad(k + 1), uint8(4));
    h = ysp_pack('open', bad);              % 'use': the open hashes no entry data
    check(isequal(ysp_pack('read', h, 'conditions.pstb'), read_entry(raw, L(2))), 'the other entries read');
    expect_error('ysp_pack:corrupt', @() ysp_pack('read', h, 'readme.txt'), 'readme.txt');
    expect_error('ysp_pack:corrupt', @() ysp_pack('verify', h));
    [ok, msg] = ysp_pack('verify', h, 'readme.txt');
    check(~ok && ~isempty(strfind(msg, 'corrupt')) && ~isempty(strfind(msg, 'readme.txt')), 'verify answers without raising');
    ysp_pack('close', h);
    expect_error('ysp_pack:corrupt', @() ysp_pack('open', bad, 'open'), 'readme.txt');
    h = ysp_pack('open', bad, 'none');
    got = ysp_pack('read', h, 'readme.txt');
    check(isequal(got, bad(L(4).data_off + (1:L(4).size))), 'none: the damage reads through');
    expect_error('ysp_pack:corrupt', @() ysp_pack('verify', h, 'readme.txt'), 'readme.txt');
    [ok, msg] = ysp_pack('verify', h, 'readme.txt');
    check(~ok && ~isempty(strfind(msg, 'readme.txt')), 'none: verify of one entry hashes it');
    ysp_pack('verify', h, 'conditions.pstb');
    info = ysp_pack('info', h);
    ysp_pack('verify', h, 'conditions.pstb');
    info2 = ysp_pack('info', h);
    check(info2.hashed_bytes == info.hashed_bytes, 'none: an entry is hashed once');
    expect_error('ysp_pack:corrupt', @() ysp_pack('verify', h));
    ysp_pack('close', h);

    % truncation: every length in the last 4 KiB (the directory and the end
    % record), every 61st before; each refused at open
    n = numel(raw);
    lens = unique([0:61:n - 4097, n - 4096:n - 1]);
    refused = 0;
    for len = lens
        try
            hh = ysp_pack('open', raw(1:len));
            ysp_pack('close', hh);
        catch err
            refused = refused + strncmp(err.identifier, 'ysp_pack:', 9);
        end
    end
    check(refused == numel(lens), 'every truncation refused (%d of %d)', refused, numel(lens));

    % a truncated file, through the mapping
    tmp = [tempname '.ysppak'];
    write_file(tmp, raw(1:end - 10));
    expect_error('ysp_pack:format', @() ysp_pack('open', tmp));
    delete(tmp);

    % flipped bytes: refused at open, at a read or at verify; never read wrong
    flips = 0;
    names = {L.name};
    for at = 1:7:n
        x = raw;
        x(at) = bitxor(x(at), uint8(1));
        flips = flips + flip_outcome(x, names, raw, L);
    end
    check(flips == numel(1:7:n), 'every flipped byte refused (%d of %d)', flips, numel(1:7:n));

    fprintf('PASS (%d checks)\n', YSP_PACK_CHECKS);
end

function r = flip_outcome(x, names, raw, L)
    % 1 when the damaged pack is refused somewhere, or reads the original bytes
    r = 0;
    try
        h = ysp_pack('open', x);
    catch err
        r = strncmp(err.identifier, 'ysp_pack:', 9);
        return;
    end
    try
        for i = 1:numel(names)
            if ~isequal(ysp_pack('read', h, names{i}), read_entry(raw, L(i)))
                ysp_pack('close', h);
                return;     % read different bytes: r = 0
            end
        end
        ysp_pack('verify', h);
        r = 1;
    catch err
        r = strncmp(err.identifier, 'ysp_pack:', 9);
    end
    ysp_pack('close', h);
end

function b = read_entry(raw, e)
    b = raw(e.data_off + (1:e.size));
end

function v = hex64(s)
    % 16 hex digits to a uint64, exactly (hex2dec of 64 bits rounds)
    v = bitshift(uint64(hex2dec(s(1:8))), 32) + uint64(hex2dec(s(9:16)));
end

function b = read_file(p)
    f = fopen(p, 'rb');
    if f < 0, error('test_mex_pack:fail', 'cannot read %s', p); end
    b = fread(f, Inf, '*uint8');
    fclose(f);
end

function write_file(p, b)
    f = fopen(p, 'wb');
    fwrite(f, b, 'uint8');
    fclose(f);
end

function check(cond, msg, varargin)
    global YSP_PACK_CHECKS %#ok<GVMIS>
    YSP_PACK_CHECKS = YSP_PACK_CHECKS + 1;
    if ~cond
        error('test_mex_pack:fail', ['FAIL: ' msg], varargin{:});
    end
end

function expect_error(id, f, text)
    global YSP_PACK_CHECKS %#ok<GVMIS>
    YSP_PACK_CHECKS = YSP_PACK_CHECKS + 1;
    try
        f();
    catch e
        if ~strcmp(e.identifier, id)
            error('test_mex_pack:fail', 'FAIL: expected %s, got %s: %s', id, e.identifier, e.message);
        end
        if nargin > 2 && isempty(strfind(e.message, text))
            error('test_mex_pack:fail', 'FAIL: %s: the message "%s" does not name "%s"', id, e.message, text);
        end
        return;
    end
    error('test_mex_pack:fail', 'FAIL: expected %s, nothing was raised', id);
end
