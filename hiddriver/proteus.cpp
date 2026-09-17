#include <xtl.h>
#include <xkelib.h>
#include <stdlib.h>
#include <string.h>

#include "proteus.h"
#include "triton_protocol.h"

typedef usb_endpoint_descriptor* (*EndpointDescriptorFn)(deviceHandle*, int, int, int);
typedef int (*AddCompleteFn)(deviceHandle*, int);
typedef int (*QueueTransferFn)(deviceHandle*, void*);
typedef NTSTATUS (*OpenDefaultEndpointFn)(deviceHandle*, DWORD*);
typedef NTSTATUS (*OpenEndpointFn)(deviceHandle*, int, int, int, int, DWORD*);
typedef NTSTATUS (*CloseEndpointFn)(deviceHandle*, void*);
typedef NTSTATUS (*CloseDefaultEndpointFn)(deviceHandle*, DWORD*);
typedef NTSTATUS (*RemoveCompleteFn)(deviceHandle*);

extern EndpointDescriptorFn UsbdGetEndpointDescriptor;
extern AddCompleteFn UsbdAddDeviceComplete;
extern QueueTransferFn UsbdQueueAsyncTransfer;
extern OpenDefaultEndpointFn UsbdOpenDefaultEndpoint;
extern OpenEndpointFn UsbdOpenEndpoint;
extern CloseEndpointFn UsbdQueueCloseEndpoint;
extern CloseDefaultEndpointFn UsbdQueueCloseDefaultEndpoint;
extern RemoveCompleteFn UsbdRemoveDeviceComplete;

