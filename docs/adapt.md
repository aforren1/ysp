# Adaptive psychophysical methods: ysp/stair.h, ysp/quest.h, ysp/aep.h

Status: **implemented and compared.** ysp/stair.h 0.1.2, ysp/quest.h
0.5.2, ysp/aep.h 0.14.0, and the pump in ysp/rt.h 0.4.0 that their async
layers use. Each header has a Python package under `bindings/python/` and a
MEX function under `bindings/mex/`. All are registered in `CMakeLists.txt`,
with self-checking tests under `tests/adapt/` that ctest runs, and CI runs
them under sanitizers. Each header's STATUS block says what was measured and
what was not. The comparisons with PsychoPy, questplus, AEPsych, mQUESTPlus,
Palamedes and Watson's own QUEST+ notebook are done, and
[Verification](#verification) records them. This page records why the APIs
look the way they do, what each header takes from the existing toolboxes,
what it leaves out, and what the measurements decided. The headers
themselves say what they do.

## Goals

- The three families of adaptive procedure a rig needs, each in one header:
  nonparametric (staircases), parametric Bayesian on a grid (QUEST+ and its
  special cases), and nonparametric / semiparametric Bayesian with a
  Gaussian process (the AEPsych subset).
- The same shape as the transport headers: one file, a caller-owned handle,
  a zero-initialized `desc` for defaults, `open` returns `bool` and writes
  the one message buffer, everything else returns a value or a code.
- One loop shape across all three, so an experiment can swap procedures
  without changing its trial code: `next`, `update`, `done`, an estimate.
- No allocation after open, and none at all for the staircase. The
  QUEST+ table and the GP matrices are sized by `*_memory_size(desc)` and
  taken once, from the caller's buffer or from one `malloc`.
- Deterministic by default. The same desc and the same history give the
  same proposals. No header owns or seeds a random number generator. Where
  randomness helps (a tie, a random stimulus subset, a random acquisition,
  a simulated observer) the caller lends a generator through a `desc.rng`
  callback or passes a uniform variate in, so a run is reproducible from
  the caller's seed.
- Multi-outcome throughout. A trial's outcome is an index in `0..K-1` in
  `ysp/quest.h` and `ysp/aep.h`, not a bool, so a confidence rating, a
  "less / same / more" judgment, a which-interval response, or a
  continuous setting is a configuration and not a different library.
- A quick, pretty good answer. These headers run during the experiment,
  and their job is the next trial and a serviceable estimate, not the
  number that goes into the analysis; the history they hand out is for
  that. So precision is a design variable, decided by measurement: where a
  float or a 16-bit table buys real speed at an accuracy cost the estimate
  cannot see, it is the default, and the header states the cost.
- Honest cost. Every header states its per-trial operation count and the
  example that measures it, in the style of `ysp/rt.h`'s timing claims.
- One frame. A trial's `next` plus `update` should fit inside one display
  frame, 16 ms, so experiment code calls them between trials with no thread
  and no pacing. Where a configuration cannot, the header says so with a
  measured number and names the knob that brings it back under; see
  [Frame budget](#frame-budget).

## Non-goals

- Fitting. A psychometric function fit with confidence intervals (psignifit,
  Palamedes `PAL_PFML_Fit`, mQUESTPlus `qpFit`) is an offline job on the
  history the headers hand out. `ysp/quest.h` reports posterior summaries
  on its grid and nothing off it.
- Interleaving, blocking, randomization of staircases. PsychoPy's
  `MultiStairHandler` is a loop over handles; the caller writes it.
- PEST (Taylor & Creelman 1967), SIAM (Kaernbach 1990), UML (Shen &
  Richards 2012), MOBS, and the running-fit method (`PAL_AMRF`). Each is
  a page of rules with its own edge cases and none is in wide use in the
  target labs. PEST in particular is decided against: it survives only as
  a deprecated Palamedes function. The staircase header has room for a
  `rule` enum entry if one becomes needed.
- Variational inference, gradient-optimized acquisition over a continuous
  domain, and monotonic rejection sampling. AEPsych has these on top of
  PyTorch; the header takes what does not need an optimizer and an autodiff
  engine. Monotonic projection needs neither, so `ysp/aep.h` has it
  (`desc.monotone_dims`).
  See [what ysp/aep.h leaves out](#what-yspaeph-leaves-out-of-aepsych-and-why).
- Threads inside the method headers. `next`, `update` and the fits are
  pure functions on a handle, and the headers include nothing. Running
  them on another thread is a separate, opt-in layer; see
  [Inference on a thread](#inference-on-a-thread).

## Prior art, and what each header takes

| Method | Where it exists | Header | What is taken |
|---|---|---|---|
| Transformed up/down (Levitt 1971) | PsychoPy `StairHandler`; Palamedes `PAL_AMUD`; every lab's own | ysp/stair.h | PsychoPy's step schedule per reversal, `applyInitialRule`, `nUp`/`nDown`; Palamedes' separate up/down step sizes and `stopCriterion` trials-or-reversals; both count the level before the turn as the reversal level |
| Weighted up/down (Kaernbach 1991) | Palamedes `PAL_AMUD` (via step sizes) | ysp/stair.h | `step_down_scale`, with `yst_weighted_scale(p)` so the caller states a target p instead of a ratio |
| Accelerated stochastic approximation (Kesten 1958) | Palamedes; Treutwein 1995 review | ysp/stair.h | The rule as Treutwein states it; the step shrinks with reversals, no schedule |
| QUEST (Watson & Pelli 1983) | Psychtoolbox `QuestCreate`/`QuestUpdate`/`QuestQuantile`/`QuestMean`/`QuestMode`; PsychoPy `QuestHandler` (a port) | ysp/quest.h | The placement rule (`YQST_SELECT_QUANTILE`, `MEAN`, `MODE`), the Gaussian prior on log threshold (`yqst_prior_normal`), the log-Weibull (`YQST_PF_GUMBEL`), fixed beta/delta/gamma as one-point axes |
| Psi (Kontsevich & Tyler 1999), Psi-marginal (Prins 2013) | Palamedes `PAL_AMPM`; PsychoPy `PsiHandler` | ysp/quest.h | Threshold and slope free, entropy selection; nuisance axes marginalized out of the selection entropy |
| QUEST+ (Watson 2017) | mQUESTPlus (MATLAB); questplus (Python); PsychoPy `QuestPlusHandler` (wraps questplus) | ysp/quest.h | The whole model: product grids, K outcomes, a psychometric function callback, expected-entropy selection with optional marginalization, precomputed likelihood table; the same grid layout (last axis fastest) so mQUESTPlus' examples reproduce cell by cell |
| Quick CSF (Lesmes et al. 2010) | qCSF MATLAB; AEPsych has no equivalent | ysp/quest.h | A `YQST_PF_CUSTOM` example, not a built-in: the model is four parameters and a formula |
| GP classifier for psychophysics (Owen et al. 2021) | AEPsych `GPClassificationModel` | ysp/aep.h | Probit GP over a box, RBF-ARD kernel, Sobol init phase (Halton here), LSE / BALV / BALD acquisitions on a candidate set, hyperparameter fitting by marginal likelihood |
| Semiparametric model (Keeley et al. 2023) | AEPsych `SemiParametricGPModel`, `HadamardSemiPModel` | ysp/aep.h | The linear-in-intensity latent as a kernel: `k_a(c,c') + x x' k_b(c,c')`. Keeley's positive slope constraint is not taken; see below |
| Look-ahead LSE acquisitions (Letham et al. 2022) | AEPsych `EAVC`, `LocalMI` | ysp/aep.h | The rank-one look-ahead posterior on the candidate set, as Letham et al. construct it; `GlobalMI` is not taken |
| Ordinal outcomes | AEPsych `OrdinalLikelihood` | ysp/aep.h | Cumulative-link likelihood with K - 1 cutpoints as hyperparameters |
| Continuous outcomes | AEPsych `GaussianLikelihood` | ysp/aep.h | Exact GP regression with a noise hyperparameter, same acquisitions on E[y] |
| Categorical outcomes (unordered) | none in AEPsych; Rasmussen & Williams sec. 3.5 | ysp/aep.h | K latents with a shared kernel, softmax likelihood, Laplace per R&W |

Palamedes' MATLAB `PAL_AM*` family and PsychoPy's handlers are the two
reference implementations for staircase semantics because they document
their edge cases (which reversal counts, when the schedule advances, what
happens at a limit). mQUESTPlus is the reference for QUEST+ because Watson
wrote it alongside the paper and it ships the paper's examples as tests.
AEPsych is the reference for the GP methods; its `tests/` directory has the
synthetic observers and the expected accuracy curves.

## Decisions

### Three headers, not one

A staircase is two hundred lines with no heap. QUEST+ is a table and a
posterior. The GP header is dense linear algebra with a Cholesky in the
loop. A user who wants a 3-down-1-up should not copy a Cholesky, and the
convention that a header `#include`s nothing but `ysp/rt.h` means the
psychometric function catalog cannot be shared. The catalog is five
formulas; `ysp/quest.h` carries it and `ysp/aep.h` does not need it (its
psychometric function is the link). Nothing is duplicated.

The cost is three prefixes to remember (`yst_`, `yqst_`, `yaep_`) and
three copies of the `next`/`update`/`done` convention in three manuals. The
convention is stated once, here, and each manual points at the others.

### No ysp/rt.h dependency

These headers do no I/O and take no timestamps. A caller who wants to know
how long `yaep_update` took brackets it with `yrt_now_ns` in their own
code; the header does not pretend to a timing claim it cannot make.
`YSP_THREADS_<name>` is `OFF` in the registry. The one exception is the
opt-in async layer of `ysp/quest.h` and `ysp/aep.h`, which includes
`ysp/rt.h`; `YSP_ASYNC_<name>` builds their compile check and test a second
time with it on.

### `next` and `update` are separate, and `update` takes the stimulus shown

Every toolbox gets this partly wrong. PsychoPy's `StairHandler` records the
intensity it proposed, not the one shown, so a display that quantized it
records a fiction. Psychtoolbox `QuestUpdate` takes the intensity shown,
which is right, but `QuestQuantile` is stateless so the two cannot be told
apart afterward. Here `next` proposes, `update` records what was shown next
to what was proposed, and the rule steps from its own proposal (staircase)
or updates from the shown value (QUEST+, GP; off-grid values evaluate the
model rather than reading the table). The history holds both, so an
analysis can see the quantization.

### Stimuli by index in ysp/quest.h

`yqst_next` returns a grid index, not a value, because the likelihood table
is indexed and an index is what `yqst_update` needs to read one row of it.
`yqst_stim_value` converts; `yqst_update_values` accepts a value for the
off-grid case at a cost of P callback evaluations. A stimulus struct with
both would have forced every caller to carry `YQST_MAX_STIM_DIMS` doubles
per trial.

### Multi-outcome in both Bayesian headers

Binary outcomes are the psychophysics of 1983. A rating scale gives
several times the information of a yes/no per trial (Klein 2001), a
which-interval response tells the model which distractor won, and a
matching task has no binary outcome at all. `ysp/quest.h` has always been
multi-outcome: the psychometric function callback fills K probabilities
per cell and the selection entropy sums over K. `ysp/aep.h` gets four
likelihoods:

| Likelihood | Latents | Inference | Outcome type |
|---|---|---|---|
| Bernoulli | 1 | Laplace | `int` in {0, 1} |
| Ordinal (cumulative link, K - 1 cutpoints) | 1 | Laplace | `int` in 0..K-1 |
| Categorical (softmax) | K | Laplace, block structure per R&W sec. 3.5 | `int` in 0..K-1 |
| Gaussian | 1 | exact | `double` via `yaep_update_real` |

The level-set acquisitions and `yaep_threshold` need a scalar target
quantity, which each likelihood defines: `P(y = 1)`, `P(y >= k*)`,
`P(y = k*)`, or `E[y]`. BALD is defined for all four without change. The
one approximation is under categorical, where BALV, BALD and the
look-aheads score each class latent one-against-the-rest and sum, because
the exact expectation over K coupled latents is a K-dimensional integral.
That approximation is not checked yet. The check is a Monte Carlo score
through `desc.rng`; if the disagreement matters, the Monte Carlo score
becomes an option. See [Open items](#open-items).

Ordinal cutpoints are hyperparameters. With a free mean they are not
identifiable, so the first cutpoint is pinned to zero when fitting. That
keeps `hyper.mean` meaningful across likelihoods.

### Ties are a desc field

Expected-entropy ties are real: stimuli far from the posterior mass have
expected entropies equal to more digits than a tolerance can separate, so
the argmin is a set early in a run and at the grid edges. The rule that
resolves the set is set at open (`yqst_desc.tiebreak`), applied every
trial, and recorded in the history through the proposal. Three
deterministic rules (lowest index, nearest the last stimulus, alternate
ends) and one random rule that needs the caller's generator. The default
stays lowest-index because it is what every existing implementation does,
so verification against them is exact; the manual says it is biased.

The same `desc.rng` callback is what enables Watson's random stimulus
subset per trial (`yqst_desc.subset_size`), which is the recommended fix
for entropy selection sitting on one stimulus. The header never seeds
anything: the posterior is a function of the history alone, and the
proposals are a function of the history and the caller's seed.

### One allocation, sized in advance

`yqst_memory_size` and `yaep_memory_size` return the bytes a desc needs,
and `desc.memory` lets the caller provide them. Otherwise `open` calls
`YQST_MALLOC`/`YAEP_MALLOC` once. The staircase handle is fixed-size and
inline (`YST_MAX_TRIALS`). This follows `yrt_worker`, which owns no
heap, as far as a variable-size table allows.

### Custom models: a batch callback in QUEST+, none in the GP header

A QUEST+ model is evaluated once per cell at open, S x P times, and never
again while the table stands. In C a per-cell callback is fine. Through a
binding it is not: a Python call costs microseconds, so a Psi-marginal grid
takes seconds at open and a CSF grid takes minutes. questplus (Python)
avoids this by evaluating the model as one broadcast array expression, and
`ysp/quest.h` gets the same shape as a second entry point, `pf_batch`:
one call with the stimulus and parameter grids in, the whole S x P x K
table out. A binding wraps a NumPy-vectorized callable in it with no
per-cell overhead, and refuses `no_table` with a callable, since that
would be one full-table call per trial.

The GP header has no custom likelihood. A likelihood there must supply
log p(y | f) with two derivatives, be log-concave for the Newton iteration
to be safe, and be integrable against a Gaussian for every acquisition;
the header would call it N times per Newton step and 20 K M times per
selection, which is past the frame budget for any binding. The modeling
freedom of a GP is in the kernel and the latent, not the link, and the
thing people actually want from a custom likelihood, a floor and a
ceiling on a 2AFC, is two desc fields (`guess`, `lapse`) on the built-in
Bernoulli and ordinal links. A batch kernel callback (`K(X, X')` in one
call) is the extension point worth adding. It is not implemented.

### No per-cell logarithm, and no intrinsics

The v0.1 implementation took a logarithm per table cell and ran at 8 ns
per cell. Two changes brought the joint path to about 1 ns per cell,
measured, and the marginal path to the same:

- The expected posterior entropy decomposes as
  `H(theta) - H(y | s) + sum_theta post(theta) h[s][theta]`, where
  `h[s][theta]` is the outcome entropy of one cell and depends on the table
  alone. Tabulated once at open (S x P floats), it turns a selection into
  contiguous multiply-adds plus S x K logarithms. The marginalized path
  cannot use it (its conditional entropy depends on the posterior over the
  nuisance axes) and does not pay for it.
- The table is stored `L[s][k][theta]` with theta fastest, and the
  parameter axes are permuted internally so the nuisance axes are slowest.
  Every accumulation is then a contiguous dot product written with four
  independent accumulators, which gcc at -O3 and MSVC at /O2 vectorize
  without fast-math. The public index order is unchanged; the posterior is
  permuted back at the boundary.

Explicit SIMD intrinsics were then measured to be worthless: a probe that
sweeps the same bytes with no arithmetic runs at the same speed, and
building with `-mavx2 -mfma` changes nothing. The header has no intrinsics
and says why.

Two precision experiments followed, both measured against the double
build and both removed (the header's PRECISION section keeps the numbers,
since the code is gone). A float posterior with float accumulators buys
1.1x at the documented `-O2` build and 1.3 to 1.5x at `-O3`, on
selections that already fit the frame budget more than ten times over,
and it was slower on the one configuration that misses the budget
(`no_table`). A 16-bit likelihood table halves the arena and runs 2 to 3x
slower, even with the F16C hardware conversion, because the conversion
per element costs more than the bandwidth it saves. The sweep is balanced
between bytes moved and work per byte, so neither lever moves it far.
The QUEST+ header stays double throughout, with the float table, and
`subset_size` remains the knob that divides the cost.

### Float table, double everything else

The QUEST+ likelihood table is the only large object and it is read once
per trial, front to back. Storing it as `float` halves the bytes moved per
`next`, which is the whole cost of `next`. Every accumulation is `double`
and the posterior is `double`, so the seven digits of a float likelihood
are lost into a renormalized posterior every trial and never compound.
mQUESTPlus and questplus use double throughout. On the paper's examples
the largest difference in posterior mass is 4.3e-7 against mQUESTPlus and
6.8e-8 against questplus, which is the float table.

### The linear algebra in ysp/aep.h

Four places where the naive cost is not the right cost.

- **Look-ahead posteriors are rank-one updates.** EAVC and localMI need
  the posterior at every candidate after one more observation at each
  candidate. With the Laplace Hessian W frozen at its current value, the
  posterior covariance after adding a point is the current one minus a
  rank-one term (Woodbury on one new row). So one M x M posterior
  covariance among the candidates, O(M^2 N) to build once per trial,
  gives every look-ahead variance in O(M^2) total. Letham et al. 2022
  construct their acquisitions on exactly this, which is why they are
  practical in AEPsych; the header does the same. Cost of freezing W: a
  one-trial Newton step's worth of approximation, which the warm-started
  refit already accepts.
- **Hyperparameter gradients are analytic.** The Laplace marginal
  likelihood has a closed-form gradient in the hyperparameters, including
  the implicit dependence through the mode (Rasmussen & Williams sec.
  5.5.1, eqs. 5.22 to 5.24). One evaluation is one Laplace refit plus
  O(N^2) per hyperparameter, and gradient ascent with a backtracking line
  search converges in 10 to 30 evaluations. The v0.0 spec's coordinate
  grid search would have been hundreds. The kernel derivatives for RBF-ARD
  and for the semiparametric composite are a few lines each.
- **The kernel Cholesky grows; the Laplace factor does not.** Under the
  Gaussian likelihood the posterior needs only the Cholesky of K + noise,
  which gains one row per trial in O(N^2) (a triangular solve and a
  square root). Under a discrete likelihood the factor is of
  B = I + W^1/2 K W^1/2, and W changes in every entry every trial, so the
  factor is rebuilt each Newton step. That is the O(N^3) that sets the
  512-trial ceiling.
- **Past the ceiling: preconditioned conjugate gradients.** A Newton step
  solves one system in B. Solved by conjugate gradients with the PREVIOUS
  trial's factor as preconditioner, it converges in a handful of
  iterations because B changed by one row and a little in W, and each
  iteration is an O(N^2) matrix-vector product. That makes an update
  O(N^2) times a small constant and moves the ceiling to a few thousand
  trials, bounded by the N^2 memory of K. It is the technique GPyTorch is
  built on (Gardner et al. 2018) and the one Cunningham et al. 2008 used
  for Laplace point-process GPs. It came after the semantics were
  verified: `desc.pcg_threshold` (v0.11.0) for updates, measured at 15,
  130 and 1170 ms at N = 512, 1024 and 2048 against 34, 340 and 3860 ms
  factored, and `desc.fit_pcg` (v0.14.0, opt-in) for the fits. See
  [What AEPsych covers](#what-aepsych-covers-and-what-this-collection-did-about-it),
  item 6.

Inducing-point and Nyström approximations were considered and rejected:
they trade accuracy for scale at N far above any adaptive session, and
they add a second set of points to choose.

Two later measurements. The EAVC look-ahead as first written made
131,328 erfc calls per selection at M = 256 (a 20-node quadrature per
candidate pair), which is why MSVC's Release build missed the frame budget
at every N; the closed-form look-ahead needs one rational Phi per pair and
none of the quadrature, and the selection went from 15 to 9 ms at
N = 100. And a single-precision build (`YAEP_REAL`) halves the memory and
changes nothing else: the Laplace factor of B is well enough conditioned,
the accuracy numbers move within the trial noise, but the time is a wash
because every loop is scalar at -O2 and the matrices reach the same cache
level either way. Double stays the default; float is documented as the
memory-bound build, and the exact Gaussian path refuses it because it
factors the raw kernel matrix.

### A posterior for the hyperparameters, not a likelihood

Type-II maximum likelihood, the fit the spec asked for, interpolates its
own trials. The audiometric benchmark showed it as the default behavior,
not a corner: the output scale sat at its ceiling in 1580 of 1600 runs,
the log marginal likelihood was far above what 150 Bernoulli trials can
support, and raising the ceiling made it worse. It also explained two
response streams that came out bit-identical: a diffuse posterior with a
mean at its bound sends the level-set acquisition to the box edges, where
the observer is deterministic. The fix is weak log-normal priors on the
lengthscales (centered on the default, sd of log 1) and on the output
scales (centered on 1, sd of log 0.7), in the objective and in its
analytic gradient, with a desc switch to turn them off. Measured on the
metabolic phenotype at beta 2: threshold error 3.14 to 1.98 dB, bounds
hit 20 of 20 to 0, log marginal -20.6 to -58.7 nats. AEPsych puts gamma
priors on the lengthscales for the same reason; this is the same choice
in a different shape.

Two consequences were measured too. A coarser candidate set had looked
like a regularizer before the prior (halving LSE's error); after the prior
the finer set wins, so the candidate count is a cost knob and nothing
else. And a lengthscale at its UPPER bound is normal for a dimension with
no structure (a flat audiogram), so the manual no longer calls every bound
hit a warning.

### The semiparametric kernel has a domain

`yaep_next` now falls back to a Halton point when the acquisition
landscape is flat or the same candidate has won three trials running, which
un-freezes the failure the benchmark found (one identical result across 20
seeds). What it cannot fix is the model: with 140 dB of intensity and a
0.5 dB psychometric width, the intercept GP of `a(c) + b(c) x` must carry
tens of latent units and type-II fitting prefers an infinite slope to a
large intercept. With ceilings raised and the prior off it reaches 6 dB
where the RBF kernel reaches 2. The manual says so with the numbers and
names the way out, rescaling the intensity axis so the rise is of order
one. Keeley's positive-slope constraint would not have changed this.

### The transition band needs Keeley's model, not a kernel trick

Both libraries leave a mean absolute error of about 0.22 in the transition
band (true p between 0.05 and 0.95) of the audiogram: a stationary RBF
fits one lengthscale per axis, 30 to 40 dB in intensity, and the true rise
is 7 dB wide while the field is flat over 140 dB. Rescaling the intensity
axis does not help the RBF (ARD already scales it); input warping bends
the axis globally while the rise sits at a different intensity in every
context; a Gibbs kernel with a local lengthscale would need to know where
the threshold is. The model that represents the field is the
semiparametric one in Keeley et al.'s own form: two latent GPs over the
context, a threshold location m(c) in intensity units and a log-slope
g(c), with f = exp(g(c)) (x - m(c)). Any rise width at any location,
monotone in intensity by construction, the threshold curve a latent with
its own posterior sd, and no intercept-scale defect because m has the
box's units. It is Laplace over 2N latents with a 2 x 2 Hessian block per
trial, which is the categorical machinery with a different likelihood.
Added as `YAEP_MODEL_PSYCHOMETRIC` beside the existing kernels (v0.3.0),
so every earlier number reproduces. Two implementation facts: the exact
Hessian is indefinite wherever a residual is nonzero, so the Newton step
uses the Gauss-Newton W, which is rank one per trial and collapses the 2N
system to one N x N Cholesky; and a normal prior on the mean log-slope is
required, without it a separable early run drives the slope to its
ceiling at trial 5 and the acquisition never moves again.

Measured on the audiometric benchmark at 150 trials, 20 replications:
field error falls 3 to 6 times against the RBF (0.012 to 0.036 against
0.06 to 0.09), the transition band goes from 0.20 to 0.25 down to 0.16 to
0.19 at beta 2, and on the metabolic phenotype every acquisition beats
both the RBF and the staircase that was told the slope (1.4 to 1.9 dB
against 2.0 to 2.6). On the sensory phenotype the medians are competitive
but a few replications reach 7 to 12 dB: the threshold GP fits a
3.8-octave lengthscale across a 27 dB climb and the log-slope GP absorbs
the curvature by flattening the rise. The fix (v0.4.0) is the log-slope
output-scale prior centered at 0.3 rather than 1, since psychometric
slopes vary across context by tens of percent, not by factors of four:
the worst sensory replication falls from 11.6 to 5.6 dB and the metabolic
cells stay inside their intervals. (The first diagnosis, a long threshold
lengthscale, was wrong; it came from one dumped replication and the
manual says so.) The hyperparameter gradient is analytic, through one
adjoint solve by preconditioned CG on the true Hessian, and the fit costs
2.7 to 5.7 ms per trial against the GP model's 1.2; the Gauss-Newton mode
search's linear convergence is what remains, and an exact-Newton attempt
was measured slower and reverted. The look-ahead is rank one, not two,
because the Gauss-Newton W of one trial is rank one on its context's
(m, g), so EAVC and LOCALMI work on this model.

Final audiometric table, 150 trials, 20 replications, threshold MAE in
dB: on three of the four cells every acquisition on this model beats
both the RBF and the staircase that was told the slope (BALD 0.93 to
1.42 against the staircase's 2.06 to 2.60, worst replication 3.1 dB);
on sensory at beta 0.5 EAVC and LOCALMI match the staircase (1.98 and
2.05 against 1.90) and the per-point acquisitions do not. Field error is
0.007 to 0.033 against the RBF's 0.06 to 0.09. Through the binding on the
metabolic phenotype the model with EAVC reaches 1.26 +- 0.09 dB and a
field error of 0.012, against AEPsych's best of 1.29 dB and 0.18. The
fitted mean slope is about half the true one (0.22 to 0.31 per dB against
0.5), and that was measured to be the mean log-slope prior on purpose:
the Laplace marginal itself peaks at the true slope (0.51), the
Gauss-Newton approximation moves it by 0.1 nats, and every prior loose
enough to recover the slope reopens the LSE tail to 7 to 18 dB, because a
level-set acquisition places its trials where the slope is unidentified
and a fit with room to steepen does so into the flat tail. BALV, which
samples the field, recovers the slope under any prior. The band error
tracks the threshold error, not the slope (a model with the exact slope
and our threshold error scores the same band error), so the default stays
(v0.4.1, prior parameters as compile-time knobs).

### Laplace, not variational, in ysp/aep.h

AEPsych's model is a variational GP classifier fit by gradient descent on
the ELBO with inducing points. That is the right tool at ten thousand
trials with a GPU; it is not available in a dependency-free C header.
Laplace's approximation (Williams & Barber 1998) needs Newton's method on
an N-vector and a Cholesky per step, and for a probit likelihood on binary
psychophysical data with N in the low hundreds it is well studied: it
underestimates posterior variance where the posterior is skewed (few
trials, extreme stimuli), and its threshold estimates agree with EP and
MCMC well inside the trial-to-trial noise of the experiment (Nickisch &
Rasmussen 2008, Table 1 and Fig. 4). Measured against AEPsych on the
audiometric observer, with both models fitted from scratch to the same
trials and AEPsych's output scale fitted, the fields differ by 0.016 and
the threshold curves by 0.14 dB (see [Verification](#verification)).

### The semiparametric model is a kernel

Keeley et al. 2023 write the latent as `f(x, c) = k(c) (x - m(c))` with GP
priors on `k` and `m` and constrain `k > 0`. Expanding, `f = a(c) + b(c) x`
with `a = -k m`, `b = k`, and the sum of a GP over `c` and a GP over `c`
scaled by `x` has the kernel `k_a(c, c') + x x' k_b(c, c')`. That is a
plain GP with a composite kernel, so the Laplace machinery is unchanged
and the model costs one enum value. What is lost is the positivity of the
slope: `b(c)` is a GP and can go negative where data are thin. The manual
says how to detect that (`yaep_threshold_multi_cross`, a slope hyper at
its bound). The positive-slope form is `YAEP_MODEL_PSYCHOMETRIC` (v0.3.0),
Laplace on the joint of a threshold GP and a log-slope GP; see
[The transition band needs Keeley's model](#the-transition-band-needs-keeleys-model-not-a-kernel-trick).

### Candidate set, not an optimizer

AEPsych optimizes the acquisition function with L-BFGS from a set of
starting points. The header scores a candidate set (a caller's list, a
product grid, or a Halton set) and takes the best. On a stimulus space a
display can only produce at discrete values this is the right answer; on a
continuous one the loss is a fraction of the candidate spacing, which is
smaller than the resolution of any threshold estimate at the trial counts
involved. The candidate set is also the search set for `yaep_threshold`
and the set the look-ahead volume is measured over, which is why the
look-ahead scores are defined on candidate indices rather than points.

### Halton, not Sobol

The init phase and the random acquisition need a low-discrepancy sequence.
Scrambled Sobol needs direction-number tables; Halton in eight dimensions
needs the first eight primes and a radical-inverse loop. The difference in
discrepancy is invisible at ten to twenty init trials.

### Gauss-Hermite, not Monte Carlo

AEPsych's `MCPosteriorVariance` and `BernoulliMCMutualInformation` draw
posterior samples. The expectations they need are one-dimensional integrals
of a link function against a Gaussian, so 20-node Gauss-Hermite quadrature
gives them to double precision with no random source and no variance from
run to run. The names `YAEP_ACQ_BALV` and `YAEP_ACQ_BALD` follow Houlsby
et al. 2011, which is what AEPsych's classes implement. The categorical
likelihood is the exception; see the multi-outcome decision above.

### What `ysp/aep.h` leaves out of AEPsych, and why

| AEPsych feature | Status | Reason |
|---|---|---|
| Variational inference, inducing points | not taken | needs an optimizer and autodiff; Laplace with conjugate-gradient Newton steps covers N to 2048 |
| `MonotonicRejectionGP` | not taken | rejection sampling needs a random source; the psychometric model is monotone by construction |
| Monotonic projection of the posterior mean | taken | `desc.monotone_dims` (v0.10.0), opt-in |
| `EAVC`, `LocalMI` (look-ahead) | taken | rank-one look-ahead on the candidate set, O(M^2) per trial after an O(M^2 N) build |
| `GlobalMI` | not taken | needs the look-ahead at every candidate for every candidate's level-set membership, O(M^3); EAVC covers the use |
| Ordinal, Gaussian likelihoods | taken | see the multi-outcome decision |
| Categorical (softmax) likelihood | added | not in AEPsych; R&W sec. 3.5 |
| Pairwise / preference models | taken | `YAEP_LIK_PAIRWISE` (v0.8.0), a probit on the latent difference |
| Optimization acquisitions (UCB, EI, Thompson) | taken | `YAEP_ACQ_UCB`, `_EI`, `_THOMPSON` (v0.7.0) |
| Server, config files, strategies | not taken | the caller's experiment code is the strategy |
| Gamma priors on lengthscales, autodiff ELBO | replaced | weak log-normal priors, set at run time through `desc.priors`, and analytic marginal-likelihood gradients with bounds; a bound hit is reported |
| Sobol init | replaced | Halton |

## Frame budget

The number that matters to experiment code is not flops per trial but
whether the two calls between trials fit in the 16 ms a 60 Hz display gives
between one frame and the next. When they do, the trial loop is a straight
line. When they do not, the caller has to move the work to a thread, pace
it across frames, or accept a variable inter-trial interval, and each of
those is a source of bugs the header cannot see. So every bench prints its
times against the 16 ms line, the manuals say which configurations fit on
the bench machine, and where a configuration does not fit the header
offers a knob rather than a shrug. The measured column quotes the header
manuals: ysp/quest.h 0.5.2 and ysp/aep.h 0.14.0 on one i7-1360P laptop core
under WSL2, gcc -O2.

| Header | What costs | Measured `next` + `update` | Knobs that bring it under the line |
|---|---|---|---|
| ysp/stair.h | nothing; O(1) per call | 1 to 4 us through the Python binding ([adapt_comparison.md](adapt_comparison.md)) | none needed |
| ysp/quest.h | one pass over the S x P x K table per `next` | Psi-marginal 0.91 ms; with `no_table` 17.9 ms, which does not fit | `subset_size` (Watson's random subset, divides the pass), a coarser grid, `no_table` is the slow direction, the async layer |
| ysp/aep.h | a Cholesky per Newton step per `update` (N^3 / 3), M N^2 per `next`, and the hyperparameter fit (10 to 40 refits) | M = 256 in 2-D: fits to N = 347 with LSE and N = 285 with EAVC, both with an exact refit every trial; past 400 with no refit inside the loop | `refit_every` (rank-one update between full refits), fewer candidates, `yaep_fit_step` (one gradient step per interval), `fit_every`, the async layer |

The QUEST+ path is memory-bound once the per-cell logarithm is gone (the
entropy decomposition in the header's LAYOUT section) and the loops are
contiguous dot products; the memory floor is set by the table size, which
is a design choice. The GP path is compute-bound and grows with the trial
count, so the manual states the largest N that fits for the configurations
its bench runs, measured, and the caller picks a `max_trials` or a
`refit_every` from that. The GP hyperparameter fit never fits: it is
resumable one step at a time (`yaep_fit_step`) so it can be spread across
intervals, or run in a block break.

None of this is a timing claim about a rig. The benches measure one
machine; the STATUS blocks report what they measured; a rig runs the bench
before it trusts the number.

## Inference on a thread

Cutting the runtime has a floor: a QUEST+ selection is bound by the table's
bytes, and a GP refit grows with the trial count, so a 400-trial session or
a hyperparameter fit will never fit a frame. The other half of the answer
is to not do the work on the frame loop's thread at all: submit the
response, let a background thread run `update`, `next` and any fitting,
and read the proposal out when the inter-trial interval ends. That turns
the frame budget from a hard constraint into a contract the code can check
("is the next stimulus ready?"), and it makes background fitting free: the
thread runs `yaep_fit_step` whenever nothing is queued.

Three pieces, in order of dependency:

- **`yrt_pump` in ysp/rt.h.** A worker thread with a fixed-size message
  ring (caller-provided or inline; no heap), an `on_msg` callback the
  thread calls for each message in order, an `on_idle` callback it calls
  repeatedly while the ring is empty and the callback says there is more
  to do, a lock the caller takes to read what the callbacks published, a
  sequence number per submit and a "done through seq" the caller polls or
  waits on with a timeout. It runs at normal or below-normal priority and
  never climbs the scheduling ladder, so it cannot steal from the frame
  loop or from a deadline worker. Stop drains the ring by default, because
  a submitted response must not be lost; an option drops instead. This is
  the generic piece, and the only one with OS code.
- **`yqst_async` and `yaep_async`.** Thin layers, compiled only under
  `YQST_ASYNC` / `YAEP_ASYNC`, which is when those headers include
  ysp/rt.h. `submit(x, outcome)` returns at once; `poll(&x)` returns the
  seq of the proposal that accounts for every submitted update, or 0 while
  one is being computed; `wait(timeout, &x)` is the blocking form for the
  interval; a snapshot struct (proposal, done flag, a threshold at a
  configured context, the posterior mode) is published under the pump's
  lock after every message, so the cheap estimates never race. Anything
  richer stops the pump, which hands the handle back to the caller.
  `on_idle` runs one fit step for the GP header.
- **The bindings** use the async layers, because a Python trial loop is
  exactly the place where a blocking 30 ms call between frames hurts and
  a background C thread costs nothing (it never touches the interpreter).

What it does not do: it does not make a slow configuration fast, it only
moves it; the runtime work above still decides how many trials a session
can afford. And it does not queue past the ring: a caller that submits
faster than the thread drains gets `YRT_ERR_FULL` and keeps its
response, which is the honest failure.

## Sizes and costs

Stated in the headers per configuration; summarized here for the choice
between them.

| Header | Handle | Heap at open | Per `next` | Per `update` |
|---|---|---|---|---|
| ysp/stair.h | 28 KB (28168 bytes; 1024 trials inline) | none | O(1) | O(1) |
| ysp/quest.h | 97 KB (99624 bytes; 2048-trial history inline) | S P K floats, plus S P floats of per-cell outcome entropy on the joint path, plus a few P doubles; Psi-marginal 4.5 MB, qCSF 173 MB or `no_table` | S P (K+1) multiply-adds and S K logs (joint), S P K and S K n_marg logs (marginal); memory-bound at 0.6 to 0.8 ns per cell measured | P K |
| ysp/aep.h | 2.9 KB (2904 bytes); the history is in the heap block, 144 bytes a trial | (1 + K) N^2 doubles, M^2 for a look-ahead acquisition, vectors, the history; N = M = 512 Bernoulli is 6.5 MB with LSE and 10.5 MB with EAVC | M N^2, plus M^2 N and M^2 for a look-ahead | 2 to 4 Cholesky (N^3 / 3 each), K times that under categorical, O(N^2) under Gaussian or past `pcg_threshold`; a hyper fit is up to 40 refits |

The handle sizes are `sizeof` on x86-64 at the default ceilings, and the
heap sizes are what `yqst_memory_size` and `yaep_memory_size` return. The
operation counts are counts, not times. `examples/quest/bench.c` and
`examples/aep/bench.c` time a desc and print the numbers to log.

## Verification

The STATUS block of each header has its own results, with the numbers.
The comparison scripts are in `tests/compare/`. They are
not CI, because AEPsych pulls PyTorch, PsychoPy is a heavy install, and
MATLAB needs a license, but they are the acceptance tests.

### Results

- **All three headers.** Compile checks as C99, C11 and C++17 under
  `-Wall -Wextra -Wpedantic -Wshadow -Werror`, and under MSVC `/W4 /WX` in
  its default C dialect and as C++17. The CMake registry has each header with
  `YSP_THREADS_<name> OFF` and all three platforms in
  `YSP_PLATFORMS_<name>`, and the interface target links `m` on Linux for
  `libm`. `ysp/quest.h` and `ysp/aep.h` also build and run their test with
  the async layer on (`YSP_ASYNC_<name>`), clean under ThreadSanitizer.
- **ysp/stair.h.** `tests/adapt/stair_test.c` checks hand-derived
  tracks for four rules, every step type, the limits and the stop reasons.
  A simulated Weibull observer over 204000 pooled trials puts the
  empirical proportion correct within 0.007 of `yst_convergence_p` for
  1-up-2-down, 1-up-3-down and a weighted 1-up-1-down.
  `compare_stair_psychopy.py` replays fixed response sequences through
  `ysp.stair` and PsychoPy's `StairHandler` (1-up-2-down and 1-up-3-down
  and 2-up-1-down, step schedules, the initial rule on and off, limits,
  linear, dB and log steps): no proposal differs on any trial, the
  reversal indices and levels are the same, and the estimates agree, the
  log and dB cases to 1e-15 because PsychoPy multiplies where the header
  adds in log units. `compare_stair_palamedes.m` replays `PAL_AMUD` through
  the MEX binding in MATLAB R2023a: eight configurations are identical
  trial for trial. The two differences are Palamedes' design choices (it
  keeps the opposite-direction counter when a response causes no step; a
  caller-changed step after the reversing update applies one trial later
  than a schedule does), and ysp/stair.h states both.
- **ysp/quest.h.** `tests/adapt/quest_test.c` makes 14770 checks
  against an independent double-precision reference (15121 with the async
  layer). `compare_quest_questplus.py` runs `ysp.quest` and questplus
  2023.1 on Psi and Psi-marginal grids with one response stream: 180 of
  180 selections identical, no ties, largest posterior difference 6.8e-8,
  which is the float table. The marginalized selection, which questplus
  lacks, was checked against the marginal entropy computed in NumPy from
  questplus's own table. `methods_compare.py --replay QuestPlus.nb`
  replays all 17 runs saved in Watson's own Mathematica notebook, the
  reference implementation behind the QUEST+ paper, through `ysp.quest`:
  1504 trials, 1452 identical selections, 52 ties within 2e-8 bits (38 of
  them exact symmetries of the circular example), 0 differences, and all
  17 final joint modes identical, across every example in the paper
  (threshold; threshold and slope; threshold, slope and lapse; cumulative
  normal; spatial and spatiotemporal CSF; Thurstone scaling; blur; rating
  with 3 and 4 categories; circular categorization). mQUESTPlus itself
  runs in MATLAB through the MEX binding (`compare_quest_mquestplus.m`):
  mQUESTPlus drives each run, both are updated with its stimulus, and on
  the paper's figure 2, 3 and 4 examples plus a marginalized case, 0 of
  424 selections differ (32 ties within 7e-10 bits, resolved by the
  header's tie rule) and the posteriors agree to 4.3e-7. Palamedes'
  `PAL_AMPM` (Psi and Psi-marginal) gives identical selections over 60
  trials each, with posteriors within 5.7e-8.
- **ysp/aep.h, the AEPsych audiometric benchmark in C.** Owen et al. 2021
  benchmark on four audiometric phenotypes (Dubno et al. 2013, thresholds
  at eight frequencies, cubic-spline interpolated, probit latent
  `(x - theta(f)) / beta` over 0.125 to 16 kHz by -20 to 120 dB HL), 5
  Sobol plus 145 adaptive trials, and two metrics: mean absolute error of
  the probability field over a 30 x 30 grid, and of the 0.75 threshold by
  local linear interpolation along the intensity axis.
  `examples/aep/audiometric.c` runs that protocol on `ysp/aep.h` (every
  acquisition, both kernels, and the psychometric model) and on
  interleaved `ysp/stair.h` staircases as the classical baseline the paper
  left out, at 20 replications, and writes the per-trial curves to a CSV.
  The paper reports curves, not numbers, so the C run is compared by eye
  with its Figure 7 and its qualitative findings (LSE best for threshold,
  BALV for the surface), which it reproduces. ysp/aep.h's STATUS block has
  the tables.
- **ysp/aep.h beside AEPsych, in one process.** `compare_gp_aepsych.py`
  runs `ysp.aep` and AEPsych 0.8.0 (variational GP classifier, its own LSE,
  EAVC and BALV acquisitions optimized continuously) on the metabolic
  phenotype at beta 2, 10 replications, the same Sobol init points and
  the same response stream. Threshold error at 150 trials, dB: EAVC 2.09
  against 2.23 (within the replication error), BALV 4.45 against 7.10
  (better), LSE 2.06 against 1.29 (worse, intervals barely overlapping).
  Field error, MAE(p): about half of AEPsych's for every method at 100 and
  150 trials, and AEPsych's plateaus with a narrow interval, which looks
  like a systematic bias of its model on a field whose probit rise spans a
  small fraction of the axis; not verified. Cost per trial: 6 to 13 ms
  against 4 to 8 s, with AEPsych refitting every trial and both sides
  sharing a loaded machine, so the ratio is an order of magnitude, not a
  number. The LSE gap was the candidate grid: with 11 x 21 candidates the
  intensity spacing is 7 dB, and on the same replications a 31 x 61 grid
  reaches 1.14 +- 0.18 dB against AEPsych's 1.29 +- 0.17, at 13 ms per
  trial, while fitting the hyperparameters every trial instead of every
  20 changes nothing and costs ten times more. So the candidate count is a
  cost knob with an accuracy price for the myopic acquisition. The
  header's answer to the grid cost is `refine_steps`, a coordinate-wise
  golden-section refinement of the acquisition between the grid winner's
  neighbors: two rounds take LSE from 2.06 to 1.44 +- 0.16 dB and EAVC
  from 2.09 to 1.44 +- 0.28 on the same replications, inside AEPsych's LSE
  interval and better than its EAVC, for no measurable time, at the price
  of a field error that rises from 0.06 to 0.08 because trials sit closer
  to the level set. The default stays 0 so every earlier number
  reproduces. Through the binding the psychometric model with EAVC reaches
  1.26 +- 0.09 dB and a field error of 0.012, against AEPsych's best of
  1.29 dB and 0.18.
- **ysp/aep.h and AEPsych on the same data.** A same-data run (both models
  fit from scratch to identical trial sequences, Sobol and adaptive, 10
  replications) isolates the model from the acquisition. In the
  transition band (true p in 0.05 to 0.95) the two agree and are equally
  too shallow, since a stationary RBF with a 30 to 40 dB lengthscale
  cannot represent a 7 dB rise. Outside it AEPsych's error is 1.25 to 2
  times this header's (0.163 against 0.082 at 150 adaptive trials), the
  models' p fields differ by 0.03 to 0.08, and their threshold curves by
  1.2 to 2.8 dB, with this header's nearer the truth. The lengthscales and
  means the two fit are close. The one systematic difference is the output
  scale, which AEPsych's default model fixes at 1 (no ScaleKernel) while
  this header fits 4.5 to 6.3, and a latent with prior sd 1 on a 140 dB box
  cannot reach p = 0 or 1 far from the threshold. Rerun with AEPsych's
  amplitude fitted (a ScaleKernel under its box prior on [1, 4]), the two
  models agree on identical adaptive data: fields differ by 0.016,
  threshold curves by 0.14 dB, both at 1.43 dB error. So Laplace with a
  candidate set and variational with an optimizer give the same estimate;
  what differed was a kernel default. The one place AEPsych's freed model
  is better is early (0.14 against 0.23 field error at 25 trials), because
  this header's output-scale prior is centered at 1 and climbs slowly.
  Remeasured on both observers with the center at 3 and with AEPsych's box
  on [1, 4]: either alternative cuts the early field error by a third, but
  a center of 3 lets the scale run to its ceiling in 39 of 40 runs and
  gives the worst late threshold, and the box is a fixed scale of 4 in all
  but name (every fit ends on its edge), which wins late on the steep 1-D
  observer and loses late on the audiogram, where the data want 6 to 7.
  The default stays centered at 1, and the manual tells a session that
  cares about its early estimate to fix the output scale at 4.
- **On real human data the models do not differ.** `compare_gp_csfdata.py`
  cross-validates on Letham et al. 2022's CSF dataset (1001 trials from
  one observer over six stimulus dimensions; downloaded at run time, not
  vendored): held-out log loss 0.529 for AEPsych in every variant, 0.537
  to 0.541 for the GP and psychometric models here, 0.543 for logistic
  regression on the raw inputs, all inside fold noise. Contrast and
  pedestal carry the signal, every model puts temporal frequency and
  eccentricity at long lengthscales, and the field is close to linear at
  this trial count, so any smooth model does the same and the acquisition
  is where methods differ. The model gaps measured on the synthetic
  audiogram come from its curvature. The run found two things in the
  header. The fit's 40-evaluation budget does not converge on this data;
  it is now a runtime knob with a convergence report (v0.13.0). And the
  psychometric model's converged fit cost 541 s against 33 s for the GP
  model at 500 trials in 6-D, on a loaded machine; `desc.fit_pcg`
  (v0.14.0, opt-in) takes the fits to 43 s and 3.4 s on an idle core.
  The same dataset as Letham et al.'s 6-D level-set problem (a GP fit to
  the trials as the truth, target 0.75, 10 Sobol plus 290 trials, 10
  replications) is in [adapt_comparison.md](adapt_comparison.md): the
  psychometric model beats the GP model on field error under every
  acquisition, BALD best; no method resolves the threshold to 0.1 log10
  contrast in 300 trials, and the two candidate truth surfaces (AEPsych's
  fit and this header's, equal in likelihood on the data) disagree by 0.22
  log10 units on that same threshold, so at this trial count the threshold
  metric cannot separate methods and the field error can. The run also
  found the GP model fitting itself into a flat p = 0.5 state under a 0.5
  guess floor. The prior on the GP model's mean (v0.5.0) fixed that, and
  the test replays the case.
- **The methods against each other.** [adapt_comparison.md](adapt_comparison.md)
  runs the three headers on Watson's examples, the AEPsych paper's test
  functions and the csf6 problem, and says which method to use when.

### Open items

- **Laplace band coverage.** The coverage of the `[lo, hi]` band from
  `yaep_threshold` at 95% nominal is not measured.
- **Categorical acquisitions.** The one-against-the-rest scores under
  `YAEP_LIK_CATEGORICAL` have no Monte Carlo check through `desc.rng`.
- **Ordinal likelihood.** No comparison with AEPsych's `OrdinalLikelihood`
  on a simulated rating observer.
- **Psychtoolbox QUEST.** `QuestDemo` and the `YQST_SELECT_QUANTILE`
  placement are not compared; Psychtoolbox is the one QUEST reference
  implementation not run.
- **Platforms.** Nothing has run on a platform other than x86-64, or in a
  browser.
- **A second translation unit.** Each compile check is one translation
  unit with the implementation. No check includes a header a second time
  without it, in the same program.

## Open and close, messages and signed timeouts (stair v0.2.0, quest v0.6.0, aep v0.15.0)

These versions apply the repository conventions (README.md, Conventions)
to the three headers. No number changed: the posterior, the proposals and
the snapshots are bit for bit the previous versions'.

- **One lifecycle.** The async layers make a thread from a desc, so they
  open and close: `yqst_async_open()`, `yaep_async_close()`. A snapshot
  restore fills a handle and allocates, so it is an open too:
  `yqst_open_saved()` and `yaep_open_saved()`. `yst_close()` is new and
  releases nothing, so that every open has a close.
- **Messages from the calls that change the handle.** Before, only open
  wrote the message. Now a failed next, update or fit writes it too, so a
  log line can say why the trial loop stopped. The work moved into private
  bodies and the public call adds the message, so the success path gains
  one branch and no `snprintf`. The const queries (predictions,
  estimates, grids) still return a code or NaN and write nothing: a reader
  that holds a const pointer never writes the handle. `yaep_next()` and
  `yaep_argmax()` return -1 for a point that is not a candidate, so only a
  code below -1 writes a message.
- **Signed timeouts.** `yqst_async_wait()` and `yaep_async_wait()` take an
  `int64_t` timeout in nanoseconds, the type of `yrt_pump_wait()`. The
  Python and MATLAB bindings keep `timeout_s` in seconds and convert; their
  "wait forever" is `INT64_MAX` now.
- **`yqst_prior_normal()` returns an int.** False meant only "bad
  arguments", so it is 0 or `YQST_ERR_ARG` like every other call that can
  fail.
- **USAGE builds as C99 and C++17.** The examples zero the desc and set
  fields. Designated initializers are C++20, and MSVC rejects them in
  C++17 (C7555).

Checked on 2026-10-10 (Windows 11, MSVC 19.44 /W4 /WX as C and C++17, with
and without the async layers, and MinGW-w64 gcc -Werror): the three tests
pass with new checks for the messages and for close on a zeroed handle,
after a failed open and twice. The compile checks name the open, close and
wait calls by type.

## Bindings

In the shape of the transport bindings: one distribution per header under
`bindings/python/` on the Limited API, sharing the `ysp` namespace
(`ysp.stair`, `ysp.quest`, `ysp.aep`), and one MEX function per header
under `bindings/mex/` (`ysp_stair`, `ysp_quest`, `ysp_aep`). No NumPy
dependency: axes and candidate sets come in as sequences or buffers, and
the posterior and the history come out as buffers NumPy can read. The
`rng` callback is a Python callable, or in MATLAB a function handle or an
integer seed for a splitmix64 generator the module keeps. A custom
psychometric function in `ysp.quest` is a vectorized callable passed as
`pf_batch`, which the header calls once per stimulus instead of once per
cell (see
[Custom models](#custom-models-a-batch-callback-in-quest-none-in-the-gp-header)).
`Async` in `ysp.quest` and `ysp.aep`, and `async_*` in the MEX functions,
wrap the headers' async layers.

The point is comparison. Each binding came with the comparison script that
runs it beside its reference implementation on one response stream; see
[Verification](#verification).

## Open questions

- Whether the categorical one-vs-rest acquisition is good enough, or the
  Monte Carlo score should be the default when `desc.rng` is present. Not
  measured; see [Open items](#open-items).
- Ordinal cutpoint fitting with few trials per category is poorly
  conditioned. The header defaults to fitting them when `desc.fit` is on.
  Whether that default should flip to fixed is not measured.

Answered since: the GP trial ceiling. Conjugate-gradient Newton steps
(`desc.pcg_threshold`) lift the Laplace refactorization. The history now
lives in the memory block, so `YAEP_MAX_TRIALS` can be raised to 2048
without growing the handle, and the N^2 matrices are the caller's memory
(97.9 MB for Bernoulli at N = 2048, M = 256). A single-precision build
(`YAEP_REAL=float`) halves the matrices and passes the same test with
the tolerances its PRECISION section lists, but it saves memory, not time,
so double stays the default.

## WebAssembly

The four computation headers compile under emcc with no change and their
full test suites pass under node with the same numbers as native
(checked in the `emscripten/emsdk` image; CI runs the same). Two facts
for a browser rig: the handles are large and the tests keep them on the
stack, so the build needs a real stack (`-sSTACK_SIZE`) and memory growth
for the arenas; and ysp/rt.h 0.4.0 has an Emscripten branch (clock via
`clock_gettime`, the scheduling ladder collapsed to normal, waits as
nanosleep plus spin for worker threads, node and tools, never the browser
main thread, threads under `-pthread` with cross-origin isolation), so
the pump and both async layers run under node: the QUEST+ and GP async
suites pass there bit-identical to their synchronous runs. Nothing has
run in a browser yet; the main-thread and clock-coarsening statements
are the browsers' documented rules. The synchronous calls fit a
`requestAnimationFrame` interval as they fit a display frame, so a
browser experiment needs neither threads nor the pump. ysp/serial.h has
no wasm backend; a Web Serial transport would be a new backend.

## What AEPsych covers, and what this collection did about it

Compared against AEPsych's current model, acquisition and generator lists
(2026-09-23). Every item below is implemented, by ysp/aep.h 0.14.0 at
the latest, with its own tests, benchmark numbers in the header's manual,
and a version bump; none changed a measured default, so every earlier
number still reproduces.

1. Runtime prior knobs. Every prior parameter is in a `priors` block in
   the GP desc with zero as the measured default, so a binding can set it
   per session; `no_hyper_prior` stays as the master switch.
2. Optimization acquisitions on the GP: UCB, expected improvement and
   Thompson sampling, with `yaep_argmax`, for "find the setting the
   observer prefers" rather than a level set.
3. Pairwise comparisons: a pairwise probit likelihood (two points per
   trial, which was preferred), Laplace on the difference, a latent
   utility with its argmax.
4. Mixed parameters: integer and categorical dimensions with a categorical
   kernel, honored by candidates, init, refinement and the threshold search.
5. Monotonic projection of the posterior mean in chosen dimensions.
6. Scale: conjugate-gradient Newton steps with the previous factor as
   preconditioner (`pcg_threshold`, default 512, so earlier results are
   untouched). Measured update cost at N = 512, 1024 and 2048: 15, 130 and
   1170 ms against 34, 340 and 3860 ms factored; the crossover is near
   N = 100. Past about 400 trials no exact update fits a frame either
   way, and the manual points long sessions to the async layer or
   `refit_every`. Fits and the psychometric model's mode search got the
   same treatment in v0.14.0 behind `fit_pcg`, off by default: one
   factorization per search for the log determinant, which has to stay
   exact because most line-search decisions are within 1e-3 nats, and
   preconditioned CG for the solves. At 500 6-D trials a GP fit goes from
   6.9 to 3.4 s and a psychometric fit from 168 to 43 s on an idle core.
   Opt-in because on the psychometric model's flat lengthscale ridge the
   tolerance-level differences end a fit at a slightly different point
   (log marginal -68.503 against -68.500, threshold 1.318 against
   1.314 dB); the default path is bit-identical to v0.13.1.
7. Snapshots for the GP and QUEST+ handles, in the shape of
   `ytr_save` / `ytr_load`, so a session resumes without replay. The
   GP test resumes 132 sessions over 11 configurations, and the QUEST+
   test cuts 5 configurations at 4 trials each; every resume is
   byte-identical to the uninterrupted run under gcc and MSVC.

Left as they are on purpose: the further look-ahead acquisitions
(GlobalMI, the SUR family) beyond EAVC and LocalMI; independent
multi-outcome models, which are K handles; variational inference, which
PCG makes unnecessary at session scale; and AEPsych's server, database
and plotting layer. A server exists there because the model is too slow
to run in the experiment process; these headers fit a frame, so the
experiment calls them directly, and a MATLAB or Python session has the
MEX or the binding. Persistence is the snapshot plus the caller's own
data file.

## References

- Bryan, B., et al. (2005). Active learning for identifying function
  threshold boundaries. NIPS.
- Cunningham, J. P., Shenoy, K. V., & Sahani, M. (2008). Fast Gaussian
  process methods for point process intensity estimation. ICML.
- Gardner, J. R., et al. (2018). GPyTorch: blackbox matrix-matrix Gaussian
  process inference with GPU acceleration. NeurIPS.
- Gotovos, A., et al. (2013). Active learning for level set estimation.
  IJCAI.
- Houlsby, N., et al. (2011). Bayesian active learning for classification
  and preference learning. arXiv:1112.5745.
- Kaernbach, C. (1991). Simple adaptive testing with the weighted up-down
  method. Perception & Psychophysics 49, 227-229.
- Keeley, S., et al. (2023). A semi-parametric model for decision making in
  high-dimensional sensory discrimination tasks. AAAI.
- Kesten, H. (1958). Accelerated stochastic approximation. Annals of
  Mathematical Statistics 29, 41-59.
- Klein, S. A. (2001). Measuring, estimating, and understanding the
  psychometric function: a commentary. Perception & Psychophysics 63,
  1421-1455.
- Kontsevich, L. L., & Tyler, C. W. (1999). Bayesian adaptive estimation
  of psychometric slope and threshold. Vision Research 39, 2729-2737.
- Lesmes, L. A., et al. (2010). Bayesian adaptive estimation of the
  contrast sensitivity function: the quick CSF method. Journal of Vision
  10(3):17.
- Letham, B., et al. (2022). Look-ahead acquisition functions for Bernoulli
  level set estimation. AISTATS.
- Levitt, H. (1971). Transformed up-down methods in psychoacoustics.
  JASA 49, 467-477.
- Nickisch, H., & Rasmussen, C. E. (2008). Approximations for binary
  Gaussian process classification. JMLR 9, 2035-2078.
- Owen, L., et al. (2021). Adaptive nonparametric psychophysics.
  arXiv:2104.09549.
- Prins, N. (2013). The psi-marginal adaptive method. Journal of Vision
  13(7):3.
- Rasmussen, C. E., & Williams, C. K. I. (2006). Gaussian Processes for
  Machine Learning. MIT Press. Chapters 3 and 5.
- Treutwein, B. (1995). Adaptive psychophysical procedures. Vision
  Research 35, 2503-2522.
- Watson, A. B. (2017). QUEST+: a general multidimensional Bayesian
  adaptive psychometric method. Journal of Vision 17(3):10.
- Watson, A. B., & Pelli, D. G. (1983). QUEST: a Bayesian adaptive
  psychometric method. Perception & Psychophysics 33, 113-120.
- Williams, C. K. I., & Barber, D. (1998). Bayesian classification with
  Gaussian processes. IEEE PAMI 20, 1342-1351.
