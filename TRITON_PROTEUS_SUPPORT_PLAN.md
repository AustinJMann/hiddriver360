# Triton-over-Proteus Support Plan

## 1. Goal

Add reliable support for one Valve Steam Controller 2026 (codename **Triton**) connected wirelessly through one Valve **Proteus** puck to HidDriver 360.

The Xbox 360 should see one ordinary virtual Xbox 360 gamepad. The implementation must handle the puck's multi-interface USB shape internally without presenting four controllers to XAM.

This document is an implementation plan, not an implementation. Protocol details are based primarily on current SDL source and must be confirmed against a real Proteus capture before the driver is considered complete.

## 2. Scope decisions

### In scope

- Valve VID/PID `28DE:1304` (Proteus puck) only.
- Proteus controller-slot HID interfaces 2 through 5.
- One active Triton exposed as one XAM/XInput gamepad.
- A controller bonded to any of the four puck slots.
- Controller connect, disconnect, reconnect, puck hotplug, and puck removal.
- Standard XInput controls:
  - A, B, X, Y
  - D-pad
  - Menu/View mapped to Back/Start using SDL's current mapping
  - LB/RB
  - LT/RT as analog triggers
  - L3/R3
  - Both analog sticks
  - Steam button through the driver's existing Xbox Guide-button path
- Triton state report IDs `0x42`, `0x45`, and `0x47`, using their stable common prefix.
- Wireless status report IDs `0x46` and `0x79`.
- Disabling firmware keyboard/mouse fallback ("lizard mode") with a periodic feature-report heartbeat.
- Host-side decoder and state-machine tests plus real-console hardware validation.

### Explicitly out of scope

- Simultaneous multi-controller support through one puck.
- Multiple Proteus pucks.
- Direct wired Triton (`28DE:1302`).
- Bluetooth/BLE Triton (`28DE:1303`).
- Nereid (`28DE:1305`).
- Pairing, bond editing, radio management, firmware update, or bootloader support.
- Rumble/haptics. HidDriver 360 already lists rumble as unsupported.
- Trackpads, touch/capacitive sensors, IMU, quaternion data, battery UI, QAM, or rear-paddle exposure. XInput has no faithful representation for most of these.
- Rebinding extra Triton controls onto unrelated XInput buttons.

### Required user setup

- Triton must already be paired to the Proteus puck.
- The puck and controller must be on mutually compatible firmware.
- If more than one controller is paired and powered on, only one will be published; the others will be ignored until the active controller disconnects.

## 3. Why this cannot be a normal static HID mapping

The current generic mapping path assumes that one USB HID interface is one controller and that the HID report descriptor truthfully describes gamepad axes and buttons. Proteus breaks both assumptions:

1. `28DE:1304` exposes four controller-slot HID interfaces (interfaces 2–5).
2. Each interface represents a wireless bond slot, not an independently desired Xbox controller.
3. The puck initially exposes keyboard/mouse-style lizard reports until the host periodically sends a Valve feature report.
4. Triton state is a Valve-defined little-endian packet, not a Generic Desktop gamepad report that the current LUFA-derived parser can map.
5. Wireless connect/disconnect is reported independently of USB enumeration.

Therefore, adding `28DE:1304` to `mapping.cpp` would create phantom/multiple controllers, start the mapping assistant on vendor reports, fail to maintain raw mode, and mishandle multibyte values on the Xbox 360's big-endian PowerPC CPU.

## 4. Evidence baseline

### Device identity and topology

- Valve VID: `0x28DE`.
- Proteus PID: `0x1304`.
- SDL accepts Proteus controller interfaces 2–5 and treats them as Triton transports.
- Each slot has the controller report descriptor; non-slot/management interfaces must not be claimed by this backend.

### State reports

The first 18 bytes are common to report IDs `0x42`, `0x45`, and `0x47`:

| Offset | Size | Encoding | Meaning |
|---:|---:|---|---|
| 0 | 1 | `u8` | Report ID |
| 1 | 1 | `u8` | Sequence number |
| 2 | 4 | little-endian `u32` | Buttons |
| 6 | 2 | little-endian signed value, valid range 0–32767 | Left trigger |
| 8 | 2 | little-endian signed value, valid range 0–32767 | Right trigger |
| 10 | 2 | little-endian `s16` | Left stick X |
| 12 | 2 | little-endian `s16` | Left stick Y, positive is up |
| 14 | 2 | little-endian `s16` | Right stick X |
| 16 | 2 | little-endian `s16` | Right stick Y, positive is up |

