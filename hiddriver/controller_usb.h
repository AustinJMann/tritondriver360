#pragma once

#include "driver_types.h"
#include "controller_routing.h"
#include "controller_usb_policy.h"
#include "triton_protocol.h"

int ControllerUsbAdd(deviceHandle* handle,
	const usb_interface_descriptor* descriptor, ControllerUsbPolicy::Kind kind);
bool ControllerUsbRemove(deviceHandle* handle);
// Hardware thread 2, IRQL 2; serialized with USB completion DPCs.
void ControllerUsbMaintenance(uint32_t nowMilliseconds);

// Routing lives in main.cpp; only the binding worker calls XAM.
bool ControllerAttachSource(uint32_t index, ControllerSourceToken* token);
void ControllerPublishState(ControllerSourceToken token,
	const TritonProtocol::ControllerState& state);
void ControllerDisconnect(ControllerSourceToken token);
void ControllerRetireSource(ControllerSourceToken token);
bool ControllerSourceRetired(ControllerSourceToken token);
uint64_t ControllerReadRumbleRequest(ControllerSourceToken token);
