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
	uint8_t pad[4];
	UsbPacket packet;
};

struct deviceHandle;
struct __declspec(align(2)) ProteusControllerExtension {
	deviceHandle* deviceHandle;
	UsbTrb interruptTrb;
	uint8_t interfaceNumber;
	uint8_t gap20[3];
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
	ProteusControllerExtension* driver;
};

static_assert(offsetof(ProteusControllerExtension, interruptTrb) == 0x04,
	"USB extension interrupt TRB offset changed");
static_assert(offsetof(ProteusControllerExtension, interfaceNumber) == 0x20,
	"USB extension interface offset changed");
static_assert(offsetof(ProteusControllerExtension, controlTrb) == 0x24,
	"USB extension control TRB offset changed");
static_assert(offsetof(ProteusControllerExtension, cleanupHandler) == 0x50,
	"USB extension cleanup handler offset changed");
static_assert(offsetof(ProteusControllerExtension, queue) == 0x6c,
	"USB extension queue offset changed");
static_assert(sizeof(ProteusControllerExtension) == 0x7c,
	"USB extension kernel-observed prefix size changed");
