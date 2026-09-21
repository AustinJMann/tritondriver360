#pragma once

#include "triton_protocol.h"
#include "usb.h"

namespace ControllerUsbPolicy {

enum Kind { kUnsupported, kProteus, kWiredTriton };

inline Kind Classify(uint16_t vendor, uint16_t product,
	const usb_interface_descriptor* descriptor) {
	if (!descriptor || descriptor->bLength < 9 || descriptor->bDescriptorType != 4 ||
		descriptor->bAlternateSetting != 0) return kUnsupported;
	if (TritonProtocol::IsProteusSlotInterface(vendor, product,
		descriptor->bInterfaceNumber, descriptor->bInterfaceClass,
		descriptor->bInterfaceSubClass, descriptor->bInterfaceProtocol)) return kProteus;
	// SDL admits Triton by product; restrict the Xbox path to non-boot HID.
	// The report descriptor is validated before starting wired input or output.
	if (vendor == TritonProtocol::kValveVendorId && product == TritonProtocol::kTritonUsbProductId &&
		descriptor->bInterfaceClass == 3 && descriptor->bInterfaceSubClass == 0 &&
		descriptor->bInterfaceProtocol == 0) return kWiredTriton;
	return kUnsupported;
}

inline bool ShouldPauseHeartbeat(Kind kind, bool connected, uint32_t failures) {
	return kind == kProteus && !connected && failures >= 3;
}

inline size_t InputLength(uint32_t completed, size_t capacity) {
	return completed <= capacity ? completed : 0;
}

} // namespace ControllerUsbPolicy
