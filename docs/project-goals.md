# Project goals

## G001 — One responsive setup UI for every port

Give every native game port the same first-run file-setup experience: start a
local server, let the player open a responsive page in any browser on the
device or a phone in the network, receive the files, and let the title's own
validator decide whether the set is complete and correct. Success requires
embedded assets with no per-title code, loopback-only by default with
explicit opt-in LAN sharing, staged uploads under the consumer's private
directory, and validation that always runs on the consumer's thread.

## Non-goals

- No file identity, digests, or naming policy: consumers own validation.
- No persistence or promotion: consumers stage, validate, and publish.
- No WASM in-page picker integration: the browser build's page stays its own.