Only this common prefix is needed for an Xbox 360 mapping. This deliberately avoids depending on the firmware-sensitive trackpad/IMU tail, including the recent transition from quaternion-bearing `0x42` traffic to no-quaternion `0x45` traffic.

### Relevant button bits

| Mask | Triton input | Xbox 360 result |
|---:|---|---|
| `0x00000001` | A | A |
| `0x00000002` | B | B |
| `0x00000004` | X | X |
| `0x00000008` | Y | Y |
| `0x00000020` | R3 | Right thumb |
| `0x00000040` | View | Start |
| `0x00000200` | R bumper | RB |
| `0x00000400` | D-pad down | D-pad down |
| `0x00000800` | D-pad right | D-pad right |
| `0x00001000` | D-pad left | D-pad left |
| `0x00002000` | D-pad up | D-pad up |
| `0x00004000` | Menu | Back |
| `0x00008000` | L3 | Left thumb |
| `0x00010000` | Steam | Existing Guide-button path |
| `0x00080000` | L bumper | LB |
| `0x00800000` | Right trigger click | Digital fallback only if analog RT is zero |
| `0x08000000` | Left trigger click | Digital fallback only if analog LT is zero |

The QAM button, four rear paddles, trackpad clicks/touches, joystick touch, and grip touch bits will be parsed only as known/reserved bits and otherwise ignored. They must not be silently aliased onto standard buttons.

### Wireless status

- Report IDs: `0x46` and `0x79`.
- Byte 1 value `1`: disconnect.
- Byte 1 value `2`: connect.
- A valid state report is also authoritative evidence that a controller is connected, allowing recovery if a status report was missed.

### Lizard-mode heartbeat

Send a 64-byte HID feature report on report ID `0x01`:

```text
01 87 03 09 00 00 00 ... 00
```

- Byte 0: feature report ID `0x01`.
- Byte 1: `ID_SET_SETTINGS_VALUES` (`0x87`).
- Byte 2: payload length `3`.
- Byte 3: `SETTING_LIZARD_MODE` (`9`).
- Bytes 4–5: little-endian `LIZARD_MODE_OFF` (`0`).
- Remaining bytes: zero.

The USB class request is expected to be:

- `bmRequestType = 0x21` (host-to-device, class, interface)
- `bRequest = 0x09` (`SET_REPORT`)
- `wValue = 0x0301` (Feature report, ID 1)
- `wIndex = slot interface number`
- `wLength = 64`

SDL refreshes lizard-off at most every 3 seconds. The Xbox driver should target a 2-second interval to leave margin for scheduling delays, never queueing a second control transfer while the previous transfer is pending.

## 5. Mandatory capture-first gate

Before finalizing constants or USB lifecycle code, capture a real `28DE:1304` puck on a PC with the exact controller/puck firmware available for testing.

### Capture set

Collect:

1. Full USB device, configuration, interface, HID, report, and endpoint descriptors.
2. Puck inserted with the controller powered off.
3. Controller powered on and left idle.
4. One deliberate sweep of every standard button, D-pad direction, trigger, and stick.
5. Controller power-off/disconnect.
6. Reconnect without unplugging the puck.
7. Puck unplug/replug with the controller already on.
8. The lizard-off feature request and at least 10 seconds of subsequent reports.

### Capture tooling

- Windows: USBPcap + Wireshark, with Steam starting after capture begins.
- Linux alternative: `usbmon`/Wireshark plus `hid-recorder` or a small hidraw dumper.
- Record firmware identifiers for Triton and Proteus alongside the capture.

### Capture hygiene

- Do not commit pairing/bond records, unit serials, radio keys, firmware blobs, or unrelated management traffic.
- Extract only descriptors, lizard-off requests, wireless-status reports, and sanitized input report fixtures.
- Store fixture provenance in a text manifest: date, firmware versions, interface number, report ID, expected decoded state, and source hash.

### Gate checks

Confirm all of the following before implementation proceeds past the protocol skeleton:

