#pragma once

#include <stdint.h>

#include "driver_types.h"
#include "triton_protocol.h"

bool ProteusIsSlot(uint16_t vendorId, uint16_t productId,
	const usb_interface_descriptor* interfaceDescriptor);
int ProteusAddSlotInterface(deviceHandle* handle,
	const usb_interface_descriptor* interfaceDescriptor);
bool ProteusRemoveSlotInterface(deviceHandle* handle);
// Call on hardware thread 2 at IRQL 2, serialized with USB completion DPCs.
// Keep this routine nonblocking; XAM binding belongs in the service thread
// before raising IRQL.
void ProteusMaintenance(uint32_t nowMilliseconds);

// Implemented by main.cpp; these keep XAM ownership out of the USB transport.
void ProteusPublishState(uint8_t interfaceNumber, const TritonProtocol::ControllerState& state);
void ProteusDisconnectController(uint8_t interfaceNumber);
