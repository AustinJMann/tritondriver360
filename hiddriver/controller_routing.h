#pragma once

#include <stdint.h>

struct ControllerSourceToken {
	uint32_t index;
	uint32_t attachmentEpoch;
};

namespace ControllerRouting {

static const int kPuckSlotCount = 4;
static const int kWiredSourceCount = 4;
static const int kSlotCount = kPuckSlotCount + kWiredSourceCount;
static const int kControllerCount = 4;
static const int kUnboundController = -1;

inline bool ReadyBefore(uint32_t first, uint32_t second) {
	return (int32_t)(first - second) < 0;
}

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

inline bool TokenMatches(ControllerSourceToken token, uint32_t epoch) {
	return IsValidSlotIndex((int)token.index) && token.attachmentEpoch != 0 &&
		token.attachmentEpoch == epoch;
}

} // namespace ControllerRouting
