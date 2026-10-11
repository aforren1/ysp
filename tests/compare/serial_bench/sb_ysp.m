function R = sb_ysp(cfg, R)
%SB_YSP  serial_bench parts for the ysp_serial MEX binding (ysp/serial.h).
%   Clock: ysp_serial('now_us'), which is yrt_now_us(): QueryPerformanceCounter
%   on Windows, CLOCK_MONOTONIC elsewhere, in whole microseconds. 'read'
%   gives no receive stamp of its own; the stamp is now_us after the read
%   returns. The stream part's 'async' mode uses the binding's reader thread
%   ('async_start'), which stamps each line on the same clock when the read
%   that brought its newline returns.
    A = 'ysp';
    clk = @() ysp_serial('now_us');

    if sb_want(cfg, 'clock')
        n = cfg.n_clock;
        d = zeros(n, 1);
        for i = 1:n
            a = ysp_serial('now_us');
            b = ysp_serial('now_us');
            d(i) = b - a;
        end
        R = sb_add(R, A, 'clock', 'clock_pair', d, 'two back-to-back now_us calls; 1 us steps');
        R = sb_add(R, A, 'clock', 'clock_step', min(d(d > 0)), 'smallest nonzero difference');
    end

    if sb_want(cfg, 'noport')
        n = cfg.n_floor;
        a = ysp_serial('now_us');
        for i = 1:n
            ysp_serial('now_us');
        end
        b = ysp_serial('now_us');
        R = sb_add(R, A, 'noport', 'floor_batch_mean', (b - a) / n, ...
                   sprintf('mean of %d now_us calls: the cheapest call into the MEX', n));
        d = zeros(cfg.n_enum, 1);
        for i = 1:cfg.n_enum
            a = ysp_serial('now_us');
            p = ysp_serial('list');
            b = ysp_serial('now_us');
            d(i) = b - a;
        end
        R = sb_add(R, A, 'noport', 'list_ports', d, sprintf('ysp_serial(''list''), %d ports found', numel(p)));
        d = zeros(cfg.n_open_absent, 1);
        for i = 1:cfg.n_open_absent
            a = ysp_serial('now_us');
            try
                h = ysp_serial('open', cfg.absent_port);
                ysp_serial('close', h);
            catch
            end
            b = ysp_serial('now_us');
            d(i) = b - a;
        end
        R = sb_add(R, A, 'noport', 'open_absent', d, ...
                   sprintf('open %s, which does not exist: the error path, with try/catch', cfg.absent_port));
        % ysp_serial and serialport raise a MATLAB error on a failed open;
        % IOPort asked for two outputs returns -1 instead. This is what the
        % raise and catch alone cost, in this clock.
        for i = 1:cfg.n_open_absent
            a = ysp_serial('now_us');
            try
                error('serial_bench:baseline', 'baseline');
            catch
            end
            b = ysp_serial('now_us');
            d(i) = b - a;
        end
        R = sb_add(R, A, 'noport', 'matlab_error_baseline', d, 'try; error(...); catch; end, no port call');
    end

    if isempty(cfg.port), return; end
    opts = struct('baud', cfg.baud);

    if sb_want(cfg, 'calls')
        dop = zeros(cfg.n_open, 1); dcl = dop;
        for i = 1:cfg.n_open
            a = ysp_serial('now_us');
            h = ysp_serial('open', cfg.port, opts);
            b = ysp_serial('now_us');
            ysp_serial('close', h);
            c = ysp_serial('now_us');
            dop(i) = b - a; dcl(i) = c - b;
        end
        R = sb_add(R, A, 'calls', 'open', dop, 'open with baud set, 8N1, DTR and RTS asserted');
        R = sb_add(R, A, 'calls', 'close', dcl, '');
    end

    h = ysp_serial('open', cfg.port, opts);
    guard = onCleanup(@() sb_ysp_close(h));
    ysp_serial('purge', h, 'rx');

    if sb_want(cfg, 'calls')
        for sz = cfg.write_sizes
            data = uint8(repmat('z', 1, sz));   % no newline: the echo board drops it
            [n, pace] = sb_pace(cfg, sz);
            d = zeros(n, 1);
            for i = 1:n
                a = ysp_serial('now_us');
                ysp_serial('write', h, data);
                b = ysp_serial('now_us');
                d(i) = b - a;
                ysp_serial('read', h, 4096, 0);   % a loopback returns the bytes
                while ysp_serial('now_us') < b + pace, end
            end
            R = sb_add(R, A, 'calls', sprintf('write_%d', sz), d, sprintf('gap %.0f us', pace));
        end
        ysp_serial('write', h, uint8(10));
        pause(0.05);
        ysp_serial('purge', h, 'rx');
        n = cfg.n_calls;
        d = zeros(n, 1); e = d;
        for i = 1:n
            a = ysp_serial('now_us');
            ysp_serial('read', h, 64, 0);
            b = ysp_serial('now_us');
            ysp_serial('available', h);
            c = ysp_serial('now_us');
            d(i) = b - a; e(i) = c - b;
        end
        R = sb_add(R, A, 'calls', 'read_empty', d, 'read(h, 64, 0) with nothing waiting');
        R = sb_add(R, A, 'calls', 'available', e, 'available(h)');
        if ~strcmp(cfg.device, 'none')
            m = cfg.n_read_waiting;
            d = nan(m, 1);
            for i = 1:m
                [cmd, L] = sb_cmd(i, cfg.device);
                ysp_serial('write', h, cmd);
                t = ysp_serial('now_us');
                while ysp_serial('available', h) < L && ysp_serial('now_us') - t < 200e3, end
                a = ysp_serial('now_us');
                r = ysp_serial('read', h, L, 0);
                b = ysp_serial('now_us');
                if numel(r) == L, d(i) = b - a; end
            end
            R = sb_add(R, A, 'calls', 'read_waiting', d, sprintf('read(h, %d, 0) with the bytes waiting', L));
        end
    end

    if sb_want(cfg, 'rt') && ~strcmp(cfg.device, 'none')
        for strat = {'block', 'poll'}
            X = sb_ysp_rt(h, cfg, cfg.n_rt, 0, strat{1}, clk);
            note = strat{1};
            R = sb_rt_rows(R, A, note, X, cfg);
        end
    end

    if sb_want(cfg, 'stream') && strcmp(cfg.device, 'echo')
        L = 26;
        nlines = round(cfg.stream_hz * cfg.stream_s);
        for mode = {'line', 'poll', 'async'}
            P1 = sb_ysp_rt(h, cfg, cfg.n_probe, 1e6, 'block', clk);
            if strcmp(mode{1}, 'async')
                % The binding's reader thread stamps each line with the
                % library clock when the read that brought its newline
                % returns; the main thread empties the ring every 0.1 s.
                ysp_serial('async_start', h, 10, nlines + 1000, 64);
                pause(0.05);
                dw = zeros(20, 1);
                for j = 1:20
                    a = ysp_serial('now_us');
                    while ysp_serial('now_us') - a < 1000, end
                    a = ysp_serial('now_us');
                    ysp_serial('write', h, uint8(10));
                    dw(j) = ysp_serial('now_us') - a;
                end
                R = sb_add(R, A, 'stream', 'async_write_call', dw, ...
                           'write(h, newline) while the reader thread waits for a byte');
                a = ysp_serial('now_us');
                ysp_serial('write', h, uint8(sprintf('s %d %d\n', cfg.stream_hz, nlines)));
                c0 = cputime; w0 = ysp_serial('now_us');
                R = sb_add(R, A, 'stream', 'async_cmd_write_call', w0 - a, ...
                           'write(h, stream command) with the reader thread running');
                deadline = w0 + (cfg.stream_s + 2) * 1e6;
                L_ = {}; T_ = []; F_ = [];
                while numel(L_) < nlines && ysp_serial('now_us') < deadline
                    pause(0.1);
                    [l, te, tf] = ysp_serial('async_read', h);
                    L_ = [L_; l]; T_ = [T_; te]; F_ = [F_; tf]; %#ok<AGROW>
                end
                w1 = ysp_serial('now_us'); c1 = cputime;
                st = ysp_serial('async_stop', h);
                ysp_serial('write', h, uint8(sprintf('x\n')));
                pause(0.05);
                ysp_serial('purge', h, 'rx');
                P2 = sb_ysp_rt(h, cfg, cfg.n_probe, 2e6, 'block', clk);
                S.reads = L_; S.host = nan(numel(L_), 1); S.stamp = T_;
                S.probe = sb_cat_probes(P1, P2);
                S.nlines = nlines; S.cpu_s = c1 - c0; S.wall_s = (w1 - w0) / 1e6;
                S.note = sprintf(['async_start reader thread, stamp when the read with the newline returned ' ...
                    '(first-byte stamp differs on %d lines); main thread in pause(0.1), then async_read; ' ...
                    '%d dropped'], sum(F_ ~= T_), st.dropped);
                R = sb_stream_rows(R, A, mode{1}, S, cfg);
                continue;
            end
            reads = cell(nlines + 100, 1); host = nan(nlines + 100, 1); nr = 0; got = 0;
            c0 = cputime; w0 = ysp_serial('now_us');
            ysp_serial('write', h, uint8(sprintf('s %d %d\n', cfg.stream_hz, nlines)));
            deadline = w0 + (cfg.stream_s + 2) * 1e6;
            while got < nlines * L && ysp_serial('now_us') < deadline
                if strcmp(mode{1}, 'line')
                    r = ysp_serial('read', h, L, 200, 'all');
                else
                    r = ysp_serial('read', h, 4096, 0);
                end
                if ~isempty(r)
                    t = ysp_serial('now_us');
                    nr = nr + 1;
                    reads{nr} = r;
                    host(nr) = t;
                    got = got + numel(r);
                end
            end
            w1 = ysp_serial('now_us'); c1 = cputime;
            ysp_serial('write', h, uint8(sprintf('x\n')));
            pause(0.05);
            ysp_serial('purge', h, 'rx');
            P2 = sb_ysp_rt(h, cfg, cfg.n_probe, 2e6, 'block', clk);
            S.reads = reads(1:nr); S.host = host(1:nr); S.stamp = nan(nr, 1);
            S.probe = sb_cat_probes(P1, P2);
            S.nlines = nlines; S.cpu_s = c1 - c0; S.wall_s = (w1 - w0) / 1e6;
            if strcmp(mode{1}, 'line')
                S.note = 'read(h, 26, 200, ''all'') per line';
            else
                S.note = 'tight loop of read(h, 4096, 0)';
            end
            R = sb_stream_rows(R, A, mode{1}, S, cfg);
        end
    end
