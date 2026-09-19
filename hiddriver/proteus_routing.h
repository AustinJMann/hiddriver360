#pragma once

#include <stdint.h>

namespace ProteusRouting {

static const int kSlotCount = 4;
static const int kControllerCount = 4;
static const int kUnboundController = -1;

inline bool IsValidSlotIndex(int slotIndex) {
	return slotIndex >= 0 && slotIndex < kSlotCount;
}

inline bool IsValidXamBinding(int result, uint8_t userIndex) {
	return result == 0 && userIndex < kControllerCount;
}

inline bool AssociationMatches(bool connected, int boundController,
	uint32_t slotGeneration, bool controllerOccupied, int controllerSlot,
	uint32_t controllerGeneration, int controllerIndex) {
	return connected && controllerOccupied &&
		IsValidSlotIndex(controllerSlot) &&
		boundController == controllerIndex &&
		slotGeneration == controllerGeneration;
}

inline bool GuidePressIsDue(uint32_t lastPressTime, uint32_t now,
	uint32_t cooldownDuration) {
	return lastPressTime == 0 || (uint32_t)(now - lastPressTime) >= cooldownDuration;
}

} // namespace ProteusRouting
