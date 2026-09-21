#include "triton_config.h"

#include <string.h>

namespace TritonConfig {

namespace {

enum Section { kSectionNone, kSectionDefaults, kSectionGames };
enum Subsection { kSubsectionNone, kSubsectionPaddles, kSubsectionRumble };

static const uint8_t kRumbleEnabled = 0x01;
static const uint8_t kRumbleLeftGain = 0x02;
static const uint8_t kRumbleRightGain = 0x04;
static const uint8_t kRumbleDeadzone = 0x08;
static const uint8_t kRumbleCurve = 0x10;

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

void ApplyBinding(Binding binding, TritonProtocol::ControllerState* state) {
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
}

bool Parse(const char* text, size_t length, Config* config, ParseError* error) {
	if (!text || !config) { SetError(error, 0, "invalid parser argument"); return false; }
	Config parsed;
	Initialize(&parsed);
	uint8_t defaultPaddleMask = 0, defaultRumbleMask = 0;
	uint8_t gamePaddleMasks[kMaxGames] = {};
	uint8_t gameRumbleMasks[kMaxGames] = {};
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
			else { SetError(error, lineNumber, "unknown defaults section"); return false; }
		} else if (indent == 2 && section == kSectionGames && !*value) {
			uint32_t titleId;
			if (!ParseTitleId(key, &titleId)) { SetError(error, lineNumber, "game key must be an eight-digit hexadecimal Title ID"); return false; }
			for (size_t i = 0; i < parsed.gameCount; ++i)
				if (parsed.games[i].titleId == titleId) { SetError(error, lineNumber, "duplicate game Title ID"); return false; }
			if (parsed.gameCount >= kMaxGames) { SetError(error, lineNumber, "too many game profiles"); return false; }
			currentGame = (int)parsed.gameCount++;
			parsed.games[currentGame].titleId = titleId;
			parsed.games[currentGame].profile.rumble = RumbleOutput::DefaultSettings();
			subsection = kSubsectionNone;
		} else if (indent == 4 && section == kSectionDefaults && *value) {
			if (subsection == kSubsectionPaddles) {
				if (!SetPaddle(&parsed.defaults, &defaultPaddleMask, key, value, error, lineNumber)) return false;
			} else if (subsection == kSubsectionRumble) {
				if (!SetRumble(&parsed.defaults, &defaultRumbleMask, key, value, error, lineNumber)) return false;
			} else { SetError(error, lineNumber, "setting is outside a defaults section"); return false; }
		} else if (indent == 4 && section == kSectionGames && currentGame >= 0 && !*value) {
			if (Equals(key, "paddles")) subsection = kSubsectionPaddles;
			else if (Equals(key, "rumble")) subsection = kSubsectionRumble;
			else { SetError(error, lineNumber, "unknown game section"); return false; }
		} else if (indent == 6 && section == kSectionGames && currentGame >= 0 && *value) {
			if (subsection == kSubsectionPaddles) {
				if (!SetPaddle(&parsed.games[currentGame].profile, &gamePaddleMasks[currentGame], key, value, error, lineNumber)) return false;
			} else if (subsection == kSubsectionRumble) {
				if (!SetRumble(&parsed.games[currentGame].profile, &gameRumbleMasks[currentGame], key, value, error, lineNumber)) return false;
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
	if (state->r4) ApplyBinding(profile.paddles[kPaddleR4], state);
	if (state->r5) ApplyBinding(profile.paddles[kPaddleR5], state);
	if (state->l4) ApplyBinding(profile.paddles[kPaddleL4], state);
	if (state->l5) ApplyBinding(profile.paddles[kPaddleL5], state);
}

const char* DefaultFileText() { return kDefaultConfig; }
size_t DefaultFileSize() { return sizeof(kDefaultConfig) - 1; }

} // namespace TritonConfig
