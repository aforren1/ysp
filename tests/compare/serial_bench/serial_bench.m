function R = serial_bench(varargin)
%SERIAL_BENCH  MATLAB serialport vs Psychtoolbox IOPort vs ysp_serial (MEX).
%
%   serial_bench                                   % no port: clocks, no-port calls
%   serial_bench('port', 'COM3')                   % adapter, nothing attached
%   serial_bench('port', 'COM3', 'device', 'loopback')   % TX jumpered to RX
%   serial_bench('port', 'COM5', 'device', 'echo')       % firmware/ysp_echo board
%
%   Every API that is present runs the same parts with the same parameters;
%   an API that is absent is skipped with a message. README.md in this
%   folder says what each part needs and how to read the output.
%
%   Parts ('parts', default: all that the hardware allows):
%     clock    cost of two back-to-back reads of the API's clock
%     noport   calls that need no port: the cheapest call into the API, port
%              enumeration, and opening a port that does not exist
%     calls    open, close, write (cfg.write_sizes bytes), read with nothing
%              waiting, bytes-available query, read with bytes waiting
%              (the last needs 'loopback' or 'echo')
%     rt       round trips 'e <seq>\n' -> answer, blocking read and poll
%              ('loopback' or 'echo'); with 'echo', the one-way split
%     stream   the board sends cfg.stream_hz lines per second for
%              cfg.stream_s s; missed and coalesced lines, latency against
%              the board's stamps, CPU use ('echo' only)
%
%   Options (name-value; defaults in brackets):
%     port         COM port, '' for none ['']
%     device       'none' | 'loopback' | 'echo' ['none']
%     baud         [115200]
%     apis         {'serialport', 'ioport', 'ioport_tuned', 'ysp'} [all four];
%                  ioport_tuned is IOPort with its latency options (sb_ioport.m)
%     parts        cell of part names [all]
%     ioport_dir   folder put first on the path for IOPort and GetSecs ['']
%     ysp_dir      folder with ysp_serial.<mexext> [bindings/mex of this repo]
%     out          output folder [tempdir/ysp_serial_bench/<date-time>]
%     label        free text for the conditions file, e.g. 'FT232R, 1 ms' ['']
%     n_clock, n_floor, n_enum, n_open_absent, n_open, n_calls, n_blocking,
%     n_read_waiting, n_rt, n_probe, stream_hz, stream_s, max_block_s,
%     write_sizes, absent_port: sample counts and sizes, see the code below.
%
%   Output in cfg.out: summary.csv and summary.txt (one row per API and
%   metric: n, median, p99, max, min, mean in microseconds, or a count),
%   <api>_samples.csv (every sample), <api>_rt_<strategy>.csv and
%   <api>_stream_<mode>.csv (raw stamps), conditions.txt.

    here = fileparts(mfilename('fullpath'));
    cfg = struct( ...
        'port', '', 'device', 'none', 'baud', 115200, ...
        'apis', {{'serialport', 'ioport', 'ioport_tuned', 'ysp'}}, ...
        'parts', {{'clock', 'noport', 'calls', 'rt', 'stream'}}, ...
        'ioport_dir', '', ...
        'ysp_dir', fullfile(here, '..', '..', '..', 'bindings', 'mex'), ...
        'out', '', 'label', '', ...
        'n_clock', 100000, 'n_floor', 100000, 'n_enum', 200, 'n_open_absent', 50, ...
        'n_open', 20, 'n_calls', 2000, 'n_blocking', 200, 'n_read_waiting', 500, ...
        'n_rt', 1000, 'n_probe', 200, 'stream_hz', 1000, 'stream_s', 10, ...
        'max_block_s', 20, 'write_sizes', [1 8 64 512], 'absent_port', 'COM250');
    if mod(numel(varargin), 2) ~= 0
        error('serial_bench:args', 'options come in name-value pairs');
    end
    for i = 1:2:numel(varargin)
        name = varargin{i};
        if ~isfield(cfg, name)
            error('serial_bench:args', 'unknown option ''%s''', name);
        end
        cfg.(name) = varargin{i + 1};
    end
    if ischar(cfg.apis), cfg.apis = {cfg.apis}; end
    if ischar(cfg.parts), cfg.parts = {cfg.parts}; end
    if ~any(strcmp(cfg.device, {'none', 'loopback', 'echo'}))
        error('serial_bench:args', 'device must be ''none'', ''loopback'' or ''echo''');
    end
    if isempty(cfg.out)
        cfg.out = fullfile(tempdir, 'ysp_serial_bench', datestr(now, 'yyyymmdd-HHMMSS')); %#ok<TNOW1,DATST>
    end
    if ~exist(cfg.out, 'dir'), mkdir(cfg.out); end
    if isempty(cfg.port)
        fprintf('serial_bench: no port given: running the clock and noport parts only\n');
    end

    avail = struct();
    for k = 1:numel(cfg.apis)
        [ok, why] = sb_probe_api(cfg, cfg.apis{k});
        avail.(cfg.apis{k}) = ok;
        if ok
            fprintf('serial_bench: %s: %s\n', cfg.apis{k}, why);
        else
            fprintf('serial_bench: SKIPPED %s: %s\n', cfg.apis{k}, why);
        end
    end

    R = struct('api', {}, 'part', {}, 'metric', {}, 'values', {}, 'note', {});
    for k = 1:numel(cfg.apis)
        api = cfg.apis{k};
        if ~avail.(api), continue; end
        fprintf('serial_bench: running %s\n', api);
        n0 = numel(R);
        try
            switch api
                case 'serialport', R = sb_serialport(cfg, R);
                case 'ioport',     R = sb_ioport(cfg, R, false);
                case 'ioport_tuned', R = sb_ioport(cfg, R, true);
                case 'ysp',        R = sb_ysp(cfg, R);
            end
        catch e
            fprintf(2, 'serial_bench: %s stopped: %s\n', api, e.message);
            R = sb_add(R, api, 'error', 'stopped', NaN, e.message);
        end
        sb_write_samples(cfg, api, R(n0 + 1:end));
    end
    sb_write_summary(cfg, R);
    sb_write_conditions(cfg, avail);
    fprintf('serial_bench: results in %s\n', cfg.out);
