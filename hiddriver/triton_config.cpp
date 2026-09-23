#include "triton_config.h"

#include <string.h>

namespace TritonConfig {

namespace {

enum Section { kSectionNone, kSectionDefaults, kSectionGames };
enum Subsection { kSubsectionNone, kSubsectionPaddles, kSubsectionRumble,
	kSubsectionMouseJoystick, kSubsectionRightTrackpad };

static const uint8_t kRumbleEnabled = 0x01;
static const uint8_t kRumbleLeftGain = 0x02;
static const uint8_t kRumbleRightGain = 0x04;
static const uint8_t kRumbleDeadzone = 0x08;
static const uint8_t kRumbleCurve = 0x10;

static const uint8_t kMouseSensitivityX = 0x01;
static const uint8_t kMouseSensitivityY = 0x02;
static const uint8_t kMouseMinimumX = 0x04;
static const uint8_t kMouseMinimumY = 0x08;
static const uint8_t kMouseSmoothing = 0x10;
static const uint8_t kMouseNoise = 0x20;

static const uint16_t kPadMode = 0x0001;
static const uint16_t kPadClickAction = 0x0002;
static const uint16_t kPadTrackballEnabled = 0x0004;
static const uint16_t kPadFrictionCurve = 0x0008;
static const uint16_t kPadFrictionStrength = 0x0010;
static const uint16_t kPadFrictionMaxSpeed = 0x0020;
static const uint16_t kPadFrictionReference = 0x0040;
static const uint16_t kPadFrictionMinimum = 0x0080;
static const uint16_t kPadFrictionMaximum = 0x0100;
static const uint16_t kPadFrictionVertical = 0x0200;
static const uint16_t kPadHapticsIntensity = 0x0400;
static const uint16_t kPadClickHapticsIntensity = 0x0800;
static const uint16_t kPadReleaseHapticsIntensity = 0x1000;
static const uint16_t kPadHapticsMaximumHz = 0x2000;
static const uint16_t kPadHapticsFullSpeed = 0x4000;
static const uint16_t kPadPhysicalStickThreshold = 0x8000;

static const char kDefaultConfig[] =
	"# TritonDriver configuration. Game keys are eight-digit Xbox 360 Title IDs.\r\n"
	"version: 1\r\n"
	"\r\n"
	"defaults:\r\n"
	"  paddles:\r\n"
	"    r4: none\r\n"
	"    r5: none\r\n"
	"    l4: none\r\n"
	"    l5: none\r\n"
	"  rumble:\r\n"
	"    enabled: true\r\n"
	"    left_gain: 2.0\r\n"
	"    right_gain: 2.0\r\n"
	"    deadzone: 0.10\r\n"
	"    curve: cubic\r\n"
	"  mouse_joystick:\r\n"
	"    sensitivity_x: 0.5\r\n"
	"    sensitivity_y: 0.5\r\n"
	"    minimum_output_x: 0.2\r\n"
	"    minimum_output_y: 0.2\r\n"
	"    smoothing_ms: 8\r\n"
	"    noise_speed_threshold: 0.2\r\n"
	"  right_trackpad:\r\n"
	"    mode: mouse_joystick\r\n"
	"    click_action: right_stick\r\n"
	"    trackball_enabled: true\r\n"
	"    friction_curve: ease_out_cubic\r\n"
	"    friction_strength: 3\r\n"
	"    friction_max_speed: 6\r\n"
	"    friction_reference_ms: 150\r\n"
	"    friction_min_ms: 30\r\n"
	"    friction_max_ms: 300\r\n"
	"    friction_vertical_scale: 0.5\r\n"
	"    # Right-pad haptic strength as a fraction of the maximum; 0 mutes.\r\n"
	"    haptics_intensity: 0.25\r\n"
	"    click_haptics_intensity: 0.7\r\n"
	"    release_haptics_intensity: 0.35\r\n"
	"    haptics_max_hz: 80\r\n"
	"    haptics_full_speed: 6.0\r\n"
	"    physical_stick_threshold: 0.15\r\n"
	"\r\n"
	"games:\r\n"
	"  \"415608C3\": # Call of Duty: Black Ops II\r\n"
	"    paddles:\r\n"
	"      r4: x\r\n"
	"      r5: y\r\n"
	"      l4: a\r\n"
	"      l5: b\r\n";

void SetError(ParseError* error, size_t line, const char* message) {
	if (!error) return;
	error->line = line;
	error->message = message;
}

char Lower(char c) {
	return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
}

bool Equals(const char* value, const char* expected) {
	while (*value && *expected) {
		if (Lower(*value++) != Lower(*expected++)) return false;
	}
	return *value == 0 && *expected == 0;
}

char* Trim(char* value) {
	while (*value == ' ') ++value;
	size_t length = strlen(value);
	while (length && value[length - 1] == ' ') value[--length] = 0;
	return value;
}

bool Unquote(char* value) {
	size_t length = strlen(value);
	if (!length) return true;
	if (value[0] != '\'' && value[0] != '"') return true;
	if (length < 2 || value[length - 1] != value[0]) return false;
	memmove(value, value + 1, length - 2);
	value[length - 2] = 0;
	return true;
}

char* FindColon(char* line) {
	char quote = 0;
	for (char* p = line; *p; ++p) {
		if ((*p == '\'' || *p == '"') && (!quote || quote == *p)) quote = quote ? 0 : *p;
		else if (*p == ':' && !quote) return p;
	}
	return 0;
}

void StripComment(char* line) {
	char quote = 0;
	for (char* p = line; *p; ++p) {
		if ((*p == '\'' || *p == '"') && (!quote || quote == *p)) quote = quote ? 0 : *p;
		else if (*p == '#' && !quote) { *p = 0; return; }
	}
}

bool ParseUnsigned(const char* value, uint32_t* result) {
	if (!*value) return false;
	uint32_t parsed = 0;
	for (; *value; ++value) {
		if (*value < '0' || *value > '9') return false;
		uint32_t digit = (uint32_t)(*value - '0');
		if (parsed > 429496729u || (parsed == 429496729u && digit > 5u))
			return false;
		parsed = parsed * 10u + digit;
	}
	*result = parsed;
	return true;
}

bool ParsePermille(const char* value, uint16_t maximum, uint16_t* result) {
	if (!*value) return false;
	uint32_t whole = 0;
	uint32_t fraction = 0;
	uint32_t fractionDigits = 0;
	bool dot = false;
	bool digitSeen = false;
	for (; *value; ++value) {
		if (*value == '.' && !dot) { dot = true; continue; }
		if (*value < '0' || *value > '9') return false;
		digitSeen = true;
		if (!dot) {
			if (whole > 10) return false;
			whole = whole * 10u + (uint32_t)(*value - '0');
		} else {
			if (fractionDigits >= 3) return false;
			fraction = fraction * 10u + (uint32_t)(*value - '0');
			++fractionDigits;
		}
	}
	if (!digitSeen) return false;
	while (fractionDigits < 3) { fraction *= 10u; ++fractionDigits; }
	uint32_t scaled = whole * 1000u + fraction;
	if (scaled > maximum) return false;
	*result = (uint16_t)scaled;
	return true;
}

bool ParseTitleId(const char* value, uint32_t* titleId) {
	if (strlen(value) != 8) return false;
	uint32_t parsed = 0;
	for (int i = 0; i < 8; ++i) {
		char c = value[i];
		uint32_t digit;
		if (c >= '0' && c <= '9') digit = (uint32_t)(c - '0');
		else if (Lower(c) >= 'a' && Lower(c) <= 'f') digit = (uint32_t)(Lower(c) - 'a' + 10);
		else return false;
		parsed = (parsed << 4) | digit;
	}
	*titleId = parsed;
	return true;
}

bool ParsePaddle(const char* key, Paddle* paddle) {
	if (Equals(key, "r4")) *paddle = kPaddleR4;
	else if (Equals(key, "r5")) *paddle = kPaddleR5;
	else if (Equals(key, "l4")) *paddle = kPaddleL4;
	else if (Equals(key, "l5")) *paddle = kPaddleL5;
	else return false;
	return true;
}

bool ParseBinding(const char* value, Binding* binding) {
	static const char* names[] = { "none", "a", "b", "x", "y", "dpad_up",
		"dpad_down", "dpad_left", "dpad_right", "left_shoulder", "right_shoulder",
		"left_stick", "right_stick", "start", "back", "guide", "left_trigger",
		"right_trigger" };
	for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); ++i) {
		if (Equals(value, names[i])) { *binding = (Binding)i; return true; }
	}
	return false;
}