- Proteus is `28DE:1304`.
- Controller slot interfaces are exactly 2–5 on real firmware.
- Slot endpoints are interrupt-IN and their maximum packet size fits the proposed 64-byte buffer.
- The feature report is exactly 64 bytes and uses interface-recipient `SET_REPORT` with `wValue 0x0301`.
- `0x46` and/or `0x79` connection status appears on the slot interface.
- Current firmware emits at least one of `0x42`, `0x45`, or `0x47` and preserves offsets 0–17.
- The Xbox USB API reports actual transfer length somewhere usable. If it does not, the implementation must clear the receive buffer before every requeue and trust only known report IDs from the matched VID/PID/interface.
- Repeating `SET_CONFIGURATION` for multiple interfaces is either harmless or avoidable. This determines the exact enumeration sequencing described below.

If any item differs, update this plan and fixture expectations before writing the console-facing parser.

## 6. Proposed architecture

### 6.1 Keep protocol code separate from Xbox USB glue

Add a small platform-independent protocol module:

- `hiddriver/triton_protocol.h`
- `hiddriver/triton_protocol.cpp`

Responsibilities:

- Constants for Valve/Proteus identity, slot interfaces, report IDs, status values, and button masks.
- Explicit bytewise little-endian helpers.
- `DecodeTritonInputPrefix(bytes, length, decodedState)`.
- `DecodeTritonWirelessStatus(bytes, length, status)`.
- `BuildTritonLizardOffFeatureReport(out[64])`.
- Conversion from decoded Triton state to `ButtonsReport`.

The protocol module must not include Xbox kernel headers or call USB/XAM functions. This makes the most error-prone code host-testable.

### 6.2 Add a dedicated Proteus transport module

Add:

- `hiddriver/proteus.h`
- `hiddriver/proteus.cpp`

Responsibilities:

- Recognize only `28DE:1304`, class `0x03`, subclass `0`, protocol `0`, interfaces 2–5.
- Own a singleton `ProteusPuckContext`, consistent with the one-puck scope.
- Own four `ProteusSlotContext` records without consuming four `connectedControllers` entries.
- Initialize and tear down each slot USB interface.
- Queue and continuously requeue interrupt-IN transfers.
- Serialize per-slot control requests.
- Schedule lizard-off probes/heartbeats.
- Process connect/disconnect/state events.
- Select at most one active slot and publish at most one XAM controller.
- Rate-limit diagnostics for unknown report IDs and repeated errors.

### 6.3 Extend the existing controller record minimally

Add a controller kind/backend discriminator to `Controller`, for example:

```cpp
enum ControllerKind {
    CONTROLLER_GENERIC_HID,
    CONTROLLER_SWITCH_PRO,
    CONTROLLER_TRITON_PROTEUS,
};
```

Only the one published Proteus controller gets a `connectedControllers[]` slot. Inactive puck slot transports stay exclusively in `ProteusPuckContext`.

The mapping manager must never try to map a Proteus transport. A published Triton controller uses the dedicated decoded state and has no generic HID `reportInfo` or `HidDeviceMapping`.

### 6.4 Replace global asynchronous initialization scratch state

The current generic USB initialization uses process-wide `g_InitState`, `c`, `globalIndex`, `hidDescriptorBuffer`, and `reportDescriptorBuffer`. Four puck interfaces can enumerate close together, so these globals are unsafe even if only one virtual controller is published.

Move asynchronous initialization state into the per-interface extension/context:

- init phase
- selected controller/slot index
- HID descriptor length/copy where generic HID still needs it
- report descriptor allocation where generic HID still needs it
- receive buffer and size
- control-transfer busy flag
- removal/cancel flag

This prerequisite should preserve existing generic controller behavior while making simultaneous interface enumeration deterministic.

### 6.5 Use typed callback ownership helpers

The current callbacks recover `HidControllerExtension` using raw offsets (`-4` and `-36`). Introduce named helpers equivalent to `CONTAINING_RECORD` for interrupt and control TRBs, plus compile-time offset/size checks for the Xbox ABI.

Do not change the binary layout accidentally. The helper refactor must be built and tested with an existing supported controller before Proteus support is enabled.

## 7. Puck and slot state model

### Puck states

```text
EMPTY
  -> ENUMERATING (first matching slot interface arrives)
  -> READY (at least one slot IN endpoint is reading)
  -> REMOVING (slot remove callback or fatal USB error)
  -> EMPTY (all claimed slot interfaces are gone and callbacks are quiesced)
```

