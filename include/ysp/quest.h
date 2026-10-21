/* ysp/quest.h - v0.7.0 - public domain single-header Bayesian adaptive library
 *
 *   Parametric Bayesian adaptive estimation on a grid: QUEST+ (Watson 2017),
 *   which contains QUEST (Watson & Pelli 1983), the Psi method (Kontsevich
 *   & Tyler 1999) and Psi-marginal (Prins 2013) as configurations. Any
 *   number of stimulus dimensions, any number of parameters, any number of
 *   outcomes, a built-in catalog of psychometric functions and a callback
 *   for the rest (a CSF model, a matching task, a three-alternative task).
 *
 *   Written in the single-header style of the stb / sokol libraries. Pure
 *   computation: no OS calls, no threads. One allocation at open, none
 *   after. Needs nothing but libm, and includes nothing but the C standard
 *   library. Define YQST_ASYNC and it also gets an opt-in layer that runs the
 *   inference on a background thread, which is the one thing here that needs
 *   ysp/rt.h and an OS; see ASYNC.
 *
 *   Targets every platform the compiler does. C99 is the floor: it builds as
 *   C99, C11 and C++17, and in the C dialect MSVC compiles by default.
 *
 *   ---------------------------------------------------------------------
 *   USAGE
 *   ---------------------------------------------------------------------
 *   Do this:
 *
 *       #define YSP_QUEST_IMPLEMENTATION
 *
 *   in *one* C or C++ file before including this header to create the
 *   implementation. Every other file just includes the header normally.
 *
 *   A 2AFC contrast threshold with a Weibull in log10 contrast (this is the
 *   Psi method: threshold and slope free, guess and lapse fixed):
 *
 *       #define YSP_QUEST_IMPLEMENTATION
 *       #include "ysp/quest.h"
 *
 *       yqst_desc d = { 0 };                          // unset fields are 0
 *       d.stim[0]  = yqst_linspace(-3.0, 0.0, 31);    // log10 contrast
 *       d.n_stim   = 1;
 *       d.param[0] = yqst_linspace(-3.0, 0.0, 61);    // threshold (log10)
 *       d.param[1] = yqst_linspace( 0.5, 6.0, 12);    // slope
 *       d.param[2] = yqst_fixed(0.5);                 // guess: 2AFC
 *       d.param[3] = yqst_fixed(0.02);                // lapse
 *       d.n_param  = 4;
 *       d.pf       = YQST_PF_GUMBEL;                  // Weibull in log units
 *       d.stop_trials = 60;
 *
 *       static yqst_quest q;                          // about 100 KB of history
 *       if (!yqst_open(&q, &d)) { fputs(yqst_error(&q), stderr); return 1; }
 *
 *       while (!yqst_done(&q)) {
 *           int    si = yqst_next(&q);              // stimulus grid index
 *           if (si < 0) { fputs(yqst_error(&q), stderr); break; }
 *           double x  = yqst_stim_value(&q, si, 0); // its log10 contrast
 *           int correct = run_trial(pow(10.0, x));  // 1 = correct, 0 = not
 *           if (yqst_update(&q, si, correct) < 0) { fputs(yqst_error(&q), stderr); break; }
 *       }
 *       double est[4];
 *       yqst_estimate(&q, YQST_EST_MEAN, est);      // est[0] = threshold
 *       yqst_close(&q);
 *
 *   Classic QUEST (Watson & Pelli 1983) is the same with param[1] fixed at
 *   3.5, param[3] fixed at 0.01, a Gaussian prior on param[0]
 *   (yqst_desc.param[0].prior) and desc.select = YQST_SELECT_QUANTILE.
 *   Psi-marginal is the same with param[2] and param[3] given a few grid
 *   points each and flagged nuisance. A CSF or any other model is
 *   YQST_PF_CUSTOM with desc.pf_fn; see PSYCHOMETRIC FUNCTIONS. So is any
 *   task with more than two outcomes: a "less / same / more" judgment
 *   with two criteria, a 4AFC scored by which interval was chosen, a
 *   categorical response on a circular stimulus (mQUESTPlus's
 *   qpPFCircular). The custom function fills K probabilities per cell and
 *   the rest of the header does not care what K is.
 *
 *   ---------------------------------------------------------------------
 *   MODEL
 *   ---------------------------------------------------------------------
 *   Everything lives on grids. The STIMULUS grid is the Cartesian product
 *   of n_stim axes (S points); the PARAMETER grid is the product of n_param
 *   axes (P points); a trial has one of K OUTCOMES. The psychometric
 *   function gives p(outcome | stimulus, parameters) for every cell, and
 *   Bayes' rule keeps a posterior over the P parameter points. Before each
 *   trial the header picks the stimulus whose outcome is expected to leave
 *   the posterior with the least entropy (Watson 2017, eq. 8-11), or the
 *   stimulus a QUEST-style placement rule asks for. This is exactly what
 *   mQUESTPlus, questplus (Python) and PsychoPy's QuestPlusHandler do; the
 *   grid layout below is chosen so their published examples reproduce.
 *
 *   AXES
 *     A yqst_axis is either an explicit array of values (values, n) or a
 *     linspace (lo, hi, n) when values is NULL. yqst_linspace() and
 *     yqst_fixed() build one. n = 1 is a FIXED parameter: it costs nothing
 *     and is what turns QUEST+ into QUEST or Psi. Grids need not be
 *     uniform; a log-spaced threshold axis is an explicit array. The handle
 *     keeps every axis's size, so yqst_stim_axis_n() and yqst_param_axis_n()
 *     read them back and a caller never has to carry the desc around.
 *
 *   LAYOUT
 *     Both joint grids are row-major with the LAST axis fastest, so
 *     flat = ((i0 * n1 + i1) * n2 + i2) ... . yqst_stim_index() and
 *     yqst_param_index() convert. That order is what every index, the
 *     posterior yqst_posterior() hands back, every marginal and every
 *     history entry is in.
 *
 *     Inside, two things differ, because the per-trial sweep is the whole
 *     cost of this header and it wants long contiguous runs.
 *
 *     First, the likelihood table is TRANSPOSED: L[s][k][theta], stimulus
 *     major, then outcome, with the parameter index fastest. One trial's
 *     selection reads it once front to back as K+1 dot products of a
 *     P-long float row against the posterior, and yqst_update() reads the
 *     one row of the outcome that happened. Both are contiguous, so a
 *     compiler that vectorizes will.
 *
 *     Second, when any parameter axis is flagged nuisance, the axes are
 *     PERMUTED internally so the nuisance ones vary slowest. The marginal
 *     the selection needs is then the fastest-varying part of the posterior,
 *     and marginalizing is a multiply-accumulate into one contiguous vector
 *     instead of a strided gather. The caller never sees this: indices,
 *     marginals, estimates, the history and yqst_posterior() are all in the
 *     order above, and yqst_posterior() permutes into a second buffer when
 *     the two orders differ (they do not when the nuisance axes are already
 *     last, which is how yqst_desc is usually written).
 *
 *     The table holds floats; every accumulation is in double; the posterior
 *     is double. That halves the table against a double table for about 1e-7
 *     of posterior mass, because a float carries seven digits and a
 *     likelihood is multiplied into a posterior that is renormalized every
 *     trial, so the error is replaced rather than compounded. Set
 *     desc.no_table for a run in double throughout, at the price of
 *     evaluating the model instead of reading it.
 *
 *   OUTCOMES
 *     K = 2 for the built-in functions: outcome 1 is "correct" / "yes" /
 *     "detected", outcome 0 the other. A custom function sets K itself
 *     (desc.n_outcomes) and defines what each index means; a 3-outcome
 *     "less / same / more" judgment, or a 4AFC with the chosen interval as
 *     the outcome, are ordinary configurations.
 *
 *   THE LOOP
 *     yqst_next(q)                 stimulus grid index to show next
 *     yqst_update(q, s, k)         what was shown (index) and the outcome
 *     yqst_update_values(q, x, k)  the same for a stimulus OFF the grid
 *     yqst_done(q)                 a stop criterion has fired
 *     yqst_estimate(q, how, out)   parameter estimates from the posterior
 *
 *   next() and update() are separate, as in ysp/stair.h: show what you can,
 *   tell update() what you showed. An off-grid stimulus (a display that
 *   quantized the contrast, an interleaved design, a catch trial) goes
 *   through yqst_update_values(), which evaluates the psychometric function
 *   at that stimulus for all P parameter points instead of reading the
 *   table. The posterior is exact either way; only the cost differs. Two
 *   details follow from that. The trial is recorded with stim_index = -1 even
 *   when the value lands exactly on a grid point, because the value, not the
 *   index, is what was shown. And the two paths agree to float precision
 *   rather than exactly, since the table path rounds the likelihood to float
 *   and this one does not: about 1e-7 per update on a renormalized posterior,
 *   which is the trade LAYOUT describes.
 *
 *   SELECTION (desc.select)
 *     YQST_SELECT_ENTROPY   (default) minimum expected posterior entropy
 *                           over the candidate stimuli. With nuisance
 *                           parameters flagged (yqst_axis.nuisance), the
 *                           entropy is that of the posterior MARGINALIZED
 *                           over them (Watson 2017 sec. 2.5; Prins 2013),
 *                           so trials are spent on the parameters you want.
 *     YQST_SELECT_QUANTILE  the QUEST placement: the stimulus on axis 0
 *                           nearest the posterior quantile
 *                           desc.select_quantile of parameter
 *                           desc.select_param (a threshold in the same
 *                           units as stimulus axis 0). 0.5 is Watson &
 *                           Pelli's default; Pelli's 1987 "ideal"
 *                           placement is a different quantile per beta.
 *     YQST_SELECT_MEAN,     the same with the marginal posterior mean or the
 *     YQST_SELECT_MODE      marginal posterior mode of that parameter.
 *     yqst_next_subset() restricts the candidates for one trial to the
 *     stimuli the display can produce right now.
 *     The three placement rules read only stimulus axis 0, so every stimulus
 *     that shares an axis-0 value is tied and the tiebreak rule below picks
 *     among them.
 *     yqst_next() caches: it recomputes only after an update, so calling it
 *     twice costs one selection and draws from desc.rng once.
 *     yqst_next_subset() always computes, and its answer becomes the cached
 *     proposal, so a yqst_next() after it repeats the subset's choice rather
 *     than scoring the whole grid.
 *
 *   TIES (desc.tiebreak, desc.tie_tolerance)
 *     Two stimuli are tied when their scores differ by less than
 *     tie_tolerance (default 1e-9). The score is the expected entropy in bits
 *     under YQST_SELECT_ENTROPY, where expected entropies of stimuli far from
 *     the posterior mass are equal to many more digits than that, so ties are
 *     common early in a run and at the edges. Under the three placement rules
 *     the score is the distance from stimulus axis 0 to the point estimate,
 *     in the units of that axis, and the tolerance is in those units too. The
 *     rule is set at open and applied every trial:
 *       YQST_TIE_LOWEST    (default) the lowest stimulus index. Fully
 *                          deterministic, and biased toward one end of
 *                          the grid.
 *       YQST_TIE_NEAREST   the tied stimulus nearest, in grid index, to
 *                          the last stimulus shown (the lowest tied index
 *                          before the first trial, and after a
 *                          yqst_update_values() trial, which has no grid
 *                          index). Deterministic; keeps the run from
 *                          jumping across the grid on a tie.
 *       YQST_TIE_ALTERNATE the lowest tied index on one tied trial, the
 *                          highest on the next. Deterministic and
 *                          unbiased over a run. Only a trial that really
 *                          was tied advances the alternation.
 *       YQST_TIE_RANDOM    a uniform draw among the tied, from desc.rng:
 *                          one variate per tied trial, choosing tied
 *                          candidate floor(u * count). open() fails
 *                          without a generator.
 *
 *   RANDOM SUBSET (desc.subset_size, desc.rng)
 *     Watson 2017 sec. 4.2 recommends scoring a random subset of the
 *     stimuli each trial rather than all of them: it breaks the tendency
 *     of pure entropy selection to sit on one stimulus, and it divides
 *     the cost of next() by S / subset_size. With desc.rng set and
 *     subset_size > 0, yqst_next() draws that many distinct stimulus
 *     indices from desc.rng each trial and scores only those. The
 *     generator is the caller's; the header never seeds or owns one, so a
 *     run is reproducible from the caller's seed. yqst_next_subset() is
 *     the same with the caller's own list, of 1 to S indices.
 *     The draw is a rejection draw, so it costs at least subset_size
 *     variates and more when one repeats: keep subset_size well under S,
 *     which is the point of it. A subset_size of S or more scores every
 *     stimulus and draws nothing; open() clamps it.
 *
 *   PRIORS
 *     Uniform on every axis by default. yqst_axis.prior points at n
 *     unnormalized weights for that axis; the joint prior is the product.
 *     desc.joint_prior points at P weights for a prior that does not
 *     factor. Either is normalized at open. A Gaussian prior on a log
 *     threshold with a given sd is what QUEST assumes; build it with
 *     yqst_prior_normal().
 *
 *   STOPPING (desc.stop_trials, desc.stop_entropy, desc.stop_sd)
 *     Any subset; the first to fire ends the run, and yqst_stop_reason()
 *     keeps naming that one: a criterion that has fired stays fired, even if
 *     a later trial would raise the entropy or the sd back over its bound.
 *     stop_entropy is a bound on the posterior entropy in bits (of the
 *     marginal over non-nuisance parameters); stop_sd is a bound on the
 *     posterior sd of parameter desc.stop_sd_param. 0 disables a criterion;
 *     open() rejects all zero. The criteria are tested at open() and after
 *     every update, so an entropy or sd bound the prior already satisfies
 *     makes yqst_done() true before the first trial. Trials after done() are
 *     still accepted and recorded, until YQST_MAX_TRIALS of them are, which
 *     is its own stop reason (YQST_STOP_FULL) and the point at which
 *     yqst_update() starts returning YQST_ERR_FULL and changing nothing.
 *
 *   ESTIMATES
 *     yqst_estimate(q, how, out) fills out[n_param] from the joint posterior:
 *       YQST_EST_MEAN    marginal posterior mean of each parameter
 *       YQST_EST_MODE    the joint posterior mode (one grid point)
 *       YQST_EST_MEDIAN  marginal posterior median of each parameter
 *     yqst_quantile(q, i, p), yqst_sd(q, i) and yqst_marginal(q, i, out)
 *     give a single parameter's marginal quantile, sd and full marginal.
 *     A quantile on a grid is a grid point, not an interpolation: it is the
 *     lowest point of axis i whose cumulative marginal mass reaches p. The
 *     median estimator is yqst_quantile(q, i, 0.5), so both step from grid
 *     point to grid point as trials come in. Psychtoolbox's QuestQuantile
 *     interpolates; this does not, because the posterior only exists on the
 *     grid.
 *     yqst_posterior(q) is the joint posterior itself, P doubles in LAYOUT
 *     order, for a plot or a fit. yqst_entropy(q) is its entropy in bits,
 *     the number QUEST+ is driving down.
 *     mQUESTPlus additionally refits the parameters by maximum likelihood
 *     off the grid (qpFit); ysp/quest.h does not, and says so, because a
 *     fit off the grid is an offline job and this header's job is the next
 *     trial. The history is there for it.
 *
 *   ---------------------------------------------------------------------
 *   PSYCHOMETRIC FUNCTIONS
 *   ---------------------------------------------------------------------
 *   Every built-in is p(correct) = gamma + (1 - gamma - lambda) * F(x),
 *   with the parameter axes in the fixed order
 *
 *       param[0] = alpha   threshold, in the units of stimulus axis 0
 *       param[1] = beta    slope
 *       param[2] = gamma   guess rate (0.5 for 2AFC, 0 for yes/no)
 *       param[3] = lambda  lapse rate
 *
 *   and n_param = 4; fix the ones you do not estimate with yqst_fixed().
 *   They use stimulus axis 0 only; the other stimulus axes, if any, are
 *   ignored (a built-in with n_stim > 1 is a way to test selection, not a
 *   model). The forms follow Palamedes (PAL_Weibull etc.), so a Palamedes
 *   or psignifit fit of the same data agrees on the parameters:
 *
 *     YQST_PF_WEIBULL   F = 1 - exp(-(x / alpha)^beta)            x > 0
 *                       open() rejects a stimulus axis 0 or an alpha axis
 *                       with a point at or below zero, where the form has
 *                       no value; an off-grid yqst_update_values() below
 *                       zero reads F = 0.
 *     YQST_PF_GUMBEL    F = 1 - exp(-10^(beta (x - alpha)))        log-Weibull:
 *                       the Weibull with x and alpha in log10 units. This is
 *                       QUEST's function (Watson & Pelli's log10 intensity)
 *                       and, with x in dB (20 log10), QUEST+'s Watson 2017
 *                       form once beta is scaled by 1/20.
 *     YQST_PF_LOGISTIC  F = 1 / (1 + exp(-beta (x - alpha)))
 *     YQST_PF_NORMAL    F = Phi(beta (x - alpha))     cumulative Gaussian
 *     YQST_PF_HYPSEC    F = (2 / pi) atan(exp((pi / 2) beta (x - alpha)))
 *     YQST_PF_CUSTOM    desc.pf_fn, below.
 *
 *   YQST_PF_CUSTOM takes the model as a callback, in one of two shapes.
 *   Set exactly one of them; open() rejects both or neither.
 *
 *   desc.pf_fn, one cell per call:
 *
 *       void pf(void* ctx, const double* stim,   // n_stim values
 *                          const double* params, // n_param values
 *                          double* p);           // out: n_outcomes values
 *
 *   desc.pf_batch, one stimulus's worth of cells per call:
 *
 *       void pf_batch(void* ctx,
 *                     const double* stims, int S,   // S rows of n_stim
 *                     const double* params, int P,  // P rows of n_param
 *                     float* out);                  // (s*P + i)*K + k
 *
 *   Both get desc.pf_ctx and must be pure functions of their arguments. Both
 *   must write K non-negative values per cell that sum to 1: within 1e-9 for
 *   pf_fn, which writes doubles, and within 1e-6 for pf_batch, which writes
 *   floats and so cannot land closer than about K times a float's last digit.
 *   open() checks every cell while it builds the table, and under
 *   desc.no_table the nine cells at the ends and the middle of both grids,
 *   and fails with the offending cell and the callback's name in the message.
 *
 *   Which to write. pf_fn is the one to write in C: it is a formula, it says
 *   what it means, and S*P calls to a C function are S*P multiply-adds' worth
 *   of overhead. pf_batch is the one to write in a binding, or wherever a
 *   call is expensive and array arithmetic is not: the header calls it once
 *   per stimulus with the whole parameter grid as a P x n_param matrix, so a
 *   vectorized callable is entered S times at open rather than S*P times, and
 *   once per yqst_update_values() or yqst_p_values() rather than P times. The
 *   rows of `params` are the header's own walk of the parameter grid, which is
 *   the LAYOUT order unless a nuisance axis made it permute the grid
 *   internally (see LAYOUT), so a callback must treat them as a list of P
 *   parameter points, in which order does not matter, and answer row for row.
 *   The header calls it with S = 1; the S argument is part of the contract so
 *   that the shape is a batch, not so that the header can subdivide it.
 *
 *   Two things follow from pf_batch writing floats. Its likelihoods carry
 *   float precision, so yqst_p(), yqst_p_values(), yqst_simulate() and an
 *   off-grid yqst_update_values() are float-exact rather than double-exact
 *   under it, which is the same precision the likelihood TABLE has had all
 *   along. And the two entry points, given the same model, fill the table
 *   identically bit for bit, which is what tests/adapt/quest_test.c
 *   checks, so a binding can develop against pf_fn and ship pf_batch.
 *
 *   desc.no_table with pf_batch is legal and works, and it is the wrong
 *   configuration for a binding: the header then calls the callback once per
 *   stimulus on EVERY yqst_next(), which moves S*P*K floats across the
 *   language boundary per trial. That is exactly the table no_table declined
 *   to store, rebuilt every trial. In C, with a cheap formula, it is a fair
 *   trade; through an interpreter it is not.
 *
 *   Watson's quick CSF (Lesmes 2010): stim = (log spatial frequency, log
 *   contrast), params = (peak gain, peak frequency, bandwidth,
 *   low-frequency truncation), a log-parabola for the sensitivity and a
 *   Weibull on the distance to it. examples/quest/qcsf.c writes it both ways
 *   and checks them against each other.
 *
 *   ---------------------------------------------------------------------
 *   RETURN VALUES AND ERRORS
 *   ---------------------------------------------------------------------
 *   yqst_open() and yqst_open_saved() return bool and fill yqst_error() on
 *   failure; they clear it on entry. The calls that change the handle
 *   (yqst_next(), yqst_next_subset(), yqst_update(), yqst_update_values())
 *   also write yqst_error() when they fail. The queries take a const handle
 *   and return a code or NaN without writing it, so a reader with a const
 *   pointer never writes to the handle. Index-returning functions return >= 0 or a negative YQST_ERR_*;
 *   the counting functions return how many values they wrote; the functions
 *   that return a double return NaN on a bad argument; yqst_strerror() names
 *   a code.
 *
 *     YQST_ERR_ARG      null handle, index out of range, outcome >= K,
 *                       non-finite value; also an update whose outcome has
 *                       zero probability under EVERY parameter point, which
 *                       no posterior can absorb. Such an update changes
 *                       nothing and is not recorded.
 *     YQST_ERR_CLOSED   the handle is not open
 *     YQST_ERR_FULL     YQST_MAX_TRIALS trials recorded; the update changes
 *                       nothing
 *     YQST_ERR_MEMORY   the caller's buffer is too small, or the allocator
 *                       returned NULL (open only, with a message)
 *
 *   yqst_open() rejects, with the reason in yqst_error(): n_stim or n_param
 *   outside 1..YQST_MAX_STIM_DIMS / YQST_MAX_PARAMS; an axis with n < 1 or a
 *   non-finite point; a negative or non-finite prior weight, or a prior or
 *   joint_prior whose weights sum to zero; a prior or a nuisance flag on a
 *   stimulus axis, where neither means anything; a
 *   built-in pf with n_param != 4 or n_outcomes neither 0 nor 2;
 *   YQST_PF_WEIBULL with a non-positive stimulus or alpha point;
 *   YQST_PF_CUSTOM with neither pf_fn nor pf_batch, or with both, or with
 *   n_outcomes outside 2..YQST_MAX_OUTCOMES, or whose outcomes do not sum to
 *   1; a pf, select or
 *   tiebreak enum out of range; select_param or stop_sd_param outside
 *   0..n_param-1; select_quantile outside (0, 1) unless it is 0 for the
 *   default of 0.5; a negative tie_tolerance, stop_trials, stop_entropy,
 *   stop_sd or subset_size; YQST_TIE_RANDOM or subset_size > 0 without rng;
 *   no stop criterion at all; memory set with memory_size 0 or below
 *   yqst_memory_size(); and a grid too large for this machine's size_t.
 *
 *   ---------------------------------------------------------------------
 *   MEMORY, COST AND THREADS
 *   ---------------------------------------------------------------------
 *   yqst_memory_size(desc) returns the bytes one handle's arena needs: the
 *   float table, the posterior (P*8), the likelihood scratch that the
 *   off-grid and no-table paths use (P*K*8), the selection scores (S*8), a
 *   marginal workspace, the axis values, the random subset, and seven spare
 *   bytes so an unaligned caller buffer works. It does NOT cover the
 *   trial history: that is YQST_MAX_TRIALS entries inline in the handle,
 *   which the caller allocates as one object. It returns 0 for a desc open()
 *   would reject, so it is also a way to validate one.
 *
 *   With desc.pf_batch the arena also holds the parameter matrix the callback
 *   is handed, P*n_param doubles, and one stimulus's worth of its output,
 *   P*K floats. Both are built once and reused, so the batch path allocates
 *   and frees nothing per call; for the qCSF example that is 45 KB on top of
 *   1.9 MB.
 *
 *   The table is S*P*K floats, plus S*P more UNLESS an axis is flagged
 *   nuisance. The extra row per stimulus is the outcome entropy of each
 *   cell, which is what lets a selection run without a logarithm per cell
 *   (see the cost paragraph below). At K = 2 that is 50% more table, and it
 *   is the one place where this header trades memory for time. A desc with a
 *   nuisance axis takes its logarithms on the marginal instead, so it does
 *   not build the row and does not pay for it, and neither does desc.
 *   no_table. A desc with a nuisance axis that is not already last also
 *   carries P more doubles, for yqst_posterior() to permute into.
 *
 *   open() takes that many bytes from desc.memory when it is set, else from
 *   YQST_MALLOC (malloc by default; define YQST_MALLOC/YQST_FREE before the
 *   include for an arena of your own) exactly once. Nothing allocates after
 *   open, and close() frees only what open() took. open() also copies the
 *   axis values into the arena and consumes the priors into the posterior, so
 *   no yqst_axis.values, yqst_axis.prior or desc.joint_prior array has to
 *   outlive the call; desc.pf_ctx and desc.rng_ctx do, because the callbacks
 *   keep using them. The handle itself is about 1 KB plus the history and
 *   lives where the caller puts it.
 *
 *   Table sizes, so you can check before you open (the middle column is
 *   S*P*K cells; the bytes include the entropy row where there is one):
 *     Psi, 31 x (61 x 12 x 1 x 1) x 2            45 K cells      270 KB
 *     Psi-marginal, 31 x (61 x 12 x 5 x 5) x 2   1.1 M cells     4.5 MB
 *     qCSF, (12 x 30) x (20 x 20 x 10 x 10) x 2  29 M cells      173 MB
 *   The last is what desc.no_table is for: no table, the callback runs
 *   S*P times per yqst_next() instead, and the handle needs a few
 *   multiples of P*8 bytes. mQUESTPlus makes the same trade with
 *   qpQuestPlusInitialize's precompute flags; questplus (Python) keeps the
 *   table in an xarray. A grid that big is also a grid that will not
 *   converge in an experiment's worth of trials; Watson 2017 sec. 4 says
 *   what grain is worth having.
 *
 *   Per trial, yqst_next() under YQST_SELECT_ENTROPY is one pass over the
 *   table, front to back. The expected entropy is not summed cell by cell as
 *   it is written; it is decomposed,
 *
 *     E[H] = H(theta) - H(y | s) + sum_theta post(theta) h(s, theta),
 *
 *   where h(s, theta) = -sum_k L log2 L is the outcome entropy of one cell.
 *   h belongs to the table, so it is tabulated at open, H(theta) is the same
 *   for every stimulus, and what is left per stimulus is K+1 dot products of
 *   length P and K logarithms: S*P*(K+1) multiply-adds and S*K logarithms for
 *   the sweep. With nuisance axes flagged the entropy is that of the
 *   marginal, which does not decompose, so that path instead accumulates K
 *   marginals (S*P*K multiply-adds, no entropy row) and takes S*K*(points in
 *   the marginal) logarithms. Either way there is no logarithm per cell, and
 *   both sweeps are contiguous. yqst_update() reads the one row of the
 *   outcome that happened, P cells, twice. qCSF without a table is 29 M
 *   callback evaluations per selection, hundreds of milliseconds, and belongs
 *   in the inter-trial interval or on a thread of the caller's.
 *
 *   Every count above is an operation count. For times, run
 *   examples/quest/bench.c: it prints the counts next to measured nanoseconds
 *   for four configurations of the Psi-marginal grid, and next to a MEMORY
 *   FLOOR, which is the same dot product over the same bytes with nothing
 *   else in it. What it prints on your machine is what to log. On one x86-64
 *   laptop (an i7-1360P under WSL2, gcc 11.4 -O2, v0.5.2, a 4.5 MB table, so
 *   well out of cache), median of 40 calls, one core, load average under 1:
 *
 *     memory floor                          0.37 ns/cell
 *     Psi, 61 x 12 grid    0.03 ms next     0.57 ns/cell, 0.7 us update
 *     joint entropy        0.70 ms next     0.61 ns/cell,  16 us update
 *     marginal entropy     0.89 ms next     0.79 ns/cell,  17 us update
 *     random subset of 8   0.22 ms next     0.76 ns/cell,  16 us update
 *     no table at all     17.3  ms next    15.3  ns/cell, 535 us update
 *
 *   A second run minutes later agreed within 12%. The table sweeps are within
 *   about twice the floor, and the rest is the entropy row, the logarithms
 *   and the per-stimulus bookkeeping. The Psi row is the fastest per cell,
 *   because its whole table is 272 KB and stays in cache. The sweeps are
 *   bound by memory, not by arithmetic: building with -mavx2 -mfma, which
 *   doubles the vector width, does not change the numbers. There are no SIMD
 *   intrinsics in this header for that reason. Under MSVC 19.44 /O2 on the
 *   same laptop, over two runs, the same grid measured 1.00 to 1.15 ns/cell
 *   joint and 0.92 to 0.97 marginal against a 0.63 to 0.66 ns/cell floor.
 *   Run to run on a loaded machine these numbers move by a factor of two,
 *   which is the reason to measure on the rig rather than to quote a number
 *   from here.
 *
 *   FRAME BUDGET
 *   One yqst_next() plus one yqst_update() that fit inside a 60 Hz frame,
 *   16 ms, can run between trials on the experiment's own thread, with no
 *   pacing, no worker and no thread of any kind. Measured on the machine
 *   above, next + update against the budget:
 *
 *     Psi, 31 x (61 x 12 x 1 x 1) x 2         0.03 ms   fits 600 times
 *     Psi-marginal, 31 x (61 x 12 x 5 x 5)    0.91 ms   fits 17 times
 *     the same grid, nothing flagged          0.71 ms   fits 22 times
 *     the same grid, subset_size 8            0.24 ms   fits 67 times
 *     the same grid, desc.no_table           17.9  ms   DOES NOT FIT
 *     qCSF with a table, 29 M cells          ~20 ms     DOES NOT FIT
 *     qCSF with desc.no_table                ~450 ms    DOES NOT FIT
 *
 *   The two qCSF rows are estimates, not measurements: the Psi-marginal rate
 *   per cell times the qCSF cell count. So every configuration that keeps a
 *   table and is not of qCSF size is a fraction of a frame, and
 *   examples/quest/qcsf.c, whose grid is ninety times smaller than the qCSF
 *   row, is one too (0.3 ms there). When a configuration does not fit, do
 *   this, in this order: set desc.subset_size, which divides the sweep by
 *   S / subset_size and is what Watson 2017 sec. 4.2 recommends anyway (8 of
 *   31 stimuli above, 67 times under the budget); coarsen the grid, which
 *   Watson 2017 sec. 4 argues for on statistical grounds as well; or run
 *   yqst_next() on a thread during the inter-trial interval, which this
 *   header allows because it does no I/O and touches nothing but its own
 *   handle, and which ASYNC does for you. Do not split one selection across
 *   frames: the posterior must not change underneath it.
 *   examples/quest/bench.c prints this line for every configuration it
 *   times, so a desc of your own can be checked against the budget before it
 *   is trusted with an experiment.
 *
 *   PRECISION
 *   The likelihood table is float; the posterior, every accumulator, the
 *   entropies and every public number are double; the psychometric function is
 *   evaluated in double. LAYOUT says why the table is the one narrow thing.
 *   Two ways of narrowing the rest were implemented, measured and then taken
 *   out again. The numbers are here so nobody has to do it twice.
 *
 *   A float posterior, scratch and accumulators (the table already being
 *   float, this makes the whole sweep single precision). The sweep turns out
 *   to be neither purely bandwidth-bound nor purely arithmetic-bound, so
 *   halving the arithmetic width helps only where the compiler vectorizes.
 *   Best of several interleaved runs on one core, Psi-marginal grid:
 *
 *     -O2 (gcc 11, no vectorizer)   1.06 to 1.17x faster
 *     -O3 (SSE2, 2 -> 4 lanes)      1.34 to 1.48x faster
 *     the small 61 x 12 Psi grid    1.2 to 2.0x, yqst_update 1.2 to 1.7x
 *     desc.no_table                 no gain, and 0.87 to 0.89x in every run
 *
 *   Accuracy was never what decided it: over 90 simulated trials on three
 *   grids not one selected stimulus or simulated outcome changed, the
 *   threshold estimate moved by at most 3.6e-6 log units against a posterior
 *   sd of 0.06, the posterior by 1.2e-6 relative, the entropy by 1.1e-4 bits,
 *   and the mode and the quantiles were identical. It was dropped because
 *   1.1 to 1.5x off a selection that already fits a display frame more than
 *   ten times over buys the experiment nothing, while a second precision would
 *   have to be documented, tested and supported for good; and because the one
 *   configuration that does NOT fit the frame, desc.no_table, is where it was
 *   consistently slower.
 *
 *   An IEEE half (binary16) table, converted to float on load. It halves the
 *   table as advertised, 4.89 MB to 2.73 MB of arena on the Psi-marginal grid,
 *   and it is 2 to 8 times SLOWER: the memory floor goes from 0.44 to 1.4-2.2
 *   ns per cell and one selection from 1.3 to 11-14 ms, because the
 *   conversion costs more arithmetic than the halved bandwidth saves. The
 *   F16C hardware conversion, one instruction but not in the x86-64 baseline,
 *   narrows it to about 2.2x slower, which is still a loss. Its accuracy
 *   would have been usable (threshold estimate within 1.4e-3 log units, no
 *   selection changes in 90 trials, a 20-run Psi threshold error of 0.0342
 *   against 0.0324 for the float table, and 4.2e-4 on the decomposed
 *   selection score); it simply is not faster. When a table does not fit, the
 *   answer is still desc.no_table, or a coarser grid.
 *
 *   One sentence explains both results. At about 0.9 ns per cell the sweep is
 *   balanced between the bytes it moves and the work it does per byte, so
 *   cutting bytes at the price of operations loses, and cutting operations at
 *   constant bytes wins a little. That is also why there are no SIMD
 *   intrinsics here: see the note above about -mavx2 changing nothing.
 *
 *   A handle is not thread-safe, and the way to use one from a frame loop
 *   without paying for it on that thread is ASYNC, not a lock of your own.
 *   Every function is deterministic unless
 *   desc.rng is set: the same desc and the same history give the same
 *   posterior and the same proposals, bit for bit on one platform, so a
 *   saved history replayed through yqst_update() restores the run. With
 *   rng set, the posterior is still a function of the history alone; only
 *   the proposals depend on the caller's seed.
 *
 *   ---------------------------------------------------------------------
 *   SIMULATION
 *   ---------------------------------------------------------------------
 *   yqst_p(q, stim_index, params, p_out) evaluates the model at any
 *   parameter values (not only grid points); yqst_simulate(q, stim_index,
 *   params, u) draws an outcome from it with the uniform variate u in
 *   [0, 1) that the caller supplies. The header has no random generator.
 *   examples/quest/sim.c runs Psi and QUEST configurations against a
 *   simulated observer and prints the posterior sd per trial next to the
 *   truth. examples/quest/qcsf.c does the same for a four-parameter quick CSF
 *   through desc.pf_fn, and tests/adapt/quest_test.c replays a fixed
 *   response sequence against a reference posterior computed from the
 *   definitions. The comparison with mQUESTPlus's
 *   qpQuestPlusPaperSimpleExamplesDemo runs through the MEX binding, in
 *   tests/compare/compare_quest_mquestplus.m. STATUS has its result, and
 *   docs/adapt.md, "Verification", has the other comparisons.
 *
 *   ---------------------------------------------------------------------
 *   SNAPSHOTS
 *   ---------------------------------------------------------------------
 *   A session can be saved at any trial and resumed later, after a crash or a
 *   break, WITHOUT replaying it:
 *
 *       size_t n = yqst_save_size(&q);
 *       yqst_save(&q, buf, n);                    // write buf to disk
 *       ...
 *       yqst_close(&q);
 *       if (!yqst_open_saved(&q, &desc, buf, n))  // q resumes where it was
 *           fputs(yqst_error(&q), stderr);
 *
 *   What is saved: the posterior, the history, the pending proposal (a
 *   yqst_next() that was answered by no yqst_update() yet is not made again,
 *   so it draws nothing from desc.rng), the tie state (YQST_TIE_NEAREST's last
 *   stimulus and YQST_TIE_ALTERNATE's parity), the stop state, and the desc's
 *   numbers. What is not: the likelihood table, which is a function of the
 *   desc and is rebuilt at load, and anything behind a pointer.
 *
 *   yqst_open_saved() is yqst_open() and then a restore. It takes a desc because a
 *   snapshot cannot carry what the desc points at: the axis arrays, the
 *   callbacks and their contexts, desc.rng and its context, desc.memory. That
 *   desc must agree with the snapshot on every number the resumed session
 *   depends on: every axis's points (as open() resolved them, so a linspace
 *   and the same values as an explicit array agree), the nuisance flags, the
 *   psychometric function and whether a custom one is pf_fn or pf_batch, the
 *   outcome count, the selection rule and its parameter and quantile, the
 *   tiebreak and its tolerance, whether there is a generator, the subset
 *   size, the stop criteria and no_table. A mismatch fails the load and names
 *   the first field that differs. The priors are the exception, and on
 *   purpose: open() consumes them into the posterior and keeps no copy, and
 *   the posterior in the snapshot replaces what they would have produced, so
 *   no difference in them could change the resumed session.
 *
 *   The GENERATOR is the caller's, as everywhere in this header: save its
 *   state beside the snapshot and put it back before the next yqst_next().
 *   With that, a resumed session is the uninterrupted one bit for bit: same
 *   proposals, same posterior, same history, same generator state at the end.
 *   tests/adapt/quest_test.c checks exactly that, cutting a 24-trial run
 *   at trials 0, 1, 7 and 23, both between an update and the next selection
 *   and between a selection and its update, in five configurations: joint,
 *   a nuisance axis last, a nuisance axis in the middle (which the header
 *   stores permuted), pf_batch, and a random subset with random ties through
 *   desc.rng.
 *
 *   COST, measured on the Psi-marginal grid of MEMORY, COST AND THREADS: a
 *   snapshot after 100 trials is 149 KB, nearly all of it the posterior (P
 *   doubles; the history is 9 bytes plus 8 per stimulus dimension a trial).
 *   yqst_save() took under a millisecond. yqst_open_saved() took what yqst_open()
 *   takes, 24 to 38 ms against 26 to 30 ms for the open, because both are the
 *   table rebuild: S*P evaluations of the model. That is the trade: the
 *   snapshot is the size of the posterior instead of the size of the table,
 *   and a resume pays one open. Under desc.no_table there is no table and a
 *   load is a millisecond.
 *
 *   LAYOUT, format 1. Every integer is little-endian two's complement, written
 *   and read one byte at a time, so a snapshot moves between compilers and
 *   platforms; an f64 is the IEEE 754 bit pattern as a u64.
 *     magic     4 bytes "PSYQ", then u32 format (1)
 *     desc      i32 n_stim; per stimulus axis i32 n and n f64 points;
 *               i32 n_param; per parameter axis i32 n, u8 nuisance and n
 *               f64 points; i32 pf, u8 model kind (0 built-in, 1 pf_fn,
 *               2 pf_batch), i32 K, i32 select, i32 select_param, f64
 *               select_quantile, i32 tiebreak, f64 tie_tolerance, u8 has
 *               rng, i32 subset_size, i32 stop_trials, f64 stop_entropy,
 *               f64 stop_sd, i32 stop_sd_param, u8 no_table, i32 S, i32 P.
 *               Defaults are written as used (select_quantile 0 as 0.5,
 *               tie_tolerance 0 as 1e-9, subset_size clamped to S).
 *     state     i32 n_trials, proposed, last_shown, tie_parity, stop
 *     posterior P f64 in the CALLER'S axis order (LAYOUT), not renormalized
 *     history   n_trials x (n_stim f64 stim, i32 stim_index, i32
 *               proposed_index, u8 outcome)
 *   Every number is range-checked as it is read, and the posterior is checked
 *   for finite, non-negative mass, so a wrong, truncated or corrupt snapshot
 *   fails the load with a message instead of indexing outside the handle, and
 *   a failed load leaves the handle closed. The format number changes when
 *   this layout does.
 *
 *   Under YQST_ASYNC the handle belongs to the thread between
 *   yqst_async_open() and yqst_async_close(), so save after the close, and
 *   open a new async session on the handle yqst_open_saved() rebuilt.
 *
 *   ---------------------------------------------------------------------
 *   PARAMETERS
 *   ---------------------------------------------------------------------
 *   yqst_params() lists every yqst_desc field that an experiment definition
 *   or a designer sets (rig_spec principle 8), in the shape ysp/stair.h and
 *   ysp/aep.h use: name, type, inclusive range, the value a zero field
 *   means, unit, a line of text, and an enum's values.
 *     name    the desc field; "a.b" is member b of a, and "a[].b" is
 *             member b of each element of the array a ("stim[].lo" is the
 *             lo of every stimulus axis, n_stim of them)
 *     type    "i32", "f64", "bool", "enum", or an array of one of them
 *             ("f64[]"): an axis's explicit points and a parameter axis's
 *             prior have n entries, joint_prior has one per point of the
 *             parameter grid (P), in LAYOUT order
 *     def     what a zero field means when every other field is at its
 *             default; a required field has 0, which can be outside
 *             [min, max], and its doc starts with "required"
 *     values  for an "enum", "name=value" pairs joined by ',' in value
 *             order, the C enum's suffix in lower case; NULL otherwise
 *   The pointers that a definition cannot hold are not rows: pf_fn,
 *   pf_batch and pf_ctx (a custom model is the program's), rng and rng_ctx
 *   (the player's generator), memory and memory_size. A row's doc says when
 *   a value needs one of them (YQST_PF_CUSTOM, YQST_TIE_RANDOM,
 *   subset_size). The table is static and the same for every handle.
 *
 *   ---------------------------------------------------------------------
 *   ASYNC (YQST_ASYNC)
 *   ---------------------------------------------------------------------
 *   Everything above runs on the thread that calls it, and FRAME BUDGET says
 *   what that costs: every configuration with a table fits a 16 ms frame many
 *   times over, and desc.no_table does not. The other half of the answer, for
 *   the configuration that does not fit and for a caller that would rather not
 *   spend the millisecond at all, is to not do the work on the frame loop's
 *   thread. Define YQST_ASYNC before the include and this header gains one:
 *
 *       #define YQST_ASYNC
 *       #define YSP_QUEST_IMPLEMENTATION
 *       #include "ysp/quest.h"          // brings ysp/rt.h with it
 *
 *       static yqst_quest q;
 *       yqst_open(&q, &desc);           // as usual, on this thread
 *       static yqst_async a;
 *       yqst_async_desc ad = { 0 };
 *       ad.quest = &q;
 *       if (!yqst_async_open(&a, &ad)) die(yqst_async_error(&a));
 *
 *       yqst_snapshot s;
 *       yqst_async_poll(&a, &s);        // the first proposal, already there
 *       for (;;) {
 *           int k = run_trial(s.stim[0]);         // your frames
 *           int seq = yqst_async_submit(&a, s.proposed, k);
 *           while (drawing_frames())              // the interval
 *               if (yqst_async_poll(&a, &s) >= seq) break;
 *           yqst_async_wait(&a, (uint32_t)seq, YRT_MS(20), &s);  // or block
 *           if (s.done) break;
 *       }
 *       yqst_async_close(&a);           // drains; the handle is yours again
 *
 *   WHAT IT IS. ysp/rt.h's yrt_pump with yqst_update() and yqst_next() in
 *   its on_msg: one thread, a queue of YQST_ASYNC_QUEUE responses (8 by
 *   default) inline in the handle, no heap, normal or below-normal priority
 *   and never above it. on_idle is NULL, because QUEST+ has nothing to do
 *   between trials; that hook is there for ysp/aep.h's background fitting.
 *   Read ysp/rt.h's PUMP section for the queue, the seq and the lock; this
 *   layer is a hundred lines on top of it and adds no concurrency of its own.
 *
 *   OWNERSHIP, which is the rule to get right. The caller opens the quest
 *   handle and yqst_async_open() takes it over: from then until
 *   yqst_async_close() returns, NO yqst_* call on that handle is allowed from
 *   any thread, including the const ones and including yqst_simulate() (a
 *   custom pf_batch model writes to the handle's staging row, so even
 *   evaluating the model is a write). Everything a trial loop needs is in the
 *   snapshot instead. After stop the handle is the caller's again and every
 *   yqst_* call is legal, which is how a session ends: stop, then read the
 *   history, the posterior and the estimates at leisure.
 *
 *   THE SEQ. yqst_async_submit() returns a positive, increasing seq for the
 *   response it copied. yqst_async_poll() returns the seq the snapshot
 *   accounts for: every response through it has been applied AND the proposal
 *   in the snapshot was computed after them. So `poll() >= seq` is the "is the
 *   next stimulus ready?" test, it is one atomic load and a struct copy, and a
 *   frame loop can afford it every frame. yqst_async_wait() is the blocking
 *   form with a relative timeout, for the end of an interval.
 *
 *   Seq 0 is the proposal yqst_async_open() computed before the thread
 *   existed, so a snapshot always exists once open has returned true and
 *   the first trial reads its stimulus from a poll that returns 0. That is why
 *   there is no "nothing published yet" state to handle: the alternatives were
 *   a sentinel or a flag, and having open publish is simpler than either.
 *
 *   THE QUEUE's capacity is YQST_ASYNC_QUEUE, which sizes the handle; how
 *   much of it a session uses is yqst_async_desc.queue_depth (0 for all of
 *   it). That is a policy, how far the trial loop may run ahead of the
 *   inference, and 1 keeps the two in lockstep: a second response submitted
 *   before the first is finished comes back YQST_ERR_BUSY.
 *
 *   A FULL QUEUE is YQST_ERR_BUSY, nothing was copied, and the caller still
 *   owns the response: retry it on the next frame. A queue that grew instead
 *   would trade a visible error for an invisible unbounded latency. With the
 *   default queue of 8 it takes eight trials submitted faster than the thread
 *   drains to get there, which for a one-millisecond selection means a trial
 *   loop that is not waiting for responses at all.
 *
 *   WHAT IT DOES NOT DO. It does not make a slow configuration fast, it moves
 *   it: the counts under MEMORY, COST AND THREADS still decide how many trials
 *   a session can afford, and a no_table qCSF selection is still about 450 ms,
 *   now on another core. It does not touch the posterior's determinism
 *   either: the thread runs the same functions in submit order, so the same
 *   responses give the same posterior bit for bit whether they went through
 *   the queue or not, and tests/adapt/quest_test.c checks exactly that
 *   with memcmp.
 *
 *   COST. The handle is about 5.5 KB (most of it yrt_pump's inline ring,
 *   which this layer does not use: YRT_PUMP_INLINE_BYTES can be set to 1 if
 *   nothing else in the program needs it) plus the queue, 40 bytes a response.
 *   A submit is a mutex, a 40-byte copy and a condition-variable signal. A
 *   poll is an atomic load and a 136-byte copy under a mutex nothing else
 *   holds for long. Nothing allocates.
 *
 *   ---------------------------------------------------------------------
 *   BUILDING
 *   ---------------------------------------------------------------------
 *   Nothing to link but libm, unless YQST_ASYNC is defined: then ysp/quest.h
 *   includes ysp/rt.h, which must sit beside it, and POSIX builds link
 *   -pthread. Copy both headers in that case, as a transport header's user
 *   does. YSP_QUEST_IMPLEMENTATION then also compiles ysp/rt.h's
 *   implementation, unless the same translation unit already defined
 *   YSP_RT_IMPLEMENTATION or implemented a transport header; either order
 *   gives exactly one copy. YQST_ASYNC together with YRT_NO_THREADS is a
 *   #error: the layer IS a thread, so there is nothing sensible to compile.
 *   Define YQST_ASYNC_QUEUE (8) to resize the response queue's capacity,
 *   which sizes the yqst_async handle; the depth a session uses is
 *   yqst_async_desc.queue_depth.
 *
 *   Every macro in this header either sizes a handle (YQST_MAX_STIM_DIMS,
 *   YQST_MAX_PARAMS, YQST_MAX_OUTCOMES, YQST_MAX_TRIALS, YQST_ASYNC_QUEUE),
 *   selects a build (YQST_ASYNC, YQST_API, YQST_MALLOC / YQST_FREE), or names
 *   a version. Nothing a session might tune is a macro, because a binding
 *   compiles the header once for every script that uses it: what a session
 *   chooses is a desc field, with zero as the default. Two constants look like
 *   knobs and are deliberately not. The sum-to-1 check on a custom model's
 *   outcomes (1e-9 for pf_fn, 1e-6 for pf_batch) is a precondition, not a
 *   preference: the decomposed selection score assumes each cell's outcomes
 *   sum to 1, so a looser check would let a biased selection through
 *   silently; normalize the model instead. And the random subset's 64 retries
 *   before it falls back to a linear probe only bound a loop against a
 *   degenerate generator, and change nothing a working one produces.
 *
 *   Nothing to link but libm. The per-trial sweeps are written as
 *   contiguous dot products with four independent accumulators, which is
 *   what a compiler needs to vectorize a floating-point reduction without
 *   being told it may reassociate. gcc does it at -O3 (or -O2
 *   -ftree-vectorize; gcc 11 at plain -O2 does not vectorize at all), MSVC at
 *   /O2, both with 16-byte vectors. Nothing here needs -ffast-math, and
 *   -ffast-math is not recommended: it would let the compiler reorder the
 *   summations and the results would stop being reproducible.
 *
 *   Define YQST_API to override the default
 *   `extern` linkage (definitions too, so static works). Define
 *   YQST_MAX_STIM_DIMS (4), YQST_MAX_PARAMS (8), YQST_MAX_OUTCOMES (8) and
 *   YQST_MAX_TRIALS (2048) before the include to resize the handle: the
 *   history is 2048 trials inline, which is most of the handle's 100 KB, so a
 *   session of 300 trials should say so. Define YQST_MALLOC and YQST_FREE
 *   (both, or neither) to replace malloc for the one allocation at open.
 *
 *       cc -O2 -Iinclude -o quest_sim examples/quest/sim.c -lm
 *       cl /O2 /Iinclude examples\quest\sim.c
 *
 *   ---------------------------------------------------------------------
 *   CHANGELOG
 *   ---------------------------------------------------------------------
 *   v0.7.0 - yqst_params(), the table of the desc's fields for a loader's
 *          key check and a designer's panel (gap G6 of
 *          docs/experiment_format.md); see PARAMETERS. Adds API; changes
 *          none. SNAPSHOTS gave the magic as "YQST"; the code has always
 *          written "PSYQ", and the manual now says so. No number changed.
 *   v0.6.0 - the repository conventions (README.md, Conventions). Breaks
 *          the API: yqst_async_start() / yqst_async_stop() are
 *          yqst_async_open() / yqst_async_close(), yqst_load() is
 *          yqst_open_saved(), yqst_async_wait() takes an int64_t timeout_ns
 *          (0 or less polls), and yqst_prior_normal() returns 0 or
 *          YQST_ERR_ARG instead of a bool. yqst_next(), yqst_next_subset(),
 *          yqst_update() and yqst_update_values() write yqst_error() when
 *          they fail; the const queries still return a code only. The USAGE
 *          and ASYNC examples zero the desc and set fields, so they build as
 *          C99 and C++17. No number changed: the arithmetic is v0.5.2's.
 *   v0.5.2 - documentation only: STATUS records the comparisons against
 *          mQUESTPlus, Palamedes' PAL_AMPM and Watson's own notebook runs,
 *          which are now done. No code changed.
 *   v0.5.1 - builds clean on gcc 16 (MinGW, -O3) and current clang, which
 *          CI found and this machine's compilers do not. No behavior change,
 *          and the default build's arithmetic is bit for bit v0.5.0. gcc 16
 *          constant-propagated a test's deliberately bad arguments (axis 9,
 *          a three-element parameter array for a three-parameter model) into
 *          paths that open()'s invariants make impossible, and with
 *          -Werror=aggressive-loop-optimizations and -Werror=array-bounds it
 *          refused them. Neither was an over-read at run time. The header now
 *          makes the invariants visible instead of assuming them: every axis a
 *          caller passes is checked against the array it will index as well as
 *          against the handle, every loop over the stimulus or parameter axes
 *          is bounded by the array it indexes, and every array a caller hands
 *          in or gets back is copied once, exactly n_stim / n_param / K
 *          elements, to or from a local of the compile-time maximum, so no
 *          caller's array is ever indexed past what the handle says it holds on
 *          any path an optimizer can build. The test initializes every array
 *          a header call fills and fills its deliberately bad desc with 0xA5
 *          instead of leaving it uninitialized.
 *   v0.5.0 - snapshots: yqst_save_size(), yqst_save() and yqst_load(), in the
 *          shape of ysp/trials.h's ytr_save / ytr_load. A session saved
 *          at any trial resumes without replay and continues bit for bit as
 *          the uninterrupted one would have, which the test checks at four
 *          cut points in five configurations. And yqst_async_desc.queue_depth:
 *          how far the trial loop may run ahead of the inference is now a
 *          session's choice, under YQST_ASYNC_QUEUE as the capacity, where it
 *          used to be the macro itself. That was the one compile-time knob a
 *          binding could not reach; see BUILDING for the ones that stay.
 *   v0.4.1 - two small things a binding asked for: version macros
 *          (YQST_VERSION_MAJOR / MINOR / PATCH / STRING) and yqst_version(),
 *          so a program can log which header it was built from, and
 *          yqst_stim_axis_n() / yqst_param_axis_n(), so a caller can read an
 *          axis's size back from the handle instead of keeping its own copy
 *          of the desc. No behavior changed.
 *   v0.4 - an opt-in async layer, compiled only under YQST_ASYNC: yqst_async,
 *          a background thread that owns a yqst_quest between start and stop,
 *          takes responses through a fixed-size queue and publishes a snapshot
 *          (the next stimulus, an estimate, the entropy, the stop state) that
 *          the frame loop polls or waits on. It is ysp/rt.h's yrt_pump with
 *          yqst_update() and yqst_next() in its on_msg, so an async run and a
 *          synchronous replay of the same responses agree bit for bit, which
 *          the test asserts. Without YQST_ASYNC the header is unchanged, still
 *          includes nothing but the C standard library, and produces the same
 *          numbers bit for bit; examples/quest/async.c is a 16 ms frame loop
 *          driving it. See ASYNC and docs/adapt.md, "Inference on a
 *          thread".
 *   v0.3.1 - no code in the header changed, and the default build is bit for
 *          bit v0.3. Two lower-precision variants were built and measured to
 *          answer whether they are worth having: a float posterior and
 *          accumulators, and a 16-bit likelihood table. Neither earned its
 *          place, both were removed, and PRECISION records the numbers so the
 *          question does not have to be reopened. examples/quest/bench.c lost
 *          a measurement bug in the process: its memory-floor pass did the
 *          same arithmetic every trial, so at -O3 the compiler hoisted it out
 *          of the timing loop and the floor came out 30 times too fast. The
 *          pass now perturbs the posterior first.
 *   v0.3 - a batch entry point for the custom psychometric function,
 *          yqst_pf_batch_fn and desc.pf_batch, beside the per-cell
 *          desc.pf_fn. It fills a whole stimulus's worth of cells in one
 *          call, which is what a binding needs: a vectorized Python callable
 *          is entered once per stimulus instead of S*P times (140 times
 *          instead of 156,800 for examples/quest/qcsf.c, which now uses it
 *          and keeps the per-cell version beside it to check against).
 *          YQST_PF_CUSTOM takes exactly one of the two; open() rejects both
 *          or neither. The two paths produce tables that are equal bit for
 *          bit, which the test asserts. The cell entropy row is now computed
 *          from the likelihoods AS STORED, so the decomposition is exact
 *          against the floats the selection reads.
 *   v0.2 - yqst_next() made fast, and measured. Same API; two changes
 *          inside, both of them visible only in memory and in the clock:
 *            - the expected entropy is computed from its decomposition,
 *              E[H] = H(theta) - H(y | s) + sum_theta post h(s, theta), with
 *              the cell outcome entropy h tabulated at open as one more table
 *              row. That takes the logarithm out of the per-cell loop: a
 *              joint selection is now S*P*(K+1) multiply-adds and S*K
 *              logarithms, where v0.1 needed S*P*K logarithms. The row costs
 *              S*P floats, 50% more table at K = 2, and is only built when
 *              it will be used: a desc with a nuisance axis takes its
 *              logarithms on the marginal and does not pay for it.
 *            - the table is stored transposed, L[S][K][P] with the parameter
 *              index fastest, and the parameter axes are permuted internally
 *              so the nuisance ones are the slowest. Every sweep is then a
 *              contiguous dot product or a contiguous multiply-accumulate
 *              over the parameter grid, which gcc and MSVC vectorize. The
 *              public index order is unchanged; see LAYOUT.
 *          Measured on one laptop, a Psi-marginal selection went from 6.3 ms
 *          to 0.95 ms and an update from 30 us to 19 us; the numbers and the
 *          memory floor they are measured against are under MEMORY, COST AND
 *          THREADS, and examples/quest/bench.c prints them for your machine.
 *          No SIMD intrinsics: at the measured rate the sweep is bound by
 *          memory, not by arithmetic, and -mavx2 changes nothing.
 *   v0.1 - the implementation. The API and the declarations are v0.0's, and
 *          the handle gained three private fields (the selection scores and
 *          the nuisance marginal's strides). Everywhere the specification
 *          was optimistic, silent or self-contradictory, this manual now
 *          says what the code does:
 *            - a selection costs one logarithm per table CELL, S*P*K of
 *              them, not S*K, and that is what a selection costs. Flagging
 *              nuisance axes makes it cheaper, not dearer. See MEMORY, COST
 *              AND THREADS, which also has measured times.
 *            - yqst_memory_size() covers the arena, not the trial history:
 *              the history is inline in the handle. It also carries seven
 *              bytes of slack so an unaligned caller buffer works.
 *            - desc.tie_tolerance is in the units of the score. Those are
 *              bits only under YQST_SELECT_ENTROPY; under the placement
 *              rules they are the units of stimulus axis 0.
 *            - a stop criterion, once fired, stays fired, and the criteria
 *              are tested at open() as well as after every update.
 *            - a quantile is a grid point, not an interpolation, and the
 *              placement rules read the marginal of desc.select_param.
 *            - an update whose outcome is impossible under every parameter
 *              point returns YQST_ERR_ARG and changes nothing.
 *            - yqst_update_values() records stim_index = -1 even on a grid
 *              point, and agrees with yqst_update() to float precision.
 *            - yqst_next() caches its proposal; yqst_next_subset() does not,
 *              and overwrites that cache.
 *            - YQST_PF_WEIBULL needs a positive stimulus axis 0 and a
 *              positive alpha axis; open() rejects anything else.
 *            - open() checks a custom pf in every cell it tabulates, and in
 *              nine cells under desc.no_table.
 *            - RETURN VALUES AND ERRORS now lists every desc open() refuses.
 *   v0.0 - specification. Declarations and the manual, no implementation.
 *
 *   STATUS: v0.7.0 (its check is at the end of this section). Implemented
 *   and tested against an independent
 *   double-precision reference written from the definitions in
 *   tests/adapt/quest_test.c: 14770 checks over the posterior update, the
 *   expected-entropy selection (joint and marginalized over nuisance axes),
 *   every built-in psychometric function, every placement rule, every
 *   tiebreak rule, every stop criterion, every estimator, a three-outcome
 *   custom model, the no_table and off-grid paths, a nuisance axis in the
 *   middle of the parameter list (which makes the internal and the public
 *   axis orders differ), both custom-callback entry points, the arena
 *   accounting, and every desc the manual says open() rejects. desc.pf_batch
 *   and desc.pf_fn, given the same model, fill the table equal BIT FOR BIT
 *   and make identical selections, with and without the permutation and at
 *   K = 2 and K = 3; the batch path's off-grid and single-cell results agree
 *   with the per-cell path's to float precision, as the manual says they
 *   will. The posterior and the selection scores agree with the
 *   reference to 1e-12 under desc.no_table (both in double) and to 1e-6
 *   through the float table, and the selected stimulus is identical. The
 *   decomposed selection score agrees with the definition to 1.3e-15 in
 *   double; through the float table the worst difference is 5.3e-8, which is
 *   the rounding of the tabulated cell entropy to float, nothing else, and is
 *   below the table's own error. A 20-replication simulated Psi run of 100 trials
 *   recovers a threshold of -1.72 log10 units with a mean absolute error of
 *   0.032 and a bias of -0.003. A snapshot taken at trials 0, 1, 7 and 23 of a
 *   24-trial run, both between an update and the next selection and between a
 *   selection and its update, resumes to the uninterrupted run BIT FOR BIT
 *   (proposals, posterior, history, estimates, entropy, and the caller's
 *   generator state) in five configurations: joint, a nuisance axis last and
 *   in the middle, pf_batch, and a random subset with random ties; and a load
 *   refuses, with the field named and the handle left closed, a desc that
 *   differs in any compared number, the other callback kind, a wrong magic or
 *   format, a truncated or trailing snapshot, and corrupt counters, posterior
 *   or history.
 *
 *   The async layer has its own block in that test, built and run both ways:
 *   14770 checks without YQST_ASYNC and 15121 with it, the extra ones covering
 *   an async run against a bit-for-bit synchronous replay, the initial
 *   proposal at seq 0, a poll that is never ahead of what was submitted nor
 *   behind a previous poll, a queue driven until it returns YQST_ERR_BUSY with
 *   the replay still matching afterwards, a stop that drains, an off-grid
 *   submit, every call on a zeroed or stopped handle, and queue_depth 1
 *   pushing back on a second submit.
 *
 *   v0.6.0 on 2026-10-10 (Windows 11, MSVC 19.44 /W4 /WX, C and C++17, with
 *   and without YQST_ASYNC, and MinGW-w64 gcc -Werror): 14782 checks without
 *   YQST_ASYNC and 15133 with it. The 12 new ones cover the messages the
 *   handle-changing calls leave and yqst_close() on a zeroed handle, after a
 *   failed open and twice; the counts above are v0.5.2's.
 *
 *   v0.7.0 on 2026-10-21 (MSVC 19.44 /W4 /WX as C and C++17 with and without
 *   YQST_ASYNC, MinGW-w64 gcc 16.1 -Werror, WSL2 gcc 11.4 under
 *   -fsanitize=address,undefined, emcc): 14833 checks without YQST_ASYNC
 *   and 15184 with it (emcc). The new ones check every row of yqst_params()
 *   against the desc field it names (by offsetof), each enum's values
 *   against the C enum, and each zero field against def, as open() resolves
 *   it and as a 20-trial run, under QUEST+ and under QUEST's quantile
 *   placement; and the snapshot's magic. 7 of 7 mutants killed
 *   (tests/mutate/quest.toml). tests/fuzz/adapt_fuzz.c now feeds the
 *   snapshot reader; docs/adapt.md has its run.
 *
 *   Clean under gcc 11.4 -fsanitize=address,undefined, and with YQST_ASYNC
 *   also under -fsanitize=thread. Compiles and runs warning-free as C99, C11
 *   and C++17 under gcc 11.4 with -Wall -Wextra -Wpedantic -Wshadow -Werror,
 *   at -O2 and at -O3, with and without YQST_ASYNC. Also built and run on
 *   Windows 11 with MSVC 19.44 (VS 2022) under /W4 /WX, in its default C
 *   dialect and as C++, both ways: the test prints the same passes and the
 *   same simulation numbers as gcc, and the four examples build and exit 0.
 *
 *   The cost numbers under MEMORY, COST AND THREADS come from
 *   examples/quest/bench.c on one x86-64 laptop (an i7-1360P under WSL2),
 *   measured on v0.5.2; the two lower-precision variants under PRECISION were
 *   measured on the same laptop at v0.3.1 and then removed. They are the only
 *   measurement of cost there is.
 *
 *   Compared cell by cell against the reference implementations, through the
 *   MEX binding in MATLAB R2023a (tests/compare/compare_quest_mquestplus.m).
 *   Against mQUESTPlus, which drives each run so that both are updated with
 *   the stimulus it chose, on the paper's figure 2 threshold (32 trials),
 *   figure 3 threshold, slope and lapse (200), figure 4's normal through
 *   desc.pf_batch on qpPFNormal (128), and a marginalized case with the
 *   nuisance axis in the middle (64): no selection differs. 32 selections are
 *   ties within 6.9e-10 bits, which this header resolves to the lowest index
 *   under desc.tie_tolerance where mQUESTPlus takes the strict argmin, and in
 *   all of them the two picked the same stimulus. The posteriors agree within
 *   4.3e-7, which is the float table (LAYOUT). Against Palamedes' PAL_AMPM,
 *   Psi and Psi-marginal: identical selections over 60 trials each, posteriors
 *   within 5.7e-8. And Watson's own QUEST+ notebook runs replay identically, 17
 *   of 17 (tests/compare/methods_compare.py --replay).
 *
 *   NOT yet verified: Psychtoolbox's Quest, the one reference implementation
 *   not compared; any platform but x86-64 (the header has no intrinsics, so
 *   vectorization elsewhere is the compiler's business, but it is untested on
 *   ARM).
 *
 *   ---------------------------------------------------------------------
 *   LICENSE: public domain / MIT-0, see end of file.
 */
