// These checks must run even when the host project defines NDEBUG.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>
#include <Windows.h>
#include <Xinput.h>
#include "../hiddriver/controller_capabilities.h"

#include "../hiddriver/triton_protocol.h"
#include "../hiddriver/proteus_routing.h"
#include "../hiddriver/usb_descriptors.h"

using namespace TritonProtocol;

struct SimSlot {
	bool connected;
	bool disconnectPending;
	int controllerIndex;
	uint32_t generation;
	uint32_t state;
};

struct SimController {
	bool occupied;
	int slotIndex;
	uint32_t generation;
	uint32_t packetNumber;
};

struct RoutingSimulation {
	SimSlot slots[ProteusRouting::kSlotCount];
	SimController controllers[ProteusRouting::kControllerCount];
	bool rejectBind[ProteusRouting::kSlotCount];

	RoutingSimulation() { Reset(); }

	void Reset() {
		memset(this, 0, sizeof(*this));
		for (int i = 0; i < ProteusRouting::kSlotCount; ++i) {
			slots[i].controllerIndex = ProteusRouting::kUnboundController;
			slots[i].generation = 1;
			controllers[i].slotIndex = -1;
		}
	}

	void Connect(int slotIndex, uint32_t state) {
		slots[slotIndex].state = state;
		slots[slotIndex].connected = true;
	}

	void Disconnect(int slotIndex) {
		slots[slotIndex].connected = false;
		slots[slotIndex].disconnectPending = true;
	}

	void Process() {
		for (int slotIndex = 0; slotIndex < ProteusRouting::kSlotCount; ++slotIndex) {
			SimSlot& slot = slots[slotIndex];
			bool disconnectPending = slot.disconnectPending;
			slot.disconnectPending = false;
			if ((disconnectPending || !slot.connected) && slot.controllerIndex >= 0) {
				SimController& controller = controllers[slot.controllerIndex];
				controller.occupied = false;
				controller.slotIndex = -1;
				slot.controllerIndex = ProteusRouting::kUnboundController;
				++slot.generation;
			}
		}
		for (int slotIndex = 0; slotIndex < ProteusRouting::kSlotCount; ++slotIndex) {
			SimSlot& slot = slots[slotIndex];
			if (!slot.connected || slot.controllerIndex >= 0) continue;
			int freeController = -1;
			for (int i = 0; i < ProteusRouting::kControllerCount; ++i) {
				if (!controllers[i].occupied) { freeController = i; break; }
			}
			if (freeController < 0) continue;
			SimController& controller = controllers[freeController];
			controller.occupied = true;
			controller.slotIndex = slotIndex;
			controller.generation = ++slot.generation;
			slot.controllerIndex = freeController;
			if (rejectBind[slotIndex]) {
				controller.occupied = false;
				controller.slotIndex = -1;
				slot.controllerIndex = ProteusRouting::kUnboundController;
				++slot.generation;
			}
		}
	}

	bool Read(int controllerIndex, uint32_t* state) {
		SimController& controller = controllers[controllerIndex];
		if (controller.slotIndex < 0) return false;
		SimSlot& slot = slots[controller.slotIndex];
		if (!ProteusRouting::AssociationMatches(slot.connected,
			slot.controllerIndex, slot.generation, controller.occupied,
			controller.slotIndex, controller.generation, controllerIndex))
			return false;
		*state = slot.state;
		++controller.packetNumber;
		return true;
	}
};

