# Native External-TV Configuration & Diagnostics UI

**Status:** APPROVED on 2026-08-19. This specification controls the UI/diagnostic phase.

**Parent:** `Docs/plans/2026-08-19-native-tv-orchestration.md`

**Purpose:** Add a native Win32 UI for configuring, inspecting, and safely testing the production Samsung/Vizio orchestration already owned by `DeviceCoordinator`.

## Scope and authority

- Add a separate `IDD_EXTERNAL_TV` dialog launched from Global Settings.
- Preserve `DeviceCoordinator` as the sole service-side authority for device state and actions.
- Add namespaced named-pipe request/response diagnostics through the service IPC boundary. Diagnostics use a dedicated bounded message-mode pipe so structured requests/responses preserve message boundaries without changing the legacy byte-stream External API pipe.
- Every diagnostic request carries a unique request ID; every response echoes it.
- Responses are structured, credential-free, and distinguish request acceptance, action execution, and verified resulting state.
- The UI may edit enable flags, topology-preservation policy, extended-idle minutes, endpoint/MAC fields, and replacement credentials.
- Existing credentials are never returned to the UI. Stored credentials render only as presence/masked state and may be replaced explicitly.
- Diagnostic actions are bounded to device-level status/probe/blank/unblank/power operations supported by the existing coordinator.
- No diagnostic control may suspend, shut down, reboot, log off, or otherwise power-manage the host workstation.
- No runtime-only mutation of the extended-idle timeout is added.
- Existing LG/WebOS configuration and lifecycle behavior remain unchanged.

## Diagnostic contract

Requests use the namespace `external_tv` and contain `request_id`, a required `device` selector, and `operation`. Service responses contain the same `request_id` plus `accepted`, `executed`, `verified`, `device`, `operation`, `resulting_state`, and a bounded human-readable `message`. Unknown operations and malformed requests fail closed.

A successful transport write or protocol ACK alone is not `verified=true`. Verification requires the existing controller/coordinator probe path to observe the requested resulting state. If verification is unavailable or inconclusive, report that explicitly without inventing success.

## UI behavior

- Configuration fields load from preferences; secret fields load empty with a separate stored/not-stored indicator.
- Save writes replacement secrets only when the corresponding secret edit is non-empty; blank secret edits preserve existing stored values.
- Test buttons disable while a request is in flight and complete by matching `request_id`, not arrival order.
- Status output shows accepted/executed/verified independently and never displays credentials.
- Pipe/service unavailable, timeout, malformed response, and device-state unknown are distinct outcomes.
- Service/network diagnostics must not block the Win32 UI thread.

## Acceptance gates

1. TDD proves request/response parsing, request-ID correlation, redaction, malformed-input rejection, and verification semantics.
2. Service IPC delegates device operations to `DeviceCoordinator`; no second orchestration state machine is created.
3. Native UI can configure both Samsung and Vizio without exposing stored credentials.
4. Release/x64 solution build succeeds from a clean incremental state and targeted/full native tests pass.
5. Live Samsung and Vizio diagnostics prove reported verified state against hardware.
6. Idle/busy regression confirms existing first-idle deadline and idempotent restore behavior is unchanged.
7. RepoAI and internal ChatGPT review have no unresolved material finding before fan-in/publish.
8. Final commit, deployed binaries, running service, and remote branch are each independently verified; local build success is not treated as publication or deployment success.
