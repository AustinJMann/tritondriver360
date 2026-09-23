#pragma once

#include <stddef.h>
#include <stdint.h>

namespace TritonHidDescriptor {

// Only the report sizes used by this driver are retained. Reject unsupported
// long items and malformed nesting rather than accepting a partial descriptor.
// Haptic command report 0x82 is optional; its absence disables pad haptics.
inline bool Validate(const uint8_t* bytes, size_t length, bool* hapticCommand = 0) {
	if (hapticCommand) *hapticCommand = false;
	if (!bytes || !length) return false;
	struct Globals {
		uint32_t size;
		uint32_t count;
		uint32_t id;
	} current = {}, stack[8];
	unsigned depth = 0, collections = 0;
	uint32_t inputBits[3] = {}, featureBits = 0, outputBits = 0, hapticBits = 0;
	for (size_t offset = 0; offset < length;) {
		uint8_t prefix = bytes[offset++];
		if (prefix == 0xfe) return false;
		unsigned size = prefix & 3;
		if (size == 3) size = 4;
		if (size > length - offset) return false;
		uint32_t value = 0;
		for (unsigned i = 0; i < size; ++i) value |= (uint32_t)bytes[offset++] << (8 * i);
		unsigned type = (prefix >> 2) & 3, tag = prefix >> 4;
		if (type == 1) {
			if (tag == 7) current.size = value;
			else if (tag == 8) {
				if (!value || value > 255) return false;
				current.id = value;
			}
			else if (tag == 9) current.count = value;
			else if (tag == 10) {
				if (depth == 8 || size) return false;
				stack[depth++] = current;
			} else if (tag == 11) {
				if (!depth || size) return false;
				current = stack[--depth];
			}
		} else if (type == 0) {
			if (tag == 10) {
				if (++collections > 32) return false;
			} else if (tag == 12) {
				if (!collections) return false;
				--collections;
			} else if (tag == 8 || tag == 9 || tag == 11) {
				if (!collections || current.size > 512 || current.count > 512) return false;
				uint32_t bits = current.size * current.count;
				uint32_t* total = 0;
				if (tag == 8) {
					if (current.id == 0x42) total = &inputBits[0];
					if (current.id == 0x45) total = &inputBits[1];
					if (current.id == 0x47) total = &inputBits[2];
				} else if (tag == 9 && current.id == 0x80) total = &outputBits;
				else if (tag == 9 && current.id == 0x82) total = &hapticBits;
				else if (tag == 11 && current.id == 1) total = &featureBits;
				if (total) {
					*total += bits;
					if (*total > 504) return false;
				}
			}
		}
	}
	const bool valid = depth == 0 && collections == 0 && featureBits == 63 * 8 &&
		outputBits == 9 * 8 &&
		(inputBits[0] >= 17 * 8 || inputBits[1] >= 17 * 8 || inputBits[2] >= 17 * 8);
	if (valid && hapticCommand) *hapticCommand = hapticBits == 3 * 8;
	return valid;
}

} // namespace TritonHidDescriptor
