# Experimental rumble validation

This implementation is ready for console testing, not hardware-certified.
Host tests verify encoding and scheduling policy; they do not establish that
Proteus firmware accepts this transport or that Xbox titles reach the hook.

## Implementation

- The XAM ordinal 402 hook treats its third argument as `XINPUT_VIBRATION*`.
  It preserves the original result for native controllers and handles the
  disconnected fallback for a valid Triton binding. Capabilities advertise
  force feedback and both 16-bit motor ranges.
- Xbox left/right strengths become Triton low/high-frequency strengths. The
  report is 10 bytes: `80 00 00 00 LL LH 00 RL RH 00`. Motor words are little
  endian; report type, intensity, and gains use the SDL defaults.
- USB uses class/interface `SET_REPORT`: request type `0x21`, request `0x09`,
  value `0x0280`, index equal to Proteus interface 2 through 5, length 10.
  This first implementation uses endpoint zero; interrupt-OUT is not implemented.
- Each slot has a generation-tagged atomic mailbox. Repeated requests replace
  pending strengths. The USB buffer remains unchanged until completion. Failed
  sends retry with backoff from 10 ms to 250 ms; explicit stops also retry.
- Active output is due every 30 ms, with a USB worker waking every 5 ms on
  hardware thread 2 at IRQL 2. Completions can dispatch the next waiting slot.
  Lizard-mode heartbeats retain priority. XAM binding runs separately at normal
  IRQL. These are scheduling targets, not measured latency guarantees.
- Disconnect and unbind disable the mailbox. Rebinding starts with zero strengths.
  Already submitted USB transfers cannot be recalled by replacing the mailbox;
  physical removal still uses the existing USB cancellation/quarantine path.

The packet and refresh approach follows the
[SDL Triton driver](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_steam_triton.c)
and its [report structures](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/steam/controller_structs.h).
The control transfer follows the fallback in
[HIDAPI's USB transport](https://github.com/libsdl-org/SDL/blob/main/src/hidapi/libusb/hid.c).
The three-argument XAM signature is supported by
[Xenia's implementation](https://github.com/xenia-project/xenia/blob/master/src/xenia/kernel/xam/xam_input.cc);
it still needs confirmation on the supported console dashboards.

## Console acceptance

Use a test title that calls `XInputSetState`, or a game with independently
observable rumble events. Record dashboard, Triton and Proteus firmware versions,
slot/interface, and completion status with each result.

1. With one controller, request left-only, right-only, and both motors at modest
   strengths. Verify the channels independently, then test the full range.
2. Make one nonzero request and leave it unchanged for at least five seconds.
   Rumble should remain continuous. Send both strengths as zero and check that
   it stops promptly. Also check a short pulse and rapidly changing strengths.
3. Power off and reconnect during rumble. The controller must stay stopped until
   a fresh game request arrives. Repeat with slot/user rebinding.
4. Unplug and reconnect the puck during rumble. Confirm input and output recover
   without a console hang or stale effects.
5. Repeat with two through four controllers using distinct effects. Check channel
   isolation and sustained output across multiple lizard-mode heartbeats. Measure
   report gaps: the firmware timeout reported by SDL is roughly 50 ms.
6. Confirm native Xbox controllers retain their original rumble behavior. Test
   game pause, Guide overlays, vibration-disabled settings, and title changes.

`rumble output accepted` means the USB request completed successfully; it does
not prove that motors moved. If requests fail or complete without vibration,
capture the puck configuration/report descriptors and a known-working SDL rumble
transaction to verify the output route, report length, and report-ID framing.
If the firmware requires interrupt-OUT, add descriptor discovery and a separate
output TRB before calling this transport supported. Do not repeatedly change
unrelated feature-report settings to troubleshoot rumble.

Store sanitized captures with the metadata required by
[the fixture manifest](../tests/fixtures/README.md). Synthetic test vectors are
kept in the host tests and are not hardware captures.