bool ParseBool(const char* value, bool* result) {
	if (Equals(value, "true")) { *result = true; return true; }
	if (Equals(value, "false")) { *result = false; return true; }
	return false;
}

bool ParsePermilleFloat(const char* value, uint16_t minimum, uint16_t maximum,
	float* result) {
	uint16_t parsed;
	if (!ParsePermille(value, maximum, &parsed) || parsed < minimum) return false;
	*result = parsed / 1000.0f;
	return true;
}

bool SetPaddle(Profile* profile, uint8_t* mask, const char* key, const char* value,
	ParseError* error, size_t line) {
	Paddle paddle;
	Binding binding;
	if (!ParsePaddle(key, &paddle)) { SetError(error, line, "unknown paddle"); return false; }
	uint8_t bit = (uint8_t)(1u << paddle);
	if (*mask & bit) { SetError(error, line, "duplicate paddle binding"); return false; }
	if (!ParseBinding(value, &binding)) { SetError(error, line, "unknown binding"); return false; }
	profile->paddles[paddle] = binding;
	*mask |= bit;
	return true;
}

bool SetRumble(Profile* profile, uint8_t* mask, const char* key, const char* value,
	ParseError* error, size_t line) {
	uint8_t bit = 0;
	if (Equals(key, "enabled")) bit = kRumbleEnabled;
	else if (Equals(key, "left_gain")) bit = kRumbleLeftGain;
	else if (Equals(key, "right_gain")) bit = kRumbleRightGain;
	else if (Equals(key, "deadzone")) bit = kRumbleDeadzone;
	else if (Equals(key, "curve")) bit = kRumbleCurve;
	else { SetError(error, line, "unknown rumble setting"); return false; }
	if (*mask & bit) { SetError(error, line, "duplicate rumble setting"); return false; }
	if (bit == kRumbleEnabled) {
		if (Equals(value, "true")) profile->rumble.enabled = true;
		else if (Equals(value, "false")) profile->rumble.enabled = false;
		else { SetError(error, line, "enabled must be true or false"); return false; }
	} else if (bit == kRumbleLeftGain || bit == kRumbleRightGain) {
		uint16_t gain;
		if (!ParsePermille(value, 2000, &gain)) { SetError(error, line, "gain must be between 0.0 and 2.0"); return false; }
		if (bit == kRumbleLeftGain) profile->rumble.leftGainPermille = gain;
		else profile->rumble.rightGainPermille = gain;
	} else if (bit == kRumbleDeadzone) {
		uint16_t deadzone;
		if (!ParsePermille(value, 950, &deadzone)) { SetError(error, line, "deadzone must be between 0.0 and 0.95"); return false; }
		profile->rumble.deadzone = (uint16_t)(((uint32_t)deadzone * 65535u + 500u) / 1000u);
	} else {
		if (Equals(value, "linear")) profile->rumble.curve = RumbleOutput::kCurveLinear;
		else if (Equals(value, "quadratic")) profile->rumble.curve = RumbleOutput::kCurveQuadratic;
		else if (Equals(value, "cubic")) profile->rumble.curve = RumbleOutput::kCurveCubic;
		else { SetError(error, line, "curve must be linear, quadratic, or cubic"); return false; }
	}
	*mask |= bit;
	return true;
}

