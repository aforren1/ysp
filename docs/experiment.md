# ysp/experiment.h

Status: v0.3.0, 2026-11-11. The header's top comment is the reference for
the calls (USAGE, THE CHECKED FORM, REFUSALS, THE HOST, CANONICAL BYTES,
BOUNDS AND MEMORY). This page records why the header exists, the format's
defaults table, the decisions, and the results.

The loader reads a definition of format version 1 or 2
([experiment_format.md](experiment_format.md), parts 2 and 3) into a
checked, fully defaulted form. It does not lower the form to ysp/trials.h,
ysp/timeline.h, ysp/gfx.h, ysp/response.h or ysp/sig.h, and it runs nothing.
The player lowers version 1; the lowering of version 2 is the next task.

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

## How-to: get a version 1 file's bytes under version 2

1. Load the definition.
2. Call `yexp_migrated(def, &a, &bytes, &n, id)`.

For a version 1 file, `bytes` are its canonical bytes with `"version": 2`
and no stimulus `visible` key (format 3.2). They load as version 2 to the
same form and give themselves back. `yexp_canonical()` still gives the
version 1 bytes, so the experiment ID of a version 1 file does not change.
D12 asks the player to record both SHA-256s. For a version 2 file the two
calls give the same bytes.

## How-to: read the version 2 parts of the form

1. `def->sigroles` are the signal roles, sorted by name: type, units, lanes.
2. `def->controls` are the top-level controls; `t->controls` a template's.
   Each control has its keys as a `yexp_prop` list, as a stimulus has, and
   its names resolved into `yexp_ref`: what the name is
   (`YEXP_REF_ROLE`, `_CONTROL`, `_STATE`, `_STIMULUS`, `_RESPONSE`,
   `_END`), its index or control, its lane, and the lanes it reads.
3. `t->uses_top[i]` is 1 when template `t` names top-level control `i`,
   directly or through another control. Its graph needs a copy of it (3.6).
4. `t->states` is NULL for a template with `events`. Each state has its
   steps and its own exits; `t->exits` are the shared exits, each with
   the indices of its states in `in`.
5. A `set` step's property, a column and a region's `center` can hold a
   value form: `YEXP_V_SIGNAL`, `YEXP_V_WHEN` (with a `YEXP_V_TEST` or a
   `YEXP_V_SIGNAL` with `by` as its test), `YEXP_V_HIT`, `YEXP_V_TIME`.
6. A node's `vars` can hold `YEXP_V_UNIFORM` and `YEXP_V_WALK2D`.
7. Use `yexp_sigrole_get()` and `yexp_control_get()` to find one by name.

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
| `sigrole()` | a signal role against the rig (version 2, format 3.5): the profile has an adapter for it, `ysig_open()` takes its type and units, its device opens | none yet: the rig profile has no adapter block (G18) |

