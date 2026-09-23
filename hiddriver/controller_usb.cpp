#include <xtl.h>
#include <xkelib.h>
#include <stdlib.h>
#include <string.h>

#include "controller_usb.h"
#include "triton_hid_descriptor.h"
#include "triton_protocol.h"
#include "rumble_output.h"
#include "usb_descriptors.h"

typedef int (*AddCompleteFn)(deviceHandle*, int);
typedef int (*QueueTransferFn)(deviceHandle*, void*);
typedef NTSTATUS (*OpenDefaultEndpointFn)(deviceHandle*, DWORD*);
typedef NTSTATUS (*OpenEndpointFn)(deviceHandle*, int, int, int, int, DWORD*);
typedef NTSTATUS (*RemoveCompleteFn)(deviceHandle*);

extern AddCompleteFn UsbdAddDeviceComplete;
extern QueueTransferFn UsbdQueueAsyncTransfer;
extern OpenDefaultEndpointFn UsbdOpenDefaultEndpoint;
extern OpenEndpointFn UsbdOpenEndpoint;
extern RemoveCompleteFn UsbdRemoveDeviceComplete;

namespace {

static const int kSlotCount = ControllerRouting::kSlotCount;
static const uint32_t kHeartbeatIntervalMs = 2000;
static const uint32_t kHeartbeatRetryMs = 250;
static const uint32_t kInputRetryMs = 50;
static const uint32_t kRemovalGraceMs = 1000;
static const uint16_t kMaxHidPacketSize = 64;
static const uint16_t kConfigurationDescriptorBufferSize = 1024;

enum ControlPurpose {
	kControlNone,
	kControlGetConfigurationDescriptor,
	kControlGetConfigurationHeader,
	kControlGetHidDescriptor,
	kControlGetCurrentConfiguration,
	kControlSetConfiguration,
	kControlLizardOff,
	kControlRumble,
	kControlHaptic
};

// Separate allocation: preserve the kernel-observed extension layout and retain
// output TRB/data storage through removal just like the existing input storage.
// Rumble and pad haptics share this TRB, so at most one of them is in flight.
struct OutputTransfer {
	UsbTrb trb;
	// The kernel writes the completed byte count at TRB+0x1c. UsbTrb models
	// only the 0x1c-byte prefix; never place the HID payload in that field.
	uint32_t transferredBytes;
	uint8_t report[TritonProtocol::kRumbleReportSize];
};
static_assert(offsetof(OutputTransfer, transferredBytes) == 0x1c,
	"USB completion byte count offset changed");
static_assert(offsetof(OutputTransfer, report) == 0x20,
	"Output report overlaps USB transfer bookkeeping");
static_assert(TritonProtocol::kHapticCommandReportSize <= TritonProtocol::kRumbleReportSize,
	"Haptic report does not fit the shared output buffer");

struct UsbDeviceContext {
	bool configured;
	bool configurationBusy;
	bool descriptorFetched;
	bool currentConfigurationKnown;
	bool hidValidated;
	bool hapticCommand;
	bool failed;
	uint8_t currentConfiguration;
	uint16_t descriptorLength;
	uint16_t hidLength;
	uint32_t retryAt;
	volatile LONG controlOwner;
	uint8_t descriptor[kConfigurationDescriptorBufferSize];
	uint8_t hidDescriptor[kConfigurationDescriptorBufferSize];
};
struct ControlReports {
	uint8_t feature[TritonProtocol::kFeatureReportSize];
	uint8_t rumble[TritonProtocol::kRumbleReportSize];
	uint8_t haptic[TritonProtocol::kHapticCommandReportSize];
};
struct UsbSource {
	UsbDeviceContext* device;
	ControllerUsbPolicy::Kind kind;
	ControllerSourceToken token;
	usb_endpoint_descriptor outputDescriptor;
	deviceHandle* handle;
	UsbControllerExtension* extension;
	uint8_t interfaceNumber;
	uint8_t* inputBuffer;
	uint16_t inputLength;
	ControlReports* reports;
	RumbleOutput::State rumble;
	OutputTransfer* output;
	uint8_t outputEndpoint;
	bool loggedRumbleWait;
	bool hapticSupported;
	bool hapticPending;
	uint8_t hapticFailureCount;
	usb_endpoint_descriptor endpointDescriptor;
	uint32_t heartbeatDeadline;
	uint32_t inputRetryDeadline;
	uint32_t retryDelay;
	uint32_t inputErrorCount;
	uint8_t featureFailureCount;
	bool listening;
	volatile LONG inputPending;
	bool connected;
	bool heartbeatEnabled;
	volatile bool controlBusy;
	ControlPurpose controlPurpose;
	bool configurationPending;
	bool loggedFirstReport;
	bool loggedFirstState;
	bool loggedInputQueueResult;
	bool loggedControlQueueResult;
	bool loggedInputCompletion;
	bool loggedLizardSuccess;
	volatile bool removing;
	volatile bool removeCompleteCalled;
	volatile bool cleanupReady;
	bool loggedRemovalWait;
	uint32_t cleanupDeadline;
};

static UsbSource g_slots[kSlotCount];
static UsbDeviceContext* g_puckDevice;
static int g_nextHeartbeatSlot;
static int g_nextRumbleSlot;
static bool g_dispatchingOutputs;
static bool g_initializing;
static uint16_t Swap16(uint16_t value) {
	return (uint16_t)((value >> 8) | (value << 8));
}

static UsbSource* FindSlotByHandle(deviceHandle* handle) {
	if (!handle) return 0;
	for (int i = 0; i < kSlotCount; ++i)
		if (g_slots[i].handle == handle) return &g_slots[i];
	return 0;
}

static int32_t QueueInput(UsbSource* slot);
static int32_t InputComplete(DWORD trbAddress, int32_t status);
static int32_t ControlComplete(DWORD trbAddress, int32_t status);
static int32_t OutputComplete(DWORD trbAddress, int32_t status);
static void StartNextConfiguration();
static void DispatchOutputs(uint32_t now);

static void UpdateRemovalReady(UsbSource* slot) {
	if (!slot || !slot->removing || !slot->removeCompleteCalled)
		return;
	if (!slot->cleanupReady) {
		slot->cleanupDeadline = GetTickCount() + kRemovalGraceMs;
		MemoryBarrier();
		slot->cleanupReady = true;
	}
}

static void FinalizeRemoval(UsbSource* slot) {
	if (!slot || !slot->cleanupReady || !ControllerSourceRetired(slot->token)) return;
	UsbDeviceContext* device = slot->device;
	// A surviving puck interface still shares this arbiter. Its old owner must
	// complete before that source index can be reused on the same context.
	if (slot->controlBusy)
		for (int i = 0; i < kSlotCount; ++i)
			if (g_slots[i].handle && !g_slots[i].removing && g_slots[i].device == device) return;
	// Retain TRBs, payloads, and device buffers for late kernel accesses.
	// New attachments always receive new allocations and callback addresses.
	memset(slot, 0, sizeof(*slot));
	MemoryBarrier();
	for (int i = 0; i < kSlotCount; ++i)
		if (g_slots[i].handle && g_slots[i].device == device) return;
	if (g_puckDevice == device) g_puckDevice = 0;
}

static bool QueueControl(UsbSource* slot, ControlPurpose purpose,
	uint8_t requestType, uint8_t request, uint16_t value,
	uint16_t index, uint16_t length, void* data) {
	if (!slot || slot->removing || slot->controlBusy) return false;
	int slotIndex = (int)(slot - g_slots);
	if (slotIndex < 0 || slotIndex >= kSlotCount) return false;
	// Puck slots share endpoint zero; separate wired attachments do not.
	if (InterlockedCompareExchange(&slot->device->controlOwner, slotIndex, -1) != -1)
		return false;
	UsbControlTrb* control = &slot->extension->controlTrb;
	control->packet.bmRequestType = requestType;
	control->packet.bRequest = request;
	control->packet.wValue = Swap16(value);
	control->packet.wIndex = Swap16(index);
	control->packet.wLength = Swap16(length);
	control->trb.buffer = data;
	control->trb.length = length;
	control->transferredBytes = 0;
	slot->controlBusy = true;
	slot->controlPurpose = purpose;
	// This API returns an opaque queue token, which may have its high bit set;
	// completion status is delivered only through ControlComplete.
	int queueToken = UsbdQueueAsyncTransfer(slot->handle, control);
	if (!slot->loggedControlQueueResult) {
		slot->loggedControlQueueResult = true;
		DbgPrint("TritonDriver: USB source %u control transfer queued token %x request %02x value %04x\n",
			slot->token.index, queueToken, request, value);
	}
	return true;
}

static bool QueueLizardOff(UsbSource* slot) {
	if (slot->controlBusy || slot->device->controlOwner != -1) return false;
	TritonProtocol::BuildLizardOffFeatureReport(slot->reports->feature);
	return QueueControl(slot, kControlLizardOff, 0x21, 0x09, 0x0301, slot->interfaceNumber,
		TritonProtocol::kFeatureReportSize, slot->reports->feature);
}

static bool QueueRumble(UsbSource* slot, uint32_t now) {
	if (slot->removing || !slot->rumble.Due(now)) return false;
	if (slot->output && slot->hapticPending) return false;
	if (!slot->output && (slot->controlBusy || slot->device->controlOwner != -1)) return false;
	uint8_t* report = slot->output ? slot->output->report : slot->reports->rumble;
	TritonProtocol::BuildRumbleOutputReport(RumbleOutput::Left(slot->rumble.desired),
		RumbleOutput::Right(slot->rumble.desired), report);
	// Mark pending before queueing: completion may run before the API returns.
	slot->rumble.Submitted(now);
	slot->loggedRumbleWait = false;
	if (slot->output) {
		UsbTrb* trb = &slot->output->trb;
		trb->buffer = report;
		trb->length = TritonProtocol::kRumbleReportSize;
		UsbdQueueAsyncTransfer(slot->handle, trb);
		return true;
	}
	// HID Output (2), report 0x80, including the report ID in the 10-byte data.
	if (QueueControl(slot, kControlRumble, 0x21, 0x09, 0x0280,
		slot->interfaceNumber, TritonProtocol::kRumbleReportSize, report)) return true;
	slot->rumble.pending = false;
	return false;
}

// Pulses are one-shot: a pulse that cannot be sent now is superseded or expires,
// and failures are not retried so stale feedback never plays late.
static bool QueueHaptic(UsbSource* slot, uint32_t now) {
	if (slot->removing || !slot->hapticSupported || slot->hapticPending) return false;
	if (slot->output ? slot->rumble.pending :
		(slot->controlBusy || slot->device->controlOwner != -1)) return false;
	TrackpadHaptics::Pulse pulse;
	if (!ControllerTakeHapticPulse(slot->token, now, &pulse)) return false;
	uint8_t* report = slot->output ? slot->output->report : slot->reports->haptic;
	TritonProtocol::BuildHapticCommandReport(TritonProtocol::kHapticSideRightPad,
		pulse.kind == TrackpadHaptics::kRequestClick ?
			TritonProtocol::kHapticClickStrong : TritonProtocol::kHapticClick,
		pulse.gainDb, report);
	if (slot->output) {
		// Mark pending before queueing: completion may run before the API returns.
		slot->hapticPending = true;
		UsbTrb* trb = &slot->output->trb;
		trb->buffer = report;
		trb->length = TritonProtocol::kHapticCommandReportSize;
		UsbdQueueAsyncTransfer(slot->handle, trb);
		return true;
	}
	// HID Output (2), report 0x82, including the report ID.
	return QueueControl(slot, kControlHaptic, 0x21, 0x09, 0x0282, slot->interfaceNumber,
		TritonProtocol::kHapticCommandReportSize, report);
}

static void FinishHaptic(UsbSource* slot, int32_t status) {
	if (status == 0) {
		slot->hapticFailureCount = 0;
		return;
	}
	if (slot->hapticFailureCount < 255) ++slot->hapticFailureCount;
	if (slot->hapticFailureCount <= 3)
		DbgPrint("TritonDriver: pad haptic USB failed interface %d status %x\n",
			slot->interfaceNumber, status);
}

static UsbSource* FindSlotByInterruptTrb(void* trb) {
	for (int i = 0; i < kSlotCount; ++i)
		if (g_slots[i].extension && &g_slots[i].extension->interruptTrb == trb)
			return &g_slots[i];
	return 0;
}

static UsbSource* FindSlotByControlTrb(void* trb) {
	for (int i = 0; i < kSlotCount; ++i)
		if (g_slots[i].extension && &g_slots[i].extension->controlTrb == trb)
			return &g_slots[i];
	return 0;
}

static void OpenRumbleEndpoint(UsbSource* slot) {
	const usb_endpoint_descriptor& endpoint = slot->outputDescriptor;
	if (!UsbDescriptors::IsInterruptOutEndpoint(endpoint)) return;
	uint16_t packetSize = TritonProtocol::ReadLE16((const uint8_t*)&endpoint.wMaxPacketSize) & 0x7ff;
	if (packetSize < TritonProtocol::kRumbleReportSize || packetSize > kMaxHidPacketSize) {
		DbgPrint("TritonDriver: rumble invalid interrupt-OUT size %d interface %d\n", packetSize, slot->interfaceNumber);
		return;
	}
	OutputTransfer* output = (OutputTransfer*)calloc(1, sizeof(OutputTransfer));
	if (!output) return;
	NTSTATUS status = UsbdOpenEndpoint(slot->handle, 3, endpoint.bEndpointAddress,
		packetSize, endpoint.bInterval, (DWORD*)&output->trb);
	if (NT_ERROR(status)) {
		DbgPrint("TritonDriver: rumble interrupt-OUT open failed interface %d status %x\n", slot->interfaceNumber, status);
		free(output);
		return;
	}
	output->trb.buffer = output->report;
	output->trb.length = TritonProtocol::kRumbleReportSize;
	output->trb.flags = 1;
	output->trb.callback = (DWORD)OutputComplete;
	output->trb.savedEndpoint = output->trb.endpoint;
	slot->output = output;
	slot->outputEndpoint = endpoint.bEndpointAddress;
	DbgPrint("TritonDriver: rumble interrupt-OUT ready interface %d endpoint %02x\n",
		slot->interfaceNumber, slot->outputEndpoint);
}

static bool StartListening(UsbSource* slot) {
	UsbDescriptors::FindInterruptInEndpoint(slot->device->descriptor,
		slot->device->descriptorLength, slot->interfaceNumber, &slot->endpointDescriptor);
	UsbDescriptors::FindInterruptOutEndpoint(slot->device->descriptor,
		slot->device->descriptorLength, slot->interfaceNumber, &slot->outputDescriptor);
	usb_endpoint_descriptor* endpoint = &slot->endpointDescriptor;
	if (!UsbDescriptors::IsInterruptInEndpoint(*endpoint)) {
		DbgPrint("TritonDriver: USB interface %d has no interrupt-IN endpoint\n", slot->interfaceNumber);
		return false;
	}
	uint16_t packetSize = TritonProtocol::ReadLE16((const uint8_t*)&endpoint->wMaxPacketSize) & 0x7ff;
	if (packetSize == 0 || packetSize > kMaxHidPacketSize) {
		DbgPrint("TritonDriver: USB interface %d invalid packet size %d\n", slot->interfaceNumber, packetSize);
		return false;
	}
	slot->inputBuffer = (uint8_t*)calloc(1, packetSize);
	if (!slot->inputBuffer) return false;
	NTSTATUS result = UsbdOpenEndpoint(slot->handle, 3, endpoint->bEndpointAddress,
		packetSize, endpoint->bInterval, (DWORD*)&slot->extension->interruptTrb);
	if (NT_ERROR(result)) {
		free(slot->inputBuffer);
		slot->inputBuffer = 0;
		return false;
	}
	slot->inputLength = packetSize;
	UsbTrb* inputTrb = &slot->extension->interruptTrb;
	inputTrb->buffer = slot->inputBuffer;
	inputTrb->length = slot->inputLength;
	inputTrb->flags = 1;
	inputTrb->callback = (DWORD)InputComplete;
	// OpenEndpoint supplies the persistent endpoint pointer. Preserve it once;
	// the queue implementation may reuse trb.endpoint while the request runs.
	inputTrb->savedEndpoint = inputTrb->endpoint;
	OpenRumbleEndpoint(slot);
	// The puck's descriptors are not fetched; it forwards SDL-sized reports.
	slot->hapticSupported = slot->kind == ControllerUsbPolicy::kProteus ||
		slot->device->hapticCommand;
	slot->listening = true;
	DbgPrint("TritonDriver: USB interface %d listening endpoint %02x size %d interval %d\n",
		slot->interfaceNumber, endpoint->bEndpointAddress, packetSize, endpoint->bInterval);
	// Wired devices need raw mode before their first state report.
	slot->heartbeatEnabled = slot->kind == ControllerUsbPolicy::kWiredTriton;
	slot->heartbeatDeadline = 0;
	QueueInput(slot);
	return true;
}

static void StartNextConfiguration() {
	if (g_initializing) return;
	g_initializing = true;
	uint32_t now = GetTickCount();
	for (int i = 0; i < kSlotCount; ++i) {
		UsbSource* slot = &g_slots[i];
		if (!slot->handle || slot->removing || !slot->configurationPending) continue;
		UsbDeviceContext* device = slot->device;
		if (device->failed || device->configurationBusy || device->controlOwner != -1 ||
			(device->retryAt && (int32_t)(now - device->retryAt) < 0)) continue;
		ControlPurpose purpose = kControlNone;
		uint8_t type = 0x80, request = 6;
		uint16_t value = 0x0200, index = 0, length = 0;
		void* buffer = device->descriptor;
		if (!device->descriptorLength) {
			purpose = kControlGetConfigurationHeader;
			length = 9;
		} else if (!device->descriptorFetched) {
			purpose = kControlGetConfigurationDescriptor;
			length = device->descriptorLength;
		} else if (slot->kind == ControllerUsbPolicy::kWiredTriton && !device->hidValidated) {
			purpose = kControlGetHidDescriptor;
			type = 0x81; value = 0x2200; index = slot->interfaceNumber;
			length = device->hidLength; buffer = device->hidDescriptor;
		} else if (!device->currentConfigurationKnown) {
			purpose = kControlGetCurrentConfiguration;
			request = 8; value = 0; length = 1; buffer = &device->currentConfiguration;
		} else if (!device->configured) {
			purpose = kControlSetConfiguration;
			type = 0; request = 9; value = device->descriptor[5]; buffer = 0;
		} else {
			if (StartListening(slot)) slot->configurationPending = false;
			else device->retryAt = now + kHeartbeatRetryMs;
			continue;
		}
		// Set ownership before queueing; the kernel may complete inline.
		device->configurationBusy = true;
		if (!QueueControl(slot, purpose, type, request, value, index, length, buffer))
			device->configurationBusy = false;
	}
	g_initializing = false;
}

static void FinishRumble(UsbSource* slot, int32_t status, uint32_t now) {
	slot->rumble.Update(ControllerReadRumbleRequest(slot->token));
	slot->rumble.Complete(status == 0, now);
	if (status != 0 && slot->rumble.failures <= 3) {
		DbgPrint("TritonDriver: rumble USB failed interface %d left %u right %u status %x elapsed %u ms\n",
			slot->interfaceNumber, RumbleOutput::Left(slot->rumble.inFlight),
			RumbleOutput::Right(slot->rumble.inFlight), status, now - slot->rumble.submittedAt);
	}
}

static int32_t OutputComplete(DWORD trbAddress, int32_t status) {
	uint32_t now = GetTickCount();
	for (int i = 0; i < kSlotCount; ++i) {
		UsbSource* slot = &g_slots[i];
		if (!slot->output || &slot->output->trb != (void*)trbAddress) continue;
		if (slot->removing) {
			slot->rumble.pending = false;
			slot->hapticPending = false;
			UpdateRemovalReady(slot);
			return status;
		}
		if (slot->hapticPending) {
			slot->hapticPending = false;
			FinishHaptic(slot, status);
			DispatchOutputs(now);
			return status;
		}
		if (!slot->rumble.pending) return status;
		FinishRumble(slot, status, now);
		DispatchOutputs(now);
		return status;
	}
	return status;
}

static int32_t ControlComplete(DWORD trbAddress, int32_t status) {
	uint32_t now = GetTickCount();
	UsbSource* slot = FindSlotByControlTrb((void*)trbAddress);
	if (!slot) return status;
	int slotIndex = (int)(slot - g_slots);
	InterlockedCompareExchange(&slot->device->controlOwner, -1, slotIndex);
	ControlPurpose purpose = slot->controlPurpose;
	slot->controlPurpose = kControlNone;
	if (slot->removing) {
		slot->controlBusy = false;
		if (purpose != kControlRumble && purpose != kControlHaptic &&
			purpose != kControlLizardOff)
			slot->device->configurationBusy = false;
		UpdateRemovalReady(slot);
		return status;
	}
	if (purpose == kControlRumble) {
		FinishRumble(slot, status, now);
		slot->controlBusy = false;
		StartNextConfiguration();
		DispatchOutputs(now);
		return status;
	}
	if (purpose == kControlHaptic) {
		FinishHaptic(slot, status);
		slot->controlBusy = false;
		StartNextConfiguration();
		DispatchOutputs(now);
		return status;
	}
	if (purpose == kControlGetConfigurationHeader || purpose == kControlGetConfigurationDescriptor ||
		purpose == kControlGetHidDescriptor || purpose == kControlGetCurrentConfiguration ||
		purpose == kControlSetConfiguration) {
		UsbDeviceContext* device = slot->device;
		device->configurationBusy = false;
		device->retryAt = 0;
		uint32_t length = slot->extension->controlTrb.transferredBytes;
		if (status != 0) {
			device->retryAt = now + kHeartbeatRetryMs;
		} else if (purpose == kControlGetConfigurationHeader) {
			uint16_t total = TritonProtocol::ReadLE16(device->descriptor + 2);
			if (length != 9 || device->descriptor[0] != 9 || device->descriptor[1] != 2 ||
				total < 9 || total > sizeof(device->descriptor) || !device->descriptor[5])
				device->failed = true;
			else device->descriptorLength = total;
		} else if (purpose == kControlGetConfigurationDescriptor) {
			if (length != device->descriptorLength || device->descriptor[0] != 9 ||
				device->descriptor[1] != 2 ||
				TritonProtocol::ReadLE16(device->descriptor + 2) != length)
				device->failed = true;
			else {
				device->descriptorFetched = true;
				if (slot->kind == ControllerUsbPolicy::kWiredTriton) {
					device->hidLength = UsbDescriptors::HidReportLength(device->descriptor,
						length, slot->interfaceNumber);
					// Wired Triton has one combined HID interface. Refuse other
					// topologies until physical-parent grouping is available.
					if (device->descriptor[4] != 1 || !device->hidLength ||
						device->hidLength > sizeof(device->hidDescriptor)) device->failed = true;
				}
			}
		} else if (purpose == kControlGetHidDescriptor) {
			device->hidValidated = length == device->hidLength &&
				TritonHidDescriptor::Validate(device->hidDescriptor, length,
					&device->hapticCommand);
			if (!device->hidValidated) device->failed = true;
		} else if (purpose == kControlGetCurrentConfiguration) {
			if (length != 1) device->failed = true;
			else {
				device->currentConfigurationKnown = true;
				device->configured = device->currentConfiguration == device->descriptor[5];
				if (device->currentConfiguration && !device->configured) device->failed = true;
			}
		} else device->configured = true;
		if (device->failed)
			DbgPrint("TritonDriver: unsupported USB descriptors/configuration, source %d interface %d\n",
				slot->token.index, slot->interfaceNumber);
		slot->controlBusy = false;
		return status;
	}
	if (purpose != kControlLizardOff) {
		slot->controlBusy = false;
		return status;
	}
	if (status == 0) {
		if (!slot->loggedLizardSuccess) {
			slot->loggedLizardSuccess = true;
			DbgPrint("TritonDriver: USB source %u lizard-off request completed successfully\n",
				slot->token.index);
		}
		slot->retryDelay = kHeartbeatRetryMs;
		slot->featureFailureCount = 0;
		slot->heartbeatEnabled = true;
		slot->heartbeatDeadline = GetTickCount() + kHeartbeatIntervalMs;
	} else {
		if (slot->featureFailureCount < 255) ++slot->featureFailureCount;
		if (slot->featureFailureCount <= 3 || slot->connected)
			DbgPrint("TritonDriver: USB source %u lizard-off request failed: %x\n", slot->token.index, status);
		if (ControllerUsbPolicy::ShouldPauseHeartbeat(slot->kind, slot->connected, slot->featureFailureCount)) {
			slot->heartbeatEnabled = false;
			DbgPrint("TritonDriver: USB source %u empty; pausing lizard probes until wireless activity\n",
				slot->interfaceNumber);
			slot->controlBusy = false;
			StartNextConfiguration();
			return status;
		}
		slot->heartbeatDeadline = GetTickCount() + slot->retryDelay;
		if (slot->retryDelay < kHeartbeatIntervalMs) slot->retryDelay *= 2;
		if (slot->retryDelay > kHeartbeatIntervalMs) slot->retryDelay = kHeartbeatIntervalMs;
	}
	MemoryBarrier();
	slot->controlBusy = false;
	StartNextConfiguration();
	DispatchOutputs(GetTickCount());
	return status;
}

static void DispatchOutputs(uint32_t now) {
	// All callers are serialized with USB DPCs. Guard synchronous completion
	// reentry; asynchronous completions can drain other slots without a tick wait.
	if (g_dispatchingOutputs) return;
	g_dispatchingOutputs = true;
	// Keep firmware raw mode alive even when games change strengths constantly.
	for (int offset = 0; offset < kSlotCount; ++offset) {
		int i = (g_nextHeartbeatSlot + offset) % kSlotCount;
		UsbSource* slot = &g_slots[i];
		if (!slot->handle || slot->removing || !slot->listening ||
			!slot->heartbeatEnabled || slot->controlBusy || slot->device->configurationBusy) continue;
		if ((!slot->heartbeatDeadline || (int32_t)(now - slot->heartbeatDeadline) >= 0) && QueueLizardOff(slot)) {
			g_nextHeartbeatSlot = (i + 1) % kSlotCount;
			break;
		}
	}
	int firstRumbleSlot = g_nextRumbleSlot;
	for (int offset = 0; offset < kSlotCount; ++offset) {
		int i = (firstRumbleSlot + offset) % kSlotCount;
		UsbSource* slot = &g_slots[i];
		if (!slot->handle || slot->removing || !slot->listening || !slot->connected) continue;
		slot->rumble.Update(ControllerReadRumbleRequest(slot->token));
		// Pad pulses are short and time-sensitive; rumble follows on completion.
		if (QueueHaptic(slot, now) || QueueRumble(slot, now)) {
			g_nextRumbleSlot = (i + 1) % kSlotCount;
		}
	}
	g_dispatchingOutputs = false;
}

static int32_t QueueInput(UsbSource* slot) {
	if (!slot || slot->removing || !slot->listening) return 0;
	// The interrupt callback and maintenance thread may both try to recover the
	// input pipe. Claim the single reusable TRB before touching or queueing it.
	if (InterlockedCompareExchange(&slot->inputPending, 1, 0) != 0) return 0;
	slot->inputRetryDeadline = 0;
	memset(slot->inputBuffer, 0, slot->inputLength);
	UsbTrb* trb = &slot->extension->interruptTrb;
	trb->length = slot->inputLength;
	slot->extension->inputTransferredBytes = 0;
	// As with control transfers, the return value is an opaque queue token.
	// Keep ownership until InputComplete releases it.
	int queueToken = UsbdQueueAsyncTransfer(slot->handle, trb);
	if (!slot->loggedInputQueueResult) {
		slot->loggedInputQueueResult = true;
		DbgPrint("TritonDriver: USB source %u input transfer queued token %x endpoint %02x\n",
			slot->token.index, queueToken, slot->endpointDescriptor.bEndpointAddress);
	}
	return queueToken;
}

static int32_t InputComplete(DWORD trbAddress, int32_t status) {
	UsbSource* slot = FindSlotByInterruptTrb((void*)trbAddress);
	if (!slot) return status;
	if (InterlockedCompareExchange(&slot->inputPending, 0, 1) != 1) {
		DbgPrint("TritonDriver: USB source %u unexpected input completion with no transfer pending\n",
			slot->token.index);
		return status;
	}
	if (slot->removing) {
		UpdateRemovalReady(slot);
		return status;
	}
	if (!slot->loggedInputCompletion) {
		slot->loggedInputCompletion = true;
		DbgPrint("TritonDriver: USB source %u first input completion status %x\n",
			slot->token.index, status);
	}
	if (status != 0) {
		++slot->inputErrorCount;
		if (slot->inputErrorCount <= 3 ||
			(slot->inputErrorCount & (slot->inputErrorCount - 1)) == 0)
			DbgPrint("TritonDriver: USB source %u input error %x count %d; retrying with backoff\n",
				slot->interfaceNumber, status, slot->inputErrorCount);
		if (slot->connected) ControllerDisconnect(slot->token);
		slot->connected = false;
		slot->inputRetryDeadline = GetTickCount() + kInputRetryMs;
		return status;
	}
	size_t received = ControllerUsbPolicy::InputLength(slot->extension->inputTransferredBytes, slot->inputLength);
	if (!received) return QueueInput(slot);
	// A successful interrupt report means this interface is active even if the
	// report ID is newer than the decoder. Prioritize its lizard-off request.
	if (!slot->heartbeatEnabled) {
		slot->heartbeatEnabled = true;
		slot->heartbeatDeadline = 0;
	}

	TritonProtocol::InputState input;
	TritonProtocol::WirelessStatus wireless;
	if (!slot->loggedFirstReport) {
		slot->loggedFirstReport = true;
		DbgPrint("TritonDriver: USB source %u first interrupt report id %02x\n",
			slot->token.index, slot->inputBuffer[0]);
	}
	if (TritonProtocol::DecodeInputPrefix(slot->inputBuffer, received, &input)) {
		if (!slot->loggedFirstState) {
			slot->loggedFirstState = true;
			DbgPrint("TritonDriver: USB source %u accepted state report %02x\n",
				slot->token.index, input.reportId);
		}
		TritonProtocol::ControllerState report;
		TritonProtocol::ConvertToControllerState(input, &report);
		TritonProtocol::RightPadState rightPad;
		TritonProtocol::RightPadState* rightPadPointer =
			TritonProtocol::DecodeRightPad(slot->inputBuffer, received, &rightPad) ?
			&rightPad : 0;
		bool wasConnected = slot->connected;
		slot->connected = true;
		slot->heartbeatEnabled = true;
		if (!wasConnected)
			slot->heartbeatDeadline = 0;
		ControllerPublishState(slot->token, report, rightPadPointer, GetTickCount());
	} else if (slot->kind == ControllerUsbPolicy::kProteus && TritonProtocol::DecodeWirelessStatus(slot->inputBuffer, received, &wireless)) {
		if (wireless == TritonProtocol::kWirelessDisconnected) {
			slot->connected = false;
			slot->heartbeatEnabled = false;
			ControllerDisconnect(slot->token);
		} else {
			bool wasConnected = slot->connected;
			slot->connected = true;
			slot->featureFailureCount = 0;
			slot->heartbeatEnabled = true;
			if (!wasConnected)
				slot->heartbeatDeadline = 0;
		}
	}
	return QueueInput(slot);
}

} // namespace

