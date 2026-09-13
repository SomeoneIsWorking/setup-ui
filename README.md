# setup-ui

A responsive browser-based first-run setup UI for native game ports. A native
game that needs player-supplied files (disc images, ROMs, disk sets) starts a
local setup server instead of opening a modal dialog. The player opens the
served page in any browser on the same device or on a phone in the same
network, picks the files, and the game validates and starts.

- **Assets** own the responsive single-page setup experience. Titles supply
  wording through the config; no game names are baked in.
- **Host** owns serving, upload transfer, staged files, progress, and
  lifecycle over `lucent::http::Server`.
- **Consumers own** file validation and identity, persistence, and the
  decision to proceed. The host never guesses what a valid file is.

## Consuming

The library is CMake target `setup_ui::setup_ui` (C++20). It resolves Lucent
like the ports resolve their shared checkouts: `SETUP_UI_LUCENT_DIR` or a
sibling `../lucent` directory. The web assets are embedded at build time from
`assets/` and compiled into the library.

C and C++ consumers both use `include/setup_ui/setup_ui.h`:

```c
setup_ui::Server server; // C++ form; the C ABI wraps the same owner
```

## Verifier

`uv run --frozen python tools/verify.py` runs Python lint and tests, C++
formatting and clang-tidy against the real compile database, and the CTest
suite (embedded-asset identity, HTTP routes, upload staging, cancellation).
No game files are build inputs.
