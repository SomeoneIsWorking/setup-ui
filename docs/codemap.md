# Codemap

setup-ui is one cohesive subsystem: a browser-based first-run setup host for
native game ports. Ownership is single-level; consumers supply everything
title-specific.

| Responsibility | Where | Notes |
|---|---|---|
| C++ server (lifecycle, routes, staging, validation dispatch) | `include/setup_ui/setup_ui.h`, `src/setup_ui.cpp` | Owns the Lucent HTTP listener, upload framing, staging directories, poll()-thread validation, event queue. Never judges file content. |
| C ABI wrapper | `include/setup_ui/setup_ui_c.h`, `src/setup_ui_c.cpp` | Mirrors the C++ owner for C consumers; event strings stay valid until the next poll. |
| Embedded web assets (page + controller) | `assets/index.html`, `assets/setup.js`, `tools/embed_assets.py` | Responsive dark UI; config wording comes from /api/config. Generated C++ pair compiled into the library. |
| Lucent HTTP transport | external `lucent::http::Server` | Loopback default; LocalNetwork requires the pairing token in every route. |
| Tests (shipping server over loopback) | `tests/test_setup_ui.cpp` | C++ and C-ABI flows: routes, staging bytes, validation threading, rejection wording, stop lifecycle. |
| Verifier | `tools/verify.py` | Ruff format/check, CMake build, clang-tidy on the compile database, CTest. |
