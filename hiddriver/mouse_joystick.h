#pragma once

#include <stdint.h>

namespace MouseJoystick {

struct Vector2 {
	float x;
	float y;
};

struct Settings {
	float sensitivityX;
	float sensitivityY;
	float minimumOutputX;
	float minimumOutputY;
	uint32_t smoothingMs;
	float noiseSpeedThreshold;
};

struct Output {
	float x;
	float y;
};

Settings DefaultSettings();

// Source-neutral velocity filter and stick converter. Velocity is expressed in
// logical motion units per second; one trackpad width is one logical unit.
class Processor {
public:
	Processor();
	void Reset();
	Output Update(Vector2 velocity, uint32_t elapsedMs, const Settings& settings);
	// Minimum output applies only to the axis that dominates direction, so
	// off-axis drift is not raised to the minimum.
	static Output Convert(Vector2 velocity, Vector2 direction, const Settings& settings);
	static int16_t ToStickAxis(float value);
	bool IsMoving() const { return moving_; }

private:
	bool moving_;
	Vector2 filtered_;
};

} // namespace MouseJoystick
