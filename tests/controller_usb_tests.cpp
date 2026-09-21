#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <vector>

// Compile the real USB engine with a 32-bit fake kernel. This preserves the
// transfer layout assertions while allowing deterministic completion ordering.
#include "../hiddriver/controller_usb.cpp"

struct PendingTransfer { deviceHandle* handle; UsbTrb* trb; };
static std::vector<PendingTransfer> pending;
static uint32_t clockMs;
static unsigned endpointId = 100;
static unsigned addCount, removeCount;
static bool autoRetire = true;
static bool inlineSetup;
static std::vector<uint8_t> inlineConfiguration;
static void CompleteInlineSetup(UsbSource* source);
struct FakeSource {
	uint32_t epoch;
	bool attached, retiring, retired;
	unsigned published;
	TritonProtocol::ControllerState state;
	uint64_t rumble;
};
static FakeSource routes[8];

uint32_t UsbTestNow() { return clockMs; }
static int AddComplete(deviceHandle*, int status) { assert(status == 0); ++addCount; return 0; }
static NTSTATUS RemoveComplete(deviceHandle*) { ++removeCount; return 0; }
static NTSTATUS OpenDefault(deviceHandle*, DWORD* endpoint) { *endpoint = endpointId++; return 0; }
static NTSTATUS OpenEndpoint(deviceHandle*, int, int, int, int, DWORD* endpoint) {
	*endpoint = endpointId++; return 0;
}
static int Queue(deviceHandle* handle, void* transfer) {
	PendingTransfer p = { handle, (UsbTrb*)transfer };
	for (size_t i = 0; i < pending.size(); ++i) assert(pending[i].trb != p.trb);
	pending.push_back(p);
	UsbSource* source = FindSlotByHandle(handle);
	if (inlineSetup && p.trb == &source->extension->controlTrb.trb)
		CompleteInlineSetup(source);
	return (int)0x80000001; // Opaque token, not a failing NTSTATUS.
}
AddCompleteFn UsbdAddDeviceComplete = AddComplete;
QueueTransferFn UsbdQueueAsyncTransfer = Queue;
OpenDefaultEndpointFn UsbdOpenDefaultEndpoint = OpenDefault;
OpenEndpointFn UsbdOpenEndpoint = OpenEndpoint;
RemoveCompleteFn UsbdRemoveDeviceComplete = RemoveComplete;

bool ControllerAttachSource(uint32_t index, ControllerSourceToken* token) {
	FakeSource& s = routes[index];
	if (s.attached && !s.retired) return false;
	++s.epoch; s.attached = true; s.retiring = s.retired = false;
	s.published = 0; s.rumble = 0;
	token->index = index; token->attachmentEpoch = s.epoch;
	return true;
}
static bool Live(ControllerSourceToken token) {
	return ControllerRouting::TokenMatches(token, routes[token.index].epoch);
}
void ControllerPublishState(ControllerSourceToken token, const TritonProtocol::ControllerState& state) {
	assert(Live(token) && !routes[token.index].retiring);
	routes[token.index].state = state;
	++routes[token.index].published;
}
void ControllerDisconnect(ControllerSourceToken token) {
	assert(Live(token)); routes[token.index].rumble = 0;
}
void ControllerRetireSource(ControllerSourceToken token) {
	assert(Live(token)); routes[token.index].retiring = true;
	routes[token.index].retired = autoRetire;
}
bool ControllerSourceRetired(ControllerSourceToken token) {
	return Live(token) && routes[token.index].retired;
}
uint64_t ControllerReadRumbleRequest(ControllerSourceToken token) {
	assert(Live(token)); return routes[token.index].rumble;
}

// Synthetic protocol declarations, not a hardware capture.
static const uint8_t hid[] = {
	0x06, 0x00, 0xff, 0x09, 1, 0xa1, 1,
	0x75, 8, 0x85, 0x42, 0x95, 63, 0x81, 2,
	0x85, 1, 0x95, 63, 0xb1, 2,
	0x85, 0x80, 0x95, 9, 0x91, 2, 0xc0
};

