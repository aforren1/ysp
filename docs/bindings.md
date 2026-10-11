# Bindings

This page is reference: the language bindings of the headers.

- **Python**: one distribution per library under
  [bindings/python/](../bindings/python/), each a dependency-free CPython
  extension on the Limited API (one abi3 wheel per platform for CPython
  3.8+). Each one compiles its header into the extension, plus `ysp/rt.h`
  where the header uses it. They share the `ysp` namespace (PEP 420, no `__init__.py`), so
  `pip install ysp-parallel ysp-serial` gives `import ysp.parallel` and
  `import ysp.serial`, and either installs alone. The adaptive headers have
  the same: `ysp-stair`, `ysp-quest`, `ysp-aep` and `ysp-trials` give
  `ysp.stair`, `ysp.quest`, `ysp.aep` and `ysp.trials`, with an `Async`
  class in quest and aep that runs inference on a C thread the interpreter
  never sees, and `ysp.trials` taking any of the three as a track.
  `ysp-color` gives `ysp.color` (calibration, conversions, gamut, a
  participant's luminance). `ysp-rdk` gives `ysp.rdk` (the dots of a trial
  again, bit for bit, from its seed and log); it needs NumPy at run time. `tests/compare/`
  runs them beside PsychoPy, questplus and AEPsych on one response stream.
  CI builds the wheels with cibuildwheel on Linux, Windows and macOS, except
  `ysp-parallel` on macOS, which its header does not support.
- **MATLAB / Octave**: [bindings/mex/](../bindings/mex/), one MEX function per
  library (`ysp_parallel`, `ysp_serial`, `ysp_stair`, `ysp_quest`,
  `ysp_aep`, `ysp_trials`, `ysp_color`) with ppdev-mex-style command dispatch, built and
  tested in MATLAB R2023a and Octave 10. `tests/compare/` runs `ysp_quest`
  beside mQUESTPlus and Palamedes' PAL_AMPM, and `ysp_stair` beside
  PAL_AMUD, on one response stream; the MEX README has the results.
  `tests/compare/color_ptb.m` runs `ysp_color` beside Psychtoolbox's
  colorimetric code (docs/color.md).
