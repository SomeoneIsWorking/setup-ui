# setup-ui

An in-app first-run setup screen for native game ports. A port that needs
player-supplied files (disc images, ROMs, disk sets) shows this screen inside
its own SDL3 window — no browser, no system message box, no LAN transfer. The
screen renders through RmlUi (HTML/CSS-like markup) and asks the platform for
files through the consumer's own picker.

- **`Session`** owns what the title requires, what the player has provided,
  where the chosen bytes are staged, and when the consumer's validator runs. It
  never decides whether a file is the right one.
- **`View`** owns the SDL3 window/renderer, the RmlUi context, embedded markup
  and styling, input, and the requests the player makes (choose, start, cancel).
- **Consumers own** the platform picker, file identity and validation,
  persistence, and the decision to start.

The screen scales with the display: RmlUi's dp unit is mapped from
`SDL_GetWindowDisplayScale`, so a phone (≈3x) and a desktop (1x) render the
same layout at the same apparent size, and `assets/setup.rcss` carries a narrow
-viewport block for a phone held upright.

## Consuming

CMake target `setup_ui::setup_ui` (C++20). Resolved like the ports resolve
their shared checkouts:

- RmlUi: `SETUP_UI_RMLUI_DIR` or a sibling `../RmlUi` / `../../RmlUi` checkout
  (the port builds it static with the debugger off).
- SDL3: `find_package(SDL3)` from the consumer's own toolchain.
- Freetype: RmlUi's font engine, from the first source that provides it — a
  `Freetype::Freetype` the consumer already declared (Android takes it from the
  shared Android prefix), a Freetype CMake package on the host, or a Freetype
  checkout at `SETUP_UI_FREETYPE_DIR` / `../freetype` built here as a static
  library. A packaged product links the static build so it ships no font-engine
  runtime it would have to bundle; the browser toolchain's Freetype port is used
  through the same target.
- Fonts: resolved from the host at runtime (a system sans on desktop,
  `/system/fonts/Roboto-Regular.ttf` on Android); no font asset is bundled.

```cpp
setup_ui::Session session(config, session_options, validator);
setup_ui::View view(session, view_options);
if (!view.open())
  return view.last_error();
while (view.running()) {
  for (const setup_ui::Request &request : view.poll()) {
    if (request.kind == setup_ui::RequestKind::Browse)
      platform_picker([&](const std::vector<std::filesystem::path> &paths) {
        std::string error;
        session.add_selected(paths, error);
      });
    else if (request.kind == setup_ui::RequestKind::Start)
      session.validate_if_ready();
  }
  view.frame();
}
```

`Config.accepts_archive` lets one bounded ZIP stand in for the whole set. A
partial selection is not a failure: the session reports
`Still needed: <names>` so the screen tells the player what is missing, and
`Session::discard_stale_staging` prunes the staging directory a killed process
left behind (Android force-stop, a crash), which its destructor never removed.

`ViewOptions` also carries `density_ratio` (0 = auto from the display scale) and
`offscreen` (render into an exactly sized software surface), so a
phone-sized viewport can be captured for verification on another host.

## Verifier

`uv run --frozen python tools/verify.py` runs Python lint, the CMake build,
clang-tidy against the real compile database, and the CTest suite (staging and
validation, archive replacement, partial-selection wording, stale staging,
embedded-asset identity, and the phone-viewport density check). No game files
are build inputs.
