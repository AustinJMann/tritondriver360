#pragma once

#include <stdint.h>

namespace RumbleOutput {

// SDL resends every 40 ms for a roughly 50 ms firmware timeout. Leave more
// scheduling headroom for the shared control endpoint on the Xbox.
static const uint32_t kRefreshMs = 30;
static const uint32_t kServiceMs = 5;
// Ignore the small motor pulses some games use before their main rumble event.
// Kept as a named threshold so hardware testing can tune it independently of
// the response curve.
static const uint16_t kIntensityDeadzone = 6554; // 10% of the XInput range.

enum Curve {
	kCurveLinear,
	kCurveQuadratic,
	kCurveCubic
};

struct Settings {
	bool enabled;
	uint16_t deadzone;
	uint16_t leftGainPermille;
	uint16_t rightGainPermille;
	Curve curve;
};

inline Settings DefaultSettings() {
	Settings settings = { true, kIntensityDeadzone, 2000, 2000, kCurveCubic };
	return settings;
}

inline uint64_t Request(uint32_t generation, uint16_t left, uint16_t right) {
	return ((uint64_t)generation << 32) | ((uint32_t)left << 16) | right;
}
inline uint32_t Generation(uint64_t request) { return (uint32_t)(request >> 32); }
inline uint16_t Left(uint64_t request) { return (uint16_t)(request >> 16); }
inline uint16_t Right(uint64_t request) { return (uint16_t)request; }
inline bool Active(uint64_t request) { return (uint32_t)request != 0; }

// Remove the low-end hardware floor, apply the selected response curve and
// gain, then clamp. Renormalizing after the deadzone preserves full output.
inline uint16_t ScaleIntensity(uint16_t value, uint16_t deadzone,
	Curve curve, uint16_t gainPermille) {
	if (value <= deadzone || gainPermille == 0) return 0;
	const uint32_t span = 65535u - deadzone;
	const uint32_t adjusted = (uint32_t)value - deadzone;
	const uint32_t normalized = (adjusted * 65535u + span / 2) / span;
	uint32_t curved = normalized;
	if (curve != kCurveLinear) {
		const uint32_t squared = (normalized * normalized + 32767u) / 65535u;
		curved = curve == kCurveQuadratic ? squared :
			(squared * normalized + 32767u) / 65535u;
	}
	uint32_t scaled = (curved * gainPermille + 500u) / 1000u;
	return (uint16_t)(scaled > 65535u ? 65535u : scaled);
}

inline uint16_t ScaleIntensity(uint16_t value) {
	Settings settings = DefaultSettings();
	return ScaleIntensity(value, settings.deadzone, settings.curve,
		settings.leftGainPermille);
}

// Route by ownership, never by the native driver's return code. XAM may accept
// a virtual device without delivering its motor values to our USB transport.
// Backend keeps platform-specific binding checks and atomic publication outside
// this dispatch policy so host tests exercise the same decision as the hook.
template <typename Vibration, typename Backend>
uint32_t SetState(uint32_t user, uint32_t flags, Vibration* vibration, Backend& backend,
	const Settings& settings) {
	uint32_t normalizedUser = (user & 0xff) == 0xff ? 0 : user;
	typename Backend::Target target;
	if (!backend.Find(normalizedUser, &target))
		return backend.Native(user, flags, vibration);
	if (!vibration) return 87; // ERROR_INVALID_PARAMETER
	if (!settings.enabled) return backend.Submit(target, 0, 0);
	return backend.Submit(target,
		ScaleIntensity(vibration->wLeftMotorSpeed, settings.deadzone,
			settings.curve, settings.leftGainPermille),
		ScaleIntensity(vibration->wRightMotorSpeed, settings.deadzone,
			settings.curve, settings.rightGainPermille));
}

template <typename Vibration, typename Backend>
uint32_t SetState(uint32_t user, uint32_t flags, Vibration* vibration, Backend& backend) {
	Settings settings = DefaultSettings();
	return SetState(user, flags, vibration, backend, settings);
}

// Zero-initializable; used only on the USB processor at dispatch IRQL.
// desired is a snapshot of the atomic, generation-tagged XAM mailbox.
struct State {
	uint64_t desired;
	uint64_t acknowledged;
	uint64_t inFlight;
	uint32_t submittedAt;
	uint32_t retryAt;
	uint32_t failures;
	bool pending;

	void Update(uint64_t request) {
		if (Generation(desired) != Generation(request)) {
			acknowledged = 0;
			failures = 0;
		}
		desired = request;
	}

	bool Due(uint32_t now) const {
		if (pending || Generation(desired) == 0) return false;
		if (failures && (int32_t)(now - retryAt) < 0) return false;
		return failures != 0 || desired != acknowledged ||
			(Active(desired) && (uint32_t)(now - submittedAt) >= kRefreshMs);
	}

	void Submitted(uint32_t now) {
		inFlight = desired;
		submittedAt = now;
		pending = true;
	}

	void Complete(bool success, uint32_t now) {
		pending = false;
		// A completion from the previous binding must not acknowledge the new one.
		if (Generation(inFlight) != Generation(desired)) return;
		if (success) {
			acknowledged = inFlight;
			failures = 0;
		} else {
			if (failures < 6) ++failures;
			uint32_t delay = 10u << (failures - 1);
			retryAt = now + (delay > 250 ? 250 : delay);
		}
	}
};

} // namespace RumbleOutput