bool SetMouseJoystick(Profile* profile, uint8_t* mask, const char* key,
	const char* value, ParseError* error, size_t line) {
	uint8_t bit;
	if (Equals(key, "sensitivity_x")) bit = kMouseSensitivityX;
	else if (Equals(key, "sensitivity_y")) bit = kMouseSensitivityY;
	else if (Equals(key, "minimum_output_x")) bit = kMouseMinimumX;
	else if (Equals(key, "minimum_output_y")) bit = kMouseMinimumY;
	else if (Equals(key, "smoothing_ms")) bit = kMouseSmoothing;
	else if (Equals(key, "noise_speed_threshold")) bit = kMouseNoise;
	else { SetError(error, line, "unknown mouse joystick setting"); return false; }
	if (*mask & bit) { SetError(error, line, "duplicate mouse joystick setting"); return false; }
	if (bit == kMouseSmoothing) {
		uint32_t parsed;
		if (!ParseUnsigned(value, &parsed) || parsed > 100) {
			SetError(error, line, "smoothing_ms must be between 0 and 100"); return false;
		}
		profile->mouseJoystick.smoothingMs = parsed;
	} else {
		float parsed;
		uint16_t minimum = (bit == kMouseSensitivityX || bit == kMouseSensitivityY) ? 10 : 0;
		uint16_t maximum = (bit == kMouseSensitivityX || bit == kMouseSensitivityY) ? 20000 : 1000;
		if (!ParsePermilleFloat(value, minimum, maximum, &parsed)) {
			SetError(error, line, "mouse joystick value is out of range"); return false;
		}
		if (bit == kMouseSensitivityX) profile->mouseJoystick.sensitivityX = parsed;
		else if (bit == kMouseSensitivityY) profile->mouseJoystick.sensitivityY = parsed;
		else if (bit == kMouseMinimumX) profile->mouseJoystick.minimumOutputX = parsed;
		else if (bit == kMouseMinimumY) profile->mouseJoystick.minimumOutputY = parsed;
		else profile->mouseJoystick.noiseSpeedThreshold = parsed;
	}
	*mask |= bit;
	return true;
}