int ControllerUsbAdd(deviceHandle* handle, const usb_interface_descriptor* descriptor,
	ControllerUsbPolicy::Kind kind) {
	if (!handle || !descriptor || kind == ControllerUsbPolicy::kUnsupported) return -1;
	UsbSource* duplicate = FindSlotByHandle(handle);
	if (duplicate) return duplicate->removing ? -1 : 0;
	int index = -1;
	if (kind == ControllerUsbPolicy::kProteus) {
		index = descriptor->bInterfaceNumber - TritonProtocol::kFirstSlotInterface;
		if (index < 0 || index >= ControllerRouting::kPuckSlotCount || g_slots[index].handle) return -1;
		// Do not join a puck that is still draining its previous attachment.
		for (int i = 0; i < ControllerRouting::kPuckSlotCount; ++i) if (g_slots[i].removing) return -1;
	} else {
		for (int i = ControllerRouting::kPuckSlotCount; i < kSlotCount; ++i)
			if (!g_slots[i].handle) { index = i; break; }
		if (index < 0) return -1;
	}
	UsbSource* slot = &g_slots[index];
	memset(slot, 0, sizeof(*slot));
	if (!ControllerAttachSource(index, &slot->token)) return -1;
	slot->handle = handle;
	slot->kind = kind;
	slot->interfaceNumber = descriptor->bInterfaceNumber;
	slot->retryDelay = kHeartbeatRetryMs;
	slot->device = kind == ControllerUsbPolicy::kProteus ? g_puckDevice : 0;
	if (!slot->device) {
		slot->device = new UsbDeviceContext();
		if (slot->device) {
			memset(slot->device, 0, sizeof(*slot->device));
			slot->device->controlOwner = -1;
			if (kind == ControllerUsbPolicy::kProteus) g_puckDevice = slot->device;
		}
	}
	slot->extension = new UsbControllerExtension();
	slot->reports = new ControlReports();
	if (!slot->device || !slot->extension || !slot->reports) {
		// No transfers exist yet, but routing retirement still needs its ack.
		slot->removing = true;
		slot->removeCompleteCalled = true;
		ControllerRetireSource(slot->token);
		UpdateRemovalReady(slot);
		return -1;
	}
	memset(slot->extension, 0, sizeof(*slot->extension));
	slot->extension->deviceHandle = handle;
	slot->extension->deviceType = 1;
	handle->driver = slot->extension;
	UsbdAddDeviceComplete(handle, 0);
	NTSTATUS result = UsbdOpenDefaultEndpoint(handle, (DWORD*)&slot->extension->controlTrb);
	if (NT_ERROR(result)) {
		ControllerUsbRemove(handle);
		return result;
	}
	UsbTrb* control = &slot->extension->controlTrb.trb;
	control->flags = 1;
	control->callback = (DWORD)ControlComplete;
	control->savedEndpoint = control->endpoint;
	slot->configurationPending = true;
	DbgPrint("TritonDriver: %s source %d interface %d initializing\n",
		kind == ControllerUsbPolicy::kProteus ? "Proteus" : "wired Triton", index, slot->interfaceNumber);
	StartNextConfiguration();
	return 0;
}

