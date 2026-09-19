#pragma once

#include <stdint.h>
#include <string.h>

namespace ControllerCapabilities {

inline bool AcceptsGamepad(uint32_t flags) {
	return !(flags & 0xff) || (flags & 1) != 0;
}

inline bool AnyUser(uint32_t user, uint32_t flags) {
	return user == 0xff || user == 0xffffffffu || (flags & 0x40000000u) != 0;
}

// Works with the SDK base structure and the driver's extended structure.
// Capabilities describe supported controls, never the latest input sample.
template <typename Capabilities>
void Fill(Capabilities* output) {
	memset(output, 0, sizeof(*output));
	output->Type = 1;
	output->SubType = 1;
	output->Gamepad.wButtons = 0xf3ff; // All standard buttons; exclude reserved bits.
	output->Gamepad.bLeftTrigger = 255;
	output->Gamepad.bRightTrigger = 255;
	output->Gamepad.sThumbLX = 32767;
	output->Gamepad.sThumbLY = 32767;
	output->Gamepad.sThumbRX = 32767;
	output->Gamepad.sThumbRY = 32767;
	// XINPUT_CAPS_FFB_SUPPORTED | XINPUT_CAPS_WIRELESS on Xbox 360.
	output->Flags = 0x0003;
	output->Vibration.wLeftMotorSpeed = 0xffff;
	output->Vibration.wRightMotorSpeed = 0xffff;
}

} // namespace ControllerCapabilities
