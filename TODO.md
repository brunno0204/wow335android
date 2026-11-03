# Development roadmap

This is a working backlog for the experimental Android ARM64 port, based on the v0.83 source snapshot. A feature having code does not mean it has passed device or server testing. Items below are acceptance goals, not promises or release dates.

## Priority 1 — Reproducible builds and stability

- [ ] Run the Android APK workflow on this repository and fix any build or dependency failures.
- [ ] Establish repeatable release signing without committing private keys or passwords.
- [ ] Record tested device models, Android versions, GPUs, client locales and server builds.
- [ ] Validate startup, login, realm selection, character selection and world entry on real devices.
- [ ] Improve crash reporting and make reproduction steps easy to collect without exposing credentials.
- [ ] Add meaningful regression tests for packet parsing, data loading and rendering changes.

## Priority 2 — Networking and gameplay

- [ ] Investigate remaining authentication and world disconnections using complete errors and controlled server tests.
- [ ] Document which server configurations have actually been tested; do not assume all 3.3.5a servers work.
- [ ] Validate movement, jumps, landing, teleports and map transitions against the server.
- [ ] Verify NPC and other player updates, interpolation, spawning and removal.
- [ ] Verify equipped weapon attacks, combat stances, offhand swings and equipment changes while playing.
- [ ] Validate combat feedback, spell effects, cooldowns, cast bars and auras against server state.
- [ ] Test quest acceptance, objectives, completion, rewards, loot and inventory synchronization.
- [ ] Implement and validate incomplete vendors, trainers, mail, auctions and talent interactions.

## Priority 3 — Interface and controls

- [ ] Replace remaining Lua API stubs with supported, accurate behavior where needed.
- [ ] Fix UI script errors and validate portraits, unit frames, chat, spellbook, action bars and world map.
- [ ] Improve touch hit areas and prevent conflicts between UI, movement and camera gestures.
- [ ] Test Android keyboard input, symbols, chat submission and Back/menu behavior.
- [ ] Add or validate physical gamepad support and configurable bindings.
- [ ] Improve English UI coverage; the English README currently documents Portuguese app labels.

## Priority 4 — Graphics, audio and performance

- [ ] Profile CPU/GPU frame time and memory on actual Android devices.
- [ ] Validate frustum culling, terrain streaming, WMO/doodad collision and camera obstruction.
- [ ] Verify character appearance, armor composition, attachments and animations across races and sexes.
- [ ] Complete or verify water, sky, fog, lighting, animated textures and particles.
- [ ] Validate music, ambience, gameplay/UI sounds and app background/resume behavior.
- [ ] Investigate original cinematic/video playback.
- [ ] Test EGL context recreation, long sessions and repeated world transitions.

## Contributions and reports

Start with a reproducible issue. Include the APK/source version, phone/GPU, Android version, client locale, server version and relevant logs. Distinguish implemented code, desktop tests and real-device results.

Keep fixes scoped and describe how they were validated. Do not commit game MPQs, exported game assets, accounts, tokens, signing keys or personal configuration. Warden is unsupported; this roadmap does not propose bypassing server protections.
