# Project state

## Comparison baseline

Each port implemented its own first-run setup: an Android `AlertDialog` plus a
SAF tree import, a desktop SDL message box plus a native file dialog, and in one
case a bespoke in-page picker. setup-ui replaces those with one in-app screen —
rendered with RmlUi inside the port's own window — driven by the platform's own
file picker.

## Current focus

Benefactor consumes setup-ui on Android and desktop; LF2 consumes the
one-selection shape for its Little Fighter 2 install.

## Capability inventory

| ID | Capability or outcome | State | Evidence or exact gap | Goals |
| --- | --- | --- | --- | --- |
| S001 | The screen renders a title-supplied config (heading, message, rows, hint, footer) from embedded RmlUi assets | verified | `tests/test_setup_ui.cpp` renders the embedded document offscreen and reads its geometry back | G001 |
| S002 | Density-independent layout: the same apparent size on a 1x desktop and a ~3x phone, with a narrow-viewport block | verified | `test_screen_is_responsive_on_a_phone_viewport` (2728x1264 px at density 3.0) plus desktop and portrait captures at 1x/2.25x | G001 |
| S003 | Chosen files are copied into one private child directory per attempt and the consumer's validator runs on the driving thread | verified | `tests/test_setup_ui.cpp` stages files, observes validator ordering and byte counts | G001 |
| S004 | One bounded ZIP stands in for a whole named set | verified | Archive case in `tests/test_setup_ui.cpp`; end-to-end ZIP import verified on the shared API 35 emulator | G001 |
| S005 | A partial selection is reported with the still-missing names; a wrong file or rejected archive reports its own reason | verified | `test_partial_selection_names_what_is_missing`; rejection wording covered by the rejection case | G001 |
| S006 | The staging directory is removed when an attempt ends, and a directory left by a killed process is pruned | verified | `Session` destructor/reset removal plus `test_stale_staging_is_pruned` | G001 |
| S007 | Start is available only for an accepted set | verified | `src/view.cpp` enables Start from the session status; the shared API 35 emulator run shows Start disabled for an incomplete set and the accepted ZIP set transitioning straight into the game | G001 |
| S008 | The consumer's own picker supplies the files; the screen never opens one | verified | Benefactor's Android SAF and desktop SDL dialog integration | G001 |
| S009 | Captured screen output can be inspected on any host | verified | `ViewOptions.offscreen` + `tools/frame_png.py` captures used for the phone-sized and desktop-sized views | G001 |
| S010 | A port that needs one selection it judges itself (a ROM, an original installer, an archive, or an existing install tree) is served by a single unnamed `FileSpec` | verified | `test_one_selection_is_adopted_where_the_player_keeps_it` and `test_one_selection_accepts_a_directory_and_replaces` in `tests/test_setup_ui.cpp` | G001 |
| S011 | `Placement::Adopt` hands the validator the player's own location, including a directory, without copying or applying a byte bound | verified | Both one-selection cases assert no staging directory is created and a `max_file_bytes` smaller than the selection does not reject it | G001 |

## Not applicable here

- Persistence, publication, and file identity are consumer capabilities; the
  authoritative record lives in each consuming project's `docs/project-state.md`.