#ifndef YSP_QUEST_H_INCLUDED
#define YSP_QUEST_H_INCLUDED

/* The version of this header, for a log line or a compile-time check. The
 * string is the three numbers, and the test asserts that it stays so. */
#define YQST_VERSION_MAJOR  0
#define YQST_VERSION_MINOR  7
#define YQST_VERSION_PATCH  0
#define YQST_VERSION_STRING "0.7.0"

/* The one optional dependency, and it comes FIRST: ysp/rt.h sets a
 * feature-test macro for the Linux clock calls and can only do that before the
 * first system header. Without YQST_ASYNC this header includes nothing but the
 * C standard library, which is what the rest of the manual assumes; with it,
 * ysp/quest.h gains the async layer and ysp/rt.h comes with it. See ASYNC and
 * BUILDING. */
#ifdef YQST_ASYNC
    #ifdef YRT_NO_THREADS
        #error "ysp/quest.h: YQST_ASYNC is a thread, and YRT_NO_THREADS removes threads. Define one or the other, not both."
    #endif
    #include "ysp/rt.h"
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef YQST_API
#define YQST_API extern
#endif

/* Handle capacities; compile-time so the handle has a fixed size. */
#ifndef YQST_MAX_STIM_DIMS
#define YQST_MAX_STIM_DIMS 4
#endif
#ifndef YQST_MAX_PARAMS
#define YQST_MAX_PARAMS 8
#endif
#ifndef YQST_MAX_OUTCOMES
#define YQST_MAX_OUTCOMES 8
#endif
#ifndef YQST_MAX_TRIALS
#define YQST_MAX_TRIALS 2048
#endif

