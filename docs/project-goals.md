# Project goals

## G001 — One responsive setup screen inside every port

Give every native game port the same first-run file-setup experience without a
system modal, a browser, or a network transfer: the port shows one screen inside
its own window, the player picks files through the platform's own picker, and
the title's validator decides whether the set is complete and correct.

Success conditions:

- The screen renders in the consumer's own SDL3 window through RmlUi with
  embedded, title-neutral assets; a consumer supplies only wording, required
  file names, and a validator.
- The layout is density-independent: the same apparent size and no clipped
  content on a 1x desktop and a ~3x phone, in the orientation the consumer
  enforces.
- Staged bytes live in a private directory the consumer names, one bounded
  child per attempt, removed when the attempt ends or a killed process left it
  behind.
- A partial selection, a wrong file, and a rejected archive each produce
  specific wording on the screen, and validation always runs on the thread that
  drives the screen.
- The screen gives the player a way to start only from an accepted set.

## Non-goals

- No file identity, digests, or naming policy: consumers own validation.
- No persistence or publication: consumers stage, validate, and publish.
- No title-specific wording, artwork, or control layout in this repository.
- No browser page, HTTP host, or LAN upload path.
