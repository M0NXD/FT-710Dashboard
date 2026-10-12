# FT-710 Dashboard review and improvement plan

Reviewed 12 October 2026 against commit `c380bba` (version 2.0.4).

The implementation work below is now included in version **2.0.5**. The original review and findings remain as the record of what was fixed. The application remains one integrated Windows desktop and remote-control project.

## Implementation status — 2.0.5

| Item | Implemented result | Regression evidence |
| --- | --- | --- |
| F1 | Worker-owned release retry for CAT/dashboard PTT; new TX rejected while release is pending; obsolete client releases rejected | ReleaseChecks injects repeated CAT failures and a dashboard failure using real Hamlib Dummy |
| F2 | Independent background queues for CAT-2/capture and audio device management; responsive GUI completion messages | GuiChecks injects a slow scope operation and checks immediate dispatch |
| F3 | Native/browser RF history clears on center/span/capture-generation changes | BrowserChecks exercises grid transitions and duplicate sequence handling |
| F4 | Serialized saves with field edit revisions; only changed fields are applied; shared typed settings validation completes before mutation; INI failures are reported | BrowserChecks reproduces delayed replies/newer edits and failed saves; GuiChecks checks shared model rejection |
| F5 | Periodic tuning flushes preserve accumulated Hz per VFO, split oversized batches, flush gesture completion and cancel obsolete gestures | BrowserChecks covers uninterrupted movement, totals, batch limits and cancellation |
| F6 | RF frames include generation/sequence/acquisition time; frozen/stalled status and fresh-frame tuning guards | BrowserChecks checks stale history and rejects tuning with stale state |
| F7 | Per-browser mic registration; local disarm/permission cleanup leaves other clients' route intact; mapping changes during TX rejected | BrowserChecks and GuiChecks verify non-owner disarming and final owner release |

Phone audio now feature-detects the Audio Session API and provides an optional stable play-and-record session, interruption/device/track handling, a resume action and cached-page reconnection. Neither resuming audio nor reconnecting re-keys PTT. The reported Safari volume behavior still requires validation on the user's actual phone; no unconditional playback gain boost was added.

Performance work includes persistent incremental waterfall images, color lookup tables, frame-coalesced drawing, hidden-settings rendering avoidance, scope subscriptions, nested reading deltas and incremental activity logs. The waterfall regression verifies that a settled 850 × 200 image updates 850 pixels for one new row, versus 170,000 pixels for the initial image; unchanged frames update zero pixels. This is an operation-count regression, not an iPhone frame-rate measurement.

The native/browser interfaces use shared typed connection/scope settings and common operations for scope apply, microphone ownership and PTT release. Both interfaces expose a saved unattended-browser-audio option that retains receive audio across Windows session changes while releasing PTT. The web diagnostics show contact/frame ages, acknowledgement timing, draw timing and audio session state.

CTest includes radio, release-failure, six-tab GUI, browser, server/client delta and authenticated HTTPS/WebSocket checks. The HTTPS fixture binds only to loopback and controls no radio; it covers assets, protected state, cookies, subscriptions and passcode/session invalidation.

Build/release work includes optional FTDI staging, a Windows CI workflow, a standalone runtime-check script, and application-only ZIP packaging with an allowlist and SHA-256 checksum. README setup and dependency instructions were updated; **SCU-LAN10 = ON** remains required for real RF capture. Third-party runtime DLLs stay outside the release archive.

Remaining acceptance checks require external hardware/environment: the user's iPhone/iOS output-volume and interruption behavior, actual FT-710 power transitions/audio/RF capture, performance on that phone, and a fresh Windows installation. Local offline verification does not substitute for those checks. The new CI workflow will execute when these changes are pushed to GitHub.

### Completed local validation

- Release build succeeded; staged executable reports version **2.0.5**.
- All **six CTest suites passed**. Tests affected by final browser/layout changes were rerun and passed.
- Configure succeeded with an absent FTDI runtime directory.
- Runtime checker passed against the staged Hamlib/threading/FTDI dependencies.
- Native Audio routing screenshot was inspected; the unattended option fits in the header and leaves PTT help readable.
- Application-only archive contents were verified as exactly `FT710Dashboard.exe`, `README.md`, and `check-runtime.ps1`; a SHA-256 checksum was generated.
- `git diff --check` passed. No third-party DLLs, libraries, or archives were added to tracked files.