namespace {

static const int kSlotCount = 4;
static const uint32_t kHeartbeatIntervalMs = 2000;
static const uint32_t kHeartbeatRetryMs = 250;
static const uint16_t kMaxHidPacketSize = 64;

struct ProteusSlot {
	deviceHandle* handle;
	HidControllerExtension* extension;
	uint8_t interfaceNumber;
	uint8_t* inputBuffer;
	uint16_t inputLength;
	uint8_t featureReport[TritonProtocol::kFeatureReportSize];
	usb_endpoint_descriptor endpointDescriptor;
	uint32_t heartbeatDeadline;
	uint32_t retryDelay;
	bool listening;
	bool connected;
	bool controlBusy;
	bool configurationPending;
	bool loggedFirstReport;
	bool loggedFirstState;
	bool loggedInputQueueResult;
	bool loggedControlQueueResult;
	bool removing;
};

static ProteusSlot g_slots[kSlotCount];
static bool g_configurationBusy;
static bool g_configured;

static uint16_t Swap16(uint16_t value) {
	return (uint16_t)((value >> 8) | (value << 8));
}

static ProteusSlot* FindSlotByInterface(uint8_t interfaceNumber) {
	if (interfaceNumber < TritonProtocol::kFirstSlotInterface ||
		interfaceNumber > TritonProtocol::kLastSlotInterface)
		return 0;
	return &g_slots[interfaceNumber - TritonProtocol::kFirstSlotInterface];
}

static ProteusSlot* FindSlotByHandle(deviceHandle* handle) {
	for (int i = 0; i < kSlotCount; ++i)
		if (g_slots[i].handle == handle) return &g_slots[i];
	return 0;
}

static void QueueInput(ProteusSlot* slot);
static int32_t InputComplete(DWORD trbAddress, int32_t status);
static int32_t ControlComplete(DWORD trbAddress, int32_t status);
static void StartNextConfiguration();

static void QueueControl(ProteusSlot* slot, uint8_t requestType, uint8_t request,
	uint16_t value, uint16_t index, uint16_t length, void* data) {
	if (!slot || slot->removing || slot->controlBusy) return;
	UsbControlTrb* control = &slot->extension->controlTrb;
	control->packet.bmRequestType = requestType;
	control->packet.bRequest = request;
	control->packet.wValue = Swap16(value);
	control->packet.wIndex = Swap16(index);
	control->packet.wLength = Swap16(length);
	control->trb.buffer = data;
	control->trb.length = length;
	control->trb.flags = 1;
	control->trb.callback = (DWORD)ControlComplete;
	control->trb.savedEndpoint = control->trb.endpoint;
	slot->controlBusy = true;
	int queueToken = UsbdQueueAsyncTransfer(slot->handle, control);
	if (!slot->loggedControlQueueResult) {
		slot->loggedControlQueueResult = true;
		DbgPrint("EINTIM: Proteus slot %d control transfer queued token %x request %02x value %04x\n",
			slot->interfaceNumber, queueToken, request, value);
	}
}

static void QueueLizardOff(ProteusSlot* slot) {
	TritonProtocol::BuildLizardOffFeatureReport(slot->featureReport);
	QueueControl(slot, 0x21, 0x09, 0x0301, slot->interfaceNumber,
		TritonProtocol::kFeatureReportSize, slot->featureReport);
}

static usb_endpoint_descriptor* ScanInterfaceInterruptInEndpoint(
	const usb_interface_descriptor* interfaceDescriptor, usb_endpoint_descriptor* copy) {
	if (!interfaceDescriptor || !copy) return 0;
	const uint8_t* cursor = (const uint8_t*)interfaceDescriptor + interfaceDescriptor->bLength;
	const uint8_t* limit = cursor + 96;
	while (cursor + 2 <= limit) {
		uint8_t length = cursor[0];
		uint8_t type = cursor[1];
		if (length < 2 || cursor + length > limit) break;
		if (type == 0x04) break;
		if (type == 0x05 && length >= sizeof(usb_endpoint_descriptor)) {
			const usb_endpoint_descriptor* endpoint = (const usb_endpoint_descriptor*)cursor;
			if ((endpoint->bEndpointAddress & 0x80) && (endpoint->bmAttributes & 0x03) == 0x03) {
				memcpy(copy, endpoint, sizeof(*copy));
				return copy;
			}
		}
		cursor += length;
	}
	return 0;
}

static bool StartListening(ProteusSlot* slot) {
	usb_endpoint_descriptor* endpoint = &slot->endpointDescriptor;
	if (endpoint->bLength == 0) {
		usb_endpoint_descriptor* indexed = UsbdGetEndpointDescriptor(
			slot->handle, slot->interfaceNumber, 3, 1);
		if (indexed) {
			memcpy(&slot->endpointDescriptor, indexed, sizeof(slot->endpointDescriptor));
			DbgPrint("EINTIM: Proteus interface %d indexed endpoint %02x descriptor %p\n",
				slot->interfaceNumber, indexed->bEndpointAddress, indexed);
		} else {
			usb_endpoint_descriptor* fallback = UsbdGetEndpointDescriptor(slot->handle, 0, 3, 1);
			if (fallback) memcpy(&slot->endpointDescriptor, fallback, sizeof(slot->endpointDescriptor));
			DbgPrint("EINTIM: Proteus interface %d indexed lookup failed; fallback endpoint %02x\n",
				slot->interfaceNumber, fallback ? fallback->bEndpointAddress : 0);
		}
	}
	endpoint = &slot->endpointDescriptor;
	if (!endpoint) {
		DbgPrint("EINTIM: Proteus interface %d has no interrupt-IN endpoint\n", slot->interfaceNumber);
		return false;
	}
	uint16_t packetSize = Swap16(endpoint->wMaxPacketSize) & 0x7ff;
	if (packetSize == 0 || packetSize > kMaxHidPacketSize) {
		DbgPrint("EINTIM: Proteus interface %d invalid packet size %d\n", slot->interfaceNumber, packetSize);
		return false;
	}
	NTSTATUS result = UsbdOpenEndpoint(slot->handle, 3, endpoint->bEndpointAddress,
		packetSize, endpoint->bInterval, (DWORD*)&slot->extension->interruptTrb);
	if (NT_ERROR(result)) return false;
	slot->inputBuffer = (uint8_t*)calloc(1, packetSize);
	if (!slot->inputBuffer) return false;
	slot->inputLength = packetSize;
	slot->listening = true;
	DbgPrint("EINTIM: Proteus slot interface %d listening endpoint %02x size %d interval %d\n",
		slot->interfaceNumber, endpoint->bEndpointAddress, packetSize, endpoint->bInterval);
	QueueInput(slot);
	slot->heartbeatDeadline = 0;
	return true;
}

static void StartNextConfiguration() {
	if (g_configurationBusy) return;
	if (g_configured) {
		for (int i = 0; i < kSlotCount; ++i) {
			ProteusSlot* slot = &g_slots[i];
			if (slot->handle && slot->configurationPending && !slot->removing) {
				slot->configurationPending = false;
				if (!StartListening(slot))
					DbgPrint("EINTIM: Proteus slot %d endpoint initialization failed\n", slot->interfaceNumber);
			}
		}
		return;
	}
	for (int i = 0; i < kSlotCount; ++i) {
		ProteusSlot* slot = &g_slots[i];
		if (slot->handle && slot->configurationPending && !slot->removing) {
			slot->configurationPending = false;
			g_configurationBusy = true;
			QueueControl(slot, 0x00, 0x09, 1, 0, 0, 0);
			return;
		}
	}
}

static int32_t ControlComplete(DWORD trbAddress, int32_t status) {
	HidControllerExtension* extension = ExtensionFromControlTrb((void*)trbAddress);
	ProteusSlot* slot = FindSlotByHandle(extension ? extension->deviceHandle : 0);
	if (!slot) return status;
	slot->controlBusy = false;
	if (slot->removing) return status;
	if (!slot->listening) {
		g_configurationBusy = false;
		if (status == 0) {
			g_configured = true;
			if (!StartListening(slot))
				DbgPrint("EINTIM: Proteus slot %d endpoint initialization failed\n", slot->interfaceNumber);
		} else {
			DbgPrint("EINTIM: Proteus slot %d configuration failed: %x\n", slot->interfaceNumber, status);
			slot->configurationPending = true;
		}
		StartNextConfiguration();
		return status;
	}
	if (status == 0) {
		slot->retryDelay = kHeartbeatRetryMs;
		slot->heartbeatDeadline = GetTickCount() + kHeartbeatIntervalMs;
	} else {
		DbgPrint("EINTIM: Proteus slot %d lizard-off request failed: %x\n", slot->interfaceNumber, status);
		slot->heartbeatDeadline = GetTickCount() + slot->retryDelay;
		if (slot->retryDelay < kHeartbeatIntervalMs) slot->retryDelay *= 2;
		if (slot->retryDelay > kHeartbeatIntervalMs) slot->retryDelay = kHeartbeatIntervalMs;
	}
	return status;
}

static void QueueInput(ProteusSlot* slot) {
	if (!slot || slot->removing || !slot->listening) return;
	memset(slot->inputBuffer, 0, slot->inputLength);
	UsbTrb* trb = &slot->extension->interruptTrb;
	trb->buffer = slot->inputBuffer;
	trb->length = slot->inputLength;
	trb->flags = 1;
	trb->callback = (DWORD)InputComplete;
	trb->savedEndpoint = trb->endpoint;
	int queueToken = UsbdQueueAsyncTransfer(slot->handle, trb);
	if (!slot->loggedInputQueueResult) {
		slot->loggedInputQueueResult = true;
		DbgPrint("EINTIM: Proteus slot %d input transfer queued token %x endpoint %02x\n",
			slot->interfaceNumber, queueToken, slot->endpointDescriptor.bEndpointAddress);
	}
}

static int32_t InputComplete(DWORD trbAddress, int32_t status) {
	HidControllerExtension* extension = ExtensionFromInterruptTrb((void*)trbAddress);
	ProteusSlot* slot = FindSlotByHandle(extension ? extension->deviceHandle : 0);
	if (!slot || slot->removing) return status;
	if (status != 0) {
		if (slot->connected) ProteusDisconnectController(slot->interfaceNumber);
		slot->connected = false;
		QueueInput(slot);
		return status;
	}

	TritonProtocol::InputState input;
	TritonProtocol::WirelessStatus wireless;
	if (!slot->loggedFirstReport) {
		slot->loggedFirstReport = true;
		DbgPrint("EINTIM: Proteus slot %d first interrupt report id %02x\n",
			slot->interfaceNumber, slot->inputBuffer[0]);
	}
	if (TritonProtocol::DecodeInputPrefix(slot->inputBuffer, slot->inputLength, &input)) {
		if (!slot->loggedFirstState) {
			slot->loggedFirstState = true;
			DbgPrint("EINTIM: Proteus slot %d accepted state report %02x\n",
				slot->interfaceNumber, input.reportId);
		}
		ButtonsReport report;
		TritonProtocol::ConvertToButtonsReport(input, &report);
		slot->connected = true;
		ProteusPublishState(slot->interfaceNumber, report);
	} else if (TritonProtocol::DecodeWirelessStatus(slot->inputBuffer, slot->inputLength, &wireless)) {
		if (wireless == TritonProtocol::kWirelessDisconnected) {
			slot->connected = false;
			ProteusDisconnectController(slot->interfaceNumber);
		} else {
			slot->connected = true;
			slot->heartbeatDeadline = 0;
		}
	}
	QueueInput(slot);
	return status;
}

} // namespace