/* --- codes ------------------------------------------------------------- */

#define YQST_OK          0
#define YQST_ERR_ARG    (-1)  /* null handle, bad index, bad outcome, NaN    */
#define YQST_ERR_CLOSED (-2)  /* yqst_open() has not succeeded on this handle */
#define YQST_ERR_FULL   (-3)  /* YQST_MAX_TRIALS reached                     */
#define YQST_ERR_MEMORY (-4)  /* buffer too small or allocation failed       */

/* Static description of a YQST_ERR_* code ("ok" for values >= 0). */
YQST_API const char* yqst_strerror(int code);

/* YQST_VERSION_STRING, as compiled into the implementation. Beside the macro,
 * which is what the CALLER was compiled against, it tells a program that links
 * a prebuilt implementation which one it got. */
YQST_API const char* yqst_version(void);

/* --- description ------------------------------------------------------- */

/* One grid axis, for a stimulus dimension or a parameter. `values` (n of
 * them) when set, else linspace(lo, hi, n). n = 1 fixes a parameter. */
typedef struct yqst_axis {
    const double* values;   /* explicit grid, or NULL for a linspace        */
    double        lo, hi;   /* linspace bounds when values is NULL          */
    int           n;        /* points; >= 1                                 */
    const double* prior;    /* parameters only: n unnormalized weights, or
                             * NULL for uniform                             */
    bool          nuisance; /* parameters only: marginalize it out of the
                             * selection entropy and the stop criteria      */
} yqst_axis;

/* Axis constructors, for a designated-initializer-free desc build. */
YQST_API yqst_axis yqst_linspace(double lo, double hi, int n);
YQST_API yqst_axis yqst_values(const double* values, int n);
YQST_API yqst_axis yqst_fixed(double value);

/* Fill `out[n]` with an unnormalized Gaussian over the axis's points, for
 * yqst_axis.prior. Returns 0, or YQST_ERR_ARG for a null axis or `out`, an
 * invalid axis, or an sd that is not finite and positive. */
YQST_API int yqst_prior_normal(const yqst_axis* axis, double mean, double sd, double* out);

typedef enum yqst_pf {
    YQST_PF_GUMBEL = 0,   /* log-Weibull; QUEST's function (default)       */
    YQST_PF_WEIBULL,      /* Weibull on a linear, positive axis            */
    YQST_PF_LOGISTIC,
    YQST_PF_NORMAL,       /* cumulative Gaussian                           */
    YQST_PF_HYPSEC,       /* hyperbolic secant                             */
    YQST_PF_CUSTOM        /* desc.pf_fn                                    */
} yqst_pf;

/* Custom psychometric function, one cell at a time: write p[0..n_outcomes-1].
 * `stim` has n_stim values, `params` n_param values, in axis order. Must be a
 * pure function of its arguments. */
typedef void (*yqst_pf_fn)(void* ctx, const double* stim, const double* params, double* p);

/* Custom psychometric function, many cells at a time: fill
 * out[(s * P + i) * K + k] for every stimulus s in 0..S-1, every parameter row
 * i in 0..P-1 and every outcome k. `stims` is S rows of n_stim doubles,
 * `params` is P rows of n_param doubles; the columns of both are in the
 * caller's axis order. The rows of `params` are the header's own walk of the
 * parameter grid, which is the LAYOUT order unless a nuisance axis made the
 * header permute the grid, so treat them as a list of P parameter points and
 * nothing more: row i of the output belongs to row i of the input.
 *
 * This is the entry point for a vectorized model, and the one a binding wants:
 * it turns S*P calls into one per stimulus. The header calls it with S = 1,
 * once per stimulus at open and once per off-grid update; the S argument is
 * there because the contract is a batch, not because the header splits it
 * further. See PSYCHOMETRIC FUNCTIONS for which one to write. */
typedef void (*yqst_pf_batch_fn)(void* ctx, const double* stims, int S,
                                 const double* params, int P, float* out);

typedef enum yqst_select {
    YQST_SELECT_ENTROPY = 0, /* minimum expected posterior entropy (QUEST+) */
    YQST_SELECT_QUANTILE,    /* QUEST: posterior quantile of a parameter    */
    YQST_SELECT_MEAN,        /* posterior mean of a parameter               */
    YQST_SELECT_MODE         /* posterior mode of a parameter               */
} yqst_select;

typedef enum yqst_estimator {
    YQST_EST_MEAN = 0,       /* marginal means                              */
    YQST_EST_MODE,           /* joint mode                                  */
    YQST_EST_MEDIAN          /* marginal medians                            */
} yqst_estimator;

typedef enum yqst_tiebreak {
    YQST_TIE_LOWEST = 0,     /* lowest tied stimulus index                  */
    YQST_TIE_NEAREST,        /* nearest tied index to the last stimulus     */
    YQST_TIE_ALTERNATE,      /* lowest, then highest, alternating           */
    YQST_TIE_RANDOM          /* uniform among the tied, from desc.rng       */
} yqst_tiebreak;

/* A uniform variate in [0, 1) from the caller's generator. Optional; used
 * for YQST_TIE_RANDOM and for desc.subset_size. */
typedef double (*yqst_rng_fn)(void* ctx);

typedef enum yqst_stop {
    YQST_STOP_NONE = 0,
    YQST_STOP_TRIALS,
    YQST_STOP_ENTROPY,
    YQST_STOP_SD,
    YQST_STOP_FULL
} yqst_stop;

