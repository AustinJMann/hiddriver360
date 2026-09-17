#include <assert.h>
#include <string.h>

#include "../hiddriver/triton_protocol.h"
#include "../hiddriver/proteus_routing.h"

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

int main() {
	TestEndianAndValidation();
	TestStateIdsAndAxes();
	TestButtonsAndTriggers();
	TestStatusAndFeature();
	TestRoutingConnectionOrders();
	TestRoutingDisconnectRetryAndGeneration();
	TestRoutingFailuresAndIsolation();
	TestRoutingRemovalOrdersAndGuideDebounce();
	return 0;
}