### Per-slot states

```text
UNSEEN
  -> INITIALIZING
  -> LISTENING_LIZARD
  -> PROBING_RAW_MODE
  -> CONNECTED_INACTIVE or CONNECTED_ACTIVE
  -> LISTENING_LIZARD on wireless disconnect
  -> REMOVING
  -> UNSEEN
```

### Single-controller arbitration

1. Queue reads on every initialized slot interface so a controller bonded to any slot works.
2. Send one lizard-off probe when each slot becomes ready. Also send one immediately after a wireless-connect status.
3. The first slot to deliver a valid state report becomes the active slot.
4. Publish exactly one virtual XAM controller from that slot.
5. Continue servicing and requeueing all other slot reads, but discard their gameplay state.
6. If another slot also has a controller, log one rate-limited `additional controller ignored` message and never allocate another XAM slot.
7. On active-slot disconnect, zero and unpublish the virtual controller. If another slot is already connected, promote the lowest-numbered eligible slot after a short event-coalescing pass; still only one controller exists at a time.
8. On active-slot reconnect, publish again without requiring a plugin or console restart.

The selection policy is deterministic: existing active slot wins; otherwise lowest interface number among slots with a valid recent state report wins.

## 8. USB enumeration and initialization sequence

### Device admission

In `HidAddDeviceHook`:

1. Read and byte-swap VID/PID as today.
2. If VID/PID is `28DE:1304`:
   - Claim only HID interfaces 2–5.
   - Route those interfaces to `ProteusAddSlotInterface`.
   - Do not route them to the generic HID mapper.
   - Do not claim any management or non-HID interface.
3. All other devices continue through the existing generic/Switch/DS3 paths.

### Configuration serialization

`SET_CONFIGURATION` is device-scoped while HidDriver 360 receives interface-oriented add callbacks. The capture gate must determine which Xbox kernel behavior is required.

Preferred implementation:

1. The first Proteus slot context becomes configuration owner.
2. It performs `SET_CONFIGURATION` once.
3. Other slot contexts wait in `INITIALIZING`.
4. On success, open each slot's interrupt-IN endpoint and start reads.

Fallback if the XDK handle model requires one request per interface:

1. Serialize the requests through the puck context.
2. Never have two `SET_CONFIGURATION` control TRBs in flight.
3. Open each interface endpoint only after its serialized request completes.

Do not issue four concurrent device configuration requests.

### Proteus descriptor handling

- Validate the interface and endpoint descriptors.
- Validate that an interrupt-IN endpoint exists before claiming the interface fully.
- Allocate the receive buffer from the endpoint maximum packet size, with a hard upper bound suitable for HID (the captured puck is expected to use 64 bytes).
- The dedicated backend does not need to parse the report descriptor to find axes/buttons.
- Still record descriptor/report lengths in diagnostics so firmware topology changes are visible.
- Fail the slot cleanly if descriptors or endpoints are malformed; never dereference a null endpoint descriptor.

### Startup probing

After a slot endpoint is listening:

1. Queue the IN transfer first so status/input responses cannot be missed.
2. Queue one lizard-off feature report on that same interface.
3. Mark a 2-second heartbeat deadline only after successful completion.
4. If the feature transfer fails, retry with bounded backoff while keeping the IN read alive.
5. Do not publish an XAM controller until a valid state report arrives.

## 9. Input decoding and Xbox mapping

### Endianness rule

Never cast the packet buffer to SDL's packed Triton structs. Xbox 360 PowerPC is big-endian and unaligned packed access is risky. Use explicit helpers:

- `ReadLE16`
- `ReadSLE16`
- `ReadLE32`

Each helper must be covered by bytes-to-value tests that would fail if host byte order leaked into the result.

### Validation

For every interrupt completion:

1. Verify the slot is still alive and not being removed.
2. Reject USB-error completions without reading the buffer.
3. Inspect byte 0 only after confirming at least one byte was transferred, if the Xbox API exposes the actual length.
4. Accept state only for IDs `0x42`, `0x45`, or `0x47` and at least 18 bytes.
5. Accept wireless status only for IDs `0x46` or `0x79` and at least 2 bytes.
6. Ignore known non-gameplay reports such as battery `0x43`, keyboard `0x41`, and mouse `0x40`.
7. Rate-limit logging of unknown IDs.
8. Clear the receive buffer before requeueing so a short transfer cannot reuse stale payload bytes.
9. Requeue exactly once unless removal/fatal endpoint shutdown has begun.