/* Run description. Zero-initialize it and set only what you need.
 * Required: n_stim >= 1 with stim[0..n_stim-1], n_param >= 1 with
 * param[0..n_param-1] (exactly 4 for a built-in pf), and at least one stop
 * criterion. YQST_PF_CUSTOM also needs pf_fn and n_outcomes. */
typedef struct yqst_desc {
    yqst_axis   stim[YQST_MAX_STIM_DIMS];
    int         n_stim;
    yqst_axis   param[YQST_MAX_PARAMS];
    int         n_param;
    const double* joint_prior;  /* P weights in LAYOUT order, or NULL       */
    yqst_pf     pf;             /* GUMBEL (0) by default                    */
    yqst_pf_fn  pf_fn;          /* YQST_PF_CUSTOM only: one cell per call   */
    yqst_pf_batch_fn pf_batch;  /* YQST_PF_CUSTOM only: a batch per call.
                                 * Set exactly one of pf_fn and pf_batch.   */
    void*       pf_ctx;         /* passed to pf_fn and to pf_batch          */
    int         n_outcomes;     /* YQST_PF_CUSTOM only; built-ins are 2     */
    yqst_select select;         /* ENTROPY (0) by default                   */
    int         select_param;   /* QUANTILE/MEAN/MODE: which parameter      */
    double      select_quantile;/* QUANTILE: 0 = 0.5                        */
    yqst_tiebreak tiebreak;     /* LOWEST (0) by default; see TIES          */
    double      tie_tolerance;  /* bits; 0 = 1e-9                           */
    yqst_rng_fn rng;            /* caller's generator, or NULL              */
    void*       rng_ctx;
    int         subset_size;    /* score this many random stimuli per next;
                                 * 0 = all. Needs rng.                      */
    int         stop_trials;    /* 0 = off                                  */
    double      stop_entropy;   /* bits; 0 = off                            */
    double      stop_sd;        /* 0 = off                                  */
    int         stop_sd_param;  /* which parameter stop_sd watches          */
    bool        no_table;       /* evaluate the pf per trial, keep no table */
    void*       memory;         /* caller's buffer of memory_size bytes, or
                                 * NULL to take one from YQST_MALLOC        */
    size_t      memory_size;
} yqst_desc;

/* --- handle ------------------------------------------------------------ */

/* One recorded trial. The stimulus is stored by value so an off-grid
 * update is recorded faithfully; stim_index is -1 for those. */
typedef struct yqst_trial {
    double  stim[YQST_MAX_STIM_DIMS];
    int     stim_index;
    int     proposed_index;  /* what yqst_next() had returned, or -1        */
    uint8_t outcome;
} yqst_trial;

/* Handle. The caller allocates it and treats every field as opaque. Must be
 * zeroed or closed before yqst_open(). */
typedef struct yqst_quest {
    yqst_desc  desc;
    int        S, P, K;            /* joint grid sizes and outcome count    */
    int        n_stim_axis[YQST_MAX_STIM_DIMS];
    int        n_param_axis[YQST_MAX_PARAMS];
    /* Everything below `mem` points into one block of `mem_size` bytes. */
    void*      mem;
    size_t     mem_size;
    bool       mem_owned;
    float* table;        /* L[S][rows][P], or NULL under no_table  */
    int        rows;               /* rows per stimulus: one per outcome, and
                                    * one more for the cell outcome entropy
                                    * where that row is kept                 */
    double*    cache;              /* 2 doubles: the posterior's entropy and
                                    * whether it is still valid              */
    double*    posterior;          /* P, normalized, INTERNAL axis order     */
    double*    post_public;        /* P in the caller's axis order, filled on
                                    * demand; NULL when the orders agree     */
    double*    scratch;            /* K*P likelihoods, row per outcome, for
                                    * the no-table and off-grid paths        */
    double*    param_matrix;       /* P rows of n_param, in the order the
                                    * batch callback is handed them; NULL
                                    * unless desc.pf_batch is set            */
    float*     batch_out;          /* P*K, what the batch callback writes    */
    double*    marginal_scratch;   /* the larger of n_marg and the longest
                                    * parameter axis                         */
    double*    scores;             /* S selection scores                    */
    double*    stim_values;        /* concatenated axis values              */
    double*    param_values;
    int*       subset_scratch;     /* subset_size indices                   */
    int        n_marg;             /* points in the non-nuisance marginal   */
    int        nuis_block;         /* points per nuisance block; n_marg times
                                    * this is P                              */
    int        perm[YQST_MAX_PARAMS];     /* internal axis -> public axis    */
    int        inv_perm[YQST_MAX_PARAMS]; /* public axis -> internal axis    */
    int        n_param_int[YQST_MAX_PARAMS]; /* axis sizes, internal order   */
    bool       permuted;           /* the two orders differ                 */
    bool       has_nuisance;
    int        proposed;           /* last yqst_next() result, or -1        */
    int        last_shown;         /* stimulus index of the last update, -1 */
    int        tie_parity;         /* YQST_TIE_ALTERNATE state              */
    int        n_trials;
    yqst_stop  stop;
    bool       open;
    yqst_trial history[YQST_MAX_TRIALS];
    char       error[256];
} yqst_quest;

/* --- lifecycle --------------------------------------------------------- */

/* Bytes yqst_open() needs for `desc`, or 0 when the desc is invalid. Use it
 * to size desc.memory, or to decide that no_table is required. The trial
 * history is not in there: it is inline in the handle. */
YQST_API size_t yqst_memory_size(const yqst_desc* desc);

/* Validate `desc`, take memory, build the table (unless no_table), install
 * the prior. Returns false with yqst_error() set on any failure; clears the
 * message on entry. Cost: S*P pf evaluations. */
YQST_API bool yqst_open(yqst_quest* q, const yqst_desc* desc);

/* Release what open() allocated (nothing when the caller gave memory). Safe
 * on a zeroed or closed handle, and on the same handle twice. Leaves the last
 * yqst_error() message readable. */
YQST_API void yqst_close(yqst_quest* q);

/* The message of the last failed open or handle-changing call ("" if none).
 * See RETURN VALUES AND ERRORS. */
YQST_API const char* yqst_error(const yqst_quest* q);
YQST_API bool        yqst_is_open(const yqst_quest* q);

/* --- grids ------------------------------------------------------------- */

/* Grid sizes, or YQST_ERR_CLOSED on a handle that is not open. */
YQST_API int yqst_n_stim(const yqst_quest* q);      /* S */
YQST_API int yqst_n_param(const yqst_quest* q);     /* P */
YQST_API int yqst_n_outcomes(const yqst_quest* q);  /* K */

/* Value of stimulus axis `axis` at stimulus grid point `index`; NaN on a bad
 * argument. yqst_stim_values() fills all n_stim of them and returns how many
 * it wrote, or a negative YQST_ERR_*. */
YQST_API double yqst_stim_value(const yqst_quest* q, int index, int axis);
YQST_API int    yqst_stim_values(const yqst_quest* q, int index, double* out);

/* Flat stimulus index of per-axis indices `sub[n_stim]`, or YQST_ERR_ARG.
 * yqst_stim_nearest() finds the grid point nearest a value on each axis. */
YQST_API int yqst_stim_index(const yqst_quest* q, const int* sub);
YQST_API int yqst_stim_nearest(const yqst_quest* q, const double* stim);

/* The same for the parameter grid; yqst_param_values() returns n_param. */
YQST_API double yqst_param_value(const yqst_quest* q, int index, int axis);
YQST_API int    yqst_param_values(const yqst_quest* q, int index, double* out);
YQST_API int    yqst_param_index(const yqst_quest* q, const int* sub);

/* Points on one axis, in the caller's axis order: stimulus axis `axis` in
 * 0..n_stim-1, parameter axis `axis` in 0..n_param-1. Returns n (>= 1),
 * YQST_ERR_ARG for a null handle or an axis out of range, or YQST_ERR_CLOSED on
 * a handle that is not open. The product over the stimulus axes is
 * yqst_n_stim(), over the parameter axes yqst_n_param(). */
YQST_API int yqst_stim_axis_n(const yqst_quest* q, int axis);
YQST_API int yqst_param_axis_n(const yqst_quest* q, int axis);

/* --- the loop ---------------------------------------------------------- */

/* Stimulus grid index to show next, per desc.select, or a negative
 * YQST_ERR_*. Cost: one pass over the table. Calling it twice without an
 * update returns the same value without recomputing. */
YQST_API int yqst_next(yqst_quest* q);

/* yqst_next() restricted to the `n` stimulus indices in `subset`, 1 <= n <= S.
 * Always recomputes, and its answer becomes the proposal yqst_next() would
 * repeat. Duplicates in `subset` are allowed and change nothing. */
YQST_API int yqst_next_subset(yqst_quest* q, const int* subset, int n);

/* Expected posterior entropy (bits) after showing stimulus `index`, the
 * quantity YQST_SELECT_ENTROPY minimizes, marginalized over the nuisance axes
 * when there are any. For a plot of the selection landscape; costs one pass
 * over that stimulus's P*(K+1) table cells, and, the first time after an
 * update, one pass of P logarithms for the constant the selection itself does
 * not need. NaN on a bad argument. */
YQST_API double yqst_expected_entropy(const yqst_quest* q, int index);

/* Record outcome `outcome` for the stimulus at grid index `index` and update
 * the posterior. Returns 0, or a negative YQST_ERR_*, in which case nothing
 * changed and nothing was recorded. Cost: two passes over the one contiguous
 * P-cell row of that stimulus and outcome. */
YQST_API int yqst_update(yqst_quest* q, int index, int outcome);

/* The same for a stimulus given by value, on or off the grid. The trial is
 * recorded with stim_index = -1 whether or not the value is a grid point.
 * Cost: P pf evaluations plus P*K. */
YQST_API int yqst_update_values(yqst_quest* q, const double* stim, int outcome);

/* True once a stop criterion has fired, which is tested at open() and after
 * every update and does not un-fire; yqst_stop_reason() says which one. */
YQST_API bool      yqst_done(const yqst_quest* q);
YQST_API yqst_stop yqst_stop_reason(const yqst_quest* q);

/* --- estimates --------------------------------------------------------- */

/* Fill out[n_param] per `how`: marginal means, the joint mode's coordinates,
 * or marginal medians. Returns 0 or YQST_ERR_*. */
YQST_API int yqst_estimate(const yqst_quest* q, yqst_estimator how, double* out);

/* Marginal posterior quantile `p` in (0, 1), marginal sd, and the full
 * marginal of parameter `axis`. The quantile is a grid point, the lowest whose
 * cumulative mass reaches p, not an interpolation. yqst_marginal() writes the
 * n points of that axis and returns n; the other two return NaN on a bad
 * argument. */
YQST_API double yqst_quantile(const yqst_quest* q, int axis, double p);
YQST_API double yqst_sd(const yqst_quest* q, int axis);
YQST_API int    yqst_marginal(const yqst_quest* q, int axis, double* out);

/* The joint posterior, P doubles in LAYOUT order. Valid until the next
 * update or close. Read-only. */
YQST_API const double* yqst_posterior(const yqst_quest* q);

/* Entropy of the posterior in bits, marginalized over nuisance axes. */
YQST_API double yqst_entropy(const yqst_quest* q);

/* --- model ------------------------------------------------------------- */

/* Evaluate the psychometric function at stimulus grid point `index` and the
 * given parameter values (any values, not only grid points). Writes K
 * probabilities to p_out. Returns 0 or YQST_ERR_*. */
YQST_API int yqst_p(const yqst_quest* q, int index, const double* params, double* p_out);

/* The same at an arbitrary stimulus value. */
YQST_API int yqst_p_values(const yqst_quest* q, const double* stim, const double* params, double* p_out);

/* Draw an outcome for stimulus `index` under `params` with the uniform
 * variate `u` in [0, 1): the smallest k with cumulative p > u, or a negative
 * YQST_ERR_* (u outside [0, 1) is YQST_ERR_ARG). */
YQST_API int yqst_simulate(const yqst_quest* q, int index, const double* params, double u);

/* --- history ----------------------------------------------------------- */

/* Trials recorded so far, and the array of them (NULL and *n = 0 on a handle
 * that is not open). The array is valid until close(). */
YQST_API int               yqst_n_trials(const yqst_quest* q);
YQST_API const yqst_trial* yqst_history(const yqst_quest* q, int* n);

/* --- snapshot ---------------------------------------------------------- */

/* Save a session and resume it later without replaying it. yqst_save_size()
 * is the bytes yqst_save() writes (0 on a handle that is not open);
 * yqst_save() writes them and returns the count, or YQST_ERR_ARG when `cap`
 * is too small, or YQST_ERR_CLOSED. yqst_open_saved() opens `q` from `desc` exactly
 * as yqst_open() would, checks that the desc agrees with the snapshot, and
 * then puts the posterior, the history, the pending proposal and the tie
 * state back, so the next yqst_next() is the one the saved session would have
 * made. On any failure it returns false, leaves `q` closed, and says why in
 * yqst_error(). It opens, so close `q` first if it is open, and close it
 * when done. SNAPSHOTS in the manual gives the layout and the rules. */
YQST_API size_t yqst_save_size(const yqst_quest* q);
YQST_API int    yqst_save(const yqst_quest* q, void* buf, size_t cap);
YQST_API bool   yqst_open_saved(yqst_quest* q, const yqst_desc* desc, const void* buf, size_t len);

/* --- parameters -------------------------------------------------------- */

/* One settable yqst_desc field. PARAMETERS in the manual gives the rules;
 * ysp/stair.h and ysp/aep.h have tables of the same shape. */
typedef struct yqst_param {
    const char* name;    /* the desc field; "a.b" a member, "a[]" an array   */
    const char* type;    /* "i32", "f64", "bool", "enum", "f64[]"            */
    double      min, max;/* inclusive                                        */
    double      def;     /* the value a zero field means                     */
    const char* unit;    /* "bit", or "" for the caller's units or a count   */
    const char* doc;
    const char* values;  /* "enum": "name=value,..." in value order; NULL    */
} yqst_param;

/* The table of desc fields a definition or a designer sets; *n gets the
 * count. Static: the pointer stays valid for the life of the program. */
YQST_API const yqst_param* yqst_params(int* n);

/* --- async ------------------------------------------------------------- *
 *  Compiled only under YQST_ASYNC. See ASYNC in the manual.
 * ----------------------------------------------------------------------- */
#ifdef YQST_ASYNC

/* Two more codes, which only the async calls return. */
#define YQST_ERR_BUSY    (-5)  /* the queue is full; the caller keeps its
                                * response and retries next frame           */
#define YQST_ERR_TIMEOUT (-6)  /* yqst_async_wait ran out of time           */

/* Responses the queue holds. One trial each; a caller that submits more than
 * this many before the thread drains gets YQST_ERR_BUSY. The ring lives inside
 * yqst_async, so this sizes the handle. */
#ifndef YQST_ASYNC_QUEUE
#define YQST_ASYNC_QUEUE 8
#endif

/* What the thread publishes after every update, and yqst_async_poll() and
 * yqst_async_wait() hand back. A plain struct, copied out under the pump's
 * lock; nothing in it points anywhere. */
typedef struct yqst_snapshot {
    uint32_t  seq;         /* the submit this accounts for: every update
                            * through this seq is applied and `proposed` was
                            * computed after them. 0 is the proposal made at
                            * yqst_async_open(), before any response.      */
    int       proposed;    /* stimulus grid index for the next trial, or a
                            * negative YQST_ERR_* if the selection failed   */
    double    stim[YQST_MAX_STIM_DIMS];  /* its values, as yqst_stim_values */
    int       update_rc;   /* what yqst_update() returned for this response:
                            * 0, or the code that made it refuse (a full
                            * history, an impossible outcome). Not part of
                            * the seq contract; it is here so a refusal on
                            * the thread cannot pass unnoticed.             */
    int       n_trials;
    bool      done;
    yqst_stop stop;
    double    estimate[YQST_MAX_PARAMS];  /* per desc.estimator            */
    double    entropy;     /* bits, as yqst_entropy()                       */
    double    sd;          /* posterior sd of parameter 0                   */
} yqst_snapshot;

/* Async description. Zero-initialize it and set only what you need; `quest` is
 * required and must already be open. */
typedef struct yqst_async_desc {
    yqst_quest*    quest;      /* the handle the thread drives. The layer
                                * OWNS it from open to close: no yqst_*
                                * call on it meanwhile, from any thread.    */
    yqst_estimator estimator;  /* which estimate the snapshot carries;
                                * YQST_EST_MEAN (0) by default              */
    bool           below_normal; /* run the thread below normal priority     */
    int            pin_cpu;    /* logical CPU to pin it to; 0 = no pinning,
                                * as yrt_pump_desc.pin_cpu                */
    int            queue_depth; /* responses this session may have queued,
                                * 1..YQST_ASYNC_QUEUE; 0 = YQST_ASYNC_QUEUE.
                                * The macro is a capacity (it sizes the
                                * handle); this is a policy: how far the
                                * trial loop may run ahead of the inference.
                                * 1 keeps them in lockstep.                  */
} yqst_async_desc;

/* One trial's response on its way to the thread. Opaque; sized here because
 * the ring is inline. */
typedef struct yqst_async_msg {
    double stim[YQST_MAX_STIM_DIMS];
    int    stim_index;     /* -1 for a by-value submit */
    int    outcome;
} yqst_async_msg;

/* Async handle. The caller allocates it and treats every field as opaque. It
 * owns no heap: the pump, the response ring and the snapshot are all inline.
 * Must be zeroed or closed before yqst_async_open(). */
typedef struct yqst_async {
    yrt_pump     pump;
    yqst_quest*    quest;
    yqst_estimator estimator;
    yqst_async_msg ring[YQST_ASYNC_QUEUE];
    yqst_snapshot  snap;       /* guarded by yrt_pump_lock()              */
    bool           published;  /* a snapshot exists; under the same lock    */
    bool           running;
    char           error[256];
} yqst_async;

/* Start the thread and publish the first proposal. Returns true with a
 * snapshot already available (yqst_async_poll() returns 0 and fills it), false
 * with yqst_async_error() set: a null handle or desc, a quest that is not
 * open, an estimator out of range, or a pump the OS would not start; clears
 * the message on entry. The only async call that writes the message: submit
 * may run on any thread, so the others return a code and leave the message
 * alone rather than race on it (yqst_strerror() names the code). Open must
 * not race another call on the same handle. Cost: one yqst_next() on the
 * calling thread, before the thread exists. */
YQST_API bool yqst_async_open(yqst_async* a, const yqst_async_desc* desc);

/* Deliver every response already queued, then join the thread. After it the
 * quest handle is the caller's again and every yqst_* call on it is legal.
 * Safe on a zeroed handle, after a failed open and twice. Must not race another call on
 * the same handle, except a yqst_async_wait() already blocked, which comes out
 * with YQST_ERR_CLOSED. */
YQST_API void yqst_async_close(yqst_async* a);

/* Hand one trial to the thread: the stimulus by grid index, or by value for an
 * off-grid trial, and the outcome. Returns a POSITIVE seq, increasing, which
 * yqst_async_poll() and yqst_async_wait() compare against. Negative on
 * failure: YQST_ERR_ARG (null handle, index or outcome out of range, a
 * non-finite value), YQST_ERR_CLOSED (not started), YQST_ERR_BUSY (the queue
 * is full: NOTHING was copied, the caller still owns the response and should
 * retry it on the next frame). Callable from any thread. Copies the response
 * and returns; it does not wait for the inference. */
YQST_API int yqst_async_submit(yqst_async* a, int stim_index, int outcome);
YQST_API int yqst_async_submit_values(yqst_async* a, const double* stim, int outcome);

/* Copy the newest snapshot into `out` (which may be NULL to ask only for the
 * seq) and return the seq it accounts for: every response through that seq is
 * applied and its proposal is the one in the snapshot. 0 means the proposal
 * made at open, before any response had landed, so a caller that wants the
 * first stimulus reads it from a poll that returns 0. Negative on error. One
 * atomic load plus a struct copy under the pump's publish lock: cheap enough
 * for every frame. */
YQST_API int yqst_async_poll(const yqst_async* a, yqst_snapshot* out);

/* The same, after blocking until the thread has finished the response `seq`
 * or `timeout_ns` of monotonic time has passed. Returns the seq accounted for
 * (>= seq on success), YQST_ERR_TIMEOUT if the time ran out first,
 * YQST_ERR_CLOSED if the thread stopped without reaching it. timeout_ns is
 * relative, in ns (YRT_MS(20)); 0 or less polls. This is the end-of-interval
 * call; poll() is the frame loop's. */
YQST_API int yqst_async_wait(yqst_async* a, uint32_t seq, int64_t timeout_ns,
                             yqst_snapshot* out);

/* Responses queued and not yet applied (the one being worked on is not
 * counted), or a negative YQST_ERR_*. For a log line, not a frame loop: it
 * takes the queue's mutex. */
YQST_API int yqst_async_pending(const yqst_async* a);

/* Last failed yqst_async_open() message for this handle ("" if none). */
YQST_API const char* yqst_async_error(const yqst_async* a);

/* True while the thread is running. */
YQST_API bool yqst_async_is_running(const yqst_async* a);

/* What the thread runs at: YRT_POLICY_NORMAL, or YRT_POLICY_BELOW_NORMAL
 * when desc.below_normal was set AND the OS granted the drop, which is not an
 * error either way. YRT_POLICY_NONE before an open and after a close. Log it:
 * "the inference was ready in time" means something different at each rung. */
YQST_API yrt_policy yqst_async_policy(const yqst_async* a);

#endif /* YQST_ASYNC */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* YSP_QUEST_H_INCLUDED */

/* ======================================================================= *
 *                             IMPLEMENTATION                              *
 * ======================================================================= */
#ifdef YSP_QUEST_IMPLEMENTATION
#ifndef YSP_QUEST_IMPLEMENTATION_GUARD
#define YSP_QUEST_IMPLEMENTATION_GUARD

/* Under YQST_ASYNC, ysp/rt.h's implementation too, unless this translation
 * unit already has it. Its implementation block sits outside its header guard
 * and carries a guard of its own, so a unit that also defines
 * YSP_RT_IMPLEMENTATION, or that also implements a transport header, still ends
 * up with exactly one copy. */
#ifdef YQST_ASYNC
    #ifndef YSP_RT_IMPLEMENTATION_GUARD
        #define YSP_RT_IMPLEMENTATION
        #include "ysp/rt.h"
    #endif
#endif

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#if defined(YQST_MALLOC) != defined(YQST_FREE)
#error "ysp_quest: define both YQST_MALLOC and YQST_FREE, or neither"
#endif
#ifndef YQST_MALLOC
#include <stdlib.h>
#define YQST_MALLOC(n) malloc(n)
#define YQST_FREE(p)   free(p)
#endif

/* Every block in the arena is a double, a float or an int, so one alignment
 * covers them all and the layout arithmetic stays in one place. */
#define YQST__ALIGN    8
#define YQST__LOG2E    1.4426950408889634074
#define YQST__SQRT1_2  0.70710678118654752440
#define YQST__NAN      ((double)NAN)

/* ======================================================================= *
 *  SMALL UTILITIES
 * ======================================================================= */

/* isfinite() is a macro whose spelling has moved between C and C++ standard
 * libraries; two comparisons are portable and say the same thing. */
static bool yqst__finite(double v) {
    return (v == v) && (v <= DBL_MAX) && (v >= -DBL_MAX);
}

static double yqst__wlog2(double w) {
    /* p log p at p = 0 is 0, and a likelihood of exactly zero is common on a
     * grid, so this branch has to be here. After the decomposition below it
     * is no longer on the per-cell path. */
    return (w > 0.0) ? w * log(w) * YQST__LOG2E : 0.0;
}

static bool yqst__mul_ok(size_t a, size_t b, size_t* out) {
    if (a != 0 && b > (size_t)-1 / a) return false;
    *out = a * b;
    return true;
}

static double yqst__axis_at(const yqst_axis* a, int i) {
    if (a->values) return a->values[i];
    if (a->n <= 1) return a->lo;
    return a->lo + (a->hi - a->lo) * ((double)i / (double)(a->n - 1));
}

