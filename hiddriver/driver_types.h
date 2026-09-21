#pragma once

#include <stddef.h>
#include <stdint.h>

#include "usb.h"

struct UsbTrb {
	uint32_t endpoint;
	uint32_t callback;
	uint32_t savedEndpoint;
	uint8_t padding[4];
	uint8_t flags;
	uint8_t controllerIndex;
	uint8_t pad2;
	uint8_t endpointIndex;
	void* buffer;
	uint32_t length;
};

struct UsbPacket {
	uint8_t bmRequestType;
	uint8_t bRequest;
	uint16_t wValue;
	uint16_t wIndex;
	uint16_t wLength;
};

struct UsbControlTrb {
	UsbTrb trb;
	uint32_t transferredBytes;
	UsbPacket packet;
};

struct deviceHandle;
struct __declspec(align(4)) UsbControllerExtension {
	deviceHandle* deviceHandle;
	UsbTrb interruptTrb;
	// The common transfer completion writes its byte count at TRB + 0x1c.
	// Interface identity belongs in the transport record, outside this prefix.
	uint32_t inputTransferredBytes;
	UsbControlTrb controlTrb;
	uint8_t gap4C[4];
	uint32_t cleanupHandler;
	uint8_t gap54[24];
	uint32_t queue;
	uint8_t alwaysOne;
	uint8_t alwaysOneTwo;
	uint8_t unknownFlag;
	uint8_t alwaysZero;
	uint8_t cleanupDone;
	uint8_t initTransferPending;
	uint8_t alwaysZeroTwo;
	uint8_t deviceType;
	uint8_t alwaysZeroThree;
	uint8_t alwaysZeroFour;
};

struct deviceHandle {
	UsbControllerExtension* driver;
};

static_assert(offsetof(UsbControllerExtension, interruptTrb) == 0x04,
	"USB extension interrupt TRB offset changed");
static_assert(offsetof(UsbControllerExtension, inputTransferredBytes) == 0x20,
	"USB extension input completion offset changed");
static_assert(offsetof(UsbControllerExtension, controlTrb) == 0x24,
	"USB extension control TRB offset changed");
static_assert(offsetof(UsbControllerExtension, cleanupHandler) == 0x50,
	"USB extension cleanup handler offset changed");
static_assert(offsetof(UsbControllerExtension, queue) == 0x6c,
	"USB extension queue offset changed");
static_assert(sizeof(UsbControllerExtension) == 0x7c,
	"USB extension kernel-observed prefix size changed");