### Axis and trigger conversion

- Sticks are already signed 16-bit full-range values.
- Keep Triton Y values as-is for XInput because both use positive-up semantics. SDL negates Y because SDL/evdev uses the opposite convention; that negation should not be copied blindly.
- Clamp each trigger to 0–32767.
- Convert trigger to 0–255 with a rounded integer mapping, e.g. `(raw * 255 + 16383) / 32767`.
- Store the resulting bytes in the existing trigger carriers used by `XInputdReadStateHook` (`rx`/`ry`), or replace those ambiguous fields with explicit trigger bytes in a focused cleanup.
- If analog trigger value is zero but its trigger-click bit is set, expose 255 as a defensive fallback.

### Buttons and D-pad

- Set `has_hat_switch = false`.
- Populate the four D-pad booleans from the button bitfield.
- Map face, shoulder, stick-click, Start/Back, and Steam buttons exactly as listed in the evidence table.
- Do not expose QAM, paddles, trackpads, or capacitive inputs in v1.

### State publication

Avoid publishing a partially updated `ButtonsReport` from the USB callback while XInput is reading it.

Preferred approach:

1. Decode into a local `ButtonsReport`.
2. Write it to the inactive of two state buffers.
3. Issue a memory barrier.
4. Atomically swap the published-buffer index.
5. Have `XInputdReadStateHook` copy from a stable published index, retrying once if the index changes during the copy.

Also clear `pInputData` on the virtual-controller path before setting buttons/axes so stale caller data cannot survive between packets.

## 10. Lizard heartbeat scheduler

Reuse the existing long-lived mapping manager thread as a general driver maintenance tick, or add a dedicated maintenance thread only if separating responsibilities is substantially clearer. A 100 ms tick is sufficient.

Per tick:

1. Consume queued USB connect/disconnect/state events outside interrupt context.
2. Publish/unpublish XAM state as required.
3. For each slot that is connected, probing, or active, check the lizard heartbeat deadline.
4. If due and no control request is pending, submit the 64-byte feature report.
5. On success, set the next deadline to current time + 2000 ms.
6. On failure, log once and retry after 250 ms, then use capped exponential backoff no longer than 2 seconds.
7. Never overlap feature requests on the same slot.
8. Stop scheduling immediately when removal begins.

The 64-byte report buffer must live in the slot context, not on a thread stack, because the USB transfer is asynchronous.

## 11. XAM lifecycle

### Publish

When the first valid state arrives from the selected slot:

1. Find one free `connectedControllers[]` entry.
2. Populate a `CONTROLLER_TRITON_PROTEUS` controller record.
3. Publish the decoded neutral/current state before binding.
4. Call `XamUserBindDeviceCallback` once.
5. Record the returned user index and device context.
6. Do not start the generic mapping assistant.

If no XAM controller slot is free, continue servicing USB and heartbeats, log the condition once, and retry publication from maintenance context when a slot becomes free.

### Wireless disconnect

On status value `1` for the active slot:

1. Atomically publish a neutral state.
2. Stop accepting gameplay state from that slot until a new valid connect/state event.
3. Unbind the one XAM controller exactly once.
4. Clear its `connectedControllers[]` record only after no XInput read can reference freed transport memory.
5. Keep the puck slot's USB IN transfer active for reconnect status.

### USB removal

Route Proteus interfaces in `HidRemoveDeviceHook` before generic controller lookup.

For each removed slot:

1. Mark it removing so callbacks stop requeueing.
2. If active, publish neutral state and unbind once.
3. Close the interrupt endpoint and default/control endpoint using the resolved XDK functions.
4. Wait for or otherwise safely account for any pending callback according to XDK semantics.
5. Free receive/report buffers before zeroing their owning context.
6. Remove the slot from the aggregate.
7. Reset the singleton only after all claimed interfaces are gone.

The existing generic removal bug that zeroes `Controller` before freeing `reportData` should be corrected as a prerequisite/regression fix.

## 12. Error behavior