/* ======================================================================= *
 *  DOT PRODUCTS
 *
 *  Every sweep in this header is a dot product of a contiguous likelihood row
 *  against the posterior, which is why the table is stored transposed. Four
 *  independent accumulators: a reduction written with one accumulator is a
 *  serial dependency chain, and neither gcc nor MSVC may reassociate it
 *  without being told that floating-point addition is associative, which it
 *  is not. The split is what lets them vectorize the loop, and where they do
 *  not it still gives four adds in flight instead of one. The summation order
 *  is fixed by the code, so the result does not depend on what the compiler
 *  chose to do.
 * ======================================================================= */

static double yqst__dot_f(const double* a, const float* b, int n) {
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
    int i, m = n & ~3;
    for (i = 0; i < m; i += 4) {
        s0 += a[i]     * (double)b[i];
        s1 += a[i + 1] * (double)b[i + 1];
        s2 += a[i + 2] * (double)b[i + 2];
        s3 += a[i + 3] * (double)b[i + 3];
    }
    for (; i < n; i++) s0 += a[i] * (double)b[i];
    return (s0 + s1) + (s2 + s3);
}

static double yqst__dot_d(const double* a, const double* b, int n) {
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
    int i, m = n & ~3;
    for (i = 0; i < m; i += 4) {
        s0 += a[i]     * b[i];
        s1 += a[i + 1] * b[i + 1];
        s2 += a[i + 2] * b[i + 2];
        s3 += a[i + 3] * b[i + 3];
    }
    for (; i < n; i++) s0 += a[i] * b[i];
    return (s0 + s1) + (s2 + s3);
}

/* acc[i] += a[i] * b[i], the marginalized sweep's inner loop. One long
 * contiguous pass per nuisance point, which is the shape that vectorizes;
 * the accumulator is the marginal itself, so nothing is summed horizontally. */
static void yqst__fma_f(double* acc, const double* a, const float* b, int n) {
    int i;
    for (i = 0; i < n; i++) acc[i] += a[i] * (double)b[i];
}

static void yqst__fma_d(double* acc, const double* a, const double* b, int n) {
    int i;
    for (i = 0; i < n; i++) acc[i] += a[i] * b[i];
}

/* The posterior update's second pass: a[i] *= b[i] * scale, contiguous. */
static void yqst__scale_f(double* a, const float* b, double scale, int n) {
    int i;
    for (i = 0; i < n; i++) a[i] = a[i] * (double)b[i] * scale;
}

static void yqst__scale_d(double* a, const double* b, double scale, int n) {
    int i;
    for (i = 0; i < n; i++) a[i] = a[i] * b[i] * scale;
}

/* ======================================================================= *
 *  DESCRIPTION: VALIDATION AND SIZES
 * ======================================================================= */

typedef struct yqst__sizes {
    int    S, P, K, n_param_axes;
    int    rows;            /* table rows per stimulus: K, or K+1 with the
                             * tabulated outcome entropy                     */
    int    n_marg;          /* points in the non-nuisance parameter marginal */
    int    nuis_block;      /* points in one nuisance block; n_marg * this = P */
    int    n_subset;        /* subset_scratch entries                        */
    int    perm[YQST_MAX_PARAMS];  /* internal axis -> public axis           */
    bool   permuted;        /* perm is not the identity                      */
    bool   batch;           /* desc.pf_batch is the model                    */
    size_t n_stim_vals;     /* concatenated stimulus axis points             */
    size_t n_param_vals;
    size_t table_cells;     /* S*rows*P, or 0 under no_table                 */
    size_t marg_doubles;    /* marginal_scratch entries                      */
} yqst__sizes;

/* err may be NULL: yqst_memory_size() validates without a message buffer. */
#define YQST__BAD(...) \
    do { if (err && cap) snprintf(err, cap, __VA_ARGS__); return false; } while (0)

static bool yqst__axis_ok(const yqst_axis* a, bool is_param,
                          const char* what, int which, char* err, size_t cap) {
    int i;
    double s = 0.0;
    if (a->n < 1) YQST__BAD("ysp_quest: %s axis %d has n = %d, need >= 1", what, which, a->n);
    if (a->values) {
        for (i = 0; i < a->n; i++)
            if (!yqst__finite(a->values[i]))
                YQST__BAD("ysp_quest: %s axis %d value %d is not finite", what, which, i);
    } else if (!yqst__finite(a->lo) || !yqst__finite(a->hi)) {
        YQST__BAD("ysp_quest: %s axis %d has a non-finite lo or hi", what, which);
    }
    if (a->prior) {
        if (!is_param) YQST__BAD("ysp_quest: %s axis %d has a prior; only parameters do", what, which);
        for (i = 0; i < a->n; i++) {
            if (!yqst__finite(a->prior[i]) || a->prior[i] < 0.0)
                YQST__BAD("ysp_quest: %s axis %d prior weight %d is negative or not finite", what, which, i);
            s += a->prior[i];
        }
        if (!(s > 0.0)) YQST__BAD("ysp_quest: %s axis %d prior sums to zero", what, which);
    }
    if (a->nuisance && !is_param)
        YQST__BAD("ysp_quest: stimulus axis %d is flagged nuisance; only parameters can be", which);
    return true;
}

static bool yqst__sizes_of(const yqst_desc* d, yqst__sizes* z, char* err, size_t cap) {
    int i, max_axis = 1;
    long long S = 1, P = 1, M = 1, B = 1;
    size_t bytes;

    memset(z, 0, sizeof(*z));
    if (!d) YQST__BAD("ysp_quest: null desc");
    if (d->n_stim < 1 || d->n_stim > YQST_MAX_STIM_DIMS)
        YQST__BAD("ysp_quest: n_stim = %d, need 1..%d", d->n_stim, YQST_MAX_STIM_DIMS);
    if (d->n_param < 1 || d->n_param > YQST_MAX_PARAMS)
        YQST__BAD("ysp_quest: n_param = %d, need 1..%d", d->n_param, YQST_MAX_PARAMS);
    if ((int)d->pf < 0 || (int)d->pf > (int)YQST_PF_CUSTOM)
        YQST__BAD("ysp_quest: desc.pf is out of range");

    for (i = 0; i < d->n_stim; i++) {
        if (!yqst__axis_ok(&d->stim[i], false, "stimulus", i, err, cap)) return false;
        S *= d->stim[i].n;
        z->n_stim_vals += (size_t)d->stim[i].n;
        if (S > 0x7fffffffLL) YQST__BAD("ysp_quest: the stimulus grid exceeds 2^31 points");
    }
    for (i = 0; i < d->n_param; i++) {
        if (!yqst__axis_ok(&d->param[i], true, "parameter", i, err, cap)) return false;
        P *= d->param[i].n;
        z->n_param_vals += (size_t)d->param[i].n;
        if (d->param[i].nuisance) B *= d->param[i].n; else M *= d->param[i].n;
        if (d->param[i].n > max_axis) max_axis = d->param[i].n;
        if (P > 0x7fffffffLL) YQST__BAD("ysp_quest: the parameter grid exceeds 2^31 points");
    }

    if (d->pf == YQST_PF_CUSTOM) {
        if (!d->pf_fn && !d->pf_batch)
            YQST__BAD("ysp_quest: YQST_PF_CUSTOM needs desc.pf_fn or desc.pf_batch");
        if (d->pf_fn && d->pf_batch)
            YQST__BAD("ysp_quest: set exactly one of desc.pf_fn and desc.pf_batch, not both");
        z->batch = (d->pf_batch != NULL);
        if (d->n_outcomes < 2 || d->n_outcomes > YQST_MAX_OUTCOMES)
            YQST__BAD("ysp_quest: n_outcomes = %d, need 2..%d", d->n_outcomes, YQST_MAX_OUTCOMES);
        z->K = d->n_outcomes;
    } else {
        if (d->n_param != 4)
            YQST__BAD("ysp_quest: a built-in psychometric function needs n_param = 4, got %d", d->n_param);
        if (d->n_outcomes != 0 && d->n_outcomes != 2)
            YQST__BAD("ysp_quest: a built-in psychometric function has 2 outcomes, not %d", d->n_outcomes);
        z->K = 2;
        /* The Weibull is undefined off the positive axis, and a grid point
         * that cannot be evaluated is a desc error, not a run-time NaN. */
        if (d->pf == YQST_PF_WEIBULL) {
            for (i = 0; i < d->stim[0].n; i++)
                if (!(yqst__axis_at(&d->stim[0], i) > 0.0))
                    YQST__BAD("ysp_quest: YQST_PF_WEIBULL needs stimulus axis 0 > 0 (point %d is not)", i);
            for (i = 0; i < d->param[0].n; i++)
                if (!(yqst__axis_at(&d->param[0], i) > 0.0))
                    YQST__BAD("ysp_quest: YQST_PF_WEIBULL needs alpha > 0 (parameter point %d is not)", i);
        }
    }

    if (d->joint_prior) {
        double s = 0.0;
        long long t;
        for (t = 0; t < P; t++) {
            if (!yqst__finite(d->joint_prior[t]) || d->joint_prior[t] < 0.0)
                YQST__BAD("ysp_quest: joint_prior[%lld] is negative or not finite", t);
            s += d->joint_prior[t];
        }
        if (!(s > 0.0)) YQST__BAD("ysp_quest: joint_prior sums to zero");
    }

    if ((int)d->select < 0 || (int)d->select > (int)YQST_SELECT_MODE)
        YQST__BAD("ysp_quest: desc.select is out of range");
    if (d->select != YQST_SELECT_ENTROPY) {
        if (d->select_param < 0 || d->select_param >= d->n_param)
            YQST__BAD("ysp_quest: select_param = %d, need 0..%d", d->select_param, d->n_param - 1);
        if (d->select_quantile != 0.0 &&
            (!yqst__finite(d->select_quantile) || d->select_quantile <= 0.0 || d->select_quantile >= 1.0))
            YQST__BAD("ysp_quest: select_quantile must be in (0, 1) or 0 for 0.5");
    }
    if ((int)d->tiebreak < 0 || (int)d->tiebreak > (int)YQST_TIE_RANDOM)
        YQST__BAD("ysp_quest: desc.tiebreak is out of range");
    if (!yqst__finite(d->tie_tolerance) || d->tie_tolerance < 0.0)
        YQST__BAD("ysp_quest: tie_tolerance must be finite and >= 0");
    if (d->tiebreak == YQST_TIE_RANDOM && !d->rng)
        YQST__BAD("ysp_quest: YQST_TIE_RANDOM needs desc.rng");
    if (d->subset_size < 0) YQST__BAD("ysp_quest: subset_size must be >= 0");
    if (d->subset_size > 0 && !d->rng)
        YQST__BAD("ysp_quest: subset_size needs desc.rng");
    if (d->stop_trials < 0) YQST__BAD("ysp_quest: stop_trials must be >= 0");
    if (!yqst__finite(d->stop_entropy) || d->stop_entropy < 0.0)
        YQST__BAD("ysp_quest: stop_entropy must be finite and >= 0");
    if (!yqst__finite(d->stop_sd) || d->stop_sd < 0.0)
        YQST__BAD("ysp_quest: stop_sd must be finite and >= 0");
    if (d->stop_trials == 0 && d->stop_entropy == 0.0 && d->stop_sd == 0.0)
        YQST__BAD("ysp_quest: no stop criterion; set stop_trials, stop_entropy or stop_sd");
    if (d->stop_sd > 0.0 && (d->stop_sd_param < 0 || d->stop_sd_param >= d->n_param))
        YQST__BAD("ysp_quest: stop_sd_param = %d, need 0..%d", d->stop_sd_param, d->n_param - 1);
    if (d->memory && d->memory_size == 0)
        YQST__BAD("ysp_quest: desc.memory is set but memory_size is 0");

    z->S = (int)S;
    z->P = (int)P;
    z->n_param_axes = d->n_param;
    z->n_marg = (int)M;
    z->nuis_block = (int)B;
    z->n_subset = (d->subset_size > 0) ? ((d->subset_size < z->S) ? d->subset_size : z->S) : 0;

    /* Internal parameter axis order: the nuisance axes FIRST, so the axes
     * that survive into the marginal are the fastest-varying ones and the
     * marginalized sweep accumulates into a contiguous n_marg-long vector
     * instead of summing a short block per marginal cell. That is the shape
     * a compiler can vectorize: one long loop, no horizontal sums. A nuisance
     * axis of one point marginalizes over nothing, so a desc without a real
     * nuisance axis keeps the caller's order and pays no translation at the
     * boundary. */
    {
        int c = 0, j;
        if (B > 1) {
            for (j = 0; j < d->n_param; j++) if (d->param[j].nuisance)  z->perm[c++] = j;
            for (j = 0; j < d->n_param; j++) if (!d->param[j].nuisance) z->perm[c++] = j;
        } else {
            for (j = 0; j < d->n_param; j++) z->perm[j] = j;
        }
        for (j = 0; j < d->n_param; j++) if (z->perm[j] != j) z->permuted = true;
    }

    /* The outcome entropy of a cell is a property of the table, so tabulating
     * it takes the logarithms out of the per-trial sweep. The marginalized
     * selection takes its logarithms on the marginal instead and has no use
     * for it, so that configuration does not pay the extra row. */
    z->rows = (!d->no_table && B == 1) ? z->K + 1 : z->K;

    z->marg_doubles = (size_t)((z->n_marg > max_axis) ? z->n_marg : max_axis);

    if (!yqst__mul_ok((size_t)z->P, (size_t)z->K, &bytes) ||
        !yqst__mul_ok(bytes, sizeof(double), &bytes) || bytes > (size_t)-1 / 16)
        YQST__BAD("ysp_quest: the parameter grid is too large for this machine");
    if (!d->no_table) {
        if (!yqst__mul_ok((size_t)z->S, (size_t)z->rows, &z->table_cells) ||
            !yqst__mul_ok(z->table_cells, (size_t)z->P, &z->table_cells) ||
            !yqst__mul_ok(z->table_cells, sizeof(float), &bytes) || bytes > (size_t)-1 / 16)
            YQST__BAD("ysp_quest: the likelihood table is too large for this machine; set desc.no_table");
    }
    return true;
}

#undef YQST__BAD

static size_t yqst__bump(size_t* off, size_t bytes) {
    size_t at = (*off + (YQST__ALIGN - 1)) & ~(size_t)(YQST__ALIGN - 1);
    *off = at + bytes;
    return at;
}

/* The one place the arena layout is written down. With base == NULL it only
 * measures, which is what yqst_memory_size() reports. The float table is last
 * of the big blocks so the posterior and the scratch share the first pages
 * and the per-trial sweep runs the table front to back. */
static size_t yqst__plan(const yqst__sizes* z, unsigned char* base, yqst_quest* q) {
    size_t off = 0;
    size_t o_cache = yqst__bump(&off, 2 * sizeof(double));
    size_t o_post  = yqst__bump(&off, (size_t)z->P * sizeof(double));
    size_t o_pub   = yqst__bump(&off, (z->permuted ? (size_t)z->P : 0) * sizeof(double));
    size_t o_scr   = yqst__bump(&off, (size_t)z->P * (size_t)z->K * sizeof(double));
    size_t o_pmat  = yqst__bump(&off, (z->batch ? (size_t)z->P * (size_t)z->n_param_axes : 0)
                                      * sizeof(double));
    size_t o_bout  = yqst__bump(&off, (z->batch ? (size_t)z->P * (size_t)z->K : 0)
                                      * sizeof(float));
    size_t o_marg  = yqst__bump(&off, z->marg_doubles * sizeof(double));
    size_t o_sco   = yqst__bump(&off, (size_t)z->S * sizeof(double));
    size_t o_sv    = yqst__bump(&off, z->n_stim_vals * sizeof(double));
    size_t o_pv    = yqst__bump(&off, z->n_param_vals * sizeof(double));
    size_t o_tab   = yqst__bump(&off, z->table_cells * sizeof(float));
    size_t o_sub   = yqst__bump(&off, (size_t)z->n_subset * sizeof(int));
    if (base && q) {
        q->cache            = (double*)(void*)(base + o_cache);
        q->posterior        = (double*)(void*)(base + o_post);
        q->post_public      = z->permuted ? (double*)(void*)(base + o_pub) : NULL;
        q->scratch          = (double*)(void*)(base + o_scr);
        q->param_matrix     = z->batch ? (double*)(void*)(base + o_pmat) : NULL;
        q->batch_out        = z->batch ? (float*)(void*)(base + o_bout) : NULL;
        q->marginal_scratch = (double*)(void*)(base + o_marg);
        q->scores           = (double*)(void*)(base + o_sco);
        q->stim_values      = (double*)(void*)(base + o_sv);
        q->param_values     = (double*)(void*)(base + o_pv);
        q->table            = z->table_cells ? (float*)(void*)(base + o_tab) : NULL;
        q->subset_scratch   = z->n_subset ? (int*)(void*)(base + o_sub) : NULL;
    }
    return off;
}

/* ======================================================================= *
 *  GRIDS
 *
 *  Public index arithmetic (yqst_param_index(), yqst_param_value(), the
 *  history, the order yqst_posterior() hands back) is in the caller's axis
 *  order. The posterior itself is stored in the internal order, which differs
 *  only when a nuisance axis is not already last. The two meet in exactly
 *  three places: the prior at open, yqst_posterior(), and the mode.
 * ======================================================================= */

/* The counts, clamped to the arrays they index. open() already guarantees
 * n_stim <= YQST_MAX_STIM_DIMS, n_param <= YQST_MAX_PARAMS and K <=
 * YQST_MAX_OUTCOMES, so at run time these are the counts themselves. They exist
 * for the optimizer, which cannot see what open() checked: without a bound it
 * can hand a constant from a caller's call site (a deliberately bad axis in a
 * test, a three-element array for a three-parameter model) into a path that
 * open()'s invariants make impossible, and then warn, or with
 * -Werror=aggressive-loop-optimizations refuse, on the impossible path. gcc 16
 * did exactly that. Every loop and index below that touches a fixed-size array
 * is bounded by one of these, or by the array's size directly. */
static int yqst__ns(const yqst_quest* q) {
    int n = q->desc.n_stim;
    return (n < 0) ? 0 : (n > YQST_MAX_STIM_DIMS ? YQST_MAX_STIM_DIMS : n);
}

static int yqst__np(const yqst_quest* q) {
    int n = q->desc.n_param;
    return (n < 0) ? 0 : (n > YQST_MAX_PARAMS ? YQST_MAX_PARAMS : n);
}

static int yqst__nk(const yqst_quest* q) {
    int n = q->K;
    return (n < 0) ? 0 : (n > YQST_MAX_OUTCOMES ? YQST_MAX_OUTCOMES : n);
}

/* A caller's axis argument, checked against the handle AND the array it will
 * index, so the out-of-range path is provably dead. */
static bool yqst__stim_axis_ok(const yqst_quest* q, int axis) {
    return axis >= 0 && axis < yqst__ns(q);
}

static bool yqst__param_axis_ok(const yqst_quest* q, int axis) {
    return axis >= 0 && axis < yqst__np(q);
}

static const double* yqst__stim_axis(const yqst_quest* q, int axis) {
    const double* p = q->stim_values;
    int j;
    for (j = 0; j < axis && j < YQST_MAX_STIM_DIMS; j++) p += q->n_stim_axis[j];
    return p;
}

static const double* yqst__param_axis(const yqst_quest* q, int axis) {
    const double* p = q->param_values;
    int j;
    for (j = 0; j < axis && j < YQST_MAX_PARAMS; j++) p += q->n_param_axis[j];
    return p;
}

/* Stride of an axis in LAYOUT order (last axis fastest). */
static int yqst__stim_stride(const yqst_quest* q, int axis) {
    int j, s = 1, n = yqst__ns(q);
    for (j = axis + 1; j < n; j++) s *= q->n_stim_axis[j];
    return s;
}

static int yqst__param_stride(const yqst_quest* q, int axis) {
    int j, s = 1, n = yqst__np(q);
    for (j = axis + 1; j < n; j++) s *= q->n_param_axis[j];
    return s;
}

/* The same for the internal order, where the posterior lives. */
static int yqst__int_stride(const yqst_quest* q, int axis) {
    int j, s = 1, n = yqst__np(q);
    for (j = axis + 1; j < n; j++) s *= q->n_param_int[j];
    return s;
}

static void yqst__stim_vec(const yqst_quest* q, int index, double* out) {
    int j, t = index;
    for (j = yqst__ns(q) - 1; j >= 0; j--) {
        int n = q->n_stim_axis[j];
        out[j] = yqst__stim_axis(q, j)[t % n];
        t /= n;
    }
}

static void yqst__param_vec(const yqst_quest* q, int index, double* out) {
    int j, t = index;
    for (j = yqst__np(q) - 1; j >= 0; j--) {
        int n = q->n_param_axis[j];
        out[j] = yqst__param_axis(q, j)[t % n];
        t /= n;
    }
}

/* An INTERNAL parameter index to the caller's parameter vector. */
static void yqst__param_vec_int(const yqst_quest* q, int index, double* out) {
    int j, t = index;
    for (j = yqst__np(q) - 1; j >= 0; j--) {
        int n = q->n_param_int[j];
        out[q->perm[j]] = yqst__param_axis(q, q->perm[j])[t % n];
        t /= n;
    }
}

/* Internal order to public order, P values. */
static void yqst__to_public(const yqst_quest* q, const double* src, double* dst) {
    int idx[YQST_MAX_PARAMS], pub_stride[YQST_MAX_PARAMS];
    int np = yqst__np(q), j, t, pub = 0;
    for (j = 0; j < np; j++) pub_stride[j] = yqst__param_stride(q, j);
    memset(idx, 0, sizeof(idx));
    for (t = 0; t < q->P; t++) {
        dst[pub] = (double)src[t];
        for (j = np - 1; j >= 0; j--) {
            if (++idx[j] < q->n_param_int[j]) { pub += pub_stride[q->perm[j]]; break; }
            idx[j] = 0;
            pub -= (q->n_param_int[j] - 1) * pub_stride[q->perm[j]];
        }
    }
}

/* ======================================================================= *
 *  PSYCHOMETRIC FUNCTIONS
 * ======================================================================= */

static double yqst__pf_shape(yqst_pf pf, double x, double alpha, double beta) {
    double z = beta * (x - alpha);
    switch (pf) {
    case YQST_PF_WEIBULL:
        if (!(x > 0.0) || !(alpha > 0.0)) return 0.0;
        return 1.0 - exp(-pow(x / alpha, beta));
    case YQST_PF_LOGISTIC: return 1.0 / (1.0 + exp(-z));
    case YQST_PF_NORMAL:   return 0.5 * erfc(-z * YQST__SQRT1_2);
    case YQST_PF_HYPSEC:   return (2.0 / 3.14159265358979323846) * atan(exp(1.57079632679489661923 * z));
    case YQST_PF_GUMBEL:
    default:               return 1.0 - exp(-pow(10.0, z));
    }
}

/* One cell, written with `stride` between outcomes. The stride is what lets
 * the transposed table be filled without a round trip through a temporary:
 * only a custom callback, which insists on K contiguous values, needs one. */
static void yqst__eval_into(const yqst_quest* q, const double* stim, const double* params,
                            double* out, size_t stride) {
    if (q->desc.pf_batch) {
        /* One cell is a batch of one stimulus and one parameter row. */
        int k;
        q->desc.pf_batch(q->desc.pf_ctx, stim, 1, params, 1, q->batch_out);
        for (k = 0; k < q->K; k++) out[(size_t)k * stride] = (double)q->batch_out[k];
    } else if (q->desc.pf == YQST_PF_CUSTOM) {
        double p[YQST_MAX_OUTCOMES];
        int k;
        q->desc.pf_fn(q->desc.pf_ctx, stim, params, p);
        if (stride == 1) {
            for (k = 0; k < q->K; k++) out[k] = p[k];
        } else {
            for (k = 0; k < q->K; k++) out[(size_t)k * stride] = p[k];
        }
    } else {
        double f = yqst__pf_shape(q->desc.pf, stim[0], params[0], params[1]);
        double p1 = params[2] + (1.0 - params[2] - params[3]) * f;
        if (!(p1 >= 0.0)) p1 = 0.0;   /* also catches a NaN from a wild grid */
        if (p1 > 1.0) p1 = 1.0;
        out[0] = 1.0 - p1;
        out[stride] = p1;
    }
}

