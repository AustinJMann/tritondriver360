#pragma once

#include <stdint.h>

#include "mouse_joystick.h"

namespace TrackballMotion {

enum Curve {
	kCurveLinear = 1,
	kCurveEaseOutQuadratic = 2,
	kCurveEaseOutCubic = 3,
	kCurveEaseOutQuartic = 4
};

struct Settings {
	bool enabled;
	Curve curve;
	float frictionStrength;
	float frictionMaxSpeed;
	uint32_t frictionReferenceMs;
	uint32_t frictionMinMs;
	uint32_t frictionMaxMs;
	float verticalScale;
};

Settings DefaultSettings();

class Processor {
public:
	Processor();
	void Reset();
	bool Start(MouseJoystick::Vector2 releaseVelocity, uint32_t timestampMs,
		const Settings& settings, float noiseSpeedThreshold);
	MouseJoystick::Vector2 Advance(uint32_t timestampMs);
	bool IsCoasting() const { return coasting_; }
	MouseJoystick::Vector2 ReleaseVelocity() const { return releaseVelocity_; }

private:
	bool coasting_;
	uint32_t startMs_;
	uint32_t durationXMs_;
	uint32_t durationYMs_;
	Curve curve_;
	MouseJoystick::Vector2 releaseVelocity_;
};

} // namespace TrackballMotion