static void Put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void Put32(uint8_t* p, uint32_t v) {
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void TestUsbDescriptors() {
	const uint8_t configuration[] = {
		9, 2, 57, 0, 2, 1, 0, 0x80, 50,
		9, 4, 2, 0, 1, 3, 0, 0, 0,
		7, 5, 0x82, 3, 64, 0, 1,
		9, 4, 2, 1, 1, 3, 0, 0, 0,
		7, 5, 0x86, 3, 32, 0, 2,
		9, 4, 3, 0, 1, 3, 0, 0, 0,
		7, 5, 0x83, 3, 64, 0, 1
	};
	usb_endpoint_descriptor endpoint = {};
	assert(UsbDescriptors::FindInterruptInEndpoint(configuration, sizeof(configuration), 2, &endpoint));
	assert(endpoint.bEndpointAddress == 0x82);
	assert(ReadLE16((const uint8_t*)&endpoint.wMaxPacketSize) == 64);
	assert(endpoint.bInterval == 1);
	assert(UsbDescriptors::FindInterruptInEndpoint(configuration, sizeof(configuration), 3, &endpoint));
	assert(endpoint.bEndpointAddress == 0x83);
	assert(!UsbDescriptors::FindInterruptInEndpoint(configuration, sizeof(configuration), 4, &endpoint));
	assert(endpoint.bEndpointAddress == 0x83);
	assert(!UsbDescriptors::FindInterruptInEndpoint(0, sizeof(configuration), 2, &endpoint));
	assert(!UsbDescriptors::FindInterruptInEndpoint(configuration, sizeof(configuration), 2, 0));
	for (size_t length = 0; length < sizeof(configuration); ++length) {
		assert(!UsbDescriptors::FindInterruptInEndpoint(configuration, length, 2, &endpoint));
		assert(endpoint.bEndpointAddress == 0x83);
	}

	uint8_t malformed[sizeof(configuration)];
	// Invalid lengths at each descriptor boundary, including after a match.
	const size_t offsets[] = { 9, 18, 25, 34, 41, 50 };
	const uint8_t lengths[] = { 0, 1, 6, 255 };
	for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
		for (size_t j = 0; j < sizeof(lengths); ++j) {
			memcpy(malformed, configuration, sizeof(configuration));
			malformed[offsets[i]] = lengths[j];
			assert(!UsbDescriptors::FindInterruptInEndpoint(malformed, sizeof(malformed), 2, &endpoint));
			assert(endpoint.bEndpointAddress == 0x83);
		}
	}
	memcpy(malformed, configuration, sizeof(configuration));
	malformed[0] = 8;
	assert(!UsbDescriptors::FindInterruptInEndpoint(malformed, sizeof(malformed), 2, &endpoint));
	memcpy(malformed, configuration, sizeof(configuration));
	malformed[1] = 1;
	assert(!UsbDescriptors::FindInterruptInEndpoint(malformed, sizeof(malformed), 2, &endpoint));
	memcpy(malformed, configuration, sizeof(configuration));
	malformed[2] = 8;
	assert(!UsbDescriptors::FindInterruptInEndpoint(malformed, sizeof(malformed), 2, &endpoint));
	memcpy(malformed, configuration, sizeof(configuration));
	malformed[2] = 26; // A dangling byte after the first complete endpoint.
	assert(!UsbDescriptors::FindInterruptInEndpoint(malformed, sizeof(malformed), 2, &endpoint));

	// An endpoint must belong to the default HID interface and be interrupt-IN.
	const size_t invalidOffsets[] = { 12, 14, 15, 16, 20, 20, 21 };
	const uint8_t invalidValues[] = { 1, 2, 1, 1, 0x02, 0x80, 2 };
	for (size_t i = 0; i < sizeof(invalidOffsets) / sizeof(invalidOffsets[0]); ++i) {
		memcpy(malformed, configuration, sizeof(configuration));
		malformed[invalidOffsets[i]] = invalidValues[i];
		assert(!UsbDescriptors::FindInterruptInEndpoint(malformed, sizeof(malformed), 2, &endpoint));
		assert(endpoint.bEndpointAddress == 0x83);
	}
	endpoint.bDescriptorType = 4;
	assert(!UsbDescriptors::IsInterruptInEndpoint(endpoint));
	endpoint.bDescriptorType = 5;
	endpoint.bLength = 6;
	assert(!UsbDescriptors::IsInterruptInEndpoint(endpoint));
}

