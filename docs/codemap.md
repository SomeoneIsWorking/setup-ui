# Codemap

setup-ui is one cohesive subsystem: an in-app first-run setup screen for
native game ports, rendered with RmlUi inside the consumer's own SDL3 window.
Ownership is single-level; consumers supply everything title-specific.

| Responsibility | Where | Notes |
|---|---|---|
| Required-file, staged-state, and validation policy | `include/setup_ui/setup_ui.h`, `src/session.cpp` | Owns `Config`/`FileSpec`/`Entry`, staging-directory lifetime, partial-set wording, archive replacement, and the call into the consumer's `Validator`. Never judges file content. |
| Screen presentation and player requests | `src/view.cpp` | Owns the SDL3 window, RmlUi context, embedded document, display-scale mapping, pointer/key input, and the `Browse`/`Start`/`Cancel` event queue. |
| RmlUi render backend over SDL3 | `src/rml_sdl_renderer.{h,cpp}` | `SDL_RenderGeometry`-based draw with integer handle tables. SDL3 has no upstream RmlUi renderer interface. |
| Markup, styling, and embedding | `assets/setup.rml`, `assets/setup.rcss`, `tools/embed_assets.py` | Layout, dark theme, narrow-viewport block; embedded into the library at build time. |
| Tests (shipping screen and session) | `tests/test_setup_ui.cpp` | Staging/validation, archive replacement, partial-selection wording, stale-staging pruning, geometry capture, phone-viewport density. |
| Verification helpers | `tools/frame_png.py`, `tools/verify.py` | Offscreen capture to PNG; ruff, CMake, clang-tidy, CTest. |