end

function [ok, why] = sb_probe_api(cfg, api)
    ok = false;
    switch api
        case 'serialport'
            if isempty(which('serialport'))
                why = 'serialport is not on this MATLAB (it needs R2019b or later)';
                return;
            end
            why = sprintf('MATLAB %s', version);
        case {'ioport', 'ioport_tuned'}
            if ~isempty(cfg.ioport_dir), addpath(cfg.ioport_dir, '-begin'); end
            if isempty(which('IOPort'))
                why = 'IOPort is not on the path (install Psychtoolbox or pass ioport_dir)';
                return;
            end
            try
                IOPort('Verbosity');
                GetSecs;
                v = IOPort('Version');
                why = sprintf('IOPort %s from %s', sb_ptb_version(v), fileparts(which('IOPort')));
            catch e
                why = sprintf('IOPort at %s does not load: %s', which('IOPort'), e.message);
                return;
            end
        case 'ysp'
            addpath(cfg.ysp_dir);
            if isempty(which('ysp_serial'))
                why = sprintf('ysp_serial.%s not found in %s: run bindings/mex/build.m', mexext, cfg.ysp_dir);
                return;
            end
            try
                ysp_serial('now');
                why = sprintf('ysp_serial at %s', which('ysp_serial'));
            catch e
                why = e.message;
                return;
            end
        otherwise
            why = 'unknown API';
            return;
    end
    ok = true;
end

function s = sb_ptb_version(v)
    if isstruct(v) && isfield(v, 'version')
        s = v.version;
    elseif ischar(v)
        s = v;
    else
        s = '(version unknown)';
    end
end

function c = sb_clock_name(api)
    switch api
        case 'serialport', c = 'tic/toc';
        case {'ioport', 'ioport_tuned'}, c = 'GetSecs';
        case 'ysp',        c = 'ysp_serial now, in us';
        otherwise,         c = '';
    end
end

function sb_write_samples(cfg, api, Rk)
    fid = fopen(fullfile(cfg.out, [api '_samples.csv']), 'w');
    c = onCleanup(@() fclose(fid));
    fprintf(fid, 'part,metric,value\n');
    for i = 1:numel(Rk)
        v = Rk(i).values;
        for j = 1:numel(v)
            fprintf(fid, '%s,%s,%.10g\n', Rk(i).part, Rk(i).metric, v(j));
        end
    end
end

