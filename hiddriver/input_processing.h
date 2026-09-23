#pragma once

#include <stdint.h>

#include "mouse_joystick.h"
#include "trackball_motion.h"
#include "trackpad_haptics.h"
#include "trackpad_motion.h"
#include "triton_config.h"
#include "triton_protocol.h"

namespace InputProcessing {

struct Output {
	TritonProtocol::ControllerState state;
	TrackpadHaptics::Request haptic;
	bool padActive;
};

// Own one instance per physical source. All calls for an instance must run on
// the same serialized input/service context.
class RightTrackpadProcessor {
public:
	RightTrackpadProcessor();
	void SetGeometry(const TrackpadMotion::Geometry& geometry);
	void SetProfile(const TritonConfig::Profile& profile, bool currentContact,
		bool currentClick, uint32_t timestampMs);
	Output ProcessSample(const TritonProtocol::ControllerState& physical,
		bool contact, int32_t x, int32_t y, bool click, uint32_t timestampMs);
	Output UpdatePhysical(const TritonProtocol::ControllerState& physical,
		uint32_t timestampMs);
	Output ProcessButtons(const TritonProtocol::ControllerState& physical,
		bool click, uint32_t timestampMs);
	Output Advance(uint32_t timestampMs);
	void Reset();
	bool HasContact() const { return contact_; }

private:
	Output Compose(uint32_t timestampMs, bool clickRising, bool clickFalling,
		uint32_t elapsedMs);
	bool PhysicalHasPriority() const;

	TritonConfig::Profile profile_;
	TritonProtocol::ControllerState physical_;
	TrackpadMotion::Processor pad_;
	MouseJoystick::Processor mouse_;
	TrackballMotion::Processor trackball_;
	TrackpadHaptics::Scheduler haptics_;
	MouseJoystick::Output padOutput_;
	MouseJoystick::Vector2 motionVelocity_;
	bool haveProfile_;
	bool contact_;
	bool click_;
	uint32_t lastUpdateMs_;
};

} // namespace InputProcessing
