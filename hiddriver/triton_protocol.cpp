#include "triton_protocol.h"

#include <string.h>

namespace TritonProtocol {

namespace {

static const uint8_t kStateReport42 = 0x42;
static const uint8_t kStateReport45 = 0x45;
static const uint8_t kStateReport47 = 0x47;
static const uint8_t kStatusReport46 = 0x46;
static const uint8_t kStatusReport79 = 0x79;

static const uint32_t kButtonA = 0x00000001;
static const uint32_t kButtonB = 0x00000002;
static const uint32_t kButtonX = 0x00000004;
static const uint32_t kButtonY = 0x00000008;
static const uint32_t kButtonR3 = 0x00000020;
static const uint32_t kButtonView = 0x00000040;
static const uint32_t kButtonRB = 0x00000200;
static const uint32_t kDpadDown = 0x00000400;
static const uint32_t kDpadRight = 0x00000800;
static const uint32_t kDpadLeft = 0x00001000;
static const uint32_t kDpadUp = 0x00002000;
static const uint32_t kButtonMenu = 0x00004000;
static const uint32_t kButtonL3 = 0x00008000;
static const uint32_t kButtonSteam = 0x00010000;
static const uint32_t kButtonLB = 0x00080000;
static const uint32_t kButtonRTClick = 0x00800000;
static const uint32_t kButtonLTClick = 0x08000000;

static uint16_t ClampTrigger(int16_t value) {
	if (value <= 0) return 0;
	if (value >= 32767) return 32767;
	return (uint16_t)value;
}

static uint8_t ScaleTrigger(uint16_t value) {
	return (uint8_t)(((uint32_t)value * 255U + 16383U) / 32767U);
}

} // namespace

uint16_t ReadLE16(const uint8_t* bytes) {
	return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

int16_t ReadSLE16(const uint8_t* bytes) {
	return (int16_t)ReadLE16(bytes);
}

uint32_t ReadLE32(const uint8_t* bytes) {
	return (uint32_t)bytes[0] |
		((uint32_t)bytes[1] << 8) |
		((uint32_t)bytes[2] << 16) |
		((uint32_t)bytes[3] << 24);
}

bool IsProteusSlotInterface(uint16_t vendorId, uint16_t productId,
	uint8_t interfaceNumber, uint8_t interfaceClass,
	uint8_t interfaceSubClass, uint8_t interfaceProtocol) {
	return vendorId == kValveVendorId && productId == kProteusProductId &&
		interfaceNumber >= kFirstSlotInterface && interfaceNumber <= kLastSlotInterface &&
		interfaceClass == 0x03 && interfaceSubClass == 0 && interfaceProtocol == 0;
}

bool DecodeInputPrefix(const uint8_t* bytes, size_t length, InputState* state) {
	if (!bytes || !state || length < kInputPrefixSize)
		return false;
	if (bytes[0] != kStateReport42 && bytes[0] != kStateReport45 && bytes[0] != kStateReport47)
		return false;

	InputState decoded;
	decoded.reportId = bytes[0];
	decoded.sequence = bytes[1];
	decoded.buttons = ReadLE32(bytes + 2);
	decoded.leftTrigger = ClampTrigger(ReadSLE16(bytes + 6));
	decoded.rightTrigger = ClampTrigger(ReadSLE16(bytes + 8));
	decoded.leftX = ReadSLE16(bytes + 10);
	decoded.leftY = ReadSLE16(bytes + 12);
	decoded.rightX = ReadSLE16(bytes + 14);
	decoded.rightY = ReadSLE16(bytes + 16);
	*state = decoded;
	return true;
}

bool DecodeWirelessStatus(const uint8_t* bytes, size_t length, WirelessStatus* status) {
	if (!bytes || !status || length < 2)
		return false;
	if (bytes[0] != kStatusReport46 && bytes[0] != kStatusReport79)
		return false;
	if (bytes[1] != kWirelessDisconnected && bytes[1] != kWirelessConnected)
		return false;
	*status = (WirelessStatus)bytes[1];
	return true;
}

void ConvertToControllerState(const InputState& state, ControllerState* report) {
	if (!report) return;
	memset(report, 0, sizeof(*report));
	const uint32_t b = state.buttons;
	report->a = (b & kButtonA) != 0;
	report->b = (b & kButtonB) != 0;
	report->x = (b & kButtonX) != 0;
	report->y = (b & kButtonY) != 0;
	report->rightStick = (b & kButtonR3) != 0;
	report->view = (b & kButtonView) != 0;
	report->rightShoulder = (b & kButtonRB) != 0;
	report->dpadDown = (b & kDpadDown) != 0;
	report->dpadRight = (b & kDpadRight) != 0;
	report->dpadLeft = (b & kDpadLeft) != 0;
	report->dpadUp = (b & kDpadUp) != 0;
	report->menu = (b & kButtonMenu) != 0;
	report->leftStick = (b & kButtonL3) != 0;
	report->guide = (b & kButtonSteam) != 0;
	report->leftShoulder = (b & kButtonLB) != 0;
	report->leftX = state.leftX;
	report->leftY = state.leftY;
	report->rightX = state.rightX;
	report->rightY = state.rightY;
	report->leftTrigger = ScaleTrigger(state.leftTrigger);
	report->rightTrigger = ScaleTrigger(state.rightTrigger);
	report->leftTriggerClick = state.leftTrigger == 0 && (b & kButtonLTClick) != 0;
	report->rightTriggerClick = state.rightTrigger == 0 && (b & kButtonRTClick) != 0;
}

void BuildLizardOffFeatureReport(uint8_t report[kFeatureReportSize]) {
	memset(report, 0, kFeatureReportSize);
	report[0] = 0x01;
	report[1] = 0x87;
	report[2] = 0x03;
	report[3] = 0x09;
}

void BuildRumbleOutputReport(uint16_t left, uint16_t right,
	uint8_t report[kRumbleReportSize]) {
	// SDL's Triton OutputReportMsg: type/intensity/gains use firmware defaults.
	// Encode explicitly: the Xbox CPU is big endian, the HID report is not.
	memset(report, 0, kRumbleReportSize);
	report[0] = 0x80;
	report[4] = (uint8_t)left;
	report[5] = (uint8_t)(left >> 8);
	report[7] = (uint8_t)right;
	report[8] = (uint8_t)(right >> 8);
}

} // namespace TritonProtocol
