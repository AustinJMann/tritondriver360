#pragma once

#include <stdint.h>

#include "mouse_joystick.h"

namespace TrackpadMotion {

struct Geometry {
	int32_t minimumX;
	int32_t maximumX;
	int32_t minimumY;
	int32_t maximumY;
	bool invertX;
	bool invertY;
};

struct Result {
	bool hasContactVelocity;
	bool released;
	MouseJoystick::Vector2 contactVelocity;
	MouseJoystick::Vector2 releaseVelocity;
	uint32_t elapsedMs;
};

class Processor {
public:
	Processor();
	void Reset();
	void SetGeometry(const Geometry& geometry);
	Result Sample(bool contact, int32_t x, int32_t y, uint32_t timestampMs);
	bool Expire(uint32_t timestampMs, uint32_t timeoutMs);
	bool HasContact() const { return contact_; }

private:
	struct HistoryEntry {
		MouseJoystick::Vector2 velocity;
		uint32_t endMs;
		uint32_t durationMs;
	};
	static const int kHistoryCapacity = 64;
	void ClearHistory();
	void AddHistory(MouseJoystick::Vector2 velocity, uint32_t endMs, uint32_t durationMs);
	MouseJoystick::Vector2 EstimateRelease(uint32_t timestampMs) const;

	Geometry geometry_;
	bool contact_;
	int32_t baseX_;
	int32_t baseY_;
	uint32_t baseMs_;
	uint32_t lastSampleMs_;
	HistoryEntry history_[kHistoryCapacity];
	int historyStart_;
	int historyCount_;
};

} // namespace TrackpadMotion