| Failure | Required behavior |
|---|---|
| Non-Proteus Valve device | Leave to existing/original path |
| Proteus management/non-slot interface | Do not claim |
| Duplicate add callback | Ignore safely; do not allocate twice |
| Missing HID/IN endpoint descriptor | Fail only that slot; no XAM controller |
| `SET_CONFIGURATION` failure | Unwind initialization; no endpoint read/XAM bind |
| Lizard feature request failure | Keep reads alive; retry with bounded backoff |
| Unknown report ID | Ignore and rate-limit log |
| Malformed/truncated state | Ignore, retain last valid state, requeue read |
| Explicit wireless disconnect | Publish neutral, unbind, keep listening |
| Active USB endpoint error | Neutral/unbind active controller; attempt clean slot restart only if XDK semantics permit |
| Second controller appears | Continue transport servicing, log once, never publish a second XAM controller |
| Second puck appears | Reject/leave unclaimed with a clear log; one-puck scope |
| Puck removed during control transfer | Mark removing, suppress requeue, finish safe teardown without use-after-free |

## 13. Targeted hardening required in existing code

These fixes are directly relevant to a four-interface puck and should land before or with Proteus support:

1. Replace global async init scratch state with per-device state.
2. Check that `interruptHandler` found a controller before indexing `connectedControllers[index]`.
3. Validate `reportInfo` before calling `FindGamepadReportId`.
4. Validate endpoint descriptors before dereferencing them.
5. Free `reportData` before clearing the `Controller` record.
6. Give `HidRemoveDeviceHook` a return value on every path.
7. Make control-transfer busy/lifetime state explicit.
8. Prevent callback requeue after `cleanupDone`/removal.
9. Keep report buffers owned until the final callback is quiesced.
10. Ensure XInput output structures are initialized on virtual-controller reads.

These are not a general rewrite. They are the minimum lifecycle work needed to avoid races, leaks, and slot cross-talk during Proteus enumeration/removal.

## 14. File-by-file implementation plan

### `hiddriver/triton_protocol.h` (new)

- Public constants and compact decoded-state/status types.
- Pure decode/build function declarations.
- No Xbox/XDK includes.

### `hiddriver/triton_protocol.cpp` (new)

- Little-endian readers.
- Common-prefix decoder for `0x42`, `0x45`, `0x47`.
- Status decoder for `0x46`, `0x79`.
- Button/axis/trigger conversion.
- Exact 64-byte lizard-off report builder.

### `hiddriver/proteus.h` (new)

- Puck/slot lifecycle API called from `main.cpp` hooks and the maintenance thread.
- Opaque contexts where possible to keep `main.cpp` small.

### `hiddriver/proteus.cpp` (new)

- Singleton puck aggregate and four slot contexts.
- USB init, interrupt callback, control callback, heartbeat scheduling.
- One-controller arbitration and XAM publish/unpublish requests.
- Diagnostics and counters.

### `hiddriver/main.cpp`

- Route Proteus slot interfaces before generic HID handling.
- Route Proteus removal before generic removal.
- Add `ControllerKind` and one published Triton controller record.
- Move existing generic initialization scratch data into per-device state.
- Add typed TRB ownership helpers and lifecycle guards.
- Call Proteus maintenance from the long-lived maintenance thread.
- Use stable state publication for Triton.
- Apply the targeted hardening list.

### `hiddriver/usb.h`

- Add named HID request/report constants if they are not kept private to `proteus.cpp`.
- Keep wire-format helpers out of packed host structs.

### `hiddriver/hiddriver.vcxproj` and `.filters`

- Add the four new source/header files.
- Do not change Xbox toolset `2010-01` or retarget the project.

### `tests/` (new)

- `triton_protocol_tests.cpp`: dependency-free host test executable using assertions.
- `fixtures/`: sanitized real report samples and manifest.
- A small native VS2022 x64 test project, or an equivalent deterministic host build command, that compiles the same `triton_protocol.cpp` used by the XEX.

### `README.md`

- Add “Steam Controller 2026 (Triton) through Proteus puck only.”
- State one-controller/one-puck limitation.
- State pre-pairing requirement.
- State that direct USB, BLE, Nereid, advanced inputs, and rumble are unsupported.
- Add protocol attribution.

## 15. Test plan

