#pragma once

#include <stddef.h>
#include <string.h>

#include "usb.h"

namespace UsbDescriptors {

static const size_t kEndpointDescriptorSize = 7;

inline bool IsInterruptInEndpoint(const usb_endpoint_descriptor& endpoint) {
	return endpoint.bLength >= kEndpointDescriptorSize &&
		endpoint.bDescriptorType == 0x05 &&
		(endpoint.bEndpointAddress & 0x80) != 0 &&
		(endpoint.bEndpointAddress & 0x0f) != 0 &&
		(endpoint.bmAttributes & 0x03) == 0x03;
}

// Parse only an owned configuration buffer, never bytes beyond an interface
// pointer returned by the USB stack. Leave the output unchanged on failure.
inline bool FindInterruptInEndpoint(const uint8_t* bytes, size_t length,
	uint8_t interfaceNumber, usb_endpoint_descriptor* output) {
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
			if (matchesInterface && !found && IsInterruptInEndpoint(endpoint)) {
				candidate = endpoint;
				found = true;
			}
		}
		offset += descriptorLength;
	}
	if (found) *output = candidate;
	return found;
}

} // namespace UsbDescriptors
