#include "trackball_motion.h"

#include <math.h>

namespace TrackballMotion {

namespace {

uint32_t ClampDuration(float value, uint32_t minimum, uint32_t maximum) {
	if (value <= (float)minimum) return minimum;
	if (value >= (float)maximum) return maximum;
	return (uint32_t)(value + 0.5f);
}

float Remaining(uint32_t elapsed, uint32_t duration, int power) {
	if (!duration || elapsed >= duration) return 0.0f;
	float remaining = 1.0f - (float)elapsed / (float)duration;
	float result = remaining;
	for (int i = 1; i < power; ++i) result *= remaining;
	return result;
}

} // namespace

Settings DefaultSettings() {
	Settings settings;
	settings.enabled = true;
	settings.curve = kCurveEaseOutQuartic;
	settings.frictionStrength = 1.0f;
	settings.frictionMaxSpeed = 0.0f;
	settings.frictionReferenceMs = 450;
	settings.frictionMinMs = 30;
	settings.frictionMaxMs = 1500;
	settings.verticalScale = 0.5f;
	return settings;
}

Processor::Processor() { Reset(); }

void Processor::Reset() {
	coasting_ = false;
	startMs_ = durationXMs_ = durationYMs_ = 0;
	curve_ = kCurveEaseOutQuartic;
	releaseVelocity_.x = releaseVelocity_.y = 0.0f;
}

bool Processor::Start(MouseJoystick::Vector2 velocity, uint32_t timestampMs,
	const Settings& settings, float noiseSpeedThreshold) {
	Reset();
	if (!settings.enabled) return false;
	float speed = (float)sqrt((double)velocity.x * velocity.x +
		(double)velocity.y * velocity.y);
	if (speed <= noiseSpeedThreshold || speed == 0.0f) return false;
	if (settings.frictionMaxSpeed > 0.0f && speed > settings.frictionMaxSpeed)
		speed = settings.frictionMaxSpeed;
	const float duration = settings.frictionReferenceMs * speed / settings.frictionStrength;
	const float verticalMultiplier = 0.25f + 1.5f * settings.verticalScale;
	// Scale before clamping so both axes stay within the configured bounds.
	durationXMs_ = ClampDuration(duration, settings.frictionMinMs, settings.frictionMaxMs);
	durationYMs_ = ClampDuration(duration / verticalMultiplier, settings.frictionMinMs,
		settings.frictionMaxMs);
	startMs_ = timestampMs;
	curve_ = settings.curve;
	releaseVelocity_ = velocity;
	coasting_ = true;
	return true;
}

MouseJoystick::Vector2 Processor::Advance(uint32_t timestampMs) {
	MouseJoystick::Vector2 velocity = { 0.0f, 0.0f };
	if (!coasting_) return velocity;
	const uint32_t elapsed = timestampMs - startMs_;
	velocity.x = releaseVelocity_.x * Remaining(elapsed, durationXMs_, (int)curve_);
	velocity.y = releaseVelocity_.y * Remaining(elapsed, durationYMs_, (int)curve_);
	if (velocity.x == 0.0f && velocity.y == 0.0f) coasting_ = false;
	return velocity;
}

} // namespace TrackballMotion