static void TestAdmissionAndValidation() {
	for (uint8_t interfaceNumber = kFirstSlotInterface;
		interfaceNumber <= kLastSlotInterface; ++interfaceNumber)
		assert(IsProteusSlotInterface(kValveVendorId, kProteusProductId,
			interfaceNumber, 3, 0, 0));
	assert(!IsProteusSlotInterface(kValveVendorId, kProteusProductId, 1, 3, 0, 0));
	assert(!IsProteusSlotInterface(kValveVendorId, kProteusProductId, 6, 3, 0, 0));
	assert(!IsProteusSlotInterface(0x1234, kProteusProductId, 2, 3, 0, 0));
	assert(!IsProteusSlotInterface(kValveVendorId, 0x5678, 2, 3, 0, 0));
	assert(!IsProteusSlotInterface(kValveVendorId, kProteusProductId, 2, 2, 0, 0));
	assert(!IsProteusSlotInterface(kValveVendorId, kProteusProductId, 2, 3, 1, 0));
	assert(!IsProteusSlotInterface(kValveVendorId, kProteusProductId, 2, 3, 0, 1));
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
	ControllerState b;
	ConvertToControllerState(state, &b);
	assert(b.a && b.b && b.x && b.y);
	assert(b.leftShoulder && b.rightShoulder && b.leftStick && b.rightStick &&
		b.menu && b.view && b.guide);
	assert(b.dpadUp && b.dpadDown && b.dpadLeft && b.dpadRight);
	assert(b.leftTrigger == 0 && b.rightTrigger == 255);
	packet[2] = packet[3] = packet[4] = packet[5] = 0;
	Put32(packet + 2, 0x08000000 | 0x00800000);
	Put16(packet + 6, 0); Put16(packet + 8, 0);
	assert(DecodeInputPrefix(packet, sizeof(packet), &state));
	ConvertToControllerState(state, &b);
	assert(b.leftTriggerClick && b.rightTriggerClick &&
		b.leftTrigger == 0 && b.rightTrigger == 0);
	Put16(packet + 6, 1); Put16(packet + 8, 16384);
	assert(DecodeInputPrefix(packet, sizeof(packet), &state));
	ConvertToControllerState(state, &b);
	assert(b.leftTrigger == 0);
	assert(b.rightTrigger == 128);
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
	WirelessStatus unchanged = kWirelessConnected;
	const uint8_t truncated[] = { 0x46 };
	const uint8_t invalidId[] = { 0x48, 1 };
	const uint8_t invalidValue[] = { 0x46, 3 };
	assert(!DecodeWirelessStatus(truncated, sizeof(truncated), &unchanged));
	assert(!DecodeWirelessStatus(invalidId, sizeof(invalidId), &unchanged));
	assert(!DecodeWirelessStatus(invalidValue, sizeof(invalidValue), &unchanged));
	assert(unchanged == kWirelessConnected);
	uint8_t report[64]; memset(report, 0xcc, sizeof(report));
	BuildLizardOffFeatureReport(report);
	assert(report[0] == 1 && report[1] == 0x87 && report[2] == 3 && report[3] == 9);
	for (size_t i = 4; i < sizeof(report); ++i) assert(report[i] == 0);
}

static void TestRoutingConnectionOrders() {
	int order[4];
	int permutations = 0;
	for (order[0] = 0; order[0] < 4; ++order[0])
		for (order[1] = 0; order[1] < 4; ++order[1])
			for (order[2] = 0; order[2] < 4; ++order[2])
				for (order[3] = 0; order[3] < 4; ++order[3]) {
					bool seen[4] = {};
					bool unique = true;
					for (int i = 0; i < 4; ++i) {
						if (seen[order[i]]) unique = false;
						seen[order[i]] = true;
					}
					if (!unique) continue;
					++permutations;
					RoutingSimulation simulation;
					for (int i = 0; i < 4; ++i) {
						simulation.Connect(order[i], (uint32_t)(100 + order[i]));
						simulation.Process();
					}
					bool controllerSeen[4] = {};
					for (int slot = 0; slot < 4; ++slot) {
						int controller = simulation.slots[slot].controllerIndex;
						assert(controller >= 0 && controller < 4);
						assert(!controllerSeen[controller]);
						controllerSeen[controller] = true;
						uint32_t state = 0;
						assert(simulation.Read(controller, &state));
						assert(state == (uint32_t)(100 + slot));
					}
				}
	assert(permutations == 24);
}