bool ProteusIsSlot(uint16_t vendorId, uint16_t productId,
	const usb_interface_descriptor* descriptor) {
	return descriptor && TritonProtocol::IsProteusSlotInterface(vendorId, productId,
		descriptor->bInterfaceNumber, descriptor->bInterfaceClass,
		descriptor->bInterfaceSubClass, descriptor->bInterfaceProtocol);
}

int ProteusAddSlotInterface(deviceHandle* handle,
	const usb_interface_descriptor* descriptor) {
	ProteusSlot* slot = FindSlotByInterface(descriptor->bInterfaceNumber);
	if (!slot || slot->handle) return 0;
	memset(slot, 0, sizeof(*slot));
	slot->handle = handle;
	slot->interfaceNumber = descriptor->bInterfaceNumber;
	usb_endpoint_descriptor* interfaceEndpoint =
		ScanInterfaceInterruptInEndpoint(descriptor, &slot->endpointDescriptor);
	if (interfaceEndpoint) {
		DbgPrint("EINTIM: Proteus interface %d descriptor-local endpoint %02x\n",
			slot->interfaceNumber, interfaceEndpoint->bEndpointAddress);
	} else {
		DbgPrint("EINTIM: Proteus interface %d could not find descriptor-local endpoint; using kernel lookup\n",
			slot->interfaceNumber);
	}
	slot->retryDelay = kHeartbeatRetryMs;
	slot->extension = new HidControllerExtension();
	if (!slot->extension) { memset(slot, 0, sizeof(*slot)); return -1; }
	memset(slot->extension, 0, sizeof(*slot->extension));
	slot->extension->deviceHandle = handle;
	slot->extension->interfaceNumber = descriptor->bInterfaceNumber;
	slot->extension->deviceType = 1;
	handle->driver = slot->extension;
	UsbdAddDeviceComplete(handle, 0);
	NTSTATUS result = UsbdOpenDefaultEndpoint(handle, (DWORD*)&slot->extension->controlTrb);
	if (NT_ERROR(result)) return result;
	DbgPrint("EINTIM: Proteus slot interface %d initializing\n", slot->interfaceNumber);
	slot->configurationPending = true;
	StartNextConfiguration();
	return 0;
}

