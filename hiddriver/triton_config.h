#pragma once

#include <stddef.h>
#include <stdint.h>

#include "rumble_output.h"
#include "triton_protocol.h"

namespace TritonConfig {

static const size_t kMaxGames = 32;

enum Paddle {
	kPaddleR4,
	kPaddleR5,
	kPaddleL4,
	kPaddleL5,
	kPaddleCount
};

enum Binding {
	kBindingNone,
	kBindingA,
	kBindingB,
	kBindingX,
	kBindingY,
	kBindingDpadUp,
	kBindingDpadDown,
	kBindingDpadLeft,
	kBindingDpadRight,
	kBindingLeftShoulder,
	kBindingRightShoulder,
	kBindingLeftStick,
	kBindingRightStick,
	kBindingStart,
	kBindingBack,
	kBindingGuide,
	kBindingLeftTrigger,
	kBindingRightTrigger
};

struct Profile {
	Binding paddles[kPaddleCount];
	RumbleOutput::Settings rumble;
};

struct GameProfile {
	uint32_t titleId;
	Profile profile;
};

struct Config {
	Profile defaults;
	GameProfile games[kMaxGames];
	size_t gameCount;
};

struct ParseError {
	size_t line;
	const char* message;
};

void Initialize(Config* config);
bool Parse(const char* text, size_t length, Config* config, ParseError* error);
const Profile* FindProfile(const Config& config, uint32_t titleId);
void ApplyPaddleBindings(const Profile& profile, TritonProtocol::ControllerState* state);
const char* DefaultFileText();
size_t DefaultFileSize();

} // namespace TritonConfig
