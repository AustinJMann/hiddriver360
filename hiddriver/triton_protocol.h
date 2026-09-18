#pragma once

#include <stddef.h>
#include <stdint.h>

namespace TritonProtocol {

static const uint16_t kValveVendorId = 0x28DE;
static const uint16_t kProteusProductId = 0x1304;
static const uint8_t kFirstSlotInterface = 2;
static const uint8_t kLastSlotInterface = 5;
static const size_t kInputPrefixSize = 18;
static const size_t kFeatureReportSize = 64;

enum WirelessStatus {
	kWirelessStatusUnknown = 0,
	kWirelessDisconnected = 1,
	kWirelessConnected = 2
};

struct InputState {
	uint8_t reportId;
	uint8_t sequence;
	uint32_t buttons;
	uint16_t leftTrigger;
	uint16_t rightTrigger;
	int16_t leftX;
	int16_t leftY;
	int16_t rightX;
	int16_t rightY;
};

#pragma pack(push, 1)
struct ControllerState {
	int16_t leftX;
	int16_t leftY;
	int16_t rightX;
	int16_t rightY;
	uint8_t leftTrigger;
	uint8_t rightTrigger;
	uint8_t a;
	uint8_t b;
	uint8_t x;
	uint8_t y;
	uint8_t dpadLeft;
	uint8_t dpadRight;
	uint8_t dpadUp;
	uint8_t dpadDown;
	uint8_t rightStick;
	uint8_t leftStick;
	uint8_t menu;
	uint8_t view;
	uint8_t rightTriggerClick;
	uint8_t leftTriggerClick;
	uint8_t rightShoulder;
	uint8_t leftShoulder;
	uint8_t guide;
};
#pragma pack(pop)

uint16_t ReadLE16(const uint8_t* bytes);
int16_t ReadSLE16(const uint8_t* bytes);
uint32_t ReadLE32(const uint8_t* bytes);

bool IsProteusSlotInterface(uint16_t vendorId, uint16_t productId,
	uint8_t interfaceNumber, uint8_t interfaceClass,
	uint8_t interfaceSubClass, uint8_t interfaceProtocol);
bool DecodeInputPrefix(const uint8_t* bytes, size_t length, InputState* state);
bool DecodeWirelessStatus(const uint8_t* bytes, size_t length, WirelessStatus* status);
void ConvertToControllerState(const InputState& state, ControllerState* stateOut);
void BuildLizardOffFeatureReport(uint8_t report[kFeatureReportSize]);

} // namespace TritonProtocol
