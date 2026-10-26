# ysp.rigfile Python binding

A CPython extension that wraps [ysp/rigfile.h](../../../include/ysp/rigfile.h):
the rig profile ("ysp-rig 1"), the file on a rig that says which device
plays each role, what the rig's loopbacks measured, and the viewing
geometry. It uses the Limited API (one abi3 wheel per platform for CPython
3.8+) and has no run-time dependencies. The distribution is `ysp-rigfile`,
the module `ysp.rigfile`. Its version is the header's.

The device layer is the separate distribution `ysp-device`
([its README](../ysp_device/README.md)). `Profile.desc(role)` gives the
keyword arguments of `ysp.device.Device`:

```python
import ysp.rigfile as yrig
import ysp.device as yd

prof = yrig.load()                         # %APPDATA%\ysp\rig\profile.json, ~/.config/ysp/rig/profile.json
for note in prof.notes:                    # a loopback file that is missing, changed or differs
    print("rig profile:", note)
print("profile", prof.hash)                # into the data file's header
dev = yd.Device(**prof.desc("resp"))
print(prof.source(dev))                    # tier 1 only when the role's bounds file checked
print(prof.ppd)                            # pixels per degree, 0 without a view
```

## Build and install

```sh
pip install .                              # from this directory
```

A build outside the repository (an sdist, cibuildwheel) needs copies of
`rigfile.h`, `device.h`, `json.h`, `rt.h`, `input.h`, `box.h` and
`serial.h` in `include/ysp/` next to `setup.py` (see `MANIFEST.in`).

## Reference

| Name | What it is |
|---|---|
| `load(name=None)` | The user's profile (`profile.json`, or `<name>.json` for a second rig) with every loopback file checked. Raises `Missing` when there is none, `Folder` when the rig folder is refused (another user can write it), `Error` for a bad file (the message names the field, line and column) |
| `load_file(path)`, `parse(text, dir=None)` | A profile from a path (its `loopback/` beside it) or from text (`dir` for the checks; none run without it) |
| `dir()`, `path(name=None)` | The user's rig folder, and a profile's path in it |
| `Profile(rig='rig')` | An empty profile |
| `.rig`, `.written`, `.dir`, `.notes`, `.roles` | The file's fields; `notes` lists what the checks found |
| `.role(name)` | A dict: `family`, `family_id`, `key`, `baud`, `latched`, `ftdi_latency` and `pulse` (seconds or `None`), `buttons`, `latency` and `bounds` (seconds, with `check` and `check_name`) |
| `.desc(role)` | The keyword arguments of `ysp.device.Device`: `role`, `family`, `key`, `baud`, `latched`, and `pulse` when the rig fixes it. Raises for an `lsl` role: give its key to ysp/net.h |
| `.source(role)`, `.source(device)` | The tier rule: `tier` 1 with the role's bounds (`lo`, `hi`) only when its bounds file checked and its family maps a device clock |
| `.display`, `.view`, `.ppd` | The display's onset offset and its photodiode run; the viewing geometry; pixels per degree at the center |
| `.text`, `.hash` | The canonical JSON and its SHA-256 |
| `.bind(role, family, key)`, `.remove(role)`, `.set_view(distance_mm, width_mm, width_px)`, `.clear_view()` | Changes in memory; a new binding drops the role's latency and bounds |
| `.save(name=None)`, `.save_file(path)` | Write it (the user's folder through a temporary file, or a path) |
| `check_name(check)`, `CHECK_*`, `ERR_*`, `TIER_*`, `FAMILY_LSL`, `FORMAT` | Names and constants |
| `Error`, `Missing`, `Folder` | Exceptions; `args == (message, ERR_*)` |

The loopback tools (`device_out_latency`, `rigfile_bounds`,
`rigfile_onset`, `rigfile_profile`) write the measured fields; this binding
reads them and does not store loopback files.

## Units

Every time is seconds as a `float`; the bounds have their exact ns beside
them (`lo_ns`, `hi_ns`). Lengths are mm, as in the file.

## Tests

```sh
pytest tests
```

On Linux and macOS the tests also check that a rig folder that all users
can write is refused. On Windows `yrt_user_dir()` reads no ACL, so that
check does not run there.