static void TestRoutingDisconnectRetryAndGeneration() {
	RoutingSimulation simulation;
	for (int i = 0; i < 4; ++i) simulation.controllers[i].occupied = true;
	simulation.Connect(0, 11);
	simulation.Process();
	assert(simulation.slots[0].controllerIndex == -1);
	simulation.controllers[2].occupied = false;
	simulation.Process();
	assert(simulation.slots[0].controllerIndex == 2);
	uint32_t oldGeneration = simulation.controllers[2].generation;
	simulation.Disconnect(0);
	uint32_t state = 99;
	assert(!simulation.Read(2, &state));
	simulation.Process();
	assert(!simulation.controllers[2].occupied);
	simulation.Connect(0, 22);
	simulation.Process();
	int rebound = simulation.slots[0].controllerIndex;
	assert(rebound >= 0);
	assert(simulation.controllers[rebound].generation != oldGeneration);
	assert(simulation.Read(rebound, &state) && state == 22);
	assert(simulation.controllers[rebound].packetNumber == 1);
	oldGeneration = simulation.controllers[rebound].generation;
	simulation.Disconnect(0);
	simulation.Connect(0, 33);
	simulation.Process();
	rebound = simulation.slots[0].controllerIndex;
	assert(rebound >= 0);
	assert(simulation.controllers[rebound].generation != oldGeneration);
	assert(simulation.Read(rebound, &state) && state == 33);
}

static void TestRoutingFailuresAndIsolation() {
	assert(ProteusRouting::IsValidXamBinding(0, 0));
	assert(ProteusRouting::IsValidXamBinding(0, 3));
	assert(!ProteusRouting::IsValidXamBinding(-1, 0));
	assert(!ProteusRouting::IsValidXamBinding(0, 4));
	RoutingSimulation simulation;
	simulation.rejectBind[1] = true;
	simulation.Connect(0, 10);
	simulation.Connect(1, 20);
	simulation.Process();
	assert(simulation.slots[0].controllerIndex >= 0);
	assert(simulation.slots[1].controllerIndex == -1);
	simulation.rejectBind[1] = false;
	simulation.Process();
	int first = simulation.slots[0].controllerIndex;
	int second = simulation.slots[1].controllerIndex;
	assert(first != second);
	simulation.Connect(0, 30);
	uint32_t state = 0;
	assert(simulation.Read(first, &state) && state == 30);
	assert(simulation.Read(second, &state) && state == 20);
	assert(simulation.controllers[first].packetNumber == 1);
	assert(simulation.controllers[second].packetNumber == 1);
}

static void TestRoutingRemovalOrdersAndGuideDebounce() {
	int order[4];
	int permutations = 0;
	for (order[0] = 0; order[0] < 4; ++order[0])
		for (order[1] = 0; order[1] < 4; ++order[1])
			for (order[2] = 0; order[2] < 4; ++order[2])
				for (order[3] = 0; order[3] < 4; ++order[3]) {
					bool seen[4] = {};
					bool unique = true;
					for (int i = 0; i < 4; ++i) {
						if (seen[order[i]]) unique = false;
						seen[order[i]] = true;
					}
					if (!unique) continue;
					++permutations;
					RoutingSimulation simulation;
					for (int i = 0; i < 4; ++i) simulation.Connect(i, (uint32_t)i);
					simulation.Process();
					for (int i = 0; i < 4; ++i) {
						simulation.Disconnect(order[i]);
						simulation.Process();
						assert(simulation.slots[order[i]].controllerIndex == -1);
						for (int slot = 0; slot < 4; ++slot)
							if (simulation.slots[slot].connected)
								assert(simulation.slots[slot].controllerIndex >= 0);
					}
				}
	assert(permutations == 24);
	assert(ProteusRouting::GuidePressIsDue(0, 10, 1000));
	assert(!ProteusRouting::GuidePressIsDue(100, 1099, 1000));
	assert(ProteusRouting::GuidePressIsDue(100, 1100, 1000));
	assert(ProteusRouting::GuidePressIsDue(0xfffffff0u, 0x000003e0u, 1000));
}