end

function X = sb_ysp_rt(h, cfg, n, seq0, strat, clk)
% n echo exchanges; seq0 offsets the sequence numbers of probe runs.
    ysp_serial('write', h, uint8(10));      % ends any partial line on the board
    pause(0.05);
    ysp_serial('purge', h, 'rx');
    X.seq = seq0 + (1:n)';
    X.t0 = nan(n, 1); X.t1 = X.t0; X.t2 = X.t0; X.when = X.t0; X.rx = X.t0; X.tx = X.t0;
    X.ok = false(n, 1);
    for i = 1:n
        [cmd, L] = sb_cmd(X.seq(i), cfg.device);
        tw = clk() + rand * 1000;           % leave the USB frame phase
        while clk() < tw, end
        t0 = ysp_serial('now_us');
        ysp_serial('write', h, cmd);
        t1 = ysp_serial('now_us');
        if strcmp(strat, 'block')
            r = ysp_serial('read', h, L, 1000, 'all');
            t2 = ysp_serial('now_us');
        else
            r = zeros(1, 0, 'uint8');
            while numel(r) < L
                r = [r, ysp_serial('read', h, L - numel(r), 0)]; %#ok<AGROW>
                t2 = ysp_serial('now_us');
                if t2 - t0 > 1e6, break; end
            end
        end
        [X.ok(i), X.rx(i), X.tx(i)] = sb_reply(r, X.seq(i), cfg.device);
        X.t0(i) = t0; X.t1(i) = t1; X.t2(i) = t2;
        if ~X.ok(i)
            pause(0.02);
            ysp_serial('purge', h, 'rx');
        end
    end
end

function sb_ysp_close(h)
    try
        ysp_serial('close', h);
    catch
    end
end
