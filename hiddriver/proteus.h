#pragma once

#include <stdint.h>

#include "driver_types.h"

bool ProteusIsSlot(uint16_t vendorId, uint16_t productId,
	const usb_interface_descriptor* interfaceDescriptor);
int ProteusAddSlotInterface(deviceHandle* handle,
	const usb_interface_descriptor* interfaceDescriptor);
bool ProteusRemoveSlotInterface(deviceHandle* handle);
void ProteusMaintenance(uint32_t nowMilliseconds);

// Implemented by main.cpp; these keep XAM ownership out of the USB transport.
void ProteusPublishState(uint8_t interfaceNumber, const ButtonsReport& state);
void ProteusDisconnectController(uint8_t interfaceNumber);