static void yqst__eval(const yqst_quest* q, const double* stim, const double* params,
                       double* p) {
    yqst__eval_into(q, stim, params, p, 1);
}

/* Fill out[k*P + theta] with the likelihood of every outcome at one stimulus,
 * walking the parameter grid in INTERNAL order with an odometer so no cell
 * costs a division. The transposed layout is what makes every later sweep a
 * contiguous dot product. */
static void yqst__lik_at(const yqst_quest* q, const double* stim, double* out) {
    const double* axv[YQST_MAX_PARAMS];
    int idx[YQST_MAX_PARAMS];
    double pv[YQST_MAX_PARAMS];
    int np = yqst__np(q), P = q->P, j, t;
    if (q->desc.pf_batch) {
        /* One call for the whole parameter grid at this stimulus, then the
         * transpose from the callback's cell-major order into the header's
         * outcome-major rows. The parameter matrix is already in the order
         * the rows of `out` are indexed by, so nothing is permuted here. */
        int k, K = q->K;
        q->desc.pf_batch(q->desc.pf_ctx, stim, 1, q->param_matrix, P, q->batch_out);
        for (t = 0; t < P; t++)
            for (k = 0; k < K; k++)
                out[(size_t)k * (size_t)P + (size_t)t] =
                    (double)q->batch_out[(size_t)t * (size_t)K + (size_t)k];
        return;
    }
    for (j = 0; j < np; j++) {
        axv[j] = yqst__param_axis(q, q->perm[j]);
        idx[j] = 0;
        pv[q->perm[j]] = axv[j][0];
    }
    for (t = 0; t < P; t++) {
        yqst__eval_into(q, stim, pv, out + t, (size_t)P);
        for (j = np - 1; j >= 0; j--) {
            if (++idx[j] < q->n_param_int[j]) { pv[q->perm[j]] = axv[j][idx[j]]; break; }
            idx[j] = 0;
            pv[q->perm[j]] = axv[j][0];
        }
    }
}

static void yqst__lik_at_index(const yqst_quest* q, int s, double* out) {
    double sv[YQST_MAX_STIM_DIMS];
    yqst__stim_vec(q, s, sv);
    yqst__lik_at(q, sv, out);
}

/* ======================================================================= *
 *  POSTERIOR SUMMARIES
 * ======================================================================= */

static double yqst__entropy_of(const double* v, int n) {
    double h = 0.0;
    int i;
    for (i = 0; i < n; i++) h -= yqst__wlog2(v[i]);
    return h;
}

/* The entropy of the posterior is a constant of every joint selection, so it
 * is computed once per posterior and kept in the arena. The cache is written
 * through a pointer, which is what lets the const functions fill it. */
static double yqst__h_post(const yqst_quest* q) {
    if (q->cache[1] == 0.0) {
        q->cache[0] = yqst__entropy_of(q->posterior, q->P);   /* already double */
        q->cache[1] = 1.0;
    }
    return q->cache[0];
}

/* Marginal of one PUBLIC parameter axis. */
static void yqst__marginal_axis(const yqst_quest* q, int axis, double* out) {
    const double* post = q->posterior;
    int j, n;
    if (!yqst__param_axis_ok(q, axis)) return;   /* callers pass valid axes */
    j = q->inv_perm[axis];
    n = q->n_param_int[j];
    int stride = yqst__int_stride(q, j);
    int i, b;
    size_t t = 0, P = (size_t)q->P;
    for (i = 0; i < n; i++) out[i] = 0.0;
    while (t < P) {
        for (i = 0; i < n; i++) {
            double s = 0.0;
            for (b = 0; b < stride; b++) s += post[t++];
            out[i] += s;
        }
    }
}

/* Posterior marginalized over the nuisance axes, into out[n_marg]. Those axes
 * are first internally, so the marginal is the fast axis and this is B passes
 * of one contiguous add. */
static void yqst__marginal_free(const yqst_quest* q, double* out) {
    const double* post = q->posterior;
    int B = q->nuis_block, M = q->n_marg, m, b;
    for (m = 0; m < M; m++) out[m] = post[m];
    for (b = 1; b < B; b++) {
        const double* p = post + (size_t)b * (size_t)M;
        for (m = 0; m < M; m++) out[m] += p[m];
    }
}

/* ======================================================================= *
 *  SELECTION
 * ======================================================================= */

/* The joint selection score, from the table.
 *
 * Watson's expected entropy decomposes. With w_k(theta) = post(theta) L, and
 * because the likelihoods of one cell sum to 1 over the outcomes,
 *
 *   sum_k sum_theta w log2 w = sum_theta post log2 post
 *                              + sum_theta post sum_k L log2 L,
 *
 * so  E[H] = H(theta) - H(y | s) + sum_theta post(theta) h(s, theta),  with
 * h(s, theta) = -sum_k L log2 L the outcome entropy of one cell. h depends on
 * the table alone, so open() tabulates it as the K+1-th row and the sweep has
 * no logarithm left in it: K+1 dot products and K logarithms per stimulus.
 * H(theta) is the same for every stimulus, so the selection leaves it out and
 * yqst_expected_entropy() adds it back. */
static double yqst__ee_joint_table(const yqst_quest* q, const float* base) {
    const double* post = q->posterior;
    int P = q->P, K = q->K, k;
    double e = 0.0;
    for (k = 0; k < K; k++)
        e += yqst__wlog2(yqst__dot_f(post, base + (size_t)k * (size_t)P, P));
    return e + yqst__dot_f(post, base + (size_t)K * (size_t)P, P);
}

/* The same quantity from the definition, for the no-table path, which has no
 * tabulated h to read:  E[H] = sum_k (p_k log2 p_k - sum_theta w log2 w).
 * This one is the true expected entropy, not the offset one. */
static double yqst__ee_joint_lik(const yqst_quest* q, const double* lik) {
    const double* post = q->posterior;
    int P = q->P, K = q->K, t, k;
    double e = 0.0;
    for (k = 0; k < K; k++) {
        const double* row = lik + (size_t)k * (size_t)P;
        double pk = 0.0, slw = 0.0;
        for (t = 0; t < P; t++) {
            double w = post[t] * row[t];
            pk += w;
            slw += yqst__wlog2(w);
        }
        e += yqst__wlog2(pk) - slw;
    }
    return e;
}

/* With nuisance axes flagged, the entropy is that of the marginal (Watson
 * 2017 sec. 2.5, Prins 2013), so the logarithms are taken on the marginal:
 * K per stimulus for the outcome, K*n_marg for the marginals themselves.
 * The nuisance axes are first internally, so one outcome's marginal is B
 * passes of yqst__fma over n_marg contiguous values, and the sweep still
 * reads the row front to back. */
static double yqst__ee_marg(const yqst_quest* q, const float* base, const double* lik) {
    const double* post = q->posterior;
    double* acc = q->marginal_scratch;
    int P = q->P, K = q->K, B = q->nuis_block, M = q->n_marg, k, m, b;
    double e = 0.0;
    for (k = 0; k < K; k++) {
        size_t off = (size_t)k * (size_t)P;
        double pk = 0.0, slw = 0.0;
        memset(acc, 0, (size_t)M * sizeof(double));
        if (base) {
            for (b = 0; b < B; b++)
                yqst__fma_f(acc, post + (size_t)b * (size_t)M,
                            base + off + (size_t)b * (size_t)M, M);
        } else {
            for (b = 0; b < B; b++)
                yqst__fma_d(acc, post + (size_t)b * (size_t)M,
                            lik + off + (size_t)b * (size_t)M, M);
        }
        for (m = 0; m < M; m++) { pk += acc[m]; slw += yqst__wlog2(acc[m]); }
        e += yqst__wlog2(pk) - slw;
    }
    return e;
}

static const float* yqst__row(const yqst_quest* q, int s, int k) {
    return q->table + ((size_t)s * (size_t)q->rows + (size_t)k) * (size_t)q->P;
}

/* What yqst__score_entropy() leaves out of the expected entropy: the same
 * constant for every stimulus, so it cannot change an argmin or a tie, and
 * only yqst_expected_entropy() has to add it back. */
static double yqst__score_offset(const yqst_quest* q) {
    return (q->rows > q->K) ? yqst__h_post(q) : 0.0;
}

static double yqst__score_entropy(const yqst_quest* q, int s) {
    if (q->table) {
        const float* base = yqst__row(q, s, 0);
        if (q->has_nuisance) return yqst__ee_marg(q, base, NULL);
        return yqst__ee_joint_table(q, base);
    }
    yqst__lik_at_index(q, s, q->scratch);
    if (q->has_nuisance) return yqst__ee_marg(q, NULL, q->scratch);
    return yqst__ee_joint_lik(q, q->scratch);
}

/* The QUEST-style placement target: a point estimate of one parameter, in the
 * units of stimulus axis 0. */
static double yqst__placement(const yqst_quest* q) {
    int axis = q->desc.select_param;
    int n;
    const double* v = yqst__param_axis(q, axis);
    double* m = q->marginal_scratch;
    int i;
    if (!yqst__param_axis_ok(q, axis)) return 0.0;   /* open() validated it */
    n = q->n_param_axis[axis];
    yqst__marginal_axis(q, axis, m);
    if (q->desc.select == YQST_SELECT_MEAN) {
        double s = 0.0;
        for (i = 0; i < n; i++) s += m[i] * v[i];
        return s;
    }
    if (q->desc.select == YQST_SELECT_MODE) {
        int best = 0;
        for (i = 1; i < n; i++) if (m[i] > m[best]) best = i;
        return v[best];
    }
    {   /* YQST_SELECT_QUANTILE */
        double c = 0.0, p = q->desc.select_quantile;
        for (i = 0; i < n; i++) {
            c += m[i];
            if (c >= p) return v[i];
        }
        return v[n - 1];
    }
}

static int yqst__draw_subset(yqst_quest* q) {
    int m = q->desc.subset_size, S = q->S, i, b;
    int* out = q->subset_scratch;
    if (m >= S) { for (i = 0; i < S; i++) out[i] = i; return S; }
    for (i = 0; i < m; i++) {
        int j = 0, tries;
        bool dup = true;
        for (tries = 0; tries < 64 && dup; tries++) {
            double u = q->desc.rng(q->desc.rng_ctx);
            if (!(u >= 0.0) || !(u < 1.0)) u = 0.0;   /* a bad variate must not index out of range */
            j = (int)(u * (double)S);
            if (j >= S) j = S - 1;
            if (j < 0) j = 0;
            dup = false;
            for (b = 0; b < i; b++) if (out[b] == j) { dup = true; break; }
        }
        if (dup) {
            /* An unlucky or degenerate generator must still terminate, so the
             * last resort is the first free index above the one drawn. */
            int step;
            for (step = 0; step < S; step++) {
                int c = (j + step) % S;
                bool used = false;
                for (b = 0; b < i; b++) if (out[b] == c) { used = true; break; }
                if (!used) { j = c; break; }
            }
        }
        out[i] = j;
    }
    return m;
}

/* Score n candidates and resolve the argmin per desc.tiebreak. `cand` is the
 * candidate list, or NULL for the whole grid. */
static int yqst__select_from(yqst_quest* q, const int* cand, int n) {
    double* sc = q->scores;
    double best = HUGE_VAL, thr, target = 0.0;
    int i, count = 0, pick = -1;
    bool entropy = (q->desc.select == YQST_SELECT_ENTROPY);

    if (entropy) {
        for (i = 0; i < n; i++) {
            sc[i] = yqst__score_entropy(q, cand ? cand[i] : i);
            if (sc[i] < best) best = sc[i];
        }
    } else {
        /* The placement rules score the distance from stimulus axis 0, the
         * slowest axis, so its index is the flat index over that axis's
         * stride and the other axes only make ties. */
        const double* ax0 = yqst__stim_axis(q, 0);
        int stride0 = yqst__stim_stride(q, 0);
        target = yqst__placement(q);
        for (i = 0; i < n; i++) {
            sc[i] = fabs(ax0[(cand ? cand[i] : i) / stride0] - target);
            if (sc[i] < best) best = sc[i];
        }
    }
    thr = best + q->desc.tie_tolerance;
    for (i = 0; i < n; i++) if (sc[i] <= thr) count++;
    if (count == 0) return cand ? cand[0] : 0;   /* every score was a NaN */

    switch (q->desc.tiebreak) {
    case YQST_TIE_NEAREST: {
        int bestd = -1;
        for (i = 0; i < n; i++) {
            if (sc[i] > thr) continue;
            {
                int s = cand ? cand[i] : i;
                int d = (q->last_shown >= 0) ? (s > q->last_shown ? s - q->last_shown
                                                                  : q->last_shown - s) : 0;
                if (bestd < 0 || d < bestd) { bestd = d; pick = i; }
                if (q->last_shown < 0) return cand ? cand[i] : i;
            }
        }
        break;
    }
    case YQST_TIE_ALTERNATE: {
        if (count > 1 && q->tie_parity) {
            for (i = n - 1; i >= 0; i--) if (sc[i] <= thr) { pick = i; break; }
        } else {
            for (i = 0; i < n; i++) if (sc[i] <= thr) { pick = i; break; }
        }
        if (count > 1) q->tie_parity ^= 1;
        break;
    }
    case YQST_TIE_RANDOM: {
        int want = 0, seen = 0;
        if (count > 1) {
            double u = q->desc.rng(q->desc.rng_ctx);
            if (!(u >= 0.0) || !(u < 1.0)) u = 0.0;
            want = (int)(u * (double)count);
            if (want >= count) want = count - 1;
            if (want < 0) want = 0;
        }
        for (i = 0; i < n; i++) {
            if (sc[i] > thr) continue;
            if (seen++ == want) { pick = i; break; }
        }
        break;
    }
    case YQST_TIE_LOWEST:
    default:
        for (i = 0; i < n; i++) if (sc[i] <= thr) { pick = i; break; }
        break;
    }
    if (pick < 0) pick = 0;
    return cand ? cand[pick] : pick;
}

/* ======================================================================= *
 *  STOPPING
 * ======================================================================= */

static void yqst__update_stop(yqst_quest* q) {
    const yqst_desc* d = &q->desc;
    if (q->stop != YQST_STOP_NONE) return;   /* the FIRST criterion to fire */
    if (d->stop_trials > 0 && q->n_trials >= d->stop_trials) { q->stop = YQST_STOP_TRIALS; return; }
    if (d->stop_entropy > 0.0 && yqst_entropy(q) <= d->stop_entropy) { q->stop = YQST_STOP_ENTROPY; return; }
    if (d->stop_sd > 0.0 && yqst_sd(q, d->stop_sd_param) <= d->stop_sd) { q->stop = YQST_STOP_SD; return; }
    if (q->n_trials >= YQST_MAX_TRIALS) q->stop = YQST_STOP_FULL;
}

/* ======================================================================= *
 *  PUBLIC API
 * ======================================================================= */

YQST_API const char* yqst_strerror(int code) {
    switch (code) {
    case YQST_ERR_ARG:    return "invalid argument";
    case YQST_ERR_CLOSED: return "handle is not open";
    case YQST_ERR_FULL:   return "trial history is full";
    case YQST_ERR_MEMORY: return "out of memory";
#ifdef YQST_ASYNC
    case YQST_ERR_BUSY:    return "queue is full";
    case YQST_ERR_TIMEOUT: return "timed out";
#endif
    default:              return (code >= 0) ? "ok" : "unknown error";
    }
}

YQST_API const char* yqst_version(void) { return YQST_VERSION_STRING; }

YQST_API yqst_axis yqst_linspace(double lo, double hi, int n) {
    yqst_axis a;
    memset(&a, 0, sizeof(a));
    a.lo = lo; a.hi = hi; a.n = n;
    return a;
}

YQST_API yqst_axis yqst_values(const double* values, int n) {
    yqst_axis a;
    memset(&a, 0, sizeof(a));
    a.values = values; a.n = n;
    if (values && n > 0) { a.lo = values[0]; a.hi = values[n - 1]; }
    return a;
}

YQST_API yqst_axis yqst_fixed(double value) {
    yqst_axis a;
    memset(&a, 0, sizeof(a));
    a.lo = a.hi = value; a.n = 1;
    return a;
}

YQST_API int yqst_prior_normal(const yqst_axis* axis, double mean, double sd, double* out) {
    int i;
    if (!axis || !out || axis->n < 1) return YQST_ERR_ARG;
    if (!yqst__finite(mean) || !yqst__finite(sd) || sd <= 0.0) return YQST_ERR_ARG;
    if (!yqst__axis_ok(axis, true, "prior", 0, NULL, 0)) return YQST_ERR_ARG;
    for (i = 0; i < axis->n; i++) {
        double z = (yqst__axis_at(axis, i) - mean) / sd;
        out[i] = exp(-0.5 * z * z);
    }
    return YQST_OK;
}

YQST_API size_t yqst_memory_size(const yqst_desc* desc) {
    yqst__sizes z;
    if (!yqst__sizes_of(desc, &z, NULL, 0)) return 0;
    /* YQST__ALIGN - 1 spare bytes: a caller's buffer need not be aligned for
     * a double, and open() aligns the base rather than refusing it. */
    return yqst__plan(&z, NULL, NULL) + (YQST__ALIGN - 1);
}

YQST_API bool yqst_open(yqst_quest* q, const yqst_desc* desc) {
    yqst__sizes z;
    size_t need, t;
    unsigned char* base;
    int i, j, k;
    double sv[YQST_MAX_STIM_DIMS];
    /* A batch callback writes floats, so K of them cannot sum closer to 1
     * than about K times a float's last digit; a per-cell callback writes
     * doubles and has no such excuse. */
    double sum_tol = (desc && desc->pf_batch) ? 1e-6 : 1e-9;

    if (!q) return false;
    memset(q->error, 0, sizeof(q->error));
    q->open = false;
    if (!desc) {
        snprintf(q->error, sizeof(q->error), "ysp_quest: null desc");
        return false;
    }
    if (!yqst__sizes_of(desc, &z, q->error, sizeof(q->error))) return false;

    q->desc = *desc;
    q->S = z.S; q->P = z.P; q->K = z.K; q->rows = z.rows;
    q->n_marg = z.n_marg;
    q->nuis_block = z.nuis_block;
    q->has_nuisance = (z.nuis_block > 1);
    q->permuted = z.permuted;
    for (i = 0; i < desc->n_stim; i++)  q->n_stim_axis[i]  = desc->stim[i].n;
    for (i = 0; i < desc->n_param; i++) q->n_param_axis[i] = desc->param[i].n;
    for (i = 0; i < desc->n_param; i++) {
        q->perm[i] = z.perm[i];
        q->inv_perm[z.perm[i]] = i;
        q->n_param_int[i] = desc->param[z.perm[i]].n;
    }
    if (q->desc.select_quantile == 0.0) q->desc.select_quantile = 0.5;
    if (q->desc.tie_tolerance == 0.0)   q->desc.tie_tolerance = 1e-9;
    q->desc.subset_size = z.n_subset;

    need = yqst__plan(&z, NULL, NULL) + (YQST__ALIGN - 1);
    if (desc->memory) {
        if (desc->memory_size < need) {
            snprintf(q->error, sizeof(q->error),
                     "ysp_quest: memory buffer too small, need %llu bytes, got %llu (%s)",
                     (unsigned long long)need, (unsigned long long)desc->memory_size,
                     yqst_strerror(YQST_ERR_MEMORY));
            return false;
        }
        q->mem = desc->memory;
        q->mem_owned = false;
    } else {
        q->mem = YQST_MALLOC(need);
        if (!q->mem) {
            snprintf(q->error, sizeof(q->error),
                     "ysp_quest: allocation of %llu bytes failed (%s)",
                     (unsigned long long)need, yqst_strerror(YQST_ERR_MEMORY));
            return false;
        }
        q->mem_owned = true;
    }
    q->mem_size = need;
    base = (unsigned char*)q->mem;
    base += (size_t)((YQST__ALIGN - ((uintptr_t)base & (YQST__ALIGN - 1))) & (YQST__ALIGN - 1));
    yqst__plan(&z, base, q);

    /* The handle keeps no pointer into the caller's arrays: axis values are
     * copied here and the priors are consumed into the posterior below. */
    {
        double* p = q->stim_values;
        for (i = 0; i < desc->n_stim; i++)
            for (j = 0; j < desc->stim[i].n; j++) *p++ = yqst__axis_at(&desc->stim[i], j);
        p = q->param_values;
        for (i = 0; i < desc->n_param; i++)
            for (j = 0; j < desc->param[i].n; j++) *p++ = yqst__axis_at(&desc->param[i], j);
        for (i = 0; i < YQST_MAX_STIM_DIMS; i++) { q->desc.stim[i].values = NULL; q->desc.stim[i].prior = NULL; }
        for (i = 0; i < YQST_MAX_PARAMS; i++)    { q->desc.param[i].values = NULL; q->desc.param[i].prior = NULL; }
        q->desc.joint_prior = NULL;
        q->desc.memory = NULL;
        /* A built-in psychometric function ignores both callbacks, so drop
         * them here and the dispatch below has one thing to look at. */
        if (desc->pf != YQST_PF_CUSTOM) { q->desc.pf_fn = NULL; q->desc.pf_batch = NULL; }
    }

    /* The batch callback is handed the whole parameter grid as a matrix, in
     * the order the header walks it, so it is built once here and reused by
     * every call. */
    if (q->param_matrix) {
        double* w = q->param_matrix;
        for (t = 0; t < (size_t)q->P; t++) {
            yqst__param_vec_int(q, (int)t, w);
            w += desc->n_param;
        }
    }

    /* Prior: the product of the per-axis weights, times the joint weights.
     * The walk is in internal order and carries the caller's flat index along
     * with it, because joint_prior is in the caller's order. */
    {
        double sum = 0.0;
        int idx[YQST_MAX_PARAMS], pub_stride[YQST_MAX_PARAMS];
        int pub = 0;
        for (i = 0; i < desc->n_param; i++) pub_stride[i] = yqst__param_stride(q, i);
        memset(idx, 0, sizeof(idx));
        for (t = 0; t < (size_t)q->P; t++) {
            double w = 1.0;
            for (i = 0; i < desc->n_param; i++)
                if (desc->param[q->perm[i]].prior) w *= desc->param[q->perm[i]].prior[idx[i]];
            if (desc->joint_prior) w *= desc->joint_prior[pub];
            q->posterior[t] = w;
            sum += w;
            for (i = desc->n_param - 1; i >= 0; i--) {
                if (++idx[i] < q->n_param_int[i]) { pub += pub_stride[q->perm[i]]; break; }
                idx[i] = 0;
                pub -= (q->n_param_int[i] - 1) * pub_stride[q->perm[i]];
            }
        }
        if (!(sum > 0.0) || !yqst__finite(sum)) {
            snprintf(q->error, sizeof(q->error), "ysp_quest: the prior has no mass");
            if (q->mem_owned) YQST_FREE(q->mem);
            q->mem = NULL;
            return false;
        }
        for (t = 0; t < (size_t)q->P; t++) q->posterior[t] /= sum;
    }

    /* The table, S*P pf evaluations, written front to back in the order the
     * selection sweep reads it: one row per outcome, then the outcome entropy
     * of each cell where that row is kept. */
    if (q->table) {
        float* w = q->table;
        for (i = 0; i < q->S; i++) {
            yqst__stim_vec(q, i, sv);
            yqst__lik_at(q, sv, q->scratch);
            if (desc->pf == YQST_PF_CUSTOM) {
                const char* who = desc->pf_batch ? "pf_batch" : "pf_fn";
                int c;
                for (c = 0; c < q->P; c++) {
                    double s = 0.0;
                    for (k = 0; k < q->K; k++) {
                        double v = q->scratch[(size_t)k * (size_t)q->P + (size_t)c];
                        if (!yqst__finite(v) || v < 0.0) {
                            snprintf(q->error, sizeof(q->error),
                                     "ysp_quest: %s wrote %g for outcome %d at stimulus %d, parameter %d",
                                     who, v, k, i, c);
                            if (q->mem_owned) YQST_FREE(q->mem);
                            q->mem = NULL;
                            return false;
                        }
                        s += v;
                    }
                    if (fabs(s - 1.0) > sum_tol) {
                        snprintf(q->error, sizeof(q->error),
                                 "ysp_quest: %s outcomes sum to %.12g at stimulus %d, parameter %d, need 1",
                                 who, s, i, c);
                        if (q->mem_owned) YQST_FREE(q->mem);
                        q->mem = NULL;
                        return false;
                    }
                }
            }
            for (t = 0; t < (size_t)q->P * (size_t)q->K; t++) *w++ = (float)q->scratch[t];
            if (q->rows > q->K) {
                /* The entropy of the row AS STORED, so the decomposition the
                 * selection uses is exact against the floats it reads and two
                 * callbacks that write the same floats give the same table. */
                const float* stored = w - (size_t)q->K * (size_t)q->P;
                int c;
                for (c = 0; c < q->P; c++) {
                    double h = 0.0;
                    for (k = 0; k < q->K; k++)
                        h -= yqst__wlog2((double)stored[(size_t)k * (size_t)q->P + (size_t)c]);
                    *w++ = (float)h;
                }
            }
        }
    } else if (desc->pf == YQST_PF_CUSTOM) {
        /* No table to check cell by cell, so check the corners and the middle. */
        int si[3], pi[3], a, b;
        si[0] = 0; si[1] = q->S / 2; si[2] = q->S - 1;
        pi[0] = 0; pi[1] = q->P / 2; pi[2] = q->P - 1;
        for (a = 0; a < 3; a++) {
            double pv[YQST_MAX_PARAMS];
            double pp[YQST_MAX_OUTCOMES];
            yqst__stim_vec(q, si[a], sv);
            for (b = 0; b < 3; b++) {
                double s = 0.0;
                yqst__param_vec_int(q, pi[b], pv);
                yqst__eval(q, sv, pv, pp);
                for (k = 0; k < q->K; k++) {
                    if (!yqst__finite(pp[k]) || pp[k] < 0.0) s = YQST__NAN;
                    else s += pp[k];
                }
                if (!(fabs(s - 1.0) <= sum_tol)) {
                    snprintf(q->error, sizeof(q->error),
                             "ysp_quest: %s outcomes sum to %.12g at stimulus %d, parameter %d, need 1",
                             desc->pf_batch ? "pf_batch" : "pf_fn", s, si[a], pi[b]);
                    if (q->mem_owned) YQST_FREE(q->mem);
                    q->mem = NULL;
                    return false;
                }
            }
        }
    }

    q->cache[0] = 0.0;
    q->cache[1] = 0.0;
    q->proposed = -1;
    q->last_shown = -1;
    q->tie_parity = 0;
    q->n_trials = 0;
    q->stop = YQST_STOP_NONE;
    q->open = true;
    yqst__update_stop(q);
    return true;
}