bool SetRightTrackpad(Profile* profile, uint16_t* mask, const char* key,
	const char* value, ParseError* error, size_t line) {
	uint16_t bit;
	if (Equals(key, "mode")) bit = kPadMode;
	else if (Equals(key, "click_action")) bit = kPadClickAction;
	else if (Equals(key, "trackball_enabled")) bit = kPadTrackballEnabled;
	else if (Equals(key, "friction_curve")) bit = kPadFrictionCurve;
	else if (Equals(key, "friction_strength")) bit = kPadFrictionStrength;
	else if (Equals(key, "friction_max_speed")) bit = kPadFrictionMaxSpeed;
	else if (Equals(key, "friction_reference_ms")) bit = kPadFrictionReference;
	else if (Equals(key, "friction_min_ms")) bit = kPadFrictionMinimum;
	else if (Equals(key, "friction_max_ms")) bit = kPadFrictionMaximum;
	else if (Equals(key, "friction_vertical_scale")) bit = kPadFrictionVertical;
	else if (Equals(key, "haptics_intensity")) bit = kPadHapticsIntensity;
	else if (Equals(key, "click_haptics_intensity")) bit = kPadClickHapticsIntensity;
	else if (Equals(key, "release_haptics_intensity")) bit = kPadReleaseHapticsIntensity;
	else if (Equals(key, "haptics_max_hz")) bit = kPadHapticsMaximumHz;
	else if (Equals(key, "haptics_full_speed")) bit = kPadHapticsFullSpeed;
	else if (Equals(key, "physical_stick_threshold")) bit = kPadPhysicalStickThreshold;
	else { SetError(error, line, "unknown right trackpad setting"); return false; }
	if (*mask & bit) { SetError(error, line, "duplicate right trackpad setting"); return false; }
	if (bit == kPadMode) {
		if (Equals(value, "disabled")) profile->rightTrackpad.mode = kRightTrackpadDisabled;
		else if (Equals(value, "mouse_joystick")) profile->rightTrackpad.mode = kRightTrackpadMouseJoystick;
		else { SetError(error, line, "mode must be disabled or mouse_joystick"); return false; }
	} else if (bit == kPadClickAction) {
		if (!ParseBinding(value, &profile->rightTrackpad.clickAction)) {
			SetError(error, line, "unknown click binding"); return false;
		}
	} else if (bit == kPadTrackballEnabled) {
		if (!ParseBool(value, &profile->rightTrackpad.trackball.enabled)) {
			SetError(error, line, "trackball_enabled must be true or false"); return false;
		}
	} else if (bit == kPadFrictionCurve) {
		if (Equals(value, "linear")) profile->rightTrackpad.trackball.curve = TrackballMotion::kCurveLinear;
		else if (Equals(value, "ease_out_quadratic")) profile->rightTrackpad.trackball.curve = TrackballMotion::kCurveEaseOutQuadratic;
		else if (Equals(value, "ease_out_cubic")) profile->rightTrackpad.trackball.curve = TrackballMotion::kCurveEaseOutCubic;
		else if (Equals(value, "ease_out_quartic")) profile->rightTrackpad.trackball.curve = TrackballMotion::kCurveEaseOutQuartic;
		else { SetError(error, line, "invalid friction_curve"); return false; }
	} else if (bit == kPadFrictionReference || bit == kPadFrictionMinimum ||
		bit == kPadFrictionMaximum || bit == kPadHapticsMaximumHz) {
		uint32_t parsed;
		const uint32_t maximum = bit == kPadHapticsMaximumHz ? 100 : 5000;
		if (!ParseUnsigned(value, &parsed) || parsed < 1 || parsed > maximum) {
			SetError(error, line, "right trackpad integer is out of range"); return false;
		}
		if (bit == kPadFrictionReference) profile->rightTrackpad.trackball.frictionReferenceMs = parsed;
		else if (bit == kPadFrictionMinimum) profile->rightTrackpad.trackball.frictionMinMs = parsed;
		else if (bit == kPadFrictionMaximum) profile->rightTrackpad.trackball.frictionMaxMs = parsed;
		else profile->rightTrackpad.haptics.maximumHz = parsed;
	} else {
		float parsed;
		uint16_t minimum = 0, maximum = 1000;
		if (bit == kPadFrictionStrength) { minimum = 100; maximum = 10000; }
		else if (bit == kPadFrictionMaxSpeed) { minimum = 0; maximum = 60000; }
		else if (bit == kPadHapticsFullSpeed) { minimum = 10; maximum = 20000; }
		if (!ParsePermilleFloat(value, minimum, maximum, &parsed)) {
			SetError(error, line, "right trackpad value is out of range"); return false;
		}
		if (bit == kPadFrictionStrength) profile->rightTrackpad.trackball.frictionStrength = parsed;
		else if (bit == kPadFrictionMaxSpeed) profile->rightTrackpad.trackball.frictionMaxSpeed = parsed;
		else if (bit == kPadFrictionVertical) profile->rightTrackpad.trackball.verticalScale = parsed;
		else if (bit == kPadHapticsIntensity) profile->rightTrackpad.haptics.movementIntensity = parsed;
		else if (bit == kPadClickHapticsIntensity) profile->rightTrackpad.haptics.clickIntensity = parsed;
		else if (bit == kPadReleaseHapticsIntensity) profile->rightTrackpad.haptics.releaseIntensity = parsed;
		else if (bit == kPadHapticsFullSpeed) profile->rightTrackpad.haptics.fullSpeed = parsed;
		else profile->rightTrackpad.physicalStickThreshold = parsed;
	}
	*mask |= bit;
	return true;
}

