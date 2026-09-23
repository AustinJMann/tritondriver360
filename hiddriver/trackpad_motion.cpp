#include "trackpad_motion.h"

#include <string.h>

namespace TrackpadMotion {

namespace {

static const uint32_t kDiscontinuityMs = 100;
static const uint32_t kReleaseWindowMs = 40;

Result EmptyResult() {
	Result result;
	memset(&result, 0, sizeof(result));
	return result;
}

} // namespace

Processor::Processor() {
	geometry_.minimumX = geometry_.minimumY = 0;
	geometry_.maximumX = geometry_.maximumY = 1;
	geometry_.invertX = geometry_.invertY = false;
	Reset();
}

void Processor::Reset() {
	contact_ = false;
	baseX_ = baseY_ = 0;
	baseMs_ = lastSampleMs_ = 0;
	ClearHistory();
}

void Processor::SetGeometry(const Geometry& geometry) {
	geometry_ = geometry;
	Reset();
}

void Processor::ClearHistory() {
	historyStart_ = historyCount_ = 0;
	memset(history_, 0, sizeof(history_));
}

void Processor::AddHistory(MouseJoystick::Vector2 velocity, uint32_t endMs,
	uint32_t durationMs) {
	int index;
	if (historyCount_ < kHistoryCapacity) {
		index = (historyStart_ + historyCount_) % kHistoryCapacity;
		++historyCount_;
	} else {
		index = historyStart_;
		historyStart_ = (historyStart_ + 1) % kHistoryCapacity;
	}
	history_[index].velocity = velocity;
	history_[index].endMs = endMs;
	history_[index].durationMs = durationMs;
}

MouseJoystick::Vector2 Processor::EstimateRelease(uint32_t timestampMs) const {
	MouseJoystick::Vector2 result = { 0.0f, 0.0f };
	uint32_t totalWeight = 0;
	for (int n = 0; n < historyCount_; ++n) {
		const int index = (historyStart_ + n) % kHistoryCapacity;
		const HistoryEntry& entry = history_[index];
		const uint32_t age = timestampMs - entry.endMs;
		if (age >= kReleaseWindowMs) continue;
		uint32_t weight = entry.durationMs;
		const uint32_t available = kReleaseWindowMs - age;
		if (weight > available) weight = available;
		result.x += entry.velocity.x * weight;
		result.y += entry.velocity.y * weight;
		totalWeight += weight;
	}
	if (totalWeight) {
		result.x /= totalWeight;
		result.y /= totalWeight;
	}
	return result;
}

Result Processor::Sample(bool contact, int32_t x, int32_t y, uint32_t timestampMs) {
	Result result = EmptyResult();
	if (!contact) {
		if (contact_) {
			result.released = true;
			if (timestampMs - lastSampleMs_ <= kReleaseWindowMs)
				result.releaseVelocity = EstimateRelease(timestampMs);
		}
		contact_ = false;
		ClearHistory();
		return result;
	}
	if (!contact_) {
		contact_ = true;
		baseX_ = x;
		baseY_ = y;
		baseMs_ = lastSampleMs_ = timestampMs;
		ClearHistory();
		return result;
	}
	lastSampleMs_ = timestampMs;
	const uint32_t elapsed = timestampMs - baseMs_;
	if (!elapsed) return result;
	if (elapsed > kDiscontinuityMs || geometry_.maximumX <= geometry_.minimumX ||
		geometry_.maximumY <= geometry_.minimumY) {
		baseX_ = x;
		baseY_ = y;
		baseMs_ = timestampMs;
		ClearHistory();
		return result;
	}
	// Both axes use the physical X width. This avoids assigning unrelated unit
	// scales to axes whose raw coordinate ranges may differ.
	const float width = (float)(geometry_.maximumX - geometry_.minimumX);
	float dx = (float)(x - baseX_) / width;
	float dy = (float)(y - baseY_) / width;
	if (geometry_.invertX) dx = -dx;
	if (geometry_.invertY) dy = -dy;
	result.contactVelocity.x = dx * 1000.0f / elapsed;
	result.contactVelocity.y = dy * 1000.0f / elapsed;
	result.elapsedMs = elapsed;
	result.hasContactVelocity = true;
	AddHistory(result.contactVelocity, timestampMs, elapsed);
	baseX_ = x;
	baseY_ = y;
	baseMs_ = timestampMs;
	return result;
}

bool Processor::Expire(uint32_t timestampMs, uint32_t timeoutMs) {
	if (!contact_ || timestampMs - lastSampleMs_ <= timeoutMs) return false;
	contact_ = false;
	ClearHistory();
	return true;
}

} // namespace TrackpadMotion
