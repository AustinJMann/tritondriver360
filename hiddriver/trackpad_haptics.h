#pragma once

#include <stdint.h>

namespace TrackpadHaptics {

// Firmware clamps HAPTIC_COMMAND gain to this range.
static const int8_t kMinimumGainDb = -23;
static const int8_t kMaximumGainDb = 24;
// Undelivered pulses older than this are dropped rather than played late.
static const uint32_t kMaximumPulseAgeMs = 25;

struct Settings {
	float movementIntensity;
	float clickIntensity;
	float releaseIntensity;
	uint32_t maximumHz;
	float fullSpeed;
};

enum RequestKind { kRequestNone, kRequestMovement, kRequestClick, kRequestRelease };

struct Request {
	RequestKind kind;
	float intensity;
};

struct Pulse {
	RequestKind kind;
	int8_t gainDb;
	uint32_t generation;
	uint32_t createdAt;
};

Settings DefaultSettings();

// Intensity is an amplitude fraction of the firmware maximum gain.
int8_t GainDb(float intensity);

class Scheduler {
public:
	Scheduler();
	void Reset();
	Request Update(float speed, bool clickRising, bool clickFalling,
		uint32_t elapsedMs, const Settings& settings);

private:
	float phase_;
};

// Holds at most one undelivered pulse. Click and release pulses are never
// replaced by movement.
class Mailbox {
public:
	Mailbox();
	void Reset();
	void Offer(RequestKind kind, int8_t gainDb, uint32_t generation, uint32_t now);
	bool Take(uint32_t now, Pulse* pulse);

private:
	Pulse pending_;
};

} // namespace TrackpadHaptics
