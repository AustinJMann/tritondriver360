#pragma once

#include <stddef.h>
#include <stdint.h>

namespace TritonProtocol {

static const uint16_t kValveVendorId = 0x28DE;
static const uint16_t kTritonUsbProductId = 0x1302;
static const uint16_t kProteusProductId = 0x1304;
static const uint8_t kFirstSlotInterface = 2;
static const uint8_t kLastSlotInterface = 5;
static const size_t kInputPrefixSize = 18;
static const size_t kInputTouchSize = 30;
static const size_t kTimestampInputTouchSize = 32;
static const size_t kFeatureReportSize = 64;
static const size_t kRumbleReportSize = 10;
static const size_t kHapticCommandReportSize = 4;

// Report 0x82 side values; report 0x81 orders the trackpads differently.
static const uint8_t kHapticSideLeftPad = 0;
static const uint8_t kHapticSideRightPad = 1;

enum HapticCommand {
	kHapticStopAll = 0,
	kHapticClick = 1,
	kHapticClickStrong = 2
};

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

struct RightPadState {
	uint8_t sequence;
	bool coordinatesValid;
	bool timestampValid;
	bool contact;
	bool click;
	int16_t x;
	int16_t y;
	uint16_t pressure;
	uint16_t timestamp;
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
	uint8_t r4;
	uint8_t r5;
	uint8_t l4;
	uint8_t l5;
};
#pragma pack(pop)

uint16_t ReadLE16(const uint8_t* bytes);
int16_t ReadSLE16(const uint8_t* bytes);
uint32_t ReadLE32(const uint8_t* bytes);

bool IsProteusSlotInterface(uint16_t vendorId, uint16_t productId,
	uint8_t interfaceNumber, uint8_t interfaceClass,
	uint8_t interfaceSubClass, uint8_t interfaceProtocol);
bool DecodeInputPrefix(const uint8_t* bytes, size_t length, InputState* state);
bool DecodeRightPad(const uint8_t* bytes, size_t length, RightPadState* state);
bool DecodeWirelessStatus(const uint8_t* bytes, size_t length, WirelessStatus* status);
void ConvertToControllerState(const InputState& state, ControllerState* stateOut);
void BuildLizardOffFeatureReport(uint8_t report[kFeatureReportSize]);
void BuildRumbleOutputReport(uint16_t left, uint16_t right,
	uint8_t report[kRumbleReportSize]);
void BuildHapticCommandReport(uint8_t side, HapticCommand command, int8_t gainDb,
	uint8_t report[kHapticCommandReportSize]);

} // namespace TritonProtocol