YQST_API void yqst_close(yqst_quest* q) {
    if (!q) return;
    if (q->mem && q->mem_owned) YQST_FREE(q->mem);
    q->mem = NULL;
    q->mem_size = 0;
    q->mem_owned = false;
    q->table = NULL;
    q->cache = NULL;
    q->posterior = NULL;
    q->post_public = NULL;
    q->scratch = NULL;
    q->param_matrix = NULL;
    q->batch_out = NULL;
    q->marginal_scratch = NULL;
    q->scores = NULL;
    q->stim_values = NULL;
    q->param_values = NULL;
    q->subset_scratch = NULL;
    q->open = false;
    q->n_trials = 0;
    q->proposed = -1;
    q->last_shown = -1;
    q->stop = YQST_STOP_NONE;
}

YQST_API const char* yqst_error(const yqst_quest* q) { return q ? q->error : ""; }
YQST_API bool yqst_is_open(const yqst_quest* q) { return q && q->open; }

YQST_API int yqst_n_stim(const yqst_quest* q)     { return (q && q->open) ? q->S : YQST_ERR_CLOSED; }
YQST_API int yqst_n_param(const yqst_quest* q)    { return (q && q->open) ? q->P : YQST_ERR_CLOSED; }
YQST_API int yqst_n_outcomes(const yqst_quest* q) { return (q && q->open) ? q->K : YQST_ERR_CLOSED; }

YQST_API double yqst_stim_value(const yqst_quest* q, int index, int axis) {
    if (!q || !q->open || index < 0 || index >= q->S) return YQST__NAN;
    if (!yqst__stim_axis_ok(q, axis)) return YQST__NAN;
    return yqst__stim_axis(q, axis)[(index / yqst__stim_stride(q, axis)) % q->n_stim_axis[axis]];
}

YQST_API int yqst_stim_values(const yqst_quest* q, int index, double* out) {
    double sv[YQST_MAX_STIM_DIMS];
    int n;
    if (!q) return YQST_ERR_ARG;
    if (!q->open) return YQST_ERR_CLOSED;
    if (index < 0 || index >= q->S || !out) return YQST_ERR_ARG;
    n = yqst__ns(q);
    yqst__stim_vec(q, index, sv);
    memcpy(out, sv, (size_t)n * sizeof(double));
    return n;
}

YQST_API int yqst_stim_index(const yqst_quest* q, const int* sub) {
    int sb[YQST_MAX_STIM_DIMS];
    int i, n, flat = 0;
    if (!q) return YQST_ERR_ARG;
    if (!q->open) return YQST_ERR_CLOSED;
    if (!sub) return YQST_ERR_ARG;
    n = yqst__ns(q);
    memcpy(sb, sub, (size_t)n * sizeof(int));
    for (i = 0; i < n; i++) {
        if (sb[i] < 0 || sb[i] >= q->n_stim_axis[i]) return YQST_ERR_ARG;
        flat = flat * q->n_stim_axis[i] + sb[i];
    }
    return flat;
}

YQST_API int yqst_stim_nearest(const yqst_quest* q, const double* stim) {
    double sv[YQST_MAX_STIM_DIMS];
    int i, j, ns, flat = 0;
    if (!q) return YQST_ERR_ARG;
    if (!q->open) return YQST_ERR_CLOSED;
    if (!stim) return YQST_ERR_ARG;
    ns = yqst__ns(q);
    memcpy(sv, stim, (size_t)ns * sizeof(double));
    for (i = 0; i < ns; i++) {
        const double* v = yqst__stim_axis(q, i);
        int n = q->n_stim_axis[i], best = 0;
        double bd;
        if (!yqst__finite(sv[i])) return YQST_ERR_ARG;
        bd = fabs(v[0] - sv[i]);
        for (j = 1; j < n; j++) {
            double d = fabs(v[j] - sv[i]);
            if (d < bd) { bd = d; best = j; }
        }
        flat = flat * n + best;
    }
    return flat;
}

YQST_API double yqst_param_value(const yqst_quest* q, int index, int axis) {
    if (!q || !q->open || index < 0 || index >= q->P) return YQST__NAN;
    if (!yqst__param_axis_ok(q, axis)) return YQST__NAN;
    return yqst__param_axis(q, axis)[(index / yqst__param_stride(q, axis)) % q->n_param_axis[axis]];
}

YQST_API int yqst_param_values(const yqst_quest* q, int index, double* out) {
    double pv[YQST_MAX_PARAMS];
    int n;
    if (!q) return YQST_ERR_ARG;
    if (!q->open) return YQST_ERR_CLOSED;
    if (index < 0 || index >= q->P || !out) return YQST_ERR_ARG;
    n = yqst__np(q);
    yqst__param_vec(q, index, pv);
    memcpy(out, pv, (size_t)n * sizeof(double));
    return n;
}

YQST_API int yqst_param_index(const yqst_quest* q, const int* sub) {
    int sb[YQST_MAX_PARAMS];
    int i, n, flat = 0;
    if (!q) return YQST_ERR_ARG;
    if (!q->open) return YQST_ERR_CLOSED;
    if (!sub) return YQST_ERR_ARG;
    n = yqst__np(q);
    memcpy(sb, sub, (size_t)n * sizeof(int));
    for (i = 0; i < n; i++) {
        if (sb[i] < 0 || sb[i] >= q->n_param_axis[i]) return YQST_ERR_ARG;
        flat = flat * q->n_param_axis[i] + sb[i];
    }
    return flat;
}

YQST_API int yqst_stim_axis_n(const yqst_quest* q, int axis) {
    if (!q) return YQST_ERR_ARG;
    if (!q->open) return YQST_ERR_CLOSED;
    if (!yqst__stim_axis_ok(q, axis)) return YQST_ERR_ARG;
    return q->n_stim_axis[axis];
}

/* n_param_axis is in the caller's order whatever LAYOUT did to the posterior,
 * so this needs no translation. */
YQST_API int yqst_param_axis_n(const yqst_quest* q, int axis) {
    if (!q) return YQST_ERR_ARG;
    if (!q->open) return YQST_ERR_CLOSED;
    if (!yqst__param_axis_ok(q, axis)) return YQST_ERR_ARG;
    return q->n_param_axis[axis];
}

static int yqst__next(yqst_quest* q) {
    if (!q) return YQST_ERR_ARG;
    if (!q->open) return YQST_ERR_CLOSED;
    if (q->proposed >= 0) return q->proposed;
    if (q->desc.subset_size > 0) {
        int n = yqst__draw_subset(q);
        q->proposed = yqst__select_from(q, q->subset_scratch, n);
    } else {
        q->proposed = yqst__select_from(q, NULL, q->S);
    }
    return q->proposed;
}

static int yqst__next_subset(yqst_quest* q, const int* subset, int n) {
    int i;
    if (!q) return YQST_ERR_ARG;
    if (!q->open) return YQST_ERR_CLOSED;
    if (!subset || n < 1 || n > q->S) return YQST_ERR_ARG;
    for (i = 0; i < n; i++) if (subset[i] < 0 || subset[i] >= q->S) return YQST_ERR_ARG;
    q->proposed = yqst__select_from(q, subset, n);
    return q->proposed;
}

YQST_API double yqst_expected_entropy(const yqst_quest* q, int index) {
    if (!q || !q->open || index < 0 || index >= q->S) return YQST__NAN;
    return yqst__score_entropy(q, index) + yqst__score_offset(q);
}

/* One Bayes step against one outcome's likelihood row, which the transposed
 * layout makes contiguous. The mass is summed before anything is written, so
 * an outcome no parameter point can produce leaves the posterior alone
 * instead of destroying it. */
static int yqst__apply(yqst_quest* q, const float* row, const double* lik) {
    double sum = row ? yqst__dot_f(q->posterior, row, q->P)
                     : yqst__dot_d(q->posterior, lik, q->P);
    if (!(sum > 0.0) || !yqst__finite(sum)) return YQST_ERR_ARG;
    if (row) yqst__scale_f(q->posterior, row, 1.0 / sum, q->P);
    else     yqst__scale_d(q->posterior, lik, 1.0 / sum, q->P);
    q->cache[1] = 0.0;   /* the posterior moved, so its entropy is stale */
    return YQST_OK;
}

/* `stim` is always a local of YQST_MAX_STIM_DIMS here, never a caller's
 * array, so reading all of it is in bounds by construction. */
static void yqst__record(yqst_quest* q, const double* stim, int stim_index, int outcome) {
    yqst_trial* tr = &q->history[q->n_trials++];
    int i, n = yqst__ns(q);
    for (i = 0; i < YQST_MAX_STIM_DIMS; i++) tr->stim[i] = (i < n) ? stim[i] : 0.0;
    tr->stim_index = stim_index;
    tr->proposed_index = q->proposed;
    tr->outcome = (uint8_t)outcome;
    q->proposed = -1;
    q->last_shown = stim_index;
    yqst__update_stop(q);
}

static int yqst__update(yqst_quest* q, int index, int outcome) {
    double sv[YQST_MAX_STIM_DIMS];
    int rc;
    if (!q) return YQST_ERR_ARG;
    if (!q->open) return YQST_ERR_CLOSED;
    if (index < 0 || index >= q->S) return YQST_ERR_ARG;
    if (outcome < 0 || outcome >= q->K) return YQST_ERR_ARG;
    if (q->n_trials >= YQST_MAX_TRIALS) return YQST_ERR_FULL;
    if (q->table) {
        rc = yqst__apply(q, yqst__row(q, index, outcome), NULL);
    } else {
        yqst__lik_at_index(q, index, q->scratch);
        rc = yqst__apply(q, NULL, q->scratch + (size_t)outcome * (size_t)q->P);
    }
    if (rc != YQST_OK) return rc;
    yqst__stim_vec(q, index, sv);
    yqst__record(q, sv, index, outcome);
    return YQST_OK;
}

static int yqst__update_values(yqst_quest* q, const double* stim, int outcome) {
    double sv[YQST_MAX_STIM_DIMS];
    int i, n, rc;
    if (!q) return YQST_ERR_ARG;
    if (!q->open) return YQST_ERR_CLOSED;
    if (!stim || outcome < 0 || outcome >= q->K) return YQST_ERR_ARG;
    n = yqst__ns(q);
    for (i = 0; i < YQST_MAX_STIM_DIMS; i++) sv[i] = 0.0;
    memcpy(sv, stim, (size_t)n * sizeof(double));
    for (i = 0; i < n; i++) if (!yqst__finite(sv[i])) return YQST_ERR_ARG;
    if (q->n_trials >= YQST_MAX_TRIALS) return YQST_ERR_FULL;
    yqst__lik_at(q, sv, q->scratch);
    rc = yqst__apply(q, NULL, q->scratch + (size_t)outcome * (size_t)q->P);
    if (rc != YQST_OK) return rc;
    yqst__record(q, sv, -1, outcome);
    return YQST_OK;
}

/* The handle-changing calls leave a message on failure (README.md,
 * Conventions); the work is in the yqst__ bodies so each return path does
 * not repeat the snprintf. */
static int yqst__fail(yqst_quest* q, int rc, const char* what) {
    if (q && rc < 0)
        snprintf(q->error, sizeof(q->error), "ysp_quest: %s: %s", what, yqst_strerror(rc));
    return rc;
}

YQST_API int yqst_next(yqst_quest* q) {
    return yqst__fail(q, yqst__next(q), "yqst_next");
}

YQST_API int yqst_next_subset(yqst_quest* q, const int* subset, int n) {
    return yqst__fail(q, yqst__next_subset(q, subset, n), "yqst_next_subset");
}

YQST_API int yqst_update(yqst_quest* q, int index, int outcome) {
    return yqst__fail(q, yqst__update(q, index, outcome), "yqst_update");
}

YQST_API int yqst_update_values(yqst_quest* q, const double* stim, int outcome) {
    return yqst__fail(q, yqst__update_values(q, stim, outcome), "yqst_update_values");
}

YQST_API bool yqst_done(const yqst_quest* q) {
    return q && q->open && q->stop != YQST_STOP_NONE;
}

YQST_API yqst_stop yqst_stop_reason(const yqst_quest* q) {
    return (q && q->open) ? q->stop : YQST_STOP_NONE;
}

YQST_API int yqst_estimate(const yqst_quest* q, yqst_estimator how, double* out) {
    double est[YQST_MAX_PARAMS];
    int i, np;
    if (!q) return YQST_ERR_ARG;
    if (!q->open) return YQST_ERR_CLOSED;
    if (!out) return YQST_ERR_ARG;
    np = yqst__np(q);
    for (i = 0; i < YQST_MAX_PARAMS; i++) est[i] = 0.0;
    switch (how) {
    case YQST_EST_MODE: {
        int best = 0;
        for (i = 1; i < q->P; i++) if (q->posterior[i] > q->posterior[best]) best = i;
        yqst__param_vec_int(q, best, est);
        break;
    }
    case YQST_EST_MEDIAN:
        for (i = 0; i < np; i++) est[i] = yqst_quantile(q, i, 0.5);
        break;
    case YQST_EST_MEAN: {
        for (i = 0; i < np; i++) {
            const double* v = yqst__param_axis(q, i);
            double* m = q->marginal_scratch;
            double s = 0.0;
            int j;
            yqst__marginal_axis(q, i, m);
            for (j = 0; j < q->n_param_axis[i]; j++) s += (double)m[j] * v[j];
            est[i] = s;
        }
        break;
    }
    default:
        return YQST_ERR_ARG;
    }
    memcpy(out, est, (size_t)np * sizeof(double));
    return YQST_OK;
}

YQST_API double yqst_quantile(const yqst_quest* q, int axis, double p) {
    const double* v;
    double* m;
    double c = 0.0;
    int i, n;
    if (!q || !q->open) return YQST__NAN;
    if (!yqst__param_axis_ok(q, axis)) return YQST__NAN;
    if (!(p > 0.0) || !(p < 1.0)) return YQST__NAN;
    n = q->n_param_axis[axis];
    v = yqst__param_axis(q, axis);
    m = q->marginal_scratch;
    yqst__marginal_axis(q, axis, m);
    for (i = 0; i < n; i++) {
        c += m[i];
        if (c >= p) return v[i];
    }
    return v[n - 1];
}

YQST_API double yqst_sd(const yqst_quest* q, int axis) {
    const double* v;
    double* m;
    double s = 0.0, s2 = 0.0, var;
    int i, n;
    if (!q || !q->open) return YQST__NAN;
    if (!yqst__param_axis_ok(q, axis)) return YQST__NAN;
    n = q->n_param_axis[axis];
    v = yqst__param_axis(q, axis);
    m = q->marginal_scratch;
    yqst__marginal_axis(q, axis, m);
    for (i = 0; i < n; i++) { s += m[i] * v[i]; s2 += m[i] * v[i] * v[i]; }
    var = s2 - s * s;
    return (var > 0.0) ? sqrt(var) : 0.0;
}

YQST_API int yqst_marginal(const yqst_quest* q, int axis, double* out) {
    if (!q) return YQST_ERR_ARG;
    if (!q->open) return YQST_ERR_CLOSED;
    if (!yqst__param_axis_ok(q, axis) || !out) return YQST_ERR_ARG;
    yqst__marginal_axis(q, axis, out);
    return q->n_param_axis[axis];
}

YQST_API const double* yqst_posterior(const yqst_quest* q) {
    if (!q || !q->open) return NULL;
    if (!q->permuted) return q->posterior;
    /* The posterior is stored with the nuisance axes first; the caller asked
     * for their own axis order, so it is permuted into a second buffer that
     * exists only in this case. */
    yqst__to_public(q, q->posterior, q->post_public);
    return q->post_public;
}

YQST_API double yqst_entropy(const yqst_quest* q) {
    if (!q || !q->open) return YQST__NAN;
    if (!q->has_nuisance) return yqst__h_post(q);
    yqst__marginal_free(q, q->marginal_scratch);
    return yqst__entropy_of(q->marginal_scratch, q->n_marg);
}

YQST_API int yqst_p(const yqst_quest* q, int index, const double* params, double* p_out) {
    double sv[YQST_MAX_STIM_DIMS];
    if (!q) return YQST_ERR_ARG;
    if (!q->open) return YQST_ERR_CLOSED;
    if (index < 0 || index >= q->S) return YQST_ERR_ARG;
    yqst__stim_vec(q, index, sv);
    return yqst_p_values(q, sv, params, p_out);
}

/* The built-in functions read params[0..3], which is in bounds for them
 * because open() gives a built-in exactly four parameters; a custom model is
 * handed n_param values and reads its own. An optimizer cannot know that the
 * built-in branch is dead for a three-parameter custom model, so the caller's
 * arrays are copied, n_stim and n_param elements, into locals of the maximum
 * size, and the model only ever sees those. */
YQST_API int yqst_p_values(const yqst_quest* q, const double* stim, const double* params, double* p_out) {
    double sv[YQST_MAX_STIM_DIMS], pv[YQST_MAX_PARAMS], p[YQST_MAX_OUTCOMES];
    int i, ns, np, nk;
    if (!q) return YQST_ERR_ARG;
    if (!q->open) return YQST_ERR_CLOSED;
    if (!stim || !params || !p_out) return YQST_ERR_ARG;
    ns = yqst__ns(q);
    np = yqst__np(q);
    nk = yqst__nk(q);
    for (i = 0; i < YQST_MAX_STIM_DIMS; i++) sv[i] = 0.0;
    for (i = 0; i < YQST_MAX_PARAMS; i++)    pv[i] = 0.0;
    memcpy(sv, stim, (size_t)ns * sizeof(double));
    memcpy(pv, params, (size_t)np * sizeof(double));
    for (i = 0; i < ns; i++) if (!yqst__finite(sv[i])) return YQST_ERR_ARG;
    for (i = 0; i < np; i++) if (!yqst__finite(pv[i])) return YQST_ERR_ARG;
    yqst__eval(q, sv, pv, p);
    memcpy(p_out, p, (size_t)nk * sizeof(double));
    return YQST_OK;
}

YQST_API int yqst_simulate(const yqst_quest* q, int index, const double* params, double u) {
    double p[YQST_MAX_OUTCOMES];
    double c = 0.0;
    int k, rc;
    if (!q) return YQST_ERR_ARG;
    if (!q->open) return YQST_ERR_CLOSED;
    if (!yqst__finite(u) || u < 0.0 || u >= 1.0) return YQST_ERR_ARG;
    rc = yqst_p(q, index, params, p);
    if (rc != YQST_OK) return rc;
    for (k = 0; k < yqst__nk(q); k++) {
        c += p[k];
        if (c > u) return k;
    }
    return q->K - 1;   /* the probabilities sum to 1 only to rounding */
}

YQST_API int yqst_n_trials(const yqst_quest* q) {
    if (!q) return YQST_ERR_ARG;
    if (!q->open) return YQST_ERR_CLOSED;
    return q->n_trials;
}

YQST_API const yqst_trial* yqst_history(const yqst_quest* q, int* n) {
    if (n) *n = (q && q->open) ? q->n_trials : 0;
    return (q && q->open) ? q->history : NULL;
}

/* ======================================================================= *
 *  SNAPSHOT
 *
 *  In the shape of ysp/trials.h's ytr_save / ytr_open_saved: versioned,
 *  little-endian, written and read one byte at a time so a snapshot moves
 *  between compilers and platforms. One writer does three jobs (count, write,
 *  compare), which is how yqst_open_saved() checks the desc against the snapshot
 *  without a second description of the layout that could drift from the first.
 * ======================================================================= */

#define YQST__SNAP_FORMAT 1u

typedef struct yqst__w {
    unsigned char*       out;
    const unsigned char* cmp;
    size_t               pos;
    size_t               cap;
    const char*          diff;   /* compare: the first field that differs */
} yqst__w;

static void yqst__put(yqst__w* w, uint64_t v, int nbytes, const char* name) {
    int i;
    unsigned char b;
    for (i = 0; i < nbytes; i++) {
        b = (unsigned char)((v >> (8 * i)) & 0xffu);
        if (w->out) {
            if (w->pos < w->cap) w->out[w->pos] = b;
        } else if (w->cmp && !w->diff) {
            if (w->pos >= w->cap || w->cmp[w->pos] != b) w->diff = name;
        }
        w->pos++;
    }
}

static void yqst__put_i32(yqst__w* w, int v, const char* name) {
    yqst__put(w, (uint64_t)(uint32_t)v, 4, name);
}

static void yqst__put_f64(yqst__w* w, double v, const char* name) {
    uint64_t u;
    memcpy(&u, &v, sizeof(u));
    yqst__put(w, u, 8, name);
}

/* The desc as the handle resolved it: defaults written as used, axes as the
 * values open() computed, whichever way they were given. The priors are NOT
 * here. open() consumes them into the posterior and keeps no copy, and the
 * posterior in the snapshot replaces whatever they would have produced, so
 * there is nothing a mismatch could change. */