Local executable: `dist/FT710Dashboard.exe`.
Final archive: `artifacts/releases/final-2.0.5/FT710Dashboard-2.0.5-Windows-x64.zip`.

## Review evidence

- Rebuilt the existing `RadioChecks` and `GuiChecks` targets from the current source.
- Existing GUI suite: **1,211 assertions passed**.
- An isolated extension of the GUI suite covering all six tabs: **1,683 assertions passed**, including small/full windows and 100/125/150/200 percent font scales.
- Offline FT-710 driver fixture: **all checks passed**, including calibrated meters, RF-power writes/readback, two simultaneous NET rigctl clients, AM power limits, and clearing TX meters on RX.
- Additional isolated probes reproduced the CAT release failure and four browser problems below. Browser probes execute the current JavaScript with controlled state, timers and command responses; they do not constitute a real Safari/device test.
- Production source, the installed executable, and GitHub releases were not changed. Local test probes are under the ignored `artifacts/review-2026-10-12/` directory.
- No physical radio connection, RF transmission, live audio recording, or iPhone testing was performed during this review. Passing layout assertions does not establish visual quality on every screen.

## Bugs to fix

### F1 — P1: Failed PTT release for a disconnected CAT client is not retried

**Evidence:** Reproduced using Hamlib Dummy and a wrapper that fails exactly one PTT-off call. After `__release_client` failed, the radio worker made only one release attempt over 2.2 seconds and Dummy still reported TX. The next release attempt occurred on worker shutdown.

**Cause:** `src/rigctl_protocol.cpp:151` retains `catPttOwner_` after a failed release, but the retry/deadline machinery in `src/radio.cpp:265` only handles `heldPtt_`, which represents dashboard-owned transmission. Polling does not schedule a release for the disconnected CAT owner.

**Fix:** Keep an explicit pending-release state in the radio worker for both dashboard and CAT ownership. Retry failed releases on a bounded interval, retain ownership until release is confirmed, and report the pending failure clearly. Cancel obsolete work when the radio connection generation changes.

**Acceptance:** A transient PTT-off failure is retried and returns to RX without closing the application. Cover dashboard release, CAT disconnect, browser disconnect, connection replacement, and repeated failures. Recovery must never issue a new PTT-on command.

### F2 — P1: Slow scope operations block the GUI and browser control dispatch

**Evidence:** Confirmed by code inspection. `Dashboard::webCommand` calls `rf_scope::applySpan` synchronously at `src/dashboard_web.cpp:300`; the desktop button uses the same synchronous call at `src/dashboard.cpp:1354`. All browser requests, including PTT-off and browser-disconnect notifications, pass through the GUI message handler at `src/dashboard.cpp:1885`.

**Cause:** `src/rf_scope.cpp:245` configures 1.5-second serial read timeouts, and the operation performs writes, a sleep, and two readback queries. These waits happen on the GUI thread. Device refresh, route shutdown and capture shutdown also contain waits and should be included in the same audit.

**Fix:** Move CAT-2 operations and potentially slow device-management work onto background workers. Return progress and completion messages to the GUI. Let PTT release enter the radio worker promptly while other operations are pending. Preserve the single owner of each physical interface.

**Acceptance:** With delayed or missing CAT-2 replies, both interfaces remain responsive and PTT-off reaches the radio queue promptly. Measure application dispatch delay separately from the radio's serial response time.

### F3 — P2: RF waterfall history acquires incorrect frequency labels

**Evidence:** Reproduced by delivering a 7.1 MHz / 100 kHz scope frame followed by a 14.2 MHz / 500 kHz frame. The first row remains in history while the entire waterfall adopts the second frame's axis.

**Cause:** `web/app.js:256` stores only bin values for historical rows; the renderer uses the latest global center and span. `src/dashboard.cpp:1552` also stores only bins in native RF history.

**Fix:** Associate history with a frequency grid. Clear history when its center/span changes, or retain frame metadata and deliberately reproject compatible rows. For the initial fix, clearing is simpler and makes the displayed frequencies unambiguous.

**Acceptance:** Tuning, changing span, switching VFOs, restarting capture and reconnecting never display old data under a new frequency axis. Spectrum/waterfall clicks use the same current grid.

### F4 — P2: A delayed settings-save acknowledgement can overwrite a newer edit

