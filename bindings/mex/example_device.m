% example_device.m - ysp_device and ysp_rigfile MEX demo (MATLAB / Octave)
%
% Build first:   run build.m   (produces ysp_device and ysp_rigfile)
%   example_device()           % simulated boards: a response box, a trigger box
%   example_device('profile')  % the roles 'resp' and 'trig' of the user's rig profile
%
% A response rule sends the button's code from the box's reader thread at
% the press, not from this loop a frame later. Every time is seconds on
% ysp_device('now').

function example_device(mode)
    if nargin < 1, mode = 'sim'; end
    simulated = ~strcmp(mode, 'profile');
    if simulated
        box_sim = ysp_device('sim_open', 'line');
        trig_sim = ysp_device('sim_open', 'triggerbox');
        trig = ysp_device('open', struct('role', 'trig', 'family', 'triggerbox', ...
                                         'key', ysp_device('sim_key', trig_sim), 'sim', trig_sim, 'pulse', 0.005));
        box_desc = struct('role', 'resp', 'family', 'line', 'key', ysp_device('sim_key', box_sim), 'sim', box_sim);
    else
        p = ysp_rigfile('load');
        info = ysp_rigfile('info', p);
        fprintf('rig %s, profile hash %s\n', info.rig, info.hash);
        for i = 1:numel(info.notes), fprintf('note: %s\n', info.notes{i}); end
        trig = ysp_device('open', ysp_rigfile('desc', p, 'trig'));
        box_desc = ysp_rigfile('desc', p, 'resp');
    end
    if ~ysp_device('wait_running', trig, 3), error('trig: %s', ysp_device('error', trig)); end
    rule = ysp_device('rule_open', trig, struct('kind', {5, 5}, 'control', {1, 2}, 'code', {101, 102}));
    box_desc.rule = rule;
    box = ysp_device('open', box_desc);
    if ~ysp_device('wait_running', box, 3), error('resp: %s', ysp_device('error', box)); end
    if ~simulated
        src = ysp_rigfile('source', p, ysp_device('info', box));
        fprintf('resp: tier %d (%s)\n', src.tier, src.note);
    end

    for trial = 1:3
        onset = ysp_device('now') + 0.05;
        ysp_device('pulse', trig, 10 + trial, 0.002);           % the stimulus code
        ysp_device('rule_arm', rule, onset + 0.1, onset + 1.5); % [min rt, deadline)
        if simulated
            ysp_device('sim_press', box_sim, onset + 0.2 + 0.1 * trial, 0.08, 1 + mod(trial, 2));
        end
        ev = ysp_device('wait', box, 2.0);
        if isempty(ev), fprintf('trial %d: no response\n', trial); continue; end
        ri = ysp_device('rule_info', rule);
        fprintf('trial %d: button %d, rt %.4f s; code %d written %.1f us after the press\n', trial, ...
                ev(1).control, ev(1).t - onset, ri.code, (ri.t_before - ri.t_event) * 1e6);
        ysp_device('rule_disarm', rule);
        pause(0.1);
        ysp_device('clear', box);
    end
    st = ysp_device('stats', box);
    fprintf('box: %d events, %d timer queries answered, %d dropped\n', st.events, st.answers, st.queue_dropped);
    % the input first, then the rule, then the output
    ysp_device('close', box);
    ysp_device('rule_close', rule);
    ysp_device('close', trig);
    if simulated
        w = ysp_device('sim_writes', trig_sim);
        fprintf('trig wrote: %s\n', sprintf('%02x ', [w.data]));
        ysp_device('sim_close', box_sim);
        ysp_device('sim_close', trig_sim);
    else
        ysp_rigfile('close', p);
    end
end