static void TestControllerCapabilities() {
	struct GuardedCapabilities {
		XINPUT_CAPABILITIES caps;
		uint32_t guard;
	} output;
	memset(&output, 0xa5, sizeof(output));
	ControllerCapabilities::Fill(&output.caps);
	assert(output.guard == 0xa5a5a5a5u);
	assert(output.caps.Type == XINPUT_DEVTYPE_GAMEPAD);
	assert(output.caps.SubType == XINPUT_DEVSUBTYPE_GAMEPAD);
	assert(output.caps.Flags == 0);
	assert(output.caps.Gamepad.wButtons == (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B |
		XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK |
		XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB |
		XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER |
		XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN |
		XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT));
	assert(output.caps.Gamepad.bLeftTrigger == 255 && output.caps.Gamepad.bRightTrigger == 255);
	assert(output.caps.Gamepad.sThumbLX == 32767 && output.caps.Gamepad.sThumbLY == 32767);
	assert(output.caps.Gamepad.sThumbRX == 32767 && output.caps.Gamepad.sThumbRY == 32767);
	assert(output.caps.Vibration.wLeftMotorSpeed == 0 && output.caps.Vibration.wRightMotorSpeed == 0);
	XINPUT_CAPABILITIES expected = output.caps;
	memset(&output.caps, 0xff, sizeof(output.caps));
	ControllerCapabilities::Fill(&output.caps);
	assert(memcmp(&expected, &output.caps, sizeof(expected)) == 0);
	struct ExtendedCapabilities : XINPUT_CAPABILITIES {
		uint32_t reserved[3];
	};
	struct GuardedExtendedCapabilities {
		ExtendedCapabilities caps;
		uint32_t guard;
	} extended;
	memset(&extended, 0xa5, sizeof(extended));
	ControllerCapabilities::Fill(&extended.caps);
	assert(extended.guard == 0xa5a5a5a5u);
	assert(memcmp(&expected, static_cast<XINPUT_CAPABILITIES*>(&extended.caps), sizeof(expected)) == 0);
	for (int i = 0; i < 3; ++i) assert(extended.caps.reserved[i] == 0);
	assert(ControllerCapabilities::AcceptsGamepad(0));
	assert(ControllerCapabilities::AcceptsGamepad(1));
	assert(!ControllerCapabilities::AcceptsGamepad(2));
	assert(ControllerCapabilities::AcceptsGamepad(0x40000001));
	assert(ControllerCapabilities::AnyUser(0xff, 0));
	assert(ControllerCapabilities::AnyUser(0xffffffffu, 0));
	assert(ControllerCapabilities::AnyUser(2, 0x40000000));
	assert(!ControllerCapabilities::AnyUser(0x1ff, 0));
	assert(!ControllerCapabilities::AnyUser(4, 0));
}

int main() {
	TestControllerCapabilities();
	TestUsbDescriptors();
	TestAdmissionAndValidation();
	TestStateIdsAndAxes();
	TestButtonsAndTriggers();
	TestStatusAndFeature();
	TestRoutingConnectionOrders();
	TestRoutingDisconnectRetryAndGeneration();
	TestRoutingFailuresAndIsolation();
	TestRoutingRemovalOrdersAndGuideDebounce();
	return 0;
}