**Evidence:** Reproduced with a delayed preferences response. The browser submits COM4, the user changes the field to COM6, the older response arrives, and the next state update restores COM4 before the newer save reads the field.

**Cause:** `web/app.js:69` clears every preference's dirty flag after any preferences acknowledgement. It does not identify which version of each field was saved.

**Fix:** Capture field values and edit revisions with each save. Clear only the revisions acknowledged by that response. Serialize or merge saves, and explicitly display saving, saved and failed states. Validate complete preference requests before mutating desktop controls. Report INI write failures instead of silently treating them as saved (`src/dashboard.cpp:1612`).

**Acceptance:** Rapid edits, slow responses, out-of-order responses, lost connections and desktop/browser edits preserve the latest intended value. The final confirmed settings survive restart.

### F5 — P2: Continuous drag/wheel tuning loses steps and waits for a pause

**Evidence:** Reproduced against the browser's current dial queue: 30 uninterrupted steps emit no command until the debounce expires, then emit only 10 steps.

**Cause:** `web/app.js:349` restarts a 90 ms timer on every event and clamps the accumulated count to ±10. Sustained motion can indefinitely postpone sending while dropping additional movement.

**Fix:** Flush tuning at a regular bounded rate during dragging, accumulate the complete delta per VFO, and split oversized batches without discarding steps. Flush the final delta when the gesture ends; discard unsent gestures when the connection generation changes.

**Acceptance:** Sustained drag and wheel input tune continuously; 30 requested steps produce 30 steps. Verify direction changes, VFO changes, touch cancellation and reconnects.

### F6 — P2: Stopped or stalled RF capture can leave a valid-looking, clickable trace

**Evidence:** Confirmed by inspection. `Capture::latest` at `src/rf_scope.cpp:149` returns the last valid frame indefinitely. Starting/stopping capture does not invalidate it or attach an age. The browser retains `rfBins`, and `tuneRfFromCanvas` at `web/app.js:315` checks for bins and an online radio but not fresh RF capture.

**Fix:** Add capture generation, sequence and acquisition time to RF state. Distinguish live, waiting, frozen history, stopped and failed. Preserve historical images only with an explicit stale/frozen indicator, and prevent tuning from an obsolete grid.

**Acceptance:** Stopping capture or losing frames visibly marks the scope stale within a defined interval. Restart waits for a fresh frame, and clicking an obsolete scope cannot retune the radio.

### F7 — P2: Disarming one browser microphone can interrupt another browser's TX

**Evidence:** Reproduced the client-side command sequence with browser 2 owning PTT and browser 1 disarming its own microphone. Browser 1 still sends the global microphone-route disable. `Dashboard::toggleAudio` releases PTT when that global route is disabled.

**Cause:** `web/app.js:239` checks PTT ownership before an explicit PTT-off request, but always disables the shared host microphone route. The permission-failure cleanup at `web/app.js:244` also disables the global route.

**Fix:** Track local microphone arming per browser. Give the host route an explicit usage/ownership policy; one browser's disarm or permission denial must not tear down another browser's active route. Preserve a deliberate global stop control and desktop override.

**Acceptance:** With two browsers armed, disarming the non-transmitting browser or denying its microphone permission leaves the transmitting browser unaffected. Disconnecting the transmitting owner still releases its PTT.

## Improvements and remaining investigations

### Phone audio and browser recovery

The reported iPhone playback-volume drop remains a device/browser investigation. The current code does not lower its listening gain during mic arming and requests echo cancellation, noise suppression and automatic gain control off. Safari audio-session/routing changes remain a plausible cause; this review did not reproduce the symptom on the phone.

Introduce a browser audio controller that owns capture, playback, permission requests and cleanup. Handle microphone track termination, audio-context interruption, device changes and page restoration. At present, `web/app.js:379` closes the socket and sets `closing = true` on `pagehide`, with no `pageshow` recovery handler for a cached page restoration. There are also no mic track-ended or audio-context state-change handlers.

Evaluate Safari audio-session handling on the actual phone. Show audio readiness and a clear resume action when a user gesture is required. Avoid compensating with an unconditional gain boost: restored output could then become too loud. Reconnection may restore listening when allowed, but must not automatically re-key PTT.

Validation matrix: Safari on the user's iPhone, desktop Chromium, speaker and available headset routes; mic arm/disarm, PTT cycles, lock/unlock, tab changes, browser back/forward, permission denial, Wi-Fi interruption and device removal.

