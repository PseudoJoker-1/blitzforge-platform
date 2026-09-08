# Design

## Source of truth
- Status: Active
- Last refreshed: 2026-08-14
- Primary product surfaces: Lua-owned in-game and hangar overlays built through the WotB Mod API UI Framework.
- Evidence reviewed: `mod_api/examples/lua_ui_framework/main.lua`, `mod_api/examples/lua_battle_telemetry/main.lua`, `mod_api/examples/lua_ally_tracker/main.lua`, `mod_api/docs/LUA_MODS_RU.md`, the current World of Tanks Blitz hangar screenshot, and live-client evidence under `mod_api/build/live_evidence` when present.

## Brand
- Personality: tactical, compact, confident, information-dense, and native to the Blitz client.
- Trust signals: sourced statistics, explicit data coverage, stable layout, familiar gold/steel/green/red status colors, and no fabricated values.
- Avoid: rounded web-dashboard cards, gradients unrelated to the game, neon cyberpunk styling, oversized empty areas, emoji, and metrics whose source cannot be proven.

## Product goals
- Goals: make Lua mods feel like first-party Blitz panels; keep important information readable at a glance; expose live state without covering game navigation; present unavailable telemetry honestly.
- Non-goals: pixel-copying proprietary screens, replacing the game HUD, inventing XVM metrics the current API does not publish, or adding a second UI toolkit.
- Success signals: the surface mounts only in its intended context, never covers the mod catalog, remains legible at 1280x720 through 1920x1080, and is removable without leaked controls or callbacks.

## Personas and jobs
- Primary personas: Blitz players using session tools and Lua mod developers using examples as production references.
- User jobs: review current-session performance in the hangar, understand which metrics are confirmed, reset the session deliberately, and open or close the panel without leaving the hangar.
- Key contexts of use: post-battle hangar review, queue preparation, and development/testing in a live client.

## Information architecture
- Primary navigation: a compact right-side launcher opens one owned panel; the panel has close and guarded reset actions.
- Core routes/screens: hangar launcher, expanded session dashboard, empty session state, populated history state, and data-quality explanation.
- Content hierarchy: session title and duration; summary metrics; battle history; aggregate details; telemetry coverage and reset action.

## Design principles
- Principle 1: truthful before complete. Show `—` or `N/A` with a reason when the native bridge cannot prove a value.
- Principle 2: native before novel. Reuse DAVA controls, WarHeliosCondCBold, sharp geometry, and Blitz-like spacing instead of importing web conventions.
- Tradeoffs: a compact fixed information grid is preferred over fluid card reflow because the API targets the desktop Blitz client and must remain predictable across screen replacement.

## Visual language
- Color: near-black blue panels (`#07101A` family), steel-blue rows, white primary text, grey-blue secondary text, amber/gold actions, green positive states, and red negative/destructive states.
- Typography: `~res:/Fonts/WarHeliosCondCBold.ttf`; condensed uppercase headings; 16 px minimum secondary text and 20–34 px key values.
- Spacing/layout rhythm: 8 px base rhythm, 16–20 px panel padding, 8–12 px control gaps, and dense rows sized for rapid scanning.
- Shape/radius/elevation: rectangular DAVA controls with no decorative radius; elevation comes from opaque nested panel tones and narrow accent bars.
- Motion: no required animation; state changes are immediate to minimize runtime work and avoid visual flicker.
- Imagery/iconography: text-first UI; use built-in textures only when a stable game URI is already known.

## Components
- Existing components to reuse: API-owned container, text, button, clone, runtime setters, UI-event subscriptions, active-screen reparenting, viewport query, and context gating patterns from the shipped Lua examples.
- New/changed components: right-side session launcher, five metric tiles, eight-row battle history, aggregate/coverage panel, and two-step reset button.
- Variants and states: launcher/panel, empty/populated history, known/unknown result, reset idle/confirm, mounted/unmounted, and active-screen rebound.
- Token/component ownership: colors and dimensions stay local to each Lua mod; native DAVA controls and the WotB Mod API remain the only UI runtime layer.

## Accessibility
- Target standard: best-effort desktop readability within the game client; WCAG-inspired contrast and no color-only meaning.
- Keyboard/focus behavior: all required actions remain pointer-accessible; no forced cursor capture in the hangar; labels accompany colored outcomes.
- Contrast/readability: opaque dark surfaces behind text, minimum 16 px secondary copy, high-contrast primary values, and explicit `WIN/LOSS/N/A` wording.
- Screen-reader semantics: not available through the current DAVA bridge; concise visible labels are mandatory.
- Reduced motion and sensory considerations: no looping animation or flashing state.

## Responsive behavior
- Supported breakpoints/devices: Windows desktop client from 1280x720 through 1920x1080 and larger.
- Layout adaptations: center the main panel inside viewport-safe bounds; anchor the launcher to the right; resize the owned root and recompute positions when the viewport changes.
- Touch/hover differences: no hover-only information; click targets are at least 38 px high.

## Interaction states
- Loading: mount with current in-memory totals immediately; data refresh is deferred to the next frame after events.
- Empty: explain that confirmed metrics appear after a battle and retain visible zero totals.
- Error: log API errors and keep unavailable metrics as `N/A`; never substitute a guessed value.
- Success: update cards and history after a finalized battle, with a concise result/coverage label.
- Disabled: fully destroy owned controls and unsubscribe callbacks.
- Offline/slow network, if applicable: not applicable; the mod uses local client events only.

## Content voice
- Tone: concise Russian tactical UI copy, factual and neutral.
- Terminology: `СЕССИЯ`, `БОИ`, `ПОБЕДЫ`, `ПОЛУЧЕННЫЙ УРОН`, `ВЫСТРЕЛЫ`, `ДАННЫЕ API`, and `N/A` for unproven values.
- Microcopy rules: one fact per line; do not call unknown outcomes draws; explain coverage near the affected metric.

## Implementation constraints
- Framework/styling system: Lua 5.4 sandbox and the repository's `wotb.ui`, `wotb.events`, `wotb.context`, and `wotb.players` APIs only.
- Design-token constraints: use local Lua color tables and the existing Blitz font URI; do not add dependencies or a parallel theme engine.
- Performance constraints: update UI only when dirty, cap history at eight rows, poll player snapshot at a low fixed cadence, and keep event handlers small.
- Compatibility constraints: do not change the frozen public V3 ABI; hide outside `HANGAR` and while `MOD_SCREEN`, `TEXT_INPUT`, or `RESULTS` is active; recover from active-screen replacement.
- Test/screenshot expectations: compile the exact shipped script in the mock host, deliver synthetic events that can fail assertions, prove teardown/context gating, run a clean full build, then capture the live hangar surface and inspect loader logs/crash evidence.

## Open questions
- [ ] Add a trusted native post-battle result source (winner team, battle identity, damage dealt, kills, hits) / API maintainers / required for full XVM-equivalent outcome and efficiency statistics.