### 15.1 Baseline before changes

1. Build current `Release Retail|Xbox 360` unchanged from a VS2022 Developer Prompt with the registered Xbox 360 platform.
2. Record output XEX size and warnings/errors.
3. Smoke-test one existing generic HID controller and Switch Pro or DS3 if available.
4. Save representative debug output for add/input/remove behavior.

### 15.2 Host protocol tests

Cover at minimum:

- Little-endian `u16`, signed `s16`, and `u32` decoding.
- All three state IDs with the same common prefix.
- All supported button bits independently and in combinations.
- D-pad cardinal and diagonal combinations.
- Stick values `-32768`, `-1`, `0`, `1`, `32767`.
- Positive-up Y behavior.
- Trigger values 0, 1, midpoint, 32766, 32767, and invalid negative encodings.
- Trigger-click fallback.
- Unknown, lizard mouse/keyboard, battery, and too-short reports leave state unchanged.
- Both wireless status IDs and both status values.
- Exact 64-byte heartbeat contents.
- Sequence wrap from 255 to 0.
- Recorded current-firmware fixtures.
- Random lengths from 0 through 64 to detect out-of-bounds reads.

### 15.3 State-machine tests

With a fake transport/XAM layer:

- Slot 2 connects and becomes active.
- A controller bonded only to slots 3, 4, or 5 still becomes active.
- Two slots report state nearly simultaneously; exactly one XAM bind occurs.
- Inactive-slot input cannot overwrite active state.
- Active disconnect produces neutral state and exactly one unbind.
- Reconnect produces exactly one new bind.
- Active disconnect followed by another already-connected slot causes deterministic single-slot failover.
- Puck interfaces can be removed in any order without double unbind/free.
- Feature requests never overlap per slot.
- Heartbeat retries stop during removal.

### 15.4 Console hardware matrix

Run on both currently supported environments if available:

- Retail dashboard 17559.
- Devkit dashboard 17489.

Scenarios:

1. Plugin loads first; puck plugs in; controller powers on.
2. Puck is present at plugin load; controller powers on later.
3. Controller is already on when puck/plugin initializes.
4. Controller powers off and reconnects without puck removal.
5. Puck is removed/reinserted repeatedly.
6. Title/dashboard transitions while connected.
7. At least 30 minutes idle plus periodic input to prove the lizard heartbeat remains active.
8. Every standard input and full stick/trigger travel in an XInput test title.
9. D-pad diagonals.
10. Steam/Guide button behavior and existing one-second debounce.
11. A second controller powers on: no second XAM controller appears and active input remains stable.
12. Existing supported USB controllers still enumerate and work before/after a puck session.
13. Different console USB ports and both USB speed paths reported by the existing diagnostics.
14. Twenty connect/disconnect and ten puck replug cycles while watching for leaks, stale controllers, or crashes.

### 15.5 Diagnostics to record

- Puck VID/PID and interface number.
- Endpoint address, maximum packet size, and interval.
- Slot initialization transitions.
- Feature transfer success/failure and retry count, not full spam every 2 seconds.
- Wireless status transitions.
- First accepted state report ID per session.
- Active-slot selection/failover.
- Ignored additional-controller event.
- Teardown order and outstanding transfer counts.

Never log full pairing/management payloads or persistent identifiers by default.

## 16. Acceptance criteria

The work is complete only when all of these hold:

1. A pre-paired Triton connected through a real `28DE:1304` Proteus puck works in the Xbox 360 dashboard and games as one XInput gamepad.
2. It works regardless of which Proteus bond-slot interface (2–5) carries the controller.
3. Exactly one virtual controller is registered for the puck.
4. No virtual controller is registered merely because an empty puck is inserted.
5. A/B/X/Y, D-pad, LB/RB, LT/RT, L3/R3, both sticks, Start, Back, and Steam/Guide behave correctly.
6. Stick axes reach expected signed extremes and are not byte-swapped or Y-inverted incorrectly.
7. Triggers reach 0 and 255 monotonically.
8. Lizard keyboard/mouse reports do not drive the virtual gamepad, and raw state continues after at least 30 minutes.
9. Power-off/reconnect and puck replug recover without reloading the plugin.
10. A second simultaneous Triton does not create another XAM controller or corrupt the first controller's state.
11. Malformed/unknown reports cannot read out of bounds, mutate state, or stop the IN requeue loop.
12. Repeated removal does not leak the report buffer or cause use-after-free/double-unbind behavior.
13. Existing DS3, Switch Pro, generic HID mapping, and static mappings show no regression.
14. The XEX builds with the existing Xbox 360 `2010-01` toolset and project settings.
15. README limitations and setup are accurate.