### Rendering and network performance

Both browser waterfall renderers rebuild their complete pixel image during drawing (`web/app.js:273`, `web/app.js:300`). RF drawing also creates a new color array per pixel. State updates trigger additional redraws even without new RF data. This is confirmed unnecessary work; its cost on the user's phone has not yet been measured.

Use a persistent image buffer, a precomputed color lookup table, and incremental waterfall-row updates. Coalesce drawing with `requestAnimationFrame` and render only visible panels when their data changes. Keep the compact spectrum, waterfall, VFOs and PTT accessible together on mobile.

The server sends a complete settings/device/activity snapshot repeatedly, and sends both scopes to connected browsers regardless of the visible page (`src/dashboard_web.cpp:159`, `src/web_server.cpp:314`). Add page/scope subscriptions, separate infrequently changing capabilities from live readings, and send activity incrementally. Measure command latency, frame rate, audio underruns and bytes sent before deciding whether separate audio/control transports are needed.

### Shared application state and diagnostics

`Dashboard::webCommand` currently applies many web changes by manipulating Win32 controls. Move validated settings and operations into a shared controller/model so native and browser views render the same confirmed state. Do this gradually alongside the fixes above.

Add a compact connection/audio diagnostic view: current COM ports, last successful CAT reply, RF frame age, browser connection state, audio-context state and selected devices, plus command timing. Distinguish queued, confirmed and failed operations. Keep verbose driver output in Activity and retain the requested simple powered-off status.

Make the Windows-session policy explicit: the current handler stops all audio on a Windows lock or RDP disconnect (`src/dashboard.cpp:1889`). Consider an unattended remote-station option that releases session-owned TX while allowing independently authenticated browser receive audio to continue. It should be an intentional operating mode with specific disconnect tests.

### Build, release and test coverage

- Add the reproduced failures to permanent regression tests. Extend the committed GUI suite from four to all six tabs; the isolated six-tab run already passes.
- Register tests with CTest and add Windows CI. Cover browser save ordering, tuning, RF packet transitions, PTT ownership, WebSocket/session expiry and browser lifecycle in isolated fixtures.
- Allow configuring/building without FTDI runtime DLLs. They are loaded dynamically for scope capture, but `CMakeLists.txt:74` and `scripts/build.ps1:8` currently require them before a build can proceed. Stage them only when supplied.
- Add an explicit release-packaging command using an allowlist and a check that Hamlib, FTDI and other third-party runtime DLLs are absent. Keep local development staging separate from published application assets.
- Add a dependency-check helper that can run before the executable starts, report missing/incompatible x64 Hamlib/compiler runtimes and link to the upstream downloads. Test a clean Windows installation using externally installed dependencies.
- Document the supported runtime versions and the required FT-710 **SCU-LAN10 = ON** setting prominently. Keep the project presented as a complete remote-control application.

## Recommended implementation order

| Phase | Work | Completion checks |
| --- | --- | --- |
| 1. Reliable transmit release | F1; F2; worker-owned release retries and responsive command dispatch | Fault-injected release recovers; slow CAT-2/device operations do not block PTT-off dispatch; existing radio checks pass |
| 2. Browser state and audio | F4; F7; audio lifecycle, cached-page restoration, actionable errors; investigate Safari volume behavior | Latest edits survive delayed replies/restart; two-browser ownership checks pass; phone interruption/recovery matrix completed |
| 3. Scope and tuning correctness | F3; F5; F6; preserve draft scope settings separately from confirmed radio values | Exact accumulated tuning deltas; correct frequency grid across span/VFO changes; stale captures clearly identified |
| 4. Performance and mobile polish | Incremental waterfalls, visible-page updates, smaller state messages, diagnostics | Profile before/after on the actual phone; compare frame time, audio underruns, bandwidth and PTT latency under concurrent scopes/audio |
| 5. Release readiness | Permanent regression coverage, six-page GUI tests, CI, optional FTDI staging, clean release packaging and setup docs | Fresh Windows setup succeeds with external dependencies; release archive contains only intended application assets; SCU-LAN10 and remote setup documented |

The plan can be delivered in these small, reviewable stages. Hardware validation should follow the relevant phase, especially for iPhone audio routing, actual FT-710 power transitions and RF scope behavior.
