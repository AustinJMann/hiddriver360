#include <assert.h>
#include <string.h>

#include "../hiddriver/triton_protocol.h"

using namespace TritonProtocol;

static void Put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void Put32(uint8_t* p, uint32_t v) {
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void TestEndianAndValidation() {
	const uint8_t bytes[] = { 0x34, 0x12, 0x78, 0x56 };
	assert(ReadLE16(bytes) == 0x1234);
	assert(ReadSLE16((const uint8_t*)"\xff\xff") == -1);
	assert(ReadLE32(bytes) == 0x56781234);
	InputState unchanged = {};
	unchanged.sequence = 77;
	for (size_t length = 0; length < kInputPrefixSize; ++length) {
		uint8_t packet[64] = {};
		packet[0] = 0x42;
		assert(!DecodeInputPrefix(packet, length, &unchanged));
		assert(unchanged.sequence == 77);
	}
	assert(!DecodeInputPrefix(0, 18, &unchanged));
}

static void TestStateIdsAndAxes() {
	const uint8_t ids[] = { 0x42, 0x45, 0x47 };
	for (size_t i = 0; i < sizeof(ids); ++i) {
		uint8_t packet[18] = {};
		packet[0] = ids[i]; packet[1] = 255;
		Put16(packet + 6, 32767); Put16(packet + 8, 0x8000);
		Put16(packet + 10, 0x8000); Put16(packet + 12, 0xffff);
		Put16(packet + 14, 1); Put16(packet + 16, 32767);
		InputState state = {};
		assert(DecodeInputPrefix(packet, sizeof(packet), &state));
		assert(state.reportId == ids[i] && state.sequence == 255);
		assert(state.leftTrigger == 32767 && state.rightTrigger == 0);
		assert(state.leftX == -32768 && state.leftY == -1);
		assert(state.rightX == 1 && state.rightY == 32767);
	}
	uint8_t unknown[18] = {};
	unknown[0] = 0x43;
	InputState state = {};
	assert(!DecodeInputPrefix(unknown, sizeof(unknown), &state));
}

static void TestButtonsAndTriggers() {
	uint8_t packet[18] = {};
	packet[0] = 0x45;
	Put32(packet + 2, 0x00000001 | 0x00000002 | 0x00000004 | 0x00000008 |
		0x00000020 | 0x00000040 | 0x00000200 | 0x00000400 | 0x00000800 |
		0x00001000 | 0x00002000 | 0x00004000 | 0x00008000 | 0x00010000 | 0x00080000);
	Put16(packet + 6, 1); Put16(packet + 8, 32767);
	InputState state = {};
	assert(DecodeInputPrefix(packet, sizeof(packet), &state));
	ButtonsReport b;
	ConvertToButtonsReport(state, &b);
	assert(b.a_button && b.b_button && b.x_button && b.y_button);
	assert(b.l1 && b.r1 && b.l3 && b.r3 && b.start && b.back && b.xbox);
	assert(b.dpad_up && b.dpad_down && b.dpad_left && b.dpad_right);
	assert(b.rx == 0 && b.ry == 255);
	packet[2] = packet[3] = packet[4] = packet[5] = 0;
	Put32(packet + 2, 0x08000000 | 0x00800000);
	Put16(packet + 6, 0); Put16(packet + 8, 0);
	assert(DecodeInputPrefix(packet, sizeof(packet), &state));
	ConvertToButtonsReport(state, &b);
	assert(b.l2 && b.r2 && b.rx == 0 && b.ry == 0);
}

static void TestStatusAndFeature() {
	const uint8_t ids[] = { 0x46, 0x79 };
	for (size_t i = 0; i < sizeof(ids); ++i) {
		for (uint8_t value = 1; value <= 2; ++value) {
			uint8_t packet[] = { ids[i], value };
			WirelessStatus status = kWirelessStatusUnknown;
			assert(DecodeWirelessStatus(packet, sizeof(packet), &status));
			assert(status == (WirelessStatus)value);
		}
	}
	uint8_t report[64]; memset(report, 0xcc, sizeof(report));
	BuildLizardOffFeatureReport(report);
	assert(report[0] == 1 && report[1] == 0x87 && report[2] == 3 && report[3] == 9);
	for (size_t i = 4; i < sizeof(report); ++i) assert(report[i] == 0);
}

int main() {
	TestEndianAndValidation();
	TestStateIdsAndAxes();
	TestButtonsAndTriggers();
	TestStatusAndFeature();
	return 0;
}