## 17. Implementation order and review checkpoints

### Phase 0 — Evidence and baseline

- Produce sanitized captures and fixture manifest.
- Confirm topology, feature request, report IDs, sizes, and firmware versions.
- Build and smoke-test the unchanged driver.
- **Checkpoint:** update protocol assumptions in this plan before code if the capture disagrees.

### Phase 1 — Pure protocol module

- Implement little-endian decoder and heartbeat builder.
- Add host tests and captured fixtures.
- No Xbox USB or XAM changes yet.
- **Checkpoint:** all host tests pass, including malformed-length cases.

### Phase 2 — Lifecycle prerequisite refactor

- Move global init scratch state to per-interface ownership.
- Add typed TRB recovery helpers and targeted safety fixes.
- Keep Proteus disabled.
- **Checkpoint:** XEX build plus regression tests with existing controllers and repeated removal.

### Phase 3 — Proteus transport, no XAM publication

- Claim interfaces 2–5.
- Initialize endpoints, read reports, send heartbeat, and log sanitized decoded state/status.
- Do not register a virtual controller yet.
- **Checkpoint:** console logs match PC captures; heartbeat survives idle and title transitions.

### Phase 4 — Single-controller publication

- Add arbitration, stable state publication, XAM bind/unbind, and XInput mapping.
- Ignore additional simultaneous controllers.
- **Checkpoint:** full standard-input validation and connect/disconnect matrix.

### Phase 5 — Hardening and documentation

- Removal stress, error injection where possible, rate-limited logs, regression matrix.
- README update and final protocol attribution.
- **Checkpoint:** every acceptance criterion checked with evidence.

Keep phases as separate commits where practical so the USB lifecycle refactor, protocol decoder, and final XAM behavior can be reviewed or reverted independently.

## 18. Main risks and mitigations

| Risk | Mitigation |
|---|---|
| Current firmware changes report tail/layout | Decode only the stable 18-byte common prefix and accept `0x42`/`0x45`/`0x47` |
| PowerPC endianness corrupts values | Bytewise LE helpers and fixture tests; no packed multibyte casts |
| Four interfaces race global init | Per-interface init state and serialized puck configuration/control |
| Four slots create four XAM controllers | Separate transport slots from one published controller; explicit arbitration |
| Lizard watchdog restores KB/mouse mode | Persistent 2-second heartbeat with busy guard and retry |
| Controller is bonded outside slot 2 | Listen/probe all interfaces 2–5 |
| USB removal races callbacks | Removing flag, no requeue, endpoint/control quiescence, delayed free |
| Firmware sends short or unexpected reports | Length/ID validation, cleared buffers, retain last valid state |
| Lack of executable tests on Xbox build host | Pure portable protocol module compiled into both host tests and XEX |
| Existing generic paths regress during init refactor | Land prerequisite separately and test existing devices before enabling Proteus |
| Multiple connected controllers | Deterministic first/lowest-slot winner; discard others; one bind maximum |

## 19. Protocol sources

Primary implementation references:

- [SDL Triton HIDAPI driver](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_steam_triton.c)
- [SDL controller wire structures](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/steam/controller_structs.h)
- [SDL controller constants](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/steam/controller_constants.h)
- [SDL Valve controller VID/PID list](https://github.com/libsdl-org/SDL/blob/main/src/joystick/controller_list.h)

Supplementary reverse-engineering/capture references, to be treated as corroboration rather than a substitute for the project's own real-device capture:

- [OpenPuck protocol notes](https://github.com/safijari/openpuck/blob/main/docs/PROTOCOL.md)
- [Steam Controller 2026 protocol notes](https://github.com/iczero/steam-controller-stuff)
- [Current-firmware report-ID/quaternion behavior report](https://github.com/ValveSoftware/steam-for-linux/issues/13255)

If source definitions are copied rather than independently re-expressed, retain the applicable SDL zlib license/attribution in the new protocol files.