void ApplyBindingInternal(Binding binding, TritonProtocol::ControllerState* state) {
	switch (binding) {
	case kBindingA: state->a = 1; break;
	case kBindingB: state->b = 1; break;
	case kBindingX: state->x = 1; break;
	case kBindingY: state->y = 1; break;
	case kBindingDpadUp: state->dpadUp = 1; break;
	case kBindingDpadDown: state->dpadDown = 1; break;
	case kBindingDpadLeft: state->dpadLeft = 1; break;
	case kBindingDpadRight: state->dpadRight = 1; break;
	case kBindingLeftShoulder: state->leftShoulder = 1; break;
	case kBindingRightShoulder: state->rightShoulder = 1; break;
	case kBindingLeftStick: state->leftStick = 1; break;
	case kBindingRightStick: state->rightStick = 1; break;
	case kBindingStart: state->view = 1; break;
	case kBindingBack: state->menu = 1; break;
	case kBindingGuide: state->guide = 1; break;
	case kBindingLeftTrigger: state->leftTrigger = 255; break;
	case kBindingRightTrigger: state->rightTrigger = 255; break;
	default: break;
	}
}

} // namespace

void Initialize(Config* config) {
	memset(config, 0, sizeof(*config));
	config->defaults.rumble = RumbleOutput::DefaultSettings();
	config->defaults.mouseJoystick = MouseJoystick::DefaultSettings();
	config->defaults.rightTrackpad.mode = kRightTrackpadDisabled;
	config->defaults.rightTrackpad.clickAction = kBindingNone;
	config->defaults.rightTrackpad.trackball = TrackballMotion::DefaultSettings();
	config->defaults.rightTrackpad.haptics = TrackpadHaptics::DefaultSettings();
	config->defaults.rightTrackpad.physicalStickThreshold = 0.15f;
}

