% example_serial.m - ysp_serial MEX demo (MATLAB / Octave)
%
% Build first:   run build.m   (produces ysp_serial.<mexext>)
% Then run this script with a device name:
%   example_serial('/dev/ttyUSB0')      % Linux
%   example_serial('COM3')              % Windows
%
% See DEVICE NAMES in ysp/serial.h for the naming rules on each platform.

function example_serial(device)

    ports = ysp_serial('list');
    fprintf('%d serial port(s) detected\n', numel(ports));
    for i = 1:numel(ports)
        fprintf('  %s | %s | %04X:%04X | %s\n', ports(i).name, ...
                ports(i).description, ports(i).vid, ports(i).pid, ...
                ports(i).serial_number);
    end

    if nargin < 1
        % Pick the first FTDI FT232R instead of hard-coding a port name.
        % Add serial_number or location when the rig has two of them.
        match = ysp_serial('find', struct('vid', hex2dec('0403'), ...
                                          'pid', hex2dec('6001')));
        if isempty(match)
            error('example_serial:nodevice', 'no device found; pass a device name');
        end
        device = match(1).name;
    end

    code = 42;

    h = ysp_serial('open', device, struct('baud', 115200, 'low_latency', true));
    cleanup = onCleanup(@() ysp_serial('close', h));   %#ok<NASGU>

    info = ysp_serial('info', h);
    fprintf('open %s: %d baud, %d%s%d, low latency %d, async policy %s\n', ...
            info.device, info.baud, info.data_bits, upper(info.parity(1)), ...
            info.stop_bits, info.low_latency, info.async_policy);

    % The box may have sent a boot banner before we opened; drop it.
    ysp_serial('purge', h, 'rx');

    ysp_serial('writebyte', h, code);          % raise a trigger and leave it
    fprintf('sent trigger %d\n', code);

    ysp_serial('pulse', h, code, 0, 0.002);    % blocking 2 ms pulse (seconds)
    fprintf('sent trigger %d (2 ms blocking pulse)\n', code);

    ysp_serial('pulseasync', h, code, 0, 0.002); % non-blocking; worker writes 0
    fprintf('queued async trigger %d\n', code);
    ysp_serial('drain', h);

    % Wait up to 2 s for a response, then poll once without blocking.
    data = ysp_serial('read', h, 64, 2);       % timeout in seconds
    fprintf('response: %s\n', mat2str(data));
    fprintf('still buffered: %d byte(s)\n', ysp_serial('available', h));
end