static usb_interface_descriptor Interface(uint8_t number) {
	usb_interface_descriptor d = { 9, 4, number, 0, 2, 3, 0, 0, 0 };
	return d;
}
static std::vector<uint8_t> Configuration(bool puck, bool output = true) {
	uint8_t header[] = { 9, 2, 0, 0, (uint8_t)(puck ? 4 : 1), 1, 0, 0x80, 50 };
	std::vector<uint8_t> bytes(header, header + sizeof(header));
	for (int i = 0; i < (puck ? 4 : 1); ++i) {
		usb_interface_descriptor d = Interface((uint8_t)(puck ? i + 2 : 0));
		d.bNumEndpoints = output ? 2 : 1;
		const uint8_t* p = (const uint8_t*)&d;
		bytes.insert(bytes.end(), p, p + 9);
		uint8_t hd[] = { 9, 0x21, 0x11, 1, 0, 1, 0x22, sizeof(hid), 0 };
		bytes.insert(bytes.end(), hd, hd + sizeof(hd));
		uint8_t in[] = { 7, 5, (uint8_t)(0x81 + i), 3, 64, 0, 1 };
		uint8_t out[] = { 7, 5, (uint8_t)(1 + i), 3, 64, 0, 1 };
		bytes.insert(bytes.end(), in, in + sizeof(in));
		if (output) bytes.insert(bytes.end(), out, out + sizeof(out));
	}
	bytes[2] = (uint8_t)bytes.size(); bytes[3] = (uint8_t)(bytes.size() >> 8);
	return bytes;
}

static int FindPending(UsbTrb* trb) {
	for (size_t i = 0; i < pending.size(); ++i) if (pending[i].trb == trb) return (int)i;
	return -1;
}
static void Complete(UsbTrb* trb, int32_t status, const uint8_t* data, uint32_t length) {
	int index = FindPending(trb); assert(index >= 0);
	pending.erase(pending.begin() + index);
	if (data) { assert(length <= trb->length); memcpy(trb->buffer, data, length); }
	*(uint32_t*)((uint8_t*)trb + 0x1c) = length;
	typedef int32_t (*Callback)(DWORD, int32_t);
	((Callback)trb->callback)((DWORD)trb, status);
}
static void Tick(uint32_t delta = 5) { clockMs += delta; ControllerUsbMaintenance(clockMs); }
static void CompleteInlineSetup(UsbSource* source) {
	UsbTrb* trb = &source->extension->controlTrb.trb;
	switch (source->controlPurpose) {
	case kControlGetConfigurationHeader: Complete(trb, 0, &inlineConfiguration[0], 9); break;
	case kControlGetConfigurationDescriptor:
		Complete(trb, 0, &inlineConfiguration[0], (uint32_t)inlineConfiguration.size()); break;
	case kControlGetHidDescriptor: Complete(trb, 0, hid, sizeof(hid)); break;
	case kControlGetCurrentConfiguration: { uint8_t value = 1; Complete(trb, 0, &value, 1); break; }
	default: break;
	}
}
static void CompleteSetup(UsbSource* source, const std::vector<uint8_t>& config) {
	UsbControlTrb* control = &source->extension->controlTrb;
	for (unsigned i = 0; i < 16 && !source->listening && !source->device->failed; ++i) {
		Tick();
		if (source->listening) break;
		if (FindPending(&control->trb) < 0) continue;
		switch (source->controlPurpose) {
		case kControlGetConfigurationHeader: Complete(&control->trb, 0, &config[0], 9); break;
		case kControlGetConfigurationDescriptor: Complete(&control->trb, 0, &config[0], (uint32_t)config.size()); break;
		case kControlGetHidDescriptor: Complete(&control->trb, 0, hid, sizeof(hid)); break;
		case kControlGetCurrentConfiguration: { uint8_t value = 1; Complete(&control->trb, 0, &value, 1); break; }
		default: assert(false);
		}
	}
	assert(source->listening);
}
static void Reset() {
	// The real driver quarantines allocations for its session. Tests do too.
	pending.clear(); memset(g_slots, 0, sizeof(g_slots)); memset(routes, 0, sizeof(routes));
	g_puckDevice = 0; g_initializing = g_dispatchingOutputs = false;
	g_nextHeartbeatSlot = g_nextRumbleSlot = 0; clockMs = 0;
	addCount = removeCount = 0; autoRetire = true;
	inlineSetup = false;
}

