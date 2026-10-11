% example.m - ysp_parallel MEX demo (MATLAB / Octave)
%
% Build first:   run build.m   (produces ysp_parallel.<mexext>)
% Then run this script. On Linux see the BACKENDS section of ysp/parallel.h for ppdev access.

code = 42;

ports = ysp_parallel('list');
fprintf('%d parallel port(s) detected\n', numel(ports));
for i = 1:numel(ports)
    fprintf('  %s (backend %d, base 0x%X)\n', ...
            ports(i).name, ports(i).backend, ports(i).base_addr);
end

% Platform defaults: ppdev /dev/parport0 (Linux), inpout @ LPT1 (Windows).
% For raw x86 I/O:   h = ysp_parallel('open', 'direct', 888);
% For a specific device: h = ysp_parallel('open', '/dev/parport1');
h = ysp_parallel('open');
cleanup = onCleanup(@() ysp_parallel('close', h));   %#ok<NASGU>

ysp_parallel('pulse', h, code, 0.002);     % blocking 2 ms trigger (seconds)
fprintf('sent trigger %d (2 ms blocking pulse)\n', code);

ysp_parallel('pulseasync', h, code, 0.002); % non-blocking; worker drops it
fprintf('queued async trigger %d\n', code);

s = ysp_parallel('status', h);
fprintf('status = %d\n', s);
