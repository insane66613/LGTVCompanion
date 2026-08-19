# Native TV Orchestration Implementation Plan

> **For agentic workers:** Implement task-by-task with test-first red/green verification. Keep TVCODE running until the explicit cutover gate.

**Goal:** Port the topology-safe Samsung/Vizio idle and power orchestration from TVCODE commit `9f72cb3fe5525cad5efd569549b8bfbc8456001e` into LGTV Companion native C++ while retaining existing LG/WebOS behavior.

**Architecture:** LGTV Companion's existing service event stream remains authoritative. A portable `IdleCoordinator` converts user-idle/busy lifecycle events into semantic device actions; native asynchronous Samsung and Vizio controllers consume those actions on the existing Boost.Asio `io_context`. Windows display-timeout enforcement is isolated behind a platform policy and uses PowrProf APIs rather than process launching.

**Tech Stack:** C++17, Boost.Asio, Boost.Beast, OpenSSL, nlohmann/json, Win32/PowrProf only in the Windows policy adapter.

**Source specification:** `X:\Code\TVCODE` at `9f72cb3fe5525cad5efd569549b8bfbc8456001e`, especially `tests/test_topology_safe_idle_policy.py`, `Lib/Pipe.ahk`, `Lib/Samsung.ahk`, `Lib/Vizio.ahk`, and `Lib/Power.ahk`.

## Global constraints

- Short idle must preserve HDMI/Windows monitor topology; panel blanking is distinct from full device power-off.
- Windows monitor timeout remains AC=0 and DC=0 when topology preservation is enabled.
- Extended shutdown deadline is 60 minutes by default and is armed from the first idle only; repeated idle never extends it.
- Busy/resume cancels the deadline and restores blanked/powered-off external TVs.
- Samsung `pictureoff` means powered=true and blanked=true.
- Samsung `KEY_POWER` is a toggle and must never be sent without a state guard.
- Samsung Accessibility enable is `KEY_AD`, approximately 1400 ms, `KEY_ENTER`, with no `KEY_RETURN` on success.
- Samsung restore from pictureoff with known-open menu is ENTER(wake), wait, ENTER(toggle off), RETURN(close); never KEY_UP.
- Unknown Samsung menu state after restart is reconciled by wake, reopen Accessibility, ENTER first item, RETURN.
- Vizio short idle uses SmartCast panel blank; full power-off occurs only at the extended deadline or suspend/shutdown.
- All network waits and menu delays must be asynchronous and must not block LGTV Companion's core event loop.
- SmartThings is optional/fallback only; local Samsung Tizen control is primary.
- Do not retire TVCODE until native unit/build/live parity is demonstrated.

## File structure

- `LGTV Companion Service/idle_coordinator.h/.cpp` — portable first-idle/deadline state machine.
- `LGTV Companion Service/samsung_controller.h/.cpp` — native Samsung state probe, Tizen remote, WOL and Accessibility state machine.
- `LGTV Companion Service/vizio_controller.h/.cpp` — native SmartCast blank/unblank/power and WOL state machine.
- `LGTV Companion Service/device_coordinator.h/.cpp` — fan-in owner for semantic lifecycle events and extended-idle timer.
- `LGTV Companion Service/windows_display_power_policy.h/.cpp` — PowrProf monitor-timeout policy; only file allowed to own Windows power-plan mutation.
- `Common/preferences.h/.cpp` — backward-compatible parsing/writing of external-device orchestration settings.
- `LGTV Companion Service/companion.cpp` — minimal wiring from existing events into `DeviceCoordinator`.
- `LGTV Companion Service/LGTV Companion Service.vcxproj` — compile/link new native sources and `PowrProf.lib`.
- `Tests/native_tv_orchestration_tests.cpp` + project — native deterministic regression tests with fake controller ports and a manual clock/scheduler boundary.

## Task 1 — Core idle state machine (TDD)

- [ ] Add native tests proving first idle arms once, repeated idle preserves the original deadline, busy cancels it, and expiry emits exactly one extended-idle action.
- [ ] Run tests and verify RED because `IdleCoordinator` does not exist.
- [ ] Implement `IdleCoordinator` with explicit `Active`, `Idle`, and `ExtendedIdle` lifecycle and monotonic deadline semantics.
- [ ] Re-run targeted tests and verify GREEN.
- [ ] Commit the independently reviewable core state-machine change.

