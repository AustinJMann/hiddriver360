#include <xtl.h>
#include <xkelib.h>
#include <stdlib.h>
#include <string.h>

#include "proteus.h"
#include "triton_protocol.h"
#include "usb_descriptors.h"

typedef usb_endpoint_descriptor* (*EndpointDescriptorFn)(deviceHandle*, int, int, int);
typedef int (*AddCompleteFn)(deviceHandle*, int);
typedef int (*QueueTransferFn)(deviceHandle*, void*);
typedef NTSTATUS (*OpenDefaultEndpointFn)(deviceHandle*, DWORD*);
typedef NTSTATUS (*OpenEndpointFn)(deviceHandle*, int, int, int, int, DWORD*);
typedef NTSTATUS (*RemoveCompleteFn)(deviceHandle*);

extern EndpointDescriptorFn UsbdGetEndpointDescriptor;
extern AddCompleteFn UsbdAddDeviceComplete;
extern QueueTransferFn UsbdQueueAsyncTransfer;
extern OpenDefaultEndpointFn UsbdOpenDefaultEndpoint;
extern OpenEndpointFn UsbdOpenEndpoint;
extern RemoveCompleteFn UsbdRemoveDeviceComplete;

namespace {

static const int kSlotCount = 4;
static const uint32_t kHeartbeatIntervalMs = 2000;
static const uint32_t kHeartbeatRetryMs = 250;
static const uint32_t kInputRetryMs = 50;
static const uint32_t kRemovalGraceMs = 1000;
static const uint16_t kMaxHidPacketSize = 64;
static const uint16_t kConfigurationDescriptorBufferSize = 512;

enum ControlPurpose {
	kControlNone,
	kControlGetConfigurationDescriptor,
	kControlSetConfiguration,
	kControlLizardOff
};

struct ProteusSlot {
	deviceHandle* handle;
	ProteusControllerExtension* extension;
	uint8_t interfaceNumber;
	uint8_t* inputBuffer;
	uint16_t inputLength;
	uint8_t featureReport[TritonProtocol::kFeatureReportSize];
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

static ProteusSlot g_slots[kSlotCount];
static bool g_configurationBusy;
static bool g_configured;
static bool g_configurationDescriptorFetched;
static volatile LONG g_controlOwnerSlot = -1;
static int g_nextHeartbeatSlot;
static uint8_t g_configurationDescriptor[kConfigurationDescriptorBufferSize];
static usb_endpoint_descriptor g_slotEndpointDescriptors[kSlotCount];

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

static int32_t QueueInput(ProteusSlot* slot);
static int32_t InputComplete(DWORD trbAddress, int32_t status);
static int32_t ControlComplete(DWORD trbAddress, int32_t status);
static void StartNextConfiguration();

static void UpdateRemovalReady(ProteusSlot* slot) {
	if (!slot || !slot->removing || !slot->removeCompleteCalled)
		return;
	if (!slot->cleanupReady) {
		slot->cleanupDeadline = GetTickCount() + kRemovalGraceMs;
		MemoryBarrier();
		slot->cleanupReady = true;
	}
}

static void FinalizeRemoval(ProteusSlot* slot) {
	if (!slot || !slot->cleanupReady)
		return;
	uint8_t interfaceNumber = slot->interfaceNumber;
	// The Xbox USB stack may retain the closed TRB after delivering its cancel
	// callback. Remove live lookup keys, but quarantine the small allocation for
	// the remainder of this driver session instead of risking a use-after-free.
	memset(slot, 0, sizeof(*slot));
	MemoryBarrier();
	DbgPrint("TritonDriver: Proteus interface %d removal complete; USB storage quarantined\n",
		interfaceNumber);
	bool anySlotsRemain = false;
	for (int i = 0; i < kSlotCount; ++i)
		if (g_slots[i].handle) anySlotsRemain = true;
	if (!anySlotsRemain) {
		g_configurationBusy = false;
		g_configured = false;
		g_configurationDescriptorFetched = false;
		InterlockedExchange(&g_controlOwnerSlot, -1);
		g_nextHeartbeatSlot = 0;
		memset(g_configurationDescriptor, 0, sizeof(g_configurationDescriptor));
		memset(g_slotEndpointDescriptors, 0, sizeof(g_slotEndpointDescriptors));
	}
}

static bool QueueControl(ProteusSlot* slot, ControlPurpose purpose,
	uint8_t requestType, uint8_t request, uint16_t value,
	uint16_t index, uint16_t length, void* data) {
	if (!slot || slot->removing || slot->controlBusy) return false;
	int slotIndex = (int)(slot - g_slots);
	if (slotIndex < 0 || slotIndex >= kSlotCount) return false;
	// All interface handles share the puck's physical endpoint zero. Allow only
	// one configuration/feature transfer across the entire puck at a time.
	if (InterlockedCompareExchange(&g_controlOwnerSlot, slotIndex, -1) != -1)
		return false;
	UsbControlTrb* control = &slot->extension->controlTrb;
	control->packet.bmRequestType = requestType;
	control->packet.bRequest = request;
	control->packet.wValue = Swap16(value);
	control->packet.wIndex = Swap16(index);
	control->packet.wLength = Swap16(length);
	control->trb.buffer = data;
	control->trb.length = length;
	slot->controlBusy = true;
	slot->controlPurpose = purpose;
	// This API returns an opaque queue token, which may have its high bit set;
	// completion status is delivered only through ControlComplete.
	int queueToken = UsbdQueueAsyncTransfer(slot->handle, control);
	if (!slot->loggedControlQueueResult) {
		slot->loggedControlQueueResult = true;
		DbgPrint("TritonDriver: Proteus slot %d control transfer queued token %x request %02x value %04x\n",
			slot->interfaceNumber, queueToken, request, value);
	}
	return true;
}

static bool QueueLizardOff(ProteusSlot* slot) {
	TritonProtocol::BuildLizardOffFeatureReport(slot->featureReport);
	return QueueControl(slot, kControlLizardOff, 0x21, 0x09, 0x0301, slot->interfaceNumber,
		TritonProtocol::kFeatureReportSize, slot->featureReport);
}

static ProteusSlot* FindSlotByInterruptTrb(void* trb) {
	for (int i = 0; i < kSlotCount; ++i)
		if (g_slots[i].extension && &g_slots[i].extension->interruptTrb == trb)
			return &g_slots[i];
	return 0;
}

static ProteusSlot* FindSlotByControlTrb(void* trb) {
	for (int i = 0; i < kSlotCount; ++i)
		if (g_slots[i].extension && &g_slots[i].extension->controlTrb == trb)
			return &g_slots[i];
	return 0;
}

static void ParseConfigurationDescriptor() {
	memset(g_slotEndpointDescriptors, 0, sizeof(g_slotEndpointDescriptors));
	for (int i = 0; i < kSlotCount; ++i) {
		uint8_t interfaceNumber = (uint8_t)(TritonProtocol::kFirstSlotInterface + i);
		usb_endpoint_descriptor* endpoint = &g_slotEndpointDescriptors[i];
		if (UsbDescriptors::FindInterruptInEndpoint(g_configurationDescriptor,
			sizeof(g_configurationDescriptor), interfaceNumber, endpoint)) {
			DbgPrint("TritonDriver: Proteus configuration maps interface %d to endpoint %02x size %d interval %d\n",
				interfaceNumber, endpoint->bEndpointAddress,
				TritonProtocol::ReadLE16((const uint8_t*)&endpoint->wMaxPacketSize),
				endpoint->bInterval);
		}
	}
}

static bool StartListening(ProteusSlot* slot) {
	int slotIndex = slot->interfaceNumber - TritonProtocol::kFirstSlotInterface;
	if (slotIndex >= 0 && slotIndex < kSlotCount && g_slotEndpointDescriptors[slotIndex].bLength)
		memcpy(&slot->endpointDescriptor, &g_slotEndpointDescriptors[slotIndex], sizeof(slot->endpointDescriptor));
	usb_endpoint_descriptor* endpoint = &slot->endpointDescriptor;
	if (endpoint->bLength == 0) {
		usb_endpoint_descriptor* indexed = UsbdGetEndpointDescriptor(
			slot->handle, slot->interfaceNumber, 3, 1);
		if (indexed) {
			memcpy(&slot->endpointDescriptor, indexed, UsbDescriptors::kEndpointDescriptorSize);
			DbgPrint("TritonDriver: Proteus interface %d indexed endpoint %02x descriptor %p\n",
				slot->interfaceNumber, indexed->bEndpointAddress, indexed);
		} else {
			usb_endpoint_descriptor* fallback = UsbdGetEndpointDescriptor(slot->handle, 0, 3, 1);
			if (fallback) memcpy(&slot->endpointDescriptor, fallback, UsbDescriptors::kEndpointDescriptorSize);
			DbgPrint("TritonDriver: Proteus interface %d indexed lookup failed; fallback endpoint %02x\n",
				slot->interfaceNumber, fallback ? fallback->bEndpointAddress : 0);
		}
	}
	if (!UsbDescriptors::IsInterruptInEndpoint(*endpoint)) {
		DbgPrint("TritonDriver: Proteus interface %d has no interrupt-IN endpoint\n", slot->interfaceNumber);
		return false;
	}
	uint16_t packetSize = Swap16(endpoint->wMaxPacketSize) & 0x7ff;
	if (packetSize == 0 || packetSize > kMaxHidPacketSize) {
		DbgPrint("TritonDriver: Proteus interface %d invalid packet size %d\n", slot->interfaceNumber, packetSize);
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
	slot->listening = true;
	DbgPrint("TritonDriver: Proteus slot interface %d listening endpoint %02x size %d interval %d\n",
		slot->interfaceNumber, endpoint->bEndpointAddress, packetSize, endpoint->bInterval);
	QueueInput(slot);
	// Do not probe empty bond slots on the puck-wide control endpoint. Wireless
	// activity below enables raw mode for the specific slot that needs it.
	slot->heartbeatEnabled = false;
	slot->heartbeatDeadline = 0;
	return true;
}

static void StartNextConfiguration() {
	if (g_configurationBusy) return;
	if (!g_configurationDescriptorFetched) {
		for (int i = 0; i < kSlotCount; ++i) {
			ProteusSlot* slot = &g_slots[i];
			if (slot->handle && slot->configurationPending && !slot->removing) {
				memset(g_configurationDescriptor, 0, sizeof(g_configurationDescriptor));
				if (QueueControl(slot, kControlGetConfigurationDescriptor,
					0x80, 0x06, 0x0200, 0, kConfigurationDescriptorBufferSize,
					g_configurationDescriptor))
					g_configurationBusy = true;
				return;
			}
		}
	}
	if (g_configured) {
		for (int i = 0; i < kSlotCount; ++i) {
			ProteusSlot* slot = &g_slots[i];
			if (slot->handle && slot->configurationPending && !slot->removing) {
				slot->configurationPending = false;
				if (!StartListening(slot))
					DbgPrint("TritonDriver: Proteus slot %d endpoint initialization failed\n", slot->interfaceNumber);
			}
		}
		return;
	}
	for (int i = 0; i < kSlotCount; ++i) {
		ProteusSlot* slot = &g_slots[i];
		if (slot->handle && slot->configurationPending && !slot->removing) {
			if (QueueControl(slot, kControlSetConfiguration, 0x00, 0x09, 1, 0, 0, 0)) {
				slot->configurationPending = false;
				g_configurationBusy = true;
			}
			return;
		}
	}
}

static int32_t ControlComplete(DWORD trbAddress, int32_t status) {
	ProteusSlot* slot = FindSlotByControlTrb((void*)trbAddress);
	if (!slot) return status;
	int slotIndex = (int)(slot - g_slots);
	InterlockedCompareExchange(&g_controlOwnerSlot, -1, slotIndex);
	ControlPurpose purpose = slot->controlPurpose;
	slot->controlPurpose = kControlNone;
	if (slot->removing) {
		slot->controlBusy = false;
		UpdateRemovalReady(slot);
		return status;
	}
	if (purpose == kControlGetConfigurationDescriptor) {
		g_configurationBusy = false;
		if (status == 0) {
			ParseConfigurationDescriptor();
		} else {
			DbgPrint("TritonDriver: Proteus configuration descriptor request failed: %x\n", status);
		}
		g_configurationDescriptorFetched = true;
		slot->controlBusy = false;
		StartNextConfiguration();
		return status;
	}
	if (purpose == kControlSetConfiguration) {
		g_configurationBusy = false;
		if (status == 0) {
			g_configured = true;
			if (!StartListening(slot))
				DbgPrint("TritonDriver: Proteus slot %d endpoint initialization failed\n", slot->interfaceNumber);
		} else {
			DbgPrint("TritonDriver: Proteus slot %d configuration failed: %x\n", slot->interfaceNumber, status);
			slot->configurationPending = true;
		}
		slot->controlBusy = false;
		StartNextConfiguration();
		return status;
	}
	if (purpose != kControlLizardOff) {
		slot->controlBusy = false;
		return status;
	}
	if (status == 0) {
		if (!slot->loggedLizardSuccess) {
			slot->loggedLizardSuccess = true;
			DbgPrint("TritonDriver: Proteus slot %d lizard-off request completed successfully\n",
				slot->interfaceNumber);
		}
		slot->retryDelay = kHeartbeatRetryMs;
		slot->featureFailureCount = 0;
		slot->heartbeatEnabled = true;
		slot->heartbeatDeadline = GetTickCount() + kHeartbeatIntervalMs;
	} else {
		++slot->featureFailureCount;
		if (slot->featureFailureCount <= 3 || slot->connected)
			DbgPrint("TritonDriver: Proteus slot %d lizard-off request failed: %x\n", slot->interfaceNumber, status);
		if (!slot->connected && slot->featureFailureCount >= 3) {
			slot->heartbeatEnabled = false;
			DbgPrint("TritonDriver: Proteus slot %d empty; pausing lizard probes until wireless activity\n",
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
	return status;
}

static int32_t QueueInput(ProteusSlot* slot) {
	if (!slot || slot->removing || !slot->listening) return 0;
	// The interrupt callback and maintenance thread may both try to recover the
	// input pipe. Claim the single reusable TRB before touching or queueing it.
	if (InterlockedCompareExchange(&slot->inputPending, 1, 0) != 0) return 0;
	slot->inputRetryDeadline = 0;
	memset(slot->inputBuffer, 0, slot->inputLength);
	UsbTrb* trb = &slot->extension->interruptTrb;
	// As with control transfers, the return value is an opaque queue token.
	// Keep ownership until InputComplete releases it.
	int queueToken = UsbdQueueAsyncTransfer(slot->handle, trb);
	if (!slot->loggedInputQueueResult) {
		slot->loggedInputQueueResult = true;
		DbgPrint("TritonDriver: Proteus slot %d input transfer queued token %x endpoint %02x\n",
			slot->interfaceNumber, queueToken, slot->endpointDescriptor.bEndpointAddress);
	}
	return queueToken;
}

static int32_t InputComplete(DWORD trbAddress, int32_t status) {
	ProteusSlot* slot = FindSlotByInterruptTrb((void*)trbAddress);
	if (!slot) return status;
	if (InterlockedCompareExchange(&slot->inputPending, 0, 1) != 1) {
		DbgPrint("TritonDriver: Proteus slot %d unexpected input completion with no transfer pending\n",
			slot->interfaceNumber);
		return status;
	}
	if (slot->removing) {
		UpdateRemovalReady(slot);
		return status;
	}
	if (!slot->loggedInputCompletion) {
		slot->loggedInputCompletion = true;
		DbgPrint("TritonDriver: Proteus slot %d first input completion status %x\n",
			slot->interfaceNumber, status);
	}
	if (status != 0) {
		++slot->inputErrorCount;
		if (slot->inputErrorCount <= 3 ||
			(slot->inputErrorCount & (slot->inputErrorCount - 1)) == 0)
			DbgPrint("TritonDriver: Proteus slot %d input error %x count %d; retrying with backoff\n",
				slot->interfaceNumber, status, slot->inputErrorCount);
		if (slot->connected) ProteusDisconnectController(slot->interfaceNumber);
		slot->connected = false;
		slot->inputRetryDeadline = GetTickCount() + kInputRetryMs;
		return status;
	}
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
		DbgPrint("TritonDriver: Proteus slot %d first interrupt report id %02x\n",
			slot->interfaceNumber, slot->inputBuffer[0]);
	}
	if (TritonProtocol::DecodeInputPrefix(slot->inputBuffer, slot->inputLength, &input)) {
		if (!slot->loggedFirstState) {
			slot->loggedFirstState = true;
			DbgPrint("TritonDriver: Proteus slot %d accepted state report %02x\n",
				slot->interfaceNumber, input.reportId);
		}
		TritonProtocol::ControllerState report;
		TritonProtocol::ConvertToControllerState(input, &report);
		bool wasConnected = slot->connected;
		slot->connected = true;
		slot->heartbeatEnabled = true;
		if (!wasConnected)
			slot->heartbeatDeadline = 0;
		ProteusPublishState(slot->interfaceNumber, report);
	} else if (TritonProtocol::DecodeWirelessStatus(slot->inputBuffer, slot->inputLength, &wireless)) {
		if (wireless == TritonProtocol::kWirelessDisconnected) {
			slot->connected = false;
			slot->heartbeatEnabled = false;
			ProteusDisconnectController(slot->interfaceNumber);
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
	slot->retryDelay = kHeartbeatRetryMs;
	slot->extension = new ProteusControllerExtension();
	if (!slot->extension) { memset(slot, 0, sizeof(*slot)); return -1; }
	memset(slot->extension, 0, sizeof(*slot->extension));
	slot->extension->deviceHandle = handle;
	slot->extension->interfaceNumber = descriptor->bInterfaceNumber;
	slot->extension->deviceType = 1;
	handle->driver = slot->extension;
	UsbdAddDeviceComplete(handle, 0);
	NTSTATUS result = UsbdOpenDefaultEndpoint(handle, (DWORD*)&slot->extension->controlTrb);
	if (NT_ERROR(result)) {
		delete slot->extension;
		handle->driver = 0;
		memset(slot, 0, sizeof(*slot));
		return result;
	}
	UsbTrb* controlTrb = &slot->extension->controlTrb.trb;
	controlTrb->flags = 1;
	controlTrb->callback = (DWORD)ControlComplete;
	// As with the interrupt TRB, keep the endpoint saved by the open call stable
	// for every later configuration and feature transfer.
	controlTrb->savedEndpoint = controlTrb->endpoint;
	DbgPrint("TritonDriver: Proteus slot interface %d initializing\n", slot->interfaceNumber);
	slot->configurationPending = true;
	StartNextConfiguration();
	return 0;
}

bool ProteusRemoveSlotInterface(deviceHandle* handle) {
	ProteusSlot* slot = FindSlotByHandle(handle);
	if (!slot) return false;
	if (slot->removing) return true;
	slot->removing = true;
	DbgPrint("TritonDriver: Proteus interface %d removal begin input %d control %d\n",
		slot->interfaceNumber, slot->inputPending, slot->controlBusy);
	ProteusDisconnectController(slot->interfaceNumber);
	if (slot->controlPurpose == kControlGetConfigurationDescriptor ||
		slot->controlPurpose == kControlSetConfiguration)
		g_configurationBusy = false;
	int slotIndex = (int)(slot - g_slots);
	InterlockedCompareExchange(&g_controlOwnerSlot, -1, slotIndex);
	// Explicit endpoint closes can block during physical composite-device
	// removal. Let the USB core cancel the pipes as part of remove completion;
	// all TRB storage remains quarantined, so late callbacks stay memory-safe.
	DbgPrint("TritonDriver: Proteus interface %d calling kernel removal complete\n",
		slot->interfaceNumber);
	UsbdRemoveDeviceComplete(handle);
	DbgPrint("TritonDriver: Proteus interface %d kernel removal returned\n",
		slot->interfaceNumber);
	slot->removeCompleteCalled = true;
	UpdateRemovalReady(slot);
	bool anySlotsRemain = false;
	for (int i = 0; i < kSlotCount; ++i)
		if (g_slots[i].handle && !g_slots[i].removing) anySlotsRemain = true;
	if (anySlotsRemain) StartNextConfiguration();
	return true;
}

void ProteusMaintenance(uint32_t nowMilliseconds) {
	for (int i = 0; i < kSlotCount; ++i) {
		ProteusSlot* slot = &g_slots[i];
		if (slot->removing) {
			if (!slot->loggedRemovalWait) {
			slot->loggedRemovalWait = true;
			DbgPrint("TritonDriver: Proteus interface %d waiting for removal transfers input %d control %d\n",
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
	}
	for (int offset = 0; offset < kSlotCount; ++offset) {
		int i = (g_nextHeartbeatSlot + offset) % kSlotCount;
		ProteusSlot* slot = &g_slots[i];
		if (!slot->handle || slot->removing || !slot->listening ||
			!slot->heartbeatEnabled || slot->controlBusy) continue;
		if ((int32_t)(nowMilliseconds - slot->heartbeatDeadline) >= 0 &&
			QueueLizardOff(slot)) {
			g_nextHeartbeatSlot = (i + 1) % kSlotCount;
			break;
		}
	}
}
