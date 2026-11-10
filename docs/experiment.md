# ysp/experiment.h

Status: v0.1.2, 2026-11-09. The header's top comment is the reference for
the calls (USAGE, THE CHECKED FORM, REFUSALS, THE HOST, CANONICAL BYTES,
BOUNDS AND MEMORY). This page records why the header exists, the format's
defaults table, the decisions, and the results.

The loader reads a definition of format version 1
([experiment_format.md](experiment_format.md)) into a checked, fully
defaulted form. It does not lower the form to ysp/trials.h, ysp/timeline.h,
ysp/gfx.h or ysp/response.h, and it runs nothing. The lowering is the
next task.

## How-to: load a definition

1. Make an arena: `yjs_arena_init(&a, buf, cap)` (the caller's memory) or
   `yjs_arena_init_heap(&a, 0)`.
2. Fill a `yexp_host` with the functions you have, or give NULL.
3. Call `yexp_load(&a, text, n, &host, &def, &report)`.
4. On `YEXP_ERR_REFUSED`, print `report.diags[i]`: its line, column and
   message. `report.n_errors` and `report.n_warnings` count all
   diagnostics. `report.run` and `report.skipped` show which host checks
   ran.
5. Read `def`. Every key that has a default holds it. Use
   `yexp_prop_get()` to find a stimulus or method key.
6. Call `yjs_arena_free(&a)` to free the form.

## How-to: get the canonical bytes and the experiment ID

1. Load the definition.
2. Call `yexp_canonical(def, &a, &bytes, &n, id)`.

`bytes` is ysp/json.h's canonical form with keys at their defaults left
out, and one LF after the value. `id` is the SHA-256 of those `n` bytes.

## How-to: supply a host

Fill only the functions that you have. A NULL function skips its check, and
`report.skipped` records it.

| Function | Check | Who has it |
|---|---|---|
| `entry()` | a pack entry exists: `timeline_variables`, `font`, `texture` (and a variable's levels), `artwork`, `audio`, `video` | pack tool, player |
| `column()` | a table's columns and their types: unknown names, built-in names (2.3), a column used where its type does not fit (a string in a number key or an `if`, a number as a text) | pack tool, player |
| `level()` | a column's levels: a lookup's `map` (2.2), the entries a `texture` column names, 0 and 1 for an integer column in an `if` | pack tool, player |
| `role()` | a role against the rig (`yrig_load()`) | player |
| `param()` | a stimulus or method value against a header's parameter table (range, enum) | designer, player |
| `rules()` | `ytr_rules()` and a dry `ytr_open()` of a leaf | player, designer |
| `check_ops()` | `ytl_check_ops()` of a template at the variables' extremes | player, designer |

The header stays free of ysp/gfx.h and ysp/screen.h, so the loader can
build in a tool with no SDL. Without `column()`, a name that nothing else
defines and that has a table in scope is taken to be a column of that
table: the file may still be valid. A level of 256 bytes or more is read
again at its length. A level above 65536 bytes, more than 65536 levels, or a
level that changes between the two reads clears `YEXP_CHECK_LEVELS` from
`report.run` and sets it in `report.skipped`. The header forces a NUL at the
end of every message a host fills, and the work limit bounds `column()`.

## Reference: from a table name to a definition key

The stimulus, method and step keys come from the headers' parameter tables
as they stood when version 1 was fixed. `yexp_defaults(1, &n)` lists every
row: section, kinds, type, key, default and enum values. The test
`tests/adapt/experiment_tables_test.c` compares every row with the live
tables. A failure means that a header moved a default: the lowering must
then write the format's value into that desc field.

| Table | Rule | Exceptions |
|---|---|---|
| `ygfx_params()` | the row's name is the key, for the format kinds the row's `kinds` reach | `color_r`, `color_g`, `color_b` are the one key `color`, `[r, g, b]` |
| `ygfx_enum_params()` | the row's name; a value is a name in the row's `values` | rows without `values` (`square`, `octaves`, `linear`, ...) are integers |
| `yrdk_params()` and `yrdk_desc` | the row's name; a desc field's name | `aperture` and `edge` are `yrdk_desc`'s; `lifetime_ns` is `lifetime_s` and `frame_ns` is `frame_s`; `seed` (D8), `stream` and `mem` are no keys |
| `yst_params()` | the row's name | `n_steps` comes from the length of `steps` |
| `yqst_params()` | the row's name | `n_stim`, `n_param`: from the lengths; `stim[]`, `param[]` are the keys `stim` and `param`, arrays of axes |
| `yaep_params()` | the row's name, dots included (`"hyper.lengthscale"`) | `n_dims`: from `lo` and `hi`; `dims` names the dimensions |
| `yrsp_params()` | `duration_ns` is `duration_s`; `min_rt_ns` is `minimum_valid_rt_s`; `max_responses`, `allow_held_key`, `wait_for_key_release` by name | `persist` comes from `response_ends_trial` at lowering; the rest (`mode`, `trace_*`, `choices.*`) are not keys of version 1 |
| `ytl_op_params()` | `tween.duration_ns` is `duration_s`; `tween.delay_ns` is `delay_s`; `tween.ease` is `ease` | |
| `ydev_params()` | `pulse_ns` is `pulse_s` | the rest stay in the rig profile (devices_spec 11.1) |
| `yvid_params()` | the row's name, on the `video` kind | |

A stimulus's `size` is the layout key on `text` (required, a literal) and
the table's `size` on `artwork`.

## Reference: the format's defaults, version 1 (D2)

A key marked "none" has no default and no value when left out; a key marked
"required" is refused when left out. The stimulus and method rows are in
`yexp_defaults()`, one row per key; they are the parameter tables' defaults
at version 1, listed by the test above.

| Object | Key | Default |
|---|---|---|
| file | `format`, `version`, `trials`, `timeline` | required |
| file | `name` | `""` |
| file | `session` | `{"fields": {"participant": {"type": "integer", "required": true}}}` |
| file | `display`, `web`, `devices`, `stimuli`, `methods` | `{}` |
| session field | `type`, `required` | required |
| display | `units` | `"px"` |
| display | `background` | `[0.5, 0.5, 0.5]` |
| display | `calibration` | `"optional"` |
| display | `min_tier` | `0` (none) |
| web | `allowed` | `false` |
| web | `exclusion` | none |
| role | `direction`, `family` | required |
| role | `required` | `true` |
| role | `min_tier` | `0` (none) |
| role | `pulse_s` | `0` |
| role | `kind` | `""` |
| role | `buttons`, `map` | `{}` |
| stimulus | `kind` | required |
| stimulus | `layer` | `0` |
| method | `type` | required |
| method | `scale` | `"linear"` |
| method | `estimate` | stair `"reversals"`; quest and aep `"mean"` |
| template | `events`, `after` | `[]` |
| template | `columns` | `{}` |
| template | `outcome` | `"correct"` with a response; empty with none |
| template | `method_value` | `"$level"`; none when no leaf with tracks runs the template |
| template | `record` | the response's `from` and any stimulus with a trigger at its onset (D10) |
| template | `response` | none |
| response | `role`, `from` | required |
| response | `choices`, `key_answer`, `trigger` | none |
| response | `duration_s`, `minimum_valid_rt_s`, `max_responses` | `0` |
| response | `response_ends_trial` | `true` |
| response | `allow_held_key`, `wait_for_key_release` | `false` |
| node | `name` | required |
| node | `timeline`, `trial` | exactly one |
| node | `timeline_variables`, `if`, `unless`, `loop`, `requeue` | none |
| node | `rules`, `tracks` | `[]` |
| node | `vars`, `data` | `{}` |
| node | `interleave` | `"random"` |
| node | `track_rate` | `0` (the rule of ytr_desc: 1 with no conditions, 0 with no tracks, required with both) |
| node | `repetitions` | `1` |
| node | `record_data` | `true` |
| step | `stagger_s`, `duration_s`, `delay_s` | `0` |
| step | `ease` | `"linear"` |

## Explanation: decisions

| Question | Choice and why |
|---|---|
| Errors: first only, or many | Many. The first pass (JSON shape, keys, types, names, the steps of a template) collects all it finds, element by element; a second pass (references, scopes, lookups, the host) runs only when the first found none, so one mistake does not report twice, and it collects too. A fixed report of 32 diagnostics; the counts go on past it |
| Where the defaults live | One table in the header, `yexp_row`, one row per key and section, with the default as JSON text. The header reads no other header at load (D2). `yexp_defaults()` gives it to a designer |
| Which keys the loader knows | The table's. A key that no row of the section has is refused. A stimulus key against a header's table goes through `param()`: the loader's table lists names and defaults only, so a designer with no gfx.h still gets the whole check of the file |
| D2's open point: two texts of one double | One ID. A real goes through its double (`yjs_double()`, then `yjs_new_double()`), so `0.1` and `0.1000000000000000055` are one value, as the player reads them. An integer within int64 stays exact (a seed, a code). A number that `yjs_double()` cannot read (more than 19 significant digits, or out of range) is refused at load, naming the key, because its double is not defined |
| One value, one form, one ID | A number whose exact decimal value is an integer in int64 range is that integer, whatever its spelling. Other reals go through the double. A real whose double is whole (above 2^53) is that integer too, because the canonical bytes hold it so. `"on": "s"` and `"on": ["s"]` write as `"on": "s"`; a choice of only a key writes as the bare string |
| Negative values | Refused for every duration key and trigger code, literal or through a known `vars` value, except `minimum_valid_rt_s`: ysp/response.h 0.8.0 makes a negative value valid (docs/response.md) |
| A variable's type | Checked where it is known: a literal or `choice` in `vars`, a session field, a jitter, a built-in, a rule variable, and a table column through `column()`. A table holds a boolean as an integer column of 0 and 1 only; a string column in an `if` is refused. An integer session field is accepted in `if` and `unless` (node and step): the player checks at session start that its value is 0 or 1. A rule variable's values are text; numeric text reads as a number |
| `vars` that name each other | A real cycle search over the chain; a chain that ends is valid up to 64 |
| A stagger on one stimulus | Valid, and left out of the canonical bytes, because it has no effect there |
| Rules text | One statement per array element (D5): a line break inside an element is refused, or `["a", "b"]` and `["a\nb"]` would be two spellings with two IDs |
| One cause, one error | An element that fails the first pass keeps its name, so a reference to it is not reported as dangling. A set refused whole makes its names unknown, and they are not reported either |
| Duplicate names in a list | Linear (a stamp per name, or a sort), so a valid definition at the stated limits stays inside the work budget. A leaf costs what its own template names |
| A response's `from` | Must be shown by a step in `events`. The response is armed at the first frame and timed from that onset, so a stimulus shown only in `after` is refused as well |
| What a step can set | Float channels only: gfx's float fields and rdk's three update floats. `layer`, integers, `density`, `lead` and `db` are read once |
| A key at its default in the canonical bytes | Left out, including a set that becomes empty (`display: {}`) and `record` when it equals its computed default. The form is the same whether the key is written or left out, so the round trip holds and a written default gets the same checks as a left-out one |
| `note` in a named set | A string-valued `note` in `stimuli`, `trials`, `methods`, `devices`, `vars`, `columns`, `data` and `session.fields` is the set's note, as D1 says for any object. A member called `note` whose value is not a string is an ordinary member, so `"stimuli": {"note": {"kind": "shape"}}` is valid. In a lookup's `map`, `note` is an ordinary level key |
| Roles: `required` by default | `true`. Refuse a session with a required role unbound rather than run without its data. devices_spec 11.1 has one role of each spelling |
| `name` on a node | Required. A node's path names it in data and seeds (D8) |
| `session.fields` without `participant` | Refused, because a leaf's rules take the participant from it (2.4) |
| A container's `rules`, `tracks`, `interleave`, `track_rate`, `requeue` | Refused: only a leaf opens a `ytr_trials` (2.4), and a container's rows run in table order |
| `$contrast_shown` with two stimuli with `{"michelson": ...}` shown in one trial | Refused: the variable has one value |
| A variable's `phase` | Session fields at session start; a container's table columns at a child node's start; a leaf's columns, `vars`, jitters and built-ins at trial start; `response`, `rt_s`, `responded`, `correct` and `outcome` in `after`; `estimate` only in `columns` (2.3: after the update, which is after `after`) |
| Step `set` and `to` | Numeric properties only: a timeline channel carries a float; a resource, a layout key or an enum is fixed at load |
| `$response` in `response.trigger.code` | Known there, in a lookup: a response rule's code is per choice (G8). A bare `$response` is refused: it is text, and a code is a number (v0.1.1) |
| A leaf with `tracks` on a template with no `response` or no `key_answer` | Refused (v0.1.1): a method hears only through a response and needs its correctness |
| A template's `outcome` | `"correct"`, an integer or a boolean, directly or through a variable or lookup (`""` is the written default with no response). Text and a non-integer number are refused (v0.1.1): `ytr_update()` takes an integer |
| `$rt_s` in a number or duration key | Refused (v0.1.1) where a trial with no press reads it: it has no value on a timeout. Valid (v0.1.2) where only a press reaches it: `outcome`, `method_value`, the steps of `{"if": "$responded"}` or `{"if": "$correct"}`, and a lookup's entry that only a press selects (`"true"` of `$responded` or `$correct`, `"correct"` or `"wrong"` of `$outcome`, a key of `$response`). Valid when the response has no deadline (`duration_s` 0). Valid in `columns`, where the cell is empty |
| The empty string as a level | A lookup by `$response` needs a `""` key when the response can time out, and a lookup by `$outcome` a `"timeout"` key. A lookup by `$method` needs a `""` key on a leaf with both `tracks` and a table (v0.1.1). Where only a press reaches the lookup (the places in the row above) `$response` has no `""` level, `$outcome` no `"timeout"` and `$responded` no `"false"`; where only a track trial reaches it (`method_value`, an entry of `$method` for a method's name) `$method` has no `""` level (v0.1.2). A response's `trigger` code needs none: it is sent only for a press. The player evaluates a lookup's selected entry only, so an entry that cannot be selected never runs |
| A lookup by `$response` with no `choices` | Refused (v0.1.1): any key can be pressed, so the map cannot name them |
| `$level` on a leaf with both `tracks` and a table | Refused in a number key (v0.1.1): empty on a condition trial. Valid where only a track trial reaches it (v0.1.2): `method_value`, and an entry of a lookup by `$method` for a method's name, also through `vars` |

## Explanation: gaps found in the format

These are the places where the format is silent or ambiguous. The list
gives the choice the loader made; a later version of the format can
decide otherwise.

| Section | Gap | Choice here | Proposal |
|---|---|---|---|
| D1 | `note` on a named set: a member, or a note? | a string `note` is the set's note | say so in D1 |
| 2.1 | `session.fields` without `participant`; field `type` and `required` have no default | refused; both keys required | list them |
| 2.2 | a lookup in a duration key | allowed, each map value a duration | say so |
| 2.4, 2.5 | a container with no leaf, a template or stimulus that no node reaches | a named warning: its variables were not checked | say so |
| 2.2, 2.3 | `$contrast_shown` with two michelson stimuli, also inside a lookup | refused | a per-stimulus variable if a task needs two |
| 2.1 | `participant` as a string or `required: false`: `rules` needs an integer | accepted; the lowering must refuse a string for `@participant` | require integer and required |
| 2.2 | a gamut on itself or in a cycle of gamuts | accepted (a gamut is on the color path, not on the value); a gamut on an enum or a text is refused | say so |
| 2.3 | an outer `vars` shadows a closer table column (the pack tool's order: vars, then tables) | as the pack tool | say which is nearest |
| 2.4, 2.5 | an empty `timeline`, `{"on": []}` | accepted: they do nothing | say so, or refuse |
| 2.3 | `$estimate` is "after the update", and `after` runs before the update (1.2) | known only in `columns` | say so |
| 2.3 | whether a `vars` value is known before trial start | no: trial start, as the table says, so a node's `if` cannot read it | say so |
| 2.4 | `name` not marked required | required | mark it |
| 2.4 | `rules`, `tracks`, ... on a container, and a container's row order | refused; table order | say which keys are leaf-only |
| 2.5, 2.6 | no default for `interleave`, `track_rate`, `repetitions`, `record_data`, `ease`, `stagger_s`, a step's `duration_s` and `delay_s`, a role's `required`, `min_tier`, `kind`, `estimate` per method | the table above | adopt it |
| 2.5 | `{"mark": NAME}`: is "end" reserved (2.5 has an implicit `end` MARK)? no name rule | any non-empty string | reserve `end` |
| 2.6 | which response keys are required | `role`, `from` | say so |
| 2.7 | "Each is a literal, not a variable": but `text` takes a variable in the examples | literal for all but `text` | say "all but `text`" |
| 2.7 | on an `rdk`, `edge` and `aperture` name both yrdk's and gfx's field | yrdk's `edge`; one `aperture` for both | `dot_edge` for gfx's |
| 2.7 | `sound` and `video`: no parameter table | `audio` plus `db`, `channels`, `offset`, `frames`, `loops`; `video` plus yvid's rows and the image's | list them |
| 2.7 | `visible` is a table row, but `on` and `off` drive it | key kept with the table's default | drop the key |
| 2.8 | a quest axis cannot express `param[].prior`, `nuisance` or `stim[].values` as a grid of points | refused | an object axis form |
| D10 | "a trigger at its onset" with steps in `if` | an `if` body counts as inline; `wait_s`, `at_s` and `then` end the onset | say so |

## Known gaps

| Gap | Behavior |
|---|---|
| A `set` or `to` of `visible` | Does not count as showing the response's `from`, so such a template is refused |
| `"dims": []` with `lo` of length 2 | Reads as no dims given and is accepted |
| `"buttons": {"note": "x"}` | `note` is read as a button key there, not as a note |

## Results

On 2026-11-09, Linux desktop, gcc 12.2, on the corpus in `tests/experiment/`.

| Check | Result |
|---|---|
| `tests/adapt/experiment_test.c` | v0.1.2: 2093 checks and 12 corpus files pass (v0.1.0: 1933); the same as C and C++17; ASan and UBSan clean under `setarch -R` |
| Corpus | The four worked examples of 2.14, `examples/pack/stroop/`, a kitchen-sink file, a negative minimum RT, and five `relax_*` files that v0.1.1 refused and v0.1.2 accepts: each loads with the test host and with none, equal to its golden dump; canonical bytes load back to the same form and the same bytes and ID |
| Negative corpus | 208 files in v0.1.2 (193 in v0.1.0), one per refusal, each compared with its message and its line and column |
| A key at its default | 462 keys of the format's table: written out or left out gives the same ID |
| Table against headers | 923 checks: `tests/adapt/experiment_tables_test.c` |
| Fuzzing (`tests/fuzz/experiment_fuzz.c`, self-driven, token-level mutations of the corpus) | 20,401,998 inputs under ASan and UBSan (`setarch -R`, gcc 12.2) on the final header, each loaded twice: once in a 32 MiB arena with the test host, where any NOMEM is a failure, and once in a small arena with a misbehaving host. 1,947,618 inputs loaded and round-tripped, 18,454,380 refused, 4,143,833 runs out of memory in the small arena, no report. The first version's run found one bug, `-0` staying `-0.0` in the form; an independent review then found the type confusion on a non-string `note` and the other items that this version fixes |
| Coverage of the fuzz target (gcov, `-O0`, 500,333 inputs, 47,678 loads) | Header: 97.75% of 2574 lines; 97.64% of 2879 branches executed, 88.02% taken. Not reached: the list and nesting limits, the work limit at its default (the target lowers it), and small accessors |
| Mutants (`tests/mutate/experiment.toml`) | v0.1.2: 169 of 169 as expected (17 new for v0.1.2, all killed): 168 killed, 1 by the time limit (an endless `column()` with its bound removed) |
| Compile checks | C99, C11 and C++17 with warnings as errors; the unit test also as C++17 under ASan and UBSan |
| Full ctest (Linux, with the pack tool and layout, `DISPLAY` unset, so no window) | 117 of 117 |
| Pack tool | `test_pack_tool`, `test_pack_import`, `test_pack_derive` and `test_layout` pass on the full tool and on the core tool; CI's `ypak verify`, `list`, `rebuild` and `trial_images --sim` lines pass; the full and the core tool give one `core.ysppak` |

Cost: arena use on the worked examples is 89 to 116 KB (88,762 bytes for
the 1,089-byte RDK file, 115,906 for the 2,739-byte Stroop file): about 60 KB
of work space and about 21 bytes per byte of source. A template costs about
530 bytes: 1024 templates in a 22,620-byte file take 587,854 bytes. The first
version took about 41 KB per template (1024 templates: 42 MB), because each
one reserved a table for the longest list; the table is now sized from the
template's own steps. `report.arena_need` is the size of the request that
failed, not the total a load needs. A definition is read once per session: a
load is not on the frame path, and no timing claim is made here because
another job was using the machine during the run.

## Not done

- The player ([player.md](player.md)) lowers the checked form and runs it
  on the simulated display. A real window and a real device are not built.
- The pack tool calls `yexp_load()` and `yexp_canonical()` from v0.8.0
  (pack.md 16). It supplies entries, columns, levels and `ytr_rules()`; the
  rig, the parameter tables and `ytl_check_ops()` are skipped at build.
- The player is the first host that calls `ytr_rules()`, a dry `ytr_open()`
  and `ytl_check_ops()` at the variables' extremes; `tests/player/` runs them
  against real tables and a real rig profile.
- No run on macOS, Windows or emcc. The unit test is not in the emcc CI
  list because it reads files; the compile check is.