static void yqst__put_desc(yqst__w* w, const yqst_quest* q) {
    const yqst_desc* d = &q->desc;
    int i, j, kind;
    yqst__put_i32(w, d->n_stim, "n_stim");
    for (i = 0; i < d->n_stim; i++) {
        const double* v = yqst__stim_axis(q, i);
        yqst__put_i32(w, q->n_stim_axis[i], "stim[].n");
        for (j = 0; j < q->n_stim_axis[i]; j++) yqst__put_f64(w, v[j], "stim[].values");
    }
    yqst__put_i32(w, d->n_param, "n_param");
    for (i = 0; i < d->n_param; i++) {
        const double* v = yqst__param_axis(q, i);
        yqst__put_i32(w, q->n_param_axis[i], "param[].n");
        yqst__put(w, d->param[i].nuisance ? 1u : 0u, 1, "param[].nuisance");
        for (j = 0; j < q->n_param_axis[i]; j++) yqst__put_f64(w, v[j], "param[].values");
    }
    /* Which kind of model, since a pointer cannot be compared across runs:
     * a built-in, a per-cell callback, or a batch one. The last two fill the
     * same table but differ in precision off the grid. */
    kind = (d->pf != YQST_PF_CUSTOM) ? 0 : (d->pf_batch ? 2 : 1);
    yqst__put_i32(w, (int)d->pf, "pf");
    yqst__put(w, (uint64_t)kind, 1, "pf_fn/pf_batch");
    yqst__put_i32(w, q->K, "n_outcomes");
    yqst__put_i32(w, (int)d->select, "select");
    yqst__put_i32(w, d->select_param, "select_param");
    yqst__put_f64(w, d->select_quantile, "select_quantile");
    yqst__put_i32(w, (int)d->tiebreak, "tiebreak");
    yqst__put_f64(w, d->tie_tolerance, "tie_tolerance");
    yqst__put(w, d->rng ? 1u : 0u, 1, "rng");
    yqst__put_i32(w, d->subset_size, "subset_size");
    yqst__put_i32(w, d->stop_trials, "stop_trials");
    yqst__put_f64(w, d->stop_entropy, "stop_entropy");
    yqst__put_f64(w, d->stop_sd, "stop_sd");
    yqst__put_i32(w, d->stop_sd_param, "stop_sd_param");
    yqst__put(w, d->no_table ? 1u : 0u, 1, "no_table");
    yqst__put_i32(w, q->S, "S");
    yqst__put_i32(w, q->P, "P");
}

static void yqst__put_all(yqst__w* w, const yqst_quest* q) {
    const double* post;
    int i, j;
    yqst__put(w, 'P', 1, "magic");
    yqst__put(w, 'S', 1, "magic");
    yqst__put(w, 'Y', 1, "magic");
    yqst__put(w, 'Q', 1, "magic");
    yqst__put(w, YQST__SNAP_FORMAT, 4, "format");
    yqst__put_desc(w, q);
    yqst__put_i32(w, q->n_trials, "n_trials");
    yqst__put_i32(w, q->proposed, "proposed");
    yqst__put_i32(w, q->last_shown, "last_shown");
    yqst__put_i32(w, q->tie_parity, "tie_parity");
    yqst__put_i32(w, (int)q->stop, "stop");
    /* The posterior in the CALLER'S axis order, so a snapshot does not depend
     * on how this version of the header lays its posterior out inside. */
    post = yqst_posterior(q);
    for (i = 0; i < q->P; i++) yqst__put_f64(w, post[i], "posterior");
    for (i = 0; i < q->n_trials; i++) {
        const yqst_trial* h = &q->history[i];
        for (j = 0; j < yqst__ns(q); j++) yqst__put_f64(w, h->stim[j], "history.stim");
        yqst__put_i32(w, h->stim_index, "history.stim_index");
        yqst__put_i32(w, h->proposed_index, "history.proposed_index");
        yqst__put(w, h->outcome, 1, "history.outcome");
    }
}

YQST_API size_t yqst_save_size(const yqst_quest* q) {
    yqst__w w;
    if (!q || !q->open) return 0;
    memset(&w, 0, sizeof(w));
    yqst__put_all(&w, q);
    return w.pos;
}

YQST_API int yqst_save(const yqst_quest* q, void* buf, size_t cap) {
    yqst__w w;
    size_t need;
    if (!q) return YQST_ERR_ARG;
    if (!q->open) return YQST_ERR_CLOSED;
    need = yqst_save_size(q);
    if (!buf || cap < need || need > 0x7fffffffu) return YQST_ERR_ARG;
    memset(&w, 0, sizeof(w));
    w.out = (unsigned char*)buf;
    w.cap = cap;
    yqst__put_all(&w, q);
    return (int)w.pos;
}

typedef struct yqst__r {
    const unsigned char* in;
    size_t               pos;
    size_t               len;
    bool                 bad;
} yqst__r;

static uint64_t yqst__get(yqst__r* r, int nbytes) {
    uint64_t v = 0;
    int i;
    if (r->bad || r->len - r->pos < (size_t)nbytes) {
        r->bad = true;
        return 0;
    }
    for (i = 0; i < nbytes; i++) v |= (uint64_t)r->in[r->pos + (size_t)i] << (8 * i);
    r->pos += (size_t)nbytes;
    return v;
}

static int yqst__get_i32(yqst__r* r) { return (int)(int32_t)(uint32_t)yqst__get(r, 4); }

static double yqst__get_f64(yqst__r* r) {
    uint64_t u = yqst__get(r, 8);
    double v;
    memcpy(&v, &u, sizeof(v));
    return v;
}

/* Public order to the internal one, P values: the inverse of
 * yqst__to_public, walked with the same odometer. */
static void yqst__from_public(const yqst_quest* q, const double* src, double* dst) {
    int idx[YQST_MAX_PARAMS], pub_stride[YQST_MAX_PARAMS];
    int np = yqst__np(q), j, t, pub = 0;
    for (j = 0; j < np; j++) pub_stride[j] = yqst__param_stride(q, j);
    memset(idx, 0, sizeof(idx));
    for (t = 0; t < q->P; t++) {
        dst[t] = src[pub];
        for (j = np - 1; j >= 0; j--) {
            if (++idx[j] < q->n_param_int[j]) { pub += pub_stride[q->perm[j]]; break; }
            idx[j] = 0;
            pub -= (q->n_param_int[j] - 1) * pub_stride[q->perm[j]];
        }
    }
}

/* Leave the handle closed and say why: a load either resumes the session or
 * leaves nothing half-restored behind. */
static bool yqst__load_fail(yqst_quest* q, const char* why) {
    char msg[sizeof(q->error)];
    snprintf(msg, sizeof(msg), "ysp_quest: yqst_open_saved: %s", why);
    yqst_close(q);
    memcpy(q->error, msg, sizeof(msg));
    return false;
}

YQST_API bool yqst_open_saved(yqst_quest* q, const yqst_desc* desc, const void* buf, size_t len) {
    const unsigned char* in = (const unsigned char*)buf;
    yqst__w w;
    yqst__r r;
    double* post_in;
    double sum = 0.0;
    int i, j, v;

    if (!q) return false;
    /* The cheap checks first: the open that follows rebuilds the table. */
    if (!in || len < 8 || in[0] != 'P' || in[1] != 'S' || in[2] != 'Y' || in[3] != 'Q') {
        memset(q->error, 0, sizeof(q->error));
        snprintf(q->error, sizeof(q->error), "ysp_quest: yqst_open_saved: not a ysp_quest snapshot");
        q->open = false;
        return false;
    }
    memset(&r, 0, sizeof(r));
    r.in = in;
    r.len = len;
    r.pos = 4;
    if ((uint32_t)yqst__get(&r, 4) != YQST__SNAP_FORMAT) {
        memset(q->error, 0, sizeof(q->error));
        snprintf(q->error, sizeof(q->error),
                 "ysp_quest: yqst_open_saved: snapshot format is not %u", YQST__SNAP_FORMAT);
        q->open = false;
        return false;
    }

    /* A full open, table and all: the table is a function of the desc and is
     * rebuilt rather than stored. That is S*P evaluations of the model, the
     * same as yqst_open(), and the price of a snapshot that is the size of the
     * posterior instead of the size of the table. */
    if (!yqst_open(q, desc)) return false;

    memset(&w, 0, sizeof(w));
    w.cmp = in;
    w.cap = len;
    w.pos = 8;
    yqst__put_desc(&w, q);
    if (w.diff) {
        char why[128];
        snprintf(why, sizeof(why), "desc.%s does not match the snapshot", w.diff);
        return yqst__load_fail(q, why);
    }
    r.pos = w.pos;

    q->n_trials   = yqst__get_i32(&r);
    q->proposed   = yqst__get_i32(&r);
    q->last_shown = yqst__get_i32(&r);
    q->tie_parity = yqst__get_i32(&r);
    v             = yqst__get_i32(&r);
    if (r.bad || q->n_trials < 0 || q->n_trials > YQST_MAX_TRIALS ||
        q->proposed < -1 || q->proposed >= q->S ||
        q->last_shown < -1 || q->last_shown >= q->S ||
        (q->tie_parity != 0 && q->tie_parity != 1) ||
        v < (int)YQST_STOP_NONE || v > (int)YQST_STOP_FULL)
        return yqst__load_fail(q, "the snapshot's counters are corrupt or truncated");
    q->stop = (yqst_stop)v;

    /* The posterior comes in the caller's order: straight into place when the
     * two orders agree, through the permutation buffer when they do not. It
     * is NOT renormalized, because the resumed session has to be the saved
     * one bit for bit. */
    post_in = q->permuted ? q->post_public : q->posterior;
    for (i = 0; i < q->P; i++) {
        double x = yqst__get_f64(&r);
        if (!yqst__finite(x) || x < 0.0) r.bad = true;
        post_in[i] = x;
        sum += x;
    }
    if (r.bad || !(sum > 0.0))
        return yqst__load_fail(q, "the snapshot's posterior is corrupt or truncated");
    if (q->permuted) yqst__from_public(q, q->post_public, q->posterior);
    q->cache[1] = 0.0;   /* the posterior's entropy is recomputed on demand */

    for (i = 0; i < q->n_trials; i++) {
        yqst_trial* h = &q->history[i];
        for (j = 0; j < YQST_MAX_STIM_DIMS; j++) h->stim[j] = 0.0;
        for (j = 0; j < yqst__ns(q); j++) {
            h->stim[j] = yqst__get_f64(&r);
            if (!yqst__finite(h->stim[j])) r.bad = true;
        }
        h->stim_index = yqst__get_i32(&r);
        h->proposed_index = yqst__get_i32(&r);
        v = (int)yqst__get(&r, 1);
        if (h->stim_index < -1 || h->stim_index >= q->S ||
            h->proposed_index < -1 || h->proposed_index >= q->S || v >= q->K)
            r.bad = true;
        h->outcome = (uint8_t)v;
    }
    if (r.bad) return yqst__load_fail(q, "the snapshot's history is corrupt or truncated");
    if (r.pos != r.len) {
        char why[96];
        snprintf(why, sizeof(why), "%lu bytes after the snapshot's end",
                 (unsigned long)(r.len - r.pos));
        return yqst__load_fail(q, why);
    }
    q->error[0] = '\0';
    return true;
}

/* ======================================================================= *
 *  PARAMETERS
 *
 *  The ranges are what yqst_open() accepts on its own; a field another
 *  field bounds (select_param below n_param) gives the widest range and
 *  says the rest in its doc.
 * ======================================================================= */

static const yqst_param yqst__params[] = {
    { "n_stim", "i32", 1, YQST_MAX_STIM_DIMS, 0, "",
      "required: stimulus dimensions, the entries of stim", NULL },
    { "stim[].values", "f64[]", -DBL_MAX, DBL_MAX, 0, "",
      "a stimulus axis's points, n of them, finite; empty = linspace(lo, hi, n)", NULL },
    { "stim[].lo", "f64", -DBL_MAX, DBL_MAX, 0, "",
      "first point of a linspace stimulus axis", NULL },
    { "stim[].hi", "f64", -DBL_MAX, DBL_MAX, 0, "",
      "last point of a linspace stimulus axis", NULL },
    { "stim[].n", "i32", 1, 2147483647.0, 0, "",
      "required: a stimulus axis's points; the product over axes is at most 2^31 - 1", NULL },
    { "n_param", "i32", 1, YQST_MAX_PARAMS, 0, "",
      "required: parameter dimensions, the entries of param; 4 for a built-in pf "
      "(threshold, slope, guess, lapse)", NULL },
    { "param[].values", "f64[]", -DBL_MAX, DBL_MAX, 0, "",
      "a parameter axis's points, n of them, finite; empty = linspace(lo, hi, n)", NULL },
    { "param[].lo", "f64", -DBL_MAX, DBL_MAX, 0, "",
      "first point of a linspace parameter axis", NULL },
    { "param[].hi", "f64", -DBL_MAX, DBL_MAX, 0, "",
      "last point of a linspace parameter axis", NULL },
    { "param[].n", "i32", 1, 2147483647.0, 0, "",
      "required: a parameter axis's points; 1 fixes the parameter", NULL },
    { "param[].prior", "f64[]", 0, DBL_MAX, 0, "",
      "n unnormalized weights over the axis's points, not all 0; empty = uniform", NULL },
    { "param[].nuisance", "bool", 0, 1, 0, "",
      "marginalize the parameter out of the selection entropy and the stop criteria", NULL },
    { "joint_prior", "f64[]", 0, DBL_MAX, 0, "",
      "P unnormalized weights over the whole parameter grid in LAYOUT order, not "
      "all 0; empty = the product of the axes' priors", NULL },
    { "pf", "enum", 0, 5, 0, "",
      "psychometric function; custom needs the program's pf_fn or pf_batch",
      "gumbel=0,weibull=1,logistic=2,normal=3,hypsec=4,custom=5" },
    { "n_outcomes", "i32", 0, YQST_MAX_OUTCOMES, 2, "",
      "response outcomes; a built-in pf takes 0 or 2, a custom pf needs 2 or more", NULL },
    { "select", "enum", 0, 3, 0, "",
      "stimulus selection: minimum expected entropy (QUEST+), or the posterior "
      "quantile (QUEST), mean or mode of parameter select_param",
      "entropy=0,quantile=1,mean=2,mode=3" },
    { "select_param", "i32", 0, YQST_MAX_PARAMS - 1, 0, "",
      "the parameter quantile, mean and mode select on; below n_param", NULL },
    { "select_quantile", "f64", 0, 1, 0.5, "",
      "the posterior quantile the quantile rule places at, in (0, 1)", NULL },
    { "tiebreak", "enum", 0, 3, 0, "",
      "stimulus among scores within tie_tolerance; random needs the player's generator",
      "lowest=0,nearest=1,alternate=2,random=3" },
    { "tie_tolerance", "f64", 0, DBL_MAX, 1e-9, "bit",
      "scores this close to the best are ties", NULL },
    { "subset_size", "i32", 0, 2147483647.0, 0, "",
      "score this many random stimuli a trial; 0 = all; needs the player's generator", NULL },
    { "stop_trials", "i32", 0, 2147483647.0, 0, "",
      "stop after this many trials; 0 = off (set a stop criterion)", NULL },
    { "stop_entropy", "f64", 0, DBL_MAX, 0, "bit",
      "stop when the posterior entropy is below this; 0 = off", NULL },
    { "stop_sd", "f64", 0, DBL_MAX, 0, "",
      "stop when the posterior sd of parameter stop_sd_param, in its units, is below "
      "this; 0 = off", NULL },
    { "stop_sd_param", "i32", 0, YQST_MAX_PARAMS - 1, 0, "",
      "the parameter stop_sd watches; below n_param", NULL },
    { "no_table", "bool", 0, 1, 0, "",
      "evaluate the pf every trial instead of keeping its table: less memory, a "
      "slower trial", NULL },
};

YQST_API const yqst_param* yqst_params(int* n) {
    if (n) *n = (int)(sizeof yqst__params / sizeof yqst__params[0]);
    return yqst__params;
}

/* ======================================================================= *
 *  ASYNC
 *
 *  A yrt_pump with QUEST+ in its on_msg. Everything here is plumbing: the
 *  inference is the same yqst_update() and yqst_next() a synchronous caller
 *  runs, on the same handle, in the same order, so an async run and a
 *  synchronous replay of the same responses agree bit for bit. What the layer
 *  adds is that the frame loop does not wait for it.
 * ======================================================================= */
#ifdef YQST_ASYNC

/* Compute the summaries first, take the publish lock only to copy them in.
 * That is ysp/rt.h's PUMP discipline: a caller may hold that lock as long as
 * it likes without stalling the inference, because the inference never holds
 * it. */
static void yqst__async_publish(yqst_async* a, uint32_t seq, int update_rc) {
    yqst_snapshot s;
    yqst_quest* q = a->quest;
    memset(&s, 0, sizeof(s));
    s.seq = seq;
    s.update_rc = update_rc;
    s.proposed = yqst_next(q);
    if (s.proposed >= 0) (void)yqst_stim_values(q, s.proposed, s.stim);
    s.n_trials = yqst_n_trials(q);
    s.done = yqst_done(q);
    s.stop = yqst_stop_reason(q);
    (void)yqst_estimate(q, a->estimator, s.estimate);
    s.entropy = yqst_entropy(q);
    s.sd = yqst_sd(q, 0);
    yrt_pump_lock(&a->pump);
    a->snap = s;
    a->published = true;
    yrt_pump_unlock(&a->pump);
}

/* One response, on the pump thread, in submit order. */
static void yqst__async_on_msg(void* ctx, const void* msg, uint32_t seq) {
    yqst_async* a = (yqst_async*)ctx;
    const yqst_async_msg* m = (const yqst_async_msg*)msg;
    int rc;
    if (m->stim_index >= 0) rc = yqst_update(a->quest, m->stim_index, m->outcome);
    else                    rc = yqst_update_values(a->quest, m->stim, m->outcome);
    yqst__async_publish(a, seq, rc);
}

/* ysp/rt.h's codes in this header's vocabulary. */
static int yqst__async_rc(int rt_rc) {
    switch (rt_rc) {
    case YRT_ERR_FULL:    return YQST_ERR_BUSY;
    case YRT_ERR_TIMEOUT: return YQST_ERR_TIMEOUT;
    case YRT_ERR_STOPPED: return YQST_ERR_CLOSED;
    case YRT_ERR_ARG:     return YQST_ERR_ARG;
    default:                return (rt_rc < 0) ? YQST_ERR_ARG : rt_rc;
    }
}

YQST_API bool yqst_async_open(yqst_async* a, const yqst_async_desc* desc) {
    yrt_pump_desc pd;
    if (!a) return false;
    memset(a->error, 0, sizeof(a->error));
    a->running = false;
    a->published = false;
    if (!desc || !desc->quest) {
        snprintf(a->error, sizeof(a->error), "ysp_quest: yqst_async_open needs desc.quest");
        return false;
    }
    if (!yqst_is_open(desc->quest)) {
        snprintf(a->error, sizeof(a->error), "ysp_quest: desc.quest is not open");
        return false;
    }
    if ((int)desc->estimator < 0 || (int)desc->estimator > (int)YQST_EST_MEDIAN) {
        snprintf(a->error, sizeof(a->error), "ysp_quest: desc.estimator is out of range");
        return false;
    }
    if (desc->queue_depth < 0 || desc->queue_depth > YQST_ASYNC_QUEUE) {
        snprintf(a->error, sizeof(a->error),
                 "ysp_quest: desc.queue_depth = %d, need 0..%d (YQST_ASYNC_QUEUE)",
                 desc->queue_depth, YQST_ASYNC_QUEUE);
        return false;
    }
    a->quest = desc->quest;
    a->estimator = desc->estimator;
    memset(&a->snap, 0, sizeof(a->snap));

    /* The first proposal, before the thread exists, so a poll() right after a
     * successful start always has something to return and the thread can never
     * publish seq 1 ahead of it. The pump's lock is a no-op while the pump is
     * not running, which is what makes this reuse of the publish path safe. */
    yqst__async_publish(a, 0, YQST_OK);

    memset(&pd, 0, sizeof(pd));
    pd.msg_size = sizeof(yqst_async_msg);
    pd.capacity = (uint32_t)(desc->queue_depth ? desc->queue_depth : YQST_ASYNC_QUEUE);
    pd.ring = a->ring;
    pd.on_msg = yqst__async_on_msg;
    pd.on_idle = NULL;      /* QUEST+ has nothing to do between trials */
    pd.ctx = a;
    pd.below_normal = desc->below_normal;
    pd.pin_cpu = desc->pin_cpu;
    if (!yrt_pump_open(&a->pump, &pd)) {
        snprintf(a->error, sizeof(a->error), "ysp_quest: %.200s",
                 yrt_pump_error(&a->pump));
        a->published = false;
        return false;
    }
    a->running = true;
    return true;
}

YQST_API void yqst_async_close(yqst_async* a) {
    if (!a) return;
    yrt_pump_close(&a->pump);   /* drains, then joins */
    a->running = false;
}

static int yqst__async_send(yqst_async* a, const yqst_async_msg* m) {
    int rc = yrt_pump_submit(&a->pump, m);
    return (rc < 0) ? yqst__async_rc(rc) : rc;
}

YQST_API int yqst_async_submit(yqst_async* a, int stim_index, int outcome) {
    yqst_async_msg m;
    int i;
    if (!a || !a->quest) return YQST_ERR_ARG;
    if (!a->running) return YQST_ERR_CLOSED;
    /* Checked here rather than on the thread, where the only thing that could
     * be done with a bad argument is to drop it silently. S and K do not
     * change after yqst_open(), so reading them beside the thread is safe. */
    if (stim_index < 0 || stim_index >= a->quest->S) return YQST_ERR_ARG;
    if (outcome < 0 || outcome >= a->quest->K) return YQST_ERR_ARG;
    for (i = 0; i < YQST_MAX_STIM_DIMS; i++) m.stim[i] = 0.0;
    m.stim_index = stim_index;
    m.outcome = outcome;
    return yqst__async_send(a, &m);
}

YQST_API int yqst_async_submit_values(yqst_async* a, const double* stim, int outcome) {
    yqst_async_msg m;
    int i, n;
    if (!a || !a->quest || !stim) return YQST_ERR_ARG;
    if (!a->running) return YQST_ERR_CLOSED;
    if (outcome < 0 || outcome >= a->quest->K) return YQST_ERR_ARG;
    n = yqst__ns(a->quest);
    for (i = 0; i < YQST_MAX_STIM_DIMS; i++) m.stim[i] = 0.0;
    memcpy(m.stim, stim, (size_t)n * sizeof(double));
    for (i = 0; i < n; i++) if (!yqst__finite(m.stim[i])) return YQST_ERR_ARG;
    m.stim_index = -1;
    m.outcome = outcome;
    return yqst__async_send(a, &m);
}

YQST_API int yqst_async_poll(const yqst_async* a, yqst_snapshot* out) {
    yqst_async* m = (yqst_async*)a;   /* the lock and the copy are not const */
    int seq;
    if (!a) return YQST_ERR_ARG;
    if (!a->running && !a->published) return YQST_ERR_CLOSED;
    yrt_pump_lock(&m->pump);
    seq = (int)m->snap.seq;
    if (out) *out = m->snap;
    yrt_pump_unlock(&m->pump);
    return seq;
}

YQST_API int yqst_async_wait(yqst_async* a, uint32_t seq, int64_t timeout_ns,
                             yqst_snapshot* out) {
    int rc;
    if (!a) return YQST_ERR_ARG;
    /* Not gated on a->running: a seq the thread DID reach before a stop still
     * answers, which is ysp/rt.h's rule for yrt_pump_wait and the reason a
     * caller can wait on the last response it submitted after stopping. */
    rc = yrt_pump_wait(&a->pump, seq, timeout_ns);
    if (rc < 0) return yqst__async_rc(rc);
    return yqst_async_poll(a, out);
}

YQST_API int yqst_async_pending(const yqst_async* a) {
    int rc;
    if (!a) return YQST_ERR_ARG;
    if (!a->running) return YQST_ERR_CLOSED;
    rc = yrt_pump_pending(&a->pump);
    return (rc < 0) ? yqst__async_rc(rc) : rc;
}

YQST_API const char* yqst_async_error(const yqst_async* a) { return a ? a->error : ""; }
YQST_API bool yqst_async_is_running(const yqst_async* a) { return a && a->running; }

YQST_API yrt_policy yqst_async_policy(const yqst_async* a) {
    return a ? yrt_pump_policy(&a->pump) : YRT_POLICY_NONE;
}

#endif /* YQST_ASYNC */

#endif /* YSP_QUEST_IMPLEMENTATION_GUARD */
#endif /* YSP_QUEST_IMPLEMENTATION */

/* ------------------------------------------------------------------------
 * This software is available under the MIT-0 (MIT No Attribution) license.
 *
 * Copyright (c) 2026 ysp contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY.
 * ------------------------------------------------------------------------ */
