#include "mouse_joystick.h"

#include <math.h>

namespace MouseJoystick {

namespace {

float Clamp01(float value) {
	if (value <= 0.0f) return 0.0f;
	if (value >= 1.0f) return 1.0f;
	return value;
}

float Magnitude(Vector2 value) {
	return (float)sqrt((double)value.x * value.x + (double)value.y * value.y);
}

float ConvertAxis(float velocity, float sensitivity, float minimumOutput) {
	if (velocity == 0.0f) return 0.0f;
	float magnitude = Clamp01((velocity < 0.0f ? -velocity : velocity) * sensitivity);
	float output = minimumOutput + (1.0f - minimumOutput) * magnitude;
	return velocity < 0.0f ? -output : output;
}

} // namespace

Settings DefaultSettings() {
	Settings settings;
	settings.sensitivityX = 1.0f;
	settings.sensitivityY = 1.0f;
	settings.minimumOutputX = 0.0f;
	settings.minimumOutputY = 0.0f;
	settings.smoothingMs = 8;
	settings.noiseSpeedThreshold = 0.015f;
	return settings;
}

Processor::Processor() { Reset(); }

void Processor::Reset() {
	moving_ = false;
	filtered_.x = filtered_.y = 0.0f;
}

Output Processor::Update(Vector2 velocity, uint32_t elapsedMs, const Settings& settings) {
	const float speed = Magnitude(velocity);
	const float threshold = moving_ ? settings.noiseSpeedThreshold * 0.75f :
		settings.noiseSpeedThreshold;
	const bool accepted = settings.noiseSpeedThreshold == 0.0f ? speed != 0.0f :
		(moving_ ? speed >= threshold : speed > threshold);
	if (!accepted) {
		Reset();
		Output zero = { 0.0f, 0.0f };
		return zero;
	}
	moving_ = true;
	if (!settings.smoothingMs) {
		filtered_ = velocity;
	} else if (elapsedMs) {
		const float alpha = (float)elapsedMs / (settings.smoothingMs + (float)elapsedMs);
		filtered_.x += (velocity.x - filtered_.x) * alpha;
		filtered_.y += (velocity.y - filtered_.y) * alpha;
	}
	return Convert(filtered_, filtered_, settings);
}

Output Processor::Convert(Vector2 velocity, Vector2 direction, const Settings& settings) {
	const float x = direction.x < 0.0f ? -direction.x : direction.x;
	const float y = direction.y < 0.0f ? -direction.y : direction.y;
	Output output;
	output.x = ConvertAxis(velocity.x, settings.sensitivityX,
		x >= y ? settings.minimumOutputX : 0.0f);
	output.y = ConvertAxis(velocity.y, settings.sensitivityY,
		y >= x ? settings.minimumOutputY : 0.0f);
	return output;
}

int16_t Processor::ToStickAxis(float value) {
	if (value >= 1.0f) return 32767;
	if (value <= -1.0f) return -32768;
	if (value > 0.0f) return (int16_t)(value * 32767.0f + 0.5f);
	if (value < 0.0f) return (int16_t)(value * 32768.0f - 0.5f);
	return 0;
}

} // namespace MouseJoystick
