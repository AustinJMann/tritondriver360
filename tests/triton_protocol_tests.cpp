// These checks must run even when the host project defines NDEBUG.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>
#include <Windows.h>
#include <Xinput.h>
#include "../hiddriver/controller_capabilities.h"
#include "../hiddriver/rumble_output.h"
#include "../hiddriver/triton_config.h"
#include "../hiddriver/mouse_joystick.h"
#include "../hiddriver/trackball_motion.h"
#include "../hiddriver/trackpad_motion.h"
#include "../hiddriver/trackpad_haptics.h"
#include "../hiddriver/input_processing.h"

#include "../hiddriver/triton_protocol.h"
#include "../hiddriver/controller_routing.h"
#include "../hiddriver/controller_usb_policy.h"
#include "../hiddriver/triton_hid_descriptor.h"
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
	SimSlot slots[ControllerRouting::kSlotCount];
	SimController controllers[ControllerRouting::kControllerCount];
	bool rejectBind[ControllerRouting::kSlotCount];

	RoutingSimulation() { Reset(); }

	void Reset() {
		memset(this, 0, sizeof(*this));
		for (int i = 0; i < ControllerRouting::kSlotCount; ++i) {
			slots[i].controllerIndex = ControllerRouting::kUnboundController;
			slots[i].generation = 1;
			if (i < ControllerRouting::kControllerCount) controllers[i].slotIndex = -1;
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
		for (int slotIndex = 0; slotIndex < ControllerRouting::kSlotCount; ++slotIndex) {
			SimSlot& slot = slots[slotIndex];
			bool disconnectPending = slot.disconnectPending;
			slot.disconnectPending = false;
			if ((disconnectPending || !slot.connected) && slot.controllerIndex >= 0) {
				SimController& controller = controllers[slot.controllerIndex];
				controller.occupied = false;
				controller.slotIndex = -1;
				slot.controllerIndex = ControllerRouting::kUnboundController;
				++slot.generation;
			}
		}
		for (int slotIndex = 0; slotIndex < ControllerRouting::kSlotCount; ++slotIndex) {
			SimSlot& slot = slots[slotIndex];
			if (!slot.connected || slot.controllerIndex >= 0) continue;
			int freeController = -1;
			for (int i = 0; i < ControllerRouting::kControllerCount; ++i) {
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
				slot.controllerIndex = ControllerRouting::kUnboundController;
				++slot.generation;
			}
		}
	}

	bool Read(int controllerIndex, uint32_t* state) {
		SimController& controller = controllers[controllerIndex];
		if (controller.slotIndex < 0) return false;
		SimSlot& slot = slots[controller.slotIndex];
		if (!ControllerRouting::AssociationMatches(slot.connected,
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

static void TestUsbOutputDescriptors() {
	// Synthetic fixture: OUT addresses differ from IN. An alternate setting and
	// a different slot must never supply the target slot's output endpoint.
	const uint8_t configuration[] = {
		9, 2, 78, 0, 2, 1, 0, 0x80, 50,
		9, 4, 2, 0, 2, 3, 0, 0, 0,
		7, 5, 0x82, 3, 64, 0, 1,
		7, 5, 0x05, 3, 32, 0, 2,
		9, 4, 2, 1, 2, 3, 0, 0, 0,
		7, 5, 0x86, 3, 64, 0, 1,
		7, 5, 0x06, 3, 64, 0, 1,
		9, 4, 3, 0, 2, 3, 0, 0, 0,
		7, 5, 0x83, 3, 64, 0, 1,
		7, 5, 0x07, 3, 64, 0, 1
	};
	usb_endpoint_descriptor endpoint = {};
	assert(UsbDescriptors::FindInterruptOutEndpoint(configuration, sizeof(configuration), 2, &endpoint));
	assert(endpoint.bEndpointAddress == 5 && endpoint.bInterval == 2);
	assert(ReadLE16((const uint8_t*)&endpoint.wMaxPacketSize) == 32);
	assert(UsbDescriptors::IsInterruptOutEndpoint(endpoint));
	assert(!UsbDescriptors::IsInterruptInEndpoint(endpoint));
	assert(UsbDescriptors::FindInterruptInEndpoint(configuration, sizeof(configuration), 2, &endpoint));
	assert(endpoint.bEndpointAddress == 0x82);
	assert(!UsbDescriptors::IsInterruptOutEndpoint(endpoint));
	assert(UsbDescriptors::FindInterruptOutEndpoint(configuration, sizeof(configuration), 3, &endpoint));
	assert(endpoint.bEndpointAddress == 7);
	assert(!UsbDescriptors::FindInterruptOutEndpoint(configuration, sizeof(configuration), 4, &endpoint));
	assert(endpoint.bEndpointAddress == 7);
	for (size_t length = 0; length < sizeof(configuration); ++length) {
		assert(!UsbDescriptors::FindInterruptOutEndpoint(configuration, length, 2, &endpoint));
		assert(endpoint.bEndpointAddress == 7);
	}
	uint8_t malformed[sizeof(configuration)];
	const size_t offsets[] = { 9, 18, 25, 32, 41, 48, 55, 64, 71 };
	for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
		memcpy(malformed, configuration, sizeof(configuration));
		malformed[offsets[i]] = 1;
		assert(!UsbDescriptors::FindInterruptOutEndpoint(malformed, sizeof(malformed), 2, &endpoint));
		assert(endpoint.bEndpointAddress == 7);
	}
	const size_t badOffsets[] = { 12, 14, 15, 16, 27, 27, 27, 28 };
	const uint8_t badValues[] = { 1, 2, 1, 1, 0, 0x85, 0x75, 2 };
	for (size_t i = 0; i < sizeof(badOffsets) / sizeof(badOffsets[0]); ++i) {
		memcpy(malformed, configuration, sizeof(configuration));
		malformed[badOffsets[i]] = badValues[i];
		assert(!UsbDescriptors::FindInterruptOutEndpoint(malformed, sizeof(malformed), 2, &endpoint));
		assert(endpoint.bEndpointAddress == 7);
	}
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
	Put32(packet + 2, 0x00000080 | 0x00000100 | 0x00020000 | 0x00040000);
	Put16(packet + 6, 0); Put16(packet + 8, 0);
	assert(DecodeInputPrefix(packet, sizeof(packet), &state));
	ConvertToControllerState(state, &b);
	assert(b.r4 && b.r5 && b.l4 && b.l5);
}

static bool Near(float left, float right, float tolerance = 0.0001f) {
	float difference = left - right;
	if (difference < 0.0f) difference = -difference;
	return difference <= tolerance;
}

static void TestConfigDefaultsAndBlackOpsProfile() {
	TritonConfig::Config config;
	TritonConfig::ParseError error = {};
	assert(TritonConfig::Parse(TritonConfig::DefaultFileText(),
		TritonConfig::DefaultFileSize(), &config, &error));
	assert(config.gameCount == 1);
	const TritonConfig::Profile* defaults = TritonConfig::FindProfile(config, 0x12345678);
	assert(defaults == &config.defaults);
	assert(defaults->rumble.enabled);
	assert(defaults->rumble.deadzone == RumbleOutput::kIntensityDeadzone);
	assert(defaults->rumble.leftGainPermille == 2000);
	assert(defaults->rumble.rightGainPermille == 2000);
	assert(defaults->rumble.curve == RumbleOutput::kCurveCubic);
	assert(Near(defaults->mouseJoystick.sensitivityX, 0.5f));
	assert(Near(defaults->mouseJoystick.sensitivityY, 0.5f));
	assert(Near(defaults->mouseJoystick.minimumOutputX, 0.2f));
	assert(Near(defaults->mouseJoystick.minimumOutputY, 0.2f));
	assert(defaults->mouseJoystick.smoothingMs == 8);
	assert(Near(defaults->mouseJoystick.noiseSpeedThreshold, 0.2f));
	assert(defaults->rightTrackpad.mode == TritonConfig::kRightTrackpadMouseJoystick);
	assert(defaults->rightTrackpad.clickAction == TritonConfig::kBindingRightStick);
	assert(defaults->rightTrackpad.trackball.curve == TrackballMotion::kCurveEaseOutCubic);
	assert(Near(defaults->rightTrackpad.trackball.frictionStrength, 3.0f));
	assert(Near(defaults->rightTrackpad.trackball.frictionMaxSpeed, 6.0f));
	assert(defaults->rightTrackpad.trackball.frictionReferenceMs == 150);
	assert(defaults->rightTrackpad.trackball.frictionMinMs == 30);
	assert(defaults->rightTrackpad.trackball.frictionMaxMs == 300);
	const TritonConfig::Profile* blackOps = TritonConfig::FindProfile(config, 0x415608C3);
	assert(blackOps != defaults);
	assert(blackOps->paddles[TritonConfig::kPaddleR4] == TritonConfig::kBindingX);
	assert(blackOps->paddles[TritonConfig::kPaddleR5] == TritonConfig::kBindingY);
	assert(blackOps->paddles[TritonConfig::kPaddleL4] == TritonConfig::kBindingA);
	assert(blackOps->paddles[TritonConfig::kPaddleL5] == TritonConfig::kBindingB);
	assert(blackOps->rumble.deadzone == defaults->rumble.deadzone);

	ControllerState state = {};
	state.r4 = state.r5 = state.l4 = state.l5 = 1;
	TritonConfig::ApplyPaddleBindings(*blackOps, &state);
	assert(state.x && state.y && state.a && state.b);
}

static void TestConfigOverridesAndValidation() {
	const char configText[] =
		"version: 1\n"
		"defaults:\n"
		"  paddles:\n"
		"    l4: left_trigger\n"
		"  rumble:\n"
		"    enabled: true\n"
		"    left_gain: 1.25\n"
		"    right_gain: 0.5\n"
		"    deadzone: 0.2\n"
		"    curve: quadratic\n"
		"games:\n"
		"  'DEADBEEF':\n"
		"    paddles:\n"
		"      r4: dpad_up\n"
		"    rumble:\n"
		"      enabled: false\n"
		"      curve: linear\n";
	TritonConfig::Config config;
	TritonConfig::ParseError error = {};
	assert(TritonConfig::Parse(configText, sizeof(configText) - 1, &config, &error));
	const TritonConfig::Profile* profile = TritonConfig::FindProfile(config, 0xdeadbeef);
	assert(profile->paddles[TritonConfig::kPaddleL4] == TritonConfig::kBindingLeftTrigger);
	assert(profile->paddles[TritonConfig::kPaddleR4] == TritonConfig::kBindingDpadUp);
	assert(!profile->rumble.enabled);
	assert(profile->rumble.leftGainPermille == 1250);
	assert(profile->rumble.rightGainPermille == 500);
	assert(profile->rumble.curve == RumbleOutput::kCurveLinear);
	ControllerState state = {};
	state.leftTrigger = 100;
	state.l4 = state.r4 = 1;
	TritonConfig::ApplyPaddleBindings(*profile, &state);
	assert(state.leftTrigger == 255 && state.dpadUp);

	const char duplicate[] = "version: 1\ndefaults:\n  paddles:\n    l4: a\n    l4: b\n";
	assert(!TritonConfig::Parse(duplicate, sizeof(duplicate) - 1, &config, &error));
	assert(error.line == 5);
	const char invalidTitle[] = "version: 1\ngames:\n  BAD:\n    paddles:\n      l4: a\n";
	assert(!TritonConfig::Parse(invalidTitle, sizeof(invalidTitle) - 1, &config, &error));
	const char invalidRange[] = "version: 1\ndefaults:\n  rumble:\n    deadzone: 0.951\n";
	assert(!TritonConfig::Parse(invalidRange, sizeof(invalidRange) - 1, &config, &error));
	const char overflowingVersion[] = "version: 4294967297\n";
	assert(!TritonConfig::Parse(overflowingVersion,
		sizeof(overflowingVersion) - 1, &config, &error));
	// Reloads keep the previous config when a new file fails to parse.
	profile = TritonConfig::FindProfile(config, 0xdeadbeef);
	assert(config.gameCount == 1);
	assert(profile->paddles[TritonConfig::kPaddleR4] == TritonConfig::kBindingDpadUp);
	assert(config.defaults.rumble.leftGainPermille == 1250);
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
	assert(ControllerRouting::IsValidXamBinding(0, 0));
	assert(ControllerRouting::IsValidXamBinding(0, 3));
	assert(!ControllerRouting::IsValidXamBinding(-1, 0));
	assert(!ControllerRouting::IsValidXamBinding(0, 4));
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
	assert(ControllerRouting::GuidePressIsDue(0, 10, 1000));
	assert(!ControllerRouting::GuidePressIsDue(100, 1099, 1000));
	assert(ControllerRouting::GuidePressIsDue(100, 1100, 1000));
	assert(ControllerRouting::GuidePressIsDue(0xfffffff0u, 0x000003e0u, 1000));
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
	assert(output.caps.Flags == 0x0003); // Xbox 360 force feedback + wireless flags.
	assert(output.caps.Gamepad.wButtons == (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B |
		XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK |
		XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB |
		XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER |
		XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN |
		XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT));
	assert(output.caps.Gamepad.bLeftTrigger == 255 && output.caps.Gamepad.bRightTrigger == 255);
	assert(output.caps.Gamepad.sThumbLX == 32767 && output.caps.Gamepad.sThumbLY == 32767);
	assert(output.caps.Gamepad.sThumbRX == 32767 && output.caps.Gamepad.sThumbRY == 32767);
	assert(output.caps.Vibration.wLeftMotorSpeed == 0xffff && output.caps.Vibration.wRightMotorSpeed == 0xffff);
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

struct TestVibration {
	uint16_t wLeftMotorSpeed;
	uint16_t wRightMotorSpeed;
};

struct TestRumbleBackend {
	typedef int Target;
	bool ownsUser;
	uint32_t nativeResult;
	uint32_t submitResult;
	uint32_t nativeCalls;
	uint32_t submitCalls;
	uint32_t lookupUser;
	uint32_t nativeUser;
	uint32_t nativeFlags;
	TestVibration* nativeVibration;
	uint16_t left;
	uint16_t right;

	bool Find(uint32_t user, Target* target) {
		lookupUser = user;
		*target = 7;
		return ownsUser;
	}
	uint32_t Native(uint32_t user, uint32_t flags, TestVibration* vibration) {
		++nativeCalls;
		nativeUser = user;
		nativeFlags = flags;
		nativeVibration = vibration;
		return nativeResult;
	}
	uint32_t Submit(Target target, uint16_t l, uint16_t r) {
		assert(target == 7);
		++submitCalls;
		left = l;
		right = r;
		return submitResult;
	}
};

static void TestRumbleXamDispatch() {
	TestRumbleBackend backend = {};
	TestVibration vibration = { 0x1234, 0xabcd };
	backend.ownsUser = true;
	// Regress the silent-drop case: native success must not swallow output for
	// a controller owned by this driver. Native is never called in that case.
	assert(RumbleOutput::SetState(2, 0, &vibration, backend) == 0);
	assert(backend.submitCalls == 1 && backend.nativeCalls == 0);
	assert(backend.lookupUser == 2);
	assert(backend.left == RumbleOutput::ScaleIntensity(0x1234));
	assert(backend.right == RumbleOutput::ScaleIntensity(0xabcd));
	backend.nativeResult = 1167;
	vibration.wLeftMotorSpeed = vibration.wRightMotorSpeed = 0;
	assert(RumbleOutput::SetState(2, 0, &vibration, backend) == 0);
	assert(backend.submitCalls == 2 && backend.nativeCalls == 0);
	assert(backend.left == 0 && backend.right == 0); // Stop takes the same route.
	assert(RumbleOutput::SetState(2, 0, (TestVibration*)0, backend) == 87);
	assert(backend.submitCalls == 2 && backend.nativeCalls == 0);
	backend.submitResult = 1167; // Binding disappeared before atomic publication.
	assert(RumbleOutput::SetState(2, 0, &vibration, backend) == 1167);
	assert(backend.nativeCalls == 0);
	backend.submitResult = 170; // Contended mailbox failure propagates.
	assert(RumbleOutput::SetState(2, 0, &vibration, backend) == 170);
	backend.submitResult = 0;
	assert(RumbleOutput::SetState(0xffffffffu, 0, &vibration, backend) == 0);
	assert(backend.lookupUser == 0);
	assert(RumbleOutput::SetState(0xff, 0, &vibration, backend) == 0);
	assert(backend.lookupUser == 0);

	backend.ownsUser = false;
	uint32_t submitted = backend.submitCalls;
	assert(RumbleOutput::SetState(0xffffffffu, 0x40000000u, &vibration, backend) == 1167);
	assert(backend.nativeUser == 0xffffffffu && backend.nativeFlags == 0x40000000u);
	assert(backend.nativeVibration == &vibration && backend.submitCalls == submitted);
	backend.nativeResult = 0;
	assert(RumbleOutput::SetState(1, 123, &vibration, backend) == 0);
	assert(backend.nativeUser == 1 && backend.nativeFlags == 123);
	backend.nativeResult = 87;
	assert(RumbleOutput::SetState(1, 0, (TestVibration*)0, backend) == 87);
	assert(backend.nativeVibration == 0 && backend.submitCalls == submitted);
}

static void TestRumbleDeadzoneAndCubicScaling() {
	RumbleOutput::Settings settings = RumbleOutput::DefaultSettings();
	settings.leftGainPermille = 1000;
	assert(RumbleOutput::ScaleIntensity(0, settings.deadzone, settings.curve, 1000) == 0);
	assert(RumbleOutput::ScaleIntensity(RumbleOutput::kIntensityDeadzone - 1,
		settings.deadzone, settings.curve, 1000) == 0);
	assert(RumbleOutput::ScaleIntensity(RumbleOutput::kIntensityDeadzone,
		settings.deadzone, settings.curve, 1000) == 0);
	assert(RumbleOutput::ScaleIntensity(RumbleOutput::kIntensityDeadzone + 1,
		settings.deadzone, settings.curve, 1000) <= 1);
	uint16_t activeMidpoint = (uint16_t)(RumbleOutput::kIntensityDeadzone +
		(0xffffu - RumbleOutput::kIntensityDeadzone) / 2);
	uint16_t midpoint = RumbleOutput::ScaleIntensity(activeMidpoint,
		settings.deadzone, settings.curve, 1000);
	assert(midpoint >= 8191 && midpoint <= 8193);
	assert(RumbleOutput::ScaleIntensity(0xffff, settings.deadzone, settings.curve, 1000) == 0xffff);
	for (uint32_t value = 1; value <= 0xffff; ++value)
		assert(RumbleOutput::ScaleIntensity((uint16_t)value, settings.deadzone, settings.curve, 1000) >=
			RumbleOutput::ScaleIntensity((uint16_t)(value - 1), settings.deadzone, settings.curve, 1000));
}

static void TestTunableRumbleScaling() {
	RumbleOutput::Settings settings = RumbleOutput::DefaultSettings();
	settings.deadzone = 0;
	settings.curve = RumbleOutput::kCurveLinear;
	settings.leftGainPermille = 500;
	settings.rightGainPermille = 2000;
	assert(RumbleOutput::ScaleIntensity(32768, settings.deadzone, settings.curve,
		settings.leftGainPermille) == 16384);
	assert(RumbleOutput::ScaleIntensity(32768, settings.deadzone, settings.curve,
		settings.rightGainPermille) == 65535);
	TestRumbleBackend backend = {};
	backend.ownsUser = true;
	TestVibration vibration = { 32768, 32768 };
	assert(RumbleOutput::SetState(0, 0, &vibration, backend, settings) == 0);
	assert(backend.left == 16384 && backend.right == 65535);
	settings.enabled = false;
	assert(RumbleOutput::SetState(0, 0, &vibration, backend, settings) == 0);
	assert(backend.left == 0 && backend.right == 0);
}

static void TestRumbleEncoding() {
	uint8_t report[kRumbleReportSize + 2];
	memset(report, 0xcc, sizeof(report));
	BuildRumbleOutputReport(0x1234, 0xabcd, report + 1);
	const uint8_t expected[] = { 0x80, 0, 0, 0, 0x34, 0x12, 0, 0xcd, 0xab, 0 };
	assert(memcmp(report + 1, expected, sizeof(expected)) == 0);
	assert(report[0] == 0xcc && report[sizeof(report) - 1] == 0xcc);
	BuildRumbleOutputReport(0xffff, 0, report);
	assert(report[4] == 0xff && report[5] == 0xff && report[7] == 0 && report[8] == 0);
	BuildRumbleOutputReport(0, 0xffff, report);
	assert(report[4] == 0 && report[5] == 0 && report[7] == 0xff && report[8] == 0xff);
	BuildRumbleOutputReport(0, 0, report);
	assert(report[0] == 0x80);
	for (size_t i = 1; i < kRumbleReportSize; ++i) assert(report[i] == 0);
}

static void TestRumbleRefreshAndCoalescing() {
	using namespace RumbleOutput;
	State state = {};
	assert(!state.Due(100));
	state.Update(Request(1, 0, 0));
	assert(state.Due(100)); // New binding starts with a stop.
	state.Submitted(100);
	assert(!state.Due(1000)); // Never reuse an in-flight buffer.
	state.Complete(true, 101);
	assert(!state.Due(1000)); // Idle does not continuously send zero packets.
	state.Update(Request(1, 0x1234, 0x5678));
	assert(Left(state.desired) == 0x1234 && Right(state.desired) == 0x5678);
	assert(state.Due(110));
	state.Submitted(110);
	state.Update(Request(1, 222, 333));
	state.Update(Request(1, 444, 555));
	assert(Left(state.inFlight) == 0x1234 && !state.Due(111));
	state.Complete(true, 112);
	assert(state.Due(112)); // Completion did not lose a newer request.
	state.Submitted(112);
	state.Complete(true, 113);
	assert(!state.Due(112 + kRefreshMs - 1));
	assert(state.Due(112 + kRefreshMs)); // Sustains even if game calls only once.
	state.Update(Request(1, 0, 0));
	assert(state.Due(114)); // Stop need not wait for the refresh deadline.
	state.Submitted(114);
	state.Complete(true, 115);
	assert(!state.Due(10000));
}

static void TestRumbleFailureAndReconnect() {
	using namespace RumbleOutput;
	State state = {};
	state.Update(Request(10, 123, 456));
	state.Submitted(100);
	state.Complete(false, 101);
	assert(!state.Due(110) && state.Due(111));
	state.Submitted(111);
	state.Complete(false, 112);
	assert(!state.Due(131) && state.Due(132));
	state.Update(Request(10, 0, 0));
	state.Submitted(132);
	state.Complete(false, 133);
	assert(state.Due(173)); // A failed stop is retried too.
	state.Submitted(173);
	state.Complete(true, 174);
	assert(!state.Due(10000));
	state.Update(Request(10, 1, 2));
	state.Submitted(200);
	state.Update(0); // Disconnect while old output is pending.
	assert(!state.Due(201));
	state.Update(Request(11, 0, 0));
	assert(!state.Due(202));
	state.Complete(true, 203);
	assert(state.acknowledged == 0 && state.Due(203));
	state.Submitted(203);
	state.Complete(true, 204);
	assert(!state.Due(999)); // Previous controller's strengths cannot replay.
	state.Update(Request(11, 1, 0));
	state.Submitted(1000);
	state.Update(Request(12, 0, 0));
	state.Complete(false, 1001);
	assert(state.failures == 0 && state.Due(1001)); // Old failure cannot delay new binding.
}

static void TestRumbleWrapAndSlotIsolation() {
	using namespace RumbleOutput;
	State slots[4] = {};
	for (int i = 0; i < 4; ++i) {
		slots[i].Update(Request((uint32_t)i + 1, (uint16_t)(100 + i), (uint16_t)i));
		slots[i].Submitted(0xfffffff0u);
		slots[i].Complete(true, 0xfffffff1u);
		assert(!slots[i].Due(0xdu) && slots[i].Due(0xeu));
	}
	slots[0].Update(0);
	slots[1].Update(Request(2, 0, 0));
	assert(!slots[0].Due(0xeu) && slots[1].Due(0xeu));
	assert(Left(slots[2].desired) == 102 && Left(slots[3].desired) == 103);
	slots[2].Submitted(0xfffffff0u);
	slots[2].Complete(false, 0xfffffffcu);
	assert(!slots[2].Due(5) && slots[2].Due(6));
	uint32_t now = 6;
	for (int i = 0; i < 12; ++i) {
		slots[2].Submitted(now);
		slots[2].Complete(false, now);
		assert((uint32_t)(slots[2].retryAt - now) <= 250);
		now = slots[2].retryAt;
		assert(slots[2].Due(now));
	}
}

static void TestWiredAdmissionAndSourceCapacity() {
	using namespace ControllerUsbPolicy;
	usb_interface_descriptor d = { 9, 4, 0, 0, 2, 3, 0, 0, 0 };
	assert(Classify(0x28de, 0x1302, &d) == kWiredTriton);
	assert(Classify(0x28de, 0x1303, &d) == kUnsupported);
	assert(Classify(0x28de, 0x1305, &d) == kUnsupported);
	assert(Classify(0x1234, 0x1302, &d) == kUnsupported);
	d.bAlternateSetting = 1; assert(Classify(0x28de, 0x1302, &d) == kUnsupported);
	d.bAlternateSetting = 0; d.bInterfaceSubClass = 1;
	assert(Classify(0x28de, 0x1302, &d) == kUnsupported);
	d.bInterfaceSubClass = 0; d.bInterfaceProtocol = 1;
	assert(Classify(0x28de, 0x1302, &d) == kUnsupported);
	d.bInterfaceProtocol = 0; d.bInterfaceNumber = 2;
	assert(Classify(0x28de, 0x1304, &d) == kProteus);
	d.bLength = 8; assert(Classify(0x28de, 0x1304, &d) == kUnsupported);
	assert(!ShouldPauseHeartbeat(kWiredTriton, false, 255));
	assert(ShouldPauseHeartbeat(kProteus, false, 3));
	assert(InputLength(65, 64) == 0 && InputLength(3, 64) == 3);
	ControllerSourceToken token = { 7, 25 };
	assert(ControllerRouting::TokenMatches(token, 25));
	assert(!ControllerRouting::TokenMatches(token, 26));
	token.index = 8; assert(!ControllerRouting::TokenMatches(token, 25));
	token.index = 0; token.attachmentEpoch = 0;
	assert(!ControllerRouting::TokenMatches(token, 0));
	assert(ControllerRouting::ReadyBefore(0xfffffff0u, 3));
	RoutingSimulation simulation;
	// Four wired sources can bind while every empty puck slot stays unbound.
	for (int i = 4; i < 8; ++i) simulation.Connect(i, (uint32_t)i);
	simulation.Process();
	for (int i = 0; i < 4; ++i) assert(simulation.slots[i + 4].controllerIndex == i);
	for (int i = 0; i < 4; ++i) simulation.Connect(i, (uint32_t)i);
	simulation.Process();
	for (int i = 0; i < 4; ++i) assert(simulation.slots[i].controllerIndex == -1);
	simulation.Disconnect(5); simulation.Process();
	assert(simulation.slots[0].controllerIndex == 1);
	assert(simulation.slots[4].controllerIndex == 0);
}

static void TestTritonHidDescriptors() {
	const uint8_t hid[] = {
		0x06, 0, 0xff, 0x09, 1, 0xa1, 1, 0x75, 8,
		0x85, 0x42, 0x95, 63, 0x81, 2,
		0x85, 1, 0x95, 63, 0xb1, 2,
		0x85, 0x80, 0x95, 9, 0x91, 2, 0xc0
	};
	assert(TritonHidDescriptor::Validate(hid, sizeof(hid)));
	for (size_t i = 0; i < sizeof(hid); ++i) assert(!TritonHidDescriptor::Validate(hid, i));
	uint8_t bad[sizeof(hid)]; memcpy(bad, hid, sizeof(hid));
	bad[18] = 62; assert(!TritonHidDescriptor::Validate(bad, sizeof(bad)));
	memcpy(bad, hid, sizeof(hid)); bad[22] = 0x81;
	assert(!TritonHidDescriptor::Validate(bad, sizeof(bad)));
	memcpy(bad, hid, sizeof(hid)); bad[10] = 0;
	assert(!TritonHidDescriptor::Validate(bad, sizeof(bad)));
	memcpy(bad, hid, sizeof(hid)); bad[0] = 0xfe;
	assert(!TritonHidDescriptor::Validate(bad, sizeof(bad)));
	bool haptic = true;
	assert(TritonHidDescriptor::Validate(hid, sizeof(hid), &haptic) && !haptic);
	const uint8_t withHaptic[] = {
		0x06, 0, 0xff, 0x09, 1, 0xa1, 1, 0x75, 8,
		0x85, 0x42, 0x95, 63, 0x81, 2,
		0x85, 1, 0x95, 63, 0xb1, 2,
		0x85, 0x80, 0x95, 9, 0x91, 2,
		0x85, 0x82, 0x95, 3, 0x91, 2, 0xc0
	};
	assert(TritonHidDescriptor::Validate(withHaptic, sizeof(withHaptic), &haptic) && haptic);
	uint8_t otherHaptic[sizeof(withHaptic)]; memcpy(otherHaptic, withHaptic, sizeof(withHaptic));
	otherHaptic[30] = 4; // Unexpected layout disables haptics, not the controller.
	assert(TritonHidDescriptor::Validate(otherHaptic, sizeof(otherHaptic), &haptic) && !haptic);
	haptic = true; // Truncated descriptors report no haptic support.
	assert(!TritonHidDescriptor::Validate(withHaptic, sizeof(withHaptic) - 1, &haptic) && !haptic);
	const uint8_t configuration[] = {
		9, 2, 27, 0, 1, 1, 0, 0x80, 50,
		9, 4, 0, 0, 1, 3, 0, 0, 0,
		9, 0x21, 0x11, 1, 0, 1, 0x22, 0x74, 1
	};
	assert(UsbDescriptors::HidReportLength(configuration, sizeof(configuration), 0) == 372);
	assert(UsbDescriptors::HidReportLength(configuration, sizeof(configuration), 1) == 0);
	for (size_t i = 0; i < sizeof(configuration); ++i)
		assert(UsbDescriptors::HidReportLength(configuration, i, 0) == 0);
}

static void TestRightPadDecoding() {
	uint8_t report[64] = {};
	report[0] = 0x45;
	report[1] = 37;
	Put32(report + 2, 0x00200000 | 0x00400000);
	Put16(report + 24, (uint16_t)-1234);
	Put16(report + 26, 2345);
	Put16(report + 28, 30000);
	RightPadState pad = {};
	assert(DecodeRightPad(report, 18, &pad));
	assert(pad.sequence == 37 && pad.contact && pad.click && !pad.coordinatesValid);
	assert(DecodeRightPad(report, 30, &pad));
	assert(pad.coordinatesValid && !pad.timestampValid);
	assert(pad.x == -1234 && pad.y == 2345 && pad.pressure == 30000);
	report[0] = 0x47;
	Put16(report + 18, 0xfffe);
	Put16(report + 26, 111);
	Put16(report + 28, (uint16_t)-222);
	Put16(report + 30, 333);
	assert(DecodeRightPad(report, 31, &pad));
	assert(!pad.coordinatesValid && !pad.timestampValid);
	assert(DecodeRightPad(report, 32, &pad));
	assert(pad.coordinatesValid && pad.timestampValid && pad.timestamp == 0xfffe);
	assert(pad.x == 111 && pad.y == -222 && pad.pressure == 333);
	RightPadState unchanged = pad;
	report[0] = 0x43;
	assert(!DecodeRightPad(report, sizeof(report), &unchanged));
	assert(unchanged.x == 111);
}

static void TestMouseJoystickConversionAndFiltering() {
	MouseJoystick::Settings settings = MouseJoystick::DefaultSettings();
	settings.smoothingMs = 0;
	settings.noiseSpeedThreshold = 0.1f;
	settings.sensitivityX = 2.0f;
	settings.sensitivityY = 1.0f;
	settings.minimumOutputX = 0.2f;
	settings.minimumOutputY = 0.3f;
	MouseJoystick::Processor processor;
	MouseJoystick::Vector2 velocity = { 0.11f, 0.0f };
	MouseJoystick::Output output = processor.Update(velocity, 10, settings);
	assert(Near(output.x, 0.376f) && output.y == 0.0f);
	velocity.x = 0.08f;
	output = processor.Update(velocity, 10, settings);
	assert(output.x > 0.0f); // Hysteresis remains active above 75%.
	velocity.x = 0.07f;
	output = processor.Update(velocity, 10, settings);
	assert(output.x == 0.0f && !processor.IsMoving());
	velocity.x = -2.0f; velocity.y = 2.0f;
	output = processor.Update(velocity, 10, settings);
	assert(output.x == -1.0f && output.y == 1.0f);
	assert(MouseJoystick::Processor::ToStickAxis(output.x) == -32768);
	assert(MouseJoystick::Processor::ToStickAxis(output.y) == 32767);
	settings.noiseSpeedThreshold = 0.0f;
	settings.minimumOutputX = 0.5f;
	velocity.x = velocity.y = 0.0f;
	processor.Reset();
	output = processor.Update(velocity, 10, settings);
	assert(output.x == 0.0f); // A minimum output never turns rest into motion.
	settings.smoothingMs = 8;
	settings.sensitivityX = 1.0f;
	velocity.x = 1.0f;
	output = processor.Update(velocity, 8, settings);
	assert(Near(output.x, 0.75f)); // Filtered 0.5, then 0.5 minimum compensation.

	// Only the dominant axis receives the minimum output.
	settings.sensitivityX = settings.sensitivityY = 0.5f;
	settings.minimumOutputX = settings.minimumOutputY = 0.2f;
	velocity.x = 1.0f; velocity.y = -0.1f;
	output = MouseJoystick::Processor::Convert(velocity, velocity, settings);
	assert(Near(output.x, 0.6f) && Near(output.y, -0.05f));
	velocity.x = 0.1f; velocity.y = 1.0f;
	output = MouseJoystick::Processor::Convert(velocity, velocity, settings);
	assert(Near(output.x, 0.05f) && Near(output.y, 0.6f));
	velocity.x = velocity.y = 0.5f;
	output = MouseJoystick::Processor::Convert(velocity, velocity, settings);
	assert(Near(output.x, 0.4f) && Near(output.y, 0.4f)); // Ties raise both.
	// An explicit direction, such as a coast's release velocity, picks the axis.
	MouseJoystick::Vector2 direction = { 1.0f, 0.1f };
	velocity.x = 0.01f; velocity.y = 0.05f;
	output = MouseJoystick::Processor::Convert(velocity, direction, settings);
	assert(Near(output.x, 0.204f) && Near(output.y, 0.025f));
}

static void TestTrackpadContactAndRelease() {
	TrackpadMotion::Processor pad;
	TrackpadMotion::Geometry geometry = { 0, 1000, 0, 1000, false, true };
	pad.SetGeometry(geometry);
	TrackpadMotion::Result result = pad.Sample(true, 100, 500, 100);
	assert(!result.hasContactVelocity && !result.released);
	result = pad.Sample(true, 150, 500, 100); // Duplicate timestamp accumulates.
	assert(!result.hasContactVelocity);
	result = pad.Sample(true, 200, 400, 110);
	assert(result.hasContactVelocity && Near(result.contactVelocity.x, 10.0f));
	assert(Near(result.contactVelocity.y, 10.0f));
	result = pad.Sample(true, 200, 400, 130); // Rest is part of release history.
	assert(result.hasContactVelocity && result.contactVelocity.x == 0.0f);
	result = pad.Sample(false, 999, 999, 140);
	assert(result.released);
	assert(Near(result.releaseVelocity.x, 10.0f / 3.0f, 0.001f));
	assert(!pad.HasContact());
	result = pad.Sample(true, 300, 300, 200);
	assert(!result.hasContactVelocity); // Recontact establishes a new baseline.
	assert(pad.Expire(301, 100));
	assert(!pad.HasContact());
}

static void TestTrackballCurvesAndTiming() {
	TrackballMotion::Settings settings = TrackballMotion::DefaultSettings();
	TrackballMotion::Processor coast;
	MouseJoystick::Vector2 release = { 1.0f, 0.0f };
	for (int power = 1; power <= 4; ++power) {
		settings.curve = (TrackballMotion::Curve)power;
		assert(coast.Start(release, 1000, settings, 0.015f));
		MouseJoystick::Vector2 velocity = coast.Advance(1225);
		float expected = 1.0f;
		for (int i = 0; i < power; ++i) expected *= 0.5f;
		assert(Near(velocity.x, expected, 0.0002f));
		assert(velocity.y == 0.0f);
	}
	MouseJoystick::Vector2 velocity = coast.Advance(1450);
	assert(velocity.x == 0.0f && velocity.y == 0.0f && !coast.IsCoasting());
	settings.curve = TrackballMotion::kCurveLinear;
	settings.verticalScale = 1.0f;
	release.y = 1.0f;
	assert(coast.Start(release, 2000, settings, 0.0f));
	velocity = coast.Advance(2450);
	assert(velocity.x > 0.0f);
	assert(velocity.y == 0.0f); // High vertical friction shortens the Y coast.
	settings.verticalScale = 0.0f;
	release.x = 0.0f;
	assert(coast.Start(release, 3000, settings, 0.0f));
	velocity = coast.Advance(3450);
	// Low vertical friction extends the Y coast, but only to friction_max_ms.
	assert(Near(velocity.y, 0.7f));
	settings.frictionMaxMs = 10000 / 4;
	assert(coast.Start(release, 3000, settings, 0.0f));
	velocity = coast.Advance(3450);
	assert(Near(velocity.y, 0.75f)); // Unclamped, Y lasts four times as long.
	settings.frictionMaxMs = 1500;
	settings.verticalScale = 1.0f;
	release.x = release.y = 0.05f / 1.41421356f; // 22.5 ms before clamping.
	assert(coast.Start(release, 3000, settings, 0.0f));
	velocity = coast.Advance(3020);
	// Both axes are raised to friction_min_ms (30 ms), even though Y alone
	// would otherwise end at 13 ms.
	assert(velocity.x > 0.0f && velocity.y > 0.0f);
	assert(Near(velocity.x, velocity.y));
	TrackballMotion::Processor direct, stepped;
	release.x = 1.0f; release.y = 0.0f;
	settings.verticalScale = 0.5f;
	assert(direct.Start(release, 4000, settings, 0.0f));
	assert(stepped.Start(release, 4000, settings, 0.0f));
	stepped.Advance(4100);
	stepped.Advance(4175);
	assert(Near(direct.Advance(4225).x, stepped.Advance(4225).x));
	settings.frictionMaxSpeed = 2.0f;
	release.x = 4.0f;
	assert(coast.Start(release, 5000, settings, 0.0f));
	velocity = coast.Advance(5900);
	assert(Near(velocity.x, 0.0f)); // The capped speed gives a 900 ms duration.
	assert(!coast.IsCoasting());
	coast.Reset();
	settings.enabled = false;
	assert(!coast.Start(release, 0, settings, 0.0f));
}

static void TestTrackpadHapticScheduling() {
	TrackpadHaptics::Settings settings = TrackpadHaptics::DefaultSettings();
	TrackpadHaptics::Scheduler scheduler;
	const float fast = settings.fullSpeed;
	TrackpadHaptics::Request request = scheduler.Update(fast, false, false, 10, settings);
	assert(request.kind == TrackpadHaptics::kRequestNone);
	request = scheduler.Update(fast, false, false, 10, settings);
	assert(request.kind == TrackpadHaptics::kRequestMovement);
	request = scheduler.Update(fast, true, false, 100, settings);
	assert(request.kind == TrackpadHaptics::kRequestClick);
	request = scheduler.Update(fast, false, false, 1000, settings);
	assert(request.kind == TrackpadHaptics::kRequestMovement); // One, not a burst.
	settings.movementIntensity = 0.0f;
	request = scheduler.Update(fast, false, false, 1000, settings);
	assert(request.kind == TrackpadHaptics::kRequestNone);
	request = scheduler.Update(0.0f, true, false, 1, settings);
	assert(request.kind == TrackpadHaptics::kRequestClick);
	request = scheduler.Update(0.0f, false, true, 1, settings);
	assert(request.kind == TrackpadHaptics::kRequestRelease);
	assert(Near(request.intensity, settings.releaseIntensity));
	assert(settings.releaseIntensity < settings.clickIntensity); // Lighter than the press.
	settings.releaseIntensity = 0.0f;
	request = scheduler.Update(0.0f, false, true, 1, settings);
	assert(request.kind == TrackpadHaptics::kRequestNone); // Release can be muted alone.
}

static void TestTrackpadHapticGainAndMailbox() {
	using namespace TrackpadHaptics;
	Settings defaults = DefaultSettings();
	assert(Near(defaults.movementIntensity, 0.25f));
	assert(Near(defaults.clickIntensity, 0.7f));
	assert(Near(defaults.releaseIntensity, 0.35f) && GainDb(0.35f) == 15);
	assert(GainDb(1.0f) == kMaximumGainDb && GainDb(2.0f) == kMaximumGainDb);
	assert(GainDb(0.25f) == 12); // A quarter of maximum amplitude.
	assert(GainDb(0.5f) == 18 && GainDb(0.7f) == 21);
	assert(GainDb(0.001f) == kMinimumGainDb && GainDb(0.0f) == kMinimumGainDb);
	Mailbox mailbox;
	Pulse pulse = {};
	assert(!mailbox.Take(0, &pulse));
	mailbox.Offer(kRequestMovement, 12, 3, 100);
	mailbox.Offer(kRequestMovement, 11, 3, 105); // Newer movement replaces older.
	assert(mailbox.Take(110, &pulse));
	assert(pulse.kind == kRequestMovement && pulse.gainDb == 11 && pulse.generation == 3);
	assert(!mailbox.Take(110, &pulse)); // One shot.
	mailbox.Offer(kRequestClick, 21, 3, 200);
	mailbox.Offer(kRequestMovement, 12, 3, 205); // Movement never hides a click.
	assert(mailbox.Take(206, &pulse) && pulse.kind == kRequestClick);
	mailbox.Offer(kRequestRelease, 15, 3, 250);
	mailbox.Offer(kRequestMovement, 12, 3, 255); // Nor a release.
	assert(mailbox.Take(256, &pulse) && pulse.kind == kRequestRelease && pulse.gainDb == 15);
	mailbox.Offer(kRequestClick, 21, 3, 300);
	mailbox.Offer(kRequestMovement, 12, 4, 301); // A new binding drops the old click.
	assert(mailbox.Take(302, &pulse) && pulse.kind == kRequestMovement);
	mailbox.Offer(kRequestClick, 21, 5, 0xfffffff0u);
	assert(!mailbox.Take(0xfffffff0u + kMaximumPulseAgeMs + 1, &pulse));
	assert(!mailbox.Take(0xfffffff0u, &pulse)); // Expired pulses are discarded.
	mailbox.Offer(kRequestMovement, 12, 5, 0xfffffff0u);
	assert(mailbox.Take(0xfffffff0u + kMaximumPulseAgeMs, &pulse)); // Clock wrap.
	mailbox.Offer(kRequestNone, 0, 5, 1);
	assert(!mailbox.Take(1, &pulse));
	mailbox.Offer(kRequestClick, 21, 5, 1);
	mailbox.Reset();
	assert(!mailbox.Take(1, &pulse));
}

static void TestHapticCommandEncoding() {
	uint8_t report[kHapticCommandReportSize + 2];
	memset(report, 0xcc, sizeof(report));
	BuildHapticCommandReport(kHapticSideRightPad, kHapticClick, 12, report + 1);
	const uint8_t expected[] = { 0x82, 1, 1, 12 };
	assert(memcmp(report + 1, expected, sizeof(expected)) == 0);
	assert(report[0] == 0xcc && report[sizeof(report) - 1] == 0xcc);
	BuildHapticCommandReport(kHapticSideRightPad, kHapticClickStrong, -23, report);
	assert(report[2] == 2 && report[3] == 0xe9);
}

static void TestMouseJoystickConfiguration() {
	const char text[] =
		"games:\n"
		"  '1234ABCD':\n"
		"    mouse_joystick:\n"
		"      sensitivity_x: 3.5\n"
		"    right_trackpad:\n"
		"      mode: mouse_joystick\n"
		"      click_action: right_stick\n"
		"      friction_min_ms: 100\n"
		"      friction_max_speed: 2.5\n"
		"      release_haptics_intensity: 0.2\n"
		"version: 1\n"
		"defaults:\n"
		"  mouse_joystick:\n"
		"    sensitivity_x: 1.25\n"
		"    sensitivity_y: 2.0\n"
		"    minimum_output_x: 0.15\n"
		"    smoothing_ms: 12\n"
		"  right_trackpad:\n"
		"    friction_max_ms: 900\n"
		"    haptics_full_speed: 4.0\n";
	TritonConfig::Config config;
	TritonConfig::ParseError error = {};
	assert(TritonConfig::Parse(text, sizeof(text) - 1, &config, &error));
	const TritonConfig::Profile* defaults = TritonConfig::FindProfile(config, 0);
	const TritonConfig::Profile* game = TritonConfig::FindProfile(config, 0x1234abcd);
	assert(Near(defaults->mouseJoystick.sensitivityX, 1.25f));
	assert(Near(defaults->mouseJoystick.sensitivityY, 2.0f));
	assert(Near(game->mouseJoystick.sensitivityX, 3.5f));
	assert(Near(game->mouseJoystick.sensitivityY, 2.0f));
	assert(game->rightTrackpad.mode == TritonConfig::kRightTrackpadMouseJoystick);
	assert(game->rightTrackpad.clickAction == TritonConfig::kBindingRightStick);
	assert(game->rightTrackpad.trackball.frictionMinMs == 100);
	assert(game->rightTrackpad.trackball.frictionMaxMs == 900);
	assert(Near(defaults->rightTrackpad.trackball.frictionMaxSpeed, 0.0f));
	assert(Near(game->rightTrackpad.trackball.frictionMaxSpeed, 2.5f));
	assert(Near(defaults->rightTrackpad.haptics.releaseIntensity, 0.35f));
	assert(Near(game->rightTrackpad.haptics.releaseIntensity, 0.2f));
	assert(Near(game->rightTrackpad.haptics.clickIntensity, 0.7f));
	ControllerState state = {};
	TritonConfig::ApplyBinding(game->rightTrackpad.clickAction, &state);
	assert(state.rightStick);
	const char badRange[] = "version: 1\ndefaults:\n  mouse_joystick:\n    sensitivity_x: 20.001\n";
	assert(!TritonConfig::Parse(badRange, sizeof(badRange) - 1, &config, &error));
	const char badCrossField[] = "version: 1\ndefaults:\n  right_trackpad:\n    friction_min_ms: 31\n    friction_max_ms: 30\n";
	assert(!TritonConfig::Parse(badCrossField, sizeof(badCrossField) - 1, &config, &error));
	const char duplicate[] = "version: 1\ndefaults:\n  mouse_joystick:\n    sensitivity_x: 1\n    sensitivity_x: 2\n";
	assert(!TritonConfig::Parse(duplicate, sizeof(duplicate) - 1, &config, &error));
	const char badRelease[] = "version: 1\ndefaults:\n  right_trackpad:\n    release_haptics_intensity: 1.001\n";
	assert(!TritonConfig::Parse(badRelease, sizeof(badRelease) - 1, &config, &error));
}

static void TestRightTrackpadCoordinator() {
	TritonConfig::Config config;
	TritonConfig::Initialize(&config);
	TritonConfig::Profile profile = config.defaults;
	profile.rightTrackpad.mode = TritonConfig::kRightTrackpadMouseJoystick;
	profile.rightTrackpad.clickAction = TritonConfig::kBindingA;
	profile.mouseJoystick.smoothingMs = 0;
	profile.mouseJoystick.noiseSpeedThreshold = 0.0f;
	profile.mouseJoystick.sensitivityX = 0.1f;
	profile.rightTrackpad.haptics.movementIntensity = 0.0f;
	InputProcessing::RightTrackpadProcessor processor;
	TrackpadMotion::Geometry geometry = { 0, 1000, 0, 1000, false, false };
	processor.SetGeometry(geometry);
	processor.SetProfile(profile, false, false, 100);
	ControllerState physical = {};
	physical.rightX = 123;
	InputProcessing::Output output = processor.ProcessSample(
		physical, true, 100, 100, false, 100);
	assert(output.state.rightX == 0 && output.padActive); // Touchdown cannot jump.
	output = processor.ProcessSample(physical, true, 200, 100, true, 110);
	assert(output.state.rightX == 32767 && output.state.a);
	assert(output.haptic.kind == TrackpadHaptics::kRequestClick);
	output = processor.ProcessSample(physical, true, 200, 100, false, 120);
	assert(!output.state.a); // Only the synthetic held contribution is released.
	assert(output.haptic.kind == TrackpadHaptics::kRequestRelease);
	output = processor.ProcessSample(physical, true, 200, 100, false, 125);
	assert(output.haptic.kind == TrackpadHaptics::kRequestNone); // Once per release.
	physical.rightX = 20000;
	output = processor.ProcessSample(physical, true, 250, 100, false, 130);
	assert(output.state.rightX == 20000); // Physical stick takes priority exactly.
	physical.rightX = 0;
	output = processor.ProcessSample(physical, true, 250, 200, false, 140);
	assert(output.state.rightY == 32767); // Upward pad motion drives the camera up.
	output = processor.ProcessSample(physical, false, 0, 0, false, 150);
	assert(output.padActive); // A recent release starts a coast.
	output = processor.Advance(2000);
	assert(!output.padActive && output.state.rightX == 0);
	profile.rightTrackpad.mode = TritonConfig::kRightTrackpadDisabled;
	processor.SetProfile(profile, false, false, 3000);
	physical.rightX = 42;
	output = processor.ProcessSample(physical, true, 900, 900, true, 3010);
	assert(output.state.rightX == 42 && !output.state.a && !output.padActive);
}

static void TestRightTrackpadMovementHaptics() {
	TritonConfig::Config config;
	TritonConfig::Initialize(&config);
	TritonConfig::Profile profile = config.defaults;
	profile.rightTrackpad.mode = TritonConfig::kRightTrackpadMouseJoystick;
	profile.mouseJoystick.smoothingMs = 0;
	profile.mouseJoystick.noiseSpeedThreshold = 0.5f;
	profile.rightTrackpad.haptics.maximumHz = 100;
	profile.rightTrackpad.haptics.fullSpeed = 1.0f;
	InputProcessing::RightTrackpadProcessor processor;
	TrackpadMotion::Geometry geometry = { 0, 1000, 0, 1000, false, false };
	processor.SetGeometry(geometry);
	processor.SetProfile(profile, false, false, 0);
	ControllerState physical = {};
	processor.ProcessSample(physical, true, 500, 500, false, 0);
	for (uint32_t t = 10; t <= 500; t += 10) {
		// Jitter of 0.1 pad widths/second stays below the noise threshold.
		InputProcessing::Output output = processor.ProcessSample(physical, true,
			500 + (int32_t)((t / 10) & 1), 500, false, t);
		assert(output.haptic.kind == TrackpadHaptics::kRequestNone);
	}
	processor.ProcessSample(physical, false, 0, 0, false, 505);
	int32_t x = 0;
	processor.ProcessSample(physical, true, x, 500, false, 600);
	bool moved = false;
	for (uint32_t t = 610; t <= 640; t += 10) {
		x += 50; // Five pad widths/second.
		InputProcessing::Output output = processor.ProcessSample(physical, true, x, 500,
			false, t);
		moved = moved || output.haptic.kind == TrackpadHaptics::kRequestMovement;
	}
	assert(moved);
	assert(processor.ProcessSample(physical, false, 0, 0, false, 650).padActive);
	physical.rightX = 20000;
	processor.UpdatePhysical(physical, 660); // Takeover cancels the coast.
	physical.rightX = 0;
	processor.UpdatePhysical(physical, 670);
	for (uint32_t t = 675; t <= 1000; t += 5) {
		InputProcessing::Output output = processor.Advance(t);
		assert(!output.padActive);
		assert(output.haptic.kind == TrackpadHaptics::kRequestNone);
	}
}

int main() {
	TestMouseJoystickConversionAndFiltering();
	TestTrackpadContactAndRelease();
	TestTrackballCurvesAndTiming();
	TestTrackpadHapticScheduling();
	TestTrackpadHapticGainAndMailbox();
	TestHapticCommandEncoding();
	TestMouseJoystickConfiguration();
	TestRightTrackpadCoordinator();
	TestRightTrackpadMovementHaptics();
	TestWiredAdmissionAndSourceCapacity();
	TestTritonHidDescriptors();
	TestUsbOutputDescriptors();
	TestRumbleXamDispatch();
	TestRumbleDeadzoneAndCubicScaling();
	TestTunableRumbleScaling();
	TestConfigDefaultsAndBlackOpsProfile();
	TestConfigOverridesAndValidation();
	TestControllerCapabilities();
	TestRumbleEncoding();
	TestRumbleRefreshAndCoalescing();
	TestRumbleFailureAndReconnect();
	TestRumbleWrapAndSlotIsolation();
	TestUsbDescriptors();
	TestAdmissionAndValidation();
	TestStateIdsAndAxes();
	TestButtonsAndTriggers();
	TestRightPadDecoding();
	TestStatusAndFeature();
	TestRoutingConnectionOrders();
	TestRoutingDisconnectRetryAndGeneration();
	TestRoutingFailuresAndIsolation();
	TestRoutingRemovalOrdersAndGuideDebounce();
	return 0;
}