bool ProteusRemoveSlotInterface(deviceHandle* handle) {
	ProteusSlot* slot = FindSlotByHandle(handle);
	if (!slot) return false;
	slot->removing = true;
	if (slot->connected) ProteusDisconnectController(slot->interfaceNumber);
	if (slot->listening) UsbdQueueCloseEndpoint(handle, &slot->extension->interruptTrb);
	UsbdQueueCloseDefaultEndpoint(handle, (DWORD*)&slot->extension->controlTrb);
	free(slot->inputBuffer);
	slot->inputBuffer = 0;
	delete slot->extension;
	handle->driver = 0;
	UsbdRemoveDeviceComplete(handle);
	memset(slot, 0, sizeof(*slot));
	bool anySlotsRemain = false;
	for (int i = 0; i < kSlotCount; ++i)
		if (g_slots[i].handle) anySlotsRemain = true;
	if (!anySlotsRemain) {
		g_configurationBusy = false;
		g_configured = false;
	}
	return true;
}

void ProteusMaintenance(uint32_t nowMilliseconds) {
	for (int i = 0; i < kSlotCount; ++i) {
		ProteusSlot* slot = &g_slots[i];
		if (!slot->handle || slot->removing || !slot->listening || slot->controlBusy) continue;
		if ((int32_t)(nowMilliseconds - slot->heartbeatDeadline) >= 0)
			QueueLizardOff(slot);
	}
}