static void TestWiredStartupInputAndStatus() {
	Reset(); deviceHandle handle = {}; usb_interface_descriptor d = Interface(0);
	assert(ControllerUsbAdd(&handle, &d, ControllerUsbPolicy::kWiredTriton) == 0);
	UsbSource* source = &g_slots[4]; CompleteSetup(source, Configuration(false));
	assert(source->heartbeatEnabled && !source->connected);
	assert(source->controlPurpose == kControlLizardOff); // Before any input.
	UsbControlTrb* feature = &source->extension->controlTrb;
	assert(Swap16(feature->packet.wIndex) == 0 && Swap16(feature->packet.wValue) == 0x0301);
	for (int i = 0; i < 5; ++i) {
		Complete(&feature->trb, -1, 0, 0); Tick(2000);
		assert(source->heartbeatEnabled && source->controlPurpose == kControlLizardOff);
	}
	Complete(&feature->trb, 0, 0, 64);
	uint8_t report[64] = { 0x42, 1, 1 };
	Complete(&source->extension->interruptTrb, 0, report, 3);
	assert(routes[4].published == 0 && !source->connected);
	Complete(&source->extension->interruptTrb, 0, report, 18);
	assert(routes[4].published == 1 && routes[4].state.a && source->connected);
	uint8_t wireless[] = { 0x46, 1 };
	Complete(&source->extension->interruptTrb, 0, wireless, 2);
	assert(source->connected); // Wireless lifecycle does not apply to USB.
	Complete(&source->extension->interruptTrb, -1, 0, 0);
	assert(!source->connected && FindPending(&source->extension->interruptTrb) < 0);
	Tick(50); assert(FindPending(&source->extension->interruptTrb) >= 0);
	Complete(&source->extension->interruptTrb, 0, report, 18);
	assert(routes[4].published == 2);
}

static void TestDeviceIsolationAndCapacity() {
	Reset(); deviceHandle wired[5] = {}, puck[4] = {};
	usb_interface_descriptor wiredInterface = Interface(0);
	for (int i = 0; i < 4; ++i) {
		usb_interface_descriptor d = Interface((uint8_t)(i + 2));
		assert(ControllerUsbAdd(&puck[i], &d, ControllerUsbPolicy::kProteus) == 0);
		assert(ControllerUsbAdd(&wired[i], &wiredInterface, ControllerUsbPolicy::kWiredTriton) == 0);
	}
	assert(addCount == 8);
	assert(ControllerUsbAdd(&wired[4], &wiredInterface, ControllerUsbPolicy::kWiredTriton) != 0);
	assert(ControllerUsbAdd(&wired[0], &wiredInterface, ControllerUsbPolicy::kWiredTriton) == 0);
	assert(addCount == 8);
	for (int i = 0; i < 4; ++i) {
		assert(g_slots[i].device == g_slots[0].device);
		assert(g_slots[i + 4].device != g_slots[0].device);
		assert(g_slots[i + 4].device->controlOwner == i + 4);
	}
	// Leave the puck and three wired devices' control requests stalled.
	CompleteSetup(&g_slots[4], Configuration(false));
	assert(!g_slots[5].listening && !g_slots[0].listening);
	uint8_t report[18] = { 0x42, 0, 1 };
	Complete(&g_slots[4].extension->interruptTrb, 0, report, sizeof(report));
	routes[4].rumble = RumbleOutput::Request(1, 1234, 4321);
	Tick(); assert(g_slots[4].rumble.pending);
	assert(g_slots[4].output->report[0] == 0x80);
	assert(TritonProtocol::ReadLE16(g_slots[4].output->report + 4) == 1234);
	assert(!g_slots[5].rumble.pending);
}