bool Parse(const char* text, size_t length, Config* config, ParseError* error) {
	if (!text || !config) { SetError(error, 0, "invalid parser argument"); return false; }
	Config parsed;
	Initialize(&parsed);
	uint8_t defaultPaddleMask = 0, defaultRumbleMask = 0, defaultMouseMask = 0;
	uint16_t defaultTrackpadMask = 0;
	uint8_t gamePaddleMasks[kMaxGames] = {};
	uint8_t gameRumbleMasks[kMaxGames] = {};
	uint8_t gameMouseMasks[kMaxGames] = {};
	uint16_t gameTrackpadMasks[kMaxGames] = {};
	bool versionSeen = false;
	bool defaultsSeen = false;
	bool gamesSeen = false;
	Section section = kSectionNone;
	Subsection subsection = kSubsectionNone;
	int currentGame = -1;
	size_t position = 0, lineNumber = 0;
	if (length >= 3 && (uint8_t)text[0] == 0xef && (uint8_t)text[1] == 0xbb && (uint8_t)text[2] == 0xbf) position = 3;
	while (position < length) {
		++lineNumber;
		char line[256];
		size_t lineLength = 0;
		while (position < length && text[position] != '\n') {
			char c = text[position++];
			if (c == '\r') continue;
			if (lineLength + 1 >= sizeof(line)) { SetError(error, lineNumber, "line is too long"); return false; }
			line[lineLength++] = c;
		}
		if (position < length && text[position] == '\n') ++position;
		line[lineLength] = 0;
		StripComment(line);
		lineLength = strlen(line);
		while (lineLength && line[lineLength - 1] == ' ') line[--lineLength] = 0;
		if (!lineLength) continue;
		size_t indent = 0;
		while (line[indent] == ' ') ++indent;
		if (line[indent] == '\t' || (indent & 1)) { SetError(error, lineNumber, "indentation must use pairs of spaces"); return false; }
		char* content = line + indent;
		char* colon = FindColon(content);
		if (!colon) { SetError(error, lineNumber, "expected key and colon"); return false; }
		*colon = 0;
		char* key = Trim(content);
		char* value = Trim(colon + 1);
		if (!Unquote(key) || !Unquote(value)) { SetError(error, lineNumber, "unterminated quoted value"); return false; }
		if (indent == 0) {
			subsection = kSubsectionNone;
			currentGame = -1;
			if (Equals(key, "version")) {
				uint32_t version;
				if (versionSeen) { SetError(error, lineNumber, "duplicate version"); return false; }
				if (!ParseUnsigned(value, &version) || version != 1) { SetError(error, lineNumber, "unsupported config version"); return false; }
				versionSeen = true;
				section = kSectionNone;
			} else if (Equals(key, "defaults") && !*value) {
				if (defaultsSeen) { SetError(error, lineNumber, "duplicate defaults section"); return false; }
				defaultsSeen = true;
				section = kSectionDefaults;
			} else if (Equals(key, "games") && !*value) {
				if (gamesSeen) { SetError(error, lineNumber, "duplicate games section"); return false; }
				gamesSeen = true;
				section = kSectionGames;
			}
			else { SetError(error, lineNumber, "unknown top-level setting"); return false; }
		} else if (indent == 2 && section == kSectionDefaults && !*value) {
			if (Equals(key, "paddles")) subsection = kSubsectionPaddles;
			else if (Equals(key, "rumble")) subsection = kSubsectionRumble;
			else if (Equals(key, "mouse_joystick")) subsection = kSubsectionMouseJoystick;
			else if (Equals(key, "right_trackpad")) subsection = kSubsectionRightTrackpad;
			else { SetError(error, lineNumber, "unknown defaults section"); return false; }
		} else if (indent == 2 && section == kSectionGames && !*value) {
			uint32_t titleId;
			if (!ParseTitleId(key, &titleId)) { SetError(error, lineNumber, "game key must be an eight-digit hexadecimal Title ID"); return false; }
			for (size_t i = 0; i < parsed.gameCount; ++i)
				if (parsed.games[i].titleId == titleId) { SetError(error, lineNumber, "duplicate game Title ID"); return false; }
			if (parsed.gameCount >= kMaxGames) { SetError(error, lineNumber, "too many game profiles"); return false; }
			currentGame = (int)parsed.gameCount++;
			parsed.games[currentGame].titleId = titleId;
			parsed.games[currentGame].profile = parsed.defaults;
			subsection = kSubsectionNone;
		} else if (indent == 4 && section == kSectionDefaults && *value) {
			if (subsection == kSubsectionPaddles) {
				if (!SetPaddle(&parsed.defaults, &defaultPaddleMask, key, value, error, lineNumber)) return false;
			} else if (subsection == kSubsectionRumble) {
				if (!SetRumble(&parsed.defaults, &defaultRumbleMask, key, value, error, lineNumber)) return false;
			} else if (subsection == kSubsectionMouseJoystick) {
				if (!SetMouseJoystick(&parsed.defaults, &defaultMouseMask, key, value, error, lineNumber)) return false;
			} else if (subsection == kSubsectionRightTrackpad) {
				if (!SetRightTrackpad(&parsed.defaults, &defaultTrackpadMask, key, value, error, lineNumber)) return false;
			} else { SetError(error, lineNumber, "setting is outside a defaults section"); return false; }
		} else if (indent == 4 && section == kSectionGames && currentGame >= 0 && !*value) {
			if (Equals(key, "paddles")) subsection = kSubsectionPaddles;
			else if (Equals(key, "rumble")) subsection = kSubsectionRumble;
			else if (Equals(key, "mouse_joystick")) subsection = kSubsectionMouseJoystick;
			else if (Equals(key, "right_trackpad")) subsection = kSubsectionRightTrackpad;
			else { SetError(error, lineNumber, "unknown game section"); return false; }
		} else if (indent == 6 && section == kSectionGames && currentGame >= 0 && *value) {
			if (subsection == kSubsectionPaddles) {
				if (!SetPaddle(&parsed.games[currentGame].profile, &gamePaddleMasks[currentGame], key, value, error, lineNumber)) return false;
			} else if (subsection == kSubsectionRumble) {
				if (!SetRumble(&parsed.games[currentGame].profile, &gameRumbleMasks[currentGame], key, value, error, lineNumber)) return false;
			} else if (subsection == kSubsectionMouseJoystick) {
				if (!SetMouseJoystick(&parsed.games[currentGame].profile, &gameMouseMasks[currentGame], key, value, error, lineNumber)) return false;
			} else if (subsection == kSubsectionRightTrackpad) {
				if (!SetRightTrackpad(&parsed.games[currentGame].profile, &gameTrackpadMasks[currentGame], key, value, error, lineNumber)) return false;
			} else { SetError(error, lineNumber, "setting is outside a game section"); return false; }
		} else { SetError(error, lineNumber, "invalid configuration structure"); return false; }
	}
	if (!versionSeen) { SetError(error, 0, "missing config version"); return false; }
	for (size_t i = 0; i < parsed.gameCount; ++i) {
		Profile overrides = parsed.games[i].profile;
		parsed.games[i].profile = parsed.defaults;
		for (int paddle = 0; paddle < kPaddleCount; ++paddle)
			if (gamePaddleMasks[i] & (1u << paddle)) parsed.games[i].profile.paddles[paddle] = overrides.paddles[paddle];
		uint8_t mask = gameRumbleMasks[i];
		if (mask & kRumbleEnabled) parsed.games[i].profile.rumble.enabled = overrides.rumble.enabled;
		if (mask & kRumbleLeftGain) parsed.games[i].profile.rumble.leftGainPermille = overrides.rumble.leftGainPermille;
		if (mask & kRumbleRightGain) parsed.games[i].profile.rumble.rightGainPermille = overrides.rumble.rightGainPermille;
		if (mask & kRumbleDeadzone) parsed.games[i].profile.rumble.deadzone = overrides.rumble.deadzone;
		if (mask & kRumbleCurve) parsed.games[i].profile.rumble.curve = overrides.rumble.curve;
		uint8_t mouseMask = gameMouseMasks[i];
		if (mouseMask & kMouseSensitivityX) parsed.games[i].profile.mouseJoystick.sensitivityX = overrides.mouseJoystick.sensitivityX;
		if (mouseMask & kMouseSensitivityY) parsed.games[i].profile.mouseJoystick.sensitivityY = overrides.mouseJoystick.sensitivityY;
		if (mouseMask & kMouseMinimumX) parsed.games[i].profile.mouseJoystick.minimumOutputX = overrides.mouseJoystick.minimumOutputX;
		if (mouseMask & kMouseMinimumY) parsed.games[i].profile.mouseJoystick.minimumOutputY = overrides.mouseJoystick.minimumOutputY;
		if (mouseMask & kMouseSmoothing) parsed.games[i].profile.mouseJoystick.smoothingMs = overrides.mouseJoystick.smoothingMs;
		if (mouseMask & kMouseNoise) parsed.games[i].profile.mouseJoystick.noiseSpeedThreshold = overrides.mouseJoystick.noiseSpeedThreshold;
		uint16_t padMask = gameTrackpadMasks[i];
		if (padMask & kPadMode) parsed.games[i].profile.rightTrackpad.mode = overrides.rightTrackpad.mode;
		if (padMask & kPadClickAction) parsed.games[i].profile.rightTrackpad.clickAction = overrides.rightTrackpad.clickAction;
		if (padMask & kPadTrackballEnabled) parsed.games[i].profile.rightTrackpad.trackball.enabled = overrides.rightTrackpad.trackball.enabled;
		if (padMask & kPadFrictionCurve) parsed.games[i].profile.rightTrackpad.trackball.curve = overrides.rightTrackpad.trackball.curve;
		if (padMask & kPadFrictionStrength) parsed.games[i].profile.rightTrackpad.trackball.frictionStrength = overrides.rightTrackpad.trackball.frictionStrength;
		if (padMask & kPadFrictionMaxSpeed) parsed.games[i].profile.rightTrackpad.trackball.frictionMaxSpeed = overrides.rightTrackpad.trackball.frictionMaxSpeed;
		if (padMask & kPadFrictionReference) parsed.games[i].profile.rightTrackpad.trackball.frictionReferenceMs = overrides.rightTrackpad.trackball.frictionReferenceMs;
		if (padMask & kPadFrictionMinimum) parsed.games[i].profile.rightTrackpad.trackball.frictionMinMs = overrides.rightTrackpad.trackball.frictionMinMs;
		if (padMask & kPadFrictionMaximum) parsed.games[i].profile.rightTrackpad.trackball.frictionMaxMs = overrides.rightTrackpad.trackball.frictionMaxMs;
		if (padMask & kPadFrictionVertical) parsed.games[i].profile.rightTrackpad.trackball.verticalScale = overrides.rightTrackpad.trackball.verticalScale;
		if (padMask & kPadHapticsIntensity) parsed.games[i].profile.rightTrackpad.haptics.movementIntensity = overrides.rightTrackpad.haptics.movementIntensity;
		if (padMask & kPadClickHapticsIntensity) parsed.games[i].profile.rightTrackpad.haptics.clickIntensity = overrides.rightTrackpad.haptics.clickIntensity;
		if (padMask & kPadReleaseHapticsIntensity) parsed.games[i].profile.rightTrackpad.haptics.releaseIntensity = overrides.rightTrackpad.haptics.releaseIntensity;
		if (padMask & kPadHapticsMaximumHz) parsed.games[i].profile.rightTrackpad.haptics.maximumHz = overrides.rightTrackpad.haptics.maximumHz;
		if (padMask & kPadHapticsFullSpeed) parsed.games[i].profile.rightTrackpad.haptics.fullSpeed = overrides.rightTrackpad.haptics.fullSpeed;
		if (padMask & kPadPhysicalStickThreshold) parsed.games[i].profile.rightTrackpad.physicalStickThreshold = overrides.rightTrackpad.physicalStickThreshold;
		if (parsed.games[i].profile.rightTrackpad.trackball.frictionMinMs >
			parsed.games[i].profile.rightTrackpad.trackball.frictionMaxMs) {
			SetError(error, 0, "friction_min_ms must not exceed friction_max_ms"); return false;
		}
	}
	if (parsed.defaults.rightTrackpad.trackball.frictionMinMs >
		parsed.defaults.rightTrackpad.trackball.frictionMaxMs) {
		SetError(error, 0, "friction_min_ms must not exceed friction_max_ms"); return false;
	}
	*config = parsed;
	if (error) { error->line = 0; error->message = 0; }
	return true;
}

const Profile* FindProfile(const Config& config, uint32_t titleId) {
	for (size_t i = 0; i < config.gameCount; ++i)
		if (config.games[i].titleId == titleId) return &config.games[i].profile;
	return &config.defaults;
}

void ApplyPaddleBindings(const Profile& profile, TritonProtocol::ControllerState* state) {
	if (!state) return;
	if (state->r4) ApplyBindingInternal(profile.paddles[kPaddleR4], state);
	if (state->r5) ApplyBindingInternal(profile.paddles[kPaddleR5], state);
	if (state->l4) ApplyBindingInternal(profile.paddles[kPaddleL4], state);
	if (state->l5) ApplyBindingInternal(profile.paddles[kPaddleL5], state);
}

void ApplyBinding(Binding binding, TritonProtocol::ControllerState* state) {
	if (state) ApplyBindingInternal(binding, state);
}

const char* DefaultFileText() { return kDefaultConfig; }
size_t DefaultFileSize() { return sizeof(kDefaultConfig) - 1; }

} // namespace TritonConfig
