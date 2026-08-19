# Native External TV Control UI Design

## Purpose
Add an in-app LGTV Companion configuration, inspection, and diagnostic surface for the already-native Samsung/Vizio orchestration stack without creating a second controller implementation.

## Parent / authority
- Parent branch: `feature/native-tv-orchestration`
- Parent reference commit at design approval: `2df3920c25e96add22fe6926295a3456b0c3010b`
- Accountable human: repository owner/operator
- Authority ceiling: configure and invoke existing native TV orchestration through LGTV Companion; no authority to shut down/reboot Windows from diagnostics, expose credentials, change unrelated LG behavior, or simplify proven Samsung/Vizio transport quirks.

## Scope
Create a separate in-app `Native External TV Control` dialog opened from existing Global Settings. The service remains authoritative for controller state, transports, device ordering, credentials, idle deadline, and verification.

The UI exposes:
- global enable, topology-preservation toggle, extended-idle minutes, service/idle/deadline status;
- Samsung enable/IP/MAC/masked-token-presence, interpreted state, connection test, Screen Off, Restore Screen, Power On, Power Off, optional WOL;
- Vizio enable/IP/MAC/masked-auth-presence, interpreted state, connection test, Blank, Unblank, Power On, Power Off, optional WOL;
- orchestration diagnostics for User Idle, User Busy, Extended Idle, Suspend, and Shutdown plans, where Suspend/Shutdown are device-orchestration simulations only and never suspend/shut down Windows.

## Exclusions
- No separate configuration utility.
- No UI-side Samsung Tizen or Vizio SmartCast transport implementation.
- No credential echo over IPC, logs, diagnostics, screenshots, or debug output.
- No temporary rewriting of `ExtendedIdleMinutes` for tests.
- No toggle semantics where explicit guarded operations exist.
- No changes to the dirty legacy `X:\Code\LGTV-Samsung` checkout.
- No removal of `X:\Code\TVCODE` reference material.

## Architecture
Use the existing duplex named pipe. Add a namespaced external-TV diagnostic/control command family handled by the service. Requests carry an operation and request identifier. Responses carry only credential-free structured status/results and are correlated by request identifier.

Flow:
`LGTV Companion UI -> existing named-pipe IPC -> Companion -> DeviceCoordinator -> existing Samsung/Vizio controllers/transports -> state verification -> credential-free result -> UI`

The UI must not infer success from HTTP/WebSocket acceptance alone when the existing controller can verify a resulting device state.

## Service diagnostics contract
The service exposes read-only state snapshots and explicit diagnostic actions. The snapshot includes orchestration enabled state, idle/deadline state, Samsung interpreted state, Vizio interpreted state, and credential-presence booleans only.

Diagnostic actions use the same production controller/transport implementation and preserve per-device strand ordering. Device results distinguish request admission, transport execution, verification outcome, resulting interpreted state, and human-readable error text.

Extended-idle testing invokes the extended-idle device action plan directly. Suspend/shutdown tests invoke their device action plans without emitting an OS power event.

## Configuration handling
Persist via existing `Preferences::external_tv_` / `ExternalTvSettings` schema under `LGTV Companion -> ExternalTVOrchestration`.

Token/Auth edit controls are masked. Existing secrets are never populated back into UI text. A blank secret field means preserve the stored value; newly entered secret text replaces it only on explicit save. Diagnostic/status responses return presence flags, never secret values.

Saving normal settings may use the existing service-restart lifecycle. Diagnostic operations must never silently rewrite saved configuration.

## UI structure
Add a dedicated dialog consistent with the current Win32 resource/dialog style instead of overcrowding `IDD_OPTIONS`. Global Settings contains a launcher button/link for `Native External TV Control...`.

Visually and semantically distinguish panel blanking (`Screen Off` / `Blank`) from actual `Power Off`. Power actions are explicit and never implemented as blind toggles.

The result area reports action requested, accepted/executed status, verification result, resulting device state, and readable error information.

## Safety invariants
1. Samsung `PictureOff` is powered-and-blanked evidence only; it never proves the Accessibility overlay remains open.
2. Samsung restoration retains the existing safe wake/reopen/disable/close sequence where required.
3. Vizio transport/power logic retains current model-specific guarded semantics, including the nominal `POW_OFF` toggle hazard.
4. Windows AC/DC monitor timeout remains topology-safe when configured.
5. A diagnostic test cannot alter the configured 60-minute deadline unless the human explicitly saves a different value.
6. Credentials never cross the diagnostics boundary in plaintext.

## Testing
TDD applies. Add deterministic tests first for:
- diagnostic operation parsing/routing;
- credential-free snapshot/result serialization;
- direct extended-idle/suspend/shutdown diagnostic plan generation without production timeout mutation;
- Samsung/Vizio state/result mapping;
- configuration merge semantics that preserve an existing secret when the UI submits no replacement;
- request/result correlation and malformed/unsupported command rejection.

Then run the existing native-TV regression suite and full x64 Release solution build with VS2026/v145.

Hardware verification must cover each Samsung and Vizio UI action, resulting-state verification, repeated service cycles, and ordinary idle -> busy behavior with the normal configured timeout unchanged.

## Acceptance criteria
- UI is part of LGTV Companion and follows existing dialog/resource conventions.
- UI uses only service-owned diagnostics/control for native Samsung/Vizio actions.
- No credential leakage occurs.
- Panel blanking and power-off actions are distinct.
- State shown after an action reflects post-action verification where available.
- Extended-idle diagnostics do not rewrite production timeout.
- Suspend/shutdown diagnostics do not suspend/reboot/shut down Windows.
- Existing Samsung/Vizio lifecycle regressions remain green.
- x64 Release/v145 build succeeds.
- Real Samsung and Vizio hardware tests pass.
- RepoAI immutable review artifact and internal ChatGPT review contain no unresolved promotion-blocking findings.

## Promotion gates
No merge/publish merely because source compiles. Promotion requires: deterministic tests -> full Release build -> hardware verification -> idle/busy regression -> RepoAI/ChatGPT review -> human-governed fan-in -> installed-service restart/verification -> push/publish.

If any service/UI schema, device-state interpretation, or build identity disagrees during fan-in, fail closed and reconcile before promotion.

## Closure condition
Feature closes only when the UI and service diagnostics are merged/published, the installed service runs the verified build, both TVs pass the approved hardware matrix, rollback reference is preserved, and the branch has no unresolved material review findings.