static void TestRetirementAndLateCompletion() {
	Reset(); deviceHandle oldHandle = {}, newHandle = {}; usb_interface_descriptor d = Interface(0);
	assert(ControllerUsbAdd(&oldHandle, &d, ControllerUsbPolicy::kWiredTriton) == 0);
	CompleteSetup(&g_slots[4], Configuration(false));
	UsbTrb* oldInput = &g_slots[4].extension->interruptTrb;
	UsbTrb* oldControl = &g_slots[4].extension->controlTrb.trb;
	ControlReports* oldReports = g_slots[4].reports;
	ControllerSourceToken oldToken = g_slots[4].token;
	autoRetire = false;
	assert(ControllerUsbRemove(&oldHandle)); assert(ControllerUsbRemove(&oldHandle));
	assert(removeCount == 1);
	Tick(1500); assert(g_slots[4].handle == &oldHandle);
	routes[4].retired = true; Tick(); assert(!g_slots[4].handle);
	assert(ControllerUsbAdd(&newHandle, &d, ControllerUsbPolicy::kWiredTriton) == 0);
	assert(g_slots[4].token.attachmentEpoch != oldToken.attachmentEpoch);
	assert(&g_slots[4].extension->interruptTrb != oldInput);
	assert(g_slots[4].reports != oldReports);
	UsbDeviceContext* device = g_slots[4].device;
	assert(device->controlOwner == 4);
	Complete(oldControl, 0, 0, 64); Complete(oldInput, -1, 0, 0);
	assert(device->controlOwner == 4 && routes[4].published == 0);
}

static void TestDescriptorFailureAndControlRumble() {
	Reset(); deviceHandle handle = {}; usb_interface_descriptor d = Interface(0);
	assert(ControllerUsbAdd(&handle, &d, ControllerUsbPolicy::kWiredTriton) == 0);
	UsbSource* source = &g_slots[4];
	std::vector<uint8_t> config = Configuration(false);
	Complete(&source->extension->controlTrb.trb, 0, &config[0], 8);
	Tick(); assert(source->device->failed && !source->listening && !source->heartbeatEnabled);
	Reset(); handle.driver = 0;
	assert(ControllerUsbAdd(&handle, &d, ControllerUsbPolicy::kWiredTriton) == 0);
	source = &g_slots[4]; CompleteSetup(source, Configuration(false, false));
	assert(!source->output);
	Complete(&source->extension->controlTrb.trb, 0, 0, 64);
	uint8_t report[18] = { 0x42 };
	Complete(&source->extension->interruptTrb, 0, report, sizeof(report));
	routes[4].rumble = RumbleOutput::Request(1, 100, 200);
	Tick(); // First valid state prioritizes raw mode once more.
	if (source->controlPurpose == kControlLizardOff)
		Complete(&source->extension->controlTrb.trb, 0, 0, 64);
	Tick(); assert(source->controlPurpose == kControlRumble);
	assert(Swap16(source->extension->controlTrb.packet.wValue) == 0x0280);
	Complete(&source->extension->controlTrb.trb, 0, 0, 10);
	routes[4].rumble = RumbleOutput::Request(1, 0, 0); Tick();
	assert(source->controlPurpose == kControlRumble);
}