bool ControllerUsbRemove(deviceHandle* handle) {
	UsbSource* slot = FindSlotByHandle(handle);
	if (!slot) return false;
	if (slot->removing) return true;
	slot->removing = true;
	DbgPrint("TritonDriver: USB interface %d removal begin input %d control %d\n",
		slot->interfaceNumber, slot->inputPending, slot->controlBusy);
	ControllerRetireSource(slot->token);
	// Completion owns the arbiter until its callback, even during removal.
	// Explicit endpoint closes can block during physical composite-device
	// removal. Let the USB core cancel the pipes as part of remove completion;
	// all TRB storage remains quarantined, so late callbacks stay memory-safe.
	DbgPrint("TritonDriver: USB interface %d calling kernel removal complete\n",
		slot->interfaceNumber);
	UsbdRemoveDeviceComplete(handle);
	DbgPrint("TritonDriver: USB interface %d kernel removal returned\n",
		slot->interfaceNumber);
	slot->removeCompleteCalled = true;
	UpdateRemovalReady(slot);
	bool anySlotsRemain = false;
	for (int i = 0; i < kSlotCount; ++i)
		if (g_slots[i].handle && !g_slots[i].removing) anySlotsRemain = true;
	if (anySlotsRemain) StartNextConfiguration();
	return true;
}