## Task 2 — Samsung protocol/state machine (TDD)

- [ ] Add tests for `pictureoff` classification, guarded full power-off, Accessibility enable sequence, known-open awake restore, pictureoff selection-preserving restore, and restart-safe unknown-menu restore.
- [ ] Verify RED.
- [ ] Implement an injectable Samsung transport interface plus the production Beast HTTP/WebSocket transport.
- [ ] Implement `SamsungController` as a nonblocking state machine using Asio timers (1400 ms / 1200 ms / 350 ms transitions).
- [ ] Verify targeted and core tests GREEN; commit.

## Task 3 — Vizio protocol/state machine (TDD)

- [ ] Add tests proving blank is distinct from power-off, HASHVAL blank action is used, busy unblanks, extended idle powers off, and failed network operations terminate without blocking the coordinator.
- [ ] Verify RED.
- [ ] Implement native SmartCast HTTPS transport on port 7345, self-signed certificate tolerance scoped to the device connection, key-command unblank, state-guarded power operations, and WOL.
- [ ] Verify GREEN; commit.

## Task 4 — Device fan-in and lifecycle mapping (TDD)

- [ ] Add integration tests mapping USER_IDLE, USER_BUSY, RESUME, SUSPEND, SHUTDOWN, and DISPLAY_OFF semantics onto fake Samsung/Vizio controllers.
- [ ] Verify RED.
- [ ] Implement `DeviceCoordinator` on the service Asio executor; ensure repeated events are idempotent and failures are isolated per device.
- [ ] Verify GREEN; commit.

## Task 5 — Topology-safe Windows policy (TDD/adapter test)

- [ ] Add adapter tests proving topology policy is invoked when enabled and omitted when disabled.
- [ ] Verify RED.
- [ ] Implement `WindowsDisplayPowerPolicy` using `PowerGetActiveScheme`, `PowerWriteACValueIndex`, `PowerWriteDCValueIndex`, and `PowerSetActiveScheme` for the display idle timeout subgroup/setting.
- [ ] Confirm no DPMS/display-off call is introduced by the external-device path.
- [ ] Verify GREEN; commit.

## Task 6 — Configuration and service wiring

- [ ] Add parse/round-trip tests for independent Samsung, Vizio, topology-preservation, and extended-idle settings with safe defaults disabled for unconfigured devices.
- [ ] Verify RED.
- [ ] Extend `Preferences` without overloading LG/WebOS device entries.
- [ ] Instantiate `DeviceCoordinator` in the service and call it from the same internal event path used before the named-pipe compatibility output.
- [ ] Keep existing LG `blankScreen`/`powerOn`/`powerOff` logic unchanged.
- [ ] Verify native tests and service build; commit.

## Task 7 — Baseline/current-upstream compatibility

- [ ] Preserve upstream August 2026 idle-input changes, including the direct-input-only option and current daemon event semantics.
- [ ] Build with the installed VS2022 toolchain using a command-line v143 override if possible; do not downgrade committed upstream v145 project settings merely for this machine.
- [ ] If v143 build cannot prove compatibility, record the exact blocker rather than fabricating build success.

## Task 8 — Live parity gate

- [ ] Migrate current TVCODE device endpoints/auth material into the new LGTV Companion configuration without logging secrets.
- [ ] Build and stage the native binaries without replacing the running install.
- [ ] With TVCODE still running, exercise controller-level live probes that do not create duplicate idle actions.
- [ ] Temporarily prevent duplicate control, then verify short idle preserves monitor count/topology and produces LG blank + Samsung `pictureoff` + Vizio blank.
- [ ] Verify busy restores Samsung persistent Screen Off Mode and Vizio blank state.
- [ ] Verify first-idle deadline remains fixed across repeated idle events using accelerated test configuration before validating the production 60-minute value.
- [ ] Verify suspend/shutdown mapping through test hooks; do not reboot/shut down the workstation solely for validation.

## Task 9 — Cutover and completion protocol