function sb_write_summary(cfg, R)
    fid = fopen(fullfile(cfg.out, 'summary.csv'), 'w');
    c1 = onCleanup(@() fclose(fid));
    fprintf(fid, 'api,part,metric,clock,n,median,p99,max,min,mean,note\n');
    lines = {sprintf('%-12s %-7s %-34s %6s %10s %10s %10s %10s  %s', ...
                     'api', 'part', 'metric', 'n', 'median', 'p99', 'max', 'mean', 'note')};
    for i = 1:numel(R)
        s = sb_stats(R(i).values);
        note = strrep(R(i).note, '"', '''');
        fprintf(fid, '%s,%s,%s,%s,%d,%.6g,%.6g,%.6g,%.6g,%.6g,"%s"\n', R(i).api, R(i).part, ...
                R(i).metric, sb_clock_name(R(i).api), s.n, s.median, s.p99, s.max, s.min, s.mean, note);
        lines{end + 1} = sprintf('%-12s %-7s %-34s %6d %10.4g %10.4g %10.4g %10.4g  %s', ...
                                 R(i).api, R(i).part, R(i).metric, s.n, s.median, s.p99, s.max, s.mean, note); %#ok<AGROW>
    end
    txt = strjoin(lines, newline);
    fprintf('%s\n', txt);
    fid2 = fopen(fullfile(cfg.out, 'summary.txt'), 'w');
    c2 = onCleanup(@() fclose(fid2));
    fprintf(fid2, 'Times in microseconds unless the note says count or percent.\n%s\n', txt);
end

function sb_write_conditions(cfg, avail)
    fid = fopen(fullfile(cfg.out, 'conditions.txt'), 'w');
    c = onCleanup(@() fclose(fid));
    fprintf(fid, 'date: %s\n', datestr(now, 'yyyy-mm-dd HH:MM:SS')); %#ok<TNOW1,DATST>
    fprintf(fid, 'label: %s\n', cfg.label);
    fprintf(fid, 'MATLAB: %s, %s\n', version, computer);
    if ispc
        [~, cpu] = system('powershell -NoProfile -Command "(Get-CimInstance Win32_Processor).Name"');
        [~, pw] = system('powershell -NoProfile -Command "(Get-CimInstance Win32_Battery).BatteryStatus"');
        [~, os] = system('ver');
        pw = strtrim(pw);
        if strcmp(pw, '2'), pw = 'AC'; elseif strcmp(pw, '1'), pw = 'battery'; end
        fprintf(fid, 'OS: %s\nCPU: %s\npower: %s\n', strtrim(os), strtrim(cpu), pw);
    end
    fprintf(fid, 'port: %s\ndevice: %s\nbaud: %d\n', cfg.port, cfg.device, cfg.baud);
    ftdi = sb_ftdi_latency(cfg.port);
    if isnan(ftdi)
        fprintf(fid, 'FTDI latency timer: not an FTDI port, or not readable\n');
    else
        fprintf(fid, 'FTDI latency timer: %d ms (registry)\n', ftdi);
    end
    names = fieldnames(avail);
    for i = 1:numel(names)
        fprintf(fid, 'api %s: %s\n', names{i}, mat2str(avail.(names{i})));
    end
    if (isfield(avail, 'ioport') && avail.ioport) || (isfield(avail, 'ioport_tuned') && avail.ioport_tuned)
        fprintf(fid, 'IOPort: %s at %s\n', sb_ptb_version(IOPort('Version')), which('IOPort'));
    end
    if isfield(avail, 'ysp') && avail.ysp
        fprintf(fid, 'ysp_serial: %s\n', which('ysp_serial'));
    end
    f = fieldnames(cfg);
    for i = 1:numel(f)
        v = cfg.(f{i});
        if iscell(v), v = strjoin(v, ' '); elseif isnumeric(v), v = mat2str(v); end
        fprintf(fid, 'cfg.%s: %s\n', f{i}, v);
    end
end

function ms = sb_ftdi_latency(port)
% The FTDI VCP driver keeps the latency timer per device in the registry:
% HKLM\SYSTEM\CurrentControlSet\Enum\FTDIBUS\<id>\0000\Device Parameters,
% values PortName and LatencyTimer. Reading it needs no administrator rights.
    ms = NaN;
    if ~ispc || isempty(port), return; end
    [st, out] = system('reg query "HKLM\SYSTEM\CurrentControlSet\Enum\FTDIBUS" /s');
    if st ~= 0, return; end
    lines = splitlines(out);
    name = ''; lat = NaN;
    for i = 1:numel(lines)
        L = strtrim(lines{i});
        if startsWith(L, 'HKEY_')
            if strcmpi(name, port) && ~isnan(lat), ms = lat; return; end
            name = ''; lat = NaN;
        elseif startsWith(L, 'PortName')
            t = regexp(L, 'REG_SZ\s+(\S+)', 'tokens', 'once');
            if ~isempty(t), name = t{1}; end
        elseif startsWith(L, 'LatencyTimer')
            t = regexp(L, 'REG_DWORD\s+0x([0-9a-fA-F]+)', 'tokens', 'once');
            if ~isempty(t), lat = hex2dec(t{1}); end
        end
    end
    if strcmpi(name, port) && ~isnan(lat), ms = lat; end
end
