#include "input_processing.h"

#include <math.h>
#include <string.h>

namespace InputProcessing {

namespace {

TrackpadHaptics::Request NoHaptic() {
	TrackpadHaptics::Request request = { TrackpadHaptics::kRequestNone, 0.0f };
	return request;
}

float Speed(MouseJoystick::Vector2 velocity) {
	return (float)sqrt((double)velocity.x * velocity.x + (double)velocity.y * velocity.y);
}

} // namespace

RightTrackpadProcessor::RightTrackpadProcessor() { Reset(); }

void RightTrackpadProcessor::SetGeometry(const TrackpadMotion::Geometry& geometry) {
	pad_.SetGeometry(geometry);
	contact_ = false;
	mouse_.Reset();
	trackball_.Reset();
}

void RightTrackpadProcessor::Reset() {
	memset(&profile_, 0, sizeof(profile_));
	memset(&physical_, 0, sizeof(physical_));
	pad_.Reset();
	mouse_.Reset();
	trackball_.Reset();
	haptics_.Reset();
	padOutput_.x = padOutput_.y = 0.0f;
	motionVelocity_.x = motionVelocity_.y = 0.0f;
	haveProfile_ = contact_ = click_ = false;
	lastUpdateMs_ = 0;
}

void RightTrackpadProcessor::SetProfile(const TritonConfig::Profile& profile,
	bool currentContact, bool currentClick, uint32_t timestampMs) {
	profile_ = profile;
	haveProfile_ = true;
	pad_.Reset();
	mouse_.Reset();
	trackball_.Reset();
	haptics_.Reset();
	padOutput_.x = padOutput_.y = 0.0f;
	motionVelocity_.x = motionVelocity_.y = 0.0f;
	contact_ = currentContact;
	click_ = currentClick;
	lastUpdateMs_ = timestampMs;
	// A held contact is deliberately not installed in the pad adapter: the next
	// coordinate sample becomes its baseline and cannot jump.
}

bool RightTrackpadProcessor::PhysicalHasPriority() const {
	float x = physical_.rightX >= 0 ? physical_.rightX / 32767.0f :
		physical_.rightX / 32768.0f;
	float y = physical_.rightY >= 0 ? physical_.rightY / 32767.0f :
		physical_.rightY / 32768.0f;
	float magnitude = (float)sqrt((double)x * x + (double)y * y);
	return magnitude > profile_.rightTrackpad.physicalStickThreshold;
}

Output RightTrackpadProcessor::ProcessSample(
	const TritonProtocol::ControllerState& physical, bool contact, int32_t x,
	int32_t y, bool click, uint32_t timestampMs) {
	physical_ = physical;
	const bool clickRising = click && !click_;
	const bool clickFalling = !click && click_;
	const uint32_t elapsed = timestampMs - lastUpdateMs_;
	lastUpdateMs_ = timestampMs;
	click_ = click;
	if (!haveProfile_ || profile_.rightTrackpad.mode == TritonConfig::kRightTrackpadDisabled) {
		contact_ = contact;
		Output output = { physical_, NoHaptic(), false };
		return output;
	}
	const bool newContact = contact && !contact_;
	TrackpadMotion::Result result = pad_.Sample(contact, x, y, timestampMs);
	contact_ = contact;
	if (newContact) {
		trackball_.Reset();
		mouse_.Reset();
		padOutput_.x = padOutput_.y = 0.0f;
		motionVelocity_.x = motionVelocity_.y = 0.0f;
	}
	if (result.hasContactVelocity) {
		padOutput_ = mouse_.Update(result.contactVelocity, result.elapsedMs,
			profile_.mouseJoystick);
		// Haptics follow accepted motion, so a resting finger stays silent.
		motionVelocity_ = result.contactVelocity;
		if (!mouse_.IsMoving()) motionVelocity_.x = motionVelocity_.y = 0.0f;
	} else if (result.released) {
		mouse_.Reset();
		padOutput_.x = padOutput_.y = 0.0f;
		motionVelocity_.x = motionVelocity_.y = 0.0f;
		trackball_.Start(result.releaseVelocity, timestampMs,
			profile_.rightTrackpad.trackball,
			profile_.mouseJoystick.noiseSpeedThreshold);
	}
	return Compose(timestampMs, clickRising, clickFalling, elapsed);
}

Output RightTrackpadProcessor::UpdatePhysical(
	const TritonProtocol::ControllerState& physical, uint32_t timestampMs) {
	physical_ = physical;
	Output output = Compose(timestampMs, false, false, 0);
	output.haptic = NoHaptic();
	return output;
}

Output RightTrackpadProcessor::ProcessButtons(
	const TritonProtocol::ControllerState& physical, bool click,
	uint32_t timestampMs) {
	physical_ = physical;
	const bool clickRising = click && !click_;
	const bool clickFalling = !click && click_;
	const uint32_t elapsed = timestampMs - lastUpdateMs_;
	lastUpdateMs_ = timestampMs;
	click_ = click;
	return Compose(timestampMs, clickRising, clickFalling, elapsed);
}

Output RightTrackpadProcessor::Advance(uint32_t timestampMs) {
	const uint32_t elapsed = timestampMs - lastUpdateMs_;
	lastUpdateMs_ = timestampMs;
	if (!haveProfile_ || profile_.rightTrackpad.mode == TritonConfig::kRightTrackpadDisabled) {
		Output output = { physical_, NoHaptic(), false };
		return output;
	}
	if (pad_.Expire(timestampMs, 100)) {
		contact_ = false;
		mouse_.Reset();
		padOutput_.x = padOutput_.y = 0.0f;
		motionVelocity_.x = motionVelocity_.y = 0.0f;
	}
	return Compose(timestampMs, false, false, elapsed);
}

Output RightTrackpadProcessor::Compose(uint32_t timestampMs, bool clickRising,
	bool clickFalling, uint32_t elapsedMs) {
	Output output;
	output.state = physical_;
	if (profile_.rightTrackpad.mode == TritonConfig::kRightTrackpadDisabled) {
		output.haptic = NoHaptic();
		output.padActive = false;
		return output;
	}
	if (PhysicalHasPriority()) {
		// Takeover cancels the coast, including its movement haptics.
		if (trackball_.IsCoasting()) motionVelocity_.x = motionVelocity_.y = 0.0f;
		trackball_.Reset();
	} else if (trackball_.IsCoasting()) {
		motionVelocity_ = trackball_.Advance(timestampMs);
		// The release direction picks the dominant axis for the whole coast,
		// even though the axes decay at different rates.
		padOutput_ = MouseJoystick::Processor::Convert(motionVelocity_,
			trackball_.ReleaseVelocity(), profile_.mouseJoystick);
		if (motionVelocity_.x != 0.0f &&
			(motionVelocity_.x < 0.0f ? -motionVelocity_.x : motionVelocity_.x) *
			profile_.mouseJoystick.sensitivityX < 0.001f) padOutput_.x = 0.0f;
		if (motionVelocity_.y != 0.0f &&
			(motionVelocity_.y < 0.0f ? -motionVelocity_.y : motionVelocity_.y) *
			profile_.mouseJoystick.sensitivityY < 0.001f) padOutput_.y = 0.0f;
		if (padOutput_.x == 0.0f && padOutput_.y == 0.0f) trackball_.Reset();
		output.state.rightX = MouseJoystick::Processor::ToStickAxis(padOutput_.x);
		output.state.rightY = MouseJoystick::Processor::ToStickAxis(padOutput_.y);
	} else if (contact_) {
		output.state.rightX = MouseJoystick::Processor::ToStickAxis(padOutput_.x);
		output.state.rightY = MouseJoystick::Processor::ToStickAxis(padOutput_.y);
	}
	output.padActive = contact_ || trackball_.IsCoasting();
	if (click_) TritonConfig::ApplyBinding(profile_.rightTrackpad.clickAction, &output.state);
	output.haptic = haptics_.Update(Speed(motionVelocity_), clickRising, clickFalling,
		elapsedMs, profile_.rightTrackpad.haptics);
	return output;
}

} // namespace InputProcessing