- [ ] Only after parity: disable/remove TVControl startup, stop `tv_control.ahk` and `pipe_reader.ahk`, and verify no orphan AHK controller remains.
- [ ] Deploy/restart LGTV Companion from the tested commit.
- [ ] Hash the running installed binaries and match them to the build artifacts from the commit.
- [ ] Re-run native tests and relevant TVCODE reference tests.
- [ ] Fan in the feature branch, commit all intended changes, push the intended fork remote(s), and verify remote heads.
- [ ] Verify clean worktree and remove the temporary worktree/branch when safe.
- [ ] Preserve `X:\Code\TVCODE` as a historical/reference implementation.


## Implementation and live-cutover record — 2026-08-19

Native orchestration was validated against the production Samsung and Vizio sets before retiring TVCODE startup ownership. The final full-solution Release/x64 service artifact was deployed after the final idempotency review fix and has SHA-256 `1ABBF8AF26677260B147C6AC676319785E3A896E1C72E254B53D0D3E7211AFEB`, matching the installed `LGTVsvc.exe` byte-for-byte.

Live accelerated parity used a temporary one-minute extended-idle interval. Starting from Samsung standby and Vizio off, `userbusy` restored both sets to `on`; `useridle` armed one fixed deadline; expiry produced Samsung `standby` and Vizio `off`; the following `userbusy` restored both to `on`. Production configuration was then restored to `ExtendedIdleMinutes=60` with native orchestration enabled.

### On-device protocol findings

- Samsung QN75Q60CAFXZA accepts a WebSocket connection before the remote-control channel is authorized. A successful socket write is not proof of key delivery. Native Tizen control now waits for `ms.channel.connect` and rejects `ms.channel.unauthorized` before sending any key.
- Samsung `KEY_POWEROFF` remains unsuitable for this set; guarded `KEY_POWER` is used only from proven powered states. The authorization fix changed the native test from `on -> accepted -> on` to `on -> accepted -> standby`.
- Samsung `pictureoff` remains a powered/blanked state, not standby. Persistent Screen Off Mode is reconciled before the extended-idle power toggle.
- Vizio SmartCast `HASHVAL` can exceed signed 32-bit range; the observed value `3453326231` requires unsigned preservation.
- Vizio `POW_OFF` is toggle-like on this set when repeated blindly. The native implementation therefore sends it only from verified `On` state and suppresses it for `Off` or `Unknown`.
- After prolonged SmartCast blanking, this Vizio can acknowledge a key command while discarding the queued action. The proven native envelope includes pyvizio-compatible `_url` metadata, ordinary HTTP/1.1 client headers, a two-second post-response settle window for `/key_command/`, and socket teardown without TLS `close_notify` on that endpoint.
- The first long-blank `POW_OFF` may still be discarded. One additional `POW_OFF` is permitted only after the complete verification window still proves `On`; the retry is suppressed immediately for `Off` or `Unknown`. This produced the final service transition to verified `off` without blind toggling.
- A powered-off Vizio may acknowledge BACK without waking. Busy/resume therefore probes power first: verified `Off` selects native WOL/power-on; verified `On` selects BACK/unblank; unknown state remains conservative and is re-probed before accepting recovery.
- Repeated lifecycle events are idempotent at the device-plan layer. Repeated idle preserves the first monotonic deadline and emits no additional blank commands; a restore-needed latch allows one startup/post-idle/post-power-off reconciliation and suppresses subsequent busy/display-on/resume remote commands until another restore is actually needed.

### Cutover and rollback

- Runtime device credentials were migrated into `LGTV Companion -> ExternalTVOrchestration` without logging them; diagnostic serialization remains redacted.
- Windows topology preservation is enforced by the native PowrProf adapter with AC/DC display timeout set to Never when the feature is enabled.
- `tv_control.ahk` and `pipe_reader.ahk` are no longer running and no longer own startup behavior.
- The former user Startup shortcut `ToggleMonitor.lnk` was removed only after its SHA-256 matched the rollback copy in `X:\Code\TVCODE\Archive\Backups\LGTVCompanion-native-cutover-20260819-100458`.
- The disabled legacy scheduled task `sam`, which launched `N:\Code\TVCODE\samsung.ahk`, was exported as `scheduled-task-sam.xml` into the same rollback bundle and then removed.
- TVCODE remains preserved as historical/reference source; it is not part of the active orchestration path.