static void TestPuckActivityAndSharedControl() {
	Reset(); deviceHandle handles[2] = {};
	usb_interface_descriptor first = Interface(2), second = Interface(3);
	assert(ControllerUsbAdd(&handles[0], &first, ControllerUsbPolicy::kProteus) == 0);
	CompleteSetup(&g_slots[0], Configuration(true));
	assert(!g_slots[0].heartbeatEnabled && !g_slots[0].connected);
	assert(ControllerUsbAdd(&handles[1], &second, ControllerUsbPolicy::kProteus) == 0);
	Tick(); assert(g_slots[1].listening && g_slots[1].device == g_slots[0].device);
	uint8_t state[18] = { 0x42, 0, 1 };
	Complete(&g_slots[0].extension->interruptTrb, 0, state, sizeof(state));
	Complete(&g_slots[1].extension->interruptTrb, 0, state, sizeof(state));
	Tick(); assert(g_slots[0].controlBusy && !g_slots[1].controlBusy);
	Complete(&g_slots[0].extension->controlTrb.trb, 0, 0, 64);
	assert(g_slots[1].controlBusy); // Shared EP0 passes to the other slot.
	Complete(&g_slots[1].extension->controlTrb.trb, 0, 0, 64);
	uint8_t disconnected[] = { 0x46, 1 };
	Complete(&g_slots[0].extension->interruptTrb, 0, disconnected, 2);
	assert(!g_slots[0].connected && !g_slots[0].heartbeatEnabled && g_slots[1].connected);
	assert(ControllerUsbRemove(&handles[0])); Tick(1500);
	assert(!g_slots[0].handle && g_slots[1].listening);
}

static void TestRejectedReportsAndConfigurationSelection() {
	Reset(); deviceHandle handle = {}; usb_interface_descriptor d = Interface(0);
	assert(ControllerUsbAdd(&handle, &d, ControllerUsbPolicy::kWiredTriton) == 0);
	UsbSource* source = &g_slots[4]; UsbTrb* control = &source->extension->controlTrb.trb;
	std::vector<uint8_t> config = Configuration(false);
	Complete(control, 0, &config[0], 9); Tick();
	Complete(control, 0, &config[0], (uint32_t)config.size()); Tick();
	uint8_t bad[sizeof(hid)]; memcpy(bad, hid, sizeof(hid)); bad[18] = 62;
	Complete(control, 0, bad, sizeof(bad)); Tick();
	assert(source->device->failed && !source->listening && routes[4].published == 0);
	Reset(); handle.driver = 0;
	assert(ControllerUsbAdd(&handle, &d, ControllerUsbPolicy::kWiredTriton) == 0);
	source = &g_slots[4]; control = &source->extension->controlTrb.trb;
	config[5] = 7;
	Complete(control, 0, &config[0], 9); Tick();
	Complete(control, 0, &config[0], (uint32_t)config.size()); Tick();
	Complete(control, 0, hid, sizeof(hid)); Tick();
	uint8_t unconfigured = 0; Complete(control, 0, &unconfigured, 1); Tick();
	assert(source->controlPurpose == kControlSetConfiguration);
	assert(Swap16(source->extension->controlTrb.packet.wValue) == 7);
	Complete(control, -1, 0, 0); Tick(100);
	assert(FindPending(control) < 0); Tick(150);
	assert(source->controlPurpose == kControlSetConfiguration);
	Complete(control, 0, 0, 0); Tick(); assert(source->listening);
}

static void TestSynchronousSetupAtClockWrap() {
	Reset(); clockMs = 0xfffffff0u;
	inlineConfiguration = Configuration(false); inlineSetup = true;
	deviceHandle handle = {}; usb_interface_descriptor d = Interface(0);
	assert(ControllerUsbAdd(&handle, &d, ControllerUsbPolicy::kWiredTriton) == 0);
	for (int i = 0; i < 8 && !g_slots[4].listening; ++i) Tick();
	assert(g_slots[4].listening && !g_slots[4].device->configurationBusy);
	assert(g_slots[4].controlPurpose == kControlLizardOff);
	assert(g_slots[4].device->controlOwner == 4);
}

int main() {
	TestWiredStartupInputAndStatus();
	TestDeviceIsolationAndCapacity();
	TestRetirementAndLateCompletion();
	TestDescriptorFailureAndControlRumble();
	TestPuckActivityAndSharedControl();
	TestRejectedReportsAndConfigurationSelection();
	TestSynchronousSetupAtClockWrap();
	puts("USB transport tests passed");
	return 0;
}
