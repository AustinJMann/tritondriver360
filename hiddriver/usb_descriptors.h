#pragma once

#include <stddef.h>
#include <string.h>

#include "usb.h"

namespace UsbDescriptors {

inline uint16_t HidReportLength(const uint8_t* bytes, size_t length, uint8_t interfaceNumber) {
	if (!bytes || length < 9 || bytes[0] != 9 || bytes[1] != 2) return 0;
	size_t total = (size_t)bytes[2] | ((size_t)bytes[3] << 8);
	if (total != length) return 0;
	bool matching = false;
	uint16_t reportLength = 0;
	for (size_t offset = 9; offset < total;) {
		if (total - offset < 2) return 0;
		const uint8_t* d = bytes + offset;
		if (d[0] < 2 || d[0] > total - offset) return 0;
		if (d[1] == 4) {
			if (d[0] < 9) return 0;
			matching = d[2] == interfaceNumber && d[3] == 0 && d[5] == 3 && d[6] == 0 && d[7] == 0;
		} else if (d[1] == 0x21 && matching) {
			if (d[0] < 6 || d[0] != 6 + 3 * d[5]) return 0;
			for (unsigned j = 6; j < d[0]; j += 3)
				if (d[j] == 0x22) reportLength = (uint16_t)(d[j + 1] | (d[j + 2] << 8));
		}
		offset += d[0];
	}
	return reportLength;
}

static const size_t kEndpointDescriptorSize = 7;

inline bool IsInterruptEndpoint(const usb_endpoint_descriptor& endpoint, bool input) {
	return endpoint.bLength >= kEndpointDescriptorSize &&
		endpoint.bDescriptorType == 0x05 &&
		((endpoint.bEndpointAddress & 0x80) != 0) == input &&
		(endpoint.bEndpointAddress & 0x70) == 0 &&
		(endpoint.bEndpointAddress & 0x0f) != 0 &&
		(endpoint.bmAttributes & 0x03) == 0x03;
}

inline bool IsInterruptInEndpoint(const usb_endpoint_descriptor& endpoint) {
	return IsInterruptEndpoint(endpoint, true);
}

inline bool IsInterruptOutEndpoint(const usb_endpoint_descriptor& endpoint) {
	return IsInterruptEndpoint(endpoint, false);
}

// Parse only an owned configuration buffer, never bytes beyond an interface
// pointer returned by the USB stack. Leave the output unchanged on failure.
inline bool FindInterruptEndpoint(const uint8_t* bytes, size_t length,
	uint8_t interfaceNumber, bool input, usb_endpoint_descriptor* output) {
	if (!bytes || !output || length < 9 || bytes[0] < 9 || bytes[1] != 0x02)
		return false;
	const size_t totalLength = (size_t)bytes[2] | ((size_t)bytes[3] << 8);
	if (totalLength < bytes[0] || totalLength > length) return false;
	bool matchesInterface = false;
	bool found = false;
	usb_endpoint_descriptor candidate = {};
	for (size_t offset = bytes[0]; offset < totalLength;) {
		if (totalLength - offset < 2) return false;
		const uint8_t* descriptor = bytes + offset;
		const size_t descriptorLength = descriptor[0];
		if (descriptorLength < 2 || descriptorLength > totalLength - offset)
			return false;
		if (descriptor[1] == 0x04) {
			if (descriptorLength < 9) return false;
			matchesInterface = descriptor[2] == interfaceNumber &&
				descriptor[3] == 0 && descriptor[5] == 0x03 &&
				descriptor[6] == 0 && descriptor[7] == 0;
		} else if (descriptor[1] == 0x05) {
			if (descriptorLength < kEndpointDescriptorSize) return false;
			usb_endpoint_descriptor endpoint = {};
			// The wire descriptor is seven bytes; the C++ structure may have
			// trailing padding. memcpy also avoids unaligned multi-byte reads.
			memcpy(&endpoint, descriptor, kEndpointDescriptorSize);
			if (matchesInterface && !found && IsInterruptEndpoint(endpoint, input)) {
				candidate = endpoint;
				found = true;
			}
		}
		offset += descriptorLength;
	}
	if (found) *output = candidate;
	return found;
}

inline bool FindInterruptInEndpoint(const uint8_t* bytes, size_t length,
	uint8_t interfaceNumber, usb_endpoint_descriptor* output) {
	return FindInterruptEndpoint(bytes, length, interfaceNumber, true, output);
}

inline bool FindInterruptOutEndpoint(const uint8_t* bytes, size_t length,
	uint8_t interfaceNumber, usb_endpoint_descriptor* output) {
	return FindInterruptEndpoint(bytes, length, interfaceNumber, false, output);
}

} // namespace UsbDescriptors
