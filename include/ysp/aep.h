/* ysp/aep.h - v0.15.0 - public domain single-header Gaussian-process adaptive library
 *
 *   Nonparametric and semiparametric Bayesian adaptive psychophysics: a
 *   Gaussian-process model over a multidimensional stimulus space with
 *   binary, ordinal, categorical or continuous outcomes, and acquisition
 *   functions for threshold (level-set) estimation and for the whole
 *   psychometric field. The subset of AEPsych (Owen et al. 2021; Letham
 *   et al. 2022; Keeley et al. 2023) that fits in one C file with no
 *   dependencies: Laplace inference instead of variational, a candidate
 *   set instead of a gradient optimizer, the semiparametric model as a
 *   kernel, and the look-ahead acquisitions through a rank-one update.
 *
 *   Written in the single-header style of the stb / sokol libraries. Pure
 *   computation: no OS calls, no threads. One allocation at open, none
 *   after. Needs nothing but libm. Not a transport header, so it does not
 *   include ysp/rt.h, unless YAEP_ASYNC asks for the optional thread that
 *   runs the inference off the frame loop; see ASYNC.
 *
 *   Targets every platform the compiler does. C99 is the floor: it builds as
 *   C99, C11 and C++17, and in the C dialect MSVC compiles by default.
 *
 *   ---------------------------------------------------------------------
 *   USAGE
 *   ---------------------------------------------------------------------
 *   Do this:
 *
 *       #define YSP_AEP_IMPLEMENTATION
 *
 *   in *one* C or C++ file before including this header to create the
 *   implementation. Every other file just includes the header normally.
 *
 *   A detection threshold as a function of one context variable (say,
 *   contrast threshold across spatial frequency), estimated at 75%:
 *
 *       #define YSP_AEP_IMPLEMENTATION
 *       #include "ysp/aep.h"
 *
 *       yaep_desc d = { 0 };               // unset fields are 0: defaults
 *       d.n_dims        = 2;
 *       d.lo[0] = 0.0;  d.hi[0] = 1.5;     // log10 spatial frequency,
 *       d.lo[1] = -3.0; d.hi[1] = 0.0;     // log10 contrast (intensity)
 *       d.intensity_dim = 1;
 *       d.kernel        = YAEP_KERNEL_SEMIP; // linear in contrast
 *       d.fit           = true;            // fit the hyperparameters
 *       d.fit_every     = 10;
 *       d.acq           = YAEP_ACQ_EAVC;   // look-ahead level set
 *       d.target_p      = 0.75;
 *       d.n_init        = 10;              // Halton trials first
 *       d.stop_trials   = 150;
 *
 *       static yaep_gp g;
 *       if (!yaep_open(&g, &d)) { fputs(yaep_error(&g), stderr); return 1; }
 *
 *       while (!yaep_done(&g)) {
 *           double x[2];
 *           if (yaep_next(&g, x) < -1) { fputs(yaep_error(&g), stderr); break; }
 *           int seen = run_trial(x);                  // 1 = yes, 0 = no
 *           if (yaep_update(&g, x, seen) < 0) fputs(yaep_error(&g), stderr);
 *       }
 *       double ctx[1] = { 0.7 }, thr, lo, hi;
 *       yaep_threshold(&g, ctx, 0.75, &thr, &lo, &hi); // contrast at 0.7
 *       yaep_close(&g);
 *
 *   The same with a four-point confidence rating instead of yes/no is
 *   d.lik = YAEP_LIK_ORDINAL, d.n_outcomes = 4, d.target_outcome = 2
 *   ("rated 2 or above"), and yaep_update() gets the rating. A "which of
 *   three intervals" task is YAEP_LIK_CATEGORICAL with n_outcomes = 3;
 *   a matching task with a continuous setting is YAEP_LIK_GAUSSIAN and
 *   yaep_update_real().
 *
 *   ---------------------------------------------------------------------
 *   MODEL
 *   ---------------------------------------------------------------------
 *   A latent field f ~ GP(mean, kernel) over a box [lo, hi] of n_dims
 *   dimensions, observed through one of five likelihoods (desc.lik):
 *
 *     YAEP_LIK_BERNOULLI   p(y = 1 | f) = link(f), or with a floor and a
 *                           ceiling desc.guess + (1 - guess - lapse)
 *                           link(f). Detection, 2AFC,
 *                           yes/no. K = 2. AEPsych's default, and this
 *                           header's.
 *     YAEP_LIK_ORDINAL     K ordered categories: a confidence rating, a
 *                           Likert scale, "less / same / more".
 *                           p(y = k | f) = link(c_k - f) - link(c_{k-1} - f)
 *                           with K - 1 increasing cutpoints c_1..c_{K-1},
 *                           c_0 = -inf, c_K = +inf. One latent. The
 *                           cutpoints are hyperparameters, fixed in
 *                           desc.hyper.cutpoint or fit; c_1 is pinned to
 *                           0 when fitting, else the mean and the
 *                           cutpoints trade off. AEPsych's
 *                           OrdinalLikelihood.
 *     YAEP_LIK_CATEGORICAL K unordered categories: which of K intervals,
 *                           which of K named percepts, which direction.
 *                           K latent GPs sharing one kernel and
 *                           p(y = k | f) = softmax_k(f_1..f_K) (Rasmussen
 *                           & Williams 2006 sec. 3.5). Costs K times the
 *                           Bernoulli Laplace. Not in AEPsych.
 *     YAEP_LIK_GAUSSIAN    a continuous y: a matching setting, a
 *                           magnitude estimate, a log reaction time.
 *                           y = f + noise, noise sd a hyperparameter.
 *                           Exact Gaussian posterior, no Newton. AEPsych's
 *                           GaussianLikelihood.
 *     YAEP_LIK_PAIRWISE    comparisons: a trial shows two stimuli, y = 1
 *                           when the first was preferred, and
 *                           p = link(f(x1) - f(x2)) (Chu and Ghahramani
 *                           2005), f a latent utility. The likelihood sees
 *                           only the difference d = f(x1) - f(x2), which is
 *                           itself a GP with the four-term kernel
 *                           k(x1,x1') + k(x2,x2') - k(x1,x2') - k(x2,x1'), so
 *                           the inference is the Bernoulli Laplace on an N x N
 *                           matrix, exact Newton, and the utility is
 *                           predicted through the cross kernel
 *                           k(x, x1) - k(x, x2). The mean cancels out of every
 *                           difference, so it is fixed at 0 and not fitted,
 *                           and the utility is identified up to that constant.
 *                           yaep_next_pair() proposes a pair,
 *                           yaep_update_pair() records one, yaep_argmax()
 *                           reports the favorite, yaep_predict_f(.., 0, ..)
 *                           the utility and yaep_predict_pair() P(x1 is
 *                           preferred to x2). yaep_next(), yaep_update(),
 *                           yaep_predict_p() and yaep_threshold() have no
 *                           meaning for a comparison and refuse it, and so
 *                           does the async layer, which submits single
 *                           stimuli. Pair selection: BALD or BALV score the
 *                           comparison between the best stimulus so far and
 *                           each candidate, on d; THOMPSON takes the favorites
 *                           of two posterior draws of the utility; RANDOM and
 *                           the init phase take two Halton points. Measured
 *                           with examples/aep/pairwise.c (a utility peaked at
 *                           (0.3, 0.7), preference Phi(2 (u1 - u2)), 80
 *                           comparisons, 12 streams): BALD and Thompson put
 *                           yaep_argmax() within 0.036 of the peak, in 12 of
 *                           12 streams, and BALV, which maximizes a
 *                           comparison's uncertainty and so spreads its
 *                           pairs, within 0.138, in 3 of 12. 1.5 to 3 ms a
 *                           comparison. Not with the psychometric model, a
 *                           floor or ceiling, or the level-set acquisitions.
 *
 *   More than one outcome per trial (a detection and a confidence rating, a
 *   matching setting and a reaction time) is one handle per outcome: K
 *   independent GPs over the same box, each opened with its own likelihood,
 *   each updated with its own outcome of the shared stimulus, the proposal
 *   taken from whichever handle the design is about or chosen by the caller
 *   from their yaep_acq_score() values. There is no multi-output type:
 *   independent outputs share nothing a joint model would use, and correlated
 *   ones need a coregionalization kernel this header does not have.
 *
 *   The link (desc.link) is the probit by default, the logit on request;
 *   it applies to BERNOULLI and ORDINAL.
 *
 *   FLOOR AND CEILING (desc.guess, desc.lapse)
 *     A two-interval task cannot score below 0.5 and no observer is right
 *     every time, so the built-in links take a floor and a ceiling:
 *
 *       p(y = 1 | f) = guess + (1 - guess - lapse) link(f)
 *
 *     under BERNOULLI, and under ORDINAL the same squeeze on every
 *     cumulative probability P(y >= k), which moves `lapse` of the mass into
 *     the bottom category and `guess` into the top and leaves each category
 *     probability a difference of two squeezed terms. Both default to 0,
 *     both are FIXED (never fitted, they are task structure and not model
 *     slack), and yaep_open() rejects a nonzero value under CATEGORICAL or
 *     GAUSSIAN. Set desc.guess = 0.5 for a two-interval task and the model
 *     no longer has to bend its prior mean to fake the chance level; a lapse
 *     of 0.02 to 0.05 is the usual allowance for attention.
 *     Every level-set quantity follows: the target of the LSE, EAVC and
 *     LOCALMI family and of yaep_threshold() is this same p, so the latent
 *     level is f* = link^-1((target_p - guess) / (1 - guess - lapse)), and
 *     target_p has to lie inside (guess, 1 - lapse) or open() refuses it.
 *     Two costs. First, the log likelihood is no longer log-concave in f
 *     once either is nonzero, so the Laplace posterior is a weaker
 *     approximation: the Newton step clamps a negative Hessian entry to zero
 *     (the treatment R&W give the non-log-concave case) and the line search
 *     guards the step, but the posterior variance at a site the clamp
 *     touched is the prior's, and the approximation degrades as the lapse
 *     grows. Second, a two-interval trial carries less information than a
 *     yes/no trial, and an unregularized hyperparameter fit on a handful of
 *     them can settle on "the whole field is at chance", which is
 *     self-confirming: hyper.mean walks to its lower bound, the output scale
 *     to its lower, the log marginal likelihood sits at N log(guess), and a
 *     level-set acquisition has no gradient left to follow. The remedies are
 *     a longer init phase and a later first fit (n_init 16 and fit_every 20
 *     rather than 8 and 10 for a hundred-trial run), or fixing hyper.mean so
 *     the fit cannot go there.
 *
 *   THE PSYCHOMETRIC MODEL (desc.model = YAEP_MODEL_PSYCHOMETRIC)
 *     Keeley et al. 2023's semiparametric model in its own form, rather than
 *     as the kernel YAEP_KERNEL_SEMIP approximates it with. Two latent GPs
 *     over the context c (every dimension but intensity_dim): m(c), the
 *     threshold, in intensity units, and g(c), the log of the slope. At a
 *     stimulus (c, x) the latent is
 *
 *       f(c, x) = exp(g(c)) (x - m(c)),     p = link(f)
 *
 *     with the floor and ceiling above, or the ordinal cutpoints on f. Every
 *     slice along the intensity is a psychometric function, increasing by
 *     construction, whose location and steepness vary smoothly with the
 *     context. That is the shape a stationary kernel over the whole box
 *     cannot hold: the RBF latent needs one lengthscale for all of the
 *     intensity axis, so a rise of 7 dB in a 140 dB box comes out tens of dB
 *     wide, and on the audiometric observer the field error inside the
 *     transition band (true p between 0.05 and 0.95) is 0.22 for the RBF GP
 *     and for AEPsych alike. The numbers this model gets are under STATUS.
 *     Priors: m has an RBF-ARD kernel over c with mean hyper.mean (default
 *     the middle of the intensity axis) and output scale hyper.outputscale
 *     (default (span / 4)^2, span the intensity axis's length, i.e. a prior
 *     sd of a quarter of the axis); g has an RBF-ARD kernel with
 *     hyper.lengthscale_g, output scale hyper.outputscale_g (default 0.3) and
 *     mean hyper.mean_g (default log(4 / span): a rise whose probit sd is a
 *     quarter of the axis). With n_dims = 1 there is no context and each GP
 *     is one Gaussian variable, which makes the model a Bayesian parametric
 *     psychometric function.
 *     Inference is Laplace over the 2N latents (m_i, g_i), one pair per
 *     trial, with the prior block-diagonal in (K_m, K_g). The exact Hessian
 *     of log p(y | m, g) is indefinite at every trial with a nonzero residual
 *     (its second-derivative-of-f term has determinant -b^2 l'^2), so the
 *     step uses the Gauss-Newton part J' (-l'') J, J = (-b, f) the gradient
 *     of f, with -l'' clamped at zero as under a floor. That W is positive
 *     semidefinite and rank one per trial, W = U U' with U 2N x N, so every
 *     2N x 2N system collapses to N x N through B = I + U' K U: one Cholesky
 *     per step, the same size as the GP model's. The mode is exact (the
 *     iteration's fixed point is the stationary point of the log posterior)
 *     and W is the curvature the Gaussian takes, which is also what makes the
 *     step converge linearly rather than quadratically: about 10 steps from a
 *     warm start where the GP model takes 2 to 4, to a step of 1e-8 relative.
 *     Exact Newton steps near the mode were tried and measured, solved by
 *     conjugate gradients with the Gauss-Newton posterior as preconditioner:
 *     they halve the steps and the factorizations, but the dropped term is
 *     large enough that the preconditioned solve takes about 10 iterations,
 *     which at N of 50 to 150 costs what the saved factorizations did, and
 *     the whole session came out 1.7 times slower. Gauss-Newton stays.
 *     Prediction at a context is a bivariate Gaussian over (m, g). The
 *     target quantity at an intensity is its expectation over that: m given
 *     g is Gaussian and f is linear in m, so the m integral is the one-latent
 *     machinery (closed form for the probit Bernoulli case) and only g takes
 *     quadrature, YAEP_QUAD_N = 20 Gauss-Hermite nodes. The test measures
 *     that rule against 60 nodes and against a brute-force rule in both
 *     coordinates: E[q] under the probit agrees to 3e-8 and 2e-8; Var[q],
 *     and under the logit E[q] as well, to about 1e-4, which is the accuracy
 *     of the one-latent 20-node rule in f when the latent sd is several
 *     units and so is the GP model's too.
 *     The threshold at a context is the crossing m + f_t exp(-g), f_t the
 *     latent level of the target, and with (m, g) bivariate Gaussian its
 *     mean and variance are closed form (see ESTIMATES): no scan, no
 *     bisection, and never more than one crossing.
 *     Acquisitions: all six. LSE, BALV and BALD are per point, each computed
 *     by the same quadrature. LSE is the straddle on the probability scale,
 *     beta sd[q] - |E[q] - target_p|, since this latent is not Gaussian and
 *     has no one sd to straddle with. The look-ahead acquisitions work on the
 *     level-set indicator [p > target] = [m + f_t exp(-g) < x], whose
 *     probability is one Gaussian CDF per node of the g quadrature. With W
 *     frozen at the candidate's predictive mean, one more trial there is a
 *     Gauss-Newton site w J J' on that context's (m, g), so it updates every
 *     context's (m, g) posterior by RANK ONE, through
 *     v = Cov((m, g), f*) = Sigma_ij J, and the cross-covariance Sigma_ij
 *     comes from the same L^-1 U' k columns the prediction computes. LOCALMI
 *     is the self-update of the candidate's own context; EAVC is the expected
 *     absolute change in sum_i P(level_i) over the candidates. EAVC keeps
 *     2 M x N of those columns, and a candidate sharing a context with the
 *     one before it shares its update, so on a grid with the intensity
 *     fastest the M^2 pairs cost one context's dot products per column plus
 *     M^2 x outcomes x about 14 Gaussian CDFs (the outer nodes whose weight
 *     is above 1e-12); about 20 ms a trial on the audiometric grid, M = 231.
 *     Under refinement EAVC's point objective is the straddle, as under the
 *     GP model, and LOCALMI is its own.
 *     The hyperparameter gradient is analytic. The explicit terms are the GP
 *     model's with U' dK U inside the trace; the implicit term, through the
 *     mode, needs the TRUE negative Hessian A = K^-1 + W_true, and one adjoint
 *     solve A lambda = d(-1/2 log|B|)/dz serves every hyperparameter. A is
 *     symmetric and positive definite at a maximum, so the solve is
 *     conjugate gradients preconditioned by the Gauss-Newton posterior
 *     K - K U B^-1 U' K, which needs only the factor already in hand, and
 *     K^-1 of each iterate falls out of the same arithmetic, so no
 *     factorization of K and none of a 2N matrix; about 10 iterations of
 *     O(N^2). A breakdown (A not positive definite, so no strict maximum)
 *     falls back to central differences for that one evaluation. Checked
 *     against differences to 1e-6 or better (STATUS). Measured on the
 *     audiometric benchmark with fits every 20 trials, a whole session costs
 *     6 to 9 ms a trial against 3 to 4 ms without fits, so the fits cost 2.7
 *     to 5.7 ms a trial (the difference gradient cost 16 to 20), against 1.2
 *     for the RBF GP: the gap left is the linearly converging mode search,
 *     which every fit evaluation repeats.
 *     desc.refit_every is ignored (every update is an exact refit).
 *     Rejected at open: CATEGORICAL and GAUSSIAN (no psychometric function
 *     of the intensity to write) and YAEP_KERNEL_SEMIP (the model has its
 *     own GPs).
 *
 *   The header keeps a Gaussian
 *   approximation to the posterior of f given the trials so far, and the
 *   acquisition function picks the next stimulus from a candidate set to
 *   shrink the uncertainty that matters for the question asked. This is
 *   AEPsych's model with two substitutions:
 *
 *     - Inference is the LAPLACE approximation (Williams & Barber 1998;
 *       Rasmussen & Williams 2006 ch. 3): Newton's method to the mode of
 *       the latent posterior, a Gaussian around it. AEPsych uses
 *       variational inference through GPyTorch. Laplace is one Cholesky
 *       per Newton step on an N x N matrix, N being the trial count, and
 *       needs no optimizer. For a probit likelihood on a few hundred
 *       binary trials the two agree to within the noise of the trials
 *       themselves: fitted to the same trials, the two models' fields
 *       differ by 0.016 (STATUS). Laplace underestimates the variance where
 *       the posterior is skewed, that is, early and at the edges;
 *       docs/adapt.md has the caveats.
 *     - The next stimulus is chosen from a CANDIDATE SET, a grid or a
 *       Halton set over the box, not by optimizing the acquisition with
 *       gradients. Every candidate is scored, the best wins; a grid of a
 *       few hundred to a few thousand points is what AEPsych's optimizer
 *       starts from anyway.
 *
 *   KERNELS (desc.kernel)
 *     YAEP_KERNEL_RBF    squared exponential with one lengthscale per
 *                         dimension (ARD) and an output scale. AEPsych's
 *                         default. Smooth, no assumption of monotonicity.
 *                         k(x, x') = outputscale *
 *                                    exp(-1/2 sum_d ((x_d - x'_d) / l_d)^2),
 *                         so the output scale is a VARIANCE: the prior sd of
 *                         the latent is its square root.
 *     YAEP_KERNEL_SEMIP  the semiparametric model (Keeley et al. 2023):
 *                         f(c, x) = a(c) + b(c) * x, where x is the
 *                         intensity dimension (desc.intensity_dim) measured
 *                         from the MIDDLE of the box, c the
 *                         other dimensions, and a and b are independent
 *                         RBF GPs over c. Every slice along the intensity
 *                         is a straight line in latent space, i.e. a
 *                         cumulative Gaussian (probit) or logistic (logit)
 *                         psychometric function whose location and slope
 *                         vary smoothly with context. The kernel is
 *                         k_a(c, c') + x x' k_b(c, c'), so it is the same
 *                         Laplace machinery with a different covariance.
 *                         The x of that product is the intensity minus the
 *                         middle of the box, because the slope term's prior
 *                         variance is x^2 k_b(c, c) and the point where the
 *                         slope stops contributing has to be inside the
 *                         stimulus range, not wherever zero falls in the
 *                         caller's units. lengthscale[intensity_dim] is
 *                         unused here, and with n_dims = 1 there is no
 *                         context at all: the model is then plain probit or
 *                         logistic regression with a Gaussian prior on the
 *                         intercept and the slope.
 *                         THE SCALE OF THE INTERCEPT is the trap. a(c) has to
 *                         carry (box middle - threshold) / width of the
 *                         psychometric rise in latent units, so an observer
 *                         whose rise is narrow against the box needs a huge
 *                         intercept: on the audiometric observer of
 *                         examples/aep/audiometric.c, 140 dB of intensity and a
 *                         probit width of 0.5 dB put a(c) over tens of latent
 *                         units and its output scale over a thousand, where
 *                         the default ceiling is 10 and the weak prior of
 *                         HYPERPARAMETERS is centered on 1. Measured there
 *                         with the level-set acquisition, 20 replications of
 *                         150 trials: 8.97 dB of threshold error with the
 *                         prior, 7.07 dB without it, 6.10 dB without it and
 *                         with the two output-scale ceilings raised to 4000
 *                         and 100, against 1.98 dB for the plain RBF kernel on
 *                         the same observer. So the semiparametric kernel is
 *                         the wrong model for that combination, not a badly
 *                         fitted one, and the way to use it on a steep
 *                         observer is to scale the intensity axis so the
 *                         psychometric rise is of order 1 (which puts the
 *                         intercept in range), or to set hyper_max and
 *                         desc.no_hyper_prior for the scale the problem needs.
 *                         Keeley's slope is constrained positive; this
 *                         kernel's is not, and a run with too few trials at
 *                         one context can put a negative slope there. The
 *                         HYPERPARAMETERS and ESTIMATES sections say how
 *                         to see it. YAEP_MODEL_PSYCHOMETRIC (MODEL) is
 *                         the positive-slope form, with the threshold as a
 *                         GP of its own, so no intercept has to carry the
 *                         threshold's distance from the box middle. This
 *                         kernel is kept as it was, for reproducibility.
 *                         Under CATEGORICAL each latent gets its own a and
 *                         b.
 *   DIMENSION KINDS (desc.dim_kind, desc.dim_levels)
 *     Every dimension is YAEP_DIM_CONTINUOUS unless desc.dim_kind says
 *     otherwise, and the intensity dimension always is.
 *     YAEP_DIM_INTEGER     the integers in [lo, hi] (lo and hi must be
 *                           integers): a count of repetitions, a number of
 *                           tones. The kernel is the RBF term on the
 *                           coordinates rounded to the nearest integer, so a
 *                           prediction at 1.2 is the prediction at 1.
 *     YAEP_DIM_CATEGORICAL dim_levels unordered levels, coded 0 .. levels - 1
 *                           with the box [0, levels - 1]: a talker, a masker
 *                           type, an ear. Its kernel factor is
 *                           exp(-(1 - delta) / l), 1 for the same level and
 *                           exp(-1 / l) for any two different ones, so every
 *                           level borrows from every other through the one
 *                           fitted l: large l, the levels behave alike; small
 *                           l, each is its own function. No order is implied,
 *                           which is the difference from coding the levels as
 *                           a CONTINUOUS axis, where level 0 and level 2 are
 *                           further apart than 0 and 1 whatever the data say.
 *                           Its lengthscale's start, bounds and prior are
 *                           those of a span of 4: prior center and start 1
 *                           (two levels correlated e^-1 = 0.37), bounds 0.2
 *                           to 8 (0.007 to 0.88).
 *     The kinds multiply into both kernels (a categorical context of SEMIP's
 *     intercept and slope GPs, of the psychometric model's threshold and
 *     slope GPs) and into the analytic hyperparameter gradient. Candidates
 *     honor them: a product grid enumerates a CATEGORICAL dimension's levels
 *     (its grid[d] is 0 or the level count) and rounds an INTEGER one (grid[d]
 *     at most the number of integers); Halton points, the init phase and
 *     RANDOM draw integers and levels with equal shares; a caller's candidate
 *     set must already hold them. Refinement (CANDIDATES) scores every level
 *     of a CATEGORICAL dimension and every integer within one candidate
 *     spacing (at least the two neighbors) of an INTEGER one, instead of the
 *     golden section. yaep_update() stores the rounded coordinates, and
 *     yaep_threshold() rounds its context. Measured with the field of
 *     test_mixed in tests/adapt/aep_test.c (a 3-level context whose middle
 *     level is the far one, an integer context 0 to 4, threshold RMSE over the
 *     15 cells, LSE on a 21 x 3 x 5 grid with refine_steps 2, 20 streams):
 *
 *       trials                          45       80      120
 *       GP, kinds                     0.041    0.024    0.024
 *       GP, both contexts CONTINUOUS  0.066    0.047    0.035
 *       psychometric model, kinds     0.046    0.025    0.022
 *
 *     The kinds cost 0.6 to 3.0 ms a trial against 0.4 to 2.2 for the same
 *     sessions as CONTINUOUS: the per-dimension kernel term is a branch
 *     instead of a multiply. A description with every dimension CONTINUOUS
 *     takes the old code path and gives the old results to the bit.
 *
 *   MONOTONIC PROJECTION (desc.monotone_dims)
 *     Neither kernel knows that p rises with the intensity, and on a sparse
 *     or noisy run the GP model's posterior mean can fall along it; the
 *     threshold search then takes the first crossing and says there was more
 *     than one (yaep_threshold_multi_cross()). Bit d of
 *     desc.monotone_dims (increasing) replaces the posterior mean at x with
 *     its largest value over the points below x on the candidate line through
 *     x along dimension d (every masked dimension's line at once, their
 *     product, when several are set), x included. That is AEPsych's
 *     MonotonicProjectionGP. The line is the product grid's own when there is
 *     one, and otherwise as many evenly spaced points as M candidates would
 *     have along one side of a grid, 9 to 65. yaep_predict_p(),
 *     yaep_predict_p_many(), yaep_predict_p_var(), yaep_threshold() (and
 *     so desc.stop_threshold_sd) and the level-set acquisitions LSE, EAVC and
 *     LOCALMI read the projected mean; yaep_predict_f() and
 *     yaep_predict_f_many() still report the latent posterior, and BALV,
 *     BALD and the optimization acquisitions are unchanged.
 *     It is a projection of the MEAN, not a constraint on the posterior: the
 *     variance, the Laplace mode, the hyperparameter fit and the look-ahead
 *     covariance are what they would be without it. Two consequences the test
 *     measures: the projected mean never falls from one line point to the
 *     next, but between two line points it can fall by as much as the
 *     unprojected mean does inside one spacing; and E[p], which also depends
 *     on the variance, can fall wherever the variance changes, which it does
 *     near every trial.
 *     GP model only, one latent: not with YAEP_MODEL_PSYCHOMETRIC (monotone
 *     in the intensity by construction), CATEGORICAL or PAIRWISE, and not on a
 *     CATEGORICAL dimension. Cost: a projected prediction is a posterior mean,
 *     one kernel row and a dot product, at every line point below x, at most
 *     65 of them per masked dimension; on a product grid the acquisition
 *     projects all M candidates by a running maximum, in O(M).
 *     Measured with examples/aep/audiometric.c (intensity masked, every
 *     phenotype at beta = 2, 20 replications, the same seeds; pooled
 *     threshold error in dB and MAE(p), trials 25 / 50 / 100 / 150):
 *
 *       lse         9.43 / 4.24 / 2.47 / 2.05 dB   MAE(p) 0.0764 at 150
 *       lse-mono    7.81 / 3.90 / 2.20 / 1.79 dB          0.0731
 *       eavc       10.53 / 5.12 / 2.81 / 2.07 dB          0.0617
 *       eavc-mono  10.72 / 4.98 / 2.50 / 1.96 dB          0.0596
 *       bald       19.69 / 11.93 / 7.29 / 5.88 dB         0.0660
 *       bald-mono  the same thresholds                    0.0636
 *
 *     The projection helps LSE most (13% less threshold error at trial 150,
 *     17% at trial 25), because its straddle reads the mean directly and a
 *     dip in the mean is a false level-set crossing it will sample. EAVC gains
 *     3 to 11% from trial 50 on and nothing before. BALD's proposals do not
 *     read the mean, so its thresholds are unchanged and only the reported
 *     field improves, by up to 4%. The price is a few more fits at the
 *     output-scale ceiling (2 and 5 runs of 80 for lse-mono and eavc-mono
 *     against 0 and 1) and, under EAVC, about 8% more time a trial.
 *
 *   HYPERPARAMETERS (desc.hyper, desc.fit)
 *     lengthscale[d], outputscale and mean for RBF; a second lengthscale
 *     set and outputscale for the slope GP under SEMIP; cutpoint[k] under
 *     ORDINAL; noise_sd under GAUSSIAN. Under YAEP_MODEL_PSYCHOMETRIC,
 *     lengthscale, outputscale and mean are the threshold GP m's (the output
 *     scale in squared intensity units, the mean in intensity units) and
 *     lengthscale_g, outputscale_g and mean_g the log-slope GP g's; MODEL has
 *     their defaults, and lengthscale[intensity_dim] is unused.
 *     A NONZERO field is fixed at that
 *     value. A zero field takes a default and is the set yaep_fit() fits:
 *     lengthscale (hi - lo) / 4, outputscale 1, mean 0, cutpoints 0, 1, 2,
 *     ... (c_1 is pinned to 0 while they are fitted, so the mean stays
 *     identifiable), noise_sd 0.1. Two consequences of "zero means fit":
 *     the mean cannot be pinned to exactly 0 while it is being fitted (set
 *     it to 1e-12, or leave desc.fit off), and setting cutpoint[0] fixes
 *     the whole cutpoint vector. A fixed mean is how the prior is centered
 *     somewhere other than p = 0.5: link^-1(p0), e.g. -1.28 for p0 = 0.1 in
 *     a yes/no task with rare false alarms.
 *     Fitting maximizes the log marginal likelihood plus the weak priors
 *     below: the Laplace
 *     approximation to it (R&W eq. 3.32) for the discrete likelihoods,
 *     the exact one (R&W eq. 2.30) for GAUSSIAN. The GRADIENT is analytic
 *     (R&W eqs. 5.22 to 5.24 for Laplace, including the implicit term
 *     through the mode; eq. 5.9 for Gaussian), so a fit is gradient ascent
 *     with a backtracking line search, bounded by [hyper_min, hyper_max],
 *     from the values in force. Lengthscales, output scales, cutpoint gaps
 *     and the noise are fitted as logarithms, the mean as itself. Bounds
 *     default to 0.05 (hi - lo) to 2 (hi - lo) for a lengthscale, 0.1 to 10
 *     for an output scale, -5 to +5 for the mean, 0.01 to 10 for a cutpoint
 *     gap and 1e-3 to 10 for the noise sd; a zero bound field means the
 *     default, so a bound of exactly zero cannot be asked for.
 *     One objective-plus-gradient evaluation is one Laplace refit plus
 *     O(N^2) per hyperparameter, and a fit stops after desc.fit_max_evals of
 *     them (0 = 40), when no gradient component exceeds desc.fit_tol (0 =
 *     1e-8, in objective units per unit of the fitted coordinate, which is
 *     the logarithm for a scale), or when no uphill step is left.
 *     yaep_fit() returns 1 when it stopped for one of the last two reasons
 *     and 0 when the budget ran out (or a guard below undid it), and
 *     yaep_fit_delta() is the objective's change. The budget is small on
 *     purpose, because a scheduled fit runs inside the trial loop, and on a
 *     large data set it does not converge: on Letham et al.'s 6-D contrast
 *     sensitivity data (tests/compare/compare_gp_csfdata.py, 500-trial
 *     subsets) one yaep_fit() never did, and it took about 3 repeated
 *     fits for the GP model and 7 for the psychometric model, whose
 *     lengthscales drift along a flat ridge, before the log marginal moved by
 *     less than 1e-3 nats. For an analysis rather than a session, call
 *     yaep_fit() until it returns 1 or yaep_fit_delta() is below the
 *     change you care about, or raise desc.fit_max_evals. A yaep_fit() that
 *     follows one the budget stopped keeps that fit's step length instead of
 *     starting again at a quarter of the gradient, which is what lets a
 *     repeated small budget make progress: in the test, a 4-evaluation fit
 *     repeated reaches the hyperparameters one 48-evaluation fit reaches, where
 *     restarting the step left it stuck after three rounds. A scheduled fit
 *     always starts afresh, as before. The step is
 *     monotone: a fit never leaves the
 *     model with a lower objective than it started with, and since v0.5.0
 *     that is checked rather than assumed: yaep_fit() and a scheduled fit
 *     compare the objective after the fit with the one before and undo the
 *     fit if it is worse or not finite, and a restore inside the ascent that
 *     does not come back to the value its point was accepted at is refitted
 *     from the prior mean. The mode search enforces its own half: the mode
 *     maximizes Psi, so a result that is not finite or scores below Psi at
 *     the prior mean is not the mode, and the search restarts from the prior
 *     mean, and returns YAEP_ERR_NUMERIC if that fails too. Both were missing
 *     until the cross-method comparison found a psychometric-model fit that
 *     restored its accepted point from a rejected point's mode, where the
 *     Gauss-Newton search ran into its step cap on a plateau of saturated
 *     links and reported success with a log marginal likelihood of -1e49
 *     (tests/adapt/aep_test.c replays the recorded trials).
 *     CATEGORICAL is the exception: its gradient is central differences on
 *     the same objective (2 P + 1 refits per gradient), because the
 *     implicit term of eq. 5.23 needs third derivatives of a coupled block
 *     Hessian. It is correct but slow, and it is the one part of the fit
 *     that is not analytic.
 *
 *     WEAK PRIORS (desc.priors, desc.no_hyper_prior). Every prior parameter
 *     is a field of desc.priors, a yaep_prior {center, sd, ceiling} per
 *     hyperparameter; zero means the default in this table, a negative sd
 *     turns that one prior off, desc.no_hyper_prior turns them all off, and
 *     yaep_get_priors() reports what is in force with the defaults filled
 *     in. The defaults, and where each was measured:
 *
 *       lengthscale    log-normal, center 0.25 (hi - lo) of its dimension,
 *                      sd 1 in the log. The 1-D and audiometric observers
 *                      of v0.1 (below).
 *       outputscale    log-normal, center 1, sd 0.7; under PSYCHOMETRIC the
 *                      threshold GP's, center (span / 4)^2. v0.1 below, and
 *                      the center-at-1 comparison against a box on [1, 4].
 *       outputscale_b  SEMIP's slope GP, as outputscale. v0.1.
 *       outputscale_g  PSYCHOMETRIC's log-slope GP, center 0.3, sd 0.7. The
 *                      sensory tail of v0.4.0 (below).
 *       mean           the GP model's mean on a discrete likelihood, normal,
 *                      center 0, sd 0.7. The novel discrimination function
 *                      of v0.5.0 (below), and a floor that swallows the
 *                      mean: with guess = 0.5, a first fit on data at the
 *                      floor rate (5 yes of 10 on the 6-D contrast
 *                      sensitivity problem, csf6 replication 4 of
 *                      tests/compare/methods_compare.py) has its likelihood
 *                      maximum at p = 0.5 everywhere, mean at its lower
 *                      bound, where the gradient in the mean vanishes;
 *                      v0.4.1 stayed there for 300 trials, log marginal
 *                      300 ln 0.5. With the prior the mean is -0.52 at
 *                      trial 10 and p spans 0.65 to 0.69; the test replays
 *                      those trials with and without it. Starting the mean
 *                      from the observed yes rate would not help: a rate at
 *                      the floor maps to a latent of minus infinity.
 *       mean_g         PSYCHOMETRIC's mean log-slope, normal in the log,
 *                      center a rise of 0.25 of the intensity axis, sd 1,
 *                      ceiling a rise of 1/256 of it. The runaway of v0.3.0
 *                      and the slope study of v0.4.1 (below).
 *       noise_sd       GAUSSIAN's noise, off unless center and sd are set.
 *
 *     The rise fractions of mean_g are the axis span over the slope's
 *     reciprocal: a center of 0.1 is a prior slope of 10 / span. Changing any
 *     of these changes a fit, so the numbers in this manual hold at the
 *     defaults only.
 *     AEPsych puts gamma priors on the
 *     lengthscales and optimizes the variational ELBO with autodiff. This is
 *     the same gradient by hand, with priors of the same purpose:
 *     log-normal on every lengthscale, centered on the default (hi - lo) / 4
 *     with an sd of 1 in logarithms, and log-normal on the output scales,
 *     centered at 1 with an sd of 0.7. Since v0.5.0 the GP model's mean has
 *     one too, on the discrete likelihoods: normal, centered at 0 (p = 0.5,
 *     or the middle of the floor and ceiling), sd 0.7. Nothing on GAUSSIAN's
 *     mean, which is in the caller's units, or on the cutpoints.
 *     The mean is where the field goes far from every trial, and a run whose
 *     responses are mostly one way sent it to its bound of 5 with nothing to
 *     stop it: on AEPsych's novel discrimination function (p has a floor of
 *     0.5 at the bottom edge and rises steeply, so most responses are yes) the
 *     model then called the whole box above the 0.75 level and most columns
 *     lost their crossing. Measured with 100 replications of 150 EAVC trials
 *     (the sd, then columns of 30 with no crossing, runs with more than half
 *     missing, threshold error, fitted mean): none, 21.9, 81, 0.081, 3.33;
 *     2, 19.7, 70, 0.083, 2.51; 1, 5.9, 17, 0.062, 1.20; 0.7, 2.7, 7, 0.054,
 *     0.63; 0.5, 2.9, 5, 0.052, 0.33. Under LSE the same five give 3.0, 1.4
 *     and 0.3 to 0.7 columns. 0.7 is the default, and on
 *     tests/compare/methods_compare.py's eleven 2-D problems (the eight
 *     audiograms, novel detection and discrimination, the CSF), 20
 *     replications each, it changes no GP threshold error by more than the
 *     spread between replications except novel discrimination under EAVC,
 *     from 21.9 columns without a crossing to 1.2, and the CSF, from 1.5 to
 *     1.9 such columns to 0.1 to 0.9.
 *     Under YAEP_MODEL_PSYCHOMETRIC the threshold GP's output-scale prior is
 *     centered on its default, (span / 4)^2, and the mean log-slope mean_g has
 *     a normal prior of its own, centered on its default log(4 / span) with an
 *     sd of 1. That one is not optional in practice: separable early
 *     responses favor an infinitely steep psychometric function exactly as
 *     they favor an infinite output scale, and a step is worse than useless
 *     to this model, because every trial then sits in the flat tail of the
 *     link, adds no curvature, and leaves the Laplace posterior of the
 *     threshold where it was, so a level-set acquisition asks the same
 *     question forever. Without it, on a 2-D observer with a 7 dB rise over
 *     a 100 dB axis, mean_g ran to its ceiling at trial 5 and the log
 *     marginal likelihood to 0.00; with an sd of 1.5 two LSE runs in five
 *     still ran away (threshold error 3.3 dB on average); with 1.0 or 0.5 none
 *     did (0.9 and 1.1 dB under LSE, 1.5 and 0.8 under BALV, five streams
 *     each), and 1.0 leaves the data more room to move the slope. mean_g is
 *     bounded to [log(0.25 / span), log(256 / span)] and mean to the
 *     intensity axis.
 *     The log-slope GP's output-scale prior is centered at 0.3, not 1 (sd 0.7
 *     in the logarithm, as for the others, default 0.3, lower bound 0.01).
 *     In psychophysics a slope varies across a context by tens of percent,
 *     not by factors of four, and a slope GP free to vary that much has a way
 *     out that the threshold GP does not: where the threshold climbs fast,
 *     it can flatten the rise instead of following the climb, and the 75%
 *     point then lands tens of dB off. That was the 7 to 12 dB tail on the
 *     audiometric sensory phenotype. Measured with examples/aep/audiometric.c,
 *     20 replications, trial 150, threshold error in dB (worst replication)
 *     and band MAE(p), for the center at 1, 0.5 and 0.3:
 *
 *                          1                  0.5                0.3
 *     sensory 2   lse   3.10 (7.7) .244   1.87 (3.8) .198   1.83 (2.8) .189
 *                 balv  2.05 (11.6) .174  2.81 (26.4) .186  1.11 (2.6) .152
 *     sensory .5  lse   3.39 (3.6) .323   3.11 (3.9) .230   2.76 (3.4) .289
 *                 balv  2.03 (2.7) .260   2.15 (3.0) .250   3.26 (5.6) .292
 *     metabolic 2 lse   1.40 (2.1) .159   1.55 (2.9) .172   1.42 (2.0) .160
 *                 balv  1.58 (3.1) .180   1.29 (1.9) .148   1.31 (2.1) .154
 *     metabolic .5 lse  1.88 (3.1) .300   1.51 (1.8) .292   1.55 (1.8) .302
 *                 balv  1.39 (1.7) .316   1.36 (1.7) .326   1.42 (2.0) .318
 *
 *     The threshold GP's lengthscale ended over 3 octaves in 0 to 5 runs of 20
 *     on sensory at every center and in 17 to 20 of 20 on metabolic.
 *     So 0.3 removes the tail (worst replication 5.6 dB, from 11.6) and costs
 *     metabolic nothing outside its interval; it is also better on the 2-D
 *     observer of the test (LSE 0.98 to 0.72 dB, BALV 1.07 to 0.75, 20
 *     streams) and within noise on the 1-D observers. The cost is sensory at
 *     beta 0.5 under BALV, 2.03 to 3.26 dB. 0.5 is worse than either: its
 *     one bad replication is 26 dB. The lengthscale count shows that a long
 *     threshold lengthscale is NOT the mechanism, contrary to a diagnosis
 *     from one dumped replication: it is the rule on metabolic, whose
 *     threshold really is smooth, and rare on sensory at every center.
 *
 *     THE FITTED MEAN SLOPE IS LOW, AND ITS PRIOR STAYS. On the audiometric
 *     observer exp(mean_g) comes out at about half the true slope (0.24 to
 *     0.30 per dB against 0.5 at beta 2), and it is the mean_g prior that puts
 *     it there, not the Laplace approximation. Measured on one metabolic
 *     beta 2 replication, with every other hyperparameter at its fitted
 *     value: the Laplace log marginal likelihood alone peaks at a slope of
 *     0.51 per dB, the truth, and so does the same marginal with the true
 *     Hessian in place of the Gauss-Newton one (they differ by 0.1 nats at
 *     the peak and at most 1.1 on the shallow side); the prior costs 1.9
 *     nats between 0.24 and 0.51 per dB, which moves the objective's maximum
 *     to 0.24, the fitted value, where the analytic gradient is -2e-3.
 *     Tightening the mode to 1e-13 moves the fitted mean_g by 4e-4.
 *     Loosening the prior was measured, and it is the wrong fix. 20
 *     replications on each of metabolic and sensory at beta 2 and 0.5, trial
 *     150, pooled over the four cells; the slope ratio is exp(mean_g) over the
 *     true slope, the band is MAE(p) where the true p is in [0.05, 0.95] on a
 *     30 x 300 grid, and each threshold is the mean error in dB (worst
 *     replication):
 *
 *     center  sd   slope ratio     LSE          BALV         EAVC      band
 *                  lse balv eavc
 *     1/4     1    .44 .54 .47   1.94 (3.6)   1.77 (5.9)   1.40 (4.3)   .22
 *     1/4     1.5  .66 .79 .73   3.78 (10.3)  1.91 (6.4)   2.43 (7.3)   .23
 *     1/4     2    .81 .92 .83   6.75 (9.7)   1.90 (5.4)   2.22 (8.2)   .23
 *     1/10    1    .61 .70 .64   2.11 (7.0)   1.67 (3.9)   2.00 (6.5)   .23
 *     1/10    1.5  .94 .86 .86   5.23 (10.3)  1.97 (4.5)   1.93 (5.1)   .23
 *     1/10    2    .89 .88 .98   3.88 (11.4)  2.02 (6.1)   2.45 (6.3)   .23
 *     1/20    1    .88 .82 .75   4.42 (11.0)  1.95 (4.6)   1.95 (5.4)   .23
 *     1/20    1.5  .98 .90 .81   4.70 (13.6)  2.26 (4.8)   2.08 (5.9)   .24
 *     1/20    2   1.01 .93 .97   5.82 (17.7)  1.71 (4.3)   2.60 (9.7)   .24
 *
 *     (The center is the rise the prior expects, as a fraction of the
 *     intensity axis; 1/4 is the default.) Every setting that lifts the
 *     slope ratio above 0.6 reopens the tail under LSE, worst replications
 *     of 7 to 18 dB; BALV is indifferent to the prior; and the band error
 *     does not move at all. The 2-D observer of the test agrees: at 1/4, sd 1
 *     it gets 0.72, 0.75 and 0.60 dB under LSE, BALV and EAVC with a band of
 *     0.07 to 0.09, and every looser setting makes LSE 1.9 to 3.0 dB with a
 *     band of 0.14 to 0.20, while the slope under LSE and EAVC OVERSHOOTS the
 *     truth (0.5 to 1.0 per dB against 0.37). The mechanism is the one the
 *     prior was introduced for: a level-set acquisition puts its trials at
 *     the threshold, where the slope is poorly identified, and a fit with room
 *     to steepen does, into the flat tail where trials stop informing the
 *     threshold. BALV, which spreads its trials, recovers the slope at any
 *     setting (0.33 to 0.43 per dB on the 2-D observer against 0.37). The
 *     early runaway itself stayed rare everywhere: at most 5 runs of 20 had
 *     mean_g at its ceiling or the log marginal above -2 by trial 25, and
 *     then only at 1/20 with sd 2.
 *     Nor is the slope what holds the band error. A model with the exact
 *     slope and a threshold off by the 1.1 to 1.3 dB this one is off by
 *     would score a band MAE of 0.15 to 0.17 at beta 2, which is what it
 *     scores; at beta 0.5 an exact slope with a 1.4 dB threshold error would
 *     score 0.48, and this model scores 0.26 to 0.31, because the posterior
 *     mean of p averages over the threshold's uncertainty and is correctly
 *     shallower than any one psychometric function. Per replication the band
 *     error tracks the threshold error (correlation 0.46 to 0.93). The band is
 *     a threshold-location metric at these widths, and the way to lower it is
 *     a better threshold. desc.priors.mean_g sets the center, the sd and the
 *     ceiling for that kind of measurement, and a desc.hyper.mean_g fixed at
 *     the true slope's logarithm is the way to use a slope known in
 *     advance.
 *     They are what stops the fit from interpolating its own trials. Type-II
 *     maximum likelihood on a run whose responses nearly separate keeps
 *     improving as the output scale grows and the lengthscale shrinks, so it
 *     ends at whatever bound it hits; the posterior is then diffuse or
 *     interpolating, and a level-set acquisition follows the variance to the
 *     edges of the box instead of the threshold. The likelihood is flat near
 *     that bound, so a prior this weak moves the answer a long way there and
 *     hardly at all once the data have something to say.
 *     What it bought, measured by examples/aep/audiometric.c on the metabolic
 *     phenotype at beta = 2 over 20 replications of 150 trials with the
 *     level-set acquisition (the same seeds, priors off then on): the
 *     threshold error at trial 150 fell from 3.14 +- 0.30 dB to
 *     1.98 +- 0.32 dB and the surface error from 0.115 to 0.077, the output
 *     scale stopped sitting on its upper bound (20 runs out of 20 before,
 *     none after), and the Laplace log marginal likelihood went from -20.6 to
 *     -58.7 nats, where 150 binary trials at a 75% level cannot honestly beat
 *     about -84. The early surface error is the one thing that got worse
 *     (0.153 to 0.256 at trial 25): a conservative fit starts further from
 *     the truth and ends closer. On the 1-D observer of the STATUS block the
 *     priors cut the mean absolute threshold error of LSE from 0.050 to 0.011
 *     and of BALD from 0.043 to 0.037 over eight streams.
 *     desc.no_hyper_prior turns them off and fits type-II maximum likelihood,
 *     which is what the numbers above call the "before".
 *
 *     WHERE THE OUTPUT-SCALE PRIOR IS CENTERED, and why it stays at 1. A
 *     same-data comparison with AEPsych showed its model ahead early (field
 *     MAE(p) 0.144 against 0.233 at trial 25) and level late, and AEPsych puts
 *     a smoothed box on [1, 4] on the output scale. Three priors were run on
 *     the same seeds: the log-normal centered at 1 (this header's), the same
 *     centered at 3, and a smoothed box on [1, 4] (flat inside, Gaussian tails
 *     of sd 0.05 in the logarithm). First examples/aep/audiometric.c, metabolic
 *     phenotype, beta = 2, 20 replications; each cell is field MAE(p) /
 *     threshold error in dB / mean fitted output scale:
 *
 *       trial              25              50             100             150
 *       LSE  center 1  .256 6.8 1.3  .163 3.7 2.1  .120 2.2 3.1  .077 2.0 6.4
 *            center 3  .165 8.6 3.8  .122 3.4 5.7  .078 2.3 9.8  .070 2.4 10.
 *            box 1-4   .177 5.7 4.0  .144 3.1 4.0  .111 1.9 4.0  .088 1.7 4.0
 *       EAVC center 1  .222 7.4 1.6  .138 4.9 2.5  .081 2.6 4.4  .058 2.0 7.0
 *            center 3  .170 7.7 4.5  .109 4.1 6.5  .075 2.9 9.2  .065 2.6 10.
 *            box 1-4   .175 9.5 4.0  .121 4.3 4.0  .084 2.7 4.0  .069 2.0 4.0
 *
 *     then the 1-D observer of the STATUS block, 20 streams of 150 trials,
 *     field MAE(p) / threshold error / output scale:
 *
 *       trial                  25                  150
 *       LSE  center 1   .120 .035 1.4    .058 .014 1.9
 *            center 3   .086 .040 3.7    .055 .016 4.2
 *            box 1-4    .087 .035 3.8    .043 .014 4.0
 *       EAVC center 1   .117 .033 1.4    .069 .010 1.9
 *            center 3   .088 .028 3.6    .050 .013 4.1
 *            box 1-4    .086 .023 3.7    .049 .013 4.0
 *
 *     Early, the lead is real: at trial 25 either alternative cuts the field
 *     error by about a third on both observers. Late, the three split.
 *     Centered at 3, the prior lets the output scale run to its upper bound
 *     of 10 (39 of 40 audiometric runs end on it, against 1 of 40 centered at
 *     1) and the late threshold is the worst of the three: that is the
 *     runaway the prior exists to stop. The box is not really a prior on
 *     these data: every fit on both observers ends at 4.00 to 4.06, its upper
 *     edge, so it acts as an output scale fixed at 4. Fixing it at 4 outright
 *     (desc.hyper.outputscale = 4) reproduces the box's field errors to within
 *     0.01 at every trial count on the audiometric observer, but not its late
 *     LSE threshold (2.00 against the box's 1.66 dB, both intervals about
 *     +- 0.3), which says that one number is seed noise. What the box
 *     reliably does is trade a better early field (and a better late field on
 *     the steep 1-D observer) for a worse late field on the audiometric one
 *     (0.088 against 0.077 for LSE, 0.069 against 0.058 for EAVC), where the
 *     data want an output scale of 6 to 7 by trial 150 and the box will not
 *     let them. Late accuracy is the tie-breaker, and it splits between the
 *     two observers, so it does not justify a change: the default stays
 *     centered at 1, where the rest of the manual's numbers were measured.
 *     A session that cares more about its early
 *     estimate can have the box's behavior by fixing desc.hyper.outputscale
 *     at 4. Bounds of [1, 4] on the fitted scale do not do it: the log-normal
 *     keeps the fit on the lower edge early (1.3 at trial 25, measured) and
 *     only the ceiling is ever reached.
 *     The fit therefore maximizes a posterior, not a likelihood.
 *     yaep_log_marginal() still reports the likelihood alone, and the
 *     symptom of a fit that ran away anyway is a log marginal much closer
 *     to zero than the responses allow: a hundred binary trials at a 75%
 *     level cannot be explained better than about -56 nats, so a fit
 *     reporting -5 has interpolated, and its threshold band will be wide and
 *     its threshold estimate unreliable. What is left to do about that is to
 *     raise hyper_min.lengthscale, lower hyper_max.outputscale, or fix the
 *     mean.
 *     A lengthscale at its UPPER bound, on the other hand, is usually not a
 *     complaint: a field that really is flat along a dimension wants an
 *     infinite lengthscale there, and the bound is the only thing stopping
 *     it. On the audiometric observer's flat phenotype the frequency
 *     lengthscale sat on its ceiling in 15 to 20 runs out of 20 with no cost
 *     to either metric. Read it as "this dimension carries no structure",
 *     and only worry when the field plainly does vary along it.
 *     Fits run every desc.fit_every trials once the init phase is over and
 *     on yaep_fit(). Log yaep_get_hyper() with your data: a lengthscale
 *     that hits its bound is a warning, a slope output scale that hits
 *     its upper bound under SEMIP is the negative-slope case above.
 *     A whole fit is one call and tens of refits, which is more than an
 *     inter-trial interval holds. yaep_fit_step() is the same ascent one
 *     evaluation at a time: it returns 1 while another step would help, 0
 *     when it is done, and leaves the best hyperparameters found so far in
 *     force every time it returns, so the caller can spend one step per
 *     trial and read estimates in between. Recording a trial throws the
 *     stepping state away, since the data it was fitting have changed; a
 *     caller that wants to interleave fitting with trials should run the
 *     steps to 0 in the gaps of one trial, or accept that a long fit
 *     restarts. yaep_fit() is a loop over yaep_fit_step().
 *
 *   ACQUISITION (desc.acq)
 *     The first desc.n_init trials are a Halton sequence over the box
 *     (AEPsych's Sobol phase; Halton because it is ten lines and
 *     deterministic). After that the candidates are scored and the best
 *     wins. The level-set family (LSE, EAVC, LOCALMI) needs a TARGET, and
 *     yaep_open() rejects a desc without one: for BERNOULLI the level
 *     P(y = 1) = target_p; for ORDINAL P(y >= target_outcome) = target_p;
 *     for CATEGORICAL P(y = target_outcome) = target_p; for GAUSSIAN
 *     E[y] = target_value. f* below is the latent value that meets it, the
 *     level of the latent field itself and not of its posterior average.
 *     Note that YAEP_ACQ_LSE is the default acq, so a zeroed desc needs a
 *     target_p even when the level set is not what is wanted; BALV, BALD
 *     and RANDOM need none.
 *
 *     YAEP_ACQ_LSE      level-set straddle (Bryan et al. 2005; Gotovos
 *                        et al. 2013; AEPsych "LevelSetEstimation"):
 *                        beta * sd_f(x) - |mean_f(x) - f*|. Cheap, myopic,
 *                        and drawn to the edges of the box. beta =
 *                        desc.acq_beta, default 1.96 (AEPsych's 3.84 is on
 *                        the variance and is the same number).
 *     YAEP_ACQ_EAVC     expected absolute volume change of the level set
 *                        (Letham et al. 2022; AEPsych "EAVC"): the
 *                        candidate whose outcome is expected to move the
 *                        most of the box across the target. The one
 *                        AEPsych's authors recommend for thresholds, and
 *                        this header's recommendation. Needs the LOOK-AHEAD
 *                        below. The volume is sum over candidates of
 *                        P(f(x) > f*), the expected NUMBER of candidates in
 *                        the level set, so the score is continuous in every
 *                        candidate's mean and variance; the score of a
 *                        candidate is E_y |volume - volume after y|.
 *     YAEP_ACQ_LOCALMI  mutual information between the outcome at x and
 *                        the level-set membership of x, the Bernoulli
 *                        1{f(x) > f*} (Letham et al. 2022; AEPsych
 *                        "LocalMI"). Look-ahead, but only at the candidate
 *                        itself, so it needs no M x M covariance and costs
 *                        one quadrature per candidate.
 *     YAEP_ACQ_BALV     posterior variance of the target probability, or
 *                        of E[y] under GAUSSIAN (AEPsych
 *                        "MCPosteriorVariance"): global estimation of the
 *                        whole field.
 *     YAEP_ACQ_BALD     mutual information between the outcome and f
 *                        (Houlsby et al. 2011; AEPsych
 *                        "BernoulliMCMutualInformation"): global, and less
 *                        drawn to the edges than BALV. Defined for every
 *                        likelihood.
 *     YAEP_ACQ_RANDOM   Halton for every trial, or draws from desc.rng
 *                        when it is set. A baseline for a simulation, or a
 *                        space-filling schedule you want reproducible. The
 *                        point is not a candidate (yaep_next() returns -1);
 *                        under yaep_next_subset() it is the subset
 *                        candidate nearest the drawn point.
 *
 *     OPTIMIZATION: "find the stimulus the observer rates highest" rather
 *     than a level set. q is the target quantity (P(y = 1), P(y >= k*),
 *     one-against-the-rest P(y = k*), or E[y] under GAUSSIAN), maximized, or
 *     minimized under desc.minimize; target_p is not needed.
 *     YAEP_ACQ_UCB      E[q] + acq_beta sd[q] (1.96 by default).
 *     YAEP_ACQ_EI       E[max(q - best, 0)], the expected improvement over
 *                        the best posterior mean of q at a stimulus already
 *                        tried (the posterior mean, since a binary outcome is
 *                        not the quantity being optimized). Closed form under
 *                        GAUSSIAN, quadrature over f otherwise.
 *     YAEP_ACQ_THOMPSON one joint draw of the latent at every candidate from
 *                        the M x M posterior covariance the look-ahead builds,
 *                        and the argmax of the draw's q. Needs desc.rng, for
 *                        the normal variates; GP model only; never refined,
 *                        since a draw has no value between candidates. Under
 *                        CATEGORICAL only the target class is drawn.
 *     yaep_argmax() then reports the stimulus with the best posterior mean
 *     of q. The repeat guard below does not apply to these three (returning
 *     to the optimum is the point); the flat test does. Measured with
 *     examples/aep/optimize.c on a 2-D bump, 80 trials, 12 streams: under
 *     GAUSSIAN all three put the argmax within 0.02 of the maximum by trial
 *     20 and within 0.01 by trial 50 (Thompson best, 0.006 at trial 80), in
 *     12 of 12 streams; under BERNOULLI, with the bump covering a tenth of
 *     the box and p = 0.02 outside it, UCB finds it in 8 of 12, EI in 7 and
 *     Thompson in 9. A stream that misses is one whose first trials all
 *     answer 0: the model then sees a flat field, UCB spends its trials at
 *     the box corners, where the latent sd is largest and a tail observation
 *     shrinks it slowly, and EI has nothing to improve on; a longer init
 *     phase is the remedy. Cost per trial at N = 80, M = 441: UCB and EI 1 to
 *     1.7 ms, Thompson 10 to 11 (the M x M covariance and its Cholesky).
 *
 *     Expectations over one latent are Gauss-Hermite quadrature with
 *     YAEP_QUAD_N (20) nodes. Under CATEGORICAL the latents are coupled
 *     by the softmax, and BALV, BALD and the look-aheads score each class
 *     latent one-against-the-rest (its mean minus the log-sum-exp of the
 *     other class means, through a logistic link) and sum; that is an
 *     approximation, and the manual says so here so the comparison against
 *     a Monte Carlo score (which desc.rng makes possible, see
 *     docs/adapt.md) is not a surprise. The look-ahead covariance under
 *     CATEGORICAL drops the between-class term of R&W eq. 3.41 as well.
 *     Ties break toward the lowest candidate index, unless desc.rng is set,
 *     in which case a tie is a draw. Two scores tie when they are within
 *     1e-12 of the best, relative.
 *     TWO DEGENERACIES, and the guard against them. A posterior so diffuse
 *     that every candidate scores alike, and a posterior so confident that
 *     one candidate wins forever, both stop a run from learning: the first
 *     hands every trial to the lowest index, the second to the same index.
 *     yaep_next() therefore falls back to a Halton point (returning -1, as
 *     in the init phase) when the best score is no better than the average of
 *     the scores by more than 1e-9 relative, or when the same candidate has
 *     won YAEP__REPEAT_MAX (3) trials in a row. yaep_next_subset() does
 *     not do this, because a Halton point is not in the subset. Measured on
 *     the semiparametric kernel with a level-set acquisition on the
 *     audiometric observer, where the second degeneracy was reliable: without
 *     the guard the threshold error froze at 23.4 dB, identical across all 20
 *     seeds, and the contour left the box in 14 of 30 context columns; with it,
 *     and with the priors above, the error falls to 8.1 dB pooled over the four
 *     phenotypes and no context column fails to cross.
 *
 *     EAVC WITH AN EMPTY LEVEL SET. When the expected number of candidates
 *     above the level is under one half or within one half of all of them,
 *     the model puts the level outside the box, every look-ahead moves the
 *     volume by almost nothing, and EAVC's argmax is noise (on the novel
 *     discrimination function it sampled the middle of the intensity range
 *     with every threshold in the bottom fifth). That trial is chosen by the
 *     LSE straddle instead, which walks toward the edge the level is past. On
 *     its own this took the columns without a crossing there from 21.9 to 12.2
 *     of 30; with the mean prior above it rarely engages.
 *
 *     THE LEVEL-SET GUARD (GP model, one-latent discrete likelihoods). A
 *     scheduled fit that reclassifies more than 5% of the candidates against
 *     the target, coming right after a fit that reclassified fewer than 2%,
 *     is undone, once: a fit proposed right after an undone one is accepted,
 *     so data that really demand the change get it one fit later. The
 *     failure it stops is a data-favored short-lengthscale mode: on the
 *     audiometric metabolic beta 0.5 observer a fit at trial 145 moved the
 *     lengthscales from (3.8 octaves, 41 dB) to (0.62, 7.5) at a log marginal
 *     likelihood 12 nats higher, interpolated the eleven sampled frequency
 *     columns, sent the unsampled ones to the mean, and took the threshold
 *     error from 2.0 to 37.8 dB. Neither a stronger prior nor a floor on the
 *     lengthscale stops that: the mean prior above holds the damage to 3.2 dB
 *     on the same trials, and a floor of one candidate spacing to 4.6, but
 *     the jump itself happens either way. The thresholds come from 1440
 *     scheduled fits of examples/aep/audiometric.c (20 replications, LSE,
 *     EAVC and BALV, metabolic and sensory at beta 2 and 0.5): after the
 *     first two fits the median fit reclassifies under 2% of the candidates;
 *     9 fits worsened the threshold by more than 3 dB, reclassifying 3 to
 *     25%; and the rule above undoes 19 fits, 8 of those 9, none of the fits
 *     that improved the threshold by more than 1 dB, and 113 dB of added error
 *     in all. On the comparison's own seeds, an independent check, it leaves
 *     every GP cell within its spread and the metabolic beta 0.5 LSE cell at
 *     1.89 dB (worst 3.74) against 4.09 (worst 37.8) before. It costs two
 *     candidate predictions per fit. It does not run under
 *     yaep_fit_step() called by the caller or the async layer's fit_in_idle,
 *     which have no before and after to compare.
 *
 *     What the guard does NOT do, in that one configuration: restore the run's
 *     dependence on the responses. Instrumented, SEMIP with LSE on the
 *     audiometric observer trips the repeat rule on every fourth trial for the
 *     whole session (145 trials, 35 fallbacks, and the flat test never fires),
 *     the acquisition returns to the same candidate immediately afterwards, and
 *     the candidate scores are bit-identical between two runs with different
 *     seeds. So the guard is a rate limiter there, worth 15 dB, and the run is
 *     still driven by the design rather than by the observer: the fit puts the
 *     intercept output scale at its ceiling, the latent variance swamps the
 *     responses, and the level-set score becomes a function of the candidate
 *     set alone. 8 dB of threshold error across 20 seeds takes only three
 *     distinct values for that reason. A fallback that proposes the best
 *     candidate NOT yet visited instead of a Halton point was measured and is
 *     not the answer: on one phenotype at beta = 2 it takes SEMIP with LSE from
 *     13.9 to 9.8 dB, but it takes SEMIP with BALV from 2.7 to 5.3 dB and RBF
 *     with LSE from 1.7 to 2.3, because a fallback that never repeats a
 *     candidate spends trials away from the threshold in the runs that were
 *     working. The repair for the SEMIP cell is in the model, not the
 *     acquisition: see SEMIP.
 *
 *   LOOK-AHEAD
 *     EAVC needs, for every candidate x*, the posterior at every other
 *     candidate AFTER one more observation at x*. With the
 *     Laplace Hessian W held at its current value, that posterior is the
 *     current one minus a rank-one term (the Woodbury identity on one new
 *     row), so the look-ahead latent variance at all M candidates for
 *     every one of the M candidates comes from the M x M posterior
 *     covariance among the candidates: O(M^2 N) to build it once per
 *     trial and O(M^2) per acquisition. The new observation enters the mean
 *     through one Newton step at the frozen W, and the outcome enters
 *     through its expectation, one quadrature per candidate. This is Letham
 *     et al.'s construction; the approximation is that W does not move on
 *     one trial, which is exactly the assumption the warm-started Newton
 *     step relies on anyway. Memory: the M x M covariance and the M x N
 *     solved kernel block it is built from, 4 MB together at M = N = 512.
 *     Only EAVC takes them; LOCALMI uses the variance at the candidate
 *     itself, which the ordinary prediction already gives.
 *     M is capped at 4096 under EAVC and 65536 otherwise.
 *     Two things make the M^2 scoring loop affordable. The level-set
 *     membership of a candidate is one normal cdf per candidate and outcome,
 *     and the loop uses a rational approximation of it (Abramowitz & Stegun
 *     26.2.17, 1e-7 absolute) rather than erfc, which costs 30 to 150 ns
 *     depending on the library and was most of a trial's time; predictions and
 *     thresholds are unaffected and still use erfc. And the sum runs over the
 *     CHANGE from the current volume, which lets it skip a candidate the new
 *     observation cannot move across the level. Together they took the scoring
 *     loop at M = 256 from 6 to 16 ms down to 3.5 to 3.9 ms, and a whole EAVC
 *     yaep_next() from 15 to 9 ms at N = 100 and from 101 to 28 ms at
 *     N = 400, measured as CPU time on one x86-64 desktop core under WSL2.
 *     What is left is the algorithm rather than the implementation: the M x M
 *     covariance costs M^2 N / 2 and the prediction M N^2 / 2, so halving M
 *     is what halves the first and quarters nothing else.
 *
 *   CANDIDATES (desc.candidates, desc.grid, desc.n_candidates,
 *   desc.refine_steps)
 *     desc.candidates: an explicit M x n_dims array the caller owns and
 *     must keep alive for the life of the handle, for the stimuli the
 *     display can actually produce. Else desc.grid[d]
 *     points per dimension for a product grid, last dimension fastest,
 *     endpoints included (every grid[d] must be at least 1 once one of them
 *     is set, except a CATEGORICAL dimension's, which is its level count; see
 *     DIMENSION KINDS under KERNELS). Else a Halton set of
 *     desc.n_candidates points (default 512). yaep_next_subset()
 *     restricts one trial to some of the candidates. The candidate set is
 *     also the set the look-ahead volume is measured on and the spacing
 *     yaep_threshold() scans with, so keep it fine enough along the
 *     intensity dimension.
 *     Finer is better, up to the cost. A coarse candidate set does act as a
 *     regularizer on a fit that has no prior, and on three replications of the
 *     audiometric observer it looked like a large one; on 20 replications with
 *     the weak priors of HYPERPARAMETERS in place the effect reverses, and the
 *     finer set wins: 1.46 +- 0.26 dB of threshold error at trial 150 on a
 *     15 x 25 grid against 1.98 +- 0.32 dB on 11 x 21, same seeds, same
 *     observer. Treat the candidate count as the cost knob it is, not as a
 *     way to regularize the model.
 *
 *     desc.refine_steps (default 0, off; at most 32) takes the grid winner off
 *     the grid. A proposal on a candidate set is quantized to it, and on the
 *     audiometric grid of 11 x 21 that is 7 dB along the intensity axis, where
 *     AEPsych optimizes the acquisition continuously. With refine_steps = n,
 *     yaep_next() runs n rounds of coordinate-wise golden-section search
 *     (10 iterations per dimension per round) on the acquisition, each
 *     dimension searched between the winner's two neighbors, one candidate
 *     spacing either side, clipped to the box; for a Halton or caller-owned
 *     set the spacing is what M points would have on a regular grid. The
 *     bracket stays on the winner's cell for every round, a round that moves
 *     nothing ends the search, and a move is taken only if it scores strictly
 *     higher, so the result never scores below the candidate it started from.
 *     A proposal that moved returns -1, like a Halton point.
 *     The objective is the acquisition at one point, which takes one
 *     prediction: exact for LSE, BALV, BALD and LOCALMI, whose candidate
 *     scores are that same arithmetic (the test checks the two agree to the
 *     last bit). EAVC is a sum over the OTHER candidates and has no value off
 *     the candidate set, so an EAVC winner is refined on the LSE straddle,
 *     which aims at the same level set: the look-ahead picks the cell and the
 *     straddle places the point inside it. RANDOM and yaep_next_subset() never
 *     refine. The cost is at most 1 + 12 n D predictions, 49 for n = 2 in two
 *     dimensions, each a kernel row and a triangular solve, which at M = 231
 *     is a fifth of the candidate sweep and inside the run-to-run noise of a
 *     whole trial.
 *     Measured with examples/aep/audiometric.c's protocol (metabolic phenotype,
 *     beta = 2, 20 replications, same seeds, trial 150):
 *
 *       refine_steps               0              2              4
 *       LSE   threshold, dB   1.98 +- 0.32   1.50 +- 0.13   1.53 +- 0.12
 *             field MAE(p)    0.077          0.089          0.085
 *             ms per trial    1.8            2.0            1.6
 *       EAVC  threshold, dB   1.98 +- 0.16   1.37 +- 0.15   1.38 +- 0.15
 *             field MAE(p)    0.058          0.073          0.073
 *             ms per trial    4.3            4.2            4.1
 *
 *     So refinement buys a quarter to a third of the threshold error and costs
 *     the field about as much: points placed exactly on the level set say more
 *     about the level set and less about everything else, and the early
 *     threshold is worse too (trial 25: 6.8 to 8.6 dB for LSE, 7.4 to 10.0 for
 *     EAVC). 4 rounds buy nothing over 2, because the search has almost always
 *     converged by then. The default stays 0, which keeps every earlier result
 *     reproducible; set 2 for a session whose deliverable is the threshold, and
 *     leave it 0 when it is the field.
 *
 *   THE LOOP
 *     yaep_next(g, x)             fills x[n_dims] with the next stimulus
 *     yaep_update(g, x, k)        what was shown, and the outcome; refits
 *     yaep_update_real(g, x, y)   the same under YAEP_LIK_GAUSSIAN
 *     yaep_done(g)                a stop criterion has fired
 *     yaep_threshold(...)         level set along the intensity dimension
 *     yaep_predict_*(...)         the field and its uncertainty anywhere
 *     yaep_fit_step(g)            one step of a hyperparameter fit
 *     yaep_refit(g)               the exact refit desc.refit_every skips
 *
 *   next() and update() are separate, as in the other adaptive headers:
 *   show what you can, tell update() what you showed, at any point of the
 *   box, candidate or not. A proposal that has not been answered is handed
 *   out again by the next yaep_next(), and yaep_history() marks a trial
 *   `proposed` only when the stimulus given to update() is exactly the one
 *   proposed, so a display that quantized it leaves a record.
 *
 *   ESTIMATES
 *     yaep_predict_f(g, x, k, &mu, &sd)  latent k's mean and sd at x
 *                                         (k = 0 except CATEGORICAL). Under
 *                                         YAEP_MODEL_PSYCHOMETRIC, k = 0 is
 *                                         the threshold m and k = 1 the log
 *                                         slope g at x's context, and the
 *                                         intensity of x is ignored.
 *     yaep_predict_p(g, x)               the target quantity at x:
 *                                         E[P(y = 1)], E[P(y >= k*)] or
 *                                         E[P(y = k*)]; E[y] under
 *                                         GAUSSIAN. The probit Bernoulli
 *                                         case is closed-form,
 *                                         Phi(mu / sqrt(1 + sd^2)); the
 *                                         rest are quadrature.
 *     yaep_predict_p_many(g, xs, n, p)   the target quantity at n points, and
 *     yaep_predict_f_many(g, xs, n, k, mu, sd)
 *                                         latent k at n points. One blocked
 *                                         pass rather than n passes, which is
 *                                         what a field plot per trial needs.
 *                                         The saving is cache, not work: a
 *                                         point costs a kernel row and a
 *                                         triangular solve either way, so at
 *                                         N = 150 a 900-point field measured
 *                                         22 us per point one at a time and
 *                                         11 us per point batched, and the
 *                                         gap grows with N as the factor
 *                                         stops fitting in cache. A field on
 *                                         the CANDIDATE set is cheaper still,
 *                                         because the acquisition has already
 *                                         computed it.
 *     yaep_predict_outcomes(g, x, p)     all K outcome probabilities.
 *                                         Under CATEGORICAL these are K
 *                                         one-against-the-rest quadratures
 *                                         renormalized to sum to 1, not the
 *                                         exact K-dimensional softmax
 *                                         expectation.
 *     yaep_threshold(g, ctx, target, &x, &lo, &hi)
 *       Bisection along the intensity dimension at context ctx (the other
 *       n_dims - 1 coordinates, in order) for the level where
 *       yaep_predict_p() crosses target (0 = the desc's target). lo and hi
 *       are the crossings of the mu - 1.96 sd and mu + 1.96 sd latent
 *       curves, sorted, so a falling curve gives the same band as a rising
 *       one: a credible band from the latent posterior, not from posterior
 *       samples of the threshold. It is a 95% interval for the threshold
 *       only as far as the latent curve is straight over its width, and it
 *       is as wide as the latent posterior at the crossing divided by the
 *       slope there, so a loose fit shows up as a wide band.
 *       A band edge with no crossing inside the
 *       box is clamped to the box. Returns YAEP_ERR_NOCROSS when the
 *       middle curve has no crossing inside the box, and
 *       yaep_threshold_multi_cross() reports when there was more than one
 *       (the non-monotone case), in which case the crossing at the lowest
 *       intensity is the one reported. The search scans the dimension
 *       before it bisects, at the candidate spacing (the grid count along
 *       that dimension, or M^(1/n_dims) clamped to 9..65 points), which is
 *       what lets it see a second crossing rather than bisect it away.
 *       Under YAEP_MODEL_PSYCHOMETRIC none of that: the threshold is the
 *       posterior mean of the crossing m + f_t exp(-g), f_t the latent level
 *       of the target (link^-1 of the target with the floor and ceiling taken
 *       off), which with (m, g) bivariate Gaussian is closed form,
 *         E = mm + f_t exp(-mg + vg / 2),
 *         Var = vm + f_t^2 (exp(-2 mg + 2 vg) - exp(-2 mg + vg))
 *               - 2 f_t cmg exp(-mg + vg / 2),
 *       and lo and hi are E -+ 1.96 sqrt(Var), clamped to the box. It is not
 *       where yaep_predict_p() crosses the target, which averages p over
 *       the posterior first; the two differ by the posterior's skew and agree
 *       as it narrows. YAEP_ERR_NOCROSS when E falls outside the box, and
 *       yaep_threshold_multi_cross() is always false.
 *     yaep_argmax(g, x, &value)
 *       The stimulus with the highest posterior mean of the target quantity
 *       (lowest under desc.minimize): the best candidate, refined by
 *       desc.refine_steps on the same objective. The optimization
 *       counterpart of yaep_threshold(), for any acquisition.
 *     yaep_get_hyper(g, out) and yaep_log_marginal(g) for the fit.
 *
 *   STOPPING (desc.stop_trials, desc.stop_threshold_sd)
 *     stop_trials ends after that many trials. stop_threshold_sd ends when
 *     the width (hi - lo) of yaep_threshold() at desc.stop_context is
 *     below it; with n_dims = 1 the context is empty. At least one is
 *     required, and yaep_open() rejects a desc with neither.
 *     YAEP_MAX_TRIALS (or desc.max_trials) is the hard ceiling.
 *     yaep_done() runs a threshold search every call while
 *     stop_threshold_sd is on, so call it once per trial, not in a poll.
 *
 *   ---------------------------------------------------------------------
 *   RETURN VALUES AND ERRORS
 *   ---------------------------------------------------------------------
 *   yaep_open() and yaep_open_saved() return bool and fill yaep_error();
 *   they clear it on entry. The calls that change the handle (yaep_next(),
 *   yaep_next_pair(), yaep_next_subset(), yaep_update(), yaep_update_real(),
 *   yaep_update_pair(), yaep_fit(), yaep_fit_step(), yaep_refit() and
 *   yaep_argmax()) also write yaep_error() when they fail. The queries take a
 *   const handle and return a code or NaN without writing the message.
 *   yaep_strerror() names a code.
 *
 *     YAEP_ERR_ARG        null handle, x outside the box, outcome out of
 *                          range, update() under GAUSSIAN or
 *                          update_real() under a discrete likelihood
 *     YAEP_ERR_CLOSED     the handle is not open
 *     YAEP_ERR_FULL       max_trials recorded
 *     YAEP_ERR_MEMORY     buffer too small or allocation failed (open)
 *     YAEP_ERR_NOCROSS    no threshold inside the box at that context
 *     YAEP_ERR_NUMERIC    a Cholesky failed; the trial is still recorded
 *                          and the posterior of the trials that did factor
 *                          stays in force, so predictions keep working from
 *                          one trial less. The next update tries again.
 *                          Happens with duplicate
 *                          stimuli at extreme outcomes and an output scale
 *                          fit too large; see desc.jitter.
 *
 *   yaep_next() is the exception to the code scheme: it returns a
 *   candidate index of 0 or more, -1 for a point that is not a candidate,
 *   and a code below -1 for a failure. YAEP_ERR_ARG is -1 and therefore
 *   unusable there, so a null handle, a null x, a closed handle and a bad
 *   subset all come back as YAEP_ERR_CLOSED, and a full handle as
 *   YAEP_ERR_FULL. yaep_argmax() returns -1 the same way, for a refined
 *   point. Both write the message only for a code below -1.
 *
 *   ---------------------------------------------------------------------
 *   MEMORY, COST AND THREADS
 *   ---------------------------------------------------------------------
 *   yaep_memory_size(desc) returns the bytes open() takes from
 *   desc.memory or from YAEP_MALLOC (malloc by default; define
 *   YAEP_MALLOC/YAEP_FREE before the include), exactly once. With N =
 *   max_trials, M candidates and K latents (1 except CATEGORICAL):
 *   N*N*8 for the kernel matrix, K*N*N*8 for the Laplace factor, another
 *   N*N*8 of workspace for the hyperparameter gradient (taken whether or
 *   not desc.fit is on, because yaep_fit() is callable at any time), one
 *   more N*N*8 under CATEGORICAL for the coupling factor, the M x M
 *   look-ahead covariance and its M x N staging block under EAVC,
 *   a handful of N*8 vectors, M*n_dims*8 for a generated candidate set,
 *   and the history. YAEP_MODEL_PSYCHOMETRIC adds a second N*N*8 prior
 *   matrix (the slope GP's), 18 N*8 vectors and M*40 bytes, and under EAVC
 *   2*M*N*8 for the look-ahead's columns and M*8, where the GP model's EAVC
 *   takes M*M*8 and M*N*8.
 *   Measured: Bernoulli at N = 512, M = 512 is 6.53 MB
 *   with LSE and 10.53 MB with EAVC; N = 200, M = 512 with LSE is 1.14 MB;
 *   a 3-class categorical at N = M = 512 with LSE is 12.63 MB; Bernoulli at
 *   N = 2048, M = 256 with LSE is 97.9 MB (MB here are 2^20 bytes). The
 *   history is in the block, N * 144 bytes, so the handle itself is 2904
 *   bytes on x86-64 whatever YAEP_MAX_TRIALS is. Nothing allocates after
 *   open.
 *
 *   Per update: a Laplace refit warm-started from the last mode, typically
 *   2 to 4 Newton steps, each an N x N Cholesky (N^3 / 3 flops) plus two
 *   triangular solves; under CATEGORICAL a step is K such factorizations
 *   plus K triangular inversions and one more Cholesky for the coupling
 *   term, so about 2.5 K times the Bernoulli cost; none under
 *   GAUSSIAN, where the kernel Cholesky grows by one row per trial in
 *   O(N^2). A hyperparameter fit
 *   is up to desc.fit_max_evals (40) refits with gradients and is the
 *   expensive call: run it in the inter-trial interval one yaep_fit_step()
 *   at a time, or set fit_every to 0 and call yaep_fit() when you choose.
 *   On a large data set a converged fit is minutes: on 500 trials of the 6-D
 *   contrast sensitivity data (HYPERPARAMETERS), about 33 s for the GP model
 *   and 541 s for the psychometric model on the loaded machine that ran
 *   tests/compare/compare_gp_csfdata.py, and 6.9 s and about 200 s (updates
 *   included) for the same protocol on a quiet one. desc.fit_pcg (below)
 *   takes them to 3.4 s and 57 s. At YAEP_MAX_TRIALS 1024 the GP model's
 *   block is about 25 MB, and an 800-trial fit costs about 4 times the
 *   500-trial one.
 *   Per trial in 6-D, the psychometric model with EAVC and 1000 candidates
 *   is about 4 s on a loaded machine: the look-ahead is M^2 N with M = 1000,
 *   and every one of the 1000 contexts takes its own bivariate quadrature.
 *   A 6-D session wants a few hundred candidates, a per-point acquisition
 *   (BALD, BALV, LSE) or the async layer, and usually two of the three.
 *
 *   desc.refit_every buys the update down to O(N^2). The exact Laplace
 *   factor cannot grow by a row, because the Hessian W changes in every
 *   entry with every trial; but if W is FROZEN at the values the last exact
 *   refit left, then adding a trial borders B with one row, its Cholesky
 *   gains one row by a triangular solve and a square root, and one
 *   quasi-Newton step moves the mode: five matrix-vector products in place
 *   of a factorization. That is the same rank-one construction the
 *   look-ahead acquisitions already use, applied to the training set.
 *   refit_every = k does that on every trial and an exact Newton refit
 *   every k trials; k larger than the session does no exact refit at all
 *   inside the loop, leaving yaep_refit() to the caller between blocks.
 *   The cost is drift: on a replayed 78-trial run, three cheap updates
 *   after the last exact refit put the posterior 1.7e-4 of probability
 *   away from the exact one, and yaep_refit() erases it. BERNOULLI and
 *   ORDINAL only; GAUSSIAN is already incremental and CATEGORICAL always
 *   refits.
 *
 *   desc.pcg_threshold keeps the update exact and makes its Newton steps
 *   O(N^2). Above that many trials (0 = 512, negative = never), an update's
 *   Newton steps solve B x = r by conjugate gradients instead of factoring
 *   B, preconditioned by the factor the previous update left, bordered by
 *   the new trial's row. At the first step that bordered factor is B's own,
 *   because the old sites' W has not moved yet; at the later steps it is
 *   close, and each solve takes a few iterations of one matrix-vector
 *   product and two triangular solves, 2 N^2 each, to a relative residual of
 *   1e-13. One factorization remains per update, at the end, for the
 *   predictive variances and the log determinant, where the factored path
 *   does one per Newton step; a solve that has not converged in 60
 *   iterations falls back to factoring. BERNOULLI, ORDINAL and PAIRWISE,
 *   the one-latent Laplace; a hyperparameter fit, CATEGORICAL, GAUSSIAN (no
 *   Newton) and the psychometric model factor as before, unless
 *   desc.fit_pcg (below) says otherwise. The result is the
 *   factored path's to the Newton tolerance, not to the bit: the test
 *   replays 200 trials both ways and finds the modes 4e-11 apart and the
 *   predictions 5e-12. The default of 512 is the default YAEP_MAX_TRIALS,
 *   so a build that does not raise it never takes the path and every
 *   earlier result stands to the bit.
 *   Measured on one x86-64 desktop core under WSL2, gcc -O2, Bernoulli probit
 *   RBF over a 2-D box, M = 256, fixed hyperparameters, YAEP_MAX_TRIALS
 *   2048 with the caller's 97.9 MB block, one update in ms, factored against
 *   conjugate gradients (about 20 iterations an update, summed over its
 *   Newton steps), and one yaep_next():
 *
 *       N     factored     CG    next
 *      64       0.25      0.37    0.6
 *      96       0.35      0.33    0.6
 *     128       1.05      0.93    1.8
 *     256       9.3       3.2     3.5
 *     512      34        15      14
 *    1024     340       130      75
 *    1536    1697       605     279
 *    2048    3858      1169    ~500
 *
 *   So the crossover is near N = 100, the gain grows to 2.6 times at
 *   N = 1024 and 3.3 at 2048, and it is capped there by the one
 *   factorization an exact update still needs (N^3 / 3: 360 M flops at
 *   1024, 2.9 G at 2048).
 *   Setting pcg_threshold to 100 buys 1.1 to 2.9 times between 128 and 512
 *   trials, at the price of results that match earlier versions to 1e-11
 *   instead of to the bit.
 *
 *   CONJUGATE GRADIENTS IN A FIT (desc.fit_pcg). What a fit evaluation
 *   factors, measured with gprof on fold 0 of the 6-D contrast sensitivity
 *   data (500 trials, the protocol of tests/compare/compare_gp_csfdata.py:
 *   stepped fits until a whole one moves the log marginal by < 1e-3 nats):
 *     GP model: 189 evaluations for 40 gradients, 136 fit steps. Every
 *     evaluation is a warm-started Newton search, 3.4 steps on average, each a
 *     Cholesky of B (635 in all, 58% of the time); a gradient adds B^-1
 *     explicitly for the trace terms (27%). A third of the evaluations are not
 *     search at all: a rejected line-search point is undone by refitting the
 *     old point from the rejected point's mode, and the gradient step after an
 *     accepted point refits the point it is already at.
 *     Psychometric model: the Gauss-Newton mode search converges linearly,
 *     about 27 steps a search, each a Cholesky of I + U' K U: 16514
 *     factorizations for 552 searches (500 updates and 52 evaluations), 90% of
 *     the time.
 *   Which solves can reuse a factor: every one. The hyperparameters move a
 *   little per line-search point and the Gauss-Newton U a little per step, so
 *   the factor the last evaluation or the last step left is a close
 *   preconditioner, and a conjugate-gradient solve with it takes about 7
 *   iterations of two matrix products and two triangular solves (a quarter of
 *   a factorization at N = 500, less below). What cannot be reused is the log
 *   determinant: the objective is log p(y | f) - 1/2 a'h - log|B|, and a line
 *   search accepts or rejects on it. Measured on the same fits, 54% (GP) and
 *   70% (psychometric) of the accept-or-reject decisions were within 1e-3
 *   nats, so an estimate of log|B| (stochastic Lanczos, Hutchinson) would
 *   have to be good to well under 1e-3 nats to leave them alone, which takes
 *   far more probe vectors than a factorization costs (not measured here),
 *   and would give up reproducibility besides; a factor reused for k
 *   evaluations gives no log|B| at the new point at all. So
 *   every evaluation still ends in one factorization, and fit_pcg removes the
 *   others:
 *     - the Newton or Gauss-Newton steps solve by preconditioned conjugate
 *       gradients (tolerance 1e-13), with one factorization at the end of the
 *       search for log|B|, the predictions and the gradient; a solve that
 *       takes 60 iterations falls back to factoring;
 *     - a rejected line-search point is undone by copying back the state the
 *       accepted point left (the factor, the mode vectors, the
 *       hyperparameters), held in a second N x N block;
 *     - the gradient step after an accepted point reuses the state that point
 *       left instead of refitting it;
 *     - the psychometric model's updates take the same conjugate-gradient mode
 *       search, preconditioned by the last trial's factor bordered by the new
 *       row.
 *   An inexact Newton variant (solve only to 1e-3 of the last step's size)
 *   cut the conjugate-gradient iterations by a third and was dropped: the
 *   modes it left were less accurate, the analytic gradient with them, and on
 *   ten audiogram sessions its psychometric fits stopped 0.38 nats lower on
 *   average.
 *   Measured (thread CPU time on an idle x86-64 core, gcc -O2, fold 0, the
 *   shorter of two runs; the fits reach the same point to the printed
 *   digits, 136 and 779 steps both ways, and the same held-out log loss):
 *
 *                           v0.13.1    v0.14.0    v0.14.0, fit_pcg
 *     GP model, fit          8.7 s      6.9 s      3.4 s
 *     psychometric, updates  32 s       32 s       14 s
 *     psychometric, fit      181 s      168 s      43 s
 *
 *   (v0.14.0 without the switch is faster because the explicit B^-1 now walks
 *   rows: 118 to 44 ms at N = 500, the same bits.) The factorizations fall
 *   from 635 to 101 (GP) and from 16514 to 555 (psychometric). On the
 *   audiogram's box at N = 150 (an LSE session replayed into each
 *   configuration, ten seeds) the GP fit goes from 0.25 to 0.16 s, to the
 *   same log marginal, and the psychometric fit from 3.2 to 2.3 s, to log
 *   marginal -68.503 against -68.500 and threshold error 1.318 against 1.314
 *   dB: the two paths agree to the Newton tolerance at every evaluation, and
 *   on the psychometric model's flat ridge of lengthscales that is enough to
 *   end a fit at a different point of the ridge. That is also why fit_pcg is
 *   off by default: the default path keeps every earlier result to the bit.
 *   The test runs the two paths in lockstep, one fit step at a time, under
 *   BERNOULLI, ORDINAL, PAIRWISE and the psychometric model, and finds them
 *   at the same point after every step to 2e-9. Memory: another N x N block
 *   and 23 N-vectors, 2.2 MB at N = 512. GAUSSIAN (no Newton step) and
 *   CATEGORICAL ignore the switch.
 *
 *   Per next: M candidates, each a kernel row (N) and a triangular solve
 *   (N^2 / 2), so M*N^2/2 flops: 256 candidates at N = 200 is 5 M, at
 *   N = 400 20 M; EAVC adds M^2 N / 2 once and M^2 per candidate and
 *   outcome. Predictions are computed in blocks of 32 candidates so the
 *   factor is read once per block rather than once per candidate.
 *
 *   FRAME BUDGET
 *   A trial's yaep_next() plus yaep_update() should fit in one display
 *   frame, 16 ms, so an experiment can call them between trials with no
 *   thread and no pacing. examples/aep/bench.c measures exactly that and
 *   prints the largest trial count that stays inside it. Measured on v0.14.0
 *   on one x86-64 laptop core (an i7-1360P under WSL2, load average under 1),
 *   gcc 11.4 -O2, Bernoulli probit RBF over a 2-D box with M = 256
 *   candidates, next + update, median of a ten-trial window, in ms:
 *
 *     N     LSE      LSE no      EAVC     EAVC no
 *           exact    refit       exact    refit
 *     50     0.34     0.24        3.2      3.3
 *     100    1.2      0.70        4.8      4.5
 *     200    4.9      2.4         9.1      7.7
 *     400   21.1      8.6        30.0     15.0
 *
 *   and the largest N whose next + update still fits the frame, from that
 *   run and a second one a minute later:
 *
 *     LSE, exact refit every trial      347 and 348
 *     LSE, no refit inside the loop     past 400
 *     EAVC, exact refit every trial     285 and 299
 *     EAVC, no refit inside the loop    past 400
 *
 *   Past 400 trials the frame is gone for an exact update whatever the
 *   solver: with conjugate-gradient Newton steps (desc.pcg_threshold) an
 *   update is 15 ms at N = 512, 130 ms at 1024 and 1.2 s at 2048, and
 *   yaep_next() at M = 256 adds 14, 75 and about 500 ms (it is M N^2 / 2).
 *   A session that long takes its trials from another thread (YAEP_ASYNC,
 *   where a proposal is late by the update's cost in frames, 8 frames at
 *   N = 1024), or runs desc.refit_every with an exact refit between blocks,
 *   or both; a larger YAEP_MAX_TRIALS changes the memory, not the frame
 *   arithmetic.
 *
 *   A hyperparameter fit does not fit a frame at any useful N, which is what
 *   yaep_fit_step() is for: on the same machine at N = 400 one step is 28 to
 *   37 ms and a whole fit is 40 steps, 1.0 to 1.2 s, so a fit spread one step
 *   to a trial costs a session 40 trials of latency instead of one trial of
 *   a second.
 *
 *   ("no refit" is refit_every past the session length.) Read those as an
 *   order of magnitude, not as a specification: the run-to-run spread is tens
 *   of percent even quiet, the largest-N figures moved between 180 and 380 over
 *   runs of the same binary on a loaded machine (v0.1, an x86-64 desktop),
 *   and anything else running inflates all of them. Measure your own
 *   configuration with aep_bench.
 *
 *   What to do when
 *   a configuration does not fit: fewer candidates (next is linear in M,
 *   and EAVC is quadratic), a cheaper acquisition (LSE and LOCALMI need no
 *   M x M covariance), desc.refit_every with yaep_refit() between blocks,
 *   yaep_fit_step() instead of yaep_fit() so the fit is spread over
 *   trials, a lower max_trials, or another thread: YAEP_ASYNC is that thread,
 *   written and tested, and ASYNC says what it does and does not buy. A handle
 *   is not thread-safe, and nothing in the header is for the frame loop
 *   itself: these calls belong between trials.
 *
 *   Every function is deterministic unless desc.rng is set: the same desc
 *   and the same history give the same posterior, the same proposals and
 *   the same fit. A saved history replayed through yaep_update()
 *   restores a run, except that the fit schedule is by trial count, so
 *   replay with the same fit_every, and a stepped fit's progress is not in
 *   the history. A snapshot (SNAPSHOTS) restores it without the replay.
 *
 *   SNAPSHOTS (yaep_save_size, yaep_save, yaep_open_saved)
 *     yaep_save() writes yaep_save_size(g) bytes; yaep_open_saved() opens a
 *     handle from them and a desc that re-supplies what a snapshot cannot
 *     carry (the pointers: rng and rng_ctx, candidates, memory), checks the
 *     desc's numbers against the saved ones, and the session continues where
 *     it was cut, a pending proposal included, with no refit and no replay:
 *     the next proposal, the next update and every estimate are the
 *     uninterrupted session's to the bit. The generator's state is the
 *     caller's: save it beside the snapshot and put it back before the next
 *     call. The test cuts sessions under eleven configurations (GP LSE with
 *     refinement, GP EAVC, the psychometric model with BALD and EAVC,
 *     CATEGORICAL, ORDINAL with refit_every, GAUSSIAN UCB, PAIRWISE
 *     Thompson, and mixed kinds with a monotone projection and conjugate-
 *     gradient updates, and GP LSE and the psychometric model with
 *     desc.fit_pcg) at six trials each, before and after the proposal,
 *     a stepped fit in progress, and loads into a handle filled with
 *     garbage: all 132 resumed sessions match the uninterrupted ones in every
 *     proposal and in their final snapshots, byte for byte.
 *     SNAPSHOT LAYOUT, format 1. Every integer is little-endian two's
 *     complement, written and read byte by byte, so a snapshot moves between
 *     compilers and platforms of the same YAEP_REAL; an f64 is the IEEE 754
 *     bit pattern as a u64, and a yaep_real is its own bit pattern (8 bytes,
 *     or 4 in a float build).
 *       magic     4 bytes "PSGP", then u32 format (1), u8 sizeof(yaep_real)
 *       desc      every number of yaep_desc in field order, arrays to
 *                 n_dims (hyper, hyper_min and hyper_max each as
 *                 lengthscale, outputscale, mean, lengthscale_b,
 *                 outputscale_b, YAEP_MAX_OUTCOMES - 1 cutpoints, noise_sd,
 *                 lengthscale_g, outputscale_g, mean_g; bools as u8; each
 *                 prior as center, sd, ceiling); rng and candidates as u8
 *                 set-or-not, and a caller's candidate set value by value
 *       state     i32 N, N_fit, halton_index, proposed, last_index, repeats,
 *                 fit_state, fit_evals, fit_back, stop; u8 fit_valid,
 *                 multi_cross, fit_blocked; f64 fit_step, fit_best,
 *                 log_marginal, fit_flip, opt_best, ps_tol, fit_delta; i32
 *                 fit_conv; the hyperparameters in force as above
 *       history   N + 1 entries (the last is the pending proposal, if any):
 *                 f64 x[n_dims], f64 y, u8 proposed, u8 init, and f64
 *                 x2[n_dims] under PAIRWISE
 *       trials    X (N x n_dims), X2 under PAIRWISE, y (N)
 *       cands     the generated candidate set, M x n_dims, when desc has none
 *       posterior per latent: f, W, the likelihood gradient, alpha,
 *                 sqrt(W) (N each) and the factor's lower triangle; the
 *                 kernel matrix's lower triangle; under the psychometric
 *                 model the slope GP's kernel matrix and the 18 site vectors
 *                 (N each); under CATEGORICAL the coupling factor; the
 *                 stepped fit's 6 x YAEP__NTHETA f64
 *       fit_pcg   only when desc.fit_pcg applies: i32 the count of the last
 *                 evaluation's theta and that many f64; u8 stash set, and
 *                 when set f64 its objective and log marginal, its
 *                 hyperparameters, its factor's lower triangle and its
 *                 23 N-vectors
 *     The candidate cache is not saved; it is a function of the rest and the
 *     next yaep_next() rebuilds it. Size: about (K + 1 + [psychometric]) N^2
 *     / 2 yaep_reals plus 30 N f64s, 23 KB at N = 40 in two dimensions,
 *     about 200 KB at N = 150 and 2.1 MB at N = 512.
 *     yaep_open_saved() compares the desc section with the desc it is given and
 *     names the first field that differs, range-checks the counters, and
 *     fails a wrong, truncated or corrupt snapshot with a message and the
 *     handle closed. Nothing but the bytes is checked: a snapshot edited to
 *     hold other finite numbers loads, and the session continues from them.
 *
 *   ---------------------------------------------------------------------
 *   SIMULATION
 *   ---------------------------------------------------------------------
 *   yaep_simulate_outcome(p, K, u) draws an outcome from K probabilities
 *   with the uniform variate u in [0, 1) that the caller supplies; K = 2
 *   with p = {1 - p1, p1} is the Bernoulli case. The header draws nothing
 *   itself; desc.rng is the caller's generator lent to the header for the
 *   two places randomness helps (ties, random acquisition). examples/aep/sim.c
 *   runs the LSE, EAVC and BALV configurations against a 2-D observer with
 *   a known threshold curve and prints the error of yaep_threshold() per
 *   trial. tests/compare/compare_gp_aepsych.py runs the comparison with
 *   AEPsych on the same kind of observer, through the Python binding, in one
 *   process, on one response stream; STATUS has its result.
 *   tests/adapt/aep_test.c is the self-checking test the STATUS block
 *   quotes; examples/aep/bench.c is where the cost numbers come from.
 *
 *   ---------------------------------------------------------------------
 *   PRECISION (YAEP_REAL)
 *   ---------------------------------------------------------------------
 *   The matrices that dominate the cost and the memory hold yaep_real,
 *   which is double unless YAEP_REAL says otherwise: the kernel matrix, the
 *   Laplace factors, the hyperparameter gradient's workspace and the
 *   look-ahead covariance. Every vector, every accumulation and every scalar
 *   is double in both builds, the dot products accumulate in double, and the
 *   Newton tolerance, the smallest accepted fit step and the default jitter
 *   move with the working precision (1e-11, 1e-9 and 1e-6 in double;
 *   1e-5, 1e-5 and 1e-4 in float; a Newton step in float cannot resolve a
 *   mode better than about 1e-5 of the latent, and asking for more only runs
 *   the iteration to its cap).
 *
 *   -DYAEP_REAL=float halves those matrices: 10.44 MB becomes 5.44 MB for
 *   Bernoulli at N = M = 512 with EAVC. What it costs, measured by
 *   tests/adapt/aep_test.c, which prints the worst error it saw in either
 *   build (double, then float): Cholesky and triangular inverse 6e-17 and
 *   5e-7, the Laplace stationarity residual 3e-16 and 9e-5, the predictive
 *   variance against a dense (K^-1 + W)^-1 9e-16 and 2e-5, the Laplace log
 *   marginal likelihood 0 and 1e-5, the rank-one look-ahead covariance 2e-12
 *   and 9e-6, the analytic hyperparameter gradient against central
 *   differences 9e-5 and 2.9e-2 relative (and the difference step has to grow
 *   from 1e-5 to 3e-3, because a float objective is only good to about 1e-7).
 *   The same holds for the fits whose gradient IS central differences
 *   (CATEGORICAL): their step is 1e-4 of the hyperparameter in double and
 *   3e-3 in float. In float the psychometric model's mode is good to about
 *   4e-4 of the latent, which is what its analytic gradient is then good to
 *   as well.
 *   yaep_refit() restores the exact posterior to 6e-15 in double and 6e-8 in
 *   float.
 *   What it does not cost: the answers an experiment reads. Over 20 response
 *   streams of a 100-trial simulated observer the mean absolute threshold
 *   error is 0.0130 against 0.0134 for LSE, 0.0128 against 0.0139 for EAVC
 *   and 0.0323 against 0.0429 for BALD, and neither build reported a single
 *   YAEP_ERR_NUMERIC in 6000 updates.
 *
 *   What float does NOT buy is time. The same 6000 updates took 42.3 s in
 *   double and 41.0 s in float, three percent; and per phase, measured as CPU
 *   time at M = 256 (candidate prediction, covariance build, scoring loop and
 *   one Laplace refit, ms), double against float:
 *
 *     N = 50    0.5 2.4 3.4 0.04   against   0.6 3.1 3.8 0.05
 *     N = 100   0.9 2.6 2.6 0.14   against   1.6 7.4 8.1 0.29
 *     N = 200   4.0 7.3 3.5 1.02   against   4.8 10.3 3.0 1.48
 *     N = 400  19.3 32.9 3.8 9.0   against  19.1 34.4 3.7 14.5
 *
 *   which is a wash or slightly worse. The reason is that every loop here is
 *   scalar at -O2, so a float multiply costs exactly what a double multiply
 *   costs, and the matrices still reach the same cache level. Float would pay
 *   where the loops vectorize or where the working set crosses a cache
 *   boundary; measure it with examples/aep/bench.c before believing in it.
 *
 *   YAEP_LIK_GAUSSIAN is not available in a float build and yaep_open()
 *   says so: its exact posterior factors the raw kernel matrix rather than
 *   I + W^1/2 K W^1/2, and that matrix is too ill-conditioned for float.
 *   Double remains the default on those numbers.
 *
 *   ---------------------------------------------------------------------
 *   ASYNC (YAEP_ASYNC)
 *   ---------------------------------------------------------------------
 *   Everything above runs on the thread that calls it, and FRAME BUDGET says
 *   what that costs: some configurations fit a 16 ms frame and the interesting
 *   ones do not. A look-ahead acquisition at a few hundred trials is tens of
 *   milliseconds and a hyperparameter fit is hundreds, so the knobs there
 *   (fewer candidates, a cheaper acquisition, refit_every, fit_step) all trade
 *   something away to buy the frame back. The other answer is to not do the
 *   work on the frame loop's thread at all. Define YAEP_ASYNC before the
 *   include and this header gains one:
 *
 *       #define YAEP_ASYNC
 *       #define YSP_AEP_IMPLEMENTATION
 *       #include "ysp/aep.h"             // brings ysp/rt.h with it
 *
 *       static yaep_gp g;
 *       yaep_open(&g, &desc);          // as usual, on this thread
 *       static yaep_async a;
 *       yaep_async_desc ad = { 0 };
 *       ad.gp           = &g;
 *       ad.context[0]   = 0.5;         // where the snapshot's threshold is
 *       ad.fit_in_idle  = true;        // the fit lives in the gaps
 *       ad.below_normal = true;        // never above the frame loop
 *       if (!yaep_async_open(&a, &ad)) die(yaep_async_error(&a));
 *
 *       yaep_snapshot s;
 *       yaep_async_poll(&a, &s);       // the first proposal, already there
 *       for (;;) {
 *           int k   = run_trial(s.x);             // your frames
 *           int seq = yaep_async_submit(&a, s.x, k);
 *           while (drawing_frames())              // the interval
 *               if (yaep_async_poll(&a, &s) >= seq) break;
 *           if (s.done) break;
 *       }
 *       yaep_async_close(&a);          // drains; the handle is yours again
 *
 *   WHAT IT IS. ysp/rt.h's yrt_pump with yaep_update(), yaep_next() and
 *   yaep_threshold() in its on_msg: one thread, a queue of YAEP_ASYNC_QUEUE
 *   responses (8 by default) inline in the handle, no heap, normal or
 *   below-normal priority and never above it. Read ysp/rt.h's PUMP section for
 *   the queue, the seq and the lock; this layer is a few hundred lines on top
 *   of it and adds no concurrency of its own. ysp/quest.h's YQST_ASYNC is the
 *   same layer over QUEST+ and reads the same.
 *
 *   THE FIT IN THE GAPS is what the GP has that QUEST+ does not.
 *   desc.fit_in_idle puts one yaep_fit_step() in the pump's on_idle, which
 *   runs whenever the response queue is empty, and takes over from
 *   desc.fit_every, which the layer parks at 0 for the duration and restores at
 *   close. The effect is that the hyperparameters keep improving between
 *   trials and cost the trial loop nothing, and that a response never waits
 *   behind a whole fit: on_idle returns after one evaluation, the queue is
 *   checked, and the response goes first. A snapshot published from an idle
 *   step carries the same seq as the last response with new hypers, a new log
 *   marginal and `fitting` still true; it does not move the proposal, which
 *   belongs to the responses. Leave fit_in_idle off and desc.fit_every is
 *   honored as usual, on the pump thread, which is then one slow trial in
 *   every fit_every rather than a spread cost.
 *
 *   OWNERSHIP, which is the rule to get right. The caller opens the gp handle
 *   and yaep_async_open() takes it over: from then until yaep_async_close()
 *   returns, NO yaep_* call on that handle is allowed from any thread,
 *   including the const ones (yaep_predict_p() writes the handle's prediction
 *   scratch, so even a read of the posterior is a write). Everything a trial
 *   loop needs is in the snapshot instead: the proposal, the trial count, the
 *   stop flags, the threshold with its band, the hyperparameters, the log
 *   marginal likelihood and how many updates have returned YAEP_ERR_NUMERIC.
 *   After stop the handle is the caller's again and every yaep_* call is
 *   legal, which is how a session ends: stop, then read the history, the
 *   posterior, the field and the estimates at leisure.
 *
 *   THE SEQ. yaep_async_submit() returns a positive, increasing seq for the
 *   response it copied. yaep_async_poll() returns the seq the snapshot
 *   accounts for: every response through it has been applied AND the proposal
 *   in the snapshot was computed after them. So `poll() >= seq` is the "is the
 *   next stimulus ready?" test, it is one atomic load and a struct copy, and a
 *   frame loop can afford it every frame. yaep_async_wait() is the blocking
 *   form with a relative timeout, for the end of an interval.
 *
 *   Seq 0 is the proposal yaep_async_open() computed before the thread
 *   existed, so a snapshot always exists once open has returned true and the
 *   first trial reads its stimulus from a poll that returns 0. That is why
 *   there is no "nothing published yet" state to handle.
 *
 *   A FULL QUEUE is YAEP_ERR_BUSY, nothing was copied, and the caller still
 *   owns the response: retry it on the next frame, which examples/aep/async.c
 *   does. A queue that grew instead would trade a visible error for an
 *   invisible unbounded latency. With the default queue of 8 it takes eight
 *   trials submitted faster than the thread drains to get there.
 *
 *   WHAT IT DOES NOT DO. It does not make a slow configuration fast, it moves
 *   it: a proposal that takes four frames still takes four frames, and what the
 *   loop gains is that it is drawing during them instead of blocked. The cost
 *   of being late is one more frame of the same stimulus, which an experiment
 *   has to be written for; examples/aep/async.c counts how often it happened.
 *   It does not touch determinism either: the thread runs the same functions in
 *   submit order, so the same responses give the same posterior bit for bit
 *   whether they went through the queue or not, and
 *   tests/adapt/aep_test.c checks exactly that with memcmp. (With
 *   fit_in_idle on the fit sees a different schedule, so the posterior is a
 *   different one, reproducible only for the same timing. Bit-for-bit replay of
 *   an async session means replaying the history through yaep_update() with
 *   fit_in_idle off.)
 *
 *   COST. The handle is 6240 bytes on x86-64 at the default ceilings: mostly
 *   yrt_pump's inline ring, which this layer does not use
 *   (YRT_PUMP_INLINE_BYTES can be set to 1 if nothing else in the program
 *   needs it), plus 80 bytes a queued response and a 440-byte snapshot.
 *   A submit is a mutex, an 80-byte copy and a condition-variable signal. A
 *   poll is an atomic load and a 440-byte copy under a lock nothing else holds
 *   for long. Nothing allocates, at open or after.
 *
 *   ---------------------------------------------------------------------
 *   BUILDING
 *   ---------------------------------------------------------------------
 *   Nothing to link but libm, unless YAEP_ASYNC is defined: then ysp/aep.h
 *   includes ysp/rt.h, which must sit beside it, and POSIX builds link
 *   -pthread. Copy both headers in that case, as a transport header's user
 *   does. YSP_AEP_IMPLEMENTATION then also compiles ysp/rt.h's implementation,
 *   unless the same translation unit already defined YSP_RT_IMPLEMENTATION or
 *   implemented a transport header; either order gives exactly one copy.
 *   YAEP_ASYNC together with YRT_NO_THREADS is a #error: the layer IS a
 *   thread, so there is nothing sensible to compile.
 *
 *   Define YAEP_API to override the default
 *   `extern` linkage (definitions too, so static works). Define
 *   YAEP_MAX_DIMS (8), YAEP_MAX_OUTCOMES (8), YAEP_MAX_TRIALS (512),
 *   YAEP_REAL (double) and YAEP_ASYNC_QUEUE (8, the response queue, which
 *   sizes the yaep_async handle)
 *   before the include to resize the handle and the ceilings; define them the
 *   same in every translation unit. Every matrix
 *   is row-major and dense, every loop is scalar: there is no BLAS and no
 *   thread, so -O2 and a compiler that vectorizes a dot product are the
 *   whole performance story.
 *
 *       cc -O2 -Iinclude -o aep_sim examples/aep/sim.c -lm
 *       cl /O2 /Iinclude examples\aep\sim.c
 *       cc -O2 -pthread -Iinclude -o aep_async examples/aep/async.c -lm
 *
 *   ---------------------------------------------------------------------
 *   CHANGELOG
 *   ---------------------------------------------------------------------
 *   v0.15.0 - the repository conventions (README.md, Conventions). Breaks
 *          the API: yaep_async_start() / yaep_async_stop() are
 *          yaep_async_open() / yaep_async_close(), yaep_load() is
 *          yaep_open_saved(), and yaep_async_wait() takes an int64_t
 *          timeout_ns (0 or less polls). The handle-changing calls (next,
 *          update, fit, refit, argmax) write yaep_error() when they fail;
 *          the const queries still return a code only. The USAGE and ASYNC
 *          examples zero the desc and set fields, so they build as C99 and
 *          C++17. No number changed: the arithmetic is v0.14.1's.
 *   v0.14.1 - two fixes. (1) A desc.memory buffer that was not 8-byte
 *          aligned: yaep_open() aligned the base, but every later call laid
 *          its scratch out from the unaligned address, so the quadrature
 *          weights and every scratch vector read a few bytes off what open()
 *          had written (the macOS CI failure: outcome probabilities summing
 *          to 8e280, in the build whose static test buffer landed on an odd
 *          address). malloc'd handles were never affected. (2)
 *          yaep_threshold()'s band read an unset edge when neither shifted
 *          curve crossed; the band is now the whole box there, which is what
 *          the unset read produced in practice. Results with malloc'd or
 *          aligned memory are v0.14.0's to the bit.
 *   v0.14.0 - desc.fit_pcg (a new field at the end of yaep_desc, off by
 *          default): conjugate-gradient Newton steps in a fit's evaluations
 *          and in the psychometric model's mode search, rejected line-search
 *          points undone by a copy, and no refit of a point the model already
 *          stands at; 2.6 times on a 500-trial 6-D GP fit and 3.7 times on
 *          the psychometric model's. The explicit B^-1 of the gradient walks
 *          rows instead of columns, 2.7 times faster with the same bits, so
 *          every result at the defaults is v0.13.1's. See CONJUGATE GRADIENTS
 *          IN A FIT under MEMORY, COST AND THREADS.
 *   v0.13.1 - no code change. The 6-D contrast sensitivity problem's stuck
 *          flat model (guess 0.5, mean at its bound, p = 0.5 everywhere)
 *          is v0.4.1's behavior, fixed since v0.5.0 by the mean prior; the
 *          test now replays its first 30 trials with and without the prior.
 *          The cost section quotes the 6-D numbers.
 *   v0.13.0 - desc.fit_max_evals and desc.fit_tol (0 = the old 40 and 1e-8),
 *          yaep_fit() returns 1 when converged and 0 when the budget ran
 *          out (it returned 0 for both), and yaep_fit_delta() reads the
 *          objective's change. A yaep_fit() after one the budget stopped
 *          continues with its step length. A caller that tested
 *          yaep_fit() == 0 for success must test >= 0. Scheduled fits, and
 *          so every session result at the defaults, are v0.12.0's.
 *   v0.12.0 - snapshots: yaep_save_size(), yaep_save() and yaep_load(), a
 *          versioned little-endian byte image of the whole session that
 *          resumes it bit for bit without replay. See SNAPSHOTS under
 *          MEMORY, COST AND THREADS.
 *   v0.11.0 - desc.pcg_threshold: above it (default 512, so never at the
 *          default YAEP_MAX_TRIALS), an update's Newton steps are
 *          preconditioned conjugate-gradient solves with the previous factor
 *          as the preconditioner, 2.6 to 3.3 times faster from N = 1024 on.
 *          The history moves from the handle into the memory block, so the
 *          handle is 2.5 KB and YAEP_MAX_TRIALS can be raised to 2048 with
 *          the caller's memory; yaep_memory_size() grows by 144 bytes a
 *          trial. yaep_desc gains pcg_threshold at its end.
 *   v0.10.0 - desc.monotone_dims, AEPsych's monotonic projection of the
 *          posterior mean along the masked dimensions, for predict_p,
 *          predict_p_many, predict_p_var, the threshold and the level-set
 *          acquisitions; a new field at the end of yaep_desc. The copies
 *          whose count is an int are loops instead of memcpy, which gcc 13's
 *          -Wstringop-overflow flagged at -O3 under _FORTIFY_SOURCE; results
 *          are v0.9.0's to the bit. See MONOTONIC PROJECTION under KERNELS.
 *   v0.9.0 - mixed parameters: desc.dim_kind[] (CONTINUOUS, INTEGER,
 *          CATEGORICAL) and desc.dim_levels[], new fields at the end of
 *          yaep_desc that a binding must add. A categorical kernel factor
 *          exp(-(1 - delta) / l); candidates, init points, refinement,
 *          updates and threshold contexts honor the kinds. A description
 *          with every dimension CONTINUOUS gives v0.8.0's results to the bit.
 *          See DIMENSION KINDS under KERNELS.
 *   v0.8.0 - YAEP_LIK_PAIRWISE, comparisons of two stimuli, with
 *          yaep_next_pair(), yaep_update_pair() and yaep_predict_pair().
 *          yaep_trial gains x2 at its end and the handle a second stimulus
 *          array; a binding that mirrors yaep_trial must add the field.
 *          Nothing else changes. See MODEL and examples/aep/pairwise.c.
 *   v0.7.0 - optimization: YAEP_ACQ_UCB, YAEP_ACQ_EI and YAEP_ACQ_THOMPSON
 *          for finding the stimulus the observer prefers most, desc.minimize
 *          (a new field at the end of yaep_desc), and yaep_argmax(). See
 *          ACQUISITION and examples/aep/optimize.c. Nothing else changes.
 *   v0.6.0 - desc.priors: every weak-prior parameter at run time, a
 *          {center, sd, ceiling} per hyperparameter with zero meaning the
 *          measured default, and yaep_get_priors() to read what is in force.
 *          The compile-time YAEP__PS_MG_RISE, YAEP__PRIOR_MG_SD,
 *          YAEP__PS_MG_HI and YAEP__PS_OS_G are gone. yaep_desc gains
 *          priors at its end, so a binding that mirrors it must add the
 *          field; at the defaults every result is v0.5.0's to the bit.
 *   v0.5.0 - three defects the cross-method comparison
 *          (tests/compare/methods_compare.py) found, fixed, each with its
 *          recorded trials replayed in the test. (1) A psychometric-model fit
 *          could restore its accepted point from a rejected point's mode, run
 *          the mode search into its step cap on a saturated plateau, and leave
 *          a log marginal likelihood of -1e49 with no error: the mode search
 *          now rejects a result below Psi at the prior mean and restarts cold
 *          (YAEP_ERR_NUMERIC if that fails), a restore that misses its value
 *          is refitted cold, and a whole fit that ends worse than it began is
 *          undone. (2) A GP-model fit could jump to a data-favored
 *          short-lengthscale mode that redraws the level set: a fit that
 *          reclassifies more than 5% of the candidates after a settled one is
 *          undone once. (3) The GP model's mean had no prior and ran to its
 *          bound on data that are mostly one way: it now has a normal prior
 *          (sd 0.7) on the discrete likelihoods, and EAVC falls back to the
 *          straddle when its expected level set is empty or full. (2) and (3)
 *          change what the GP model computes by default; nothing changes in a
 *          run none of them touches except through the mean prior. See
 *          HYPERPARAMETERS and ACQUISITION.
 *   v0.4.1 - no behavior changed. Why YAEP_MODEL_PSYCHOMETRIC's fitted mean
 *          slope is about half the true one (its prior, not the Laplace
 *          approximation), and why that prior stays (every looser setting
 *          measured reopens the threshold tail under LSE and leaves the band
 *          error where it was), under HYPERPARAMETERS; and the prior's center,
 *          sd and ceiling as compile-time knobs for measuring it.
 *   v0.4.0 - three changes to YAEP_MODEL_PSYCHOMETRIC. Its hyperparameter
 *          gradient is analytic (one conjugate-gradient adjoint solve through
 *          the true Hessian, preconditioned by the Gauss-Newton posterior),
 *          which cuts the cost of a fit about five times. It supports EAVC and
 *          LOCALMI, through a rank-one look-ahead in the collapsed system. And
 *          the log-slope GP's output-scale prior is centered at 0.3 rather than
 *          1, which removes a 7 to 12 dB tail on the audiometric sensory
 *          phenotype. The last one changes what the model computes by default;
 *          nothing changes under YAEP_MODEL_GP. No public struct changed.
 *   v0.3.0 - desc.model and YAEP_MODEL_PSYCHOMETRIC: Keeley et al. 2023's
 *          semiparametric model in its own form, a threshold GP and a
 *          log-slope GP over the context feeding a probit or logit in the
 *          intensity, for the observer whose psychometric rise is narrow
 *          against the box (see MODEL, and STATUS for what it measured).
 *          yaep_hyper gains lengthscale_g, outputscale_g and mean_g, and
 *          yaep_desc gains model; both at the end of their structs, so every
 *          earlier field keeps its offset, but both structs are larger and a
 *          binding that mirrors them must add the fields. The default model
 *          is YAEP_MODEL_GP, and with it nothing computes differently from
 *          v0.2.1; YAEP_KERNEL_SEMIP is unchanged.
 *   v0.2.1 - version macros (YAEP_VERSION_MAJOR / MINOR / PATCH / STRING)
 *          and yaep_version(), so a program can log which header it was
 *          built from and a binding can read its version from the header;
 *          and desc.refine_steps, a golden-section refinement of the grid
 *          winner off the candidate set (see CANDIDATES). refine_steps is the
 *          LAST field of yaep_desc, so every earlier field keeps its offset,
 *          but the struct is larger and a binding that mirrors it must add the
 *          field. At its default of 0 nothing computes differently from v0.2.
 *   v0.2 - the optional async layer, YAEP_ASYNC: ysp/rt.h's yrt_pump with
 *          yaep_update(), yaep_next() and yaep_threshold() on one thread, a
 *          queue of responses, a published snapshot a frame loop polls, and
 *          desc.fit_in_idle, which spends the gaps between trials on
 *          yaep_fit_step(). Nothing changed in the synchronous header, which
 *          still includes nothing but the C standard library and is bit for bit
 *          what v0.1 computed; see ASYNC.
 *   v0.1 - the implementation. All four likelihoods, both kernels, all six
 *          acquisitions, the analytic marginal-likelihood gradient (numerical
 *          under CATEGORICAL), the threshold search, and five things v0.0 did
 *          not plan: yaep_fit_step(), which makes a hyperparameter fit
 *          resumable one evaluation at a time, and desc.refit_every with
 *          yaep_refit(), which trades the exact Newton refit of every trial
 *          for an O(N^2) rank-one update, and desc.guess / desc.lapse, a
 *          floor and a ceiling on the Bernoulli and ordinal links so a
 *          two-interval task states its chance level instead of faking it.
 *          YAEP_REAL makes the kernel matrix, the Laplace factors and the
 *          look-ahead covariance float on request; see PRECISION for what that
 *          buys, which is memory rather than time.
 *          It also fits a MAP rather than a type-II maximum likelihood: weak
 *          log-normal priors on the lengthscales and the output scales
 *          (desc.no_hyper_prior turns them off), which is what stops a nearly
 *          separable run from interpolating its own trials; yaep_next()
 *          guards against an acquisition that has stopped discriminating;
 *          and yaep_predict_p_many() / yaep_predict_f_many() evaluate a
 *          field in one blocked pass. Three changes to what
 *          v0.0 specified: EAVC measures the level set with the EXPECTED
 *          count of candidates above the level rather than the count of
 *          candidates whose mean is above it (the count is integer-valued,
 *          and it pins the acquisition to one stimulus as soon as no
 *          candidate can flip); the semiparametric kernel measures the
 *          intensity from the middle of the box; and the hyperparameter
 *          gradient under CATEGORICAL is central differences, not analytic.
 *   v0.0 - specification. Declarations and the manual, no implementation.
 *
 *   STATUS: v0.15.0. Implemented and checked by tests/adapt/aep_test.c,
 *   which builds and passes as C11, C99 and C++17 under gcc with -Wall
 *   -Wextra -Wpedantic -Wshadow -Werror, with and without YAEP_ASYNC, as
 *   MSVC's default C dialect with /W4 /WX both ways, and is clean under
 *   -fsanitize=address,undefined and, with the async layer, under
 *   -fsanitize=thread. v0.15.0 was checked on 2026-10-10 with MSVC 19.44
 *   /W4 /WX (C and C++17, with and without YAEP_ASYNC) and MinGW-w64 gcc
 *   -Werror; the new checks cover the messages the handle-changing calls
 *   leave and yaep_close() on a zeroed handle, after a failed open and
 *   twice. The sanitizer runs are v0.14.1's.
 *   What it verifies, with the numbers it measured: the Cholesky and the
 *   triangular solves against a hand-built SPD matrix (1e-12); the kernel
 *   symmetric and positive definite on random points for both kernels; the
 *   20-node Gauss-Hermite rule integrating x^0 to x^6 against the Gaussian
 *   to 1e-11; the exact Gaussian posterior, its variance and its log
 *   marginal likelihood against a dense Gauss-Jordan solve (1e-9); the
 *   Bernoulli probit Laplace mode satisfying f = m + K grad log p(y|f) at
 *   six points (1e-8) and its predictive variance against a dense
 *   (K^-1 + W)^-1 (1e-9); the softmax mode satisfying the same condition
 *   per class with three classes (1e-7); the analytic gradient of the
 *   Laplace log
 *   marginal likelihood against central differences for every free
 *   hyperparameter under RBF and SEMIP crossed with Bernoulli, ordinal and
 *   Gaussian, and again with the logit link, worst relative error below
 *   1e-4; the rank-one look-ahead
 *   covariance against a brute-force refit with W frozen (1e-10);
 *   yaep_memory_size() exact to the byte; the Halton points; the desc
 *   validation; a caller-owned candidate list, the random acquisition and
 *   the threshold-width stop rule running a session; yaep_predict_p_many()
 *   and yaep_predict_f_many() agreeing with the one-point calls to the last
 *   bit over a 900-point grid; and a reported
 *   Cholesky failure leaving the previous
 *   posterior in force. On a simulated 1-D probit observer (slope 8,
 *   50% point 0.40, box [0, 1]) over 8 response streams of 100 trials, the
 *   mean absolute error of the 75% threshold is 0.011 for LSE, 0.010 for
 *   EAVC and 0.037 for BALD, against a binomial noise floor near 0.02 for
 *   a hundred trials; the tolerance the test enforces is 0.08. Over 20
 *   further streams outside the test the same three come out at 0.013,
 *   0.013 and 0.032, worst case 0.05, 0.04 and 0.12, with no run failing to
 *   find a crossing. The other
 *   three likelihoods recover their simulated field: rms error 0.003 for
 *   GAUSSIAN over 60 trials, 0.076 for the ordinal P(y >= 2) with four
 *   categories over 150 trials, and 0.069 for the three softmax class
 *   probabilities over 120 trials.
 *
 *   The floor and the ceiling are checked the same way: the analytic
 *   gradient still matches central differences to better than 1e-4 with
 *   guess = 0.5 and lapse = 0.02 under both links, both kernels, and both
 *   Bernoulli and ordinal; and a simulated two-interval observer
 *   (p = 0.5 + 0.48 Phi(8 (x - 0.4))) recovers its 75% threshold with a mean
 *   absolute error of 0.022 over 5 streams of 120 trials.
 *
 *   The weak priors of HYPERPARAMETERS are in the same comparison: without
 *   them the same eight streams give 0.050 for LSE and 0.043 for BALD, and
 *   two of the eight collapse onto a diffuse fit whose threshold is 0.14
 *   out. The gradient check covers them, since they are part of the
 *   objective the finite differences see, and desc.no_hyper_prior is how the
 *   comparison was run.
 *
 *   examples/aep/audiometric.c is the harder test, and the numbers that justify
 *   the priors and the acquisition guard come from it: the audiometric observer
 *   of Owen et al. 2021, four phenotypes by 20 replications of 150 trials, same
 *   seeds throughout. Pooled over the phenotypes at trial 150, threshold error
 *   in dB at beta = 2 then beta = 0.5: LOCALMI 1.74 / 1.75, LSE 2.04 / 2.28,
 *   EAVC 2.14 / 2.03, BALV 3.76 / 3.40, BALD 5.66 / 5.28, RANDOM 5.42 / 5.45,
 *   SEMIP with BALV 3.41 / 3.46, SEMIP with LSE 8.05 / 8.14, and eight
 *   interleaved staircases given the frequencies and the psychometric width
 *   2.72 / 2.07. Surface error MAE(p): BALV 0.0558 / 0.0621, which is the best
 *   of the nine in all eight phenotype-by-beta cells, then EAVC 0.0668, BALD
 *   0.0672, LOCALMI 0.0733, RANDOM 0.0763, LSE 0.0824; the staircase reads
 *   0.0186 there, with beta known for free. So the level-set acquisitions win
 *   the threshold and the variance-based ones win the field, which is the
 *   paper's own finding, and a classical staircase that is told the frequencies
 *   and the width is still competitive on both.
 *
 *   That run is also the before-and-after for the two v0.1 fixes. The weak
 *   priors: LSE was 3.20 / 3.07 dB without them and BALD 8.48 / 8.91, against
 *   2.04 / 2.28 and 5.66 / 5.28 with them, and the output scale sat at a bound
 *   in 130 of 1600 runs rather than 1580, all 130 in the BALV, LOCALMI or
 *   beta = 0.5 cells where the latent really is steep. They cost early
 *   accuracy: at trial 25, MAE(p) is 0.03 to 0.11 worse with the prior than
 *   without. The acquisition guard: SEMIP with a level-set acquisition used to
 *   separate its own data and then propose one candidate forever, and its
 *   threshold contour left the box in 7 to 19 of 30 context columns in every
 *   cell; now no cell of the 24 has a single non-crossing column. Over all 1600
 *   runs, YAEP_ERR_NUMERIC 0 and a multi-crossing threshold 0. SEMIP with LSE
 *   at 8 dB is still the worst configuration measured, and the reason is in the
 *   model rather than the fit: see SEMIP below, and ACQUISITION for what the
 *   guard does and does not repair.
 *
 *   The two cost knobs are checked too: yaep_fit_step() run to completion
 *   lands on the same hyperparameters (1e-12) and the same log marginal
 *   likelihood (1e-9) as yaep_fit(), and desc.refit_every = 5 replayed against
 *   refit_every = 1 on one 78-trial response stream differs by at most
 *   1.7e-4 of probability, which yaep_refit() then erases to 6e-15.
 *
 *   The Laplace log marginal likelihood itself is checked against an
 *   independent dense evaluation of R&W eq. 3.32 (1e-8), which matters
 *   because it is the larger half of what the fit maximizes. The weak priors
 *   are the other half, and a fit that ran away in spite of them still shows
 *   up as a log marginal far closer to zero than the responses allow;
 *   HYPERPARAMETERS says how that looks and what else to do about it.
 *
 *   YAEP_MODEL_PSYCHOMETRIC is checked by the same test, under both links:
 *   the 2N-latent mode satisfies z - mean = K grad log p(y | z) to 4e-8 of
 *   the latent (the Newton tolerance is 1e-8); the predictive mean and
 *   covariance of (m, g) agree with a dense (K2^-1 + W2)^-1 over all 2N
 *   latents to 2e-10 and 2e-9; the Laplace log marginal likelihood agrees
 *   with a dense evaluation through log|K2| + log|K2^-1 + W2| to 1e-8; the
 *   quadrature is checked as MODEL says; the closed-form threshold agrees with
 *   a 40-node quadrature of the crossing to 3e-12; the analytic
 *   hyperparameter gradient agrees with central differences to 2e-8 to 2e-6
 *   over all six hyperparameters under probit and logit, with guess 0.5 and
 *   lapse 0.02, and over all eight with four ordinal outcomes (the mode
 *   converged to 1e-13 for that check, so the differences resolve it); the
 *   look-ahead's cross-covariance between two contexts agrees with the dense
 *   2N posterior to 3e-10, and its rank-one update with a dense update of the
 *   two contexts' joint 4 x 4 covariance to 2e-9; LOCALMI's candidate score
 *   equals its point score to the last bit; ordinal and two-interval sessions
 *   run clean; and on a 2-D observer with a curved threshold and a 7 dB rise
 *   the model's EAVC and LOCALMI find the threshold to 0.7 and 1.0 dB in 80
 *   trials.
 *   On examples/aep/audiometric.c, 20 replications, trial 150: field MAE(p) /
 *   band MAE(p) / threshold error in dB, the worst replication's threshold
 *   error, and ms per trial with fits every 20 trials. semip2 is this model,
 *   YAEP_MODEL_PSYCHOMETRIC, under the example's name for it:
 *
 *                      metabolic, beta 2              metabolic, beta 0.5
 *     RBF LSE       .077 .213 1.98 +- .32 3.9  3   .083 .298 2.49 +- .32 4.9  3
 *     RBF EAVC      .058 .204 1.98 +- .16 2.9  7   .056 .285 2.28 +- .15 2.7  7
 *     semip2-lse    .013 .160 1.41 +- .12 2.0  9   .013 .302 1.54 +- .05 1.8  9
 *     semip2-balv   .011 .153 1.28 +- .16 2.2 10   .011 .316 1.41 +- .08 1.9  8
 *     semip2-bald   .008 .129 1.08 +- .09 1.7 11   .011 .316 1.42 +- .09 2.0 10
 *     semip2-eavc   .011 .140 1.18 +- .11 2.0 24   .011 .264 1.25 +- .23 3.1 29
 *     semip2-lmi    .011 .152 1.25 +- .07 1.6 11   .012 .321 1.44 +- .07 1.8 10
 *     staircase     .017 .267 2.60 +- .22 3.5      .014 .322 2.06 +- .17 3.0
 *
 *                      sensory, beta 2                sensory, beta 0.5
 *     RBF LSE       .092 .249 1.77 +- .19 3.1  3   .092 .276 2.82 +- .21 3.2  3
 *     RBF EAVC      .069 .216 2.14 +- .16 2.9  7   .078 .200 2.33 +- .20 3.2  7
 *     semip2-lse    .016 .193 1.89 +- .20 2.8 11   .033 .230 2.89 +- .33 3.6  9
 *     semip2-balv   .009 .156 1.16 +- .22 2.1  8   .030 .287 3.23 +- .82 5.9  7
 *     semip2-bald   .007 .126 0.93 +- .14 1.6 11   .026 .295 3.18 +- .80 5.5 10
 *     semip2-eavc   .009 .149 1.18 +- .22 2.6 25   .022 .204 1.98 +- .50 4.3 26
 *     semip2-lmi    .013 .168 1.44 +- .20 2.2 12   .021 .240 2.05 +- .31 3.3 11
 *     staircase     .018 .277 2.49 +- .31 3.5      .014 .539 1.90 +- .09 2.4
 *
 *   On three of the four cells every semip2 method beats the RBF GP and the
 *   staircase (which is given the psychometric width) on threshold: BALD
 *   0.93 to 1.42 dB, EAVC 1.18 to 1.25, against 2.06 to 2.60 for the
 *   staircase and 1.77 to 2.49 for the RBF GP. The worst replication is 3.1
 *   dB or less there, and 5.9 dB over all four cells, where v0.3.0's was
 *   11.6. Sensory at beta 0.5, a 27 dB climb in two octaves with a 2.6 dB
 *   rise, is the hard cell: there EAVC (1.98 dB) and LOCALMI (2.05) match
 *   the staircase (1.90) and beat both RBF methods, and the per-point
 *   acquisitions do not (2.9 to 3.2). The field error is 3 to 10 times
 *   smaller than the RBF GP's everywhere and already 0.04 to 0.12 at trial
 *   25, against 0.22 to 0.28. The band error is under the RBF GP's at
 *   beta 2 (0.13 to 0.19 against 0.20 to 0.25); at beta 0.5 the band is about
 *   one grid point per column and measures the threshold's location, not the
 *   model's shape, and the staircase given the true width scores 0.32 and
 *   0.54 there. The cost is 8 to 11 ms a trial for the per-point
 *   acquisitions and about 25 for EAVC, against 3 and 7 for the RBF GP.
 *   BALD is the best of the per-point acquisitions on this model and EAVC
 *   the most robust overall.
 *
 *   The async layer of v0.2 is checked by the same test, in a YAEP_ASYNC
 *   block: a 60-trial session driven through yaep_async_submit() reproduces
 *   the synchronous session bit for bit (memcmp of the history and of the
 *   proposal stream, and the same log marginal likelihood and threshold to the
 *   last bit) with fit_in_idle off; with it on, the log marginal improved in an
 *   idle gap on 27 of 60 trials while the proposals kept up, and
 *   desc.fit_every was parked at 0 for the session and restored at stop; and a
 *   burst that outruns the thread gives YAEP_ERR_BUSY with nothing copied
 *   (8 accepted, 3 refused, 8 still pending at a stop that drained them). That
 *   test is clean under -fsanitize=thread with no warnings, and the rest of the
 *   test's output is byte-identical between the YAEP_ASYNC build and the plain
 *   one. examples/aep/async.c runs 200 EAVC trials through a 16 ms frame loop in
 *   11.2 s: with fit_in_idle on and a 225-candidate grid the proposal was ready
 *   by the next frame on 28 trials and late on 172, 2.4 frames of waiting on
 *   average and 5 at worst, the queue never filled, and the threshold at three
 *   contexts came out 0.007 to 0.060 from the truth.
 *
 *   desc.refine_steps is checked there too: the point score it climbs equals
 *   the candidate score to the last bit at every candidate for LSE, BALV,
 *   BALD and LOCALMI, including under CATEGORICAL; every refined proposal stays
 *   within one grid step of the winner it started from and never scores below
 *   it, under all five acquisitions; and the eight LSE streams above give a
 *   mean threshold error of 0.0111 with refine_steps = 2 against 0.0112
 *   without, which on a 65-point grid is what refinement should change: next
 *   to nothing. CANDIDATES has the audiometric numbers, where the grid is
 *   coarse and it matters.
 *
 *   The later additions are checked there as well, and each section says
 *   what it measured: PAIRWISE's trial kernel, mode and predictions against a
 *   dense 2N posterior; the mixed dimension kinds' kernel values, their
 *   analytic lengthscale gradient against differences (2e-5 at worst) and
 *   every proposal on its levels and integers; the monotonic projection
 *   against a brute force through yaep_predict_f(), to the bit; the
 *   conjugate-gradient Newton steps against factoring, to 4e-11; desc.fit_pcg's
 *   fits against the factored fits in lockstep, step by step, to 2e-9; and
 *   132 snapshot resumes against the uninterrupted sessions, byte for byte.
 *
 *   The float build of YAEP_REAL passes the same test with the tolerances
 *   PRECISION lists, measured rather than assumed, with YAEP_LIK_GAUSSIAN
 *   rejected at open and its checks skipped. That includes the psychometric
 *   model, whose float mode PRECISION describes, and whose analytic gradient
 *   the float test can check against differences only to 7e-2, since a float
 *   objective is too rough for differences to resolve more; on the 7 dB
 *   observer the float build's threshold error is 0.98 dB against the double
 *   build's 1.12, band error 0.143 against 0.141, same streams.
 *
 *   KNOWN TAILS. The cross-method comparison (tests/compare/methods_compare.py,
 *   streams seeded [20170310, problem, replication]) has no collapsed run
 *   left after v0.5.0, and three single replications that are poor for a
 *   reason the header does not fix:
 *   - csf6 (6-D), GP model, LSE, replication 5: MAE(p) 0.125 and 82 contexts
 *     of 118 with no crossing at trial 300 (0.077 and 57 at trial 500). No
 *     hyperparameter is at a bound (contrast lengthscale 1.35 in 0.075 to 3,
 *     output scale 0.61, mean 0.05). The cause is the problem's numbers: with
 *     guess 0.5 and no lapse, p at the prior mean is 0.75, the target, so in
 *     every context the data have not reached the column crosses or not on
 *     the sign of a mean near 0, and LSE in 6-D leaves most contexts
 *     unreached. Replication 4 of the same method has 25.
 *   - The psychometric model with LSE: metabolic+sensory beta 2 replication
 *     14 (9.5 dB) and older-normal beta 0.5 replication 10 (7.6 dB). The
 *     log-slope output scale is not at its ceiling (0.33 and 0.49 of 10). In
 *     both a fit shortens the threshold GP's frequency lengthscale (3.2 to
 *     0.55 at trial 65; 9.1 to 1.35 at trial 125, after the model had sat at
 *     1.15 dB for 40 trials), the defect-2 jump of the GP model, and LSE then
 *     puts 59 and 49 of its 150 trials in one frequency column. The level-set
 *     guard that stops the jump in the GP model does not run for the
 *     psychometric model; extended to it as a trial, it sees 4.8% of the
 *     candidates flip at the trial-125 fit, under its 5% trigger, so it would
 *     take retuning to one run to catch it, and that was not done. EAVC on
 *     the same streams ends at 0.85 and 0.93 dB.
 *   - The 2-D CSF, GP model, EAVC, replication 12: 7.0 dB. The error is all in
 *     the highest-frequency columns, whose threshold (-5 dB) is at the top of
 *     the box: the posterior mean falls and rises again along intensity there,
 *     and the first crossing is at -42 dB. desc.monotone_dims on the
 *     intensity removes it (1.9 dB); over the 20 replications it takes the
 *     mean threshold error from 1.65 to 1.58 dB and the worst from 6.99 to
 *     2.51, but MAE(p) from 0.086 to 0.092, and five replications get worse
 *     (three by 1 to 1.5 dB), which is why the projection stays opt-in.
 *
 *   COMPARED with AEPsych 0.8.0 through the Python binding, in one process
 *   on one response stream (tests/compare/compare_gp_aepsych.py): the
 *   metabolic phenotype of examples/aep/audiometric.c at beta 2, 10
 *   replications, both libraries given the same Sobol init points. AEPsych
 *   runs its variational GP and its own LSE, EAVC and BALV, optimized over
 *   the box. Threshold error at trial 150, this header's GP model against
 *   AEPsych: EAVC 2.09 against 2.23 dB (inside the replication error), BALV
 *   4.45 against 7.10, LSE 2.06 against 1.29. The LSE gap is the 11 x 21
 *   candidate grid: with refine_steps = 2 LSE reaches 1.44 +- 0.16 dB and
 *   EAVC 1.44 +- 0.28 (CANDIDATES). The field error MAE(p) is about half of
 *   AEPsych's for every acquisition at 100 and 150 trials. The psychometric
 *   model with EAVC reaches 1.26 +- 0.09 dB and a field error of 0.012,
 *   against AEPsych's best of 1.29 dB and 0.18. Fitted from scratch to the
 *   same trials, with AEPsych's output scale fitted (a ScaleKernel) instead
 *   of fixed at 1, the two GP models agree: the fields differ by 0.016 and
 *   the threshold curves by 0.14 dB. The cost per trial is 6 to 13 ms here
 *   against 4 to 8 s for AEPsych, which refits every trial; both ran on one
 *   loaded machine, so the ratio is an order of magnitude, not a number.
 *   On human data (tests/compare/compare_gp_csfdata.py: Letham et al.
 *   2022's 1001-trial 6-D contrast sensitivity set, 5-fold
 *   cross-validation) the held-out log loss is 0.537 to 0.541 for both
 *   models here, 0.529 for AEPsych and 0.543 for logistic regression, all
 *   inside the fold noise. docs/adapt.md,
 *   "Verification", and docs/adapt_comparison.md have the details.
 *
 *   NOT verified: the Laplace band's 95% coverage is unmeasured, and the
 *   categorical one-against-the-rest acquisitions have no Monte Carlo check.
 *   The timings vary by tens of percent between runs of the same binary, so
 *   the frame-budget trial counts are an order of magnitude and not a
 *   specification (examples/aep/bench.c prints its own numbers; MEMORY, COST
 *   AND THREADS quotes them), and the async layer's late counts are a
 *   property of that machine and that grid, not of the header. No
 *   platform other than x86-64 has been built or run. The async layer has run
 *   on Linux (pthreads) and on Windows (MSVC, native threads, and the OS
 *   granted the below-normal drop); ysp/rt.h's macOS path is untested here.
 *   gcc 13 has not built this header here: the fix for its
 *   -Wstringop-overflow report at -O3 is by construction (no memcpy takes a
 *   size derived from an int), and clang 18 (zig cc 0.13) builds the test,
 *   the examples and both compile units clean with -Werror.
 *
 *   ---------------------------------------------------------------------
 *   LICENSE: public domain / MIT-0, see end of file.
 */
#ifndef YSP_AEP_H_INCLUDED
#define YSP_AEP_H_INCLUDED

/* The version of this header, for a log line or a compile-time check. The
 * string is the three numbers, and the test asserts that it stays so. */
#define YAEP_VERSION_MAJOR  0
#define YAEP_VERSION_MINOR  15
#define YAEP_VERSION_PATCH  0
#define YAEP_VERSION_STRING "0.15.0"

/* The one optional dependency, and it comes FIRST: ysp/rt.h sets a
 * feature-test macro for the Linux clock calls and can only do that before the
 * first system header. Without YAEP_ASYNC this header includes nothing but the
 * C standard library, which is what the rest of the manual assumes; with it,
 * ysp/aep.h gains the async layer and ysp/rt.h comes with it. See ASYNC and
 * BUILDING. */
#ifdef YAEP_ASYNC
    #ifdef YRT_NO_THREADS
        #error "ysp/aep.h: YAEP_ASYNC is a thread, and YRT_NO_THREADS removes threads. Define one or the other, not both."
    #endif
    #include "ysp/rt.h"
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef YAEP_API
#define YAEP_API extern
#endif

/* The element type of the matrices that dominate the cost: the kernel matrix,
 * the Laplace factors, the gradient workspace and the look-ahead covariance.
 * Define YAEP_REAL to float before the include to halve their memory and the
 * traffic through it; every vector, every accumulation and every scalar stays
 * double either way. Define it the same in every translation unit, because it
 * changes the layout of yaep_gp. See PRECISION in the manual for what the
 * float build costs and what it does not support. */
#ifndef YAEP_REAL
#define YAEP_REAL double
#endif
typedef YAEP_REAL yaep_real;

#ifndef YAEP_MAX_DIMS
#define YAEP_MAX_DIMS 8
#endif
#ifndef YAEP_MAX_OUTCOMES
#define YAEP_MAX_OUTCOMES 8
#endif
#ifndef YAEP_MAX_TRIALS
#define YAEP_MAX_TRIALS 512
#endif
#define YAEP_QUAD_N 20   /* Gauss-Hermite nodes for expectations over f */

/* --- codes ------------------------------------------------------------- */

#define YAEP_OK            0
#define YAEP_ERR_ARG      (-1)
#define YAEP_ERR_CLOSED   (-2)
#define YAEP_ERR_FULL     (-3)
#define YAEP_ERR_MEMORY   (-4)
#define YAEP_ERR_NOCROSS  (-5)  /* no level-set crossing inside the box    */
#define YAEP_ERR_NUMERIC  (-6)  /* Cholesky failed; posterior unchanged    */

/* Static description of a YAEP_ERR_* code ("ok" for values >= 0). */
YAEP_API const char* yaep_strerror(int code);

/* YAEP_VERSION_STRING, as compiled into the implementation. Beside the macro,
 * which is what the CALLER was compiled against, it tells a program that links
 * a prebuilt implementation which one it got. */
YAEP_API const char* yaep_version(void);

/* --- description ------------------------------------------------------- */

typedef enum yaep_lik {
    YAEP_LIK_BERNOULLI = 0, /* y in {0, 1}, p = link(f) (default)          */
    YAEP_LIK_ORDINAL,       /* y in 0..K-1 ordered, K - 1 cutpoints        */
    YAEP_LIK_CATEGORICAL,   /* y in 0..K-1 unordered, K latents, softmax   */
    YAEP_LIK_GAUSSIAN,      /* y real, y = f + noise                       */
    YAEP_LIK_PAIRWISE       /* a trial is two stimuli; y = 1 if the first
                              * was preferred, p = link(f(x1) - f(x2)).
                              * yaep_next_pair() / yaep_update_pair()    */
} yaep_lik;

typedef enum yaep_kernel {
    YAEP_KERNEL_RBF = 0,  /* ARD squared exponential (default)            */
    YAEP_KERNEL_SEMIP     /* a(c) + b(c) x along intensity_dim            */
} yaep_kernel;

typedef enum yaep_link {
    YAEP_LINK_PROBIT = 0, /* p = Phi(f) (default)                          */
    YAEP_LINK_LOGIT       /* p = 1 / (1 + exp(-f))                         */
} yaep_link;

/* What the latent is. YAEP_MODEL_GP is one GP over the whole box, shaped by
 * desc.kernel. YAEP_MODEL_PSYCHOMETRIC is a psychometric function whose
 * threshold m(c) and log-slope g(c) are two GPs over the context c (every
 * dimension but intensity_dim): f = exp(g(c)) (x - m(c)). See MODEL. */
typedef enum yaep_model {
    YAEP_MODEL_GP = 0,          /* one GP over the box (default)            */
    YAEP_MODEL_PSYCHOMETRIC     /* threshold and slope GPs over the context */
} yaep_model;

/* What a dimension's values are; see KERNELS. The intensity dimension is
 * always CONTINUOUS. */
typedef enum yaep_dim_kind {
    YAEP_DIM_CONTINUOUS = 0,    /* any value in [lo, hi] (default)         */
    YAEP_DIM_INTEGER,           /* the integers in [lo, hi]; lo and hi are
                                  * integers                                */
    YAEP_DIM_CATEGORICAL        /* unordered levels 0 .. dim_levels - 1;
                                  * lo = 0, hi = dim_levels - 1             */
} yaep_dim_kind;

typedef enum yaep_acq {
    YAEP_ACQ_LSE = 0,     /* level-set straddle (default)                  */
    YAEP_ACQ_EAVC,        /* look-ahead expected absolute volume change    */
    YAEP_ACQ_LOCALMI,     /* look-ahead local mutual information           */
    YAEP_ACQ_BALV,        /* posterior variance of the target quantity     */
    YAEP_ACQ_BALD,        /* outcome / latent mutual information           */
    YAEP_ACQ_RANDOM,      /* Halton, or desc.rng, every trial              */
    YAEP_ACQ_UCB,         /* optimization: E[q] + beta sd[q]               */
    YAEP_ACQ_EI,          /* optimization: expected improvement of q       */
    YAEP_ACQ_THOMPSON     /* optimization: argmax of one posterior sample;
                            * needs desc.rng. GP model only                 */
} yaep_acq;

typedef enum yaep_stop {
    YAEP_STOP_NONE = 0,
    YAEP_STOP_TRIALS,
    YAEP_STOP_THRESHOLD_SD,
    YAEP_STOP_FULL
} yaep_stop;

/* A uniform variate in [0, 1) from the caller's generator. Optional; see
 * SIMULATION for what the header does with it. */
typedef double (*yaep_rng_fn)(void* ctx);

/* Model hyperparameters. A zero field means "default, or fit when
 * desc.fit is on"; see HYPERPARAMETERS. The `_b` fields apply to the slope
 * GP under YAEP_KERNEL_SEMIP only, and lengthscale[intensity_dim] is
 * ignored there. cutpoint[] is ORDINAL only (K - 1 entries, increasing);
 * noise_sd is GAUSSIAN only. */
typedef struct yaep_hyper {
    double lengthscale[YAEP_MAX_DIMS];
    double outputscale;
    double mean;
    double lengthscale_b[YAEP_MAX_DIMS];
    double outputscale_b;
    double cutpoint[YAEP_MAX_OUTCOMES - 1];
    double noise_sd;
    double lengthscale_g[YAEP_MAX_DIMS]; /* YAEP_MODEL_PSYCHOMETRIC only: */
    double outputscale_g;          /* the log-slope GP g(c). The threshold  */
    double mean_g;                 /* GP m(c) uses lengthscale, outputscale,
                                    * mean                                  */
} yaep_hyper;

/* One weak hyperparameter prior. Zero in any field means the measured
 * default (HYPERPARAMETERS has each one and where it was measured); a
 * NEGATIVE sd turns that one prior off. For a scale (lengthscale, output
 * scale, noise) the prior is log-normal: center is the scale's median and sd
 * the sd of its logarithm. For a mean it is normal: center and sd in the
 * latent's units. */
typedef struct yaep_prior {
    double center;
    double sd;
    double ceiling;            /* mean_g only: the steepest mean slope
                                * allowed, as a rise fraction of the
                                * intensity axis (see mean_g below)        */
} yaep_prior;

/* The weak priors of HYPERPARAMETERS, all of them. desc.no_hyper_prior is the
 * master switch and turns every one off. */
typedef struct yaep_priors {
    yaep_prior lengthscale;   /* center as a fraction of (hi - lo) of its
                                * dimension, for every lengthscale set
                                * (the GP's, SEMIP's slope GP's, and the
                                * psychometric model's two). 0.25, sd 1   */
    yaep_prior outputscale;   /* 1, sd 0.7; under PSYCHOMETRIC the threshold
                                * GP's, center (span / 4)^2 in squared
                                * intensity units                          */
    yaep_prior outputscale_b; /* SEMIP's slope GP. 1, sd 0.7              */
    yaep_prior outputscale_g; /* PSYCHOMETRIC's log-slope GP. 0.3, sd 0.7;
                                * its center is also outputscale_g's
                                * default value                            */
    yaep_prior mean;          /* the GP model's mean on a discrete
                                * likelihood, normal. 0, sd 0.7. None on
                                * GAUSSIAN's mean or the psychometric
                                * model's threshold mean                   */
    yaep_prior mean_g;        /* PSYCHOMETRIC's mean log-slope, normal in
                                * log(slope). center as a RISE fraction of
                                * the intensity axis: the prior's slope is
                                * 1 / (center * span), so 0.25 (the
                                * default) is a rise a quarter of the axis
                                * wide. sd 1. ceiling: the fit's upper
                                * bound, the same way, 1 / 256; it is also
                                * the default of hyper_max.mean_g           */
    yaep_prior noise_sd;      /* GAUSSIAN's noise sd. Off by default (no
                                * prior was ever measured to be needed);
                                * set center and sd to turn it on          */
} yaep_priors;

/* Run description. Zero-initialize it and set only what you need.
 * Required: n_dims in [1, YAEP_MAX_DIMS], lo[d] < hi[d] for each, and at
 * least one stop criterion. ORDINAL and CATEGORICAL need n_outcomes >= 2
 * (>= 3 for CATEGORICAL). A level-set acquisition needs target_p (or
 * target_value under GAUSSIAN). */
typedef struct yaep_desc {
    int          n_dims;
    double       lo[YAEP_MAX_DIMS];
    double       hi[YAEP_MAX_DIMS];
    int          intensity_dim;   /* the dimension thresholds are along;
                                   * SEMIP's x. Default 0.                  */
    yaep_lik    lik;             /* BERNOULLI (0) by default               */
    int          n_outcomes;      /* K; ORDINAL / CATEGORICAL only          */
    yaep_kernel kernel;
    yaep_link   link;
    double       guess;           /* gamma: the floor on p, e.g. 0.5 for a
                                   * two-interval task. BERNOULLI and
                                   * ORDINAL only; fixed, never fit         */
    double       lapse;           /* lambda: the gap below 1                */
    yaep_hyper  hyper;           /* fixed values, or 0 = default / fit     */
    bool         fit;             /* fit the zero hyperparameters           */
    int          fit_every;       /* refit every this many trials after
                                   * n_init; 0 = only on yaep_fit()        */
    bool         no_hyper_prior;  /* drop the weak priors of HYPERPARAMETERS
                                   * and fit type-II maximum likelihood       */
    int          refit_every;     /* 0 or 1 = an exact Newton refit on every
                                   * update; k > 1 = an O(N^2) rank-one
                                   * update per trial and an exact refit
                                   * every k trials. BERNOULLI and ORDINAL
                                   * only; see MEMORY, COST AND THREADS     */
    yaep_hyper  hyper_min;       /* fit bounds; 0 = 0.05 (hi - lo) for
                                   * lengthscales, 0.1 for output scales,
                                   * -5 for the mean, 0.01 for a cutpoint
                                   * gap, 1e-3 for noise_sd                 */
    yaep_hyper  hyper_max;       /* 0 = 2 (hi - lo), 10, +5, 10, 10        */
    double       jitter;          /* added to the kernel diagonal for the
                                   * Cholesky; 0 = 1e-6                     */
    yaep_acq    acq;
    double       target_p;        /* level-set probability (discrete liks) */
    double       target_value;    /* level-set value (GAUSSIAN)             */
    int          target_outcome;  /* ORDINAL: y >= this; CATEGORICAL: y ==
                                   * this. Default 1.                       */
    double       acq_beta;        /* LSE straddle width; 0 = 1.96           */
    int          n_init;          /* Halton trials before the acquisition
                                   * runs; 0 = 2 * n_dims + 2               */
    yaep_rng_fn rng;             /* optional generator: ties, RANDOM acq   */
    void*        rng_ctx;
    const double* candidates;     /* M x n_dims, caller-owned, or NULL; it
                                   * must outlive the handle                */
    int          n_candidates;    /* M for candidates / Halton; 0 = 512     */
    int          grid[YAEP_MAX_DIMS]; /* product grid instead; 0 = unused,
                                        * else every dimension needs >= 1   */
    int          stop_trials;     /* 0 = off                                */
    double       stop_threshold_sd; /* 0 = off                              */
    double       stop_context[YAEP_MAX_DIMS]; /* context for the above     */
    int          max_trials;      /* 0 = YAEP_MAX_TRIALS                   */
    void*        memory;          /* caller's buffer, or NULL for the
                                   * allocator                              */
    size_t       memory_size;
    int          refine_steps;    /* rounds of golden-section refinement of
                                   * the grid winner; 0 = off. See
                                   * CANDIDATES. Last in the struct so the
                                   * fields before it keep their offsets.   */
    yaep_model  model;           /* GP (0) by default; see MODEL           */
    yaep_priors priors;          /* 0 = the measured defaults; see
                                   * HYPERPARAMETERS                        */
    bool         minimize;        /* the optimization acquisitions and
                                   * yaep_argmax() minimize the target
                                   * quantity instead of maximizing it     */
    yaep_dim_kind dim_kind[YAEP_MAX_DIMS]; /* CONTINUOUS (0) by default;
                                   * see KERNELS                            */
    int          dim_levels[YAEP_MAX_DIMS]; /* a CATEGORICAL dimension's
                                   * level count, at least 2                */
    unsigned     monotone_dims;   /* bit d set: the posterior mean is
                                   * projected to increase along dimension
                                   * d; see MONOTONIC PROJECTION           */
    int          pcg_threshold;   /* above this many trials an update's
                                   * Newton steps solve by conjugate
                                   * gradients; 0 = 512, negative = never.
                                   * See MEMORY, COST AND THREADS          */
    int          fit_max_evals;   /* a fit's objective evaluations; 0 = 40 */
    double       fit_tol;         /* a fit has converged when no gradient
                                   * component of its objective exceeds
                                   * this; 0 = 1e-8. See HYPERPARAMETERS   */
    bool         fit_pcg;         /* conjugate-gradient Newton steps in a
                                   * fit's evaluations and in the
                                   * psychometric model's mode search; off
                                   * by default. See MEMORY, COST AND
                                   * THREADS                               */
} yaep_desc;

/* --- handle ------------------------------------------------------------ */

typedef struct yaep_trial {
    double  x[YAEP_MAX_DIMS];
    double  y;          /* the outcome index, or the value under GAUSSIAN   */
    uint8_t proposed;   /* 1 when x is exactly what yaep_next() proposed  */
    uint8_t init;       /* 1 when the trial fell in the init phase          */
    double  x2[YAEP_MAX_DIMS]; /* PAIRWISE: the second stimulus            */
} yaep_trial;

/* Handle. The caller allocates it and treats every field as opaque. Must
 * be zeroed or closed before yaep_open(). */
typedef struct yaep_gp {
    yaep_desc  desc;
    yaep_hyper hyper;         /* the values in force (fit or default)     */
    int         N;             /* trials so far                            */
    int         N_fit;         /* trials the factor in L covers: N, or less
                                * after YAEP_ERR_NUMERIC                  */
    int         N_max;
    int         M;             /* candidates                               */
    int         K;             /* latents: 1, or n_outcomes if CATEGORICAL */
    void*       mem;
    size_t      mem_size;
    bool        mem_owned;
    double*     X;             /* N_max x n_dims                           */
    double*     X2;            /* the same for the second stimulus of a
                                * PAIRWISE trial                           */
    double*     y;             /* N_max                                    */
    yaep_real* Kmat;          /* N_max x N_max kernel matrix; the
                                * threshold GP's under PSYCHOMETRIC       */
    yaep_real* Kg;            /* the log-slope GP's, PSYCHOMETRIC only    */
    double      fit_flip;      /* level-set change of the last accepted
                                * fit, a fraction of M; -1 before one      */
    bool        fit_blocked;   /* the last fit was undone by the guard     */
    yaep_priors priors;       /* desc.priors with the defaults filled in */
    double      opt_best;      /* EI's incumbent: the best signed posterior
                                * mean of q at a trial so far               */
    bool        mixed;         /* some dimension is not CONTINUOUS         */
    double      ps_tol;        /* its Newton tolerance, YAEP__PS_TOL; a
                                * field so a gradient check can converge one
                                * handle's mode further than a session needs */
    yaep_real* L;             /* K x N_max x N_max Laplace factors, or the
                                * growing kernel Cholesky under GAUSSIAN   */
    double*     f;             /* K x N_max latent mode                    */
    double*     W;             /* K x N_max Hessian diagonal               */
    double*     grad;          /* K x N_max d log p(y|f) / df at the mode  */
    double*     cand;          /* M x n_dims, generated candidates         */
    double*     cand_mu;       /* K x M latent means at the candidates     */
    yaep_real* cand_cov;      /* M x M latent covariance, look-ahead only */
    double*     scratch;       /* Newton, gradient and prediction space    */
    double      log_marginal;
    int         halton_index;
    int         proposed;      /* candidate index of the last next(), -1  */
    int         last_index;    /* the candidate the last next() proposed   */
    int         repeats;       /* how many trials in a row it has won      */
    int         fit_state;     /* yaep_fit_step()'s place in a fit        */
    int         fit_evals;     /* objective evaluations spent in it        */
    int         fit_back;      /* consecutive line-search backtracks       */
    double      fit_step;      /* the line search's current step           */
    double      fit_best;      /* the best log marginal it has reached     */
    bool        fit_valid;     /* the Laplace state matches X, y, hyper    */
    bool        cand_valid;    /* cand_mu / cand_cov match the fit         */
    bool        multi_cross;   /* last threshold search crossed > once     */
    yaep_stop  stop;
    bool        open;
    yaep_trial* history;      /* N_max trials, in the memory block        */
    double      fit_delta;     /* the objective's change over the last
                                * whole fit, as applied                    */
    int         fit_conv;      /* how yaep_fit_step() last ended: 1 at a
                                * stationary point, 0 out of budget        */
    bool        fit_resume;    /* this fit continues one the budget cut    */
    int         fit_th_n;      /* desc.fit_pcg: how many hyperparameters the
                                * last fit evaluation had (its theta is in
                                * the stash block), 0 when unknown         */
    bool        stash_ok;      /* desc.fit_pcg: the stash holds the state
                                * at the fit's current point              */
    double      stash_val;     /* its objective                            */
    double      stash_lm;      /* its log marginal likelihood              */
    yaep_hyper stash_hyper;   /* its hyperparameters                      */
    int         pcg_iters;     /* conjugate-gradient iterations the last
                                * update's Newton steps took; 0 when they
                                * factored                                 */
    char        error[256];
} yaep_gp;

/* --- lifecycle --------------------------------------------------------- */

/* Bytes yaep_open() needs for `desc`, or 0 when the desc is invalid. */
YAEP_API size_t yaep_memory_size(const yaep_desc* desc);

/* Validate `desc`, take memory, generate the candidate set. Returns false
 * with yaep_error() set on failure; clears the message on entry. */
YAEP_API bool yaep_open(yaep_gp* g, const yaep_desc* desc);

/* Release what open() allocated and zero the handle. Safe on a zeroed
 * handle, after a failed open and twice. */
YAEP_API void yaep_close(yaep_gp* g);

/* The message of the last failed open or handle-changing call ("" if none).
 * See RETURN VALUES AND ERRORS. */
YAEP_API const char* yaep_error(const yaep_gp* g);
YAEP_API bool        yaep_is_open(const yaep_gp* g);

/* --- the loop ---------------------------------------------------------- */

/* Fill x[n_dims] with the next stimulus: a Halton point during the init
 * phase, else the best candidate under desc.acq, refined off the candidate set
 * when desc.refine_steps asks for it. Returns the candidate index (>= 0), or
 * -1 for a Halton, rng or refined point, or a YAEP_ERR_* below -1 (a bad
 * argument is reported as YAEP_ERR_CLOSED, since -1 is taken).
 * Calling it twice without an update returns the same point. yaep_next_subset()
 * never refines, since the caller asked for one of the listed candidates. */
YAEP_API int yaep_next(yaep_gp* g, double* x);

/* YAEP_LIK_PAIRWISE's yaep_next(): the pair to show, first and second.
 * During the init phase and under RANDOM, two Halton points; under
 * YAEP_ACQ_BALD or BALV, the candidate with the best posterior mean utility
 * against the candidate whose comparison with it scores highest; under
 * THOMPSON, the argmaxes of two posterior samples. Returns 0, or a
 * YAEP_ERR_* below it. */
YAEP_API int yaep_next_pair(yaep_gp* g, double* x1, double* x2);

/* yaep_next() restricted to the `n` candidate indices in `subset`. Skips
 * the init phase. */
YAEP_API int yaep_next_subset(yaep_gp* g, const int* subset, int n, double* x);

/* The acquisition score of candidate `index`, for a plot. The look-ahead
 * scores are defined on candidates only, which is why this takes an
 * index and not a point. NaN during the init phase or on a bad argument. */
YAEP_API double yaep_acq_score(const yaep_gp* g, int index);

/* Record outcome `outcome` in 0..K-1 at stimulus x[n_dims], refit the
 * Laplace posterior, and refit hyperparameters when the schedule says so.
 * Returns 0, or a negative YAEP_ERR_*; on YAEP_ERR_NUMERIC the trial is
 * recorded and the previous posterior stays in force. YAEP_ERR_ARG under
 * YAEP_LIK_GAUSSIAN. */
YAEP_API int yaep_update(yaep_gp* g, const double* x, int outcome);

/* The same for a continuous outcome under YAEP_LIK_GAUSSIAN. */
YAEP_API int yaep_update_real(yaep_gp* g, const double* x, double y);
/* YAEP_LIK_PAIRWISE: x1 and x2 were compared, and outcome is 1 if x1 was
 * preferred, 0 if x2 was. */
YAEP_API int yaep_update_pair(yaep_gp* g, const double* x1, const double* x2,
                                int outcome);
/* YAEP_LIK_PAIRWISE: P(x1 is preferred to x2) under the posterior. The
 * utility itself is yaep_predict_f(g, x, 0, ...) and its best stimulus is
 * yaep_argmax(). */
YAEP_API double yaep_predict_pair(const yaep_gp* g, const double* x1,
                                    const double* x2);

YAEP_API bool       yaep_done(const yaep_gp* g);
YAEP_API yaep_stop yaep_stop_reason(const yaep_gp* g);

/* --- estimates --------------------------------------------------------- */

/* Latent `k`'s posterior mean and sd at x (k = 0 except CATEGORICAL).
 * Returns 0 or YAEP_ERR_*. */
YAEP_API int yaep_predict_f(const yaep_gp* g, const double* x, int k, double* mu, double* sd);

/* The target quantity at x under the posterior (see ESTIMATES); NaN on a
 * bad argument. */
YAEP_API double yaep_predict_p(const yaep_gp* g, const double* x);

/* The same for `n` points at once, xs being n * n_dims coordinates and p an
 * n-element output. One blocked pass over the factor instead of n passes, which
 * is what makes a field plot per trial affordable: see ESTIMATES. Returns 0 or
 * YAEP_ERR_*; yaep_predict_f_many() is the latent underneath, with k = 0
 * except under CATEGORICAL, and either output pointer may be NULL. */
YAEP_API int yaep_predict_p_many(const yaep_gp* g, const double* xs, int n,
                                   double* p);
YAEP_API int yaep_predict_f_many(const yaep_gp* g, const double* xs, int n,
                                   int k, double* mu, double* sd);

/* Posterior variance of the target quantity at x, by quadrature; the BALV
 * score. */
YAEP_API double yaep_predict_p_var(const yaep_gp* g, const double* x);

/* All K outcome probabilities at x under the posterior (2 for BERNOULLI;
 * YAEP_ERR_ARG under GAUSSIAN, where yaep_predict_f is the answer). */
YAEP_API int yaep_predict_outcomes(const yaep_gp* g, const double* x, double* p);

/* Threshold along the intensity dimension at context ctx[n_dims - 1] (the
 * coordinates of every other dimension, in order; NULL when n_dims == 1).
 * `target` = 0 uses the desc's target. Writes the crossing of
 * yaep_predict_p() to *x, and the crossings of the +-1.96 sd latent
 * curves to *lo and *hi (either may be NULL). Returns 0,
 * YAEP_ERR_NOCROSS, or another YAEP_ERR_*. */
YAEP_API int  yaep_threshold(const yaep_gp* g, const double* ctx, double target,
                               double* x, double* lo, double* hi);
YAEP_API bool yaep_threshold_multi_cross(const yaep_gp* g);

/* --- hyperparameters --------------------------------------------------- */

/* One step of the fit above, for a caller that cannot give it a whole
 * inter-trial interval: at most two Laplace refits, one of them with the
 * gradient. Returns 1 while another step would help, 0 when the fit has
 * converged or run out of its evaluation budget, or a negative YAEP_ERR_*.
 * The hyperparameters in force after a call are always the best found so far,
 * so estimates read between steps are consistent. yaep_update() discards the
 * state, since the data it was fitting have changed. */
YAEP_API int yaep_fit_step(yaep_gp* g);

/* Force the exact Newton refit that desc.refit_every may be skipping. Does
 * nothing under GAUSSIAN or CATEGORICAL, which never skip one. Returns 0 or
 * YAEP_ERR_*. */
YAEP_API int yaep_refit(yaep_gp* g);

/* Fit the hyperparameters left zero in desc.hyper, from the values in force;
 * see HYPERPARAMETERS. Works whether or not desc.fit is on. Returns 1 when
 * the fit converged (no gradient component above desc.fit_tol, a step that
 * can no longer move, or nothing to fit: fewer than two trials or no free
 * field), 0 when it spent desc.fit_max_evals evaluations first or a guard
 * undid it (call it again to continue from where it stopped), or
 * YAEP_ERR_*. yaep_fit_delta() is the objective's change. Expensive; see
 * MEMORY, COST AND THREADS. */
YAEP_API int yaep_fit(yaep_gp* g);

/* The change in the fit objective (log marginal plus log prior) over the last
 * whole fit, yaep_fit() or a scheduled one, as applied: 0 when a guard undid
 * it. Repeating yaep_fit() until this is small is how to converge a fit that
 * the evaluation budget stops. NAN on a closed handle. */
YAEP_API double yaep_fit_delta(const yaep_gp* g);

/* The hyperparameters in force, and the log marginal likelihood of the
 * current fit (Laplace-approximate, or exact under GAUSSIAN). */
YAEP_API int    yaep_get_hyper(const yaep_gp* g, yaep_hyper* out);
/* The stimulus the posterior mean of the target quantity is highest at
 * (lowest under desc.minimize): the best candidate, refined by
 * desc.refine_steps as yaep_next() refines. Fills x[n_dims] and, if value is
 * not NULL, the posterior mean of the target quantity there. Returns the
 * candidate index, -1 for a refined point, or a YAEP_ERR_* below -1. The
 * optimization counterpart of yaep_threshold(). */
YAEP_API int    yaep_argmax(yaep_gp* g, double* x, double* value);

/* The priors in force: desc.priors with every zero replaced by its default
 * for this model and box, and a prior that is off (negative sd, or
 * desc.no_hyper_prior) reported with sd -1. */
YAEP_API int    yaep_get_priors(const yaep_gp* g, yaep_priors* out);
YAEP_API double yaep_log_marginal(const yaep_gp* g);

/* --- candidates and history -------------------------------------------- */

YAEP_API int yaep_n_candidates(const yaep_gp* g);
YAEP_API int yaep_candidate(const yaep_gp* g, int index, double* x);

YAEP_API int                yaep_n_trials(const yaep_gp* g);
YAEP_API const yaep_trial* yaep_history(const yaep_gp* g, int* n);

/* --- snapshots --------------------------------------------------------- */

/* Snapshot: bytes needed (0 on a closed handle); write them (returns the
 * bytes written, or YAEP_ERR_ARG when cap is too small); and rebuild an open
 * handle from them. `desc` re-supplies the pointers (rng and rng_ctx,
 * candidates, memory) and must agree with the snapshot on every number; a
 * mismatch, a wrong magic, format or YAEP_REAL, or a truncated or corrupt
 * snapshot fails the load with a message in yaep_error() and leaves the
 * handle closed. yaep_open_saved() opens, so close an open handle first and
 * close it when done. SNAPSHOTS gives the layout. */
YAEP_API size_t yaep_save_size(const yaep_gp* g);
YAEP_API int    yaep_save(const yaep_gp* g, void* buf, size_t cap);
YAEP_API bool   yaep_open_saved(yaep_gp* g, const yaep_desc* desc, const void* buf,
                            size_t len);

/* --- simulation -------------------------------------------------------- */

/* Draw an outcome from the K probabilities p[0..K-1] with the uniform
 * variate u in [0, 1): the smallest k with cumulative p > u. */
YAEP_API int yaep_simulate_outcome(const double* p, int K, double u);

/* -----------------------------------------------------------------------
 *  ASYNC
 *  Compiled only under YAEP_ASYNC. See ASYNC in the manual.
 * ----------------------------------------------------------------------- */
#ifdef YAEP_ASYNC

/* Two more codes, which only the async calls return. */
#define YAEP_ERR_BUSY    (-7)  /* the queue is full; the caller keeps its
                                 * response and retries next frame          */
#define YAEP_ERR_TIMEOUT (-8)  /* yaep_async_wait ran out of time          */

/* Responses the queue holds. One trial each; a caller that submits more than
 * this many before the thread drains gets YAEP_ERR_BUSY. The ring lives
 * inside yaep_async, so this sizes the handle. */
#ifndef YAEP_ASYNC_QUEUE
#define YAEP_ASYNC_QUEUE 8
#endif

/* What the thread publishes after every update, and yaep_async_poll() and
 * yaep_async_wait() hand back. A plain struct, copied out under the pump's
 * lock; nothing in it points anywhere. */
typedef struct yaep_snapshot {
    uint32_t    seq;           /* the submit this accounts for: every update
                                * through this seq is applied and `x` was
                                * computed after them. 0 is the proposal made
                                * at yaep_async_open(), before any response */
    double      x[YAEP_MAX_DIMS];  /* the next stimulus                     */
    int         proposed;      /* its candidate index, -1 for a Halton, rng
                                * or refined point, or a YAEP_ERR_* below -1
                                * when yaep_next() refused                  */
    int         update_rc;     /* what yaep_update() returned for this
                                * response: 0, or the code that made it
                                * refuse. Not part of the seq contract; it is
                                * here so a refusal on the thread cannot pass
                                * unnoticed                                  */
    int         n_trials;
    bool        done;
    yaep_stop  stop;
    double      threshold;     /* yaep_threshold() at desc.context and
                                * desc.target, with its band               */
    double      threshold_lo;
    double      threshold_hi;
    int         threshold_rc;  /* 0, YAEP_ERR_NOCROSS, or another code     */
    bool        multi_cross;   /* that search crossed more than once        */
    yaep_hyper hyper;         /* the values in force                       */
    double      log_marginal;
    int         numeric;       /* updates that returned YAEP_ERR_NUMERIC so
                                * far in this session                       */
    bool        fitting;       /* desc.fit_in_idle and the fit has not
                                * converged yet, so the hyperparameters and
                                * the log marginal are still moving         */
} yaep_snapshot;

/* Async description. Zero-initialize it and set only what you need; `gp` is
 * required and must already be open. */
typedef struct yaep_async_desc {
    yaep_gp* gp;              /* the handle the thread drives. The layer
                                * OWNS it from open to close: no yaep_*
                                * call on it meanwhile, from any thread.    */
    double    context[YAEP_MAX_DIMS]; /* the n_dims - 1 coordinates the
                                * snapshot's threshold is taken at; unused
                                * when n_dims is 1                          */
    double    target;          /* the level for it; 0 = the desc's target   */
    bool      fit_in_idle;     /* run one yaep_fit_step() whenever the queue
                                * is empty. Takes over from desc.fit_every,
                                * which is then ignored; see ASYNC          */
    bool      below_normal;    /* run the thread below normal priority      */
    int       pin_cpu;         /* logical CPU to pin it to; 0 = no pinning,
                                * as yrt_pump_desc.pin_cpu                */
} yaep_async_desc;

/* One trial's response on its way to the thread. Opaque; sized here because
 * the ring is inline. */
typedef struct yaep_async_msg {
    double x[YAEP_MAX_DIMS];
    double y;                  /* the outcome index, or the value under
                                * YAEP_LIK_GAUSSIAN                        */
    int    real;               /* 1 when it came from submit_real()          */
} yaep_async_msg;

/* Async handle. The caller allocates it and treats every field as opaque. It
 * owns no heap: the pump, the response ring and the snapshot are all inline.
 * Must be zeroed or closed before yaep_async_open(). */
typedef struct yaep_async {
    yrt_pump      pump;
    yaep_gp*       gp;
    yaep_async_msg ring[YAEP_ASYNC_QUEUE];
    yaep_snapshot  snap;      /* guarded by yrt_pump_lock()              */
    double          context[YAEP_MAX_DIMS];
    double          target;
    int             fit_every; /* the caller's, parked while fit_in_idle     */
    int             numeric;
    bool            fit_in_idle;
    bool            fit_active; /* pump thread only: the fit has more to do  */
    bool            published;  /* a snapshot exists; under the pump's lock  */
    bool            running;
    char            error[256];
} yaep_async;

/* Start the thread and publish the first proposal. Returns true with a
 * snapshot already available (yaep_async_poll() returns 0 and fills it),
 * false with yaep_async_error() set: a null handle or desc, a gp that is not
 * open, a context outside the box, or a pump the OS would not start; clears
 * the message on entry. The only async call that writes the message: submit
 * may run on any thread, so the others return a code and leave the message
 * alone rather than race on it (yaep_strerror() names the code). Open must
 * not race another call on the same handle. Cost: one yaep_next() and one
 * yaep_threshold() on the calling thread, before the thread exists. */
YAEP_API bool yaep_async_open(yaep_async* a, const yaep_async_desc* desc);

/* Deliver every response already queued, then join the thread. After it the gp
 * handle is the caller's again and every yaep_* call on it is legal. Safe on
 * a zeroed handle, after a failed open and twice. Must not race another call on the same
 * handle, except a yaep_async_wait() already blocked, which comes out with
 * YAEP_ERR_CLOSED. */
YAEP_API void yaep_async_close(yaep_async* a);

/* Hand one trial to the thread: what was shown and the outcome. Returns a
 * POSITIVE seq, increasing, which yaep_async_poll() and yaep_async_wait()
 * compare against. Negative on failure: YAEP_ERR_ARG (null handle or x, x
 * outside the box, outcome out of range, the wrong submit for the likelihood),
 * YAEP_ERR_CLOSED (not started), YAEP_ERR_BUSY (the queue is full: NOTHING
 * was copied, the caller still owns the response and should retry it on the
 * next frame). Callable from any thread. Copies the response and returns; it
 * does not wait for the inference. */
YAEP_API int yaep_async_submit(yaep_async* a, const double* x, int outcome);
YAEP_API int yaep_async_submit_real(yaep_async* a, const double* x, double y);

/* Copy the newest snapshot into `out` (which may be NULL to ask only for the
 * seq) and return the seq it accounts for: every response through that seq is
 * applied and its proposal is the one in the snapshot. 0 means the proposal
 * made at open, before any response had landed, so a caller that wants the
 * first stimulus reads it from a poll that returns 0. Negative on error. One
 * atomic load plus a struct copy under the pump's publish lock: cheap enough
 * for every frame. */
YAEP_API int yaep_async_poll(const yaep_async* a, yaep_snapshot* out);

/* The same, after blocking until the thread has finished the response `seq` or
 * `timeout_ns` of monotonic time has passed. Returns the seq accounted for
 * (>= seq on success), YAEP_ERR_TIMEOUT if the time ran out first,
 * YAEP_ERR_CLOSED if the thread stopped without reaching it. timeout_ns is
 * relative, in ns (YRT_MS(20)); 0 or less polls. This is the end-of-interval
 * call; poll() is the frame loop's. */
YAEP_API int yaep_async_wait(yaep_async* a, uint32_t seq, int64_t timeout_ns,
                               yaep_snapshot* out);

/* Responses queued and not yet applied (the one being worked on is not
 * counted), or a negative YAEP_ERR_*. For a log line, not a frame loop: it
 * takes the queue's mutex. */
YAEP_API int yaep_async_pending(const yaep_async* a);

/* Last failed yaep_async_open() message for this handle ("" if none). */
YAEP_API const char* yaep_async_error(const yaep_async* a);

/* True while the thread is running. */
YAEP_API bool yaep_async_is_running(const yaep_async* a);

/* What the thread runs at: YRT_POLICY_NORMAL, or YRT_POLICY_BELOW_NORMAL
 * when desc.below_normal was set AND the OS granted the drop, which is not an
 * error either way. YRT_POLICY_NONE before an open and after a close. Log it:
 * "the inference was ready in time" means something different at each rung. */
YAEP_API yrt_policy yaep_async_policy(const yaep_async* a);

#endif /* YAEP_ASYNC */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* YSP_AEP_H_INCLUDED */

/* ======================================================================= *
 *                             IMPLEMENTATION                              *
 * ======================================================================= */
#ifdef YSP_AEP_IMPLEMENTATION
#ifndef YSP_AEP_IMPLEMENTATION_GUARD
#define YSP_AEP_IMPLEMENTATION_GUARD

/* Under YAEP_ASYNC, ysp/rt.h's implementation too, unless this translation
 * unit already has it. Its implementation block sits outside its header guard
 * and carries a guard of its own, so a unit that also defines
 * YSP_RT_IMPLEMENTATION, or that also implements a transport header, still ends
 * up with exactly one copy. */
#ifdef YAEP_ASYNC
    #ifndef YSP_RT_IMPLEMENTATION_GUARD
        #define YSP_RT_IMPLEMENTATION
        #include "ysp/rt.h"
    #endif
#endif

#include <math.h>
#include <string.h>
#include <stdio.h>   /* snprintf, for yaep_open's one message buffer */
#include <stdarg.h>

#if !defined(YAEP_MALLOC) || !defined(YAEP_FREE)
    #include <stdlib.h>
#endif
#ifndef YAEP_MALLOC
#define YAEP_MALLOC(sz) malloc(sz)
#endif
#ifndef YAEP_FREE
#define YAEP_FREE(p) free(p)
#endif

/* Candidates are predicted in blocks, so one pass of the triangular solve
 * streams the Cholesky factor once for YAEP__BLK right-hand sides instead of
 * once per candidate. That is the difference between a factor read M times and
 * a factor read M / 32 times. */
#define YAEP__BLK 32

/* Ceiling on the free hyperparameters: a lengthscale per dimension for each of
 * the two GPs, a cutpoint per outcome, and the four scalars. */
#define YAEP__NTHETA (2 * YAEP_MAX_DIMS + YAEP_MAX_OUTCOMES + 4)

#define YAEP__SQRT2   1.4142135623730950488
#define YAEP__SQRT2PI 2.5066282746310005024
#define YAEP__LOG2PI  1.8378770664093454836
#define YAEP__SQRTPI  1.7724538509055160273

#define YAEP__NEWTON_MAX 50

/* Newton stops when its step is this small, relative. It has to be reachable
 * in the working precision: a float build resolves a latent to about 1e-7, so
 * asking for 1e-11 there would run every Newton to its iteration cap and every
 * line search into the noise. The same reason sets the smallest improvement a
 * hyperparameter step has to show. */
#define YAEP__NEWTON_TOL (sizeof(yaep_real) == sizeof(double) ? 1e-11 : 1e-5)
#define YAEP__FIT_TOL    (sizeof(yaep_real) == sizeof(double) ? 1e-9 : 1e-5)

/* Where a Newton run starts: from the prior mean, from the last mode with one
 * trial added, or from the last mode as it stands. */
#define YAEP__START_COLD 0
#define YAEP__START_ROW  1
#define YAEP__START_KEEP 2

/* Cumulative-link arguments are clamped here before the tail cancels the
 * category probability to nothing. Beyond +-8.5 the probit adds 1e-17 of
 * probability per category, so the clamp costs nothing a double can see, and
 * it keeps the third derivative of an empty category finite. */
#define YAEP__PROBIT_CLAMP 8.5
#define YAEP__LOGIT_CLAMP  36.0

/* --- scalar math -------------------------------------------------------- */

static double yaep__phi(double z) {
    return exp(-0.5 * z * z) / YAEP__SQRT2PI;
}

static double yaep__Phi(double z) {
    return 0.5 * erfc(-z / YAEP__SQRT2);
}

/* log Phi(z), by the asymptotic series where erfc has already underflowed. */
static double yaep__log_Phi(double z) {
    if (z > -20.0) {
        double p = yaep__Phi(z);
        return p > 0.0 ? log(p) : -7.0e2;
    } else {
        double zi = 1.0 / (z * z);
        double s = 1.0 - zi * (1.0 - 3.0 * zi * (1.0 - 5.0 * zi));
        return -0.5 * z * z - log(-z * YAEP__SQRT2PI) + log(s);
    }
}

/* The inverse Mills ratio phi(z) / Phi(z). The ratio tends to -z in the left
 * tail, where both parts have underflowed, so it gets the same series. */
static double yaep__mills(double z) {
    if (z > -20.0) {
        double p = yaep__Phi(z);
        return p > 1e-300 ? yaep__phi(z) / p : -z;
    } else {
        double zi = 1.0 / (z * z);
        double s = 1.0 - zi * (1.0 - 3.0 * zi * (1.0 - 5.0 * zi));
        return -z / s;
    }
}

/* Phi^-1 by Acklam's rational approximation plus one Halley step, which takes
 * it to full double precision. Used for the latent level of a target
 * probability, never in a hot loop. */
static double yaep__Phi_inv(double p) {
    static const double a[6] = { -3.969683028665376e+01,  2.209460984245205e+02,
                                 -2.759285104469687e+02,  1.383577518672690e+02,
                                 -3.066479806614716e+01,  2.506628277459239e+00 };
    static const double b[5] = { -5.447609879822406e+01,  1.615858368580409e+02,
                                 -1.556989798598866e+02,  6.680131188771972e+01,
                                 -1.328068155288572e+01 };
    static const double c[6] = { -7.784894002430293e-03, -3.223964580411365e-01,
                                 -2.400758277161838e+00, -2.549732539343734e+00,
                                  4.374664141464968e+00,  2.938163982698783e+00 };
    static const double d[4] = {  7.784695709041462e-03,  3.224671290700398e-01,
                                  2.445134137142996e+00,  3.754408661907416e+00 };
    double x, q, r, e, u;
    if (!(p > 0.0)) return -HUGE_VAL;
    if (!(p < 1.0)) return HUGE_VAL;
    if (p < 0.02425) {
        q = sqrt(-2.0 * log(p));
        x = (((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5]) /
            ((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1.0);
    } else if (p <= 1.0 - 0.02425) {
        q = p - 0.5; r = q * q;
        x = (((((a[0]*r+a[1])*r+a[2])*r+a[3])*r+a[4])*r+a[5]) * q /
            (((((b[0]*r+b[1])*r+b[2])*r+b[3])*r+b[4])*r+1.0);
    } else {
        q = sqrt(-2.0 * log(1.0 - p));
        x = -(((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5]) /
             ((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1.0);
    }
    e = yaep__Phi(x) - p;
    u = e * YAEP__SQRT2PI * exp(0.5 * x * x);
    return x - u / (1.0 + 0.5 * x * u);
}

static double yaep__sigmoid(double z) {
    if (z >= 0.0) return 1.0 / (1.0 + exp(-z));
    { double e = exp(z); return e / (1.0 + e); }
}

/* log sigmoid(z), without the cancellation of log(1 / (1 + exp(-z))). */
static double yaep__log_sigmoid(double z) {
    if (z >= 0.0) return -log1p(exp(-z));
    return z - log1p(exp(z));
}

/* The link and its first three derivatives at u. The ordinal likelihood needs
 * all four at both of the cutpoints that bound a category, and every f and
 * cutpoint derivative of that likelihood is a sum of them. */
static void yaep__link_all(int link, double u, double* L0, double* L1,
                            double* L2, double* L3) {
    if (link == YAEP_LINK_LOGIT) {
        double s = yaep__sigmoid(u), d = s * (1.0 - s);
        *L0 = s;
        *L1 = d;
        *L2 = d * (1.0 - 2.0 * s);
        *L3 = d * (1.0 - 6.0 * s * (1.0 - s));
    } else {
        double d = yaep__phi(u);
        *L0 = yaep__Phi(u);
        *L1 = d;
        *L2 = -u * d;
        *L3 = (u * u - 1.0) * d;
    }
}

static double yaep__link(int link, double z) {
    return link == YAEP_LINK_LOGIT ? yaep__sigmoid(z) : yaep__Phi(z);
}

static double yaep__link_inv(int link, double p) {
    if (link == YAEP_LINK_LOGIT) {
        if (p <= 0.0) return -HUGE_VAL;
        if (p >= 1.0) return HUGE_VAL;
        return log(p / (1.0 - p));
    }
    return yaep__Phi_inv(p);
}

static double yaep__clamp(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* log(sum(exp(v))) over n entries, shifted by the largest. */
static double yaep__logsumexp(const double* v, int n) {
    double m = v[0], s = 0.0;
    for (int i = 1; i < n; i++) if (v[i] > m) m = v[i];
    for (int i = 0; i < n; i++) s += exp(v[i] - m);
    return m + log(s);
}

/* --- Gauss-Hermite ------------------------------------------------------ */

/* Nodes and weights for int f(x) exp(-x^2) dx, by Newton's method on the
 * normalized Hermite polynomials (Numerical Recipes' gauher). Computed at
 * open, not tabulated, so YAEP_QUAD_N can change without a new table. The
 * expectation of g under N(mu, s^2) is then sum w_i g(mu + sqrt(2) s x_i) /
 * sqrt(pi). */
static void yaep__gh_init(double* x, double* w, int n) {
    const double pim4 = 0.7511255444649425;   /* pi^-1/4 */
    int m = (n + 1) / 2;
    double z = 0.0, pp = 1.0;
    for (int i = 1; i <= m; i++) {
        if (i == 1) {
            double an = 2.0 * n + 1.0;
            z = sqrt(an) - 1.85575 * pow(an, -0.16667);
        } else if (i == 2) {
            z -= 1.14 * pow((double)n, 0.426) / z;
        } else if (i == 3) {
            z = 1.86 * z - 0.86 * x[0];
        } else if (i == 4) {
            z = 1.91 * z - 0.91 * x[1];
        } else {
            z = 2.0 * z - x[i - 3];
        }
        for (int it = 0; it < 100; it++) {
            double p1 = pim4, p2 = 0.0, p3, z1;
            for (int j = 1; j <= n; j++) {
                p3 = p2; p2 = p1;
                p1 = z * sqrt(2.0 / (double)j) * p2 -
                     sqrt((double)(j - 1) / (double)j) * p3;
            }
            pp = sqrt(2.0 * (double)n) * p2;
            z1 = z;
            z = z1 - p1 / pp;
            if (fabs(z - z1) <= 1e-14 * (1.0 + fabs(z))) break;
        }
        x[i - 1] = z;
        x[n - i] = -z;
        w[i - 1] = 2.0 / (pp * pp);
        w[n - i] = w[i - 1];
    }
}

/* --- Halton ------------------------------------------------------------- */

/* The n-th prime (n = 0 gives 2), by trial division. n is at most
 * YAEP_MAX_DIMS, so the loop is shorter than a table would be to maintain. */
static unsigned yaep__nth_prime(int n) {
    unsigned p = 2;
    for (int found = 0;; p++) {
        unsigned d;
        for (d = 2; d * d <= p; d++) if (p % d == 0) break;
        if (d * d > p) {
            if (found == n) return p;
            found++;
        }
    }
}

/* The radical inverse of i in the given base: index 1 gives 1/base. Index 0
 * would be the corner of the box, which is why the sequences start at 1. */
static double yaep__radinv(unsigned i, unsigned base) {
    double f = 1.0 / (double)base, r = 0.0;
    while (i > 0u) {
        r += f * (double)(i % base);
        i /= base;
        f /= (double)base;
    }
    return r;
}

/* Phi to about 1e-7 absolute with one exponential instead of erfc
 * (Abramowitz & Stegun 26.2.17). The look-ahead evaluates M^2 of these per
 * trial and erfc costs 30 to 150 ns depending on the library, so this is the
 * difference between a look-ahead that fits a display frame and one that does
 * not. Nothing outside the look-ahead uses it: predictions and thresholds want
 * the full-precision yaep__Phi. */
static double yaep__Phi_fast(double z) {
    static const double b0 = 0.319381530, b1 = -0.356563782, b2 = 1.781477937,
                        b3 = -1.821255978, b4 = 1.330274429;
    double az = fabs(z), tt, poly, p;
    if (az > 8.0) return z > 0.0 ? 1.0 : 0.0;
    tt = 1.0 / (1.0 + 0.2316419 * az);
    poly = tt * (b0 + tt * (b1 + tt * (b2 + tt * (b3 + tt * b4))));
    p = exp(-0.5 * az * az) * 0.39894228040143268 * poly;
    return z > 0.0 ? 1.0 - p : p;
}

/* --- dense linear algebra ----------------------------------------------- */

/* Sum of a[i] b[i]. Four accumulators, because this loop is the inner loop of
 * the Cholesky, of every triangular solve and of the candidate covariance, and
 * one accumulator serializes it on the latency of the addition: a compiler may
 * not reassociate floating point on its own, and at -O2 gcc 11 does not
 * vectorize it either. */
static double yaep__dot(const double* a, const double* b, int n) {
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        s0 += a[i] * b[i];
        s1 += a[i + 1] * b[i + 1];
        s2 += a[i + 2] * b[i + 2];
        s3 += a[i + 3] * b[i + 3];
    }
    for (; i < n; i++) s0 += a[i] * b[i];
    return (s0 + s1) + (s2 + s3);
}

/* The same over a matrix row and a vector, and over two matrix rows. Separate
 * functions rather than one on yaep_real, because the vectors stay double
 * whatever YAEP_REAL is, and the accumulation is double either way. */
static double yaep__dotr(const yaep_real* a, const double* b, int n) {
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        s0 += a[i] * b[i];
        s1 += a[i + 1] * b[i + 1];
        s2 += a[i + 2] * b[i + 2];
        s3 += a[i + 3] * b[i + 3];
    }
    for (; i < n; i++) s0 += a[i] * b[i];
    return (s0 + s1) + (s2 + s3);
}

static double yaep__dotrr(const yaep_real* a, const yaep_real* b, int n) {
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        s0 += (double)a[i] * b[i];
        s1 += (double)a[i + 1] * b[i + 1];
        s2 += (double)a[i + 2] * b[i + 2];
        s3 += (double)a[i + 3] * b[i + 3];
    }
    for (; i < n; i++) s0 += (double)a[i] * b[i];
    return (s0 + s1) + (s2 + s3);
}
/* Everything is row-major with an explicit leading dimension, so an N x N
 * matrix keeps its stride while N grows with the trial count. */

/* Copies with an int count, as loops rather than memcpy: a count that reaches
 * memcpy as size_t after a signed multiply is one gcc's -Wstringop-overflow can
 * prove "may be negative" under _FORTIFY_SOURCE at -O3, and every count here is
 * a trial, dimension or parameter count that is never negative. */
static void yaep__copy(double* dst, const double* src, int n) {
    for (int i = 0; i < n; i++) dst[i] = src[i];
}
static void yaep__copyr(yaep_real* dst, const yaep_real* src, int n) {
    for (int i = 0; i < n; i++) dst[i] = src[i];
}

/* In-place lower Cholesky of the lower triangle. The upper triangle is left
 * alone. Returns false on a non-positive pivot, which is the one numeric
 * failure the API reports. */
static bool yaep__chol(yaep_real* A, int n, int ld) {
    for (int j = 0; j < n; j++) {
        yaep_real* rj = A + (size_t)j * ld;
        double dv = rj[j] - yaep__dotrr(rj, rj, j), inv;
        if (!(dv > 0.0)) return false;
        dv = sqrt(dv);
        rj[j] = (yaep_real)dv;
        inv = 1.0 / dv;
        for (int i = j + 1; i < n; i++) {
            yaep_real* ri = A + (size_t)i * ld;
            ri[j] = (yaep_real)((ri[j] - yaep__dotrr(ri, rj, j)) * inv);
        }
    }
    return true;
}

/* Solve L z = b in place. */
static void yaep__tri_fwd(const yaep_real* L, int n, int ld, double* b) {
    for (int i = 0; i < n; i++) {
        const yaep_real* ri = L + (size_t)i * ld;
        b[i] = (b[i] - yaep__dotr(ri, b, i)) / ri[i];
    }
}

/* Solve L^T x = z in place. */
static void yaep__tri_bwd(const yaep_real* L, int n, int ld, double* b) {
    for (int i = n - 1; i >= 0; i--) {
        const yaep_real* ri = L + (size_t)i * ld;
        double s = b[i] / ri[i];
        b[i] = s;
        for (int k = 0; k < i; k++) b[k] -= ri[k] * s;
    }
}

/* y = A x for a matrix stored in full. */
static void yaep__gemv(const yaep_real* A, int n, int ld, const double* x,
                        double* y) {
    for (int i = 0; i < n; i++) y[i] = yaep__dotr(A + (size_t)i * ld, x, n);
}

/* Invert a lower-triangular factor in place, row by row: row i of the
 * inverse is -(sum_k L_ik Linv_kj) / L_ii over the rows k < i already
 * inverted. The sums run k outer and j inner, so every access walks a row
 * (the column walk this replaced was a cache miss per term at N = 500, three
 * times slower), and each acc[j] still adds its terms in increasing k from
 * k = j, the order of the column form, so the result is the same to the bit.
 * acc holds n doubles. */
static void yaep__tri_inv(yaep_real* L, int n, int ld, double* acc) {
    for (int i = 0; i < n; i++) {
        yaep_real* ri = L + (size_t)i * ld;
        double inv = 1.0 / ri[i];
        for (int j = 0; j < i; j++) acc[j] = 0.0;
        for (int k = 0; k < i; k++) {
            const yaep_real* rk = L + (size_t)k * ld;
            double a = (double)ri[k];
            for (int j = 0; j <= k; j++) acc[j] += a * rk[j];
        }
        for (int j = 0; j < i; j++) ri[j] = (yaep_real)(-acc[j] * inv);
        ri[i] = (yaep_real)inv;
    }
}

/* LU with partial pivoting, row-major, in place; piv records the row swaps as
 * doubles because the one arena this header owns holds doubles. The implicit
 * term of the hyperparameter gradient needs (I + K W)^-1 with the TRUE Hessian,
 * and that Hessian has negative entries as soon as desc.guess or desc.lapse
 * takes the likelihood out of log-concavity, so there is no Cholesky to lean
 * on. Nothing else in the header needs a general solve. */
static bool yaep__lu(yaep_real* A, int n, int ld, double* piv) {
    for (int c = 0; c < n; c++) {
        int p = c;
        double best = fabs(A[(size_t)c * ld + c]);
        for (int r = c + 1; r < n; r++) {
            double v = fabs(A[(size_t)r * ld + c]);
            if (v > best) { best = v; p = r; }
        }
        piv[c] = (double)p;
        if (p != c) {
            for (int k = 0; k < n; k++) {
                yaep_real tmp = A[(size_t)c * ld + k];
                A[(size_t)c * ld + k] = A[(size_t)p * ld + k];
                A[(size_t)p * ld + k] = tmp;
            }
        }
        if (!(fabs(A[(size_t)c * ld + c]) > 0.0)) return false;
        for (int r = c + 1; r < n; r++) {
            double f = A[(size_t)r * ld + c] / A[(size_t)c * ld + c];
            A[(size_t)r * ld + c] = (yaep_real)f;
            for (int k = c + 1; k < n; k++)
                A[(size_t)r * ld + k] -= (yaep_real)(f * A[(size_t)c * ld + k]);
        }
    }
    return true;
}

static void yaep__lu_solve(const yaep_real* A, int n, int ld, const double* piv,
                            double* b) {
    for (int c = 0; c < n; c++) {
        int p = (int)piv[c];
        if (p != c) { double tmp = b[c]; b[c] = b[p]; b[p] = tmp; }
        for (int r = c + 1; r < n; r++) b[r] -= A[(size_t)r * ld + c] * b[c];
    }
    for (int i = n - 1; i >= 0; i--) {
        for (int k = i + 1; k < n; k++) b[i] -= A[(size_t)i * ld + k] * b[k];
        b[i] /= A[(size_t)i * ld + i];
    }
}

/* A := A^T A for a lower-triangular A, in place, both triangles filled. Row i
 * of the result needs rows >= i of A, so rows are finished from the top and
 * each one is staged in `tmp` before it overwrites the row it was read from. */
static void yaep__tri_sqr(yaep_real* A, int n, int ld, double* tmp) {
    for (int i = 0; i < n; i++) {
        /* k outer, j inner: rows, not columns, and each tmp[j] still sums in
         * increasing k from k = i, so the bits are the column form's. */
        for (int j = 0; j <= i; j++) tmp[j] = 0.0;
        for (int k = i; k < n; k++) {
            const yaep_real* rk = A + (size_t)k * ld;
            double a = (double)rk[i];
            for (int j = 0; j <= i; j++) tmp[j] += a * rk[j];
        }
        for (int j = 0; j <= i; j++) {
            A[(size_t)i * ld + j] = (yaep_real)tmp[j];
            A[(size_t)j * ld + i] = (yaep_real)tmp[j];
        }
    }
}

/* --- memory layout ------------------------------------------------------ */

/* The N-vectors of YAEP_MODEL_PSYCHOMETRIC, rows of scratch.ps. The latent is
 * z = (m_1..m_N, g_1..g_N); A is K^-1 (z - mean), H is z - mean, U is the
 * square root of the Gauss-Newton Hessian (one column per trial, nonzero in
 * that trial's m and g rows only, so two numbers), G is d log p / dz. */
#define YAEP__PS_AM  0
#define YAEP__PS_AG  1
#define YAEP__PS_HM  2
#define YAEP__PS_HG  3
#define YAEP__PS_UM  4
#define YAEP__PS_UG  5
#define YAEP__PS_GM  6
#define YAEP__PS_GG  7
#define YAEP__PS_BM  8    /* Newton workspace from here on */
#define YAEP__PS_BG  9
#define YAEP__PS_NM  10
#define YAEP__PS_NG  11
#define YAEP__PS_QM  12
#define YAEP__PS_QG  13
#define YAEP__PS_KM  14   /* prediction workspace */
#define YAEP__PS_KG  15
#define YAEP__PS_V1  16
#define YAEP__PS_V2  17
#define YAEP__PS_VEC 18

/* Scratch areas, carved out of the one allocation. The blocks above `blk` keep
 * their contents between calls; the ones below are temporaries. */
typedef struct yaep__scr {
    double* alpha;    /* K x Nmax: K^-1 (fhat - mean), the predictive weights */
    double* sW;       /* K x Nmax: sqrt of the Laplace Hessian diagonal       */
    double* cand_sd;  /* K x M: latent sd at the candidates                   */
    double* cand_t;   /* M: the latent level of the level set at a candidate */
    double* cand_dv;  /* M: the look-ahead covariance diagonal                */
    double* cand_p0;  /* M: P(f > f*) now, the look-ahead's baseline          */
    double* cand_isd; /* M: 1 / sd at a candidate, for the look-ahead's skip   */
    yaep_real* Rm;       /* Nmax x Nmax: (K + W^-1)^-1 for the hyper gradient,
                       *    and the per-class workspace of the categorical
                       *    Newton step                                       */
    yaep_real* Echol;    /* Nmax x Nmax: chol(sum_c E_c), CATEGORICAL only       */
    double* cat;      /* 4 x K x Nmax: the categorical Newton step, CAT only  */
    yaep_real* V;        /* M x Nmax: L^-1 W^1/2 k(X, cand), look-ahead only     */
    double* blk;      /* 2 x YAEP__BLK x Nmax: blocked prediction            */
    double* kg;       /* YAEP__NTHETA x Nmax: (dK/dtheta) grad               */
    double* score;    /* M: acquisition scores                               */
    double* gh_x;     /* YAEP_QUAD_N                                         */
    double* gh_w;     /* YAEP_QUAD_N                                         */
    double* th;       /* 6 x YAEP__NTHETA: the fit's vectors                 */
    double* t1;       /* Nmax                                                 */
    double* t2;
    double* t3;
    double* t4;
    double* t5;
    double* t6;
    double* t7;
    double* pv;       /* 4 x YAEP_MAX_OUTCOMES: outcome probabilities        */
    double* ps;       /* YAEP__PS_VEC x Nmax, PSYCHOMETRIC only; see there    */
    double* ps_c;     /* 5 x M: (m, g) posteriors at the candidates, the same */
    yaep_real* ps_v; /* 2M x Nmax: L^-1 U' k(X, c) for m and g, EAVC only    */
    double* ps_p0;    /* M: P(level) now, EAVC's baseline                     */
    yaep_real* stL;  /* Nmax x Nmax: the fit's accepted factor, fit_pcg only */
    double* stv;      /* the fit's accepted vectors and theta, fit_pcg only   */
} yaep__scr;

/* One walk of the arena for both jobs: yaep_memory_size() calls it with a
 * null base to add up the bytes, yaep_open() and every hot function call it
 * with the real base to get their pointers. One function, so the size and the
 * layout cannot drift apart. */
/* The fit's stash: f, W, grad, alpha, sqrt(W) and the psychometric vectors,
 * N each, then the theta the stash was taken at. */
#define YAEP__STV (5 + YAEP__PS_VEC)

/* desc.fit_pcg applies to the one-latent Laplace (BERNOULLI, ORDINAL,
 * PAIRWISE) and the psychometric model; GAUSSIAN has no Newton step and
 * CATEGORICAL's is K coupled factorizations. */
static bool yaep__fpcg_desc(const yaep_desc* d) {
    return d->fit_pcg && (d->model == YAEP_MODEL_PSYCHOMETRIC ||
                          (d->lik != YAEP_LIK_GAUSSIAN && d->lik != YAEP_LIK_CATEGORICAL));
}

static size_t yaep__layout(const yaep_desc* d, int Nmax, int M, int K,
                            unsigned char* base, yaep_gp* g, yaep__scr* s) {
    size_t off = 0;
    int nd = d->n_dims;
    bool fpcg = yaep__fpcg_desc(d);
    bool cat  = (d->lik == YAEP_LIK_CATEGORICAL);
    bool ps   = (d->model == YAEP_MODEL_PSYCHOMETRIC);
    bool pair = (d->lik == YAEP_LIK_PAIRWISE);
    bool look = (d->acq == YAEP_ACQ_EAVC || d->acq == YAEP_ACQ_THOMPSON || pair) && !ps;
    bool pslook = (d->acq == YAEP_ACQ_EAVC) && ps;

#define YAEP__TAKE(pp, count) do {                                  \
        double** pp_ = (pp);                                         \
        if (base && pp_) *pp_ = (double*)(base + off);               \
        off += (size_t)(count) * sizeof(double);                      \
    } while (0)
/* The same for a yaep_real block, its size rounded up to a whole double so
 * that the blocks after it stay aligned in a float build. */
#define YAEP__TAKER(pp, count) do {                                 \
        yaep_real** ppr_ = (pp);                                    \
        if (base && ppr_) *ppr_ = (yaep_real*)(base + off);         \
        off += (((size_t)(count) * sizeof(yaep_real) + sizeof(double) - 1) \
                / sizeof(double)) * sizeof(double);                  \
    } while (0)

    /* The history first: yaep_trial holds doubles, so its size is a whole
     * number of them and the blocks after it stay aligned. */
    if (base && g) g->history = (yaep_trial*)(base + off);
    off += (size_t)Nmax * sizeof(yaep_trial);
    YAEP__TAKE(g ? &g->X : NULL,       (size_t)Nmax * nd);
    YAEP__TAKE(g ? &g->X2 : NULL,      pair ? (size_t)Nmax * nd : 0);
    YAEP__TAKE(g ? &g->y : NULL,       (size_t)Nmax);
    YAEP__TAKER(g ? &g->Kmat : NULL,   (size_t)Nmax * Nmax);
    YAEP__TAKER(g ? &g->L : NULL,      (size_t)K * Nmax * Nmax);
    YAEP__TAKE(g ? &g->f : NULL,       (size_t)K * Nmax);
    YAEP__TAKE(g ? &g->W : NULL,       (size_t)K * Nmax);
    YAEP__TAKE(g ? &g->grad : NULL,    (size_t)K * Nmax);
    YAEP__TAKE(g ? &g->cand : NULL,    d->candidates ? 0 : (size_t)M * nd);
    YAEP__TAKE(g ? &g->cand_mu : NULL, (size_t)K * M);
    YAEP__TAKER(g ? &g->cand_cov : NULL, look ? (size_t)M * M : 0);
    YAEP__TAKER(g ? &g->Kg : NULL,     ps ? (size_t)Nmax * Nmax : 0);
    if (g) g->scratch = (double*)(base ? base + off : NULL);
    YAEP__TAKE(s ? &s->alpha : NULL,   (size_t)K * Nmax);
    YAEP__TAKE(s ? &s->sW : NULL,      (size_t)K * Nmax);
    YAEP__TAKE(s ? &s->cand_sd : NULL, (size_t)K * M);
    YAEP__TAKE(s ? &s->cand_t : NULL,  (size_t)M);
    YAEP__TAKE(s ? &s->cand_dv : NULL, (size_t)M);
    YAEP__TAKE(s ? &s->cand_p0 : NULL, (size_t)M);
    YAEP__TAKE(s ? &s->cand_isd : NULL, (size_t)M);
    YAEP__TAKER(s ? &s->Rm : NULL,     (size_t)Nmax * Nmax);
    YAEP__TAKER(s ? &s->Echol : NULL,  cat ? (size_t)Nmax * Nmax : 0);
    YAEP__TAKE(s ? &s->cat : NULL,     cat ? (size_t)4 * K * Nmax : 0);
    YAEP__TAKER(s ? &s->V : NULL,      look ? (size_t)M * Nmax : 0);
    YAEP__TAKE(s ? &s->blk : NULL,     (size_t)2 * YAEP__BLK * Nmax);
    YAEP__TAKE(s ? &s->kg : NULL,      (size_t)YAEP__NTHETA * Nmax);
    YAEP__TAKE(s ? &s->score : NULL,   (size_t)M);
    YAEP__TAKE(s ? &s->gh_x : NULL,    (size_t)YAEP_QUAD_N);
    YAEP__TAKE(s ? &s->gh_w : NULL,    (size_t)YAEP_QUAD_N);
    YAEP__TAKE(s ? &s->th : NULL,      (size_t)6 * YAEP__NTHETA);
    YAEP__TAKE(s ? &s->t1 : NULL,      (size_t)Nmax);
    YAEP__TAKE(s ? &s->t2 : NULL,      (size_t)Nmax);
    YAEP__TAKE(s ? &s->t3 : NULL,      (size_t)Nmax);
    YAEP__TAKE(s ? &s->t4 : NULL,      (size_t)Nmax);
    YAEP__TAKE(s ? &s->t5 : NULL,      (size_t)Nmax);
    YAEP__TAKE(s ? &s->t6 : NULL,      (size_t)Nmax);
    YAEP__TAKE(s ? &s->t7 : NULL,      (size_t)Nmax);
    YAEP__TAKE(s ? &s->pv : NULL,      (size_t)4 * YAEP_MAX_OUTCOMES);
    YAEP__TAKE(s ? &s->ps : NULL,      ps ? (size_t)YAEP__PS_VEC * Nmax : 0);
    YAEP__TAKE(s ? &s->ps_c : NULL,    ps ? (size_t)5 * M : 0);
    YAEP__TAKER(s ? &s->ps_v : NULL,   pslook ? (size_t)2 * M * Nmax : 0);
    YAEP__TAKE(s ? &s->ps_p0 : NULL,   pslook ? (size_t)M : 0);
    YAEP__TAKER(s ? &s->stL : NULL,    fpcg ? (size_t)Nmax * Nmax : 0);
    YAEP__TAKE(s ? &s->stv : NULL,     fpcg ? (size_t)YAEP__STV * Nmax + YAEP__NTHETA : 0);
#undef YAEP__TAKE
#undef YAEP__TAKER
    return off;
}

/* The scratch pointers of an open handle. Recomputed rather than stored: the
 * walk is a few dozen adds and the handle stays the size the manual says. */
static unsigned char* yaep__base(void* mem) {
    unsigned char* base = (unsigned char*)mem;
    size_t rem = (size_t)((uintptr_t)base % sizeof(double));
    if (rem) base += sizeof(double) - rem;
    return base;
}

static void yaep__scr_of(const yaep_gp* g, yaep__scr* s) {
    /* From the aligned base yaep_open() laid the arena out from. Before
     * v0.14.1 this used desc.memory as given, so a caller's buffer that was
     * not 8-byte aligned put the scratch a few bytes off the blocks open()
     * had initialized: the quadrature weights read as garbage (a sum of
     * probabilities of 8e280 on a macOS build whose static buffer landed on
     * an odd address). malloc's blocks are aligned, so only desc.memory
     * could see it. */
    yaep__layout(&g->desc, g->N_max, g->M, g->K, yaep__base(g->mem), NULL, s);
}

/* --- desc validation ---------------------------------------------------- */

/* Candidate counts above this would make the M x M look-ahead covariance the
 * whole address space; the plain count is capped so M * M * 8 cannot overflow
 * a 32-bit size_t either. */
#define YAEP__M_CAP      65536
/* More rounds than this cannot move a point that has converged, and a typo of
 * 1000 would otherwise cost a thousand predictions per dimension per trial. */
#define YAEP__REFINE_MAX 32
#define YAEP__M_CAP_LOOK 4096

static void yaep__err(char* buf, size_t cap, const char* fmt, ...) {
    va_list ap;
    if (!buf || cap == 0) return;
    va_start(ap, fmt);
    vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
}

/* The value a coordinate stands for: the nearest integer, or level, on a
 * dimension that is not CONTINUOUS. Rounding here and not only where stimuli
 * are made means a prediction at 1.2 on an INTEGER dimension is the
 * prediction at 1, which is what the observer would have been shown. */
static double yaep__coord(const yaep_desc* d, int i, double v) {
    return d->dim_kind[i] == YAEP_DIM_CONTINUOUS ? v : floor(v + 0.5);
}

/* One dimension's term q in exp(-q / 2), and in *dlog its derivative against
 * log lengthscale, so dk/dlog l = k * dlog. A CATEGORICAL dimension has no
 * distance, only same or different, and exp(-(1 - delta) / l) lets every
 * level borrow from every other through a single fitted l: large l, the
 * levels act alike; small l, each level is its own function. */
static double yaep__dq(const yaep_desc* d, int i, double a, double b, double ls,
                        double* dlog) {
    double q;
    if (d->dim_kind[i] == YAEP_DIM_CATEGORICAL) {
        q = floor(a + 0.5) != floor(b + 0.5) ? 2.0 / ls : 0.0;
        if (dlog) *dlog = 0.5 * q;
        return q;
    }
    if (d->dim_kind[i] == YAEP_DIM_INTEGER) {
        a = floor(a + 0.5);
        b = floor(b + 0.5);
    }
    q = (a - b) / ls;
    q *= q;
    if (dlog) *dlog = q;
    return q;
}

/* The span a dimension's lengthscale defaults, bounds and prior are relative
 * to. A CATEGORICAL dimension's span in level numbers means nothing, so it
 * gets a fixed 4: prior center and start 1 (levels correlated e^-1 = 0.37),
 * bounds 0.2 to 8 (e^-5 to 0.88). */
static double yaep__ls_range(const yaep_desc* d, int i) {
    return d->dim_kind[i] == YAEP_DIM_CATEGORICAL ? 4.0 : d->hi[i] - d->lo[i];
}

/* A dimension's point count on a product grid: its levels when CATEGORICAL,
 * whatever grid[] says (0 or the level count). */
static int yaep__grid_n(const yaep_desc* d, int i) {
    return d->dim_kind[i] == YAEP_DIM_CATEGORICAL ? d->dim_levels[i] : d->grid[i];
}

/* A unit-interval draw as a coordinate: uniform over the box, or over the
 * integers or levels, each with the same share. */
static double yaep__from_unit(const yaep_desc* d, int i, double u) {
    double n;
    if (d->dim_kind[i] == YAEP_DIM_CONTINUOUS)
        return d->lo[i] + (d->hi[i] - d->lo[i]) * u;
    n = d->hi[i] - d->lo[i];
    u = floor(u * (n + 1.0));
    return d->lo[i] + (u < n ? u : n);
}

/* Shared by yaep_memory_size() (which returns 0 on failure) and yaep_open()
 * (which reports why). Fills the three sizes the layout needs. */
static bool yaep__validate(const yaep_desc* d, char* err, size_t cap,
                            int* Nmax_out, int* M_out, int* K_out) {
    int nd, Nmax, M, K, nout;
    if (!d) { yaep__err(err, cap, "null desc"); return false; }
    nd = d->n_dims;
    if (nd < 1 || nd > YAEP_MAX_DIMS) {
        yaep__err(err, cap, "n_dims = %d, need 1 to %d", nd, YAEP_MAX_DIMS);
        return false;
    }
    for (int i = 0; i < nd; i++) {
        if (!(d->lo[i] < d->hi[i])) {
            yaep__err(err, cap, "lo[%d] = %g is not below hi[%d] = %g",
                       i, d->lo[i], i, d->hi[i]);
            return false;
        }
    }
    if (d->intensity_dim < 0 || d->intensity_dim >= nd) {
        yaep__err(err, cap, "intensity_dim = %d, need 0 to %d",
                   d->intensity_dim, nd - 1);
        return false;
    }
    for (int i = 0; i < nd; i++) {
        yaep_dim_kind dk = d->dim_kind[i];
        if (dk < YAEP_DIM_CONTINUOUS || dk > YAEP_DIM_CATEGORICAL) {
            yaep__err(err, cap, "dim_kind[%d] = %d is not a yaep_dim_kind", i, (int)dk);
            return false;
        }
        if (dk != YAEP_DIM_CONTINUOUS && i == d->intensity_dim) {
            yaep__err(err, cap, "dim_kind[%d]: the intensity dimension is "
                       "CONTINUOUS; thresholds are searched along it", i);
            return false;
        }
        if (dk == YAEP_DIM_INTEGER &&
            (floor(d->lo[i]) != d->lo[i] || floor(d->hi[i]) != d->hi[i])) {
            yaep__err(err, cap, "dim_kind[%d] is INTEGER: lo and hi must be "
                       "integers, not %g and %g", i, d->lo[i], d->hi[i]);
            return false;
        }
        if (dk == YAEP_DIM_CATEGORICAL &&
            (d->dim_levels[i] < 2 || d->lo[i] != 0.0 ||
             d->hi[i] != (double)(d->dim_levels[i] - 1))) {
            yaep__err(err, cap, "dim_kind[%d] is CATEGORICAL: dim_levels must be "
                       "at least 2 and the box [0, dim_levels - 1]", i);
            return false;
        }
        if (dk == YAEP_DIM_CATEGORICAL && d->grid[i] != 0 &&
            d->grid[i] != d->dim_levels[i]) {
            yaep__err(err, cap, "grid[%d] = %d: a CATEGORICAL dimension's grid is "
                       "its %d levels (0 says the same)", i, d->grid[i],
                       d->dim_levels[i]);
            return false;
        }
        if (dk == YAEP_DIM_INTEGER && d->grid[i] > (int)(d->hi[i] - d->lo[i]) + 1) {
            yaep__err(err, cap, "grid[%d] = %d: an INTEGER dimension holds only "
                       "%d values", i, d->grid[i], (int)(d->hi[i] - d->lo[i]) + 1);
            return false;
        }
    }
    if (d->monotone_dims) {
        if (nd < 32 && (d->monotone_dims >> nd) != 0u) {
            yaep__err(err, cap, "monotone_dims = 0x%x names a dimension past "
                       "n_dims = %d", d->monotone_dims, nd);
            return false;
        }
        if (d->model != YAEP_MODEL_GP || d->lik == YAEP_LIK_CATEGORICAL ||
            d->lik == YAEP_LIK_PAIRWISE) {
            yaep__err(err, cap, "monotone_dims projects the one latent of the GP "
                       "model: not with YAEP_MODEL_PSYCHOMETRIC (monotone in the "
                       "intensity already), CATEGORICAL or PAIRWISE");
            return false;
        }
        for (int i = 0; i < nd; i++)
            if (((d->monotone_dims >> i) & 1u) && d->dim_kind[i] == YAEP_DIM_CATEGORICAL) {
                yaep__err(err, cap, "monotone_dims: dimension %d is CATEGORICAL, "
                           "and unordered levels have no direction", i);
                return false;
            }
    }
    if (d->candidates) {
        for (int j = 0; j < d->n_candidates; j++)
            for (int i = 0; i < nd; i++) {
                double v = d->candidates[(size_t)j * nd + i];
                if (yaep__coord(d, i, v) != v) {
                    yaep__err(err, cap, "candidate %d: %g is not a value of "
                               "dimension %d's kind", j, v, i);
                    return false;
                }
            }
    }
    if (d->lik == YAEP_LIK_PAIRWISE) {
        if (d->model != YAEP_MODEL_GP || d->kernel != YAEP_KERNEL_RBF) {
            yaep__err(err, cap, "YAEP_LIK_PAIRWISE is the GP model with the RBF "
                       "kernel: a comparison has no intensity to be psychometric in");
            return false;
        }
        if (d->acq != YAEP_ACQ_BALD && d->acq != YAEP_ACQ_BALV &&
            d->acq != YAEP_ACQ_THOMPSON && d->acq != YAEP_ACQ_RANDOM) {
            yaep__err(err, cap, "YAEP_LIK_PAIRWISE proposes pairs with BALD, BALV, "
                       "THOMPSON or RANDOM");
            return false;
        }
        if (d->guess != 0.0 || d->lapse != 0.0) {
            yaep__err(err, cap, "guess and lapse do not apply to YAEP_LIK_PAIRWISE");
            return false;
        }
    }
    if (d->lik < YAEP_LIK_BERNOULLI || d->lik > YAEP_LIK_PAIRWISE) {
        yaep__err(err, cap, "lik = %d is not a yaep_lik", (int)d->lik);
        return false;
    }
    if (d->kernel != YAEP_KERNEL_RBF && d->kernel != YAEP_KERNEL_SEMIP) {
        yaep__err(err, cap, "kernel = %d is not a yaep_kernel", (int)d->kernel);
        return false;
    }
    if (d->link != YAEP_LINK_PROBIT && d->link != YAEP_LINK_LOGIT) {
        yaep__err(err, cap, "link = %d is not a yaep_link", (int)d->link);
        return false;
    }
    if (d->acq == YAEP_ACQ_THOMPSON || d->lik == YAEP_LIK_PAIRWISE) {
        if (d->acq == YAEP_ACQ_THOMPSON && !d->rng) {
            yaep__err(err, cap, "YAEP_ACQ_THOMPSON draws a posterior sample and "
                       "needs desc.rng");
            return false;
        }
        if (d->model == YAEP_MODEL_PSYCHOMETRIC) {
            yaep__err(err, cap, "YAEP_ACQ_THOMPSON is GP-model only; use UCB or EI "
                       "with YAEP_MODEL_PSYCHOMETRIC");
            return false;
        }
    }
    if (d->acq < YAEP_ACQ_LSE || d->acq > YAEP_ACQ_THOMPSON) {
        yaep__err(err, cap, "acq = %d is not a yaep_acq", (int)d->acq);
        return false;
    }
    {
        const yaep_priors* pr = &d->priors;
        if (pr->lengthscale.center < 0.0 || pr->outputscale.center < 0.0 ||
            pr->outputscale_b.center < 0.0 || pr->outputscale_g.center < 0.0 ||
            pr->mean_g.center < 0.0 || pr->mean_g.ceiling < 0.0 ||
            pr->noise_sd.center < 0.0) {
            yaep__err(err, cap, "a prior center or ceiling is negative; scales "
                       "and rise fractions are positive (0 = the default)");
            return false;
        }
    }
    if (d->model != YAEP_MODEL_GP && d->model != YAEP_MODEL_PSYCHOMETRIC) {
        yaep__err(err, cap, "model = %d is not a yaep_model", (int)d->model);
        return false;
    }
    if (d->model == YAEP_MODEL_PSYCHOMETRIC) {
        if (d->lik != YAEP_LIK_BERNOULLI && d->lik != YAEP_LIK_ORDINAL) {
            yaep__err(err, cap,
                       "YAEP_MODEL_PSYCHOMETRIC needs BERNOULLI or ORDINAL: its "
                       "latent is a psychometric function of the intensity");
            return false;
        }
        if (d->kernel != YAEP_KERNEL_RBF) {
            yaep__err(err, cap,
                       "YAEP_MODEL_PSYCHOMETRIC has its own two RBF GPs; leave "
                       "kernel at YAEP_KERNEL_RBF");
            return false;
        }
    }
    nout = d->n_outcomes;
    if (d->lik == YAEP_LIK_ORDINAL) {
        if (nout < 2 || nout > YAEP_MAX_OUTCOMES) {
            yaep__err(err, cap, "ORDINAL n_outcomes = %d, need 2 to %d",
                       nout, YAEP_MAX_OUTCOMES);
            return false;
        }
    } else if (d->lik == YAEP_LIK_CATEGORICAL) {
        if (nout < 3 || nout > YAEP_MAX_OUTCOMES) {
            yaep__err(err, cap, "CATEGORICAL n_outcomes = %d, need 3 to %d",
                       nout, YAEP_MAX_OUTCOMES);
            return false;
        }
    } else if (nout != 0 && nout != 2) {
        yaep__err(err, cap, "n_outcomes = %d, but this likelihood sets it",
                   nout);
        return false;
    }
    if (d->lik == YAEP_LIK_ORDINAL) {
        int t = d->target_outcome == 0 ? 1 : d->target_outcome;
        if (t < 1 || t > nout - 1) {
            yaep__err(err, cap, "target_outcome = %d, need 1 to %d", t, nout - 1);
            return false;
        }
        /* Fixed cutpoints must be usable as they stand. */
        if (d->hyper.cutpoint[0] != 0.0) {
            for (int i = 1; i < nout - 1; i++) {
                if (!(d->hyper.cutpoint[i] > d->hyper.cutpoint[i - 1])) {
                    yaep__err(err, cap, "hyper.cutpoint is not increasing at %d", i);
                    return false;
                }
            }
        }
    } else if (d->lik == YAEP_LIK_CATEGORICAL) {
        if (d->target_outcome < 0 || d->target_outcome > nout - 1) {
            yaep__err(err, cap, "target_outcome = %d, need 0 to %d",
                       d->target_outcome, nout - 1);
            return false;
        }
    }
    if (d->lik == YAEP_LIK_GAUSSIAN && sizeof(yaep_real) != sizeof(double)) {
        yaep__err(err, cap,
                   "YAEP_LIK_GAUSSIAN needs the double build: the exact "
                   "posterior factors the raw kernel matrix, which is too "
                   "ill-conditioned for %u-byte reals",
                   (unsigned)sizeof(yaep_real));
        return false;
    }
    if (d->guess < 0.0 || d->lapse < 0.0 || d->guess + d->lapse >= 1.0) {
        yaep__err(err, cap,
                   "guess = %g and lapse = %g: both need to be at least 0 and "
                   "sum below 1", d->guess, d->lapse);
        return false;
    }
    if ((d->guess != 0.0 || d->lapse != 0.0) &&
        (d->lik == YAEP_LIK_CATEGORICAL || d->lik == YAEP_LIK_GAUSSIAN)) {
        yaep__err(err, cap,
                   "guess and lapse apply to BERNOULLI and ORDINAL only");
        return false;
    }
    if (d->acq == YAEP_ACQ_LSE || d->acq == YAEP_ACQ_EAVC ||
        d->acq == YAEP_ACQ_LOCALMI) {
        if (d->lik != YAEP_LIK_GAUSSIAN &&
            !(d->target_p > d->guess && d->target_p < 1.0 - d->lapse)) {
            yaep__err(err, cap,
                       "a level-set acquisition needs target_p in (%g, %g), got %g",
                       d->guess, 1.0 - d->lapse, d->target_p);
            return false;
        }
    }
    for (int i = 0; i < nd; i++) {
        if (d->hyper.lengthscale[i] < 0.0 || d->hyper.lengthscale_b[i] < 0.0 ||
            d->hyper.lengthscale_g[i] < 0.0) {
            yaep__err(err, cap, "hyper.lengthscale[%d] is negative", i);
            return false;
        }
    }
    if (d->hyper.outputscale < 0.0 || d->hyper.outputscale_b < 0.0 ||
        d->hyper.outputscale_g < 0.0 ||
        d->hyper.noise_sd < 0.0 || d->jitter < 0.0) {
        yaep__err(err, cap, "an output scale, the noise sd or the jitter is negative");
        return false;
    }
    if (d->stop_trials < 0 || d->stop_threshold_sd < 0.0) {
        yaep__err(err, cap, "a stop criterion is negative");
        return false;
    }
    if (d->stop_trials == 0 && d->stop_threshold_sd == 0.0) {
        yaep__err(err, cap,
                   "no stop criterion: set stop_trials or stop_threshold_sd");
        return false;
    }
    if (d->max_trials < 0 || d->max_trials > YAEP_MAX_TRIALS) {
        yaep__err(err, cap, "max_trials = %d, need 0 to %d (YAEP_MAX_TRIALS)",
                   d->max_trials, YAEP_MAX_TRIALS);
        return false;
    }
    if (d->n_init < 0) { yaep__err(err, cap, "n_init is negative"); return false; }
    if (d->fit_every < 0 || d->refit_every < 0) {
        yaep__err(err, cap, "fit_every or refit_every is negative");
        return false;
    }
    if (d->n_candidates < 0) {
        yaep__err(err, cap, "n_candidates is negative");
        return false;
    }
    if (d->refine_steps < 0 || d->refine_steps > YAEP__REFINE_MAX) {
        yaep__err(err, cap, "refine_steps = %d, need 0 to %d", d->refine_steps,
                   YAEP__REFINE_MAX);
        return false;
    }
    Nmax = d->max_trials > 0 ? d->max_trials : YAEP_MAX_TRIALS;
    K = d->lik == YAEP_LIK_CATEGORICAL ? nout : 1;
    /* The candidate set: the caller's list, a product grid, or Halton. */
    if (d->candidates) {
        if (d->n_candidates <= 0) {
            yaep__err(err, cap, "desc.candidates needs n_candidates > 0");
            return false;
        }
        M = d->n_candidates;
    } else {
        int any = 0;
        for (int i = 0; i < nd; i++) if (d->grid[i] != 0) any = 1;
        if (any) {
            double prod = 1.0;
            for (int i = 0; i < nd; i++) {
                if (yaep__grid_n(d, i) < 1) {
                    yaep__err(err, cap,
                               "grid[%d] = %d: every dimension needs at least 1 point",
                               i, d->grid[i]);
                    return false;
                }
                prod *= (double)yaep__grid_n(d, i);
            }
            if (prod > (double)YAEP__M_CAP) {
                yaep__err(err, cap, "the product grid has %.0f points, cap is %d",
                           prod, YAEP__M_CAP);
                return false;
            }
            M = 1;
            for (int i = 0; i < nd; i++) M *= yaep__grid_n(d, i);
        } else {
            M = d->n_candidates > 0 ? d->n_candidates : 512;
        }
    }
    if (M > YAEP__M_CAP) {
        yaep__err(err, cap, "n_candidates = %d, cap is %d", M, YAEP__M_CAP);
        return false;
    }
    if ((d->acq == YAEP_ACQ_EAVC || d->acq == YAEP_ACQ_THOMPSON ||
         d->lik == YAEP_LIK_PAIRWISE) && M > YAEP__M_CAP_LOOK) {
        yaep__err(err, cap,
                   "EAVC holds an M x M covariance: M = %d, cap is %d", M,
                   YAEP__M_CAP_LOOK);
        return false;
    }
    if (Nmax_out) *Nmax_out = Nmax;
    if (M_out) *M_out = M;
    if (K_out) *K_out = K;
    return true;
}

/* --- kernel ------------------------------------------------------------- */

/* The intensity enters the semiparametric kernel through x x', so the slope
 * GP's prior variance is x^2 k_b(c, c) and the point where the slope term
 * vanishes is x = 0. Measuring x from the middle of the box puts that point in
 * the middle of the stimulus range instead of wherever zero happens to fall in
 * the caller's units, which is the difference between a usable prior and one
 * that pins the latent to its mean at a box edge. */
static double yaep__xt(const yaep_gp* g, const double* x) {
    int id = g->desc.intensity_dim;
    return x[id] - 0.5 * (g->desc.lo[id] + g->desc.hi[id]);
}

/* k_a and the slope term x x' k_b of the kernel, separately: the hyperparameter
 * gradient needs the two parts, and the kernel value is their sum. */
static void yaep__kparts(const yaep_gp* g, const double* xa, const double* xb,
                          double* ka, double* kb, double* xx) {
    const yaep_hyper* h = &g->hyper;
    int nd = g->desc.n_dims, id = g->desc.intensity_dim;
    double qa = 0.0;
    if (g->desc.kernel == YAEP_KERNEL_RBF) {
        if (g->mixed) {
            for (int i = 0; i < nd; i++)
                qa += yaep__dq(&g->desc, i, xa[i], xb[i], h->lengthscale[i], NULL);
        } else {
            for (int i = 0; i < nd; i++) {
                double dd = (xa[i] - xb[i]) / h->lengthscale[i];
                qa += dd * dd;
            }
        }
        *ka = h->outputscale * exp(-0.5 * qa);
        *kb = 0.0;
        *xx = 0.0;
    } else {
        double qb = 0.0;
        for (int i = 0; i < nd; i++) {
            double dl;
            if (i == id) continue;
            if (g->mixed) {
                qa += yaep__dq(&g->desc, i, xa[i], xb[i], h->lengthscale[i], NULL);
                qb += yaep__dq(&g->desc, i, xa[i], xb[i], h->lengthscale_b[i], NULL);
                continue;
            }
            dl = (xa[i] - xb[i]) / h->lengthscale[i];
            qa += dl * dl;
            dl = (xa[i] - xb[i]) / h->lengthscale_b[i];
            qb += dl * dl;
        }
        *ka = h->outputscale * exp(-0.5 * qa);
        *kb = h->outputscale_b * exp(-0.5 * qb);
        *xx = yaep__xt(g, xa) * yaep__xt(g, xb);
    }
}

static double yaep__kernel(const yaep_gp* g, const double* xa, const double* xb) {
    double ka, kb, xx;
    yaep__kparts(g, xa, xb, &ka, &kb, &xx);
    return ka + xx * kb;
}

static bool yaep__is_ps(const yaep_gp* g) {
    return g->desc.model == YAEP_MODEL_PSYCHOMETRIC;
}

/* The covariance between trial i's latent and f(x). A trial's latent is f at
 * its stimulus, or under PAIRWISE the difference f(x1) - f(x2), whose
 * covariance with f(x) is the difference of two kernel values. Every
 * predictor goes through this, so PAIRWISE predicts the utility f with the
 * same formulas. */
static double yaep__kx(const yaep_gp* g, int i, const double* x) {
    int nd = g->desc.n_dims;
    double v = yaep__kernel(g, g->X + (size_t)i * nd, x);
    if (g->desc.lik == YAEP_LIK_PAIRWISE)
        v -= yaep__kernel(g, g->X2 + (size_t)i * nd, x);
    return v;
}

/* The covariance between two trials' latents: under PAIRWISE the four-term
 * kernel of the differences. */
static double yaep__kt(const yaep_gp* g, int i, int j) {
    int nd = g->desc.n_dims;
    const double* xi = g->X + (size_t)i * nd;
    const double* xj = g->X + (size_t)j * nd;
    double v = yaep__kernel(g, xi, xj);
    if (g->desc.lik == YAEP_LIK_PAIRWISE) {
        const double* yi = g->X2 + (size_t)i * nd;
        const double* yj = g->X2 + (size_t)j * nd;
        v += yaep__kernel(g, yi, yj) - yaep__kernel(g, xi, yj) -
             yaep__kernel(g, yi, xj);
    }
    return v;
}

/* The two context kernels of YAEP_MODEL_PSYCHOMETRIC, RBF-ARD over every
 * dimension but the intensity: which = 0 for the threshold GP m, 1 for the
 * log-slope GP g. With one dimension there is no context, and each GP is a
 * single Gaussian variable with the output scale as its variance. */
static double yaep__kctx(const yaep_gp* g, int which, const double* xa,
                          const double* xb) {
    const yaep_hyper* h = &g->hyper;
    const double* ls = which ? h->lengthscale_g : h->lengthscale;
    int nd = g->desc.n_dims, id = g->desc.intensity_dim;
    double q = 0.0;
    for (int i = 0; i < nd; i++) {
        double dd;
        if (i == id) continue;
        if (g->mixed) { q += yaep__dq(&g->desc, i, xa[i], xb[i], ls[i], NULL); continue; }
        dd = (xa[i] - xb[i]) / ls[i];
        q += dd * dd;
    }
    return (which ? h->outputscale_g : h->outputscale) * exp(-0.5 * q);
}

/* Two stimuli share a context when every coordinate but the intensity agrees,
 * and then they share the posterior of (m, g) exactly. */
static bool yaep__same_ctx(const yaep_gp* g, const double* xa, const double* xb) {
    int nd = g->desc.n_dims, id = g->desc.intensity_dim;
    const yaep_desc* d = &g->desc;
    for (int i = 0; i < nd; i++)
        if (i != id && yaep__coord(d, i, xa[i]) != yaep__coord(d, i, xb[i]))
            return false;
    return true;
}

/* The intensity axis's span, and the defaults the psychometric model derives
 * from it: a threshold prior sd of a quarter of the axis, centered in the
 * middle, and a slope whose rise takes about a quarter of the axis. */
static double yaep__ispan(const yaep_desc* d) {
    return d->hi[d->intensity_dim] - d->lo[d->intensity_dim];
}
/* desc.priors with every zero replaced by its measured default. A negative sd
 * stays negative (that prior off). */
static yaep_priors yaep__priors_resolve(const yaep_desc* d) {
    yaep_priors r = d->priors;
    double q = 0.25 * (d->hi[d->intensity_dim] - d->lo[d->intensity_dim]);
    bool ps = d->model == YAEP_MODEL_PSYCHOMETRIC;
#define YAEP__DEF(f, v) do { if (r.f == 0.0) r.f = (v); } while (0)
    YAEP__DEF(lengthscale.center, 0.25);
    YAEP__DEF(lengthscale.sd, 1.0);
    YAEP__DEF(outputscale.center, ps ? q * q : 1.0);
    YAEP__DEF(outputscale.sd, 0.7);
    YAEP__DEF(outputscale_b.center, 1.0);
    YAEP__DEF(outputscale_b.sd, 0.7);
    YAEP__DEF(outputscale_g.center, 0.3);
    YAEP__DEF(outputscale_g.sd, 0.7);
    YAEP__DEF(mean.sd, 0.7);                 /* center 0 is its default */
    YAEP__DEF(mean_g.center, 0.25);
    YAEP__DEF(mean_g.sd, 1.0);
    YAEP__DEF(mean_g.ceiling, 1.0 / 256.0);
    if (r.noise_sd.center == 0.0 || r.noise_sd.sd == 0.0) r.noise_sd.sd = -1.0;
#undef YAEP__DEF
    return r;
}

static double yaep__ps_os_m(const yaep_desc* d) {
    double q = 0.25 * yaep__ispan(d);
    return q * q;
}

/* --- hyperparameter vector ---------------------------------------------- */

enum {
    YAEP__P_LS = 0, YAEP__P_OS, YAEP__P_MEAN,
    YAEP__P_LSB, YAEP__P_OSB, YAEP__P_CUT, YAEP__P_NOISE,
    YAEP__P_LSG, YAEP__P_OSG, YAEP__P_MEANG
};

typedef struct yaep__param {
    int    kind;
    int    dim;      /* dimension, or cutpoint array index */
    double lo, hi;   /* bounds in the fitted coordinate */
    bool   logsp;    /* fitted as log(value) */
} yaep__param;

static double yaep__pick(double v, double dflt) { return v != 0.0 ? v : dflt; }

/* The free hyperparameters, in a fixed order, with their bounds. A field the
 * caller set to something nonzero is fixed there; a zero field is fitted (and
 * carries a default until it is). That is why desc.hyper, not the values in
 * force, decides. */
static int yaep__params_ps(const yaep_gp* g, yaep__param* p) {
    const yaep_desc* d = &g->desc;
    const yaep_hyper* h0 = &d->hyper;
    int nd = d->n_dims, id = d->intensity_dim, np = 0;
    double os_m = yaep__ps_os_m(d), span = yaep__ispan(d);
    for (int which = 0; which < 2; which++) {
        const double* ls0 = which ? h0->lengthscale_g : h0->lengthscale;
        const double* lsmin = which ? d->hyper_min.lengthscale_g : d->hyper_min.lengthscale;
        const double* lsmax = which ? d->hyper_max.lengthscale_g : d->hyper_max.lengthscale;
        for (int i = 0; i < nd; i++) {
            double range = yaep__ls_range(d, i);
            if (i == id || ls0[i] != 0.0) continue;
            p[np].kind = which ? YAEP__P_LSG : YAEP__P_LS;
            p[np].dim = i; p[np].logsp = true;
            p[np].lo = log(yaep__pick(lsmin[i], 0.05 * range));
            p[np].hi = log(yaep__pick(lsmax[i], 2.0 * range));
            np++;
        }
    }
    if (h0->outputscale == 0.0) {
        p[np].kind = YAEP__P_OS; p[np].dim = 0; p[np].logsp = true;
        p[np].lo = log(yaep__pick(d->hyper_min.outputscale, 0.1 * os_m));
        p[np].hi = log(yaep__pick(d->hyper_max.outputscale, 10.0 * os_m));
        np++;
    }
    if (h0->mean == 0.0) {
        p[np].kind = YAEP__P_MEAN; p[np].dim = 0; p[np].logsp = false;
        p[np].lo = yaep__pick(d->hyper_min.mean, d->lo[id]);
        p[np].hi = yaep__pick(d->hyper_max.mean, d->hi[id]);
        np++;
    }
    if (h0->outputscale_g == 0.0) {
        p[np].kind = YAEP__P_OSG; p[np].dim = 0; p[np].logsp = true;
        p[np].lo = log(yaep__pick(d->hyper_min.outputscale_g, 0.01));
        p[np].hi = log(yaep__pick(d->hyper_max.outputscale_g, 10.0));
        np++;
    }
    if (h0->mean_g == 0.0) {
        /* A rise from 16 times the axis down to a sixty-fourth of it. */
        p[np].kind = YAEP__P_MEANG; p[np].dim = 0; p[np].logsp = false;
        p[np].lo = yaep__pick(d->hyper_min.mean_g, log(0.25 / span));
        p[np].hi = yaep__pick(d->hyper_max.mean_g,
                               log(1.0 / (g->priors.mean_g.ceiling * span)));
        np++;
    }
    if (d->lik == YAEP_LIK_ORDINAL && h0->cutpoint[0] == 0.0) {
        for (int m = 1; m < d->n_outcomes - 1; m++) {
            p[np].kind = YAEP__P_CUT; p[np].dim = m; p[np].logsp = true;
            p[np].lo = log(yaep__pick(d->hyper_min.cutpoint[m], 0.01));
            p[np].hi = log(yaep__pick(d->hyper_max.cutpoint[m], 10.0));
            np++;
        }
    }
    return np;
}

static int yaep__params(const yaep_gp* g, yaep__param* p) {
    const yaep_desc* d = &g->desc;
    const yaep_hyper* h0 = &d->hyper;
    int nd = d->n_dims, id = d->intensity_dim, np = 0;
    bool semip = (d->kernel == YAEP_KERNEL_SEMIP);
    if (yaep__is_ps(g)) return yaep__params_ps(g, p);
    for (int i = 0; i < nd; i++) {
        double range = yaep__ls_range(d, i);
        if (semip && i == id) continue;   /* no lengthscale along x */
        if (h0->lengthscale[i] != 0.0) continue;
        p[np].kind = YAEP__P_LS; p[np].dim = i; p[np].logsp = true;
        p[np].lo = log(yaep__pick(d->hyper_min.lengthscale[i], 0.05 * range));
        p[np].hi = log(yaep__pick(d->hyper_max.lengthscale[i], 2.0 * range));
        np++;
    }
    if (h0->outputscale == 0.0) {
        p[np].kind = YAEP__P_OS; p[np].dim = 0; p[np].logsp = true;
        p[np].lo = log(yaep__pick(d->hyper_min.outputscale, 0.1));
        p[np].hi = log(yaep__pick(d->hyper_max.outputscale, 10.0));
        np++;
    }
    /* A comparison sees only differences, which the mean cancels out of:
     * under PAIRWISE it stays at hyper.mean (0) and is not fitted. */
    if (h0->mean == 0.0 && d->lik != YAEP_LIK_PAIRWISE) {
        p[np].kind = YAEP__P_MEAN; p[np].dim = 0; p[np].logsp = false;
        p[np].lo = yaep__pick(d->hyper_min.mean, -5.0);
        p[np].hi = yaep__pick(d->hyper_max.mean, 5.0);
        np++;
    }
    if (semip) {
        for (int i = 0; i < nd; i++) {
            double range = yaep__ls_range(d, i);
            if (i == id) continue;
            if (h0->lengthscale_b[i] != 0.0) continue;
            p[np].kind = YAEP__P_LSB; p[np].dim = i; p[np].logsp = true;
            p[np].lo = log(yaep__pick(d->hyper_min.lengthscale_b[i], 0.05 * range));
            p[np].hi = log(yaep__pick(d->hyper_max.lengthscale_b[i], 2.0 * range));
            np++;
        }
        if (h0->outputscale_b == 0.0) {
            p[np].kind = YAEP__P_OSB; p[np].dim = 0; p[np].logsp = true;
            p[np].lo = log(yaep__pick(d->hyper_min.outputscale_b, 0.1));
            p[np].hi = log(yaep__pick(d->hyper_max.outputscale_b, 10.0));
            np++;
        }
    }
    if (d->lik == YAEP_LIK_ORDINAL && h0->cutpoint[0] == 0.0) {
        /* c_1 is pinned to 0 and the rest are fitted as log gaps, which keeps
         * them increasing and keeps hyper.mean identifiable. */
        for (int m = 1; m < d->n_outcomes - 1; m++) {
            p[np].kind = YAEP__P_CUT; p[np].dim = m; p[np].logsp = true;
            p[np].lo = log(yaep__pick(d->hyper_min.cutpoint[m], 0.01));
            p[np].hi = log(yaep__pick(d->hyper_max.cutpoint[m], 10.0));
            np++;
        }
    }
    if (d->lik == YAEP_LIK_GAUSSIAN && h0->noise_sd == 0.0) {
        p[np].kind = YAEP__P_NOISE; p[np].dim = 0; p[np].logsp = true;
        p[np].lo = log(yaep__pick(d->hyper_min.noise_sd, 1e-3));
        p[np].hi = log(yaep__pick(d->hyper_max.noise_sd, 10.0));
        np++;
    }
    return np;
}

static double yaep__param_value(const yaep_gp* g, const yaep__param* p) {
    const yaep_hyper* h = &g->hyper;
    switch (p->kind) {
        case YAEP__P_LS:   return h->lengthscale[p->dim];
        case YAEP__P_OS:   return h->outputscale;
        case YAEP__P_MEAN: return h->mean;
        case YAEP__P_LSB:  return h->lengthscale_b[p->dim];
        case YAEP__P_OSB:  return h->outputscale_b;
        case YAEP__P_CUT:  return h->cutpoint[p->dim] - h->cutpoint[p->dim - 1];
        case YAEP__P_LSG:  return h->lengthscale_g[p->dim];
        case YAEP__P_OSG:  return h->outputscale_g;
        case YAEP__P_MEANG: return h->mean_g;
        default:            return h->noise_sd;
    }
}

static void yaep__theta_get(const yaep_gp* g, const yaep__param* p, int np,
                             double* th) {
    for (int i = 0; i < np; i++) {
        double v = yaep__param_value(g, &p[i]);
        th[i] = yaep__clamp(p[i].logsp ? log(v) : v, p[i].lo, p[i].hi);
    }
}

static void yaep__theta_set(yaep_gp* g, const yaep__param* p, int np,
                             const double* th) {
    yaep_hyper* h = &g->hyper;
    for (int i = 0; i < np; i++) {
        double v = yaep__clamp(th[i], p[i].lo, p[i].hi);
        if (p[i].logsp) v = exp(v);
        switch (p[i].kind) {
            case YAEP__P_LS:   h->lengthscale[p[i].dim] = v; break;
            case YAEP__P_OS:   h->outputscale = v; break;
            case YAEP__P_MEAN: h->mean = v; break;
            case YAEP__P_LSB:  h->lengthscale_b[p[i].dim] = v; break;
            case YAEP__P_OSB:  h->outputscale_b = v; break;
            case YAEP__P_CUT:  h->cutpoint[p[i].dim] = h->cutpoint[p[i].dim - 1] + v;
                                break;
            case YAEP__P_LSG:  h->lengthscale_g[p[i].dim] = v; break;
            case YAEP__P_OSG:  h->outputscale_g = v; break;
            case YAEP__P_MEANG: h->mean_g = v; break;
            default:            h->noise_sd = v; break;
        }
    }
}

/* dk(xa, xb)/dtheta for every parameter at once: one pass over the matrix
 * instead of one pass per hyperparameter, which is what keeps a gradient
 * evaluation at O(N^2) per hyperparameter with the constant of a single
 * exponential. */
static void yaep__kernel_grad(const yaep_gp* g, const yaep__param* p, int np,
                               const double* xa, const double* xb, double* dk) {
    const yaep_hyper* h = &g->hyper;
    double ka, kb, xx;
    yaep__kparts(g, xa, xb, &ka, &kb, &xx);
    for (int i = 0; i < np; i++) {
        int dm = p[i].dim;
        double dd;
        switch (p[i].kind) {
            case YAEP__P_LS:
                if (g->mixed) {
                    yaep__dq(&g->desc, dm, xa[dm], xb[dm], h->lengthscale[dm], &dd);
                    dk[i] = ka * dd;
                    break;
                }
                dd = (xa[dm] - xb[dm]) / h->lengthscale[dm];
                dk[i] = ka * dd * dd;
                break;
            case YAEP__P_OS:
                dk[i] = ka;
                break;
            case YAEP__P_LSB:
                if (g->mixed) {
                    yaep__dq(&g->desc, dm, xa[dm], xb[dm], h->lengthscale_b[dm], &dd);
                    dk[i] = xx * kb * dd;
                    break;
                }
                dd = (xa[dm] - xb[dm]) / h->lengthscale_b[dm];
                dk[i] = xx * kb * dd * dd;
                break;
            case YAEP__P_OSB:
                dk[i] = xx * kb;
                break;
            default:
                dk[i] = 0.0;   /* mean, cutpoints and noise are not in K */
                break;
        }
    }
}

/* --- likelihoods -------------------------------------------------------- */

/* PAIRWISE is the Bernoulli likelihood on d = f(x1) - f(x2), so every
 * one-latent likelihood function treats the two alike. */
static bool yaep__bern(const yaep_gp* g) {
    return g->desc.lik == YAEP_LIK_BERNOULLI || g->desc.lik == YAEP_LIK_PAIRWISE;
}
static bool yaep__is_pair(const yaep_gp* g) {
    return g->desc.lik == YAEP_LIK_PAIRWISE;
}

/* The span the link is squeezed into: p = guess + span * link(f). One for a
 * likelihood the floor and ceiling do not apply to, so every formula below can
 * use it unconditionally. */
static double yaep__span(const yaep_gp* g) {
    return 1.0 - g->desc.guess - g->desc.lapse;
}


/* The cumulative link writes category j as Lambda(u1) - Lambda(u0) with
 * u0 = c_j - f and u1 = c_{j+1} - f, so every derivative in f and in the two
 * cutpoints is a combination of the partials of log(Lambda(u1) - Lambda(u0)).
 * Filling them all once is what lets the f-derivatives and the cutpoint
 * gradient share one code path.
 * G = { g0, g1, g00, g01, g11, g000, g001, g011, g111, log p }. */
static void yaep__ord_G(const yaep_gp* g, double fv, int j, double* G) {
    int link = (int)g->desc.link;
    int nout = g->desc.n_outcomes;
    double clamp = link == YAEP_LINK_LOGIT ? YAEP__LOGIT_CLAMP : YAEP__PROBIT_CLAMP;
    double A0 = 0.0, l0 = 0.0, m0 = 0.0, n0 = 0.0;
    double A1 = 1.0, l1 = 0.0, m1 = 0.0, n1 = 0.0;
    double span = yaep__span(g), off = 0.0;
    double P, iP, iP2, iP3;
    if (j > 0) {
        double u0 = yaep__clamp(g->hyper.cutpoint[j - 1] - fv, -clamp, clamp);
        yaep__link_all(link, u0, &A0, &l0, &m0, &n0);
    }
    if (j < nout - 1) {
        double u1 = yaep__clamp(g->hyper.cutpoint[j] - fv, -clamp, clamp);
        yaep__link_all(link, u1, &A1, &l1, &m1, &n1);
    }
    /* desc.guess and desc.lapse move mass to the top and bottom categories:
     * P(y >= k) becomes guess + span P(y >= k) for every inner k, so a
     * category probability is span times the old difference, plus the lapse
     * for the bottom category and the guess for the top. The partials keep
     * their shape, with the densities scaled by the span. */
    if (j == 0) off = g->desc.lapse;
    if (j == nout - 1) off += g->desc.guess;
    l0 *= span; l1 *= span;
    m0 *= span; m1 *= span;
    n0 *= span; n1 *= span;
    P = off + span * (A1 - A0);
    if (P < 1e-14) P = 1e-14;
    iP = 1.0 / P; iP2 = iP * iP; iP3 = iP2 * iP;
    G[0] = -l0 * iP;
    G[1] =  l1 * iP;
    G[2] = -m0 * iP - l0 * l0 * iP2;
    G[3] =  l0 * l1 * iP2;
    G[4] =  m1 * iP - l1 * l1 * iP2;
    G[5] = -n0 * iP - 3.0 * l0 * m0 * iP2 - 2.0 * l0 * l0 * l0 * iP3;
    G[6] =  m0 * l1 * iP2 + 2.0 * l0 * l0 * l1 * iP3;
    G[7] =  l0 * m1 * iP2 - 2.0 * l0 * l1 * l1 * iP3;
    G[8] =  n1 * iP - 3.0 * l1 * m1 * iP2 + 2.0 * l1 * l1 * l1 * iP3;
    G[9] = log(P);
}

/* log p(y | f) and its first three derivatives in f, for the one-latent
 * likelihoods. */
static void yaep__ll(const yaep_gp* g, double fv, double yv,
                      double* lp, double* d1, double* d2, double* d3) {
    if (yaep__bern(g) &&
        (g->desc.guess > 0.0 || g->desc.lapse > 0.0)) {
        /* p(y = 1) = guess + span link(f). The tail-stable forms below do not
         * apply once there is a floor, so this path works from the link and
         * its three derivatives and the generic derivatives of a logarithm,
         * with the link argument clamped where the density underflows. */
        int link = (int)g->desc.link;
        double clamp = link == YAEP_LINK_LOGIT ? YAEP__LOGIT_CLAMP
                                                : YAEP__PROBIT_CLAMP;
        double span = yaep__span(g);
        double L0, L1, L2, L3, q, q1, q2, q3, r;
        yaep__link_all(link, yaep__clamp(fv, -clamp, clamp), &L0, &L1, &L2, &L3);
        if (yv > 0.5) {
            q = g->desc.guess + span * L0;
            q1 = span * L1; q2 = span * L2; q3 = span * L3;
        } else {
            q = g->desc.lapse + span * (1.0 - L0);
            q1 = -span * L1; q2 = -span * L2; q3 = -span * L3;
        }
        if (q < 1e-300) q = 1e-300;
        r = q1 / q;
        *lp = log(q);
        *d1 = r;
        *d2 = q2 / q - r * r;
        *d3 = q3 / q - 3.0 * (q2 / q) * r + 2.0 * r * r * r;
    } else if (yaep__bern(g)) {
        if (g->desc.link == YAEP_LINK_LOGIT) {
            double t = yv > 0.5 ? 1.0 : 0.0;
            double pr = yaep__sigmoid(fv), q = pr * (1.0 - pr);
            *lp = t > 0.5 ? yaep__log_sigmoid(fv) : yaep__log_sigmoid(-fv);
            *d1 = t - pr;
            *d2 = -q;
            *d3 = -q * (1.0 - 2.0 * pr);
        } else {
            double s = yv > 0.5 ? 1.0 : -1.0;
            double z = s * fv, r = yaep__mills(z);
            *lp = yaep__log_Phi(z);
            *d1 = s * r;
            *d2 = -(r * r + z * r);
            *d3 = s * (2.0 * r * r * r + 3.0 * z * r * r + (z * z - 1.0) * r);
        }
    } else {
        double G[10] = { 0 };
        int j = (int)(yv + 0.5);
        yaep__ord_G(g, fv, j, G);
        *lp = G[9];
        *d1 = -(G[0] + G[1]);
        *d2 = G[2] + 2.0 * G[3] + G[4];
        *d3 = -(G[5] + 3.0 * G[6] + 3.0 * G[7] + G[8]);
    }
}

/* The cutpoint partials the hyperparameter gradient needs: d log p / dc_m,
 * d2 log p / df dc_m and d3 log p / df^2 dc_m, where m indexes the cutpoint
 * array (c_{m+1} of the manual). Only the two cutpoints that bound the
 * observed category contribute. */
static void yaep__ll_cut(const yaep_gp* g, double fv, double yv, int m,
                          double* e0, double* e1, double* e2) {
    double G[10] = { 0 };
    int j = (int)(yv + 0.5);
    *e0 = *e1 = *e2 = 0.0;
    if (m != j - 1 && m != j) return;
    yaep__ord_G(g, fv, j, G);
    if (m == j - 1) {
        *e0 = G[0];
        *e1 = -(G[2] + G[3]);
        *e2 = G[5] + 2.0 * G[6] + G[7];
    } else {
        *e0 = G[1];
        *e1 = -(G[3] + G[4]);
        *e2 = G[6] + 2.0 * G[7] + G[8];
    }
}

/* All K outcome probabilities at an exact latent value, for the one-latent
 * likelihoods. */
static void yaep__probs_of_f(const yaep_gp* g, double fv, double* p) {
    int link = (int)g->desc.link;
    double span = yaep__span(g);
    if (yaep__bern(g)) {
        p[1] = g->desc.guess + span * yaep__link(link, fv);
        p[0] = 1.0 - p[1];
    } else {
        int nout = g->desc.n_outcomes;
        double prev = 0.0;
        for (int j = 0; j < nout - 1; j++) {
            double c = yaep__link(link, g->hyper.cutpoint[j] - fv);
            p[j] = span * (c - prev) + (j == 0 ? g->desc.lapse : 0.0);
            if (p[j] < 0.0) p[j] = 0.0;
            prev = c;
        }
        p[nout - 1] = span * (1.0 - prev) + g->desc.guess;
        if (p[nout - 1] < 0.0) p[nout - 1] = 0.0;
    }
}

/* The target quantity at an exact latent value: P(y = 1), P(y >= k*),
 * one-against-the-rest P(y = k*), or y itself. `other` carries the log-sum-exp
 * of the competing latents under CATEGORICAL and is ignored elsewhere. */
static double yaep__q_of_f(const yaep_gp* g, double fv, double other) {
    int link = (int)g->desc.link;
    int k = g->desc.target_outcome == 0 && g->desc.lik == YAEP_LIK_ORDINAL
            ? 1 : g->desc.target_outcome;
    switch (g->desc.lik) {
        case YAEP_LIK_BERNOULLI:
            return g->desc.guess + yaep__span(g) * yaep__link(link, fv);
        case YAEP_LIK_ORDINAL:
            return g->desc.guess + yaep__span(g) *
                   (1.0 - yaep__link(link, g->hyper.cutpoint[k - 1] - fv));
        case YAEP_LIK_CATEGORICAL: return yaep__sigmoid(fv - other);
        default:                    return fv;
    }
}

/* --- the kernel matrix -------------------------------------------------- */

static const double* yaep__cand(const yaep_gp* g, int i) {
    int nd = g->desc.n_dims;
    return g->desc.candidates ? g->desc.candidates + (size_t)i * nd
                              : g->cand + (size_t)i * nd;
}

static double yaep__jitter(const yaep_gp* g) {
    if (g->desc.jitter > 0.0) return g->desc.jitter;
    /* The default has to cover the rounding the factorization itself adds,
     * which is about N times the working epsilon. */
    return sizeof(yaep_real) == sizeof(double) ? 1e-6 : 1e-4;
}

/* Row and column n of the kernel matrix, for a trial just recorded. */
static void yaep__kmat_row(yaep_gp* g, int n) {
    int nd = g->desc.n_dims, ld = g->N_max;
    const double* xn = g->X + (size_t)n * nd;
    if (yaep__is_ps(g)) {
        for (int i = 0; i < n; i++) {
            const double* xi = g->X + (size_t)i * nd;
            yaep_real vm = (yaep_real)yaep__kctx(g, 0, xi, xn);
            yaep_real vg = (yaep_real)yaep__kctx(g, 1, xi, xn);
            g->Kmat[(size_t)n * ld + i] = vm; g->Kmat[(size_t)i * ld + n] = vm;
            g->Kg[(size_t)n * ld + i] = vg;   g->Kg[(size_t)i * ld + n] = vg;
        }
        g->Kmat[(size_t)n * ld + n] = (yaep_real)(yaep__kctx(g, 0, xn, xn) +
                                                   yaep__jitter(g));
        g->Kg[(size_t)n * ld + n] = (yaep_real)(yaep__kctx(g, 1, xn, xn) +
                                                 yaep__jitter(g));
        return;
    }
    for (int i = 0; i < n; i++) {
        double v = yaep__kt(g, i, n);
        g->Kmat[(size_t)n * ld + i] = (yaep_real)v;
        g->Kmat[(size_t)i * ld + n] = (yaep_real)v;
    }
    (void)xn;
    g->Kmat[(size_t)n * ld + n] = (yaep_real)(yaep__kt(g, n, n) + yaep__jitter(g));
}

/* The whole matrix, after a hyperparameter change. */
static void yaep__kmat_build(yaep_gp* g) {
    for (int n = 0; n < g->N; n++) yaep__kmat_row(g, n);
}

/* --- Laplace, one latent ------------------------------------------------ */

/* The relative residual a conjugate-gradient Newton solve stops at, and its
 * iteration budget: past that many iterations a factorization is cheaper at
 * the N where the path is on, and the step falls back to one. */
#ifndef YAEP__NPCG_TOL
#define YAEP__NPCG_TOL (sizeof(yaep_real) == sizeof(double) ? 1e-13 : 1e-6)
#endif
#define YAEP__NPCG_MAX 60

static bool yaep__pcg_on(const yaep_gp* g, int n) {
    int th = g->desc.pcg_threshold == 0 ? 512 : g->desc.pcg_threshold;
    return th >= 0 && n > th;
}

/* Solve B x = rhs, B = I + W^1/2 K W^1/2, by conjugate gradients
 * preconditioned with the factor in g->L. That factor is B's at the previous
 * trial's mode, bordered by the new trial's row, which is B itself at the
 * first Newton step and close to it at every later one, so a step takes a
 * few O(N^2) iterations where a factorization takes N^3 / 3. With u2 set, B
 * is the psychometric model's I + U' K U, sW standing for U's m entries and u2
 * for its g entries against g->Kg. wk holds six N-vectors. Returns the
 * iteration count, or -1 when the budget ran out. */
static int yaep__pcg(const yaep_gp* g, int n, const double* sW, const double* u2,
                      const double* rhs, double rtol, double* x, double* wk) {
    int ld = g->N_max;
    double *r = wk, *z = wk + ld, *pv = wk + 2 * ld, *q = wk + 3 * ld, *u = wk + 4 * ld;
    double *q2 = wk + 5 * ld;
    double rz, rr0 = yaep__dot(rhs, rhs, n), tol;
    for (int i = 0; i < n; i++) { x[i] = 0.0; r[i] = rhs[i]; z[i] = rhs[i]; }
    if (!(rr0 > 0.0)) return 0;
    tol = rtol * rtol * rr0;
    yaep__tri_fwd(g->L, n, ld, z);
    yaep__tri_bwd(g->L, n, ld, z);
    yaep__copy(pv, z, n);
    rz = yaep__dot(r, z, n);
    for (int it = 1; it <= YAEP__NPCG_MAX; it++) {
        double alpha, rz2;
        for (int i = 0; i < n; i++) u[i] = sW[i] * pv[i];
        yaep__gemv(g->Kmat, n, ld, u, q);
        if (u2) {
            for (int i = 0; i < n; i++) u[i] = u2[i] * pv[i];
            yaep__gemv(g->Kg, n, ld, u, q2);
            for (int i = 0; i < n; i++) q[i] = pv[i] + sW[i] * q[i] + u2[i] * q2[i];
        } else {
            for (int i = 0; i < n; i++) q[i] = pv[i] + sW[i] * q[i];
        }
        alpha = rz / yaep__dot(pv, q, n);
        if (!(alpha == alpha)) return -1;
        for (int i = 0; i < n; i++) { x[i] += alpha * pv[i]; r[i] -= alpha * q[i]; }
        if (yaep__dot(r, r, n) <= tol) return it;
        yaep__copy(z, r, n);
        yaep__tri_fwd(g->L, n, ld, z);
        yaep__tri_bwd(g->L, n, ld, z);
        rz2 = yaep__dot(r, z, n);
        for (int i = 0; i < n; i++) pv[i] = z[i] + (rz2 / rz) * pv[i];
        rz = rz2;
    }
    return -1;
}

/* B at the current W into g->L's lower triangle, factored. */
static bool yaep__b_factor(yaep_gp* g, int n, const double* sW, double* logdet) {
    int ld = g->N_max;
    for (int i = 0; i < n; i++) {
        const yaep_real* kr = g->Kmat + (size_t)i * ld;
        yaep_real* br = g->L + (size_t)i * ld;
        double si = sW[i];
        for (int j = 0; j <= i; j++) br[j] = (yaep_real)(si * kr[j] * sW[j]);
        br[i] += 1.0;
    }
    if (!yaep__chol(g->L, n, ld)) return false;
    *logdet = 0.0;
    for (int i = 0; i < n; i++) *logdet += log(g->L[(size_t)i * ld + i]);
    return true;
}

/* Newton's method to the mode of the latent posterior, in the form of
 * Rasmussen & Williams Algorithm 3.1: the step goes through
 * B = I + W^1/2 K W^1/2, whose Cholesky is the only factorization, and the
 * objective Psi = -1/2 a'(f - m) + log p(y|f) guards it with a backtracking
 * line search. Leaves L, W, sqrt(W), the log-likelihood gradient and
 * alpha = K^-1 (fhat - m) in place for prediction, and the Laplace log
 * marginal likelihood (eq. 3.32) in g->log_marginal.
 *
 * `mode` is the start: YAEP__START_COLD from the prior mean,
 * YAEP__START_ROW to reuse the last mode with one trial added, and
 * YAEP__START_KEEP to reuse it as it stands, which is what a hyperparameter
 * step does. */
static int yaep__laplace(yaep_gp* g, int mode) {
    yaep__scr s;
    int n = g->N, ld = g->N_max;
    double mean = yaep__is_pair(g) ? 0.0 : g->hyper.mean;
    double *fv, *W, *d1, *sW, *a, *h, *b, *cv, *an, *hn;
    double psi = 0.0, logdet = 0.0, ll = 0.0;
    bool pcg, border;
    yaep__scr_of(g, &s);
    fv = g->f; W = g->W; d1 = g->grad; sW = s.sW; a = s.alpha;
    h = s.t1; b = s.t2; cv = s.t3; an = s.t4; hn = s.t5;

    /* A start has to be a consistent (alpha, f) pair or Psi is not comparable
     * across steps, so f is always recomputed from alpha rather than carried
     * over. Any alpha gives a consistent pair, which is what makes a warm start
     * safe after the kernel has changed under it. */
    /* Psi at the prior mean, the floor any mode must clear (see
     * yaep__laplace_ps). */
    double psi0 = 0.0;
    for (int i = 0; i < n; i++) {
        double lp, x1, x2, x3;
        yaep__ll(g, mean, g->y[i], &lp, &x1, &x2, &x3);
        psi0 += lp;
    }
    if (mode == YAEP__START_COLD) for (int i = 0; i < n; i++) a[i] = 0.0;
    else if (mode == YAEP__START_ROW) a[n - 1] = 0.0;
    yaep__gemv(g->Kmat, n, ld, a, h);
    for (int i = 0; i < n; i++) fv[i] = mean + h[i];
    /* The conjugate-gradient path needs the previous trial's factor intact,
     * so only an update (one row added to a valid state) takes it. */
    pcg = mode == YAEP__START_ROW && n > 1 && g->fit_valid && g->N_fit == n - 1 &&
          yaep__pcg_on(g, n);
    border = pcg;
    /* desc.fit_pcg: a fit evaluation starts from the last one's mode, and the
     * factor that evaluation left, B at the old hyperparameters, is the
     * preconditioner. */
    if (!pcg && mode == YAEP__START_KEEP && yaep__fpcg_desc(&g->desc) &&
        g->fit_valid && g->N_fit == n)
        pcg = true;
    g->pcg_iters = 0;

    for (int it = 0; it < YAEP__NEWTON_MAX; it++) {
        double ah = 0.0, step = 1.0;
        bool moved = false;
        ll = 0.0;
        for (int i = 0; i < n; i++) {
            double lp, dd1, dd2, dd3, w;
            yaep__ll(g, fv[i], g->y[i], &lp, &dd1, &dd2, &dd3);
            ll += lp;
            d1[i] = dd1;
            w = -dd2;                       /* log-concave: w >= 0 */
            if (!(w > 0.0)) w = 0.0;        /* rounding, or a saturated site */
            if (w > 1e8) w = 1e8;
            W[i] = w;
            sW[i] = sqrt(w);
        }
        if (border && pcg && it == 0) {
            /* Border the previous factor with the new trial's row of B: the
             * old sites' W is unchanged at this point, so the bordered factor
             * is B's own and the first solve converges at once. */
            double* row = s.blk;
            const yaep_real* kn = g->Kmat + (size_t)(n - 1) * ld;
            yaep_real* ln = g->L + (size_t)(n - 1) * ld;
            double dd = 1.0 + W[n - 1] * (double)kn[n - 1];
            for (int j = 0; j < n - 1; j++) row[j] = sW[n - 1] * (double)kn[j] * sW[j];
            yaep__tri_fwd(g->L, n - 1, ld, row);
            dd -= yaep__dot(row, row, n - 1);
            if (dd > 0.0) {
                for (int j = 0; j < n - 1; j++) ln[j] = (yaep_real)row[j];
                ln[n - 1] = (yaep_real)sqrt(dd);
            } else {
                pcg = false;
            }
        }
        /* B = I + W^1/2 K W^1/2, lower triangle only: the Cholesky reads no
         * more than that. */
        if (!pcg && !yaep__b_factor(g, n, sW, &logdet)) return YAEP_ERR_NUMERIC;
        for (int i = 0; i < n; i++) ah += a[i] * h[i];
        psi = -0.5 * ah + ll;

        for (int i = 0; i < n; i++) b[i] = W[i] * h[i] + d1[i];
        yaep__gemv(g->Kmat, n, ld, b, cv);
        for (int i = 0; i < n; i++) cv[i] *= sW[i];
        if (pcg) {
            double* sol = s.blk + (size_t)6 * ld;
            int k = yaep__pcg(g, n, sW, NULL, cv, YAEP__NPCG_TOL, sol, s.blk);
            if (k >= 0) {
                yaep__copy(cv, sol, n);
                g->pcg_iters += k;
            } else {
                /* Out of budget: factor this step and every later one. */
                pcg = false;
                if (!yaep__b_factor(g, n, sW, &logdet)) return YAEP_ERR_NUMERIC;
            }
        }
        if (!pcg) {
            yaep__tri_fwd(g->L, n, ld, cv);
            yaep__tri_bwd(g->L, n, ld, cv);
        }
        for (int i = 0; i < n; i++) an[i] = b[i] - sW[i] * cv[i];
        yaep__gemv(g->Kmat, n, ld, an, hn);

        /* Convergence on the step, not on the objective: the objective is
         * stationary at the mode, so it stops moving while the mode is still
         * micrometers away, and W, L and the log determinant all carry that
         * error into the marginal likelihood and its gradient. */
        {
            double dmax = 0.0, hmax = 0.0;
            for (int i = 0; i < n; i++) {
                double dd = fabs(hn[i] - h[i]);
                if (dd > dmax) dmax = dd;
                if (fabs(h[i]) > hmax) hmax = fabs(h[i]);
            }
            if (dmax <= YAEP__NEWTON_TOL * (1.0 + hmax)) break;
        }

        for (int ls = 0; ls < 20; ls++) {
            double ah2 = 0.0, ll2 = 0.0, psi2;
            for (int i = 0; i < n; i++) {
                double at = a[i] + step * (an[i] - a[i]);
                double ht = h[i] + step * (hn[i] - h[i]);
                double lp, x1, x2, x3;
                ah2 += at * ht;
                yaep__ll(g, mean + ht, g->y[i], &lp, &x1, &x2, &x3);
                ll2 += lp;
            }
            psi2 = -0.5 * ah2 + ll2;
            if (psi2 > psi - 1e-13 * (1.0 + fabs(psi))) {
                for (int i = 0; i < n; i++) {
                    a[i] += step * (an[i] - a[i]);
                    h[i] += step * (hn[i] - h[i]);
                    fv[i] = mean + h[i];
                }
                moved = true;
                break;
            }
            step *= 0.5;
        }
        if (!moved) break;   /* no uphill step left; L still matches f */
    }
    /* The conjugate-gradient path never factored, and prediction and the log
     * determinant need B's factor at the W the loop ended on: one
     * factorization for the update instead of one per Newton step. */
    if (pcg && !yaep__b_factor(g, n, sW, &logdet)) return YAEP_ERR_NUMERIC;
    if (!(psi == psi) || !(logdet == logdet) || psi - psi0 < -1e-9 * (1.0 + fabs(psi0))) {
        if (mode != YAEP__START_COLD) return yaep__laplace(g, YAEP__START_COLD);
        return YAEP_ERR_NUMERIC;
    }
    g->log_marginal = psi - logdet;
    g->N_fit = n;
    g->fit_valid = true;
    return YAEP_OK;
}

/* --- Laplace, K softmax latents ----------------------------------------- */

/* Rasmussen & Williams Algorithm 3.3. The Hessian is no longer diagonal:
 * W = diag(pi) - Pi Pi', so the step needs one Cholesky per class of
 * B_c = I + D_c^1/2 K D_c^1/2 plus one of sum_c E_c with
 * E_c = (K + D_c^-1)^-1, and sum_c E_c is what the class blocks couple
 * through. g->W holds pi, which is the diagonal part. */
static int yaep__laplace_cat(yaep_gp* g, int mode) {
    yaep__scr s;
    int n = g->N, ld = g->N_max, K = g->K;
    size_t bs = (size_t)ld * ld, kn = (size_t)K * ld;
    double mean = g->hyper.mean;
    double *pi, *d1, *sW, *a, *h, *b, *cv, *an, *hn, *r, *z;
    double psi = 0.0, logdet = 0.0, ll = 0.0;
    yaep__scr_of(g, &s);
    pi = g->W; d1 = g->grad; sW = s.sW; a = s.alpha;
    h = s.cat; b = s.cat + kn; an = s.cat + 2 * kn; hn = s.cat + 3 * kn;
    cv = s.t1; r = s.t2; z = s.t3;

    for (int c = 0; c < K; c++) {
        if (mode == YAEP__START_COLD)
            for (int i = 0; i < n; i++) a[(size_t)c * ld + i] = 0.0;
        else if (mode == YAEP__START_ROW) a[(size_t)c * ld + n - 1] = 0.0;
        yaep__gemv(g->Kmat, n, ld, a + (size_t)c * ld, h + (size_t)c * ld);
        for (int i = 0; i < n; i++)
            g->f[(size_t)c * ld + i] = mean + h[(size_t)c * ld + i];
    }

    for (int it = 0; it < YAEP__NEWTON_MAX; it++) {
        double ah = 0.0, step = 1.0;
        bool moved = false;
        ll = 0.0;
        for (int i = 0; i < n; i++) {
            double fs[YAEP_MAX_OUTCOMES] = { 0.0 }, lse;
            int yi = (int)(g->y[i] + 0.5);
            for (int c = 0; c < K; c++) fs[c] = g->f[(size_t)c * ld + i];
            lse = yaep__logsumexp(fs, K);
            ll += fs[yi] - lse;
            for (int c = 0; c < K; c++) {
                double p = exp(fs[c] - lse);
                if (p < 1e-10) p = 1e-10;
                pi[(size_t)c * ld + i] = p;
                sW[(size_t)c * ld + i] = sqrt(p);
                d1[(size_t)c * ld + i] = (c == yi ? 1.0 : 0.0) - p;
            }
        }
        /* One B_c per class, then sum_c E_c and its factor. */
        for (int i = 0; i < n; i++) z[i] = 0.0;
        for (int c = 0; c < K; c++) {
            yaep_real* Lc = g->L + (size_t)c * bs;
            const double* sc = sW + (size_t)c * ld;
            for (int i = 0; i < n; i++) {
                const yaep_real* kr = g->Kmat + (size_t)i * ld;
                yaep_real* br = Lc + (size_t)i * ld;
                double si = sc[i];
                for (int j = 0; j <= i; j++)
                    br[j] = (yaep_real)(si * kr[j] * sc[j]);
                br[i] += (yaep_real)1.0;
            }
            if (!yaep__chol(Lc, n, ld)) return YAEP_ERR_NUMERIC;
            /* E_c = Z'Z with Z = L_c^-1 D_c^1/2. */
            for (int i = 0; i < n; i++)
                yaep__copyr(s.Rm + (size_t)i * ld, Lc + (size_t)i * ld, i + 1);
            yaep__tri_inv(s.Rm, n, ld, cv);
            for (int i = 0; i < n; i++)
                for (int j = 0; j <= i; j++) s.Rm[(size_t)i * ld + j] = (yaep_real)(s.Rm[(size_t)i * ld + j] * sc[j]);
            yaep__tri_sqr(s.Rm, n, ld, cv);
            if (c == 0) {
                for (int i = 0; i < n; i++)
                    yaep__copyr(s.Echol + (size_t)i * ld, s.Rm + (size_t)i * ld, i + 1);
            } else {
                for (int i = 0; i < n; i++)
                    for (int j = 0; j <= i; j++)
                        s.Echol[(size_t)i * ld + j] += s.Rm[(size_t)i * ld + j];
            }
        }
        if (!yaep__chol(s.Echol, n, ld)) return YAEP_ERR_NUMERIC;
        logdet = 0.0;
        for (int i = 0; i < n; i++) logdet += log(s.Echol[(size_t)i * ld + i]);
        for (int c = 0; c < K; c++) {
            const yaep_real* Lc = g->L + (size_t)c * bs;
            for (int i = 0; i < n; i++) logdet += log(Lc[(size_t)i * ld + i]);
        }
        for (size_t i = 0; i < kn; i++) ah += a[i] * h[i];
        psi = -0.5 * ah + ll;

        /* b = (D - Pi Pi') h + (y - pi) */
        for (int i = 0; i < n; i++) {
            double ph = 0.0;
            for (int c = 0; c < K; c++)
                ph += pi[(size_t)c * ld + i] * h[(size_t)c * ld + i];
            for (int c = 0; c < K; c++) {
                size_t o = (size_t)c * ld + i;
                b[o] = pi[o] * (h[o] - ph) + d1[o];
            }
        }
        /* c_c = E_c K b_c and r = sum_c c_c */
        for (int i = 0; i < n; i++) r[i] = 0.0;
        for (int c = 0; c < K; c++) {
            const yaep_real* Lc = g->L + (size_t)c * bs;
            const double* sc = sW + (size_t)c * ld;
            yaep__gemv(g->Kmat, n, ld, b + (size_t)c * ld, cv);
            for (int i = 0; i < n; i++) cv[i] *= sc[i];
            yaep__tri_fwd(Lc, n, ld, cv);
            yaep__tri_bwd(Lc, n, ld, cv);
            for (int i = 0; i < n; i++) {
                cv[i] *= sc[i];
                an[(size_t)c * ld + i] = cv[i];   /* park c_c */
                r[i] += cv[i];
            }
        }
        yaep__tri_fwd(s.Echol, n, ld, r);
        yaep__tri_bwd(s.Echol, n, ld, r);
        /* a_c = b_c - c_c + E_c (sum E)^-1 r */
        for (int c = 0; c < K; c++) {
            const yaep_real* Lc = g->L + (size_t)c * bs;
            const double* sc = sW + (size_t)c * ld;
            for (int i = 0; i < n; i++) z[i] = sc[i] * r[i];
            yaep__tri_fwd(Lc, n, ld, z);
            yaep__tri_bwd(Lc, n, ld, z);
            for (int i = 0; i < n; i++) {
                size_t o = (size_t)c * ld + i;
                an[o] = b[o] - an[o] + sc[i] * z[i];
            }
            yaep__gemv(g->Kmat, n, ld, an + (size_t)c * ld, hn + (size_t)c * ld);
        }
        /* Converge on the step; see yaep__laplace. */
        {
            double dmax = 0.0, hmax = 0.0;
            for (size_t i = 0; i < kn; i++) {
                double dd = fabs(hn[i] - h[i]);
                if (dd > dmax) dmax = dd;
                if (fabs(h[i]) > hmax) hmax = fabs(h[i]);
            }
            if (dmax <= YAEP__NEWTON_TOL * (1.0 + hmax)) break;
        }

        for (int ls = 0; ls < 20; ls++) {
            double ah2 = 0.0, ll2 = 0.0, psi2;
            for (int c = 0; c < K; c++)
                for (int i = 0; i < n; i++) {
                    size_t o = (size_t)c * ld + i;
                    ah2 += (a[o] + step * (an[o] - a[o])) *
                           (h[o] + step * (hn[o] - h[o]));
                }
            for (int i = 0; i < n; i++) {
                double fs[YAEP_MAX_OUTCOMES] = { 0.0 };
                int yi = (int)(g->y[i] + 0.5);
                for (int c = 0; c < K; c++) {
                    size_t o = (size_t)c * ld + i;
                    fs[c] = mean + h[o] + step * (hn[o] - h[o]);
                }
                ll2 += fs[yi] - yaep__logsumexp(fs, K);
            }
            psi2 = -0.5 * ah2 + ll2;
            if (psi2 > psi - 1e-13 * (1.0 + fabs(psi))) {
                for (int c = 0; c < K; c++)
                    for (int i = 0; i < n; i++) {
                        size_t o = (size_t)c * ld + i;
                        a[o] += step * (an[o] - a[o]);
                        h[o] += step * (hn[o] - h[o]);
                        g->f[o] = mean + h[o];
                    }
                moved = true;
                break;
            }
            step *= 0.5;
        }
        if (!moved) break;
    }
    g->log_marginal = psi - logdet;
    g->N_fit = n;
    g->fit_valid = true;
    return YAEP_OK;
}

/* --- exact Gaussian regression ------------------------------------------ */

/* No Newton: the posterior is Gaussian and the factor of K + noise^2 I gains
 * one row per trial in O(N^2), a triangular solve and a square root. `from` is
 * the first row that is not factored yet; 0 rebuilds. sqrt(W) is set to 1
 * because the noise is already inside this factor, which lets prediction and
 * the look-ahead share the Laplace code path. */
static int yaep__gauss_fit(yaep_gp* g, int from) {
    yaep__scr s;
    int n = g->N, ld = g->N_max;
    double nv = g->hyper.noise_sd * g->hyper.noise_sd;
    double logdet = 0.0, quad = 0.0;
    yaep__scr_of(g, &s);
    for (int i = from; i < n; i++) {
        yaep_real* Lr = g->L + (size_t)i * ld;
        double d = g->Kmat[(size_t)i * ld + i] + nv;
        for (int k = 0; k < i; k++) s.t1[k] = g->Kmat[(size_t)i * ld + k];
        yaep__tri_fwd(g->L, i, ld, s.t1);
        for (int k = 0; k < i; k++) {
            Lr[k] = (yaep_real)s.t1[k];
            d -= s.t1[k] * s.t1[k];
        }
        if (!(d > 0.0)) return YAEP_ERR_NUMERIC;
        Lr[i] = (yaep_real)sqrt(d);
    }
    for (int i = 0; i < n; i++) {
        s.alpha[i] = g->y[i] - g->hyper.mean;
        s.sW[i] = 1.0;
        g->W[i] = nv > 0.0 ? 1.0 / nv : 1e8;   /* the look-ahead's precision */
    }
    yaep__tri_fwd(g->L, n, ld, s.alpha);
    yaep__tri_bwd(g->L, n, ld, s.alpha);
    for (int i = 0; i < n; i++) {
        logdet += log(g->L[(size_t)i * ld + i]);
        quad += (g->y[i] - g->hyper.mean) * s.alpha[i];
        g->f[i] = g->hyper.mean;   /* unused, but keeps the field defined */
        g->grad[i] = (g->y[i] - g->hyper.mean) / (nv > 0.0 ? nv : 1e-8);
    }
    g->log_marginal = -0.5 * quad - logdet - 0.5 * (double)n * YAEP__LOG2PI;
    g->N_fit = n;
    g->fit_valid = true;
    return YAEP_OK;
}

/* One trial's worth of posterior update in O(N^2), for desc.refit_every: the
 * same rank-one construction the look-ahead acquisitions use, applied to the
 * training set instead of the candidate set.
 *
 * With W frozen at the values the last exact refit left, adding a trial adds
 * one row and column to B = I + W^1/2 K W^1/2, and the Cholesky of a bordered
 * matrix gains one row by a triangular solve and a square root. The new site's
 * own W comes from the look-ahead value: the predictive mean there, which is
 * K a with the old alpha extended by a zero, through the likelihood. One
 * quasi-Newton step at that frozen W then moves the mode.
 *
 * What it costs in accuracy is the drift of W at the old sites, which is why
 * an exact refit still runs every desc.refit_every trials. What it saves is
 * the refactorization: five matrix-vector products instead of a Cholesky per
 * Newton step. Single-latent discrete likelihoods only. */
static int yaep__laplace_grow(yaep_gp* g) {
    yaep__scr s;
    int n = g->N, ld = g->N_max, m = n - 1;
    double mean = g->hyper.mean;
    double *fv, *W, *d1, *sW, *a, *h, *b, *cv;
    double mu_new, w_new, dd, ll = 0.0, ah = 0.0, logdet = 0.0;
    if (n < 2 || g->N_fit != m) return yaep__laplace(g, YAEP__START_ROW);
    yaep__scr_of(g, &s);
    fv = g->f; W = g->W; d1 = g->grad; sW = s.sW; a = s.alpha;
    h = s.t1; b = s.t2; cv = s.t3;

    a[m] = 0.0;
    yaep__gemv(g->Kmat, n, ld, a, h);
    mu_new = mean + h[m];
    {   /* the new site's curvature at its predictive mean, which is the
         * look-ahead's frozen W. Its gradient is not needed here: the step
         * below reads every site's gradient at the mode it lands on. */
        double lp, dd1, dd2, dd3;
        yaep__ll(g, mu_new, g->y[m], &lp, &dd1, &dd2, &dd3);
        w_new = -dd2;
    }
    if (w_new > 1e8) w_new = 1e8;
    if (!(w_new > 0.0)) w_new = 0.0;
    W[m] = w_new;
    sW[m] = sqrt(w_new);

    for (int i = 0; i < m; i++) cv[i] = sW[i] * g->Kmat[(size_t)m * ld + i] * sW[m];
    yaep__tri_fwd(g->L, m, ld, cv);
    dd = 1.0 + sW[m] * sW[m] * g->Kmat[(size_t)m * ld + m];
    for (int i = 0; i < m; i++) {
        g->L[(size_t)m * ld + i] = (yaep_real)cv[i];
        dd -= cv[i] * cv[i];
    }
    if (!(dd > 0.0)) return YAEP_ERR_NUMERIC;
    g->L[(size_t)m * ld + m] = (yaep_real)sqrt(dd);

    for (int i = 0; i < n; i++) {
        double lp, dd1, dd2, dd3;
        fv[i] = mean + h[i];
        yaep__ll(g, fv[i], g->y[i], &lp, &dd1, &dd2, &dd3);
        d1[i] = dd1;
        b[i] = W[i] * h[i] + dd1;
    }
    yaep__gemv(g->Kmat, n, ld, b, cv);
    for (int i = 0; i < n; i++) cv[i] *= sW[i];
    yaep__tri_fwd(g->L, n, ld, cv);
    yaep__tri_bwd(g->L, n, ld, cv);
    for (int i = 0; i < n; i++) a[i] = b[i] - sW[i] * cv[i];
    yaep__gemv(g->Kmat, n, ld, a, h);
    for (int i = 0; i < n; i++) {
        double lp, dd1, dd2, dd3;
        fv[i] = mean + h[i];
        yaep__ll(g, fv[i], g->y[i], &lp, &dd1, &dd2, &dd3);
        d1[i] = dd1;
        ll += lp;
        ah += a[i] * h[i];
    }
    for (int i = 0; i < n; i++) logdet += log(g->L[(size_t)i * ld + i]);
    g->log_marginal = -0.5 * ah + ll - logdet;
    g->N_fit = n;
    g->fit_valid = true;
    return YAEP_OK;
}

/* --- Laplace, the psychometric model ------------------------------------ */

static double* yaep__psv(const yaep__scr* s, int ld, int k) {
    return s->ps + (size_t)k * ld;
}

/* The site values of every trial at the current h = z - mean: the latent
 * f = b (x - m) with b = exp(g), the log-likelihood and its gradient in (m, g),
 * and the Gauss-Newton square root U. Returns the log-likelihood.
 *
 * The exact Hessian of log p(y | m, g) is J' l'' J + l' d2f, with J = (-b, f)
 * the gradient of f and d2f = [0, -b; -b, f] its Hessian. The second term has
 * determinant -b^2 l'^2, so it is indefinite at every trial with a nonzero
 * residual, and the exact Hessian with it. The Laplace step keeps the first
 * term only, with l'' clamped to <= 0 as the one-latent model does under a
 * floor: W = sum_i w_i J_i J_i' is then positive semidefinite and rank one per
 * trial, so W = U U' with U N columns wide, and every 2N x 2N system below
 * collapses to N x N through B = I + U' K U. The mode is the exact mode all the
 * same, since the iteration's fixed point is where the gradient of the
 * log posterior vanishes; W decides only the step and the Gaussian's
 * curvature. */
static double yaep__ps_sites(yaep_gp* g, const yaep__scr* s, const double* hm,
                              const double* hg, bool fill) {
    int n = g->N, ld = g->N_max, nd = g->desc.n_dims, id = g->desc.intensity_dim;
    double mum = g->hyper.mean, mug = g->hyper.mean_g, ll = 0.0;
    double *um = yaep__psv(s, ld, YAEP__PS_UM), *ug = yaep__psv(s, ld, YAEP__PS_UG);
    double *gm = yaep__psv(s, ld, YAEP__PS_GM), *gg = yaep__psv(s, ld, YAEP__PS_GG);
    for (int i = 0; i < n; i++) {
        double xi = g->X[(size_t)i * nd + id];
        double b = exp(yaep__clamp(mug + hg[i], -50.0, 50.0));
        double fv = b * (xi - (mum + hm[i]));
        double lp, d1, d2, d3, w, sw;
        yaep__ll(g, fv, g->y[i], &lp, &d1, &d2, &d3);
        ll += lp;
        if (!fill) continue;
        w = -d2;
        if (!(w > 0.0)) w = 0.0;
        if (w > 1e8) w = 1e8;
        sw = sqrt(w);
        g->f[i] = fv;
        g->W[i] = w;
        g->grad[i] = d1;
        gm[i] = -d1 * b;
        gg[i] = d1 * fv;
        um[i] = -sw * b;
        ug[i] = sw * fv;
    }
    return ll;
}

/* Newton (Gauss-Newton, see above) to the mode of the 2N latents in R&W
 * Algorithm 3.1's form: a is the pair of K^-1 (z - mean) blocks, the step is
 * a_new = b - U B^-1 U' K b with b = W h + grad, and the same backtracking line
 * search on Psi = -1/2 a'h + log p(y|z) guards it. Leaves L = chol(B), U, a and
 * the Laplace log marginal likelihood in place. */
/* The Gauss-Newton step converges linearly, not quadratically, so the last
 * three digits of the one-latent model's 1e-11 cost about seven iterations of
 * seventeen. Measured on a 2-D observer over three 150-trial sessions: 1e-8
 * changes no threshold by more than 0.04 dB and no MAE(p) by more than 5e-4,
 * and takes 40% less time. */
#ifndef YAEP__PS_TOL
#define YAEP__PS_TOL (sizeof(yaep_real) == sizeof(double) ? 1e-8 : 1e-5)
#endif

/* B = I + U' K U at the current U, lower triangle, into g->L, factored. */
static bool yaep__ps_b_factor(yaep_gp* g, int n, const double* um, const double* ug) {
    int ld = g->N_max;
    for (int i = 0; i < n; i++) {
        const yaep_real* kr = g->Kmat + (size_t)i * ld;
        const yaep_real* gr = g->Kg + (size_t)i * ld;
        yaep_real* br = g->L + (size_t)i * ld;
        for (int j = 0; j <= i; j++)
            br[j] = (yaep_real)(um[i] * kr[j] * um[j] + ug[i] * gr[j] * ug[j]);
        br[i] += 1.0;
    }
    return yaep__chol(g->L, n, ld);
}

/* desc.fit_pcg's mode search: yaep__laplace_ps with every Gauss-Newton solve
 * a conjugate-gradient one preconditioned by the newest factor in g->L, and a
 * factorization only when there is none to use (a cold start), when a solve
 * runs out of iterations, and once at the end for the log determinant and the
 * predictions. The Gauss-Newton search converges linearly and takes 20 to 30
 * steps at N = 500; the factorized search factors at every one. */
static int yaep__laplace_ps_cg(yaep_gp* g, int mode) {
    yaep__scr s;
    int n = g->N, ld = g->N_max;
    double *am, *ag, *hm, *hg, *um, *ug, *gm, *gg, *bm, *bg, *nm, *ng, *qm, *qg, *t;
    double psi = 0.0, logdet = 0.0, psi0;
    bool have, fresh = false;
    yaep__scr_of(g, &s);
    am = yaep__psv(&s, ld, YAEP__PS_AM); ag = yaep__psv(&s, ld, YAEP__PS_AG);
    hm = yaep__psv(&s, ld, YAEP__PS_HM); hg = yaep__psv(&s, ld, YAEP__PS_HG);
    um = yaep__psv(&s, ld, YAEP__PS_UM); ug = yaep__psv(&s, ld, YAEP__PS_UG);
    gm = yaep__psv(&s, ld, YAEP__PS_GM); gg = yaep__psv(&s, ld, YAEP__PS_GG);
    bm = yaep__psv(&s, ld, YAEP__PS_BM); bg = yaep__psv(&s, ld, YAEP__PS_BG);
    nm = yaep__psv(&s, ld, YAEP__PS_NM); ng = yaep__psv(&s, ld, YAEP__PS_NG);
    qm = yaep__psv(&s, ld, YAEP__PS_QM); qg = yaep__psv(&s, ld, YAEP__PS_QG);
    t = s.t1;
    for (int i = 0; i < n; i++) bm[i] = bg[i] = 0.0;
    psi0 = yaep__ps_sites(g, &s, bm, bg, false);
    have = g->fit_valid && mode != YAEP__START_COLD &&
           (g->N_fit == n || (mode == YAEP__START_ROW && g->N_fit == n - 1 && n > 1));
    if (mode == YAEP__START_COLD) {
        for (int i = 0; i < n; i++) am[i] = ag[i] = 0.0;
    } else if (mode == YAEP__START_ROW) {
        am[n - 1] = ag[n - 1] = 0.0;
    }
    yaep__gemv(g->Kmat, n, ld, am, hm);
    yaep__gemv(g->Kg, n, ld, ag, hg);
    g->pcg_iters = 0;

    for (int it = 0; ; it++) {
        double ll, step = 1.0;
        bool moved = false;
        ll = yaep__ps_sites(g, &s, hm, hg, true);
        fresh = false;
        if (have && it == 0 && g->N_fit == n - 1) {
            /* A new trial: the old sites' h and U are unchanged, so the old
             * factor bordered by the new row is B itself. */
            double* row = s.blk;
            const yaep_real* kn = g->Kmat + (size_t)(n - 1) * ld;
            const yaep_real* gn = g->Kg + (size_t)(n - 1) * ld;
            yaep_real* ln = g->L + (size_t)(n - 1) * ld;
            double dd = 1.0 + um[n - 1] * (double)kn[n - 1] * um[n - 1] +
                        ug[n - 1] * (double)gn[n - 1] * ug[n - 1];
            for (int j = 0; j < n - 1; j++)
                row[j] = um[n - 1] * (double)kn[j] * um[j] + ug[n - 1] * (double)gn[j] * ug[j];
            yaep__tri_fwd(g->L, n - 1, ld, row);
            dd -= yaep__dot(row, row, n - 1);
            if (dd > 0.0) {
                for (int j = 0; j < n - 1; j++) ln[j] = (yaep_real)row[j];
                ln[n - 1] = (yaep_real)sqrt(dd);
                fresh = true;
            } else {
                have = false;
            }
        }
        if (!have) {
            if (!yaep__ps_b_factor(g, n, um, ug)) return YAEP_ERR_NUMERIC;
            have = fresh = true;
        }
        psi = -0.5 * (yaep__dot(am, hm, n) + yaep__dot(ag, hg, n)) + ll;
        if (it >= YAEP__NEWTON_MAX) break;

        for (int i = 0; i < n; i++) {
            double r = um[i] * hm[i] + ug[i] * hg[i];
            bm[i] = um[i] * r + gm[i];
            bg[i] = ug[i] * r + gg[i];
        }
        yaep__gemv(g->Kmat, n, ld, bm, nm);
        yaep__gemv(g->Kg, n, ld, bg, ng);
        for (int i = 0; i < n; i++) t[i] = um[i] * nm[i] + ug[i] * ng[i];
        if (fresh) {
            yaep__tri_fwd(g->L, n, ld, t);
            yaep__tri_bwd(g->L, n, ld, t);
        } else {
            double* sol = s.blk + (size_t)6 * ld;
            int k = yaep__pcg(g, n, um, ug, t, YAEP__NPCG_TOL, sol, s.blk);
            if (k >= 0) {
                yaep__copy(t, sol, n);
                g->pcg_iters += k;
            } else {
                if (!yaep__ps_b_factor(g, n, um, ug)) return YAEP_ERR_NUMERIC;
                fresh = true;
                yaep__tri_fwd(g->L, n, ld, t);
                yaep__tri_bwd(g->L, n, ld, t);
            }
        }
        for (int i = 0; i < n; i++) {
            nm[i] = bm[i] - um[i] * t[i];
            ng[i] = bg[i] - ug[i] * t[i];
        }
        yaep__gemv(g->Kmat, n, ld, nm, qm);
        yaep__gemv(g->Kg, n, ld, ng, qg);
        {
            double dmax = 0.0, hmax = 0.0;
            for (int i = 0; i < n; i++) {
                double d1 = fabs(qm[i] - hm[i]), d2 = fabs(qg[i] - hg[i]);
                if (d1 > dmax) dmax = d1;
                if (d2 > dmax) dmax = d2;
                if (fabs(hm[i]) > hmax) hmax = fabs(hm[i]);
                if (fabs(hg[i]) > hmax) hmax = fabs(hg[i]);
            }
            if (dmax <= g->ps_tol * (1.0 + hmax)) break;
        }
        for (int ls = 0; ls < 20; ls++) {
            double ah2 = 0.0, psi2;
            for (int i = 0; i < n; i++) {
                double atm = am[i] + step * (nm[i] - am[i]);
                double atg = ag[i] + step * (ng[i] - ag[i]);
                bm[i] = hm[i] + step * (qm[i] - hm[i]);
                bg[i] = hg[i] + step * (qg[i] - hg[i]);
                ah2 += atm * bm[i] + atg * bg[i];
            }
            psi2 = -0.5 * ah2 + yaep__ps_sites(g, &s, bm, bg, false);
            if (psi2 > psi - 1e-13 * (1.0 + fabs(psi))) {
                for (int i = 0; i < n; i++) {
                    am[i] += step * (nm[i] - am[i]);
                    ag[i] += step * (ng[i] - ag[i]);
                    hm[i] = bm[i];
                    hg[i] = bg[i];
                }
                moved = true;
                break;
            }
            step *= 0.5;
        }
        if (!moved) break;   /* the sites are still the ones at h */
    }
    /* The log determinant, the predictions and the gradient need B's factor
     * at the sites the search ended on. */
    if (!fresh && !yaep__ps_b_factor(g, n, um, ug)) return YAEP_ERR_NUMERIC;
    logdet = 0.0;
    for (int i = 0; i < n; i++) logdet += log(g->L[(size_t)i * ld + i]);
    if (!(psi == psi) || !(logdet == logdet) || psi - psi0 < -1e-9 * (1.0 + fabs(psi0))) {
        if (mode != YAEP__START_COLD) return yaep__laplace_ps_cg(g, YAEP__START_COLD);
        return YAEP_ERR_NUMERIC;
    }
    g->log_marginal = psi - logdet;
    g->N_fit = n;
    g->fit_valid = true;
    return YAEP_OK;
}

static int yaep__laplace_ps(yaep_gp* g, int mode) {
    yaep__scr s;
    int n = g->N, ld = g->N_max;
    double *am, *ag, *hm, *hg, *um, *ug, *gm, *gg, *bm, *bg, *nm, *ng, *qm, *qg, *t;
    double psi = 0.0, logdet = 0.0, psi0;
    yaep__scr_of(g, &s);
    am = yaep__psv(&s, ld, YAEP__PS_AM); ag = yaep__psv(&s, ld, YAEP__PS_AG);
    hm = yaep__psv(&s, ld, YAEP__PS_HM); hg = yaep__psv(&s, ld, YAEP__PS_HG);
    um = yaep__psv(&s, ld, YAEP__PS_UM); ug = yaep__psv(&s, ld, YAEP__PS_UG);
    gm = yaep__psv(&s, ld, YAEP__PS_GM); gg = yaep__psv(&s, ld, YAEP__PS_GG);
    bm = yaep__psv(&s, ld, YAEP__PS_BM); bg = yaep__psv(&s, ld, YAEP__PS_BG);
    nm = yaep__psv(&s, ld, YAEP__PS_NM); ng = yaep__psv(&s, ld, YAEP__PS_NG);
    qm = yaep__psv(&s, ld, YAEP__PS_QM); qg = yaep__psv(&s, ld, YAEP__PS_QG);
    t = s.t1;
    if (g->desc.fit_pcg) return yaep__laplace_ps_cg(g, mode);

    /* Psi at the prior mean, a = 0: the mode maximizes Psi, so a result below
     * this is not the mode, whatever the iteration says about its steps. */
    for (int i = 0; i < n; i++) bm[i] = bg[i] = 0.0;
    psi0 = yaep__ps_sites(g, &s, bm, bg, false);
    if (mode == YAEP__START_COLD) {
        for (int i = 0; i < n; i++) am[i] = ag[i] = 0.0;
    } else if (mode == YAEP__START_ROW) {
        am[n - 1] = ag[n - 1] = 0.0;
    }
    yaep__gemv(g->Kmat, n, ld, am, hm);
    yaep__gemv(g->Kg, n, ld, ag, hg);

    for (int it = 0; ; it++) {
        double ll, step = 1.0;
        bool moved = false;
        ll = yaep__ps_sites(g, &s, hm, hg, true);
        /* B = I + U' K U, lower triangle only. */
        for (int i = 0; i < n; i++) {
            const yaep_real* kr = g->Kmat + (size_t)i * ld;
            const yaep_real* gr = g->Kg + (size_t)i * ld;
            yaep_real* br = g->L + (size_t)i * ld;
            for (int j = 0; j <= i; j++)
                br[j] = (yaep_real)(um[i] * kr[j] * um[j] + ug[i] * gr[j] * ug[j]);
            br[i] += 1.0;
        }
        if (!yaep__chol(g->L, n, ld)) return YAEP_ERR_NUMERIC;
        logdet = 0.0;
        for (int i = 0; i < n; i++) logdet += log(g->L[(size_t)i * ld + i]);
        psi = -0.5 * (yaep__dot(am, hm, n) + yaep__dot(ag, hg, n)) + ll;
        if (it >= YAEP__NEWTON_MAX) break;   /* L, U and psi match the last h */

        for (int i = 0; i < n; i++) {
            double r = um[i] * hm[i] + ug[i] * hg[i];
            bm[i] = um[i] * r + gm[i];
            bg[i] = ug[i] * r + gg[i];
        }
        yaep__gemv(g->Kmat, n, ld, bm, nm);
        yaep__gemv(g->Kg, n, ld, bg, ng);
        for (int i = 0; i < n; i++) t[i] = um[i] * nm[i] + ug[i] * ng[i];
        yaep__tri_fwd(g->L, n, ld, t);
        yaep__tri_bwd(g->L, n, ld, t);
        for (int i = 0; i < n; i++) {
            nm[i] = bm[i] - um[i] * t[i];
            ng[i] = bg[i] - ug[i] * t[i];
        }
        yaep__gemv(g->Kmat, n, ld, nm, qm);
        yaep__gemv(g->Kg, n, ld, ng, qg);
        {
            double dmax = 0.0, hmax = 0.0;
            for (int i = 0; i < n; i++) {
                double d1 = fabs(qm[i] - hm[i]), d2 = fabs(qg[i] - hg[i]);
                if (d1 > dmax) dmax = d1;
                if (d2 > dmax) dmax = d2;
                if (fabs(hm[i]) > hmax) hmax = fabs(hm[i]);
                if (fabs(hg[i]) > hmax) hmax = fabs(hg[i]);
            }
            if (dmax <= g->ps_tol * (1.0 + hmax)) break;
        }
        for (int ls = 0; ls < 20; ls++) {
            double ah2 = 0.0, psi2;
            for (int i = 0; i < n; i++) {
                double atm = am[i] + step * (nm[i] - am[i]);
                double atg = ag[i] + step * (ng[i] - ag[i]);
                bm[i] = hm[i] + step * (qm[i] - hm[i]);
                bg[i] = hg[i] + step * (qg[i] - hg[i]);
                ah2 += atm * bm[i] + atg * bg[i];
            }
            psi2 = -0.5 * ah2 + yaep__ps_sites(g, &s, bm, bg, false);
            if (psi2 > psi - 1e-13 * (1.0 + fabs(psi))) {
                for (int i = 0; i < n; i++) {
                    am[i] += step * (nm[i] - am[i]);
                    ag[i] += step * (ng[i] - ag[i]);
                    hm[i] = bm[i];
                    hg[i] = bg[i];
                }
                moved = true;
                break;
            }
            step *= 0.5;
        }
        if (!moved) {
            /* No uphill step: the sites and L are still the ones at h. */
            break;
        }
    }
    /* A warm start far from the mode (a hyperparameter step that rescaled K
     * under the old a, so h = K a and exp(g) are enormous) can run the
     * Gauss-Newton iteration into its step cap on a plateau where the link
     * has saturated: Psi near -1e49, every gradient zero, and nothing above
     * to flag it. A result that is not finite, or scores below the prior
     * mean, is not the mode; start again from the prior mean, and if that
     * fails too, the posterior is not usable and the caller hears so. */
    if (!(psi == psi) || !(logdet == logdet) || psi - psi0 < -1e-9 * (1.0 + fabs(psi0))) {
        if (mode != YAEP__START_COLD) return yaep__laplace_ps(g, YAEP__START_COLD);
        return YAEP_ERR_NUMERIC;
    }
    g->log_marginal = psi - logdet;
    g->N_fit = n;
    g->fit_valid = true;
    return YAEP_OK;
}

/* Whether this configuration may take the cheap update at all. */
static bool yaep__can_grow(const yaep_gp* g) {
    return g->desc.refit_every > 1 && g->K == 1 &&
           g->desc.lik != YAEP_LIK_GAUSSIAN && !yaep__is_ps(g);
}

/* One entry point for all four: refit the posterior at the current data and
 * hyperparameters. `grow` is the row the factor is valid up to, so an update
 * can add a row under GAUSSIAN instead of refactoring. */
static int yaep__infer(yaep_gp* g, int mode, int grow) {
    if (g->N == 0) {
        g->N_fit = 0;
        g->fit_valid = true;
        g->log_marginal = 0.0;
        return YAEP_OK;
    }
    if (g->desc.lik == YAEP_LIK_GAUSSIAN)
        return yaep__gauss_fit(g, mode == YAEP__START_ROW ? grow : 0);
    if (yaep__is_ps(g)) return yaep__laplace_ps(g, mode);
    if (g->K > 1) return yaep__laplace_cat(g, mode);
    return yaep__laplace(g, mode);
}

/* --- prediction --------------------------------------------------------- */

/* Latent k at x: Rasmussen & Williams Algorithm 3.2, with the extra term the
 * softmax posterior adds through sum_c E_c (eq. 3.41). w1 and w2 are N-vectors
 * of workspace the caller owns. */
static void yaep__predict_k(const yaep_gp* g, const yaep__scr* s,
                             const double* x, int k, double* mu, double* var,
                             double* w1, double* w2) {
    int n = g->N_fit, ld = g->N_max, nd = g->desc.n_dims;
    double kxx = yaep__kernel(g, x, x), m = 0.0, v;
    const double* al = s->alpha + (size_t)k * ld;
    const double* sw = s->sW + (size_t)k * ld;
    const yaep_real* Lk = g->L + (size_t)k * ld * ld;
    if (n == 0) { *mu = g->hyper.mean; *var = kxx; return; }
    (void)nd;
    for (int i = 0; i < n; i++) w1[i] = yaep__kx(g, i, x);
    m = yaep__dot(al, w1, n);
    for (int i = 0; i < n; i++) w2[i] = sw[i] * w1[i];
    yaep__tri_fwd(Lk, n, ld, w2);
    v = kxx - yaep__dot(w2, w2, n);
    if (g->K > 1) {
        yaep__tri_bwd(Lk, n, ld, w2);
        for (int i = 0; i < n; i++) w2[i] *= sw[i];
        yaep__tri_fwd(s->Echol, n, ld, w2);
        for (int i = 0; i < n; i++) v += w2[i] * w2[i];
    }
    *mu = g->hyper.mean + m;
    *var = v > 0.0 ? v : 0.0;
}

/* Latent means and sds at every candidate, in blocks of YAEP__BLK so the
 * triangular solve streams the factor once per block and not once per
 * candidate. When `kv` is a latent index, its L^-1 W^1/2 k(X, c) columns are
 * kept in V for the look-ahead covariance. */
/* Latent k's mean and sd at n points, in blocks of YAEP__BLK so the triangular
 * solve streams the factor once per block instead of once per point. This is the
 * same path the candidate cache uses, which is why a field plot costs what one
 * acquisition sweep costs rather than n times a single prediction. */
static void yaep__predict_many(const yaep_gp* g, const yaep__scr* s,
                                const double* xs, int n, int k,
                                double* mu, double* sd) {
    int nf = g->N_fit, ld = g->N_max, nd = g->desc.n_dims;
    const double* al = s->alpha + (size_t)k * ld;
    const double* sw = s->sW + (size_t)k * ld;
    const yaep_real* Lk = g->L + (size_t)k * ld * ld;
    for (int b0 = 0; b0 < n; b0 += YAEP__BLK) {
        int b1 = b0 + YAEP__BLK < n ? b0 + YAEP__BLK : n;
        double kxx[YAEP__BLK] = { 0 };
        for (int j = b0; j < b1; j++) {
            const double* xj = xs + (size_t)j * nd;
            double* row = s->blk + (size_t)(j - b0) * ld;
            kxx[j - b0] = yaep__kernel(g, xj, xj);
            for (int i = 0; i < nf; i++)
                row[i] = yaep__kx(g, i, xj);
        }
        for (int j = b0; j < b1; j++) {
            const double* row = s->blk + (size_t)(j - b0) * ld;
            double* wv = s->blk + (size_t)(YAEP__BLK + j - b0) * ld;
            double v = kxx[j - b0];
            mu[j] = g->hyper.mean + yaep__dot(al, row, nf);
            if (nf == 0) { sd[j] = sqrt(v); continue; }
            for (int i = 0; i < nf; i++) wv[i] = sw[i] * row[i];
            yaep__tri_fwd(Lk, nf, ld, wv);
            v -= yaep__dot(wv, wv, nf);
            if (g->K > 1) {
                double* w2 = s->t6;
                yaep__copy(w2, wv, nf);
                yaep__tri_bwd(Lk, nf, ld, w2);
                for (int i = 0; i < nf; i++) w2[i] *= sw[i];
                yaep__tri_fwd(s->Echol, nf, ld, w2);
                v += yaep__dot(w2, w2, nf);
            }
            sd[j] = v > 0.0 ? sqrt(v) : 0.0;
        }
    }
}

/* --- monotonic projection ---------------------------------------------- */

/* The candidate line along dimension d: the product grid's values when there
 * is one, else as many evenly spaced points as M candidates would have along
 * one side of a regular grid, 9 to 65 of them. */
static int yaep__line_n(const yaep_gp* g, int d) {
    const yaep_desc* ds = &g->desc;
    int n, any = 0;
    if (!ds->candidates) {
        for (int i = 0; i < ds->n_dims; i++) if (ds->grid[i] != 0) any = 1;
        if (any) return yaep__grid_n(ds, d);
    }
    n = (int)ceil(pow((double)g->M, 1.0 / (double)ds->n_dims));
    if (n < 9) n = 9;
    if (n > 65) n = 65;
    return n;
}

#define YAEP__LINE_MAX 66

/* Latent 0's posterior mean alone: the projection needs the mean at many
 * points and never their variances, and the mean is one kernel row and a dot
 * product, where the variance is a triangular solve. */
static double yaep__mean0(const yaep_gp* g, const yaep__scr* s, const double* x,
                           double* w) {
    int n = g->N_fit;
    if (n == 0) return g->hyper.mean;
    for (int i = 0; i < n; i++) w[i] = yaep__kx(g, i, x);
    return g->hyper.mean + yaep__dot(s->alpha, w, n);
}

/* AEPsych's MonotonicProjectionGP: the posterior mean at x replaced by its
 * largest value over the points below x on the candidate lines through x
 * along every masked dimension (their product when several are masked), x
 * itself included, whose mean the caller passes in mu. A projection of the
 * mean; the variance and the posterior itself are left alone. */
static double yaep__mono_mu(const yaep_gp* g, const yaep__scr* s, const double* x,
                             double mu, double* w) {
    const yaep_desc* d = &g->desc;
    double vals[YAEP_MAX_DIMS][YAEP__LINE_MAX], y[YAEP_MAX_DIMS] = { 0 };
    int md[YAEP_MAX_DIMS] = { 0 }, cnt[YAEP_MAX_DIMS] = { 0 }, at[YAEP_MAX_DIMS] = { 0 }, nm = 0;
    int nd = d->n_dims;
    double best = mu;
    for (int i = 0; i < nd; i++) {
        int n, c = 0;
        if (!((d->monotone_dims >> i) & 1u)) continue;
        n = yaep__line_n(g, i);
        for (int k = 0; k < n; k++) {
            double v = yaep__coord(d, i, n == 1 ? 0.5 * (d->lo[i] + d->hi[i])
                       : d->lo[i] + (d->hi[i] - d->lo[i]) * (double)k / (double)(n - 1));
            if (v < x[i]) vals[nm][c++] = v;
        }
        vals[nm][c++] = x[i];
        md[nm] = i;
        cnt[nm] = c;
        at[nm] = 0;
        nm++;
    }
    yaep__copy(y, x, nd);
    for (;;) {
        bool self = true;
        for (int m = 0; m < nm; m++) {
            y[md[m]] = vals[m][at[m]];
            if (at[m] != cnt[m] - 1) self = false;
        }
        if (!self) {
            double v = yaep__mean0(g, s, y, w);
            if (v > best) best = v;
        }
        {
            int m = 0;
            while (m < nm && ++at[m] == cnt[m]) at[m++] = 0;
            if (m == nm) break;
        }
    }
    return best;
}

/* The level-set acquisitions read the projected mean. */
static bool yaep__mono_acq(const yaep_gp* g) {
    yaep_acq a = g->desc.acq;
    return g->desc.monotone_dims != 0u &&
           (a == YAEP_ACQ_LSE || a == YAEP_ACQ_EAVC || a == YAEP_ACQ_LOCALMI);
}

/* The projection of every candidate's mean. On a product grid the lines are
 * the grid's own, and a running maximum along each masked dimension in turn
 * gives the maximum over the lower orthant in O(M); any other set takes the
 * point-by-point projection. */
static void yaep__mono_cands(yaep_gp* g, const yaep__scr* s) {
    const yaep_desc* d = &g->desc;
    int M = g->M, nd = d->n_dims, any = 0;
    double* mu = g->cand_mu;
    if (!d->candidates) for (int i = 0; i < nd; i++) if (d->grid[i] != 0) any = 1;
    if (any) {
        for (int i = 0; i < nd; i++) {
            int stride = 1, n = yaep__grid_n(d, i);
            if (!((d->monotone_dims >> i) & 1u)) continue;
            for (int k = i + 1; k < nd; k++) stride *= yaep__grid_n(d, k);
            for (int j = 0; j < M; j++)
                if ((j / stride) % n > 0 && mu[j - stride] > mu[j]) mu[j] = mu[j - stride];
        }
        return;
    }
    for (int j = 0; j < M; j++)
        mu[j] = yaep__mono_mu(g, s, yaep__cand(g, j), mu[j], s->t1);
}

static void yaep__cand_fill(yaep_gp* g, int kv) {
    yaep__scr s;
    int n = g->N_fit, ld = g->N_max, M = g->M, K = g->K;
    yaep__scr_of(g, &s);
    for (int b0 = 0; b0 < M; b0 += YAEP__BLK) {
        int b1 = b0 + YAEP__BLK < M ? b0 + YAEP__BLK : M;
        double kxx[YAEP__BLK] = { 0 };
        for (int j = b0; j < b1; j++) {
            const double* cj = yaep__cand(g, j);
            double* row = s.blk + (size_t)(j - b0) * ld;
            kxx[j - b0] = yaep__kernel(g, cj, cj);
            for (int i = 0; i < n; i++)
                row[i] = yaep__kx(g, i, cj);
        }
        for (int k = 0; k < K; k++) {
            const double* al = s.alpha + (size_t)k * ld;
            const double* sw = s.sW + (size_t)k * ld;
            const yaep_real* Lk = g->L + (size_t)k * ld * ld;
            for (int j = b0; j < b1; j++) {
                const double* row = s.blk + (size_t)(j - b0) * ld;
                double* wv = s.blk + (size_t)(YAEP__BLK + j - b0) * ld;
                double v = kxx[j - b0];
                g->cand_mu[(size_t)k * M + j] = g->hyper.mean +
                                                yaep__dot(al, row, n);
                if (n == 0) { s.cand_sd[(size_t)k * M + j] = sqrt(v); continue; }
                for (int i = 0; i < n; i++) wv[i] = sw[i] * row[i];
                yaep__tri_fwd(Lk, n, ld, wv);
                v -= yaep__dot(wv, wv, n);
                if (kv == k && s.V) {
                    yaep_real* vrow = s.V + (size_t)j * ld;
                    for (int i = 0; i < n; i++) vrow[i] = (yaep_real)wv[i];
                }
                if (K > 1) {
                    double* w2 = s.t6;
                    yaep__copy(w2, wv, n);
                    yaep__tri_bwd(Lk, n, ld, w2);
                    for (int i = 0; i < n; i++) w2[i] *= sw[i];
                    yaep__tri_fwd(s.Echol, n, ld, w2);
                    for (int i = 0; i < n; i++) v += w2[i] * w2[i];
                }
                s.cand_sd[(size_t)k * M + j] = v > 0.0 ? sqrt(v) : 0.0;
            }
        }
    }
}

/* The M x M latent covariance among the candidates for latent kv, which is the
 * one object the look-ahead needs beyond the per-candidate marginals: with W
 * frozen, every look-ahead variance is a rank-one update of it. Under
 * CATEGORICAL the between-class term of eq. 3.41 is dropped here, on top of the
 * one-against-the-rest approximation the manual states. */
static void yaep__cand_cov_build(yaep_gp* g, int kv) {
    yaep__scr s;
    int n = g->N_fit, M = g->M, ld = g->N_max;
    yaep__scr_of(g, &s);
    yaep__cand_fill(g, kv);
    for (int i = 0; i < M; i++) {
        const double* ci = yaep__cand(g, i);
        const yaep_real* vi = s.V + (size_t)i * ld;
        for (int j = 0; j <= i; j++) {
            const yaep_real* vj = s.V + (size_t)j * ld;
            double c = yaep__kernel(g, ci, yaep__cand(g, j)) -
                       yaep__dotrr(vi, vj, n);
            g->cand_cov[(size_t)i * M + j] = (yaep_real)c;
            g->cand_cov[(size_t)j * M + i] = (yaep_real)c;
        }
    }
}

/* --- marginal likelihood and its gradient ------------------------------- */

/* The objective of a hyperparameter fit, and its derivative in the fitted
 * coordinates. The value is the Laplace approximation to the log marginal
 * likelihood (Rasmussen & Williams eq. 3.32), or the exact one under GAUSSIAN
 * (eq. 2.30). The gradient is analytic: eqs. 5.22 to 5.24 for Laplace, which
 * means an explicit part through K and an implicit part through the mode, and
 * eq. 5.9 for the Gaussian case, which has no implicit part. Every call leaves
 * the handle's posterior at `th`.
 *
 * The one-pass structure matters: the derivative of the kernel in every
 * hyperparameter is read off one visit to each matrix element, so the whole
 * gradient costs one pass and not one pass per hyperparameter. */
/* Weak priors on the two hyperparameters an unregularized type-II fit runs away
 * with: log-normal on the output scales, normal on the mean, both centered on
 * the defaults. Data that nearly separate (a run where almost every trial came
 * back the same way) let the marginal likelihood keep improving as the output
 * scale and the mean grow, and it stops only at the bounds: the posterior is
 * then diffuse, a level-set acquisition follows the variance to the edges of
 * the box instead of the threshold, and the run never recovers. The likelihood
 * near that bound is flat, so a prior this weak is enough to hold the fit at a
 * sane place while data that really do insist still move it. This is what makes
 * the fit a MAP estimate rather than type-II maximum likelihood; yaep_log_
 * marginal() still reports the likelihood alone. */
/* How many trials in a row one candidate may win before yaep_next() breaks the
 * loop with a Halton point. */
#define YAEP__REPEAT_MAX 3

/* The psychometric model's mean log-slope gets a normal prior as well, centered
 * on its default: separable responses favor an infinitely steep function just as
 * they favor an infinite output scale, and a step is worse than useless here,
 * because every observation then sits in the flat tail of the link, carries no
 * curvature, and leaves the Laplace posterior of the threshold where it was. */
#ifndef YAEP__EAVC_GUARD
#define YAEP__EAVC_GUARD 1
#endif

static double yaep__log_prior(const yaep_gp* g, const yaep__param* p, int np,
                               const double* th, double* grad) {
    const yaep_priors* pr = &g->priors;
    double lp = 0.0;
    if (g->desc.no_hyper_prior) return 0.0;
    for (int i = 0; i < np; i++) {
        double sd, center, v;
        switch (p[i].kind) {
            case YAEP__P_LS: case YAEP__P_LSB: case YAEP__P_LSG:
                sd = pr->lengthscale.sd;
                center = log(pr->lengthscale.center *
                             yaep__ls_range(&g->desc, p[i].dim));
                break;
            case YAEP__P_OS:
                sd = pr->outputscale.sd;
                center = log(pr->outputscale.center);
                break;
            case YAEP__P_OSB:
                sd = pr->outputscale_b.sd;
                center = log(pr->outputscale_b.center);
                break;
            case YAEP__P_OSG:
                sd = pr->outputscale_g.sd;
                center = log(pr->outputscale_g.center);
                break;
            case YAEP__P_MEANG:
                sd = pr->mean_g.sd;
                center = log(1.0 / (pr->mean_g.center * yaep__ispan(&g->desc)));
                break;
            case YAEP__P_MEAN:
                /* The GP model's mean on a discrete likelihood: where the
                 * field goes far from every trial. Unconstrained, a run whose
                 * responses are mostly one way sends it to its bound, and the
                 * whole unsampled box with it. */
                if (yaep__is_ps(g) || g->desc.lik == YAEP_LIK_GAUSSIAN) continue;
                sd = pr->mean.sd;
                center = pr->mean.center;
                break;
            case YAEP__P_NOISE:
                sd = pr->noise_sd.sd;
                center = pr->noise_sd.center > 0.0 ? log(pr->noise_sd.center) : 0.0;
                break;
            default:
                continue;                /* none on the cutpoints */
        }
        if (!(sd > 0.0)) continue;       /* that prior is off */
        v = (yaep__clamp(th[i], p[i].lo, p[i].hi) - center) / sd;
        lp -= 0.5 * v * v;
        if (grad) grad[i] -= v / sd;
    }
    return lp;
}

/* The adjoint solve stops at this residual, relative to the right-hand side:
 * the gradient it feeds is checked to 1e-4 against differences, and the
 * conjugate-gradient error in it is of the order of this squared. */
#define YAEP__PCG_TOL 1e-10

static int yaep__fit_grad_fd(yaep_gp* g, const yaep__param* p, int np,
                              const double* th, double* val, double* grad);

/* --- the psychometric model's hyperparameter gradient ------------------- */

/* The Laplace objective is log q = Psi(z) - 1/2 log|B|, B = I + U' K U with U
 * the Gauss-Newton square root at the mode z. Its total derivative in a
 * hyperparameter is the explicit part at fixed z plus the implicit part through
 * the mode, and only log|B| contributes to the second, because Psi is
 * stationary there:
 *
 *   dF/dtheta = dF/dtheta|_z + gz' dz/dtheta,   gz = -1/2 d log|B| / dz,
 *   dz/dtheta = A^-1 dG/dtheta,  A = K^-1 + W_true,  G = grad log p - K^-1 (z - m)
 *
 * with W_true the TRUE negative Hessian of the log-likelihood in z, which the
 * mode is defined by, not the Gauss-Newton one the step uses. One adjoint
 * solve, lambda = A^-1 gz, serves every hyperparameter. A is symmetric, and
 * positive definite at a maximum of the log posterior, so the solve is
 * preconditioned conjugate gradients with the Gauss-Newton posterior
 * M = (K^-1 + W_gn)^-1 = K - K U B^-1 U' K as the preconditioner. Everything it
 * needs is already factored: M r is two kernel products, two triangular solves
 * and two more kernel products; A z = M^-1 z + E z = r + E z for z = M r, with
 * E = W_true - W_gn block-diagonal 2 x 2; and K^-1 z = r - U B^-1 U' K r falls
 * out of the same arithmetic, which is what lets the kernel terms, which need
 * K^-1 lambda, avoid K^-1. The solve is O(N^2) an iteration where a dense 2N
 * factorization would be 5 N^3. Returns false if the iteration breaks down (A
 * not positive definite, so z is not a strict maximum and there is no gradient
 * to speak of), and the caller falls back to differences. */
static bool yaep__fit_grad_ps(yaep_gp* g, const yaep__param* p, int np,
                               const double* th, double* grad) {
    yaep__scr s;
    int n = g->N, ld = g->N_max, nd = g->desc.n_dims;
    double *am, *ag, *hg, *um, *ug, *w;
    double *bb, *E1, *E2, *E3, *sp, *cm, *cg, *gzm, *gzg;
    double *xm, *xg, *nm, *ng, *rm, *rg, *zm, *zg, *pm, *pg, *qm, *qg;
    double *km, *kg, *kim, *kig, *kzm, *kzg, *tt, *um2, *ug2;
    double mug = g->hyper.mean_g, rz, rr0, gs[YAEP__NTHETA] = { 0 };
    bool ord = g->desc.lik == YAEP_LIK_ORDINAL;
    yaep__scr_of(g, &s);
    am = yaep__psv(&s, ld, YAEP__PS_AM); ag = yaep__psv(&s, ld, YAEP__PS_AG);
    hg = yaep__psv(&s, ld, YAEP__PS_HG);
    um = yaep__psv(&s, ld, YAEP__PS_UM); ug = yaep__psv(&s, ld, YAEP__PS_UG);
    w = s.blk;
#define YAEP__R(k) (w + (size_t)(k) * ld)
    bb = YAEP__R(0);  E1 = YAEP__R(1);  E2 = YAEP__R(2);  E3 = YAEP__R(3);
    sp = YAEP__R(4);  cm = YAEP__R(5);  cg = YAEP__R(6);
    gzm = YAEP__R(7); gzg = YAEP__R(8);
    xm = YAEP__R(9);  xg = YAEP__R(10); nm = YAEP__R(11); ng = YAEP__R(12);
    rm = YAEP__R(13); rg = YAEP__R(14); zm = YAEP__R(15); zg = YAEP__R(16);
    pm = YAEP__R(17); pg = YAEP__R(18); qm = YAEP__R(19); qg = YAEP__R(20);
    km = YAEP__R(21); kg = YAEP__R(22); kim = YAEP__R(23); kig = YAEP__R(24);
    kzm = YAEP__R(25); kzg = YAEP__R(26); tt = YAEP__R(27);
    um2 = YAEP__R(28); ug2 = YAEP__R(29);
#undef YAEP__R

    /* B^-1, dense: the trace terms and the log-determinant's z-gradient read
     * every element. */
    for (int i = 0; i < n; i++)
        yaep__copyr(s.Rm + (size_t)i * ld, g->L + (size_t)i * ld, i + 1);
    yaep__tri_inv(s.Rm, n, ld, s.t1);
    yaep__tri_sqr(s.Rm, n, ld, s.t1);

    /* Per trial: the slope, the part of the true Hessian the Gauss-Newton W
     * left out, and ds/df with s = sqrt(w), which is how U moves with z. A site
     * the step clamped (w forced to 0 or capped) has a U that does not move. */
    for (int i = 0; i < n; i++) {
        double lp, l1, l2, l3, b, fv = g->f[i], wc = g->W[i], wt, dw, sv;
        yaep__ll(g, fv, g->y[i], &lp, &l1, &l2, &l3);
        b = exp(yaep__clamp(mug + hg[i], -50.0, 50.0));
        wt = -l2;
        dw = wt - wc;
        bb[i] = b;
        E1[i] = dw * b * b;
        E2[i] = -dw * b * fv + l1 * b;
        E3[i] = dw * fv * fv - l1 * fv;
        sv = sqrt(wc);
        sp[i] = (wc > 0.0 && wc == wt) ? -l3 / (2.0 * sv) : 0.0;
    }
    /* cm_i = (K_m diag(um) B^-1)_ii and cg_i likewise: d log|B| =
     * 2 sum_i (cm_i dum_i + cg_i dug_i). */
    for (int i = 0; i < n; i++) {
        const yaep_real* kr = g->Kmat + (size_t)i * ld;
        const yaep_real* gr = g->Kg + (size_t)i * ld;
        double a1 = 0.0, a2 = 0.0;
        for (int j = 0; j < n; j++) {
            double bij = s.Rm[(size_t)j * ld + i];
            a1 += kr[j] * um[j] * bij;
            a2 += gr[j] * ug[j] * bij;
        }
        cm[i] = a1;
        cg[i] = a2;
    }
    for (int i = 0; i < n; i++) {
        double b = bb[i], fv = g->f[i], sv = sqrt(g->W[i]), q = sp[i] * fv + sv;
        double dum_m = sp[i] * b * b, dum_g = -b * q;
        double dug_m = -b * q, dug_g = fv * q;
        gzm[i] = -(cm[i] * dum_m + cg[i] * dug_m);
        gzg[i] = -(cm[i] * dum_g + cg[i] * dug_g);
    }

    /* PCG on A lambda = gz, tracking K^-1 of every iterate. */
    for (int i = 0; i < n; i++) {
        xm[i] = xg[i] = nm[i] = ng[i] = 0.0;
        rm[i] = gzm[i]; rg[i] = gzg[i];
    }
#define YAEP__APPLY_M() do {                                              \
        yaep__gemv(g->Kmat, n, ld, rm, km);                               \
        yaep__gemv(g->Kg, n, ld, rg, kg);                                 \
        for (int i = 0; i < n; i++) tt[i] = um[i] * km[i] + ug[i] * kg[i]; \
        yaep__tri_fwd(g->L, n, ld, tt);                                   \
        yaep__tri_bwd(g->L, n, ld, tt);                                   \
        for (int i = 0; i < n; i++) { um2[i] = um[i] * tt[i]; ug2[i] = ug[i] * tt[i]; } \
        yaep__gemv(g->Kmat, n, ld, um2, zm);                              \
        yaep__gemv(g->Kg, n, ld, ug2, zg);                                \
        for (int i = 0; i < n; i++) {                                      \
            zm[i] = km[i] - zm[i]; zg[i] = kg[i] - zg[i];                  \
            kzm[i] = rm[i] - um2[i]; kzg[i] = rg[i] - ug2[i];              \
        }                                                                  \
    } while (0)
    YAEP__APPLY_M();
    rz = yaep__dot(rm, zm, n) + yaep__dot(rg, zg, n);
    rr0 = yaep__dot(rm, rm, n) + yaep__dot(rg, rg, n);
    for (int i = 0; i < n; i++) {
        pm[i] = zm[i]; pg[i] = zg[i];
        kim[i] = kzm[i]; kig[i] = kzg[i];
        qm[i] = rm[i] + E1[i] * zm[i] + E2[i] * zg[i];
        qg[i] = rg[i] + E2[i] * zm[i] + E3[i] * zg[i];
    }
    if (rr0 > 0.0) {
        int it;
        for (it = 0; it < 4 * n + 20; it++) {
            double pq = yaep__dot(pm, qm, n) + yaep__dot(pg, qg, n), al, rr, rzn, be;
            if (!(pq > 0.0)) return false;
            al = rz / pq;
            for (int i = 0; i < n; i++) {
                xm[i] += al * pm[i]; xg[i] += al * pg[i];
                nm[i] += al * kim[i]; ng[i] += al * kig[i];
                rm[i] -= al * qm[i]; rg[i] -= al * qg[i];
            }
            rr = yaep__dot(rm, rm, n) + yaep__dot(rg, rg, n);
            if (rr <= YAEP__PCG_TOL * YAEP__PCG_TOL * rr0) break;
            YAEP__APPLY_M();
            rzn = yaep__dot(rm, zm, n) + yaep__dot(rg, zg, n);
            be = rzn / rz;
            rz = rzn;
            for (int i = 0; i < n; i++) {
                pm[i] = zm[i] + be * pm[i];
                pg[i] = zg[i] + be * pg[i];
                kim[i] = kzm[i] + be * kim[i];
                kig[i] = kzg[i] + be * kig[i];
                qm[i] = rm[i] + E1[i] * zm[i] + E2[i] * zg[i] + be * qm[i];
                qg[i] = rg[i] + E2[i] * zm[i] + E3[i] * zg[i] + be * qg[i];
            }
        }
        if (it >= 4 * n + 20) return false;
    }
#undef YAEP__APPLY_M

    /* Kernel terms, one visit per pair and block: 1/2 a' dK a - 1/2
     * tr(B^-1 U' dK U) + nu' dK a with nu = K^-1 lambda. */
    for (int k = 0; k < np; k++) gs[k] = 0.0;
    for (int blk = 0; blk < 2; blk++) {
        const double* a = blk ? ag : am;
        const double* u = blk ? ug : um;
        const double* nu = blk ? ng : nm;
        const double* ls = blk ? g->hyper.lengthscale_g : g->hyper.lengthscale;
        int kls = blk ? YAEP__P_LSG : YAEP__P_LS;
        int kos = blk ? YAEP__P_OSG : YAEP__P_OS;
        for (int i = 0; i < n; i++) {
            const double* xi = g->X + (size_t)i * nd;
            for (int j = 0; j <= i; j++) {
                const double* xj = g->X + (size_t)j * nd;
                double k0 = yaep__kctx(g, blk, xi, xj);
                double c = (i == j ? 1.0 : 2.0) *
                           (0.5 * a[i] * a[j] - 0.5 * s.Rm[(size_t)i * ld + j] * u[i] * u[j])
                           + nu[i] * a[j] + (i == j ? 0.0 : nu[j] * a[i]);
                for (int q = 0; q < np; q++) {
                    if (p[q].kind == kos) gs[q] += c * k0;
                    else if (p[q].kind == kls) {
                        int dm = p[q].dim;
                        double dd;
                        if (g->mixed) {
                            yaep__dq(&g->desc, dm, xi[dm], xj[dm], ls[dm], &dd);
                            gs[q] += c * k0 * dd;
                            continue;
                        }
                        dd = (xi[dm] - xj[dm]) / ls[dm];
                        gs[q] += c * k0 * dd * dd;
                    }
                }
            }
        }
    }
    for (int q = 0; q < np; q++) {
        if (p[q].kind == YAEP__P_MEAN)
            for (int i = 0; i < n; i++) gs[q] += am[i] + nm[i];
        else if (p[q].kind == YAEP__P_MEANG)
            for (int i = 0; i < n; i++) gs[q] += ag[i] + ng[i];
    }
    /* Cutpoints: the likelihood's own derivative, U's through w, and the
     * implicit term through the gradient of dlog p/dc in z. The fitted
     * coordinate is the log gap below, as in the GP model. */
    if (ord) {
        double gc[YAEP_MAX_OUTCOMES] = { 0 };
        int nout = g->desc.n_outcomes;
        for (int m = 1; m < nout - 1; m++) {
            double ex = 0.0;
            for (int i = 0; i < n; i++) {
                double e0, e1, e2, fv = g->f[i], b = bb[i], wc = g->W[i], dsdc = 0.0;
                double lp, l1, l2, l3;
                yaep__ll_cut(g, fv, g->y[i], m, &e0, &e1, &e2);
                yaep__ll(g, fv, g->y[i], &lp, &l1, &l2, &l3);
                if (wc > 0.0 && wc == -l2) dsdc = -e2 / (2.0 * sqrt(wc));
                ex += e0 - (cm[i] * (-b * dsdc) + cg[i] * (fv * dsdc));
                ex += xm[i] * (-b * e1) + xg[i] * (fv * e1);
            }
            gc[m] = ex;
        }
        for (int q = 0; q < np; q++) {
            double gap, sum = 0.0;
            if (p[q].kind != YAEP__P_CUT) continue;
            gap = g->hyper.cutpoint[p[q].dim] - g->hyper.cutpoint[p[q].dim - 1];
            for (int m = p[q].dim; m < nout - 1; m++) sum += gc[m];
            gs[q] = gap * sum;
        }
    }
    /* The log parameters' chain rule: a lengthscale or output scale is fitted
     * as its logarithm, and dK/dlog(theta) is what the loop above used. */
    for (int q = 0; q < np; q++) grad[q] = gs[q];
    yaep__log_prior(g, p, np, th, grad);
    return true;
}

static int yaep__fit_eval(yaep_gp* g, const yaep__param* p, int np,
                           const double* th, double* val, double* grad) {
    yaep__scr s;
    int n, ld = g->N_max, rc;
    bool gauss = (g->desc.lik == YAEP_LIK_GAUSSIAN);
    bool clamped = false;
    double *q, *s2, *bv, *s3, *tv, *tmp, *piv;
    double aa[YAEP__NTHETA] = { 0 }, tr[YAEP__NTHETA] = { 0 };
    yaep__scr_of(g, &s);
    if (yaep__fpcg_desc(&g->desc) && g->fit_th_n == np && g->fit_valid && g->N_fit == g->N &&
        memcmp(s.stv + (size_t)YAEP__STV * ld, th, (size_t)np * sizeof(double)) == 0) {
        /* desc.fit_pcg: the model already stands at these hyperparameters
         * (the gradient step after an accepted line-search point), and a
         * refit from its own mode would only repeat it. */
    } else {
        yaep__theta_set(g, p, np, th);
        yaep__kmat_build(g);
        rc = yaep__infer(g, g->fit_valid && g->N_fit == g->N ? YAEP__START_KEEP
                                                            : YAEP__START_COLD, 0);
        if (rc != YAEP_OK) { g->fit_valid = false; g->fit_th_n = 0; return rc; }
        if (yaep__fpcg_desc(&g->desc)) {
            yaep__copy(s.stv + (size_t)YAEP__STV * ld, th, np);
            g->fit_th_n = np;
        }
    }
    *val = g->log_marginal + yaep__log_prior(g, p, np, th, NULL);
    if (!grad) return YAEP_OK;
    if (yaep__is_ps(g)) {
        if (yaep__fit_grad_ps(g, p, np, th, grad)) return YAEP_OK;
        return yaep__fit_grad_fd(g, p, np, th, val, grad);
    }
    if (g->K > 1) return YAEP_ERR_ARG;   /* softmax: yaep__fit_grad_fd */
    n = g->N;
    q = s.t1; s2 = s.t2; bv = s.t3; s3 = s.t4; tv = s.t5; tmp = s.t6; piv = s.t7;

    /* R = (K + W^-1)^-1 = W^1/2 B^-1 W^1/2, dense because the trace term needs
     * every element of it. Under GAUSSIAN sqrt(W) is 1 and the factor already
     * holds K + noise^2 I, so the same lines give (K + noise^2 I)^-1. */
    for (int i = 0; i < n; i++)
        yaep__copyr(s.Rm + (size_t)i * ld, g->L + (size_t)i * ld, i + 1);
    yaep__tri_inv(s.Rm, n, ld, tmp);
    yaep__tri_sqr(s.Rm, n, ld, tmp);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            s.Rm[(size_t)i * ld + j] = (yaep_real)(s.Rm[(size_t)i * ld + j] *
                                                   s.sW[i] * s.sW[j]);

    if (!gauss) {
        /* diag((K^-1 + W)^-1) = diag(K) - diag(C'C), C = L^-1 W^1/2 K, one
         * block of columns at a time so the factor is read M / BLK times. */
        for (int b0 = 0; b0 < n; b0 += YAEP__BLK) {
            int b1 = b0 + YAEP__BLK < n ? b0 + YAEP__BLK : n;
            for (int j = b0; j < b1; j++) {
                double* wv = s.blk + (size_t)(j - b0) * ld;
                double v = g->Kmat[(size_t)j * ld + j];
                for (int i = 0; i < n; i++)
                    wv[i] = s.sW[i] * g->Kmat[(size_t)j * ld + i];
                yaep__tri_fwd(g->L, n, ld, wv);
                q[j] = 0.5 * (v - yaep__dot(wv, wv, n));
            }
        }
        for (int i = 0; i < n; i++) {
            double lp, d1, d2, d3;
            yaep__ll(g, g->f[i], g->y[i], &lp, &d1, &d2, &d3);
            /* A site whose W the Newton step clamped to zero (which only
             * happens when desc.guess or desc.lapse has taken the log
             * likelihood out of log-concavity) contributes no dW/dtheta, so it
             * contributes nothing here either. Without this the gradient would
             * be the derivative of an objective the code does not evaluate. */
            s2[i] = g->W[i] > 0.0 ? q[i] * d3 : 0.0;
            if (!(-d2 > 0.0)) clamped = true;
        }
        if (clamped) {
            /* The mode is defined by the TRUE gradient, so the implicit term
             * goes through the true Hessian even where the Newton step used a
             * clamped one. Build I + K W_true and factor it with an LU, in the
             * Cholesky factor's buffer: nothing below this point reads the
             * factor, and the lines at the end put it back. */
            for (int i = 0; i < n; i++) {
                double lp, d1, d2, d3;
                yaep__ll(g, g->f[i], g->y[i], &lp, &d1, &d2, &d3);
                tv[i] = -d2;
            }
            for (int i = 0; i < n; i++) {
                yaep_real* ar = g->L + (size_t)i * ld;
                const yaep_real* kr = g->Kmat + (size_t)i * ld;
                for (int j = 0; j < n; j++) ar[j] = (yaep_real)(kr[j] * tv[j] + (i == j ? 1.0 : 0.0));
            }
            if (!yaep__lu(g->L, n, ld, piv)) clamped = false;
        }
    }

    /* One visit per matrix element for the explicit terms and for
     * (dK/dtheta) grad, which the implicit term needs. */
    for (int i = 0; i < np; i++) {
        aa[i] = 0.0;
        tr[i] = 0.0;
        for (int j = 0; j < n; j++) s.kg[(size_t)i * ld + j] = 0.0;
    }
    for (int i = 0; i < n; i++) {
        const double* xi = g->X + (size_t)i * g->desc.n_dims;
        for (int j = 0; j <= i; j++) {
            double dk[YAEP__NTHETA];
            double wgt = (i == j) ? 1.0 : 2.0;
            yaep__kernel_grad(g, p, np, xi, g->X + (size_t)j * g->desc.n_dims, dk);
            if (yaep__is_pair(g)) {
                /* d(k(x1i,x1j) + k(x2i,x2j) - k(x1i,x2j) - k(x2i,x1j)) */
                const double* yi = g->X2 + (size_t)i * g->desc.n_dims;
                const double* xj = g->X + (size_t)j * g->desc.n_dims;
                const double* yj = g->X2 + (size_t)j * g->desc.n_dims;
                double d2[YAEP__NTHETA];
                yaep__kernel_grad(g, p, np, yi, yj, d2);
                for (int u = 0; u < np; u++) dk[u] += d2[u];
                yaep__kernel_grad(g, p, np, xi, yj, d2);
                for (int u = 0; u < np; u++) dk[u] -= d2[u];
                yaep__kernel_grad(g, p, np, yi, xj, d2);
                for (int u = 0; u < np; u++) dk[u] -= d2[u];
            }
            for (int t = 0; t < np; t++) {
                if (dk[t] == 0.0) continue;
                aa[t] += wgt * s.alpha[i] * dk[t] * s.alpha[j];
                tr[t] += wgt * s.Rm[(size_t)i * ld + j] * dk[t];
                s.kg[(size_t)t * ld + i] += dk[t] * g->grad[j];
                if (i != j) s.kg[(size_t)t * ld + j] += dk[t] * g->grad[i];
            }
        }
    }

    for (int t = 0; t < np; t++) {
        double gv = 0.0;
        bool implicit = !gauss;
        switch (p[t].kind) {
            case YAEP__P_LS: case YAEP__P_OS:
            case YAEP__P_LSB: case YAEP__P_OSB:
                gv = 0.5 * aa[t] - 0.5 * tr[t];
                yaep__copy(bv, s.kg + (size_t)t * ld, n);
                break;
            case YAEP__P_MEAN:
                for (int i = 0; i < n; i++) { gv += s.alpha[i]; bv[i] = 1.0; }
                break;
            case YAEP__P_NOISE: {
                double nv = g->hyper.noise_sd * g->hyper.noise_sd, trc = 0.0, aa2 = 0.0;
                for (int i = 0; i < n; i++) {
                    trc += s.Rm[(size_t)i * ld + i];
                    aa2 += s.alpha[i] * s.alpha[i];
                }
                gv = nv * (aa2 - trc);
                implicit = false;
                break;
            }
            default: {   /* YAEP__P_CUT: assembled below */
                gv = 0.0;
                implicit = false;
                break;
            }
        }
        if (implicit) {
            /* s3 = (I + K W)^-1 b, which is b - K R b while W is the Laplace
             * Hessian, and an LU solve when a clamped entry means R was built
             * from something else. The implicit term is then s2' s3 (eq. 5.24). */
            if (clamped) {
                yaep__copy(s3, bv, n);
                yaep__lu_solve(g->L, n, ld, piv, s3);
                for (int i = 0; i < n; i++) gv += s2[i] * s3[i];
            } else {
                yaep__gemv(s.Rm, n, ld, bv, tv);
                yaep__gemv(g->Kmat, n, ld, tv, s3);
                for (int i = 0; i < n; i++) gv += s2[i] * (bv[i] - s3[i]);
            }
        }
        grad[t] = gv;
    }

    /* Cutpoints: the same explicit-plus-implicit split, but the likelihood
     * depends on them directly, so the log-determinant term comes in through
     * dW/dc and the implicit term through K de1/dc. The fitted coordinate is
     * the log gap to the cutpoint below, which is why one gap moves every
     * cutpoint above it. */
    if (g->desc.lik == YAEP_LIK_ORDINAL) {
        double gc[YAEP_MAX_OUTCOMES] = { 0 };
        int nout = g->desc.n_outcomes;
        bool any = false;
        for (int t = 0; t < np; t++) if (p[t].kind == YAEP__P_CUT) any = true;
        if (any) {
            for (int m = 1; m < nout - 1; m++) {
                double ex = 0.0;
                for (int i = 0; i < n; i++) {
                    double e0, e1, e2;
                    yaep__ll_cut(g, g->f[i], g->y[i], m, &e0, &e1, &e2);
                    ex += e0 + (g->W[i] > 0.0 ? q[i] * e2 : 0.0);
                    tv[i] = e1;
                }
                yaep__gemv(g->Kmat, n, ld, tv, bv);
                if (clamped) {
                    yaep__copy(s3, bv, n);
                    yaep__lu_solve(g->L, n, ld, piv, s3);
                    for (int i = 0; i < n; i++) ex += s2[i] * s3[i];
                } else {
                    yaep__gemv(s.Rm, n, ld, bv, tv);
                    yaep__gemv(g->Kmat, n, ld, tv, s3);
                    for (int i = 0; i < n; i++) ex += s2[i] * (bv[i] - s3[i]);
                }
                gc[m] = ex;
            }
            for (int t = 0; t < np; t++) {
                if (p[t].kind != YAEP__P_CUT) continue;   /* the gaps below */
                {
                    double gap = g->hyper.cutpoint[p[t].dim] -
                                 g->hyper.cutpoint[p[t].dim - 1];
                    double sum = 0.0;
                    for (int m = p[t].dim; m < nout - 1; m++) sum += gc[m];
                    grad[t] = gap * sum;
                }
            }
        }
    }
    yaep__log_prior(g, p, np, th, grad);
    if (clamped) {
        /* Put the Cholesky of B back: prediction reads it, and a caller may
         * read estimates between two steps of a fit. */
        for (int i = 0; i < n; i++) {
            const yaep_real* kr = g->Kmat + (size_t)i * ld;
            yaep_real* br = g->L + (size_t)i * ld;
            double si = s.sW[i];
            for (int j = 0; j <= i; j++) br[j] = (yaep_real)(si * kr[j] * s.sW[j]);
            br[i] += 1.0;
        }
        if (!yaep__chol(g->L, n, ld)) { g->fit_valid = false; return YAEP_ERR_NUMERIC; }
    }
    return YAEP_OK;
}

/* The softmax likelihood's gradient, by central differences on the objective.
 * The implicit term of eq. 5.23 needs third derivatives of a coupled block
 * Hessian; until that is written and checked, a numerical gradient of a
 * verified objective is the honest option. It costs 2 P + 1 Laplace refits. */
/* The difference step, relative: 1e-4 in double; in float the objective is
 * only good to about 1e-6 of itself, so a step that small differences noise,
 * and 3e-3 is what the float build's analytic-gradient check needed too. */
#define YAEP__FD_STEP (sizeof(yaep_real) == sizeof(double) ? 1e-4 : 3e-3)

static int yaep__fit_grad_fd(yaep_gp* g, const yaep__param* p, int np,
                              const double* th, double* val, double* grad) {
    yaep__scr s;
    double* tw;
    int rc;
    yaep__scr_of(g, &s);
    tw = s.th + 5 * YAEP__NTHETA;
    rc = yaep__fit_eval(g, p, np, th, val, NULL);
    if (rc != YAEP_OK) return rc;
    yaep__copy(tw, th, np);
    for (int i = 0; i < np; i++) {
        double step = YAEP__FD_STEP * (1.0 + fabs(th[i])), vp, vm;
        double hi = th[i] + step, lo = th[i] - step;
        if (hi > p[i].hi) hi = p[i].hi;
        if (lo < p[i].lo) lo = p[i].lo;
        if (!(hi > lo)) { grad[i] = 0.0; continue; }
        tw[i] = hi;
        if (yaep__fit_eval(g, p, np, tw, &vp, NULL) != YAEP_OK) { grad[i] = 0.0; tw[i] = th[i]; continue; }
        tw[i] = lo;
        if (yaep__fit_eval(g, p, np, tw, &vm, NULL) != YAEP_OK) { grad[i] = 0.0; tw[i] = th[i]; continue; }
        grad[i] = (vp - vm) / (hi - lo);
        tw[i] = th[i];
    }
    return yaep__fit_eval(g, p, np, th, val, NULL);
}

static int yaep__fit_value_grad(yaep_gp* g, const yaep__param* p, int np,
                                 const double* th, double* val, double* grad) {
    if (grad && g->K > 1) return yaep__fit_grad_fd(g, p, np, th, val, grad);
    return yaep__fit_eval(g, p, np, th, val, grad);
}

/* Gradient ascent with a backtracking line search, bounded by hyper_min and
 * hyper_max in the fitted coordinates. Bounded and monotone: a step is taken
 * only when it raises the marginal likelihood, so a fit never makes the model
 * worse than the values it started from.
 *
 * The ascent is a state machine rather than a loop, because a whole fit is
 * tens of refits and an experiment between two trials may only have room for
 * one. yaep_fit_step() advances it by one evaluation and yaep_fit() runs it
 * to the end. Every exit leaves the model at the best point found, so a caller
 * that stops stepping early still has a consistent posterior. */
#define YAEP__FIT_EVALS 40

static int yaep__fit_evals_max(const yaep_gp* g) {
    return g->desc.fit_max_evals > 0 ? g->desc.fit_max_evals : YAEP__FIT_EVALS;
}
#define YAEP__FIT_IDLE  0
#define YAEP__FIT_GRAD  1
#define YAEP__FIT_TRY   2

/* desc.fit_pcg: the state at the fit's current point, so a rejected
 * line-search point is undone by a copy instead of a refit from the rejected
 * point's mode. */
static void yaep__stash(yaep_gp* g, double val) {
    yaep__scr s;
    int n = g->N, ld = g->N_max;
    yaep__scr_of(g, &s);
    if (!yaep__fpcg_desc(&g->desc)) return;
    for (int i = 0; i < n; i++)
        yaep__copyr(s.stL + (size_t)i * ld, g->L + (size_t)i * ld, i + 1);
    yaep__copy(s.stv, g->f, n);
    yaep__copy(s.stv + (size_t)ld, g->W, n);
    yaep__copy(s.stv + (size_t)2 * ld, g->grad, n);
    yaep__copy(s.stv + (size_t)3 * ld, s.alpha, n);
    yaep__copy(s.stv + (size_t)4 * ld, s.sW, n);
    if (yaep__is_ps(g))
        for (int v = 0; v < YAEP__PS_VEC; v++)
            yaep__copy(s.stv + (size_t)(5 + v) * ld, s.ps + (size_t)v * ld, n);
    g->stash_val = val;
    g->stash_lm = g->log_marginal;
    g->stash_hyper = g->hyper;
    g->stash_ok = true;
}

static void yaep__unstash(yaep_gp* g, const double* th, int np) {
    yaep__scr s;
    int n = g->N, ld = g->N_max;
    yaep__scr_of(g, &s);
    g->hyper = g->stash_hyper;
    yaep__kmat_build(g);
    for (int i = 0; i < n; i++)
        yaep__copyr(g->L + (size_t)i * ld, s.stL + (size_t)i * ld, i + 1);
    yaep__copy(g->f, s.stv, n);
    yaep__copy(g->W, s.stv + (size_t)ld, n);
    yaep__copy(g->grad, s.stv + (size_t)2 * ld, n);
    yaep__copy(s.alpha, s.stv + (size_t)3 * ld, n);
    yaep__copy(s.sW, s.stv + (size_t)4 * ld, n);
    if (yaep__is_ps(g))
        for (int v = 0; v < YAEP__PS_VEC; v++)
            yaep__copy(s.ps + (size_t)v * ld, s.stv + (size_t)(5 + v) * ld, n);
    g->log_marginal = g->stash_lm;
    g->N_fit = n;
    g->fit_valid = true;
    yaep__copy(s.stv + (size_t)YAEP__STV * ld, th, np);
    g->fit_th_n = np;
}

static int yaep__fit_step(yaep_gp* g) {
    yaep__scr s;
    yaep__param p[YAEP__NTHETA];
    double *th, *cand, *gr;
    double val, gmax = 0.0;
    int np, rc;
    if (!g || !g->open) return YAEP_ERR_CLOSED;
    if (!g->fit_valid) return YAEP_ERR_NUMERIC;
    np = yaep__params(g, p);
    if (np == 0 || g->N < 2) {   /* nothing free, or nothing to fit it to */
        g->fit_state = YAEP__FIT_IDLE;
        g->fit_conv = 1;
        return 0;
    }
    yaep__scr_of(g, &s);
    th = s.th; cand = s.th + YAEP__NTHETA; gr = s.th + 2 * YAEP__NTHETA;
    if (g->fit_state == YAEP__FIT_IDLE) {
        yaep__theta_get(g, p, np, th);
        g->fit_evals = 0;
        g->fit_back = 0;
        /* A yaep_fit() that follows one the budget stopped keeps its step:
         * started over at 1/4 of the gradient, a small budget spends itself
         * rediscovering the step and never moves. */
        if (!g->fit_resume) g->fit_step = 0.0;
        g->fit_state = YAEP__FIT_GRAD;
    }
    if (g->fit_evals >= yaep__fit_evals_max(g)) {
        g->fit_state = YAEP__FIT_IDLE;
        g->fit_conv = 0;
        return 0;
    }
    if (g->fit_state == YAEP__FIT_GRAD) {
        rc = yaep__fit_value_grad(g, p, np, th, &val, gr);
        g->fit_evals++;
        g->fit_state = YAEP__FIT_IDLE;
        if (rc != YAEP_OK) {
            yaep__fit_eval(g, p, np, th, &val, NULL);   /* put the model back */
            return rc;
        }
        g->fit_best = val;
        yaep__stash(g, val);
        for (int i = 0; i < np; i++) if (fabs(gr[i]) > gmax) gmax = fabs(gr[i]);
        if (!(gmax > (g->desc.fit_tol > 0.0 ? g->desc.fit_tol : 1e-8))) {
            g->fit_conv = 1;                             /* converged */
            return 0;
        }
        if (!(g->fit_step > 0.0)) g->fit_step = 0.25 / gmax;
        g->fit_back = 0;
        g->fit_state = YAEP__FIT_TRY;
        return 1;
    }
    {   /* YAEP__FIT_TRY: one candidate along the gradient. */
        double move = 0.0;
        for (int i = 0; i < np; i++) {
            cand[i] = yaep__clamp(th[i] + g->fit_step * gr[i], p[i].lo, p[i].hi);
            move += fabs(cand[i] - th[i]);
        }
        if (move < 1e-10) {   /* pinned at the bounds: a constrained optimum */
            g->fit_state = YAEP__FIT_IDLE;
            g->fit_conv = 1;
            return 0;
        }
        rc = yaep__fit_eval(g, p, np, cand, &val, NULL);
        g->fit_evals++;
        if (rc == YAEP_OK &&
            val > g->fit_best + YAEP__FIT_TOL * (1.0 + fabs(g->fit_best))) {
            yaep__copy(th, cand, np);
            g->fit_best = val;
            g->fit_step *= 2.0;
            g->fit_state = YAEP__FIT_GRAD;
            return 1;
        }
        /* Rejected, so the model is sitting at a worse point: move it back
         * before returning, because the caller may read estimates between
         * steps. That is the second refit a step can cost. */
        g->fit_step *= 0.5;
        g->fit_back++;
        if (g->stash_ok) {
            yaep__unstash(g, th, np);
            if (g->fit_back >= 12) {
                g->fit_state = YAEP__FIT_IDLE;
                g->fit_conv = 1;
                return 0;
            }
            return 1;
        }
        rc = yaep__fit_eval(g, p, np, th, &val, NULL);
        if (rc != YAEP_OK ||
            !(val >= g->fit_best - YAEP__FIT_TOL * (1.0 + fabs(g->fit_best)))) {
            /* The restore warm-starts from the REJECTED point's mode, which
             * can be far from this one; if it did not come back to the value
             * this point was accepted at, refit it from the prior mean. */
            g->fit_valid = false;
            rc = yaep__fit_eval(g, p, np, th, &val, NULL);
        }
        if (rc != YAEP_OK) {
            g->fit_state = YAEP__FIT_IDLE;
            return YAEP_ERR_NUMERIC;
        }
        /* The restore is bookkeeping, not search, so it does not spend the
         * budget: what the budget counts is how much of the hyperparameter
         * space the fit is allowed to look at. */
        if (g->fit_back >= 12) {
            /* No uphill step at 1/4096 of the last one that worked: as
             * stationary as this ascent can tell. */
            g->fit_state = YAEP__FIT_IDLE;
            g->fit_conv = 1;
            return 0;
        }
        return 1;
    }
}

/* A whole fit, and the promise the manual makes about it checked rather than
 * assumed: the objective afterwards is compared with the one before, and if it
 * is worse or not finite the fit is undone and the model refitted at the
 * hyperparameters it started from. The ascent is monotone by construction;
 * this is the guard for the ways a Laplace refit inside it can still go
 * wrong, and it costs one objective evaluation when nothing did. */
/* The level-set guard's two thresholds, as fractions of the candidates: a fit
 * that reclassifies more than YAEP__FLIP_JUMP of them (above or below the
 * target) right after one that reclassified fewer than YAEP__FLIP_SETTLED is
 * undone. See HYPERPARAMETERS for how they were set. */
#define YAEP__FLIP_JUMP    0.05
#define YAEP__FLIP_SETTLED 0.02

static double yaep__q_smooth(const yaep_gp* g, const yaep__scr* s,
                              double mu, double sd, double other);

/* The candidates' classification against the target, into s->score as 0 or
 * 1, or the number that differ from what is there. The acquisition cache is
 * stale after a fit anyway, so its buffer is free. */
static int yaep__level_set(yaep_gp* g, bool compare) {
    yaep__scr s;
    int flips = 0;
    yaep__scr_of(g, &s);
    yaep__cand_fill(g, -1);
    for (int j = 0; j < g->M; j++) {
        double above = yaep__q_smooth(g, &s, g->cand_mu[j], s.cand_sd[j], 0.0) >
                       g->desc.target_p ? 1.0 : 0.0;
        if (compare) flips += above != s.score[j];
        else s.score[j] = above;
    }
    g->cand_valid = false;
    return flips;
}

static int yaep__fit_run(yaep_gp* g, bool resume) {
    yaep__param p[YAEP__NTHETA];
    double th0[YAEP__NTHETA] = { 0 }, th1[YAEP__NTHETA] = { 0 }, v0 = 0.0, v1;
    int rc, np = yaep__params(g, p);
    bool guard = g->fit_valid && np > 0 && g->N >= 2;
    /* The level-set guard is for the GP model on a one-latent discrete
     * likelihood, where the failure it stops was measured. */
    bool lguard = guard && !yaep__is_ps(g) && g->K == 1 &&
                  g->desc.lik != YAEP_LIK_GAUSSIAN && g->desc.target_p > 0.0;
    g->fit_delta = 0.0;
    if (guard) {
        yaep__theta_get(g, p, np, th0);
        v0 = g->log_marginal + yaep__log_prior(g, p, np, th0, NULL);
        if (!(v0 == v0)) guard = lguard = false;
    }
    if (lguard) yaep__level_set(g, false);
    g->fit_state = YAEP__FIT_IDLE;
    g->fit_resume = resume && g->fit_conv == 0 && g->fit_step > 0.0;
    g->fit_conv = 1;
    while ((rc = yaep__fit_step(g)) > 0) { }
    g->fit_state = YAEP__FIT_IDLE;
    g->fit_resume = false;
    if (lguard && rc == YAEP_OK && g->fit_valid) {
        /* A settled model whose fit suddenly redraws the level set: the
         * failure measured on the audiometric observer, where a data-favored
         * short-lengthscale mode interpolates the sampled columns and sends
         * every unsampled one to the mean. Undo it, once: a fit proposed right
         * after an undone one is accepted, so data that really do demand the
         * change get it one fit later. */
        double flip = (double)yaep__level_set(g, true) / (double)g->M;
        if (g->fit_flip >= 0.0 && g->fit_flip < YAEP__FLIP_SETTLED &&
            flip > YAEP__FLIP_JUMP && !g->fit_blocked) {
            double v;
            g->fit_blocked = true;
            rc = yaep__fit_eval(g, p, np, th0, &v, NULL);
            if (rc == YAEP_OK && !(v == v)) rc = YAEP_ERR_NUMERIC;
            return rc < 0 ? rc : 0;
        }
        g->fit_flip = flip;
        g->fit_blocked = false;
    }
    if (guard) {
        yaep__theta_get(g, p, np, th1);
        v1 = g->fit_valid ? g->log_marginal + yaep__log_prior(g, p, np, th1, NULL)
                          : (double)NAN;
        if (!(v1 == v1) || v1 < v0 - YAEP__FIT_TOL * (1.0 + fabs(v0))) {
            double v;
            g->fit_valid = false;              /* refit from the prior mean */
            rc = yaep__fit_eval(g, p, np, th0, &v, NULL);
            if (rc == YAEP_OK && !(v == v)) rc = YAEP_ERR_NUMERIC;
            return rc < 0 ? rc : 0;
        }
        g->fit_delta = v1 - v0;
    }
    return rc < 0 ? rc : g->fit_conv;
}

/* --- the target quantity under the posterior ---------------------------- */

#define YAEP__NSCEN (YAEP_MAX_OUTCOMES > YAEP_QUAD_N ? YAEP_MAX_OUTCOMES \
                                                        : YAEP_QUAD_N)

/* The latent level at which the target quantity is met exactly, which is the
 * level set the LSE straddle and LocalMI work on. */
static double yaep__f_star(const yaep_gp* g) {
    const yaep_desc* d = &g->desc;
    int link = (int)d->link;
    double p = d->target_p;
    if (d->lik == YAEP_LIK_GAUSSIAN) return d->target_value;
    if (!(p > 0.0 && p < 1.0)) return 0.0;   /* no level-set acquisition asked */
    if (d->lik == YAEP_LIK_CATEGORICAL) return log(p / (1.0 - p));
    /* Undo the floor and the ceiling before inverting the link: the level is
     * on the probability the observer produces, not on link(f). */
    p = (p - d->guess) / yaep__span(g);
    if (!(p > 0.0 && p < 1.0)) return 0.0;
    if (d->lik == YAEP_LIK_ORDINAL) {
        int k = d->target_outcome == 0 ? 1 : d->target_outcome;
        return g->hyper.cutpoint[k - 1] - yaep__link_inv(link, 1.0 - p);
    }
    return yaep__link_inv(link, p);
}

/* E[target quantity] at a latent mean and sd: closed form for the probit
 * Bernoulli case, Gauss-Hermite quadrature otherwise. */
static double yaep__q_smooth(const yaep_gp* g, const yaep__scr* s,
                              double mu, double sd, double other) {
    double acc = 0.0;
    if (g->desc.lik == YAEP_LIK_GAUSSIAN) return mu;
    if (yaep__bern(g) && g->desc.link == YAEP_LINK_PROBIT)
        return g->desc.guess +
               yaep__span(g) * yaep__Phi(mu / sqrt(1.0 + sd * sd));
    if (!(sd > 0.0)) return yaep__q_of_f(g, mu, other);
    for (int i = 0; i < YAEP_QUAD_N; i++)
        acc += s->gh_w[i] * yaep__q_of_f(g, mu + YAEP__SQRT2 * sd * s->gh_x[i],
                                         other);
    return acc / YAEP__SQRTPI;
}

/* Var[target quantity], the BALV score. */
static double yaep__q_var(const yaep_gp* g, const yaep__scr* s,
                           double mu, double sd, double other) {
    double m1 = 0.0, m2 = 0.0;
    if (g->desc.lik == YAEP_LIK_GAUSSIAN) return sd * sd;
    if (!(sd > 0.0)) return 0.0;
    for (int i = 0; i < YAEP_QUAD_N; i++) {
        double q = yaep__q_of_f(g, mu + YAEP__SQRT2 * sd * s->gh_x[i], other);
        m1 += s->gh_w[i] * q;
        m2 += s->gh_w[i] * q * q;
    }
    m1 /= YAEP__SQRTPI;
    m2 /= YAEP__SQRTPI;
    return m2 - m1 * m1 > 0.0 ? m2 - m1 * m1 : 0.0;
}

static double yaep__entropy(const double* p, int n) {
    double h = 0.0;
    for (int i = 0; i < n; i++) if (p[i] > 1e-300) h -= p[i] * log(p[i]);
    return h;
}

/* All outcome probabilities at an exact latent value, in the form the
 * acquisitions score: two entries under CATEGORICAL, where the class is scored
 * against the rest. */
static int yaep__scen_probs(const yaep_gp* g, double fv, double other, double* p) {
    if (g->desc.lik == YAEP_LIK_CATEGORICAL) {
        p[1] = yaep__sigmoid(fv - other);
        p[0] = 1.0 - p[1];
        return 2;
    }
    yaep__probs_of_f(g, fv, p);
    return yaep__bern(g) ? 2 : g->desc.n_outcomes;
}

/* BALD: the mutual information between the outcome and the latent (Houlsby et
 * al. 2011), H[E p(y|f)] - E H[p(y|f)], by quadrature. Closed form under
 * GAUSSIAN, where it is the information of one noisy reading. */
static double yaep__bald(const yaep_gp* g, const yaep__scr* s,
                          double mu, double sd, double other) {
    double pm[YAEP_MAX_OUTCOMES] = { 0 }, pf[YAEP_MAX_OUTCOMES] = { 0 };
    double eh = 0.0;
    int nout = 0;
    if (g->desc.lik == YAEP_LIK_GAUSSIAN) {
        double nv = g->hyper.noise_sd * g->hyper.noise_sd;
        return nv > 0.0 ? 0.5 * log1p(sd * sd / nv) : 0.0;
    }
    if (!(sd > 0.0)) return 0.0;
    for (int j = 0; j < YAEP_MAX_OUTCOMES; j++) pm[j] = 0.0;
    for (int i = 0; i < YAEP_QUAD_N; i++) {
        double w = s->gh_w[i] / YAEP__SQRTPI;
        nout = yaep__scen_probs(g, mu + YAEP__SQRT2 * sd * s->gh_x[i], other, pf);
        for (int j = 0; j < nout; j++) pm[j] += w * pf[j];
        eh += w * yaep__entropy(pf, nout);
    }
    return yaep__entropy(pm, nout) - eh;
}

/* The outcomes a look-ahead has to average over, with their predictive
 * probabilities: the K outcomes of a discrete likelihood, or the quadrature
 * nodes of the predictive of y under GAUSSIAN. */
static int yaep__scenarios(const yaep_gp* g, const yaep__scr* s,
                            double mu, double sd, double other,
                            double* oy, double* op) {
    if (g->desc.lik == YAEP_LIK_GAUSSIAN) {
        double nv = g->hyper.noise_sd * g->hyper.noise_sd;
        double t = sqrt(2.0 * (sd * sd + nv));
        for (int i = 0; i < YAEP_QUAD_N; i++) {
            oy[i] = mu + t * s->gh_x[i];
            op[i] = s->gh_w[i] / YAEP__SQRTPI;
        }
        return YAEP_QUAD_N;
    } else {
        double pm[YAEP_MAX_OUTCOMES] = { 0 }, pf[YAEP_MAX_OUTCOMES] = { 0 };
        int nout = yaep__scen_probs(g, mu, other, pf);
        for (int j = 0; j < nout; j++) pm[j] = 0.0;
        if (sd > 0.0) {
            for (int i = 0; i < YAEP_QUAD_N; i++) {
                double w = s->gh_w[i] / YAEP__SQRTPI;
                yaep__scen_probs(g, mu + YAEP__SQRT2 * sd * s->gh_x[i], other, pf);
                for (int j = 0; j < nout; j++) pm[j] += w * pf[j];
            }
        } else {
            for (int j = 0; j < nout; j++) pm[j] = pf[j];
        }
        for (int j = 0; j < nout; j++) { oy[j] = (double)j; op[j] = pm[j]; }
        return nout;
    }
}

/* The new site's log-likelihood gradient and Hessian at the predictive mean,
 * which is the rank-one term of the look-ahead: W frozen means W is read here
 * and not recomputed by a Newton step. */
static void yaep__site(const yaep_gp* g, double mu, double yv, double other,
                        double* d1, double* w) {
    if (g->desc.lik == YAEP_LIK_CATEGORICAL) {
        double pr = yaep__sigmoid(mu - other);
        *d1 = (yv > 0.5 ? 1.0 : 0.0) - pr;
        *w = pr * (1.0 - pr);
    } else if (g->desc.lik == YAEP_LIK_GAUSSIAN) {
        double nv = g->hyper.noise_sd * g->hyper.noise_sd;
        if (!(nv > 0.0)) nv = 1e-8;
        *d1 = (yv - mu) / nv;
        *w = 1.0 / nv;
    } else {
        double lp, dd1, dd2, dd3;
        yaep__ll(g, mu, yv, &lp, &dd1, &dd2, &dd3);
        *d1 = dd1;
        *w = -dd2 > 0.0 ? -dd2 : 0.0;
    }
}

static bool yaep__is_opt(yaep_acq acq) {
    return acq == YAEP_ACQ_UCB || acq == YAEP_ACQ_EI || acq == YAEP_ACQ_THOMPSON;
}

static double yaep__opt_sign(const yaep_gp* g) {
    return g->desc.minimize ? -1.0 : 1.0;
}

/* Expected improvement of the signed target quantity over g->opt_best, with f
 * Gaussian: closed form under GAUSSIAN, where q is f itself, and a quadrature
 * over f otherwise, since q is a monotone but nonlinear function of it. */
static double yaep__ei(const yaep_gp* g, const yaep__scr* s, double mu,
                        double sd, double other) {
    double sg = yaep__opt_sign(g), best = g->opt_best, acc = 0.0;
    if (g->desc.lik == YAEP_LIK_GAUSSIAN) {
        double m = sg * mu - best, z;
        if (!(sd > 0.0)) return m > 0.0 ? m : 0.0;
        z = m / sd;
        return m * yaep__Phi(z) + sd * exp(-0.5 * z * z) / YAEP__SQRT2PI;
    }
    if (!(sd > 0.0)) {
        double m = sg * yaep__q_of_f(g, mu, other) - best;
        return m > 0.0 ? m : 0.0;
    }
    for (int i = 0; i < YAEP_QUAD_N; i++) {
        double m = sg * yaep__q_of_f(g, mu + YAEP__SQRT2 * sd * s->gh_x[i], other) - best;
        if (m > 0.0) acc += s->gh_w[i] * m;
    }
    return acc / YAEP__SQRTPI;
}

/* --- the psychometric model's predictive ------------------------------- */

/* The bivariate Gaussian posterior of (m, g) at one context. */
typedef struct yaep__mg {
    double mm, mg, vm, vg, cmg;
} yaep__mg;

/* K** - K*' U B^-1 U' K*, where K* has the threshold GP's kernel column in the
 * m rows and the slope GP's in the g rows: two triangular solves per context,
 * and every stimulus at that context shares the answer. */
static void yaep__ps_post(const yaep_gp* g, const yaep__scr* s,
                           const double* x, yaep__mg* o) {
    int n = g->N_fit, ld = g->N_max, nd = g->desc.n_dims;
    const double *am = yaep__psv(s, ld, YAEP__PS_AM), *ag = yaep__psv(s, ld, YAEP__PS_AG);
    const double *um = yaep__psv(s, ld, YAEP__PS_UM), *ug = yaep__psv(s, ld, YAEP__PS_UG);
    double *km = yaep__psv(s, ld, YAEP__PS_KM), *kg = yaep__psv(s, ld, YAEP__PS_KG);
    double *v1 = yaep__psv(s, ld, YAEP__PS_V1), *v2 = yaep__psv(s, ld, YAEP__PS_V2);
    double lim;
    o->mm = g->hyper.mean;
    o->mg = g->hyper.mean_g;
    o->vm = yaep__kctx(g, 0, x, x);
    o->vg = yaep__kctx(g, 1, x, x);
    o->cmg = 0.0;
    if (n == 0) return;
    for (int i = 0; i < n; i++) {
        const double* xi = g->X + (size_t)i * nd;
        km[i] = yaep__kctx(g, 0, xi, x);
        kg[i] = yaep__kctx(g, 1, xi, x);
        v1[i] = um[i] * km[i];
        v2[i] = ug[i] * kg[i];
    }
    o->mm += yaep__dot(am, km, n);
    o->mg += yaep__dot(ag, kg, n);
    yaep__tri_fwd(g->L, n, ld, v1);
    yaep__tri_fwd(g->L, n, ld, v2);
    o->vm -= yaep__dot(v1, v1, n);
    o->vg -= yaep__dot(v2, v2, n);
    o->cmg = -yaep__dot(v1, v2, n);
    if (o->vm < 0.0) o->vm = 0.0;
    if (o->vg < 0.0) o->vg = 0.0;
    lim = sqrt(o->vm * o->vg);
    o->cmg = yaep__clamp(o->cmg, -lim, lim);
}

/* One node of the quadrature over g: the slope there and the Gaussian that f
 * is given that g, since f = b (x - m) is linear in m and m given g is
 * Gaussian. */
static void yaep__ps_node(const yaep__mg* o, double xint, double gk,
                           double* muf, double* sdf) {
    double b = exp(yaep__clamp(gk, -50.0, 50.0));
    double mc = o->mm, vc = o->vm;
    if (o->vg > 0.0) {
        mc += o->cmg / o->vg * (gk - o->mg);
        vc -= o->cmg * o->cmg / o->vg;
    }
    if (vc < 0.0) vc = 0.0;
    *muf = b * (xint - mc);
    *sdf = b * sqrt(vc);
}

/* The quadrature over g: YAEP_QUAD_N Gauss-Hermite nodes, or one when g is
 * known exactly. Fills the nodes and their weights, returns the count. */
static int yaep__ps_nodes(const yaep__scr* s, const yaep__mg* o, double* gk,
                           double* wk) {
    if (!(o->vg > 0.0)) { gk[0] = o->mg; wk[0] = 1.0; return 1; }
    for (int k = 0; k < YAEP_QUAD_N; k++) {
        gk[k] = o->mg + YAEP__SQRT2 * sqrt(o->vg) * s->gh_x[k];
        wk[k] = s->gh_w[k] / YAEP__SQRTPI;
    }
    return YAEP_QUAD_N;
}

/* E[q] and Var[q] of the target quantity at intensity xint. The m integral is
 * the one-latent machinery (closed form for probit Bernoulli, Gauss-Hermite
 * otherwise) and only g takes quadrature, so this is a 20-node rule where a
 * product rule would need 20 x 20. */
static void yaep__ps_moments(const yaep_gp* g, const yaep__scr* s, double xint,
                              const yaep__mg* o, double* eq, double* vq) {
    double gk[YAEP_QUAD_N] = { 0 }, wk[YAEP_QUAD_N] = { 0 }, e1 = 0.0, e2 = 0.0;
    int nk = yaep__ps_nodes(s, o, gk, wk);
    for (int k = 0; k < nk; k++) {
        double muf, sdf, q1;
        yaep__ps_node(o, xint, gk[k], &muf, &sdf);
        q1 = yaep__q_smooth(g, s, muf, sdf, 0.0);
        e1 += wk[k] * q1;
        if (vq) e2 += wk[k] * (yaep__q_var(g, s, muf, sdf, 0.0) + q1 * q1);
    }
    *eq = e1;
    if (vq) { double v = e2 - e1 * e1; *vq = v > 0.0 ? v : 0.0; }
}

/* The predictive outcome probabilities and the expected outcome entropy,
 * E[p(y | m, g)] and E[H(p(y | m, g))]: BALD is the entropy of the first
 * minus the second. Returns the outcome count. */
static int yaep__ps_mix(const yaep_gp* g, const yaep__scr* s, double xint,
                         const yaep__mg* o, double* pm, double* eh) {
    double gk[YAEP_QUAD_N] = { 0 }, wk[YAEP_QUAD_N] = { 0 }, pf[YAEP_MAX_OUTCOMES] = { 0 };
    int nout = yaep__bern(g) ? 2 : g->desc.n_outcomes;
    int nk = yaep__ps_nodes(s, o, gk, wk);
    for (int j = 0; j < nout; j++) pm[j] = 0.0;
    *eh = 0.0;
    for (int k = 0; k < nk; k++) {
        double muf, sdf;
        yaep__ps_node(o, xint, gk[k], &muf, &sdf);
        if (!(sdf > 0.0)) {
            yaep__probs_of_f(g, muf, pf);
            for (int j = 0; j < nout; j++) pm[j] += wk[k] * pf[j];
            *eh += wk[k] * yaep__entropy(pf, nout);
            continue;
        }
        for (int i = 0; i < YAEP_QUAD_N; i++) {
            double w = wk[k] * s->gh_w[i] / YAEP__SQRTPI;
            yaep__probs_of_f(g, muf + YAEP__SQRT2 * sdf * s->gh_x[i], pf);
            for (int j = 0; j < nout; j++) pm[j] += w * pf[j];
            *eh += w * yaep__entropy(pf, nout);
        }
    }
    return nout;
}

/* The latent level the target quantity is met at, for a caller's target. */
static double yaep__f_level(const yaep_gp* g, double target) {
    const yaep_desc* d = &g->desc;
    int link = (int)d->link;
    double pp = (target - d->guess) / yaep__span(g);
    if (d->lik == YAEP_LIK_ORDINAL) {
        int k = d->target_outcome == 0 ? 1 : d->target_outcome;
        return g->hyper.cutpoint[k - 1] - yaep__link_inv(link, 1.0 - pp);
    }
    return yaep__link_inv(link, pp);
}

/* P(level) at an intensity x: the probability that the target is met there,
 * P(m + f_t exp(-g) < x), one Gaussian CDF per node of the quadrature over g
 * because m given g is Gaussian. The look-ahead acquisitions' level-set
 * indicator. Split in two because everything but x depends on the context's
 * posterior alone: prepare once per context (the exponentials, the
 * conditional means, and only the nodes whose weight matters), then evaluate
 * per intensity, which on a grid is a whole column for one preparation. */
typedef struct yaep__lvl {
    int    nk;
    double w[YAEP_QUAD_N], a[YAEP_QUAD_N];
    double isc;        /* 1 / sd of m given g; 0 when m given g is exact */
} yaep__lvl;

static void yaep__lvl_prep(const yaep__scr* s, double ft, const yaep__mg* o,
                            yaep__lvl* L) {
    double gk[YAEP_QUAD_N] = { 0 }, wk[YAEP_QUAD_N] = { 0 }, sc2 = o->vm;
    int nk = yaep__ps_nodes(s, o, gk, wk);
    if (o->vg > 0.0) sc2 -= o->cmg * o->cmg / o->vg;
    L->isc = sc2 > 0.0 ? 1.0 / sqrt(sc2) : 0.0;
    L->nk = 0;
    for (int k = 0; k < nk; k++) {
        /* The outer Gauss-Hermite weights fall to 1e-22; below 1e-12 a node
         * cannot move a probability. */
        if (wk[k] < 1e-12) continue;
        L->w[L->nk] = wk[k];
        L->a[L->nk] = ft * exp(-yaep__clamp(gk[k], -50.0, 50.0)) +
                      o->mm + (o->vg > 0.0 ? o->cmg / o->vg * (gk[k] - o->mg) : 0.0);
        L->nk++;
    }
}

static double yaep__lvl_eval(const yaep__lvl* L, double x, bool fast) {
    double acc = 0.0;
    for (int k = 0; k < L->nk; k++) {
        double z = (x - L->a[k]) * L->isc, pk;
        if (L->isc == 0.0) pk = x - L->a[k] > 0.0 ? 1.0 : 0.0;
        else if (z > 8.5) pk = 1.0;            /* saturated: skip the exponential */
        else if (z < -8.5) pk = 0.0;
        else pk = fast ? yaep__Phi_fast(z) : yaep__Phi(z);
        acc += L->w[k] * pk;
    }
    return acc;
}

static double yaep__ps_level(const yaep__scr* s, double xint, double ft,
                              const yaep__mg* o, bool fast) {
    yaep__lvl L;
    yaep__lvl_prep(s, ft, o, &L);
    return yaep__lvl_eval(&L, xint, fast);
}

/* One more trial at intensity xint of a context whose posterior is o, with
 * outcome y: the Gauss-Newton site at the predictive mean, which is the
 * look-ahead's frozen W. J is df/d(m, g) there, c = J' Sigma J. */
static void yaep__ps_site(const yaep_gp* g, const yaep__mg* o, double xint,
                           double yv, double* J, double* d1, double* w, double* c) {
    double b = exp(yaep__clamp(o->mg, -50.0, 50.0)), fv = b * (xint - o->mm);
    double lp, l1, l2, l3;
    yaep__ll(g, fv, yv, &lp, &l1, &l2, &l3);
    J[0] = -b;
    J[1] = fv;
    *d1 = l1;
    *w = -l2 > 0.0 ? (-l2 < 1e8 ? -l2 : 1e8) : 0.0;
    *c = o->vm * J[0] * J[0] + 2.0 * o->cmg * J[0] * J[1] + o->vg * J[1] * J[1];
}

/* A context's (m, g) posterior after that trial, given v = Cov((m, g), f*) =
 * Sigma_ij J: the rank-one update the Gauss-Newton W makes it, one Newton step
 * from the predictive mean with W frozen, as in the GP model's look-ahead. */
static void yaep__ps_lookahead(const yaep__mg* oi, const double* v, double d1,
                                double w, double c, yaep__mg* out) {
    double den = 1.0 + w * c, lim;
    out->mm = oi->mm + v[0] * d1 / den;
    out->mg = oi->mg + v[1] * d1 / den;
    out->vm = oi->vm - v[0] * v[0] * w / den;
    out->vg = oi->vg - v[1] * v[1] * w / den;
    out->cmg = oi->cmg - v[0] * v[1] * w / den;
    if (out->vm < 0.0) out->vm = 0.0;
    if (out->vg < 0.0) out->vg = 0.0;
    lim = sqrt(out->vm * out->vg);
    out->cmg = yaep__clamp(out->cmg, -lim, lim);
}

static double yaep__h2(double p) {
    double pp[2] = { 0 };
    pp[0] = 1.0 - p; pp[1] = p;
    return yaep__entropy(pp, 2);
}

/* LocalMI at x: the information one trial there carries about whether x
 * itself is above the level, H(pi) - E_y H(pi | y), with the self-update of
 * x's own context. Per point, so it is its own refinement objective. */
static double yaep__ps_localmi(const yaep_gp* g, const yaep__scr* s,
                                const double* x, const yaep__mg* o) {
    double xint = x[g->desc.intensity_dim], ft = yaep__f_level(g, g->desc.target_p);
    double pm[YAEP_MAX_OUTCOMES] = { 0 }, eh = 0.0, ehy = 0.0, pi0;
    int nout = yaep__ps_mix(g, s, xint, o, pm, &eh);
    pi0 = yaep__ps_level(s, xint, ft, o, false);
    for (int y = 0; y < nout; y++) {
        double J[2] = { 0 }, d1, w, c, v[2] = { 0 };
        yaep__mg o2;
        if (!(pm[y] > 0.0)) continue;
        yaep__ps_site(g, o, xint, (double)y, J, &d1, &w, &c);
        v[0] = o->vm * J[0] + o->cmg * J[1];
        v[1] = o->cmg * J[0] + o->vg * J[1];
        yaep__ps_lookahead(o, v, d1, w, c, &o2);
        ehy += pm[y] * yaep__h2(yaep__ps_level(s, xint, ft, &o2, false));
    }
    return yaep__h2(pi0) - ehy;
}

/* The acquisition score at x under the psychometric model. LSE is the straddle
 * on the probability scale, beta sd[q] - |E[q] - target_p|, because the latent
 * of this model is not Gaussian and has no single sd to straddle with. */
static double yaep__ps_score(const yaep_gp* g, const yaep__scr* s,
                              const double* x, const yaep__mg* o) {
    double xint = x[g->desc.intensity_dim], eq, vq;
    switch (g->desc.acq) {
        case YAEP_ACQ_LSE:
        case YAEP_ACQ_EAVC: {
            /* EAVC's own score is a sum over the other candidates (see
             * yaep__ps_eavc); at one point, for refinement, it is the
             * straddle, as under the GP model. */
            double beta = g->desc.acq_beta > 0.0 ? g->desc.acq_beta : 1.96;
            yaep__ps_moments(g, s, xint, o, &eq, &vq);
            return beta * sqrt(vq) - fabs(eq - g->desc.target_p);
        }
        case YAEP_ACQ_LOCALMI:
            return yaep__ps_localmi(g, s, x, o);
        case YAEP_ACQ_BALV:
            yaep__ps_moments(g, s, xint, o, &eq, &vq);
            return vq;
        case YAEP_ACQ_UCB: {
            double beta = g->desc.acq_beta > 0.0 ? g->desc.acq_beta : 1.96;
            yaep__ps_moments(g, s, xint, o, &eq, &vq);
            return yaep__opt_sign(g) * eq + beta * sqrt(vq);
        }
        case YAEP_ACQ_EI: {
            /* The one-latent expected improvement at each node of the g
             * quadrature, where f is Gaussian. */
            double gk[YAEP_QUAD_N] = { 0 }, wk[YAEP_QUAD_N] = { 0 }, acc = 0.0;
            int nk = yaep__ps_nodes(s, o, gk, wk);
            for (int k = 0; k < nk; k++) {
                double muf, sdf;
                yaep__ps_node(o, xint, gk[k], &muf, &sdf);
                acc += wk[k] * yaep__ei(g, s, muf, sdf, 0.0);
            }
            return acc;
        }
        case YAEP_ACQ_BALD: {
            double pm[YAEP_MAX_OUTCOMES] = { 0 }, eh;
            int nout = yaep__ps_mix(g, s, xint, o, pm, &eh);
            return yaep__entropy(pm, nout) - eh;
        }
        default:
            return 0.0;
    }
}

/* EAVC under the psychometric model: the expected absolute change in the
 * expected number of candidates above the level, sum_i P(level_i), after one
 * more trial at candidate j. With W frozen at j's predictive mean the trial
 * updates every context's (m, g) posterior by rank one, through
 * v_i = Sigma_ij J with Sigma_ij = K_ij - V_i' V_j, V the L^-1 U' k columns
 * the prediction already computed (kept in ps_v). A candidate i sharing a
 * context with the one before it shares v, so on a grid with the intensity
 * fastest the M x M pairs cost one context's worth of dot products per
 * column. What is left is M^2 x outcomes x YAEP_QUAD_N Gaussian CDFs. */
static void yaep__ps_eavc(yaep_gp* g, yaep__scr* s) {
    int M = g->M, n = g->N_fit, ld = g->N_max, id = g->desc.intensity_dim;
    int nout = yaep__bern(g) ? 2 : g->desc.n_outcomes;
    double ft = yaep__f_level(g, g->desc.target_p);
    double* wj = yaep__psv(s, ld, YAEP__PS_KM);
    yaep__lvl lv[YAEP_MAX_OUTCOMES];
    {
        const double* prev = NULL;
        for (int i = 0; i < M; i++) {
            const double* ci = yaep__cand(g, i);
            if (!prev || !yaep__same_ctx(g, prev, ci)) {
                yaep__mg oi;
                memcpy(&oi, s->ps_c + (size_t)5 * i, sizeof(oi));
                yaep__lvl_prep(s, ft, &oi, &lv[0]);
            }
            prev = ci;
            s->ps_p0[i] = yaep__lvl_eval(&lv[0], ci[id], true);
        }
    }
    for (int j = 0; j < M; j++) {
        yaep__mg oj;
        const double* cj = yaep__cand(g, j);
        const yaep_real* v1j = s->ps_v + (size_t)(2 * j) * ld;
        const yaep_real* v2j = s->ps_v + (size_t)(2 * j + 1) * ld;
        double pm[YAEP_MAX_OUTCOMES] = { 0 }, eh, J[2] = { 0 }, c = 0.0, d1[YAEP_MAX_OUTCOMES] = { 0 };
        double wy[YAEP_MAX_OUTCOMES] = { 0 }, dv[YAEP_MAX_OUTCOMES] = { 0 }, sc = 0.0;
        const double* prev = NULL;
        memcpy(&oj, s->ps_c + (size_t)5 * j, sizeof(oj));
        yaep__ps_mix(g, s, cj[id], &oj, pm, &eh);
        for (int y = 0; y < nout; y++) {
            yaep__ps_site(g, &oj, cj[id], (double)y, J, &d1[y], &wy[y], &c);
            dv[y] = 0.0;
        }
        for (int k = 0; k < n; k++) wj[k] = (double)v1j[k] * J[0] + (double)v2j[k] * J[1];
        for (int i = 0; i < M; i++) {
            const double* ci = yaep__cand(g, i);
            if (!prev || !yaep__same_ctx(g, prev, ci)) {
                /* A new context: its v = Sigma_ij J, and each outcome's updated
                 * posterior prepared for the whole column. */
                const yaep_real* v1i = s->ps_v + (size_t)(2 * i) * ld;
                const yaep_real* v2i = s->ps_v + (size_t)(2 * i + 1) * ld;
                double vi[2] = { 0 };
                yaep__mg oi;
                memcpy(&oi, s->ps_c + (size_t)5 * i, sizeof(oi));
                vi[0] = yaep__kctx(g, 0, ci, cj) * J[0] - yaep__dotr(v1i, wj, n);
                vi[1] = yaep__kctx(g, 1, ci, cj) * J[1] - yaep__dotr(v2i, wj, n);
                for (int y = 0; y < nout; y++) {
                    yaep__mg o2;
                    yaep__ps_lookahead(&oi, vi, d1[y], wy[y], c, &o2);
                    yaep__lvl_prep(s, ft, &o2, &lv[y]);
                }
            }
            prev = ci;
            for (int y = 0; y < nout; y++) {
                if (!(pm[y] > 0.0)) continue;
                dv[y] += yaep__lvl_eval(&lv[y], ci[id], true) - s->ps_p0[i];
            }
        }
        for (int y = 0; y < nout; y++) sc += pm[y] * fabs(dv[y]);
        s->score[j] = sc;
    }
}

/* Score every candidate, one posterior per context: a product grid with the
 * intensity fastest shares each context across a whole column. */
static void yaep__ps_fill(yaep_gp* g) {
    yaep__scr s;
    yaep__mg o;
    const double* prev = NULL;
    int M = g->M;
    yaep__scr_of(g, &s);
    memset(&o, 0, sizeof(o));
    for (int j = 0; j < M; j++) {
        const double* cj = yaep__cand(g, j);
        if (!prev || !yaep__same_ctx(g, prev, cj)) yaep__ps_post(g, &s, cj, &o);
        prev = cj;
        s.ps_c[(size_t)5 * j + 0] = o.mm;
        s.ps_c[(size_t)5 * j + 1] = o.mg;
        s.ps_c[(size_t)5 * j + 2] = o.vm;
        s.ps_c[(size_t)5 * j + 3] = o.vg;
        s.ps_c[(size_t)5 * j + 4] = o.cmg;
        if (g->desc.acq == YAEP_ACQ_EAVC) {
            /* ps_post leaves this context's L^-1 U' k columns in the V1 and V2
             * rows, and they stay there while the context repeats. */
            const double* v1 = yaep__psv(&s, g->N_max, YAEP__PS_V1);
            const double* v2 = yaep__psv(&s, g->N_max, YAEP__PS_V2);
            yaep_real* d1 = s.ps_v + (size_t)(2 * j) * g->N_max;
            yaep_real* d2 = s.ps_v + (size_t)(2 * j + 1) * g->N_max;
            for (int k = 0; k < g->N_fit; k++) { d1[k] = (yaep_real)v1[k]; d2[k] = (yaep_real)v2[k]; }
            continue;
        }
        s.score[j] = g->desc.acq == YAEP_ACQ_RANDOM ? 0.0
                                                     : yaep__ps_score(g, &s, cj, &o);
    }
    if (g->desc.acq == YAEP_ACQ_EAVC) yaep__ps_eavc(g, &s);
    g->cand_valid = true;
}


/* --- acquisition -------------------------------------------------------- */

/* The score of one latent at one point, for the acquisitions whose score needs
 * nothing but that point's predictive: the candidate sweep and the refinement
 * of desc.refine_steps both call it, so a refined point is judged by exactly
 * the arithmetic that picked the candidate it started from. */
static double yaep__score_one(const yaep_gp* g, const yaep__scr* s,
                               yaep_acq acq, double mu, double sd, double other,
                               double beta, double fstar) {
    switch (acq) {
        case YAEP_ACQ_UCB: {
            double v = yaep__q_var(g, s, mu, sd, other);
            return yaep__opt_sign(g) * yaep__q_smooth(g, s, mu, sd, other) +
                   beta * sqrt(v > 0.0 ? v : 0.0);
        }
        case YAEP_ACQ_EI:
            return yaep__ei(g, s, mu, sd, other);
        case YAEP_ACQ_LSE:
            return beta * sd - fabs(mu - other - fstar);
        case YAEP_ACQ_BALV:
            return yaep__q_var(g, s, mu, sd, other);
        case YAEP_ACQ_BALD:
            return yaep__bald(g, s, mu, sd, other);
        case YAEP_ACQ_LOCALMI: {
            double oy[YAEP__NSCEN] = { 0 }, op[YAEP__NSCEN] = { 0 };
            double cjj = sd * sd, pi0, eh = 0.0, pp[2] = { 0 };
            int nsc;
            if (!(cjj > 0.0)) return 0.0;
            pi0 = yaep__Phi_fast((mu - other - fstar) / sd);
            pp[0] = 1.0 - pi0; pp[1] = pi0;
            nsc = yaep__scenarios(g, s, mu, sd, other, oy, op);
            for (int t = 0; t < nsc; t++) {
                double d1, w, den, mup, sdp, piy;
                if (op[t] <= 0.0) continue;
                yaep__site(g, mu, oy[t], other, &d1, &w);
                den = 1.0 + w * cjj;
                mup = mu + cjj * d1 / den;
                sdp = sqrt(cjj / den);
                piy = yaep__Phi_fast((mup - other - fstar) / sdp);
                pp[0] = 1.0 - piy; pp[1] = piy;
                eh += op[t] * yaep__entropy(pp, 2);
            }
            pp[0] = 1.0 - pi0; pp[1] = pi0;
            return yaep__entropy(pp, 2) - eh;
        }
        default:
            return 0.0;
    }
}

/* The acquisition at an arbitrary point, the objective desc.refine_steps
 * climbs. EAVC is a sum over the OTHER candidates and has no value off the
 * candidate set, so it is refined on the LSE straddle, the per-point score
 * that aims at the same level set. */
static double yaep__point_score(const yaep_gp* g, const yaep__scr* s,
                                 const double* x) {
    double mus[YAEP_MAX_OUTCOMES] = { 0 }, sds[YAEP_MAX_OUTCOMES] = { 0 }, total = 0.0;
    double beta = g->desc.acq_beta > 0.0 ? g->desc.acq_beta : 1.96;
    double fstar = yaep__f_star(g);
    yaep_acq acq = g->desc.acq == YAEP_ACQ_EAVC ? YAEP_ACQ_LSE : g->desc.acq;
    int K = g->K;
    if (yaep__is_ps(g)) {
        yaep__mg o;
        yaep__ps_post(g, s, x, &o);
        return yaep__ps_score(g, s, x, &o);
    }
    for (int c = 0; c < K; c++) {
        double v;
        yaep__predict_k(g, s, x, c, &mus[c], &v, s->t1, s->t2);
        sds[c] = sqrt(v);
    }
    if (yaep__mono_acq(g) && K == 1) mus[0] = yaep__mono_mu(g, s, x, mus[0], s->t1);
    for (int c = 0; c < K; c++) {
        double other = 0.0;
        if (K > 1) {
            double fs[YAEP_MAX_OUTCOMES] = { 0.0 };
            int nr = 0;
            for (int cc = 0; cc < K; cc++) if (cc != c) fs[nr++] = mus[cc];
            other = yaep__logsumexp(fs, nr);
        }
        if (yaep__is_opt(acq) && K > 1 && c != g->desc.target_outcome) continue;
        total += yaep__score_one(g, s, acq, mus[c], sds[c], other, beta, fstar);
    }
    return total;
}

static void yaep__target_latent(const yaep_gp* g, const yaep__scr* s,
                                 const double* x, double* mu, double* sd,
                                 double* other, double* w1, double* w2);

/* The signed posterior mean of the target quantity at x: the objective of
 * yaep_argmax() and of its refinement. */
static double yaep__mean_score(const yaep_gp* g, const yaep__scr* s,
                                const double* x) {
    double v;
    if (yaep__is_ps(g)) {
        yaep__mg o;
        yaep__ps_post(g, s, x, &o);
        yaep__ps_moments(g, s, x[g->desc.intensity_dim], &o, &v, NULL);
    } else {
        double mu, sd, other;
        yaep__target_latent(g, s, x, &mu, &sd, &other, s->t1, s->t2);
        /* A comparison's quantity is the latent utility itself. */
        v = yaep__is_pair(g) ? mu : yaep__q_smooth(g, s, mu, sd, other);
    }
    return yaep__opt_sign(g) * v;
}

/* Golden-section iterations per dimension per round: the bracket of two
 * candidate spacings shrinks to 0.618^10 of itself, under 2% of one spacing,
 * which is finer than a display quantizes a stimulus. */
#define YAEP__GOLDEN_ITERS 10

/* Half-width of the refinement bracket along each dimension: one grid step, or
 * for a Halton or caller-owned set the spacing M points would have on a
 * regular grid over the box. 0 for a dimension the grid holds fixed. */
static void yaep__refine_step(const yaep_gp* g, double* h) {
    const yaep_desc* d = &g->desc;
    int nd = d->n_dims, grid = 0;
    for (int k = 0; k < nd; k++) if (d->grid[k] != 0) grid = 1;
    for (int k = 0; k < nd; k++) {
        double span = d->hi[k] - d->lo[k];
        if (!d->candidates && grid)
            h[k] = yaep__grid_n(d, k) > 1
                 ? span / (double)(yaep__grid_n(d, k) - 1) : 0.0;
        else
            h[k] = span * pow((double)g->M, -1.0 / (double)nd);
    }
}

/* desc.refine_steps rounds of coordinate-wise golden-section search on the
 * point score, starting from the grid winner in x. Every round searches the
 * same box, the winner's neighbors along each dimension, and not a box around
 * wherever the last round ended: a bracket that followed the point would let
 * EAVC's straddle objective walk a look-ahead winner several cells to the
 * straddle's own optimum, which is a different acquisition. A move is taken
 * only when it scores strictly higher, so the result is never worse than the
 * candidate by the objective it is judged on. Returns true when x moved. */
typedef double (*yaep__objective)(const yaep_gp* g, const yaep__scr* s,
                                   const double* x);

static bool yaep__refine(const yaep_gp* g, const yaep__scr* s, double* x,
                          yaep__objective obj) {
    const yaep_desc* d = &g->desc;
    const double r = 0.61803398874989484820;
    double h[YAEP_MAX_DIMS] = { 0 }, x0[YAEP_MAX_DIMS] = { 0 };
    double fx = obj(g, s, x);
    bool moved = false;
    yaep__refine_step(g, h);
    yaep__copy(x0, x, d->n_dims);
    for (int round = 0; round < d->refine_steps; round++) {
        bool moved_round = false;
        for (int k = 0; k < d->n_dims; k++) {
            double a, b, c1, c2, f1, f2, keep = x[k];
            if (d->dim_kind[k] != YAEP_DIM_CONTINUOUS) {
                /* No bracket to shrink: every level, or every integer within
                 * the bracket (at least the two neighbors), is scored. */
                double from, to, best = keep;
                if (d->dim_kind[k] == YAEP_DIM_CATEGORICAL) {
                    from = d->lo[k];
                    to = d->hi[k];
                } else {
                    double w = h[k] > 1.0 ? floor(h[k]) : 1.0;
                    if (w > 16.0) w = 16.0;
                    from = x0[k] - w > d->lo[k] ? x0[k] - w : d->lo[k];
                    to = x0[k] + w < d->hi[k] ? x0[k] + w : d->hi[k];
                }
                for (double v = from; v <= to; v += 1.0) {
                    if (v == keep) continue;
                    x[k] = v;
                    f1 = obj(g, s, x);
                    if (f1 > fx) { fx = f1; best = v; moved_round = true; }
                }
                x[k] = best;
                continue;
            }
            if (!(h[k] > 0.0)) continue;
            a = x0[k] - h[k] > d->lo[k] ? x0[k] - h[k] : d->lo[k];
            b = x0[k] + h[k] < d->hi[k] ? x0[k] + h[k] : d->hi[k];
            c1 = b - r * (b - a);
            c2 = a + r * (b - a);
            x[k] = c1; f1 = obj(g, s, x);
            x[k] = c2; f2 = obj(g, s, x);
            for (int it = 0; it < YAEP__GOLDEN_ITERS; it++) {
                if (f1 >= f2) {
                    b = c2; c2 = c1; f2 = f1;
                    c1 = b - r * (b - a);
                    x[k] = c1; f1 = obj(g, s, x);
                } else {
                    a = c1; c1 = c2; f1 = f2;
                    c2 = a + r * (b - a);
                    x[k] = c2; f2 = obj(g, s, x);
                }
            }
            if (f1 >= f2 && f1 > fx) { x[k] = c1; fx = f1; moved_round = true; }
            else if (f2 > f1 && f2 > fx) { x[k] = c2; fx = f2; moved_round = true; }
            else x[k] = keep;
        }
        if (!moved_round) break;   /* a round that moved nothing has converged */
        moved = true;
    }
    return moved;
}

/* One latent's contribution to the acquisition score of every candidate. With
 * one latent that is the whole score; under CATEGORICAL the caller sums the K
 * one-against-the-rest contributions, which is the approximation the manual
 * states. */
static void yaep__acq_class(yaep_gp* g, const yaep__scr* s, int c) {
    int M = g->M, K = g->K;
    const double* mus = g->cand_mu + (size_t)c * M;
    const double* sds = s->cand_sd + (size_t)c * M;
    double beta = g->desc.acq_beta > 0.0 ? g->desc.acq_beta : 1.96;
    double fstar = yaep__f_star(g);
    yaep_acq acq = g->desc.acq;
    double V0 = 0.0;
    if (acq == YAEP_ACQ_EAVC) {
        /* The level set's volume, and the per-candidate levels and variances
         * the look-ahead updates. The volume is the EXPECTED number of
         * candidates above the level, sum_i P(f_i > f*), not the number whose
         * posterior mean is above it: a count changes by whole candidates or
         * not at all, and once no candidate is near enough to flip, every
         * score is zero and the acquisition stops moving. */
        for (int j = 0; j < M; j++) {
            double other = 0.0;
            if (K > 1) {
                double fs[YAEP_MAX_OUTCOMES] = { 0.0 };
                int nr = 0;
                for (int cc = 0; cc < K; cc++)
                    if (cc != c) fs[nr++] = g->cand_mu[(size_t)cc * M + j];
                other = yaep__logsumexp(fs, nr);
            }
            s->cand_t[j] = fstar + other;
            s->cand_dv[j] = g->cand_cov[(size_t)j * M + j];
            s->cand_isd[j] = s->cand_dv[j] > 0.0 ? 1.0 / sqrt(s->cand_dv[j]) : 0.0;
            s->cand_p0[j] = s->cand_dv[j] > 0.0
                  ? yaep__Phi_fast((mus[j] - s->cand_t[j]) * s->cand_isd[j])
                  : (mus[j] > s->cand_t[j] ? 1.0 : 0.0);
            V0 += s->cand_p0[j];
        }
        /* An expected level set of no candidate or of every candidate: the
         * model believes the level is outside the box, every look-ahead
         * changes the volume by almost nothing, and the argmax is noise. The
         * straddle aims at the level itself, so it walks toward the edge the
         * level is past. */
        if (YAEP__EAVC_GUARD && (V0 < 0.5 || V0 > (double)M - 0.5)) acq = YAEP_ACQ_LSE;
    }
    for (int j = 0; j < M; j++) {
        double mu = mus[j], sd = sds[j], other = 0.0, sc = 0.0;
        if (K > 1) {
            double fs[YAEP_MAX_OUTCOMES] = { 0.0 };
            int nr = 0;
            for (int cc = 0; cc < K; cc++)
                if (cc != c) fs[nr++] = g->cand_mu[(size_t)cc * M + j];
            other = yaep__logsumexp(fs, nr);
        }
        if (yaep__is_opt(acq) && K > 1 && c != g->desc.target_outcome) continue;
        switch (acq) {
            case YAEP_ACQ_LSE:
            case YAEP_ACQ_BALV:
            case YAEP_ACQ_BALD:
            case YAEP_ACQ_LOCALMI:
            case YAEP_ACQ_UCB:
            case YAEP_ACQ_EI:
                sc = yaep__score_one(g, s, acq, mu, sd, other, beta, fstar);
                break;
            case YAEP_ACQ_EAVC: {
                double oy[YAEP__NSCEN] = { 0 }, op[YAEP__NSCEN] = { 0 };
                const yaep_real* row = g->cand_cov + (size_t)j * M;
                double cjj = row[j];
                int nsc = yaep__scenarios(g, s, mu, sd, other, oy, op);
                /* The sum runs over the CHANGE from the baseline volume, so
                 * a candidate the new observation cannot move across the level
                 * contributes nothing and is skipped. A baseline probability of
                 * exactly 0 or 1 means that candidate is already more than
                 * eight standard deviations from the level, and the look-ahead
                 * only ever shrinks its variance, so a mean shift below six of
                 * them leaves it there. That test costs two comparisons and
                 * saves an exponential, a square root and a division. */
                for (int t = 0; t < nsc; t++) {
                    double d1, w, coef, wd, dV = 0.0;
                    if (op[t] <= 0.0) continue;
                    yaep__site(g, mu, oy[t], other, &d1, &w);
                    coef = d1 / (1.0 + w * cjj);
                    wd = w / (1.0 + w * cjj);
                    for (int i = 0; i < M; i++) {
                        double dij = row[i], shift = dij * coef, v, z;
                        double p0 = s->cand_p0[i];
                        if ((p0 == 0.0 || p0 == 1.0) &&
                            fabs(shift) * s->cand_isd[i] < 2.0) continue;
                        v = s->cand_dv[i] - dij * dij * wd;
                        if (v < 1e-12) v = 1e-12;
                        z = (mus[i] + shift - s->cand_t[i]) / sqrt(v);
                        dV += yaep__Phi_fast(z) - p0;
                    }
                    sc += op[t] * fabs(dV);
                }
                break;
            }
            default:
                sc = 0.0;
                break;
        }
        s->score[j] += sc;
    }
}

/* Score every candidate under desc.acq. */
/* EI's incumbent: the best signed posterior mean of the target quantity at a
 * stimulus already tried. The posterior mean and not the observed outcome,
 * because a binary outcome is not the quantity being optimized. */
static void yaep__opt_incumbent(yaep_gp* g) {
    yaep__scr s;
    int nd = g->desc.n_dims;
    double best = -HUGE_VAL;
    yaep__scr_of(g, &s);
    for (int i = 0; i < g->N; i++) {
        double v = yaep__mean_score(g, &s, g->X + (size_t)i * nd);
        if (v > best) best = v;
    }
    if (g->N == 0) best = yaep__opt_sign(g) * yaep__q_smooth(g, &s, g->hyper.mean, 0.0, 0.0);
    g->opt_best = best;
}

/* A standard normal variate from the caller's generator (Box and Muller). */
static double yaep__normal(const yaep_gp* g) {
    double u1 = g->desc.rng(g->desc.rng_ctx), u2 = g->desc.rng(g->desc.rng_ctx);
    if (!(u1 > 1e-300)) u1 = 1e-300;
    return sqrt(-2.0 * log(u1)) * cos(2.0 * 3.14159265358979323846 * u2);
}

/* Thompson sampling: one draw of the target latent at every candidate from
 * its joint posterior (the M x M covariance the look-ahead builds, factored
 * with a jitter that grows until it factors), and the signed target quantity
 * of the draw as the score. Under CATEGORICAL only the target class is drawn
 * and the others stay at their means, the one-against-the-rest
 * approximation of the rest of the header. */
static void yaep__thompson(yaep_gp* g, yaep__scr* s) {
    int M = g->M, K = g->K, kv = K > 1 ? g->desc.target_outcome : 0;
    double sg = yaep__opt_sign(g), dmean = 0.0, jit;
    double* z = s->cand_t;
    bool ok = false;
    yaep__cand_cov_build(g, kv);
    for (int j = 0; j < M; j++) dmean += g->cand_cov[(size_t)j * M + j];
    dmean = dmean > 0.0 ? dmean / M : 1.0;
    for (jit = 1e-9 * dmean; jit < 1e-2 * dmean && !ok; jit *= 100.0) {
        if (jit > 1e-9 * dmean) yaep__cand_cov_build(g, kv);
        for (int j = 0; j < M; j++) g->cand_cov[(size_t)j * M + j] += (yaep_real)jit;
        ok = yaep__chol(g->cand_cov, M, M);
    }
    for (int j = 0; j < M; j++) z[j] = yaep__normal(g);
    for (int i = 0; i < M; i++) {
        double f = g->cand_mu[(size_t)kv * M + i], other = 0.0;
        if (ok) {
            const yaep_real* row = g->cand_cov + (size_t)i * M;
            for (int j = 0; j <= i; j++) f += row[j] * z[j];
        } else {
            f += s->cand_sd[(size_t)kv * M + i] * z[i];   /* independent draws */
        }
        if (K > 1) {
            double fs[YAEP_MAX_OUTCOMES] = { 0.0 };
            int nr = 0;
            for (int cc = 0; cc < K; cc++)
                if (cc != kv) fs[nr++] = g->cand_mu[(size_t)cc * M + i];
            other = yaep__logsumexp(fs, nr);
        }
        s->score[i] = sg * yaep__q_of_f(g, f, other);
    }
}

static void yaep__acq_fill(yaep_gp* g) {
    yaep__scr s;
    int M = g->M, K = g->K;
    bool look = (g->desc.acq == YAEP_ACQ_EAVC);
    if (g->desc.acq == YAEP_ACQ_EI) yaep__opt_incumbent(g);
    if (yaep__is_ps(g)) { yaep__ps_fill(g); return; }
    yaep__scr_of(g, &s);
    for (int j = 0; j < M; j++) s.score[j] = 0.0;
    if (g->desc.acq == YAEP_ACQ_RANDOM) { yaep__cand_fill(g, -1); return; }
    if (g->desc.acq == YAEP_ACQ_THOMPSON) {
        yaep__thompson(g, &s);
        g->cand_valid = true;
        return;
    }
    for (int c = 0; c < K; c++) {
        if (look) yaep__cand_cov_build(g, c);
        else if (c == 0) yaep__cand_fill(g, -1);
        if (yaep__mono_acq(g)) yaep__mono_cands(g, &s);
        yaep__acq_class(g, &s, c);
    }
    g->cand_valid = true;
}

/* --- candidate set ------------------------------------------------------ */

static void yaep__candidates_make(yaep_gp* g) {
    const yaep_desc* d = &g->desc;
    int nd = d->n_dims, M = g->M;
    int any = 0;
    if (d->candidates) return;
    for (int i = 0; i < nd; i++) if (d->grid[i] != 0) any = 1;
    if (any) {
        /* Product grid, last dimension fastest, endpoints included. */
        for (int j = 0; j < M; j++) {
            int rest = j;
            for (int i = nd - 1; i >= 0; i--) {
                int n = yaep__grid_n(d, i), k = rest % n;
                rest /= n;
                g->cand[(size_t)j * nd + i] = yaep__coord(d, i, n == 1
                    ? 0.5 * (d->lo[i] + d->hi[i])
                    : d->lo[i] + (d->hi[i] - d->lo[i]) * (double)k / (double)(n - 1));
            }
        }
    } else {
        for (int j = 0; j < M; j++)
            for (int i = 0; i < nd; i++)
                g->cand[(size_t)j * nd + i] = yaep__from_unit(d, i,
                    yaep__radinv((unsigned)j + 1u, yaep__nth_prime(i)));
    }
}

/* A point from the caller's generator, or the next Halton point. Both walk the
 * same counter so a run stays reproducible from the caller's seed. */
static void yaep__free_point(yaep_gp* g, double* x) {
    const yaep_desc* d = &g->desc;
    int nd = d->n_dims;
    if (d->rng) {
        for (int i = 0; i < nd; i++) {
            double u = yaep__clamp(d->rng(d->rng_ctx), 0.0, 1.0);
            x[i] = yaep__from_unit(d, i, u);
        }
    } else {
        g->halton_index++;
        for (int i = 0; i < nd; i++)
            x[i] = yaep__from_unit(d, i,
                   yaep__radinv((unsigned)g->halton_index, yaep__nth_prime(i)));
    }
}

/* --- prediction helpers ------------------------------------------------- */

/* The latent the target quantity is read from: latent 0, or the target class
 * under CATEGORICAL, where `other` carries the log-sum-exp of the competing
 * class means. */
static void yaep__target_latent(const yaep_gp* g, const yaep__scr* s,
                                 const double* x, double* mu, double* sd,
                                 double* other, double* w1, double* w2) {
    if (g->K > 1) {
        double fs[YAEP_MAX_OUTCOMES] = { 0.0 };
        int kt = g->desc.target_outcome, nr = 0;
        double m, v;
        for (int c = 0; c < g->K; c++) {
            yaep__predict_k(g, s, x, c, &m, &v, w1, w2);
            if (c == kt) { *mu = m; *sd = sqrt(v); }
            else fs[nr++] = m;
        }
        *other = yaep__logsumexp(fs, nr);
    } else {
        double m, v;
        yaep__predict_k(g, s, x, 0, &m, &v, w1, w2);
        *mu = m;
        *sd = sqrt(v);
        *other = 0.0;
    }
}

/* target_latent with the monotonic projection of the mean, for everything that
 * reports a probability or a threshold. */
static void yaep__target_latent_p(const yaep_gp* g, const yaep__scr* s,
                                   const double* x, double* mu, double* sd,
                                   double* other, double* w1, double* w2) {
    yaep__target_latent(g, s, x, mu, sd, other, w1, w2);
    if (g->desc.monotone_dims) *mu = yaep__mono_mu(g, s, x, *mu, w1);
}

static bool yaep__in_box(const yaep_gp* g, const double* x) {
    const yaep_desc* d = &g->desc;
    for (int i = 0; i < d->n_dims; i++) {
        double tol = 1e-9 * (d->hi[i] - d->lo[i]);
        if (!(x[i] >= d->lo[i] - tol && x[i] <= d->hi[i] + tol)) return false;
    }
    return true;
}

/* --- threshold ---------------------------------------------------------- */

/* Points along the intensity dimension are compared on this many samples
 * before the bisection runs, so a second crossing is seen rather than bisected
 * away. The count follows the candidate spacing along the dimension. */
static int yaep__scan_n(const yaep_gp* g) {
    const yaep_desc* d = &g->desc;
    int id = d->intensity_dim, n;
    if (!d->candidates) {
        int any = 0;
        for (int i = 0; i < d->n_dims; i++) if (d->grid[i] != 0) any = 1;
        if (any && d->grid[id] > 1) return d->grid[id];
    }
    n = (int)ceil(pow((double)g->M, 1.0 / (double)d->n_dims));
    if (n < 9) n = 9;
    if (n > 65) n = 65;
    return n;
}

/* The target quantity along the intensity dimension, either averaged over the
 * latent posterior (nsd = 0, what the manual calls yaep_predict_p) or read off
 * the latent curve shifted by nsd standard deviations, which is what the band
 * edges are. */
static double yaep__curve(const yaep_gp* g, const yaep__scr* s, double* x,
                           double v, double nsd, double* w1, double* w2) {
    double mu, sd, other;
    x[g->desc.intensity_dim] = v;
    yaep__target_latent_p(g, s, x, &mu, &sd, &other, w1, w2);
    if (nsd == 0.0) return yaep__q_smooth(g, s, mu, sd, other);
    return yaep__q_of_f(g, mu + nsd * sd, other);
}

/* Scan, then bisect the first bracket. Returns false when the curve never
 * crosses inside the box; `cross` counts the brackets found. */
static bool yaep__cross(const yaep_gp* g, const yaep__scr* s, double* x,
                         double target, double nsd, double* out, int* cross,
                         double* w1, double* w2) {
    int id = g->desc.intensity_dim, ns = yaep__scan_n(g);
    double lo = g->desc.lo[id], hi = g->desc.hi[id];
    double a = lo, qa = yaep__curve(g, s, x, lo, nsd, w1, w2) - target;
    double fa = qa, fb = 0.0, b = lo;
    bool have = false;
    *cross = 0;
    for (int k = 1; k < ns; k++) {
        double v = lo + (hi - lo) * (double)k / (double)(ns - 1);
        double qv = yaep__curve(g, s, x, v, nsd, w1, w2) - target;
        if ((qa <= 0.0 && qv > 0.0) || (qa >= 0.0 && qv < 0.0)) {
            (*cross)++;
            if (!have) { a = lo + (hi - lo) * (double)(k - 1) / (double)(ns - 1);
                         b = v; fa = qa; fb = qv; have = true; }
        }
        qa = qv;
    }
    if (!have) return false;
    for (int it = 0; it < 40; it++) {
        double mid = 0.5 * (a + b);
        double fm = yaep__curve(g, s, x, mid, nsd, w1, w2) - target;
        if ((fa <= 0.0 && fm > 0.0) || (fa >= 0.0 && fm < 0.0)) { b = mid; fb = fm; }
        else { a = mid; fa = fm; }
    }
    (void)fb;
    *out = 0.5 * (a + b);
    return true;
}

/* --- public: lifecycle -------------------------------------------------- */

YAEP_API const char* yaep_version(void) { return YAEP_VERSION_STRING; }

YAEP_API const char* yaep_strerror(int code) {
    switch (code) {
        case YAEP_ERR_ARG:     return "bad argument";
        case YAEP_ERR_CLOSED:  return "handle not open";
        case YAEP_ERR_FULL:    return "max_trials recorded";
        case YAEP_ERR_MEMORY:  return "buffer too small or allocation failed";
        case YAEP_ERR_NOCROSS: return "no level-set crossing inside the box";
        case YAEP_ERR_NUMERIC: return "Cholesky failed; posterior unchanged";
#ifdef YAEP_ASYNC
        case YAEP_ERR_BUSY:    return "queue is full";
        case YAEP_ERR_TIMEOUT: return "timed out";
#endif
        default:                return code >= 0 ? "ok" : "unknown error";
    }
}

YAEP_API const char* yaep_error(const yaep_gp* g) {
    return g ? g->error : "null gp handle";
}

YAEP_API bool yaep_is_open(const yaep_gp* g) {
    return g && g->open;
}

YAEP_API size_t yaep_memory_size(const yaep_desc* desc) {
    int Nmax, M, K;
    if (!yaep__validate(desc, NULL, 0, &Nmax, &M, &K)) return 0;
    /* The eight bytes are the alignment slack a caller's buffer may need. */
    return yaep__layout(desc, Nmax, M, K, NULL, NULL, NULL) + sizeof(double);
}

static void yaep__hyper_defaults(yaep_gp* g) {
    yaep_hyper* h = &g->hyper;
    const yaep_desc* d = &g->desc;
    *h = d->hyper;
    for (int i = 0; i < d->n_dims; i++) {
        double q = 0.25 * yaep__ls_range(d, i);
        if (h->lengthscale[i] == 0.0) h->lengthscale[i] = q;
        if (h->lengthscale_b[i] == 0.0) h->lengthscale_b[i] = q;
    }
    if (h->outputscale == 0.0) h->outputscale = 1.0;
    if (h->outputscale_b == 0.0) h->outputscale_b = 1.0;
    if (h->noise_sd == 0.0) h->noise_sd = 0.1;
    if (d->model == YAEP_MODEL_PSYCHOMETRIC) {
        int id = d->intensity_dim;
        for (int i = 0; i < d->n_dims; i++)
            if (h->lengthscale_g[i] == 0.0)
                h->lengthscale_g[i] = 0.25 * yaep__ls_range(d, i);
        if (d->hyper.outputscale == 0.0) h->outputscale = yaep__ps_os_m(d);
        if (h->mean == 0.0) h->mean = 0.5 * (d->lo[id] + d->hi[id]);
        if (h->outputscale_g == 0.0) h->outputscale_g = g->priors.outputscale_g.center;
        if (h->mean_g == 0.0)
            h->mean_g = log(1.0 / (g->priors.mean_g.center * yaep__ispan(d)));
    }
    if (d->lik == YAEP_LIK_ORDINAL && h->cutpoint[0] == 0.0)
        for (int m = 0; m < d->n_outcomes - 1; m++) h->cutpoint[m] = (double)m;
}

static int yaep__n_init(const yaep_gp* g) {
    return g->desc.n_init > 0 ? g->desc.n_init : 2 * g->desc.n_dims + 2;
}

YAEP_API bool yaep_open(yaep_gp* g, const yaep_desc* desc) {
    int Nmax = 0, M = 0, K = 0;
    size_t need;
    unsigned char* base;
    yaep__scr s;
    if (!g) return false;
    memset(g, 0, sizeof(*g));
    if (!yaep__validate(desc, g->error, sizeof(g->error), &Nmax, &M, &K))
        return false;
    g->desc = *desc;
    g->priors = yaep__priors_resolve(desc);
    for (int i = 0; i < desc->n_dims; i++)
        if (desc->dim_kind[i] != YAEP_DIM_CONTINUOUS) g->mixed = true;
    g->N_max = Nmax;
    g->M = M;
    g->K = K;
    need = yaep__layout(desc, Nmax, M, K, NULL, NULL, NULL) + sizeof(double);
    if (desc->memory) {
        if (desc->memory_size < need) {
            yaep__err(g->error, sizeof(g->error),
                       "desc.memory_size is %lu, yaep_memory_size() needs %lu",
                       (unsigned long)desc->memory_size, (unsigned long)need);
            return false;
        }
        g->mem = desc->memory;
        g->mem_owned = false;
    } else {
        g->mem = YAEP_MALLOC(need);
        if (!g->mem) {
            yaep__err(g->error, sizeof(g->error), "allocation of %lu bytes failed",
                       (unsigned long)need);
            return false;
        }
        g->mem_owned = true;
    }
    g->mem_size = need;
    /* Align the base, which is why yaep_memory_size() asks for the slack. */
    base = yaep__base(g->mem);
    yaep__layout(desc, Nmax, M, K, base, g, &s);
    memset(base, 0, need - (size_t)(base - (unsigned char*)g->mem));
    yaep__hyper_defaults(g);
    yaep__gh_init(s.gh_x, s.gh_w, YAEP_QUAD_N);
    yaep__candidates_make(g);
    g->proposed = -1;
    g->last_index = -1;
    g->repeats = 0;
    g->ps_tol = YAEP__PS_TOL;
    g->fit_flip = -1.0;
    g->fit_blocked = false;
    g->N_fit = 0;
    g->fit_valid = true;
    g->cand_valid = false;
    g->stop = YAEP_STOP_NONE;
    g->open = true;
    return true;
}

YAEP_API void yaep_close(yaep_gp* g) {
    if (!g) return;
    if (g->mem && g->mem_owned) YAEP_FREE(g->mem);
    memset(g, 0, sizeof(*g));
}

/* --- public: stopping --------------------------------------------------- */

static yaep_stop yaep__stop_reason(const yaep_gp* g) {
    const yaep_desc* d;
    if (!g || !g->open) return YAEP_STOP_NONE;
    d = &g->desc;
    if (g->N >= g->N_max) return YAEP_STOP_FULL;
    if (d->stop_trials > 0 && g->N >= d->stop_trials) return YAEP_STOP_TRIALS;
    if (d->stop_threshold_sd > 0.0 && g->N > 0) {
        double x = 0.0, lo = 0.0, hi = 0.0;
        if (yaep_threshold(g, d->stop_context, 0.0, &x, &lo, &hi) == YAEP_OK &&
            hi - lo < d->stop_threshold_sd)
            return YAEP_STOP_THRESHOLD_SD;
    }
    return YAEP_STOP_NONE;
}

YAEP_API bool yaep_done(const yaep_gp* g) {
    return yaep__stop_reason(g) != YAEP_STOP_NONE;
}

YAEP_API yaep_stop yaep_stop_reason(const yaep_gp* g) {
    return yaep__stop_reason(g);
}

/* --- public: the loop --------------------------------------------------- */

static int yaep__next_impl(yaep_gp* g, const int* subset, int nsub, double* x) {
    yaep__scr s;
    int nd, n_init, best = -1, ties = 1;
    double bscore = 0.0;
    if (!g || !g->open) return YAEP_ERR_CLOSED;
    if (!x) return YAEP_ERR_CLOSED;
    if (g->N >= g->N_max) return YAEP_ERR_FULL;
    nd = g->desc.n_dims;
    if (subset) {
        if (nsub <= 0) return YAEP_ERR_CLOSED;
        for (int i = 0; i < nsub; i++)
            if (subset[i] < 0 || subset[i] >= g->M) return YAEP_ERR_CLOSED;
    }
    /* A proposal that has not been answered yet is handed out again, so a
     * caller that asks twice shows the same stimulus. */
    if (g->history[g->N].proposed) {
        yaep__copy(x, g->history[g->N].x, nd);
        return g->proposed;
    }
    yaep__scr_of(g, &s);
    n_init = yaep__n_init(g);
    if ((!subset && g->N < n_init) ||
        (!subset && g->desc.acq == YAEP_ACQ_RANDOM)) {
        yaep__free_point(g, x);
        g->proposed = -1;
    } else if (subset && g->desc.acq == YAEP_ACQ_RANDOM) {
        double p[YAEP_MAX_DIMS] = { 0 }, bd = 0.0;
        yaep__free_point(g, p);
        for (int i = 0; i < nsub; i++) {
            const double* c = yaep__cand(g, subset[i]);
            double dd = 0.0;
            for (int k = 0; k < nd; k++) {
                double t = (c[k] - p[k]) / (g->desc.hi[k] - g->desc.lo[k]);
                dd += t * t;
            }
            if (best < 0 || dd < bd) { bd = dd; best = subset[i]; }
        }
        yaep__copy(x, yaep__cand(g, best), nd);
        g->proposed = best;
    } else {
        double ssum = 0.0;
        int nscored = 0;
        if (!g->cand_valid) yaep__acq_fill(g);
        for (int i = 0; i < (subset ? nsub : g->M); i++) {
            int j = subset ? subset[i] : i;
            double sc = s.score[j];
            ssum += sc;
            nscored++;
            if (best < 0 || sc > bscore + 1e-12 * (1.0 + fabs(bscore))) {
                best = j; bscore = sc; ties = 1;
            } else if (sc > bscore - 1e-12 * (1.0 + fabs(bscore))) {
                /* A tie: the lowest index wins, or the caller's generator
                 * picks, by reservoir so no list of ties is kept. */
                ties++;
                if (g->desc.rng && g->desc.rng(g->desc.rng_ctx) < 1.0 / (double)ties)
                    best = j;
            }
        }
        if (best < 0) return YAEP_ERR_CLOSED;
        /* Two ways an acquisition stops saying anything, both of them real: a
         * posterior so diffuse that every candidate scores the same, and a
         * posterior so confident that one candidate wins forever. Either way
         * the run stops learning, so fall back to a Halton point, which is
         * where the init phase would have put the trial anyway. */
        if (!subset) {
            double mean = nscored > 0 ? ssum / (double)nscored : 0.0;
            bool flat = bscore - mean <= 1e-9 * (1.0 + fabs(mean));
            g->repeats = (best == g->last_index) ? g->repeats + 1 : 1;
            g->last_index = best;
            /* Returning to the optimum is what an optimization acquisition is
             * for, so only the flat test applies to those. */
            if (yaep__is_opt(g->desc.acq)) g->repeats = 0;
            if (flat || g->repeats > YAEP__REPEAT_MAX) {
                yaep__free_point(g, x);
                g->repeats = 0;
                g->last_index = -1;
                g->proposed = -1;
                yaep__copy(g->history[g->N].x, x, nd);
                g->history[g->N].proposed = 1;
                g->history[g->N].init = 0;
                return -1;
            }
        }
        yaep__copy(x, yaep__cand(g, best), nd);
        g->proposed = best;
        /* The repeat guard above counts grid winners, not refined points, so
         * a run that keeps refining around one candidate still trips it. */
        if (!subset && g->desc.refine_steps > 0 &&
            g->desc.acq != YAEP_ACQ_THOMPSON &&
            yaep__refine(g, &s, x, yaep__point_score))
            g->proposed = -1;
    }
    yaep__copy(g->history[g->N].x, x, nd);
    g->history[g->N].proposed = 1;
    g->history[g->N].init = (uint8_t)((!subset && g->N < n_init) ? 1 : 0);
    return g->proposed;
}

static int yaep__next(yaep_gp* g, double* x) {
    if (g && g->open && yaep__is_pair(g)) return YAEP_ERR_CLOSED;  /* next_pair */
    return yaep__next_impl(g, NULL, 0, x);
}

/* Two standard normal draws per candidate pair of samples need the M x M
 * factor once; this is its factorization with a jitter that grows until it
 * factors. Returns false to fall back to independent draws. */
static bool yaep__cand_cov_chol(yaep_gp* g, int kv) {
    int M = g->M;
    double dmean = 0.0, jit;
    bool ok = false;
    yaep__cand_cov_build(g, kv);
    for (int j = 0; j < M; j++) dmean += g->cand_cov[(size_t)j * M + j];
    dmean = dmean > 0.0 ? dmean / M : 1.0;
    for (jit = 1e-9 * dmean; jit < 1e-2 * dmean && !ok; jit *= 100.0) {
        if (jit > 1e-9 * dmean) yaep__cand_cov_build(g, kv);
        for (int j = 0; j < M; j++) g->cand_cov[(size_t)j * M + j] += (yaep_real)jit;
        ok = yaep__chol(g->cand_cov, M, M);
    }
    return ok;
}

/* The argmax over candidates of one posterior draw of the utility, skipping
 * candidate `not`. */
static int yaep__draw_argmax(yaep_gp* g, yaep__scr* s, bool ok, int not_j) {
    int M = g->M, best = -1;
    double bv = -HUGE_VAL, sg = yaep__opt_sign(g);
    double* z = s->cand_t;
    for (int j = 0; j < M; j++) z[j] = yaep__normal(g);
    for (int i = 0; i < M; i++) {
        double f = g->cand_mu[i];
        if (ok) {
            const yaep_real* row = g->cand_cov + (size_t)i * M;
            for (int j = 0; j <= i; j++) f += row[j] * z[j];
        } else {
            f += s->cand_sd[i] * z[i];
        }
        if (i != not_j && (best < 0 || sg * f > bv)) { bv = sg * f; best = i; }
    }
    return best;
}

static int yaep__next_pair(yaep_gp* g, double* x1, double* x2) {
    yaep__scr s;
    int nd, M, j1 = -1, j2 = -1;
    if (!g || !g->open) return YAEP_ERR_CLOSED;
    if (!yaep__is_pair(g) || !x1 || !x2) return YAEP_ERR_ARG;
    if (g->N >= g->N_max) return YAEP_ERR_FULL;
    nd = g->desc.n_dims;
    M = g->M;
    if (g->history[g->N].proposed) {      /* asked twice: the same pair */
        yaep__copy(x1, g->history[g->N].x, nd);
        yaep__copy(x2, g->history[g->N].x2, nd);
        return YAEP_OK;
    }
    if (!g->fit_valid) return YAEP_ERR_NUMERIC;
    yaep__scr_of(g, &s);
    if (g->N < yaep__n_init(g) || g->desc.acq == YAEP_ACQ_RANDOM) {
        yaep__free_point(g, x1);
        yaep__free_point(g, x2);
    } else if (g->desc.acq == YAEP_ACQ_THOMPSON) {
        /* Two independent draws of the utility; each one's favorite. */
        bool ok = yaep__cand_cov_chol(g, 0);
        j1 = yaep__draw_argmax(g, &s, ok, -1);
        j2 = yaep__draw_argmax(g, &s, ok, j1);
    } else {
        /* The best candidate so far against the one whose comparison with it
         * is most informative: the difference d = f(x1) - f(x2) is Gaussian
         * with variance var1 + var2 - 2 cov12, cov12 from the L^-1 W^1/2 k
         * columns the candidate cache keeps. */
        double sg = yaep__opt_sign(g), bv = -HUGE_VAL, bs = -HUGE_VAL;
        int n = g->N_fit, ld = g->N_max;
        yaep__cand_fill(g, 0);
        for (int j = 0; j < M; j++)
            if (j1 < 0 || sg * g->cand_mu[j] > bv) { bv = sg * g->cand_mu[j]; j1 = j; }
        for (int j = 0; j < M; j++) {
            double md, vd, sc, c12;
            if (j == j1) continue;
            c12 = yaep__kernel(g, yaep__cand(g, j1), yaep__cand(g, j)) -
                  yaep__dotrr(s.V + (size_t)j1 * ld, s.V + (size_t)j * ld, n);
            md = g->cand_mu[j1] - g->cand_mu[j];
            vd = s.cand_sd[j1] * s.cand_sd[j1] + s.cand_sd[j] * s.cand_sd[j] - 2.0 * c12;
            if (vd < 0.0) vd = 0.0;
            sc = g->desc.acq == YAEP_ACQ_BALV ? yaep__q_var(g, &s, md, sqrt(vd), 0.0)
                                               : yaep__bald(g, &s, md, sqrt(vd), 0.0);
            if (j2 < 0 || sc > bs) { bs = sc; j2 = j; }
        }
    }
    if (j1 >= 0) yaep__copy(x1, yaep__cand(g, j1), nd);
    if (j2 >= 0) yaep__copy(x2, yaep__cand(g, j2), nd);
    g->cand_valid = false;
    yaep__copy(g->history[g->N].x, x1, nd);
    yaep__copy(g->history[g->N].x2, x2, nd);
    g->history[g->N].proposed = 1;
    g->history[g->N].init = (uint8_t)(g->N < yaep__n_init(g) ? 1 : 0);
    return YAEP_OK;
}

static int yaep__next_subset(yaep_gp* g, const int* subset, int n, double* x) {
    if (!subset) return YAEP_ERR_CLOSED;
    return yaep__next_impl(g, subset, n, x);
}

YAEP_API double yaep_acq_score(const yaep_gp* g, int index) {
    yaep__scr s;
    yaep_gp* m = (yaep_gp*)g;   /* the cache is the handle's own memory */
    if (!g || !g->open || index < 0 || index >= g->M) return (double)NAN;
    if (g->desc.acq == YAEP_ACQ_RANDOM || g->N < yaep__n_init(g))
        return (double)NAN;
    if (!g->cand_valid) yaep__acq_fill(m);
    yaep__scr_of(g, &s);
    return s.score[index];
}

static int yaep__record(yaep_gp* g, const double* x, const double* x2, double yv) {
    int n, nd, rc, n_init;
    double xs[YAEP_MAX_DIMS] = { 0 }, xs2[YAEP_MAX_DIMS] = { 0 };
    if (!g || !g->open) return YAEP_ERR_CLOSED;
    if (!x || (yaep__is_pair(g) && !x2)) return YAEP_ERR_ARG;
    if (g->N >= g->N_max) return YAEP_ERR_FULL;
    if (!yaep__in_box(g, x) || (x2 && !yaep__in_box(g, x2))) return YAEP_ERR_ARG;
    nd = g->desc.n_dims;
    if (g->mixed) {
        /* The history holds what was shown: integers and levels. */
        for (int i = 0; i < nd; i++) xs[i] = yaep__coord(&g->desc, i, x[i]);
        x = xs;
        if (x2) {
            for (int i = 0; i < nd; i++) xs2[i] = yaep__coord(&g->desc, i, x2[i]);
            x2 = xs2;
        }
    }
    n = g->N;
    n_init = yaep__n_init(g);
    {
        yaep_trial* t = &g->history[n];
        bool same = t->proposed != 0;
        for (int i = 0; i < nd && same; i++) if (t->x[i] != x[i]) same = false;
        if (x2) for (int i = 0; i < nd && same; i++) if (t->x2[i] != x2[i]) same = false;
        yaep__copy(t->x, x, nd);
        if (x2) yaep__copy(t->x2, x2, nd);
        t->y = yv;
        t->proposed = (uint8_t)(same ? 1 : 0);
        t->init = (uint8_t)(n < n_init ? 1 : 0);
    }
    yaep__copy(g->X + (size_t)n * nd, x, nd);
    if (x2) yaep__copy(g->X2 + (size_t)n * nd, x2, nd);
    g->y[n] = yv;
    g->N = n + 1;
    g->proposed = -1;
    g->cand_valid = false;
    if (g->N < g->N_max) memset(&g->history[g->N], 0, sizeof(yaep_trial));
    yaep__kmat_row(g, n);
    g->fit_state = YAEP__FIT_IDLE;   /* a stepped fit was fitting other data */
    g->fit_th_n = 0;
    g->stash_ok = false;
    /* The cheap path when desc.refit_every says so, with the exact refit as
     * the fallback if the bordered Cholesky will not factor. */
    if (yaep__can_grow(g) && (g->N % g->desc.refit_every) != 0)
        rc = yaep__laplace_grow(g);
    else
        rc = yaep__infer(g, YAEP__START_ROW, n);
    if (rc != YAEP_OK && yaep__can_grow(g))
        rc = yaep__infer(g, YAEP__START_ROW, n);
    if (rc != YAEP_OK) {
        /* Put the posterior of the trials that did factor back in force. The
         * leading block of the kernel matrix is untouched by the new row, so
         * refitting at the old count reproduces it. */
        int saved = g->N;
        g->N = g->N_fit;
        if (g->N > 0) {
            if (yaep__infer(g, YAEP__START_COLD, 0) != YAEP_OK) g->fit_valid = false;
        } else {
            g->fit_valid = true;
            g->log_marginal = 0.0;
        }
        g->N = saved;
        g->stop = yaep__stop_reason(g);
        return YAEP_ERR_NUMERIC;
    }
    if (g->desc.fit && g->desc.fit_every > 0 && g->N >= n_init &&
        (g->N - n_init) % g->desc.fit_every == 0) {
        rc = yaep__fit_run(g, false);
        if (rc < 0) { g->stop = yaep__stop_reason(g); return rc; }
    }
    g->stop = yaep__stop_reason(g);
    return YAEP_OK;
}

static int yaep__update(yaep_gp* g, const double* x, int outcome) {
    int nout;
    if (!g || !g->open) return YAEP_ERR_CLOSED;
    if (g->desc.lik == YAEP_LIK_GAUSSIAN || yaep__is_pair(g)) return YAEP_ERR_ARG;
    nout = yaep__bern(g) ? 2 : g->desc.n_outcomes;
    if (outcome < 0 || outcome >= nout) return YAEP_ERR_ARG;
    return yaep__record(g, x, NULL, (double)outcome);
}

static int yaep__update_pair(yaep_gp* g, const double* x1, const double* x2,
                             int outcome) {
    if (!g || !g->open) return YAEP_ERR_CLOSED;
    if (!yaep__is_pair(g) || (outcome != 0 && outcome != 1)) return YAEP_ERR_ARG;
    return yaep__record(g, x1, x2, (double)outcome);
}

YAEP_API double yaep_predict_pair(const yaep_gp* g, const double* x1,
                                    const double* x2) {
    yaep__scr s;
    double m1, v1, m2, v2, c12, vd;
    int n, ld;
    if (!g || !g->open || !yaep__is_pair(g) || !x1 || !x2 || !g->fit_valid ||
        !yaep__in_box(g, x1) || !yaep__in_box(g, x2))
        return (double)NAN;
    yaep__scr_of(g, &s);
    n = g->N_fit; ld = g->N_max;
    yaep__predict_k(g, &s, x1, 0, &m1, &v1, s.t1, s.t3);
    yaep__predict_k(g, &s, x2, 0, &m2, &v2, s.t2, s.t4);
    /* t3 and t4 hold L^-1 W^1/2 k for the two points; their dot is the part
     * of the prior covariance the data explain. */
    c12 = yaep__kernel(g, x1, x2) - (n > 0 ? yaep__dot(s.t3, s.t4, n) : 0.0);
    (void)ld;
    vd = v1 + v2 - 2.0 * c12;
    return yaep__q_smooth(g, &s, m1 - m2, sqrt(vd > 0.0 ? vd : 0.0), 0.0);
}

static int yaep__update_real(yaep_gp* g, const double* x, double y) {
    if (!g || !g->open) return YAEP_ERR_CLOSED;
    if (g->desc.lik != YAEP_LIK_GAUSSIAN) return YAEP_ERR_ARG;
    if (!(y == y)) return YAEP_ERR_ARG;
    return yaep__record(g, x, NULL, y);
}

/* --- public: estimates -------------------------------------------------- */

YAEP_API int yaep_predict_f(const yaep_gp* g, const double* x, int k,
                              double* mu, double* sd) {
    yaep__scr s;
    double m, v;
    if (!g || !g->open) return YAEP_ERR_CLOSED;
    if (!x || k < 0 || k >= (yaep__is_ps(g) ? 2 : g->K) || !yaep__in_box(g, x))
        return YAEP_ERR_ARG;
    if (!g->fit_valid) return YAEP_ERR_NUMERIC;
    yaep__scr_of(g, &s);
    if (yaep__is_ps(g)) {
        yaep__mg o;
        yaep__ps_post(g, &s, x, &o);
        m = k == 0 ? o.mm : o.mg;
        v = k == 0 ? o.vm : o.vg;
    } else {
        yaep__predict_k(g, &s, x, k, &m, &v, s.t1, s.t2);
    }
    if (mu) *mu = m;
    if (sd) *sd = sqrt(v);
    return YAEP_OK;
}

YAEP_API double yaep_predict_p(const yaep_gp* g, const double* x) {
    yaep__scr s;
    double mu, sd, other;
    if (!g || !g->open || !x || !yaep__in_box(g, x) || !g->fit_valid ||
        yaep__is_pair(g))
        return (double)NAN;
    yaep__scr_of(g, &s);
    if (yaep__is_ps(g)) {
        yaep__mg o;
        double eq;
        yaep__ps_post(g, &s, x, &o);
        yaep__ps_moments(g, &s, x[g->desc.intensity_dim], &o, &eq, NULL);
        return eq;
    }
    yaep__target_latent_p(g, &s, x, &mu, &sd, &other, s.t1, s.t2);
    return yaep__q_smooth(g, &s, mu, sd, other);
}

YAEP_API int yaep_predict_f_many(const yaep_gp* g, const double* xs, int n,
                                   int k, double* mu, double* sd) {
    yaep__scr s;
    int nd;
    if (!g || !g->open) return YAEP_ERR_CLOSED;
    if (!xs || n < 0 || k < 0 || k >= (yaep__is_ps(g) ? 2 : g->K))
        return YAEP_ERR_ARG;
    if (!g->fit_valid) return YAEP_ERR_NUMERIC;
    nd = g->desc.n_dims;
    for (int i = 0; i < n; i++)
        if (!yaep__in_box(g, xs + (size_t)i * nd)) return YAEP_ERR_ARG;
    yaep__scr_of(g, &s);
    if (yaep__is_ps(g)) {
        yaep__mg o;
        const double* prev = NULL;
        memset(&o, 0, sizeof(o));
        for (int i = 0; i < n; i++) {
            const double* xi = xs + (size_t)i * nd;
            if (!prev || !yaep__same_ctx(g, prev, xi)) yaep__ps_post(g, &s, xi, &o);
            prev = xi;
            if (mu) mu[i] = k == 0 ? o.mm : o.mg;
            if (sd) sd[i] = sqrt(k == 0 ? o.vm : o.vg);
        }
        return YAEP_OK;
    }
    /* A chunk at a time, so an output the caller did not ask for goes to the
     * stack and nothing borrows the candidate cache. */
    for (int b0 = 0; b0 < n; b0 += YAEP__BLK) {
        int cnt = n - b0 < YAEP__BLK ? n - b0 : YAEP__BLK;
        double mub[YAEP__BLK] = { 0 }, sdb[YAEP__BLK] = { 0 };
        yaep__predict_many(g, &s, xs + (size_t)b0 * nd, cnt, k, mub, sdb);
        if (mu) yaep__copy(mu + b0, mub, cnt);
        if (sd) yaep__copy(sd + b0, sdb, cnt);
    }
    return YAEP_OK;
}

YAEP_API int yaep_predict_p_many(const yaep_gp* g, const double* xs, int n,
                                   double* p) {
    yaep__scr s;
    int nd, rc;
    if (!g || !g->open) return YAEP_ERR_CLOSED;
    if (!xs || !p || n < 0) return YAEP_ERR_ARG;
    if (!g->fit_valid) return YAEP_ERR_NUMERIC;
    nd = g->desc.n_dims;
    if (yaep__is_ps(g)) {
        yaep__mg o;
        const double* prev = NULL;
        for (int i = 0; i < n; i++)
            if (!yaep__in_box(g, xs + (size_t)i * nd)) return YAEP_ERR_ARG;
        yaep__scr_of(g, &s);
        memset(&o, 0, sizeof(o));
        for (int i = 0; i < n; i++) {
            const double* xi = xs + (size_t)i * nd;
            if (!prev || !yaep__same_ctx(g, prev, xi)) yaep__ps_post(g, &s, xi, &o);
            prev = xi;
            yaep__ps_moments(g, &s, xi[g->desc.intensity_dim], &o, &p[i], NULL);
        }
        return YAEP_OK;
    }
    if (g->K > 1) {
        /* K coupled latents: one point at a time, since the target quantity of
         * a class needs the other classes at the same point. */
        for (int i = 0; i < n; i++) {
            p[i] = yaep_predict_p(g, xs + (size_t)i * nd);
            if (!(p[i] == p[i])) return YAEP_ERR_ARG;
        }
        return YAEP_OK;
    }
    yaep__scr_of(g, &s);
    for (int b0 = 0; b0 < n; b0 += YAEP__BLK) {
        int cnt = n - b0 < YAEP__BLK ? n - b0 : YAEP__BLK;
        double mub[YAEP__BLK] = { 0 }, sdb[YAEP__BLK] = { 0 };
        rc = yaep_predict_f_many(g, xs + (size_t)b0 * nd, cnt, 0, mub, sdb);
        if (rc != YAEP_OK) return rc;
        if (g->desc.monotone_dims)
            for (int i = 0; i < cnt; i++)
                mub[i] = yaep__mono_mu(g, &s, xs + (size_t)(b0 + i) * nd, mub[i], s.t1);
        for (int i = 0; i < cnt; i++)
            p[b0 + i] = yaep__q_smooth(g, &s, mub[i], sdb[i], 0.0);
    }
    return YAEP_OK;
}

YAEP_API double yaep_predict_p_var(const yaep_gp* g, const double* x) {
    yaep__scr s;
    double mu, sd, other;
    if (!g || !g->open || !x || !yaep__in_box(g, x) || !g->fit_valid)
        return (double)NAN;
    yaep__scr_of(g, &s);
    if (yaep__is_ps(g)) {
        yaep__mg o;
        double eq, vq;
        yaep__ps_post(g, &s, x, &o);
        yaep__ps_moments(g, &s, x[g->desc.intensity_dim], &o, &eq, &vq);
        return vq;
    }
    yaep__target_latent_p(g, &s, x, &mu, &sd, &other, s.t1, s.t2);
    return yaep__q_var(g, &s, mu, sd, other);
}

YAEP_API int yaep_predict_outcomes(const yaep_gp* g, const double* x, double* p) {
    yaep__scr s;
    if (!g || !g->open) return YAEP_ERR_CLOSED;
    if (!x || !p || !yaep__in_box(g, x)) return YAEP_ERR_ARG;
    if (g->desc.lik == YAEP_LIK_GAUSSIAN) return YAEP_ERR_ARG;
    if (!g->fit_valid) return YAEP_ERR_NUMERIC;
    yaep__scr_of(g, &s);
    if (yaep__is_ps(g)) {
        yaep__mg o;
        double eh;
        yaep__ps_post(g, &s, x, &o);
        yaep__ps_mix(g, &s, x[g->desc.intensity_dim], &o, p, &eh);
        return YAEP_OK;
    }
    if (g->K > 1) {
        /* One-latent quadrature per class, then normalized: the exact softmax
         * expectation is a K-dimensional integral. */
        double mu[YAEP_MAX_OUTCOMES] = { 0 }, sd[YAEP_MAX_OUTCOMES] = { 0 }, tot = 0.0;
        for (int c = 0; c < g->K; c++) {
            double m, v;
            yaep__predict_k(g, &s, x, c, &m, &v, s.t1, s.t2);
            mu[c] = m;
            sd[c] = sqrt(v);
        }
        for (int c = 0; c < g->K; c++) {
            double fs[YAEP_MAX_OUTCOMES] = { 0.0 }, acc = 0.0, other;
            int nr = 0;
            for (int cc = 0; cc < g->K; cc++) if (cc != c) fs[nr++] = mu[cc];
            other = yaep__logsumexp(fs, nr);
            for (int i = 0; i < YAEP_QUAD_N; i++)
                acc += s.gh_w[i] * yaep__sigmoid(mu[c] +
                       YAEP__SQRT2 * sd[c] * s.gh_x[i] - other);
            p[c] = acc / YAEP__SQRTPI;
            tot += p[c];
        }
        if (tot > 0.0) for (int c = 0; c < g->K; c++) p[c] /= tot;
    } else {
        double m, v, sd;
        int nout = yaep__bern(g) ? 2 : g->desc.n_outcomes;
        double pf[YAEP_MAX_OUTCOMES] = { 0 };
        yaep__predict_k(g, &s, x, 0, &m, &v, s.t1, s.t2);
        sd = sqrt(v);
        for (int j = 0; j < nout; j++) p[j] = 0.0;
        if (sd > 0.0) {
            for (int i = 0; i < YAEP_QUAD_N; i++) {
                double w = s.gh_w[i] / YAEP__SQRTPI;
                yaep__probs_of_f(g, m + YAEP__SQRT2 * sd * s.gh_x[i], pf);
                for (int j = 0; j < nout; j++) p[j] += w * pf[j];
            }
        } else {
            yaep__probs_of_f(g, m, p);
        }
    }
    return YAEP_OK;
}

YAEP_API int yaep_threshold(const yaep_gp* g, const double* ctx, double target,
                              double* x, double* lo, double* hi) {
    yaep__scr s;
    yaep_gp* mg = (yaep_gp*)g;
    double pt[YAEP_MAX_DIMS] = { 0 };
    double v, a, b;
    int nd, id, cross = 0, ca = 0, cb = 0;
    bool ha, hb;
    if (!g || !g->open) return YAEP_ERR_CLOSED;
    if (!g->fit_valid) return YAEP_ERR_NUMERIC;
    nd = g->desc.n_dims;
    id = g->desc.intensity_dim;
    if (nd > 1 && !ctx) return YAEP_ERR_ARG;
    if (target == 0.0)
        target = g->desc.lik == YAEP_LIK_GAUSSIAN ? g->desc.target_value
                                                   : g->desc.target_p;
    if (g->desc.lik != YAEP_LIK_GAUSSIAN &&
        !(target > g->desc.guess && target < 1.0 - g->desc.lapse))
        return YAEP_ERR_ARG;   /* no such level exists with this floor */
    for (int i = 0, k = 0; i < nd; i++) {
        if (i == id) { pt[i] = g->desc.lo[i]; continue; }
        pt[i] = yaep__coord(&g->desc, i, ctx[k++]);
    }
    for (int i = 0; i < nd; i++) {
        if (i == id) continue;
        if (!(pt[i] >= g->desc.lo[i] && pt[i] <= g->desc.hi[i])) return YAEP_ERR_ARG;
    }
    yaep__scr_of(g, &s);
    if (yaep__is_ps(g)) {
        /* The crossing of a psychometric function is m + f_t / b, and with
         * (m, g) bivariate Gaussian its mean and variance are closed form:
         * E[exp(-g)] = exp(-mg + vg / 2), Var[exp(-g)] = exp(-2 mg + 2 vg) -
         * exp(-2 mg + vg), Cov(m, exp(-g)) = -cmg E[exp(-g)]. No scan, and so
         * never more than one crossing. */
        yaep__mg o;
        double ft = yaep__f_level(g, target), e1, var, sd, lo0, hi0;
        yaep__ps_post(g, &s, pt, &o);
        e1 = exp(-o.mg + 0.5 * o.vg);
        v = o.mm + ft * e1;
        var = o.vm + ft * ft * (exp(-2.0 * o.mg + 2.0 * o.vg) -
                                exp(-2.0 * o.mg + o.vg))
              - 2.0 * ft * o.cmg * e1;
        sd = var > 0.0 ? sqrt(var) : 0.0;
        mg->multi_cross = false;
        lo0 = g->desc.lo[id];
        hi0 = g->desc.hi[id];
        if (!(v >= lo0 && v <= hi0)) return YAEP_ERR_NOCROSS;
        if (x) *x = v;
        if (lo) *lo = v - 1.96 * sd > lo0 ? v - 1.96 * sd : lo0;
        if (hi) *hi = v + 1.96 * sd < hi0 ? v + 1.96 * sd : hi0;
        return YAEP_OK;
    }
    if (!yaep__cross(g, &s, pt, target, 0.0, &v, &cross, s.t1, s.t2)) {
        mg->multi_cross = false;
        return YAEP_ERR_NOCROSS;
    }
    mg->multi_cross = (cross > 1);
    if (x) *x = v;
    if (lo || hi) {
        /* The band edges are where the latent curve shifted by +-1.96 sd meets
         * the same level. Which shift gives the lower intensity depends on the
         * sign of the slope, so they are sorted rather than assumed. */
        ha = yaep__cross(g, &s, pt, target, 1.96, &a, &ca, s.t1, s.t2);
        hb = yaep__cross(g, &s, pt, target, -1.96, &b, &cb, s.t1, s.t2);
        /* A shifted curve that never meets the level puts that edge at the
         * box's end on the far side of the other edge; when neither meets it
         * the band is the whole box. (Before v0.14.1 the both-missing case
         * read an unset edge: whatever was on the stack.) */
        if (!ha && !hb) {
            a = g->desc.lo[id];
            b = g->desc.hi[id];
        } else {
            if (!ha) a = (b > v) ? g->desc.lo[id] : g->desc.hi[id];
            if (!hb) b = (a > v) ? g->desc.lo[id] : g->desc.hi[id];
        }
        if (lo) *lo = a < b ? a : b;
        if (hi) *hi = a < b ? b : a;
    }
    return YAEP_OK;
}

YAEP_API bool yaep_threshold_multi_cross(const yaep_gp* g) {
    return g && g->open && g->multi_cross;
}

/* --- public: hyperparameters -------------------------------------------- */

static int yaep__fit(yaep_gp* g) {
    if (!g || !g->open) return YAEP_ERR_CLOSED;
    if (g->N < 2) { g->fit_delta = 0.0; return 1; }
    if (!g->fit_valid) return YAEP_ERR_NUMERIC;
    return yaep__fit_run(g, true);
}

YAEP_API double yaep_fit_delta(const yaep_gp* g) {
    if (!g || !g->open) return (double)NAN;
    return g->fit_delta;
}

static int yaep__refit(yaep_gp* g) {
    if (!g || !g->open) return YAEP_ERR_CLOSED;
    if (g->N == 0) return YAEP_OK;
    return yaep__infer(g, YAEP__START_KEEP, 0);
}

static int yaep__argmax(yaep_gp* g, double* x, double* value) {
    yaep__scr s;
    int nd, best = -1, rc;
    double bv = -HUGE_VAL;
    if (!g || !g->open) return YAEP_ERR_CLOSED;
    if (!x) return YAEP_ERR_ARG;
    if (g->desc.lik == YAEP_LIK_CATEGORICAL &&
        (g->desc.target_outcome < 0 || g->desc.target_outcome >= g->K))
        return YAEP_ERR_ARG;
    if (!g->fit_valid) return YAEP_ERR_NUMERIC;
    nd = g->desc.n_dims;
    yaep__scr_of(g, &s);
    for (int j = 0; j < g->M; j++) {
        double v = yaep__mean_score(g, &s, yaep__cand(g, j));
        if (best < 0 || v > bv) { bv = v; best = j; }
    }
    yaep__copy(x, yaep__cand(g, best), nd);
    rc = best;
    if (g->desc.refine_steps > 0 && yaep__refine(g, &s, x, yaep__mean_score)) rc = -1;
    if (value) *value = yaep__opt_sign(g) * yaep__mean_score(g, &s, x);
    return rc;
}

/* The handle-changing calls leave a message on failure (README.md,
 * Conventions); the work is in the yaep__ bodies so each return path does
 * not repeat the snprintf. `floor` is -1 for the two calls where -1 is a
 * result (a point that is not a candidate), so only a code below it is a
 * failure. */
static int yaep__fail(yaep_gp* g, int rc, int floor, const char* what) {
    if (g && rc < floor)
        snprintf(g->error, sizeof(g->error), "ysp_aep: %s: %s", what, yaep_strerror(rc));
    return rc;
}

YAEP_API int yaep_next(yaep_gp* g, double* x) {
    return yaep__fail(g, yaep__next(g, x), -1, "yaep_next");
}
YAEP_API int yaep_next_pair(yaep_gp* g, double* x1, double* x2) {
    return yaep__fail(g, yaep__next_pair(g, x1, x2), 0, "yaep_next_pair");
}
YAEP_API int yaep_next_subset(yaep_gp* g, const int* subset, int n, double* x) {
    return yaep__fail(g, yaep__next_subset(g, subset, n, x), -1, "yaep_next_subset");
}
YAEP_API int yaep_update(yaep_gp* g, const double* x, int outcome) {
    return yaep__fail(g, yaep__update(g, x, outcome), 0, "yaep_update");
}
YAEP_API int yaep_update_pair(yaep_gp* g, const double* x1, const double* x2,
                                int outcome) {
    return yaep__fail(g, yaep__update_pair(g, x1, x2, outcome), 0, "yaep_update_pair");
}
YAEP_API int yaep_update_real(yaep_gp* g, const double* x, double y) {
    return yaep__fail(g, yaep__update_real(g, x, y), 0, "yaep_update_real");
}
YAEP_API int yaep_fit_step(yaep_gp* g) {
    return yaep__fail(g, yaep__fit_step(g), 0, "yaep_fit_step");
}
YAEP_API int yaep_fit(yaep_gp* g) {
    return yaep__fail(g, yaep__fit(g), 0, "yaep_fit");
}
YAEP_API int yaep_refit(yaep_gp* g) {
    return yaep__fail(g, yaep__refit(g), 0, "yaep_refit");
}
YAEP_API int yaep_argmax(yaep_gp* g, double* x, double* value) {
    return yaep__fail(g, yaep__argmax(g, x, value), -1, "yaep_argmax");
}

YAEP_API int yaep_get_priors(const yaep_gp* g, yaep_priors* out) {
    if (!g || !g->open) return YAEP_ERR_CLOSED;
    if (!out) return YAEP_ERR_ARG;
    *out = g->priors;
    if (g->desc.no_hyper_prior) {
        out->lengthscale.sd = out->outputscale.sd = out->outputscale_b.sd = -1.0;
        out->outputscale_g.sd = out->mean.sd = out->mean_g.sd = out->noise_sd.sd = -1.0;
    }
#define YAEP__OFF(f) do { if (!(out->f.sd > 0.0)) out->f.sd = -1.0; } while (0)
    YAEP__OFF(lengthscale); YAEP__OFF(outputscale); YAEP__OFF(outputscale_b);
    YAEP__OFF(outputscale_g); YAEP__OFF(mean); YAEP__OFF(mean_g); YAEP__OFF(noise_sd);
#undef YAEP__OFF
    return YAEP_OK;
}

YAEP_API int yaep_get_hyper(const yaep_gp* g, yaep_hyper* out) {
    if (!g || !g->open) return YAEP_ERR_CLOSED;
    if (!out) return YAEP_ERR_ARG;
    *out = g->hyper;
    return YAEP_OK;
}

YAEP_API double yaep_log_marginal(const yaep_gp* g) {
    if (!g || !g->open || !g->fit_valid) return (double)NAN;
    return g->log_marginal;
}

/* --- public: candidates and history ------------------------------------- */

YAEP_API int yaep_n_candidates(const yaep_gp* g) {
    if (!g || !g->open) return YAEP_ERR_CLOSED;
    return g->M;
}

YAEP_API int yaep_candidate(const yaep_gp* g, int index, double* x) {
    if (!g || !g->open) return YAEP_ERR_CLOSED;
    if (!x || index < 0 || index >= g->M) return YAEP_ERR_ARG;
    yaep__copy(x, yaep__cand(g, index), g->desc.n_dims);
    return YAEP_OK;
}

YAEP_API int yaep_n_trials(const yaep_gp* g) {
    if (!g || !g->open) return YAEP_ERR_CLOSED;
    return g->N;
}

YAEP_API const yaep_trial* yaep_history(const yaep_gp* g, int* n) {
    if (!g || !g->open) { if (n) *n = 0; return NULL; }
    if (n) *n = g->N;
    return g->history;
}

/* --- public: snapshots -------------------------------------------------- */

#define YAEP__SNAP_FORMAT 1u

/* One writer for three jobs, as in ysp/trials.h: count (out and cmp NULL),
 * write (out), and compare against a snapshot (cmp), which is how
 * yaep_open_saved() checks the desc without a second description of the layout
 * that could drift from the first. */
typedef struct yaep__w {
    unsigned char*       out;
    const unsigned char* cmp;
    size_t               pos;
    size_t               cap;
    const char*          diff;   /* compare: the first field that differs */
} yaep__w;

static void yaep__put(yaep__w* w, uint64_t v, int nbytes, const char* name) {
    for (int i = 0; i < nbytes; i++) {
        unsigned char b = (unsigned char)((v >> (8 * i)) & 0xffu);
        if (w->out) {
            if (w->pos < w->cap) w->out[w->pos] = b;
        } else if (w->cmp && !w->diff) {
            if (w->pos >= w->cap || w->cmp[w->pos] != b) w->diff = name;
        }
        w->pos++;
    }
}

static void yaep__put_i32(yaep__w* w, int v, const char* name) {
    yaep__put(w, (uint64_t)(uint32_t)v, 4, name);
}

static void yaep__put_f64(yaep__w* w, double v, const char* name) {
    uint64_t u;
    memcpy(&u, &v, sizeof(u));
    yaep__put(w, u, 8, name);
}

static void yaep__put_f64s(yaep__w* w, const double* v, int n, const char* name) {
    for (int i = 0; i < n; i++) yaep__put_f64(w, v[i], name);
}

/* A yaep_real, as its own bit pattern: 8 bytes in a double build, 4 in a
 * float one, which the header's real size records. */
static void yaep__put_real(yaep__w* w, yaep_real v, const char* name) {
    /* A switch and not an if: the size is a constant, and MSVC /W4 flags an
     * if on a constant (C4127). */
    switch (sizeof(yaep_real)) {
        case sizeof(double): {
            uint64_t u;
            memcpy(&u, &v, sizeof(u));
            yaep__put(w, u, 8, name);
            break;
        }
        default: {
            uint32_t u;
            memcpy(&u, &v, sizeof(u));
            yaep__put(w, u, 4, name);
            break;
        }
    }
}

/* The lower triangle of an n x n matrix with leading dimension ld: all a
 * factor holds, and all of a symmetric matrix. */
static void yaep__put_tril(yaep__w* w, const yaep_real* A, int n, int ld,
                            const char* name) {
    for (int i = 0; i < n; i++)
        for (int j = 0; j <= i; j++) yaep__put_real(w, A[(size_t)i * ld + j], name);
}

static void yaep__put_hyper(yaep__w* w, const yaep_hyper* h, int nd, const char* name) {
    yaep__put_f64s(w, h->lengthscale, nd, name);
    yaep__put_f64(w, h->outputscale, name);
    yaep__put_f64(w, h->mean, name);
    yaep__put_f64s(w, h->lengthscale_b, nd, name);
    yaep__put_f64(w, h->outputscale_b, name);
    yaep__put_f64s(w, h->cutpoint, YAEP_MAX_OUTCOMES - 1, name);
    yaep__put_f64(w, h->noise_sd, name);
    yaep__put_f64s(w, h->lengthscale_g, nd, name);
    yaep__put_f64(w, h->outputscale_g, name);
    yaep__put_f64(w, h->mean_g, name);
}

static void yaep__put_prior(yaep__w* w, const yaep_prior* p, const char* name) {
    yaep__put_f64(w, p->center, name);
    yaep__put_f64(w, p->sd, name);
    yaep__put_f64(w, p->ceiling, name);
}

/* Every number of the desc, in field order; the pointers only as whether they
 * are set, since the caller re-supplies them. A caller-owned candidate set is
 * compared value by value: it is part of what the snapshot's state means. */
static void yaep__put_desc(yaep__w* w, const yaep_desc* d) {
    int nd = d->n_dims;
    yaep__put_i32(w, nd, "n_dims");
    yaep__put_f64s(w, d->lo, nd, "lo");
    yaep__put_f64s(w, d->hi, nd, "hi");
    yaep__put_i32(w, d->intensity_dim, "intensity_dim");
    yaep__put_i32(w, (int)d->lik, "lik");
    yaep__put_i32(w, d->n_outcomes, "n_outcomes");
    yaep__put_i32(w, (int)d->kernel, "kernel");
    yaep__put_i32(w, (int)d->link, "link");
    yaep__put_f64(w, d->guess, "guess");
    yaep__put_f64(w, d->lapse, "lapse");
    yaep__put_hyper(w, &d->hyper, nd, "hyper");
    yaep__put(w, d->fit ? 1u : 0u, 1, "fit");
    yaep__put_i32(w, d->fit_every, "fit_every");
    yaep__put(w, d->no_hyper_prior ? 1u : 0u, 1, "no_hyper_prior");
    yaep__put_i32(w, d->refit_every, "refit_every");
    yaep__put_hyper(w, &d->hyper_min, nd, "hyper_min");
    yaep__put_hyper(w, &d->hyper_max, nd, "hyper_max");
    yaep__put_f64(w, d->jitter, "jitter");
    yaep__put_i32(w, (int)d->acq, "acq");
    yaep__put_f64(w, d->target_p, "target_p");
    yaep__put_f64(w, d->target_value, "target_value");
    yaep__put_i32(w, d->target_outcome, "target_outcome");
    yaep__put_f64(w, d->acq_beta, "acq_beta");
    yaep__put_i32(w, d->n_init, "n_init");
    yaep__put(w, d->rng ? 1u : 0u, 1, "rng");
    yaep__put(w, d->candidates ? 1u : 0u, 1, "candidates");
    yaep__put_i32(w, d->n_candidates, "n_candidates");
    if (d->candidates)
        yaep__put_f64s(w, d->candidates, d->n_candidates * nd, "candidates[]");
    for (int i = 0; i < nd; i++) yaep__put_i32(w, d->grid[i], "grid");
    yaep__put_i32(w, d->stop_trials, "stop_trials");
    yaep__put_f64(w, d->stop_threshold_sd, "stop_threshold_sd");
    yaep__put_f64s(w, d->stop_context, nd, "stop_context");
    yaep__put_i32(w, d->max_trials, "max_trials");
    yaep__put_i32(w, d->refine_steps, "refine_steps");
    yaep__put_i32(w, (int)d->model, "model");
    yaep__put_prior(w, &d->priors.lengthscale, "priors.lengthscale");
    yaep__put_prior(w, &d->priors.outputscale, "priors.outputscale");
    yaep__put_prior(w, &d->priors.outputscale_b, "priors.outputscale_b");
    yaep__put_prior(w, &d->priors.outputscale_g, "priors.outputscale_g");
    yaep__put_prior(w, &d->priors.mean, "priors.mean");
    yaep__put_prior(w, &d->priors.mean_g, "priors.mean_g");
    yaep__put_prior(w, &d->priors.noise_sd, "priors.noise_sd");
    yaep__put(w, d->minimize ? 1u : 0u, 1, "minimize");
    for (int i = 0; i < nd; i++) yaep__put_i32(w, (int)d->dim_kind[i], "dim_kind");
    for (int i = 0; i < nd; i++) yaep__put_i32(w, d->dim_levels[i], "dim_levels");
    yaep__put(w, d->monotone_dims, 4, "monotone_dims");
    yaep__put_i32(w, d->pcg_threshold, "pcg_threshold");
    yaep__put_i32(w, d->fit_max_evals, "fit_max_evals");
    yaep__put_f64(w, d->fit_tol, "fit_tol");
    yaep__put(w, d->fit_pcg ? 1u : 0u, 1, "fit_pcg");
}

/* The handle's state after the desc: counters, the hyperparameters in force,
 * the trials, the generated candidates, and the posterior exactly as it
 * stands (mode, Hessian, factors, kernel matrices, the psychometric model's
 * vectors and a stepped fit's place), so a resumed session computes what the
 * uninterrupted one would have, bit for bit, with no refit. The candidate
 * cache is not saved: it is a function of the rest and is rebuilt on the
 * next yaep_next(). */
static void yaep__put_state(yaep__w* w, const yaep_gp* g) {
    yaep__scr s;
    int nd = g->desc.n_dims, n = g->N, ld = g->N_max, K = g->K, M = g->M;
    bool pair = g->desc.lik == YAEP_LIK_PAIRWISE, ps = yaep__is_ps(g);
    yaep__scr_of(g, &s);
    yaep__put_i32(w, n, "N");
    yaep__put_i32(w, g->N_fit, "N_fit");
    yaep__put_i32(w, g->halton_index, "halton_index");
    yaep__put_i32(w, g->proposed, "proposed");
    yaep__put_i32(w, g->last_index, "last_index");
    yaep__put_i32(w, g->repeats, "repeats");
    yaep__put_i32(w, g->fit_state, "fit_state");
    yaep__put_i32(w, g->fit_evals, "fit_evals");
    yaep__put_i32(w, g->fit_back, "fit_back");
    yaep__put_i32(w, (int)g->stop, "stop");
    yaep__put(w, g->fit_valid ? 1u : 0u, 1, "fit_valid");
    yaep__put(w, g->multi_cross ? 1u : 0u, 1, "multi_cross");
    yaep__put(w, g->fit_blocked ? 1u : 0u, 1, "fit_blocked");
    yaep__put_f64(w, g->fit_step, "fit_step");
    yaep__put_f64(w, g->fit_best, "fit_best");
    yaep__put_f64(w, g->log_marginal, "log_marginal");
    yaep__put_f64(w, g->fit_flip, "fit_flip");
    yaep__put_f64(w, g->opt_best, "opt_best");
    yaep__put_f64(w, g->ps_tol, "ps_tol");
    yaep__put_f64(w, g->fit_delta, "fit_delta");
    yaep__put_i32(w, g->fit_conv, "fit_conv");
    yaep__put_hyper(w, &g->hyper, nd, "hyper in force");
    /* The history, and the pending proposal at index N when there is one. */
    for (int i = 0; i <= n && i < ld; i++) {
        const yaep_trial* t = &g->history[i];
        yaep__put_f64s(w, t->x, nd, "history");
        yaep__put_f64(w, t->y, "history");
        yaep__put(w, t->proposed, 1, "history");
        yaep__put(w, t->init, 1, "history");
        if (pair) yaep__put_f64s(w, t->x2, nd, "history");
    }
    yaep__put_f64s(w, g->X, n * nd, "X");
    if (pair) yaep__put_f64s(w, g->X2, n * nd, "X2");
    yaep__put_f64s(w, g->y, n, "y");
    if (!g->desc.candidates) yaep__put_f64s(w, g->cand, M * nd, "candidates");
    for (int k = 0; k < K; k++) {
        yaep__put_f64s(w, g->f + (size_t)k * ld, n, "f");
        yaep__put_f64s(w, g->W + (size_t)k * ld, n, "W");
        yaep__put_f64s(w, g->grad + (size_t)k * ld, n, "grad");
        yaep__put_f64s(w, s.alpha + (size_t)k * ld, n, "alpha");
        yaep__put_f64s(w, s.sW + (size_t)k * ld, n, "sW");
        yaep__put_tril(w, g->L + (size_t)k * ld * ld, n, ld, "L");
    }
    yaep__put_tril(w, g->Kmat, n, ld, "Kmat");
    if (ps) {
        yaep__put_tril(w, g->Kg, n, ld, "Kg");
        for (int v = 0; v < YAEP__PS_VEC; v++)
            yaep__put_f64s(w, s.ps + (size_t)v * ld, n, "ps");
    }
    if (g->desc.lik == YAEP_LIK_CATEGORICAL) yaep__put_tril(w, s.Echol, n, ld, "Echol");
    yaep__put_f64s(w, s.th, 6 * YAEP__NTHETA, "fit_step state");
    if (yaep__fpcg_desc(&g->desc)) {
        yaep__put_i32(w, g->fit_th_n, "fit_th_n");
        yaep__put_f64s(w, s.stv + (size_t)YAEP__STV * ld, g->fit_th_n, "fit theta");
        yaep__put(w, g->stash_ok ? 1u : 0u, 1, "stash_ok");
        if (g->stash_ok) {
            yaep__put_f64(w, g->stash_val, "stash");
            yaep__put_f64(w, g->stash_lm, "stash");
            yaep__put_hyper(w, &g->stash_hyper, nd, "stash");
            yaep__put_tril(w, s.stL, n, ld, "stash");
            for (int v = 0; v < YAEP__STV; v++)
                yaep__put_f64s(w, s.stv + (size_t)v * ld, n, "stash");
        }
    }
}

static void yaep__put_all(yaep__w* w, const yaep_gp* g) {
    yaep__put(w, 'P', 1, "magic");
    yaep__put(w, 'S', 1, "magic");
    yaep__put(w, 'G', 1, "magic");
    yaep__put(w, 'P', 1, "magic");
    yaep__put(w, YAEP__SNAP_FORMAT, 4, "format");
    yaep__put(w, (uint64_t)sizeof(yaep_real), 1, "YAEP_REAL");
    yaep__put_desc(w, &g->desc);
    yaep__put_state(w, g);
}

YAEP_API size_t yaep_save_size(const yaep_gp* g) {
    yaep__w w;
    if (!g || !g->open) return 0;
    memset(&w, 0, sizeof(w));
    yaep__put_all(&w, g);
    return w.pos;
}

YAEP_API int yaep_save(const yaep_gp* g, void* buf, size_t cap) {
    yaep__w w;
    size_t need;
    if (!g || !g->open) return YAEP_ERR_CLOSED;
    need = yaep_save_size(g);
    if (!buf || cap < need || need > 0x7fffffff) return YAEP_ERR_ARG;
    memset(&w, 0, sizeof(w));
    w.out = (unsigned char*)buf;
    w.cap = cap;
    yaep__put_all(&w, g);
    return (int)w.pos;
}

typedef struct yaep__r {
    const unsigned char* in;
    size_t               pos;
    size_t               len;
    bool                 bad;
} yaep__r;

static uint64_t yaep__get(yaep__r* r, int nbytes) {
    uint64_t v = 0;
    if (r->bad || r->len - r->pos < (size_t)nbytes) {
        r->bad = true;
        return 0;
    }
    for (int i = 0; i < nbytes; i++) v |= (uint64_t)r->in[r->pos + (size_t)i] << (8 * i);
    r->pos += (size_t)nbytes;
    return v;
}

static int yaep__get_i32(yaep__r* r) { return (int)(int32_t)(uint32_t)yaep__get(r, 4); }

static double yaep__get_f64(yaep__r* r) {
    uint64_t u = yaep__get(r, 8);
    double v;
    memcpy(&v, &u, sizeof(v));
    return v;
}

static void yaep__get_f64s(yaep__r* r, double* v, int n) {
    for (int i = 0; i < n; i++) v[i] = yaep__get_f64(r);
}

static yaep_real yaep__get_real(yaep__r* r) {
    yaep_real v;
    switch (sizeof(yaep_real)) {
        case sizeof(double): {
            uint64_t u = yaep__get(r, 8);
            memcpy(&v, &u, sizeof(v));
            break;
        }
        default: {
            uint32_t u = (uint32_t)yaep__get(r, 4);
            memcpy(&v, &u, sizeof(v));
            break;
        }
    }
    return v;
}

/* The lower triangle back, and the upper one mirrored from it for a
 * symmetric matrix: yaep__kmat_row writes both halves from one value, so
 * the mirror is the matrix it wrote. */
static void yaep__get_tril(yaep__r* r, yaep_real* A, int n, int ld, bool sym) {
    for (int i = 0; i < n; i++)
        for (int j = 0; j <= i; j++) {
            yaep_real v = yaep__get_real(r);
            A[(size_t)i * ld + j] = v;
            if (sym) A[(size_t)j * ld + i] = v;
        }
}

static void yaep__get_hyper(yaep__r* r, yaep_hyper* h, int nd) {
    yaep__get_f64s(r, h->lengthscale, nd);
    h->outputscale = yaep__get_f64(r);
    h->mean = yaep__get_f64(r);
    yaep__get_f64s(r, h->lengthscale_b, nd);
    h->outputscale_b = yaep__get_f64(r);
    yaep__get_f64s(r, h->cutpoint, YAEP_MAX_OUTCOMES - 1);
    h->noise_sd = yaep__get_f64(r);
    yaep__get_f64s(r, h->lengthscale_g, nd);
    h->outputscale_g = yaep__get_f64(r);
    h->mean_g = yaep__get_f64(r);
}

static bool yaep__load_fail(yaep_gp* g, const char* msg, const char* field) {
    char buf[256];
    if (field) yaep__err(buf, sizeof(buf), msg, field);
    else yaep__err(buf, sizeof(buf), "%s", msg);
    yaep_close(g);
    memcpy(g->error, buf, sizeof(buf));
    return false;
}

YAEP_API bool yaep_open_saved(yaep_gp* g, const yaep_desc* desc, const void* buf, size_t len) {
    yaep__w w;
    yaep__r r;
    yaep__scr s;
    const unsigned char* in = (const unsigned char*)buf;
    int nd, n, ld, K, M;
    bool pair, ps;
    if (!g) return false;
    if (!yaep_open(g, desc)) return false;
    if (!in)
        return yaep__load_fail(g, "yaep_open_saved: null snapshot", NULL);
    if (len < 9 || in[0] != 'P' || in[1] != 'S' || in[2] != 'G' || in[3] != 'P')
        return yaep__load_fail(g, "yaep_open_saved: not a ysp_aep snapshot", NULL);
    memset(&r, 0, sizeof(r));
    r.in = in;
    r.len = len;
    r.pos = 4;
    if ((uint32_t)yaep__get(&r, 4) != YAEP__SNAP_FORMAT)
        return yaep__load_fail(g, "yaep_open_saved: the snapshot's format is not this "
                                "header's", NULL);
    if (yaep__get(&r, 1) != (uint64_t)sizeof(yaep_real))
        return yaep__load_fail(g, "yaep_open_saved: the snapshot was written by a build "
                                "with another YAEP_REAL", NULL);
    memset(&w, 0, sizeof(w));
    w.cmp = in;
    w.cap = len;
    w.pos = r.pos;
    yaep__put_desc(&w, &g->desc);
    if (w.diff)
        return yaep__load_fail(g, "yaep_open_saved: desc.%s does not match the snapshot",
                                w.diff);
    r.pos = w.pos;

    nd = g->desc.n_dims; ld = g->N_max; K = g->K; M = g->M;
    pair = g->desc.lik == YAEP_LIK_PAIRWISE;
    ps = yaep__is_ps(g);
    yaep__scr_of(g, &s);
    n = yaep__get_i32(&r);
    g->N_fit = yaep__get_i32(&r);
    g->halton_index = yaep__get_i32(&r);
    g->proposed = yaep__get_i32(&r);
    g->last_index = yaep__get_i32(&r);
    g->repeats = yaep__get_i32(&r);
    g->fit_state = yaep__get_i32(&r);
    g->fit_evals = yaep__get_i32(&r);
    g->fit_back = yaep__get_i32(&r);
    g->stop = (yaep_stop)yaep__get_i32(&r);
    g->fit_valid = yaep__get(&r, 1) != 0;
    g->multi_cross = yaep__get(&r, 1) != 0;
    g->fit_blocked = yaep__get(&r, 1) != 0;
    if (r.bad || n < 0 || n > ld || g->N_fit < 0 || g->N_fit > n ||
        g->halton_index < 0 || g->proposed < -1 || g->proposed >= M ||
        g->last_index < -1 || g->last_index >= M || g->repeats < 0 ||
        g->fit_state < 0 || g->fit_state > 2 || g->fit_evals < 0 || g->fit_back < 0 ||
        (int)g->stop < (int)YAEP_STOP_NONE || (int)g->stop > (int)YAEP_STOP_FULL)
        return yaep__load_fail(g, "yaep_open_saved: the snapshot's counters are corrupt "
                                "or truncated", NULL);
    g->N = n;
    g->fit_step = yaep__get_f64(&r);
    g->fit_best = yaep__get_f64(&r);
    g->log_marginal = yaep__get_f64(&r);
    g->fit_flip = yaep__get_f64(&r);
    g->opt_best = yaep__get_f64(&r);
    g->ps_tol = yaep__get_f64(&r);
    g->fit_delta = yaep__get_f64(&r);
    g->fit_conv = yaep__get_i32(&r);
    yaep__get_hyper(&r, &g->hyper, nd);
    for (int i = 0; i <= n && i < ld; i++) {
        yaep_trial* t = &g->history[i];
        yaep__get_f64s(&r, t->x, nd);
        t->y = yaep__get_f64(&r);
        t->proposed = (uint8_t)yaep__get(&r, 1);
        t->init = (uint8_t)yaep__get(&r, 1);
        if (pair) yaep__get_f64s(&r, t->x2, nd);
    }
    yaep__get_f64s(&r, g->X, n * nd);
    if (pair) yaep__get_f64s(&r, g->X2, n * nd);
    yaep__get_f64s(&r, g->y, n);
    if (!g->desc.candidates) yaep__get_f64s(&r, g->cand, M * nd);
    for (int k = 0; k < K; k++) {
        yaep__get_f64s(&r, g->f + (size_t)k * ld, n);
        yaep__get_f64s(&r, g->W + (size_t)k * ld, n);
        yaep__get_f64s(&r, g->grad + (size_t)k * ld, n);
        yaep__get_f64s(&r, s.alpha + (size_t)k * ld, n);
        yaep__get_f64s(&r, s.sW + (size_t)k * ld, n);
        yaep__get_tril(&r, g->L + (size_t)k * ld * ld, n, ld, false);
    }
    yaep__get_tril(&r, g->Kmat, n, ld, true);
    if (ps) {
        yaep__get_tril(&r, g->Kg, n, ld, true);
        for (int v = 0; v < YAEP__PS_VEC; v++)
            yaep__get_f64s(&r, s.ps + (size_t)v * ld, n);
    }
    if (g->desc.lik == YAEP_LIK_CATEGORICAL) yaep__get_tril(&r, s.Echol, n, ld, false);
    yaep__get_f64s(&r, s.th, 6 * YAEP__NTHETA);
    if (yaep__fpcg_desc(&g->desc)) {
        g->fit_th_n = yaep__get_i32(&r);
        if (g->fit_th_n < 0 || g->fit_th_n > YAEP__NTHETA) r.bad = true;
        else yaep__get_f64s(&r, s.stv + (size_t)YAEP__STV * ld, g->fit_th_n);
        g->stash_ok = yaep__get(&r, 1) != 0;
        if (g->stash_ok && !r.bad) {
            g->stash_val = yaep__get_f64(&r);
            g->stash_lm = yaep__get_f64(&r);
            yaep__get_hyper(&r, &g->stash_hyper, nd);
            yaep__get_tril(&r, s.stL, n, ld, false);
            for (int v = 0; v < YAEP__STV; v++)
                yaep__get_f64s(&r, s.stv + (size_t)v * ld, n);
        }
    }
    if (r.bad)
        return yaep__load_fail(g, "yaep_open_saved: the snapshot is corrupt or truncated",
                                NULL);
    if (r.pos != r.len)
        return yaep__load_fail(g, "yaep_open_saved: bytes after the snapshot's end", NULL);
    g->cand_valid = false;
    g->error[0] = '\0';
    return true;
}

/* --- public: simulation ------------------------------------------------- */

YAEP_API int yaep_simulate_outcome(const double* p, int K, double u) {
    double acc = 0.0;
    if (!p || K < 1) return YAEP_ERR_ARG;
    for (int k = 0; k < K; k++) {
        acc += p[k];
        if (u < acc) return k;
    }
    return K - 1;
}

/* ======================================================================= *
 *  ASYNC
 *
 *  A yrt_pump with this header in its on_msg. Everything here is plumbing:
 *  the inference is the same yaep_update(), yaep_next() and
 *  yaep_threshold() a synchronous caller runs, on the same handle, in the
 *  same order, so an async run and a synchronous replay of the same responses
 *  agree bit for bit. What the layer adds is that the frame loop does not wait
 *  for it, and that the hyperparameter fit gets the gaps between trials.
 * ======================================================================= */
#ifdef YAEP_ASYNC

/* Compute the summaries first, take the publish lock only to copy them in.
 * That is ysp/rt.h's PUMP discipline: a caller may hold that lock as long as
 * it likes without stalling the inference, because the inference never holds
 * it. */
static void yaep__async_publish(yaep_async* a, uint32_t seq, int update_rc) {
    yaep_snapshot s;
    yaep_gp* g = a->gp;
    const double* ctx = g->desc.n_dims > 1 ? a->context : NULL;
    memset(&s, 0, sizeof(s));
    s.seq = seq;
    s.update_rc = update_rc;
    s.proposed = yaep_next(g, s.x);
    s.n_trials = yaep_n_trials(g);
    s.done = yaep_done(g);
    s.stop = yaep_stop_reason(g);
    s.threshold_rc = yaep_threshold(g, ctx, a->target, &s.threshold,
                                     &s.threshold_lo, &s.threshold_hi);
    s.multi_cross = yaep_threshold_multi_cross(g);
    (void)yaep_get_hyper(g, &s.hyper);
    s.log_marginal = yaep_log_marginal(g);
    s.numeric = a->numeric;
    s.fitting = a->fit_in_idle && a->fit_active;
    yrt_pump_lock(&a->pump);
    a->snap = s;
    a->published = true;
    yrt_pump_unlock(&a->pump);
}

/* One response, on the pump thread, in submit order. */
static void yaep__async_on_msg(void* ctx, const void* msg, uint32_t seq) {
    yaep_async* a = (yaep_async*)ctx;
    const yaep_async_msg* m = (const yaep_async_msg*)msg;
    int rc = m->real ? yaep_update_real(a->gp, m->x, m->y)
                     : yaep_update(a->gp, m->x, (int)(m->y + 0.5));
    if (rc == YAEP_ERR_NUMERIC) a->numeric++;
    /* A new trial is new information, so the fit has something to do again.
     * yaep_update() has already thrown away the stepping state for the same
     * reason. */
    if (a->fit_in_idle) a->fit_active = true;
    yaep__async_publish(a, seq, rc);
}

/* One step of the hyperparameter fit, on the pump thread, whenever the queue
 * is empty. The fit's own hyperparameters and log marginal are republished
 * (the seq and the proposal are NOT: the proposal is recomputed when the next
 * response lands, which is the only moment the seq contract talks about). */
static bool yaep__async_on_idle(void* ctx) {
    yaep_async* a = (yaep_async*)ctx;
    int rc;
    if (!a->fit_active) return false;
    rc = yaep_fit_step(a->gp);
    if (rc <= 0) a->fit_active = false;
    yrt_pump_lock(&a->pump);
    (void)yaep_get_hyper(a->gp, &a->snap.hyper);
    a->snap.log_marginal = yaep_log_marginal(a->gp);
    a->snap.fitting = a->fit_active;
    yrt_pump_unlock(&a->pump);
    return a->fit_active;
}

/* ysp/rt.h's codes in this header's vocabulary. */
static int yaep__async_rc(int rt_rc) {
    switch (rt_rc) {
    case YRT_ERR_FULL:    return YAEP_ERR_BUSY;
    case YRT_ERR_TIMEOUT: return YAEP_ERR_TIMEOUT;
    case YRT_ERR_STOPPED: return YAEP_ERR_CLOSED;
    case YRT_ERR_ARG:     return YAEP_ERR_ARG;
    default:                return (rt_rc < 0) ? YAEP_ERR_ARG : rt_rc;
    }
}

YAEP_API bool yaep_async_open(yaep_async* a, const yaep_async_desc* desc) {
    yrt_pump_desc pd;
    if (!a) return false;
    memset(a->error, 0, sizeof(a->error));
    a->running = false;
    a->published = false;
    a->numeric = 0;
    a->fit_active = false;
    if (!desc || !desc->gp) {
        snprintf(a->error, sizeof(a->error),
                 "ysp_aep: yaep_async_open needs desc.gp");
        return false;
    }
    if (!yaep_is_open(desc->gp)) {
        snprintf(a->error, sizeof(a->error), "ysp_aep: desc.gp is not open");
        return false;
    }
    {   /* The context has to be inside the box, or every snapshot would carry
         * the same YAEP_ERR_ARG and nobody would look at it twice. */
        const yaep_desc* d = &desc->gp->desc;
        for (int i = 0, k = 0; i < d->n_dims; i++) {
            if (i == d->intensity_dim) continue;
            if (!(desc->context[k] >= d->lo[i] && desc->context[k] <= d->hi[i])) {
                snprintf(a->error, sizeof(a->error),
                         "ysp_aep: desc.context[%d] = %g is outside [%g, %g]",
                         k, desc->context[k], d->lo[i], d->hi[i]);
                return false;
            }
            k++;
        }
    }
    a->gp = desc->gp;
    a->target = desc->target;
    a->fit_in_idle = desc->fit_in_idle;
    memcpy(a->context, desc->context, sizeof(a->context));
    memset(&a->snap, 0, sizeof(a->snap));
    /* fit_in_idle and fit_every are two answers to the same question, and
     * running both would fit twice. The caller's value is parked here and put
     * back by stop(). */
    a->fit_every = a->gp->desc.fit_every;
    if (a->fit_in_idle) a->gp->desc.fit_every = 0;

    /* The first proposal, before the thread exists, so a poll() right after a
     * successful start always has something to return and the thread can never
     * publish seq 1 ahead of it. The pump's lock is a no-op while the pump is
     * not running, which is what makes this reuse of the publish path safe. */
    yaep__async_publish(a, 0, YAEP_OK);
    if (a->fit_in_idle) a->fit_active = true;

    memset(&pd, 0, sizeof(pd));
    pd.msg_size = sizeof(yaep_async_msg);
    pd.capacity = (uint32_t)YAEP_ASYNC_QUEUE;
    pd.ring = a->ring;
    pd.on_msg = yaep__async_on_msg;
    pd.on_idle = desc->fit_in_idle ? yaep__async_on_idle : NULL;
    pd.ctx = a;
    pd.below_normal = desc->below_normal;
    pd.pin_cpu = desc->pin_cpu;
    if (!yrt_pump_open(&a->pump, &pd)) {
        snprintf(a->error, sizeof(a->error), "ysp_aep: %.200s",
                 yrt_pump_error(&a->pump));
        a->gp->desc.fit_every = a->fit_every;
        a->published = false;
        return false;
    }
    a->running = true;
    return true;
}

YAEP_API void yaep_async_close(yaep_async* a) {
    if (!a) return;
    yrt_pump_close(&a->pump);   /* drains, then joins */
    if (a->running && a->gp && a->fit_in_idle) a->gp->desc.fit_every = a->fit_every;
    a->running = false;
}

static int yaep__async_send(yaep_async* a, const yaep_async_msg* m) {
    int rc = yrt_pump_submit(&a->pump, m);
    return (rc < 0) ? yaep__async_rc(rc) : rc;
}

/* The checks the synchronous yaep_update() would make, made here instead:
 * on the thread the only thing to do with a bad argument is to drop it
 * silently. Nothing read here changes after yaep_open(). */
static int yaep__async_fill(yaep_async* a, yaep_async_msg* m,
                             const double* x, double y, int real) {
    const yaep_desc* d;
    if (!a || !a->gp || !x) return YAEP_ERR_ARG;
    if (!a->running) return YAEP_ERR_CLOSED;
    d = &a->gp->desc;
    if (real != ((d->lik == YAEP_LIK_GAUSSIAN) ? 1 : 0)) return YAEP_ERR_ARG;
    if (!(y == y)) return YAEP_ERR_ARG;
    for (int i = 0; i < YAEP_MAX_DIMS; i++) m->x[i] = 0.0;
    for (int i = 0; i < d->n_dims; i++) {
        double tol = 1e-9 * (d->hi[i] - d->lo[i]);
        if (!(x[i] >= d->lo[i] - tol && x[i] <= d->hi[i] + tol))
            return YAEP_ERR_ARG;
        m->x[i] = x[i];
    }
    m->y = y;
    m->real = real;
    return YAEP_OK;
}

YAEP_API int yaep_async_submit(yaep_async* a, const double* x, int outcome) {
    yaep_async_msg m;
    int rc, nout;
    if (!a || !a->gp) return YAEP_ERR_ARG;
    nout = a->gp->desc.lik == YAEP_LIK_BERNOULLI ? 2 : a->gp->desc.n_outcomes;
    if (outcome < 0 || outcome >= nout) return YAEP_ERR_ARG;
    rc = yaep__async_fill(a, &m, x, (double)outcome, 0);
    if (rc != YAEP_OK) return rc;
    return yaep__async_send(a, &m);
}

YAEP_API int yaep_async_submit_real(yaep_async* a, const double* x, double y) {
    yaep_async_msg m;
    int rc = yaep__async_fill(a, &m, x, y, 1);
    if (rc != YAEP_OK) return rc;
    return yaep__async_send(a, &m);
}

YAEP_API int yaep_async_poll(const yaep_async* a, yaep_snapshot* out) {
    yaep_async* m = (yaep_async*)a;   /* the lock and the copy are not const */
    int seq;
    if (!a) return YAEP_ERR_ARG;
    if (!a->running && !a->published) return YAEP_ERR_CLOSED;
    yrt_pump_lock(&m->pump);
    seq = (int)m->snap.seq;
    if (out) *out = m->snap;
    yrt_pump_unlock(&m->pump);
    return seq;
}

YAEP_API int yaep_async_wait(yaep_async* a, uint32_t seq, int64_t timeout_ns,
                               yaep_snapshot* out) {
    int rc;
    if (!a) return YAEP_ERR_ARG;
    /* Not gated on a->running: a seq the thread DID reach before a stop still
     * answers, which is ysp/rt.h's rule for yrt_pump_wait and the reason a
     * caller can wait on the last response it submitted after stopping. */
    rc = yrt_pump_wait(&a->pump, seq, timeout_ns);
    if (rc < 0) return yaep__async_rc(rc);
    return yaep_async_poll(a, out);
}

YAEP_API int yaep_async_pending(const yaep_async* a) {
    int rc;
    if (!a) return YAEP_ERR_ARG;
    if (!a->running) return YAEP_ERR_CLOSED;
    rc = yrt_pump_pending(&a->pump);
    return (rc < 0) ? yaep__async_rc(rc) : rc;
}

YAEP_API const char* yaep_async_error(const yaep_async* a) {
    return a ? a->error : "";
}

YAEP_API bool yaep_async_is_running(const yaep_async* a) {
    return a && a->running && yrt_pump_is_running(&a->pump);
}

YAEP_API yrt_policy yaep_async_policy(const yaep_async* a) {
    return a ? yrt_pump_policy(&a->pump) : YRT_POLICY_NONE;
}

#endif /* YAEP_ASYNC */

#endif /* YSP_AEP_IMPLEMENTATION_GUARD */
#endif /* YSP_AEP_IMPLEMENTATION */

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
