#include "trackpad_haptics.h"

#include <math.h>

namespace TrackpadHaptics {

Settings DefaultSettings() {
	Settings settings;
	settings.movementIntensity = 0.25f;
	settings.clickIntensity = 0.7f;
	settings.releaseIntensity = 0.35f;
	settings.maximumHz = 80;
	settings.fullSpeed = 6.0f;
	return settings;
}

int8_t GainDb(float intensity) {
	if (intensity >= 1.0f) return kMaximumGainDb;
	if (intensity <= 0.0f) return kMinimumGainDb;
	double gain = kMaximumGainDb + 20.0 * log10((double)intensity);
	if (gain <= kMinimumGainDb) return kMinimumGainDb;
	return (int8_t)floor(gain + 0.5);
}

Scheduler::Scheduler() { Reset(); }

void Scheduler::Reset() { phase_ = 0.0f; }

Request Scheduler::Update(float speed, bool clickRising, bool clickFalling,
	uint32_t elapsedMs, const Settings& settings) {
	Request request = { kRequestNone, 0.0f };
	if (clickRising && settings.clickIntensity > 0.0f) {
		request.kind = kRequestClick;
		request.intensity = settings.clickIntensity;
		return request;
	}
	if (clickFalling && settings.releaseIntensity > 0.0f) {
		request.kind = kRequestRelease;
		request.intensity = settings.releaseIntensity;
		return request;
	}
	if (speed <= 0.0f || settings.movementIntensity <= 0.0f) {
		phase_ = 0.0f;
		return request;
	}
	float ratio = speed / settings.fullSpeed;
	if (ratio > 1.0f) ratio = 1.0f;
	phase_ += elapsedMs * settings.maximumHz * ratio / 1000.0f;
	if (phase_ < 1.0f) return request;
	// Coalesce missed pulses rather than dispatching a catch-up burst.
	phase_ -= (uint32_t)phase_;
	request.kind = kRequestMovement;
	request.intensity = settings.movementIntensity;
	return request;
}

Mailbox::Mailbox() { Reset(); }

void Mailbox::Reset() {
	pending_.kind = kRequestNone;
	pending_.gainDb = 0;
	pending_.generation = 0;
	pending_.createdAt = 0;
}

void Mailbox::Offer(RequestKind kind, int8_t gainDb, uint32_t generation,
	uint32_t now) {
	if (kind == kRequestNone) return;
	if (kind == kRequestMovement && pending_.kind != kRequestNone &&
		pending_.kind != kRequestMovement &&
		pending_.generation == generation &&
		now - pending_.createdAt <= kMaximumPulseAgeMs) return;
	pending_.kind = kind;
	pending_.gainDb = gainDb;
	pending_.generation = generation;
	pending_.createdAt = now;
}

bool Mailbox::Take(uint32_t now, Pulse* pulse) {
	if (pending_.kind == kRequestNone) return false;
	const bool fresh = now - pending_.createdAt <= kMaximumPulseAgeMs;
	if (fresh && pulse) *pulse = pending_;
	pending_.kind = kRequestNone;
	return fresh && pulse != 0;
}

} // namespace TrackpadHaptics