void ControllerUsbMaintenance(uint32_t nowMilliseconds) {
	for (int i = 0; i < kSlotCount; ++i) {
		UsbSource* slot = &g_slots[i];
		if (slot->removing) {
			if (!slot->loggedRemovalWait) {
			slot->loggedRemovalWait = true;
			DbgPrint("TritonDriver: USB interface %d waiting for removal transfers input %d control %d\n",
				slot->interfaceNumber, slot->inputPending, slot->controlBusy);
			}
			UpdateRemovalReady(slot);
			if (slot->cleanupReady &&
				(int32_t)(nowMilliseconds - slot->cleanupDeadline) >= 0)
				FinalizeRemoval(slot);
			continue;
		}
		if (slot->handle && !slot->removing && slot->listening &&
			!slot->inputPending && slot->inputRetryDeadline != 0 &&
			(int32_t)(nowMilliseconds - slot->inputRetryDeadline) >= 0)
			QueueInput(slot);
		if (slot->rumble.pending && !slot->loggedRumbleWait &&
			(uint32_t)(nowMilliseconds - slot->rumble.submittedAt) >= 250) {
			slot->loggedRumbleWait = true;
			DbgPrint("TritonDriver: rumble USB pending over 250 ms interface %d via %s endpoint %02x control owner %d\n",
				slot->interfaceNumber, slot->output ? "interrupt-OUT" : "control",
				slot->outputEndpoint, slot->device->controlOwner);
			// A timeout is diagnostic only. USB may still own the buffer/TRB.
		}
	}
	StartNextConfiguration();
	DispatchOutputs(nowMilliseconds);
}
