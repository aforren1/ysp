function R = sb_ioport(cfg, R, tuned)
%SB_IOPORT  serial_bench parts for Psychtoolbox's IOPort.
%   Clock: GetSecs (QueryPerformanceCounter on Windows), in seconds, scaled to
%   us here. IOPort('Read') returns a receive stamp 'when' in the same clock:
%   for a direct read, GetSecs when the read completed; for the background
%   reader with ReadFilterFlags=4, GetSecs when the reader thread got the
%   first byte of the line. Writes use blocking = 0 in the round trips (the
%   low-latency choice); the call-cost part also times the default blocking
%   = 1, which waits until the driver reports the transmit queue empty.
%
%   tuned = true (api 'ioport_tuned') adds IOPort's latency options. Line
%   numbers are PsychSourceGL/Source/Windows/IOPort/PsychSerialWindowsGlue.c
%   at tag 3.0.19.16; docs/serial.md has the whole audit.
%     BlockingBackgroundRead=1  A blocking 'Read' then skips its wait for
%                    the first byte (lines 1281-1303), which sleeps
%                    PollLatency between ClearCommError polls, and waits in
%                    ReadFile in the kernel instead. PTB's own demos set it.
%     PollLatency=0  Where IOPort still polls (a blocking 'Read' on a
%                    background reader, line 1253), Sleep(0) instead of
%                    Sleep(1): PsychYieldIntervalSeconds clamps any positive
%                    value to 1 ms on Windows (PsychTimeGlue.c:188), so
%                    PsychRTBox's PollLatency=0.0001 is 1 ms there too.
%     HardwareBufferSizes=32768,32768  CMUBox's setting; a hint to the
%                    driver (SetupComm), as ysp/serial.h's 4096.
%   The async mode is the same in both: it already blocks in the reader.
    if nargin < 3, tuned = false; end
    if tuned
        A = 'ioport_tuned';
        extra = ' BlockingBackgroundRead=1 PollLatency=0 HardwareBufferSizes=32768,32768';
    else
        A = 'ioport';
        extra = '';
    end
    clk = @() GetSecs * 1e6;

    if sb_want(cfg, 'clock')
        n = cfg.n_clock;
        d = zeros(n, 1);
        for i = 1:n
            a = GetSecs;
            b = GetSecs;
            d(i) = (b - a) * 1e6;
        end
        R = sb_add(R, A, 'clock', 'clock_pair', d, 'two back-to-back GetSecs calls');
        R = sb_add(R, A, 'clock', 'clock_step', min(d(d > 0)), 'smallest nonzero difference');
    end

    if sb_want(cfg, 'noport')
        n = cfg.n_floor;
        a = GetSecs;
        for i = 1:n
            IOPort('Verbosity');
        end
        b = GetSecs;
        R = sb_add(R, A, 'noport', 'floor_batch_mean', (b - a) * 1e6 / n, ...
                   sprintf('mean of %d IOPort(''Verbosity'') calls: the cheapest call into the MEX', n));
        d = zeros(cfg.n_open_absent, 1);
        old = IOPort('Verbosity', 0);
        for i = 1:cfg.n_open_absent
            a = GetSecs;
            try
                [h, err] = IOPort('OpenSerialPort', cfg.absent_port); %#ok<ASGLU>
                if h >= 0, IOPort('Close', h); end
            catch
            end
            b = GetSecs;
            d(i) = (b - a) * 1e6;
        end
        IOPort('Verbosity', old);
        R = sb_add(R, A, 'noport', 'open_absent', d, ...
                   sprintf('open %s, which does not exist: the error path, with try/catch', cfg.absent_port));
        R = sb_add(R, A, 'noport', 'list_ports', NaN, 'IOPort has no port enumeration');
    end

    if isempty(cfg.port), return; end
    conf = sprintf('BaudRate=%d DataBits=8 Parity=None StopBits=1 FlowControl=None DTR=1 RTS=1 ReceiveTimeout=1.0%s', cfg.baud, extra);

    if sb_want(cfg, 'calls')
        dop = zeros(cfg.n_open, 1); dcl = dop;
        for i = 1:cfg.n_open
            a = GetSecs;
            h = sb_ioport_open(cfg.port, conf);
            b = GetSecs;
            IOPort('Close', h);
            c = GetSecs;
            dop(i) = (b - a) * 1e6; dcl(i) = (c - b) * 1e6;
        end
        R = sb_add(R, A, 'calls', 'open', dop, ['OpenSerialPort ''' conf '''']);
        R = sb_add(R, A, 'calls', 'close', dcl, '');
    end

    h = sb_ioport_open(cfg.port, conf);
    guard = onCleanup(@() sb_ioport_close(h));
    IOPort('Purge', h);

    if sb_want(cfg, 'calls')
        for blocking = [0 1]
            for sz = cfg.write_sizes
                data = uint8(repmat('z', 1, sz));
                [n, pace] = sb_pace(cfg, sz);
                if blocking, n = min(n, cfg.n_blocking); end
                d = zeros(n, 1);
                for i = 1:n
                    a = GetSecs;
                    IOPort('Write', h, data, blocking);
                    b = GetSecs;
                    d(i) = (b - a) * 1e6;
                    IOPort('Read', h);
                    while GetSecs < b + pace / 1e6, end
                end
                if blocking
                    R = sb_add(R, A, 'calls', sprintf('write_%d_blocking', sz), d, ...
                               sprintf('Write(h, data, 1), the default; gap %.0f us', pace));
                else
                    R = sb_add(R, A, 'calls', sprintf('write_%d', sz), d, ...
                               sprintf('Write(h, data, 0); gap %.0f us', pace));
                end
            end
        end
        IOPort('Write', h, uint8(10), 0);
        pause(0.05);
        IOPort('Purge', h);
        n = cfg.n_calls;
        d = zeros(n, 1); e = d;
        for i = 1:n
            a = GetSecs;
            IOPort('Read', h);
            b = GetSecs;
            IOPort('BytesAvailable', h);
            c = GetSecs;
            d(i) = (b - a) * 1e6; e(i) = (c - b) * 1e6;
        end
        R = sb_add(R, A, 'calls', 'read_empty', d, 'Read(h), non-blocking, with nothing waiting');
        R = sb_add(R, A, 'calls', 'available', e, 'BytesAvailable(h)');
        if ~strcmp(cfg.device, 'none')
            m = cfg.n_read_waiting;
            d = nan(m, 1);
            for i = 1:m
                [cmd, L] = sb_cmd(i, cfg.device);
                IOPort('Write', h, cmd, 0);
                t = GetSecs;
                while IOPort('BytesAvailable', h) < L && GetSecs - t < 0.2, end
                a = GetSecs;
                r = IOPort('Read', h, 0, L);
                b = GetSecs;
                if numel(r) == L, d(i) = (b - a) * 1e6; end
            end
            R = sb_add(R, A, 'calls', 'read_waiting', d, sprintf('Read(h, 0, %d) with the bytes waiting', L));
        end
    end

    if sb_want(cfg, 'rt') && ~strcmp(cfg.device, 'none')
        for strat = {'block', 'poll'}
            X = sb_ioport_rt(h, cfg, cfg.n_rt, 0, strat{1}, clk);
            R = sb_rt_rows(R, A, strat{1}, X, cfg);
        end
    end

    if sb_want(cfg, 'stream') && strcmp(cfg.device, 'echo')
        L = 26;
        nlines = round(cfg.stream_hz * cfg.stream_s);
        for mode = {'line', 'poll', 'async'}
            P1 = sb_ioport_rt(h, cfg, cfg.n_probe, 1e6, 'block', clk);
            reads = cell(nlines + 100, 1); host = nan(nlines + 100, 1); stamp = host; nr = 0; got = 0;
            cmd = uint8(sprintf('s %d %d\n', cfg.stream_hz, nlines));
            if strcmp(mode{1}, 'async')
                % One line per quantum, stamped at its first byte by the
                % reader thread; the buffer must hold a whole number of them.
                % The reader thread never checks for a full ring: it wraps
                % and overwrites, and the next 'Read' that finds more than
                % InputBufferSize bytes pending flushes everything and fails
                % (PsychSerialWindowsGlue.c:1322-1335). The first bench run
                % sized it for 4096 of 10000 lines and waited for the end, so
                % it got 0 lines. PTB's DatarecordingFromSerialPortDemo sizes
                % it for an hour; here it holds the whole stream with room,
                % and the main thread empties it every 0.1 s as an
                % experiment loop would.
                IOPort('ConfigureSerialPort', h, sprintf(['InputBufferSize=%d Terminator=10 ' ...
                    'BlockingBackgroundRead=1 ReadFilterFlags=4 StartBackgroundRead=%d'], L * (nlines + 1000), L));
                % A write while the reader thread waits in its ReadFile: the
                % port is a non-overlapped handle, and Windows runs the I/O
                % of such a handle one call at a time. The board ignores an
                % empty line.
                pause(0.05);
                dw = zeros(5, 1);
                for j = 1:5
                    a = GetSecs;
                    while GetSecs - a < 1e-3, end
                    a = GetSecs;
                    IOPort('Write', h, uint8(10), 0);
                    dw(j) = (GetSecs - a) * 1e6;
                end
                R = sb_add(R, A, 'stream', 'async_write_call', dw, ...
                           'Write(h, newline, 0) while the background reader waits for a byte');
                % The stream command waits the same way, so the clock of
                % the receive phase starts when it has been sent.
                a = GetSecs;
                IOPort('Write', h, cmd, 0);
                c0 = cputime; w0 = GetSecs;
                R = sb_add(R, A, 'stream', 'async_cmd_write_call', (w0 - a) * 1e6, ...
                           'Write(h, stream command, 0) with the background reader running');
                nq = 0;
                while got < nlines * L && GetSecs - w0 < cfg.stream_s + 2
                    pause(0.1);
                    while true
                        [r, when, err] = IOPort('Read', h, 0, L);
                        if ~isempty(err)
                            error('serial_bench:ioport', 'async Read: %s', err);
                        end
                        if isempty(r), break; end
                        % A reader-thread read that timed out with no byte
                        % stores a quantum of zeros; it is not data.
                        if all(r == 0), nq = nq + 1; continue; end
                        r = uint8(r(:)');
                        r = r(1:find(r == 10, 1, 'last'));   % drop the zero padding
                        nr = nr + 1;
                        reads{nr} = r;
                        stamp(nr) = when * 1e6;
                        got = got + numel(r);
                    end
                end
                w1 = GetSecs; c1 = cputime;
                IOPort('ConfigureSerialPort', h, 'StopBackgroundRead');
                note = sprintf(['background reader thread, BlockingBackgroundRead=1, ReadFilterFlags=4, ' ...
                    'InputBufferSize for %d lines; main thread in pause(0.1), then Read(h, 0, %d) until empty; ' ...
                    '%d empty quanta'], nlines + 1000, L, nq);
            else
                c0 = cputime; w0 = GetSecs;
                IOPort('Write', h, cmd, 0);
                while got < nlines * L && GetSecs - w0 < cfg.stream_s + 2
                    if strcmp(mode{1}, 'line')
                        [r, when] = IOPort('Read', h, 1, L);
                    else
                        [r, when] = IOPort('Read', h);
                    end
                    if ~isempty(r)
                        t = GetSecs;
                        nr = nr + 1;
                        reads{nr} = uint8(r(:)');
                        host(nr) = t * 1e6;
                        stamp(nr) = when * 1e6;
                        got = got + numel(r);
                    end
                end
                w1 = GetSecs; c1 = cputime;
                if strcmp(mode{1}, 'line')
                    note = 'Read(h, 1, 26) per line (blocking)';
                else
                    note = 'tight loop of Read(h) (non-blocking)';
                end
            end
            IOPort('Write', h, uint8(sprintf('x\n')), 0);
            pause(0.05);
            IOPort('Purge', h);
            P2 = sb_ioport_rt(h, cfg, cfg.n_probe, 2e6, 'block', clk);
            S.reads = reads(1:nr); S.host = host(1:nr); S.stamp = stamp(1:nr);
            S.probe = sb_cat_probes(P1, P2);
            S.nlines = nlines; S.cpu_s = c1 - c0; S.wall_s = w1 - w0;
            S.note = note;
            R = sb_stream_rows(R, A, mode{1}, S, cfg);
        end
    end
end

function h = sb_ioport_open(port, conf)
    [h, err] = IOPort('OpenSerialPort', port, conf);
    if h < 0
        error('serial_bench:ioport', 'IOPort could not open %s: %s', port, err);
    end
end

function X = sb_ioport_rt(h, cfg, n, seq0, strat, clk)
    IOPort('Write', h, uint8(10), 0);
    pause(0.05);
    IOPort('Purge', h);
    X.seq = seq0 + (1:n)';
    X.t0 = nan(n, 1); X.t1 = X.t0; X.t2 = X.t0; X.when = X.t0; X.rx = X.t0; X.tx = X.t0;
    X.ok = false(n, 1);
    for i = 1:n
        [cmd, L] = sb_cmd(X.seq(i), cfg.device);
        tw = clk() + rand * 1000;
        while clk() < tw, end
        t0 = GetSecs;
        IOPort('Write', h, cmd, 0);
        t1 = GetSecs;
        if strcmp(strat, 'block')
            [r, when] = IOPort('Read', h, 1, L);
            t2 = GetSecs;
        else
            r = [];
            while numel(r) < L
                [q, when] = IOPort('Read', h, 0, L - numel(r));
                r = [r, q(:)']; %#ok<AGROW>
                t2 = GetSecs;
                if t2 - t0 > 1, break; end
            end
        end
        [X.ok(i), X.rx(i), X.tx(i)] = sb_reply(uint8(r), X.seq(i), cfg.device);
        X.t0(i) = t0 * 1e6; X.t1(i) = t1 * 1e6; X.t2(i) = t2 * 1e6; X.when(i) = when * 1e6;
        if ~X.ok(i)
            pause(0.02);
            IOPort('Purge', h);
        end
    end
end

function sb_ioport_close(h)
    try
        IOPort('Close', h);
    catch
    end
end