`sigrole()` gets a `yexp_sigrole`: name, type, units, lanes,
`allow_relative` and `required`. A host that has the profile's adapter
block refuses a required role with no adapter, a relative adapter under
`position2d` without `allow_relative`, and an adapter of another type or
units, with a message the loader prefixes with the role's name. The bit
`YEXP_CHECK_SIGROLES` is set in `report.run` or `report.skipped` only when
the file has signal roles, so a version 1 file's report is as before. The
check of a `recenter` on an absolute adapter is the player's, at run time
(D19's note).

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
| `yaep_params()` | the row's name, dots included (`"hyperparams.lengthscale"`) | `n_dims`: from `lo` and `hi`; `dims` names the dimensions |
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

## Reference: the format's defaults, version 2 (D41)

`yexp_defaults(2, &n)` lists version 1's rows with part 3's changes (a
stimulus's `visible` is a step's property only, `display.units` has `cm`,
the template's `outcome` has a second row for a template with states,
default `1`) and the rows of the sections version 2 adds: `signal role`,
`control` (one `kinds` bit per kind of control, in 3.7's order), `state`,
`over`, `exit` (bit 0 a state's own, bit 1 a shared exit), `response with
signal choices`, `uniform`, `walk2d`, and the template's and the steps'
new keys (steps: bit 11 `until`, 12 `recenter`, 13 `flag`). The table is
format 3.17's: `tests/adapt/experiment_test.c` reads 3.17 from the format
page and compares every row, and requires every row that version 2 adds
to be in 3.17 or in its list of nine with a reason (a form's own key,
a step's verb, the signal response's keys "as 2.6", `over`'s `outcome`).

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
| The levels of `$outcome` | `"correct"` and `"wrong"` with a `key_answer`, `"responded"` without one (the player's value: a press is neither correct nor wrong), and `"timeout"` when the response can time out (v0.1.3). Before v0.1.3 the loader asked for `"correct"` and `"wrong"` keys that a template with no `key_answer` never reaches, and none for `"responded"` |
| A lookup by `$response` with no `choices` | Refused (v0.1.1): any key can be pressed, so the map cannot name them |
| `$level` on a leaf with both `tracks` and a table | Refused in a number key (v0.1.1): empty on a condition trial. Valid where only a track trial reaches it (v0.1.2): `method_value`, and an entry of a lookup by `$method` for a method's name, also through `vars` |

### Version 2 (v0.3.0)

| Question | Choice and why |
|---|---|
| One table or two | Two arrays that share their large blocks through macros: `yexp_defaults(1)` returns exactly the rows of v0.2.0, so a version 1 file reads as before; version 2's array is version 1's with part 3's changes and the new sections. The loader picks the array by the file's `version` |
| A version 1 file's bytes and ID | Unchanged: it loads with the version 1 table and writes `"version": 1`. 3.2's migrated bytes come from `yexp_migrated()`, so the stored ID (D12) stays the version 1 bytes' and the migrated one is new |
| Where names resolve | In the first pass: a template's names (states, flags, controls) are collected before its keys are read, so a key can name what comes later in the file. A name resolves to what it is (`yexp_ref`); each key then says what it takes (3.9's table), and the message says both |
| A top-level control's names | Only what every template has: signal roles, stimuli, other top-level controls (finding F1). Resolved once, not per template |
| What a template's graph holds | Its controls and the top-level controls it names, directly or through another control (`uses_top`). The second pass checks a top-level control's variables in the leaves of those templates only, as only they build its nodes (3.6) |
| `until` | Kept as a step (`YEXP_UNTIL`); the lowering splits the steps (3.11 rule 9). The loader checks where it may go and that its control has hits |
| A cycle of the chart | Depth first over the edges that do not wait (finding F5), linear in states and exits; 4096 chained states load in the work budget |
| The value forms | Read where 3.10 and 3.15 allow them, with their names resolved once the template's controls exist. A lookup's entry and a choice's value take none: one form per place, as the lowering builds one node per distinct form |
| Variables of version 2 | A walk is its five variables `NAME_x` to `NAME_deg`, not `NAME`; a uniform of n 1 is a number, of n above 1 an array, read only by a moving target's `phases` of the same n. Their levels are not known at load, so a lookup by one is not checked |
| Flags | Booleans that resolve in a state's steps, an exit's `if` and a column (finding F14) |
| The `outcome` default with states | `1` when the template has no `key_answer` (3.11), from the template's second `outcome` row; `"correct"` with one; written at its default it is left out |
| A written `[]` for `events` or `after` beside `states` | The default, as D2 has it, so not "with events" |
| The sigrole bit | Set only when the file has signal roles, so the reports of version 1 files and of hosts that predate it do not change |

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

### Version 2 (v0.3.0, part 3 of the format page)

Where part 3 is not exact enough to write the loader, the loader takes the
reading below and marks it in the code as "finding Fn". The lines are of
[experiment_format.md](experiment_format.md) at commit b838bca.

| Id | Section, line | Gap | Reading taken |
|---|---|---|---|
| F1 | 3.3 l.1508, 3.9 l.1807 (D25) | What a top-level control's keys may name: a state, a flag, `response` exist only in a template | Only what every template has: signal roles, stimuli, top-level controls |
| F2 | 3.10 l.1852 | The type of `above`'s level L | A number, a variable or a lookup, as 3.7 has for a control's numbers (it lowers to a detect's level) |
| F3 | 3.11 rule 9, l.1958 | `until` in a state that has exits: which part of the split takes the exits | The state's exits arm at its entry and stay armed (rule 3); `until` splits the steps only |
| F4 | 3.11 rule 9, l.1958; 3.12 l.1993 | `until` in an `if` or `unless` body, or in `after` | Refused: rule 9 places it in `events` or a state's steps |
| F5 | D20 rule 6, l.443 | Whether a no-exit state's end of steps and `over` are edges of a cycle | Every edge that is not an `on` or `after_s` exit counts ("every cycle must contain an exit that waits"); a no-exit state whose steps hold an `until` waits there |
| F6 | 3.11 l.1902; D20 rule 2, l.426; rule 9, l.1958 | An exit's `outcome` is "as 2.5's", but 2.5 refuses text and D20 and rule 9 use text (`"timeout"`, `"too_many_breaks"`) | Text, an integer or a boolean, literal, variable or lookup: the `outcome` column's value |
| F7 | 3.4 l.1534; 3.16 l.2117 | 3.4: every value that drives x, y, w or h is in `display.units`; 3.16 refuses only a cursor | A cursor's roles (`role`, `x`, `y`, `mean`) have the display's units, with or without `stimulus`; nothing else is checked |
| F8 | 3.7 l.1619 | A point's length, also for a 3-lane `of` | `[x, y]`, two elements |
| F9 | 3.16 l.2117 | No refusal for controls that read each other in a loop (`of`, `center`, `freeze_at`, `from`), which no graph opens | Refused |
| F10 | 3.10 l.1858 | `{"signal": N}` in `color` (three fields) | N gives as many lanes as the place: 3 in `color`, 1 elsewhere |
| F11 | 3.7 l.1718 | "A movement that no response names" for a top-level movement | Refused when no template's response names it |
| F12 | 3.7 l.1660; 3.16 l.2118 | 3.7: no step gives x or y of a cursor's stimulus; 3.16 lists a `set` | Any step (`set`, `to`, `then`) that gives x or y of the stimulus of a cursor or a moving target in the template's graph |
| F13 | 3.14 l.2041 | A walk's variable (`aim_x`) and a vars name of the same node | Refused as a duplicate |
| F14 | 3.12 l.1997; D20 rule 10, l.456 | Where `$FLAG` reads | A state's steps, an exit's `if`, a column; not `events` |
| F15 | 3.11 l.1892; 3.17 | 3.17 lists `over`'s `to` and not its `outcome` | Default none, as 3.11 says |

## Known gaps

| Gap | Behavior |
|---|---|
| A `set` or `to` of `visible` | Does not count as showing the response's `from`, so such a template is refused |
| `"dims": []` with `lo` of length 2 | Reads as no dims given and is accepted |
| `"buttons": {"note": "x"}` | `note` is read as a button key there, not as a note |
| `until` and `response_ends_trial` with states | Kept as written; the lowering splits the steps at `until` and adds the shared exit `{"on": "response", "to": "end"}` (3.11 rule 9, 3.13) |
| A shared exit's `in` that lists a state twice | Accepted |
| Two templates' traces of one name | Compared by rate and items (name, lane, lanes); a top-level trace is one control, so it agrees with itself |
| `display.units` `cm` without the rig's `view`, a `recenter` on an absolute adapter | The player's checks at a start (3.16, D19's note) |
| A message about a lane | Names the role or control without its lane (`'hand' gives 1 lane` for `hand.x`) |

## Results

### v0.3.0: format version 2 (2026-11-11)

Linux desktop, gcc 12.2, Release and the `sanitizers` preset (ASan and UBSan,
built and run under `setarch -R`), on the corpus in `tests/experiment/`.

| Check | Result |
|---|---|
| `tests/adapt/experiment_test.c` | 4,380 checks pass, as C and as C++17, Release and ASan with UBSan. 20 corpus files load with the test host and with none, each equal to its golden dump, its canonical bytes loading back to the same form, bytes and ID |
| Version 1 identity | The 13 version 1 golden dumps of v0.2.0 and 208 of its 209 refusal messages are unchanged; `version_newer` is now version 3 (version 2 is read now). Every version 1 file's ID is the same. 14 version 1 files (the 13 and `visible_key`, new) give migrated bytes (3.2) equal to their bytes with `"version": 2` and no `visible`, which load as version 2 and give themselves back |
| Version 2 corpus | The two worked examples of 3.18 (`v2_visuomotor` with its table `targets.pstb` in the test host and the two `"endpoint"` warnings that 3.18 expects, in its `.warn` file; `v2_bimanual`), and the valid but unusual shapes: `v2_retry` (a cycle with a waiting exit, `max_visits` and `over`, shared exits in several states, an instant chain through three states, a state with no exits in the middle, `until` and `after_s` in one state, a flag read in a later state, `requeue_outcomes`), `v2_forms` (nested `when`, a `when` in a column, `above`, top-level controls, a region on a signal, `recenter`, a frame trace, rotation and mirror by lookup, `uniform` and an events template with `until`), `v2_instant_shared`, `v2_requeue_warn` (its warning) |
| Negative corpus | 164 version 2 files, one per refusal of 3.16 and of the loader's rules, each against its message, line and column (373 in all) |
| Defaults | 484 keys of `yexp_defaults(2)` written at their default give the ID of the key left out (version 1: 462, as before); 75 keys of 3.17 read from the format page against the table; every row version 2 adds is in 3.17 but nine with a reason |
| Mutants | `tests/mutate/experiment.toml`: 266 of 266 as expected: 265 killed, 1 by the time limit (r-44, as before). 95 are new (v-01 to v-95), one per version 2 branch; 11 of version 1's anchors follow the code. `derive.toml`: 82 of 82 |
| Compile checks | C99, C11 and C++17, warnings as errors, each loading a version 2 file too; `compile_windows_macros_c` and `_cxx` pass |
| Full ctest (Linux, pack tool, layout and player, `DISPLAY` unset) | Release: 122 of 122. Sanitizers: 120 of 121; `test_player_shapes` fails 4 `--allow-below-tier` cases with exit 1 for 2: LeakSanitizer reports the `--field` name that `player/main.c:89` allocates and the early return leaks. `player/main.c` and the test are unchanged from b838bca, so the failure is not this change's |
| Player data (`player_test.py`, 44 cases, and `compare.py`, from scratch folders, against b838bca's build) | 439 CSV files and 717 files in all identical. 94 `session.json` files differ in `headers.experiment` (0.3.0) and `pack_id` only, 4 of them also in `rig_profile_sha256` (the test writes its rig profile again, with its date); every `experiment_id` is the same |
| Pack IDs | 110 version 1 packs: every entry but the manifest byte-identical (223 entries); each manifest, with `ypak 0.8.0` and `ysp/experiment.h 0.2.0` put back, is byte-identical to b838bca's, so its SHA-256, the pack ID, is too |
| Pack tool | Both worked examples build into packs (the visuomotor one with its table, warning as 3.18 says); `test_pack_tool` builds a version 2 definition and checks its entry is the canonical bytes |
| Player | A version 2 pack is refused by name, exit 1, no data: "the definition is format version 2; this player lowers version 1" |
| Fuzzing (`tests/fuzz/experiment_fuzz.c`, self-driven; seeds: every good file, the version 2 ones weighted, and the 164 version 2 refusals) | 20,002,560 inputs under ASan and UBSan (`setarch -R`, 8 processes, 21 minutes), each in a 32 MiB arena with the test host and again in a small arena with a misbehaving host: 1,806,207 loaded and round-tripped (979,919 version 1, whose migrated bytes loaded as version 2 and gave themselves back; 826,288 version 2), 18,196,353 refused, 4,292,217 runs out of memory in the small arena, no report. The target now also checks `yexp_migrated()` and reads the version 2 table's keys as words |
| Coverage of the fuzz target (gcov, `-O0`, 500,320 inputs, 45,387 loads of which 20,739 version 2) | Header: 96.99% of 4,224 lines; 97.45% of 5,092 branches executed, 86.41% taken. Every version 2 function runs, on loads that succeed as well (`yexp_migrated()` 24,648 times). Not reached in version 2's code, among others: a `when` whose test is neither a name nor a signal form, a moving target's lane that is no object, a `mean` of roles of one type but not of 2 lanes, `states` that is no array, a point inside a lookup, and the NOMEM returns |

Cost: a version 1 file takes about 1.6 KB more arena than in v0.2.0 (RDK
90,400 bytes, was 88,762; Stroop 116,336, was 115,906): the context holds
the version 2 table's defaults and the template's names. The visuomotor
example takes 137,920 bytes for 4,005 bytes of source and the bimanual one
174,832 for 5,670, measured as the smallest fixed arena that loads, no host.
No timing claim: a load is not on the frame path.

### v0.1.0 to v0.2.0

On 2026-11-09, Linux desktop, gcc 12.2, on the corpus in `tests/experiment/`.

| Check | Result |
|---|---|
| `tests/adapt/experiment_test.c` | v0.1.3: 2115 checks and 13 corpus files pass (v0.1.0: 1933); the same as C and C++17; ASan and UBSan clean under `setarch -R`. v0.2.0 (2026-11-11, the AEP keys `hyperparams.*`, `hyperparams_min.*` and `hyperparams_max.*`; [experiment_format.md](experiment_format.md), 2.11): the same 2115 checks pass with the renamed corpus file and its golden dump |
| Corpus | The four worked examples of 2.14, `examples/pack/stroop/`, a kitchen-sink file, a negative minimum RT, five `relax_*` files that v0.1.1 refused and v0.1.2 accepts, and `outcome_responded` (v0.1.3): each loads with the test host and with none, equal to its golden dump; canonical bytes load back to the same form and the same bytes and ID |
| Negative corpus | 209 files in v0.1.3 (193 in v0.1.0), one per refusal, each compared with its message and its line and column |
| A key at its default | 462 keys of the format's table: written out or left out gives the same ID |
| Table against headers | 923 checks: `tests/adapt/experiment_tables_test.c` |
| Fuzzing (`tests/fuzz/experiment_fuzz.c`, self-driven, token-level mutations of the corpus) | 20,401,998 inputs under ASan and UBSan (`setarch -R`, gcc 12.2) on the final header, each loaded twice: once in a 32 MiB arena with the test host, where any NOMEM is a failure, and once in a small arena with a misbehaving host. 1,947,618 inputs loaded and round-tripped, 18,454,380 refused, 4,143,833 runs out of memory in the small arena, no report. The first version's run found one bug, `-0` staying `-0.0` in the form; an independent review then found the type confusion on a non-string `note` and the other items that this version fixes |
| Coverage of the fuzz target (gcov, `-O0`, 500,333 inputs, 47,678 loads) | Header: 97.75% of 2574 lines; 97.64% of 2879 branches executed, 88.02% taken. Not reached: the list and nesting limits, the work limit at its default (the target lowers it), and small accessors |
| Mutants (`tests/mutate/experiment.toml`) | v0.1.3: 171 of 171 as expected (17 new for v0.1.2 and 2 for v0.1.3, all killed): 170 killed, 1 by the time limit (an endless `column()` with its bound removed) |
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
- Version 2 is read, not lowered: no ysp/sig.h graph, timeline op tables per
  state, selector channels, exit tests, traces or state rows exist yet. The
  player refuses a version 2 definition by name. That is the next task.
- No host has `sigrole()`: the rig profile has no adapter block (G18).
- G14 to G20 of the format page are open; none is needed to read the file.
