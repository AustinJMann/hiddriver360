#include <xtl.h>
#include <xkelib.h>
#include <string.h>

#include "Detours.h"
#include "driver_types.h"
#include "proteus.h"
#include "proteus_routing.h"
#include "triton_protocol.h"
#include "usb.h"

static const int kControllerCount = 4;
static const DWORD kDeviceContextBase = 0x10000005;
static const DWORD kGuideCooldownMs = 1000;

Detour g_hidAddDeviceDetour;
Detour g_hidRemoveDeviceDetour;
Detour g_xamInputSetStateDetour;
Detour g_xamInputGetCapabilitiesDetour;
Detour g_xinputReadStateDetour;

typedef struct _XINPUT_CAPABILITIESEX {
	BYTE Type;
	BYTE SubType;
	WORD Flags;
	XINPUT_GAMEPAD Gamepad;
	XINPUT_VIBRATION Vibration;
	DWORD reserved1;
	DWORD reserved2;
	DWORD reserved3;
} XINPUT_CAPABILITIES_EX, *PXINPUT_CAPABILITIES_EX;

typedef usb_device_descriptor* (*UsbDeviceDescriptorFn)(deviceHandle*);
typedef usb_interface_descriptor* (*UsbInterfaceDescriptorFn)(deviceHandle*);
typedef usb_endpoint_descriptor* (*EndpointDescriptorFn)(deviceHandle*, int, int, int);
typedef int (*AddCompleteFn)(deviceHandle*, int);
typedef int (*QueueTransferFn)(deviceHandle*, void*);
typedef NTSTATUS (*OpenDefaultEndpointFn)(deviceHandle*, DWORD*);
typedef NTSTATUS (*OpenEndpointFn)(deviceHandle*, int, int, int, int, DWORD*);
typedef NTSTATUS (*RemoveCompleteFn)(deviceHandle*);
typedef int (*XamBindDeviceFn)(unsigned int, unsigned int, unsigned __int8, bool, unsigned __int8*);
typedef int (*UsbNotificationFn)();
typedef void (*FreePhysicalMemoryFn)(DWORD, DWORD);

UsbDeviceDescriptorFn UsbdGetDeviceDescriptor = 0;
UsbInterfaceDescriptorFn UsbdGetInterfaceDescriptor = 0;
EndpointDescriptorFn UsbdGetEndpointDescriptor = 0;
AddCompleteFn UsbdAddDeviceComplete = 0;
OpenDefaultEndpointFn UsbdOpenDefaultEndpoint = 0;
OpenEndpointFn UsbdOpenEndpoint = 0;
QueueTransferFn UsbdQueueAsyncTransfer = 0;
RemoveCompleteFn UsbdRemoveDeviceComplete = 0;
XamBindDeviceFn XamUserBindDeviceCallback = 0;

UsbNotificationFn g_usbdPowerDownNotification = 0;
UsbNotificationFn g_usbdDriverEntry = 0;
FreePhysicalMemoryFn g_freePhysicalMemory = 0;
void* g_xamInputSetState = 0;
void* g_xamInputGetCapabilities = 0;
void* g_xinputReadState = 0;
bool g_isDevkit = true;
DWORD g_usbPhysicalPage = 0;

struct TritonVirtualController {
	volatile LONG inUse;
	uint8_t userIndex;
	uint8_t slotIndex;
	uint16_t reserved;
	uint32_t deviceContext;
	volatile LONG packetNumber;
	DWORD guideLastPressTime;
	uint32_t slotGeneration;
} __declspec(align(4));

struct ProteusRoutingSlot {
	TritonProtocol::ControllerState stateBuffers[2];
	volatile LONG publishedStateIndex;
	volatile LONG stateSequence;
	volatile LONG connected;
	volatile LONG disconnectPending;
	volatile LONG controllerIndex;
	volatile LONG generation;
};

TritonVirtualController g_controllers[kControllerCount];
ProteusRoutingSlot g_proteusSlots[ProteusRouting::kSlotCount];

uint16_t Swap16(uint16_t value) { return (uint16_t)((value >> 8) | (value << 8)); }

BOOL IsTrayOpen() {
	BYTE input[0x10] = { 0 }, output[0x10] = { 0 };
	input[0] = 0x0a;
	HalSendSMCMessage(input, output);
	return output[1] == 0x60;
}

HANDLE MakeSystemThread(LPTHREAD_START_ROUTINE entry, PVOID argument) {
	HANDLE thread = 0;
	ExCreateThread(&thread, 0, 0, XapiThreadStartup, entry, argument,
		EX_CREATE_FLAG_SUSPENDED | EX_CREATE_FLAG_SYSTEM | 0x18000424);
	if (!thread) return 0;
	XSetThreadProcessor(thread, 4);
	SetThreadPriority(thread, THREAD_PRIORITY_NORMAL);
	ResumeThread(thread);
	return thread;
}

void InitializeRouting() {
	memset(g_controllers, 0, sizeof(g_controllers));
	memset(g_proteusSlots, 0, sizeof(g_proteusSlots));
	for (int i = 0; i < ProteusRouting::kSlotCount; ++i) {
		g_proteusSlots[i].controllerIndex = ProteusRouting::kUnboundController;
		g_proteusSlots[i].generation = 1;
	}
}

int ReserveController() {
	for (int i = 0; i < kControllerCount; ++i) {
		if (InterlockedCompareExchange(&g_controllers[i].inUse, 1, 0) == 0) {
			memset((uint8_t*)&g_controllers[i] + sizeof(g_controllers[i].inUse), 0,
				sizeof(TritonVirtualController) - sizeof(g_controllers[i].inUse));
			g_controllers[i].userIndex = 0xff;
			g_controllers[i].slotIndex = 0xff;
			return i;
		}
	}
	return -1;
}

void ReleaseController(int index) {
	if (index < 0 || index >= kControllerCount) return;
	TritonVirtualController& controller = g_controllers[index];
	memset((uint8_t*)&controller + sizeof(controller.inUse), 0,
		sizeof(controller) - sizeof(controller.inUse));
	MemoryBarrier();
	InterlockedExchange(&controller.inUse, 0);
}

bool SnapshotState(const ProteusRoutingSlot& slot, TritonProtocol::ControllerState* state) {
	for (int attempt = 0; attempt < 4; ++attempt) {
		LONG before = slot.stateSequence;
		if (before & 1) continue;
		MemoryBarrier();
		LONG index = slot.publishedStateIndex;
		*state = slot.stateBuffers[index];
		MemoryBarrier();
		LONG after = slot.stateSequence;
		if (before == after && !(after & 1)) return true;
	}
	memset(state, 0, sizeof(*state));
	return false;
}

void UnbindController(int slotIndex) {
	ProteusRoutingSlot& slot = g_proteusSlots[slotIndex];
	int controllerIndex = slot.controllerIndex;
	if (controllerIndex < 0 || controllerIndex >= kControllerCount) return;
	TritonVirtualController& controller = g_controllers[controllerIndex];
	if (!controller.inUse || controller.slotIndex != slotIndex ||
		controller.slotGeneration != (uint32_t)slot.generation) {
		InterlockedExchange(&slot.controllerIndex, ProteusRouting::kUnboundController);
		InterlockedIncrement(&slot.generation);
		return;
	}
	XamUserBindDeviceCallback(0xa7553952 + controllerIndex,
		kDeviceContextBase + controllerIndex, 0, true, 0);
	InterlockedExchange(&slot.controllerIndex, ProteusRouting::kUnboundController);
	InterlockedIncrement(&slot.generation);
	DbgPrint("TritonDriver: interface %d unbound from controller %d, XAM user %d\n",
		slotIndex + TritonProtocol::kFirstSlotInterface, controllerIndex, controller.userIndex);
	ReleaseController(controllerIndex);
}

bool BindController(int slotIndex) {
	ProteusRoutingSlot& slot = g_proteusSlots[slotIndex];
	if (!slot.connected || slot.controllerIndex >= 0) return false;
	int controllerIndex = ReserveController();
	if (controllerIndex < 0) return false;
	TritonVirtualController& controller = g_controllers[controllerIndex];
	controller.deviceContext = kDeviceContextBase + controllerIndex;
	controller.slotIndex = (uint8_t)slotIndex;
	controller.slotGeneration = (uint32_t)InterlockedIncrement(&slot.generation);
	InterlockedExchange(&slot.controllerIndex, controllerIndex);
	MemoryBarrier();
	uint8_t userIndex = 0xff;
	int result = XamUserBindDeviceCallback(0xa7553952 + controllerIndex,
		controller.deviceContext, 0, false, &userIndex);
	if (!ProteusRouting::IsValidXamBinding(result, userIndex)) {
		DbgPrint("TritonDriver: interface %d bind failed for controller %d: %x user %d\n",
			slotIndex + TritonProtocol::kFirstSlotInterface, controllerIndex, result, userIndex);
		if (result == 0) XamUserBindDeviceCallback(0xa7553952 + controllerIndex,
			controller.deviceContext, 0, true, 0);
		InterlockedExchange(&slot.controllerIndex, ProteusRouting::kUnboundController);
		InterlockedIncrement(&slot.generation);
		ReleaseController(controllerIndex);
		return false;
	}
	controller.userIndex = userIndex;
	DbgPrint("TritonDriver: interface %d bound to controller %d, XAM user %d\n",
		slotIndex + TritonProtocol::kFirstSlotInterface, controllerIndex, userIndex);
	return true;
}

void ProcessProteusEvents() {
	for (int i = 0; i < ProteusRouting::kSlotCount; ++i) {
		LONG pending = InterlockedExchange(&g_proteusSlots[i].disconnectPending, 0);
		if ((pending || !g_proteusSlots[i].connected) && g_proteusSlots[i].controllerIndex >= 0)
			UnbindController(i);
	}
	for (int i = 0; i < ProteusRouting::kSlotCount; ++i)
		if (g_proteusSlots[i].connected && g_proteusSlots[i].controllerIndex < 0)
			BindController(i);
}

unsigned int __stdcall ProteusServiceThreadProc(void*) {
	for (;;) {
		ProcessProteusEvents();
		ProteusMaintenance(GetTickCount());
		Sleep(100);
	}
}

TritonVirtualController* FindControllerByUser(DWORD user) {
	for (int i = 0; i < kControllerCount; ++i)
		if (g_controllers[i].inUse && g_controllers[i].userIndex == user) return &g_controllers[i];
	return 0;
}

TritonVirtualController* FindControllerByContext(DWORD context, int* index) {
	for (int i = 0; i < kControllerCount; ++i) {
		if (g_controllers[i].inUse && g_controllers[i].deviceContext == context) {
			if (index) *index = i;
			return &g_controllers[i];
		}
	}
	return 0;
}

int HidRemoveDeviceHook(deviceHandle* handle) {
	if (ProteusRemoveSlotInterface(handle)) return 0;
	return g_hidRemoveDeviceDetour.GetOriginal<decltype(&HidRemoveDeviceHook)>()(handle);
}

int HidAddDeviceHook(deviceHandle* handle) {
	usb_device_descriptor* device = UsbdGetDeviceDescriptor(handle);
	usb_interface_descriptor* interfaceDescriptor = UsbdGetInterfaceDescriptor(handle);
	if (!device || !interfaceDescriptor)
		return g_hidAddDeviceDetour.GetOriginal<decltype(&HidAddDeviceHook)>()(handle);
	uint16_t vendorId = Swap16(device->idVendor);
	uint16_t productId = Swap16(device->idProduct);
	if (ProteusIsSlot(vendorId, productId, interfaceDescriptor))
		return ProteusAddSlotInterface(handle, interfaceDescriptor);
	return g_hidAddDeviceDetour.GetOriginal<decltype(&HidAddDeviceHook)>()(handle);
}

DWORD XamInputSetStateHook(DWORD user, DWORD flags, XINPUT_STATE* state,
	BYTE amplitude, BYTE frequency, BYTE offset) {
	DWORD status = g_xamInputSetStateDetour.GetOriginal<decltype(&XamInputSetStateHook)>()(
		user, flags, state, amplitude, frequency, offset);
	if ((user & 0xff) == 0xff) user = 0;
	if (status == ERROR_DEVICE_NOT_CONNECTED && FindControllerByUser(user)) return ERROR_SUCCESS;
	return status;
}

DWORD XamInputGetCapabilitiesHook(DWORD unknown, DWORD user, DWORD flags,
	PXINPUT_CAPABILITIES_EX capabilities) {
	DWORD status = g_xamInputGetCapabilitiesDetour.GetOriginal<decltype(&XamInputGetCapabilitiesHook)>()(
		unknown, user, flags, capabilities);
	if ((user & 0xff) == 0xff) user = 0;
	if (!capabilities || status != ERROR_DEVICE_NOT_CONNECTED || !FindControllerByUser(user)) return status;
	capabilities->Type = XINPUT_DEVTYPE_GAMEPAD;
	capabilities->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
	capabilities->Flags = 0;
	XINPUT_STATE state;
	memset(&state, 0, sizeof(state));
	XInputGetState(user, &state);
	capabilities->Gamepad = state.Gamepad;
	capabilities->Vibration.wLeftMotorSpeed = 0;
	capabilities->Vibration.wRightMotorSpeed = 0;
	return ERROR_SUCCESS;
}

NTSTATUS XInputdReadStateHook(DWORD context, PDWORD packetNumber,
	PXINPUT_GAMEPAD output, PBOOL unknown) {
	if (context < kDeviceContextBase || context >= kDeviceContextBase + kControllerCount)
		return g_xinputReadStateDetour.GetOriginal<decltype(&XInputdReadStateHook)>()(
			context, packetNumber, output, unknown);
	if (!output) return ERROR_INVALID_PARAMETER;
	int controllerIndex = -1;
	TritonVirtualController* controller = FindControllerByContext(context, &controllerIndex);
	if (!controller || !ProteusRouting::IsValidSlotIndex(controller->slotIndex)) return ERROR_INVALID_PARAMETER;
	ProteusRoutingSlot& slot = g_proteusSlots[controller->slotIndex];
	uint32_t generation = controller->slotGeneration;
	TritonProtocol::ControllerState state = {};
	if (ProteusRouting::AssociationMatches(slot.connected != 0, slot.controllerIndex,
		(uint32_t)slot.generation, controller->inUse != 0, controller->slotIndex,
		generation, controllerIndex)) {
		SnapshotState(slot, &state);
		if (!slot.connected || slot.controllerIndex != controllerIndex ||
			(uint32_t)slot.generation != generation) memset(&state, 0, sizeof(state));
	}
	memset(output, 0, sizeof(*output));
	if (state.guide) {
		DWORD now = GetTickCount();
		if (ProteusRouting::GuidePressIsDue(controller->guideLastPressTime, now, kGuideCooldownMs)) {
			controller->guideLastPressTime = now;
			XamInputSendXenonButtonPress(controller->userIndex);
		}
	}
	if (state.a) output->wButtons |= XINPUT_GAMEPAD_A;
	if (state.b) output->wButtons |= XINPUT_GAMEPAD_B;
	if (state.x) output->wButtons |= XINPUT_GAMEPAD_X;
	if (state.y) output->wButtons |= XINPUT_GAMEPAD_Y;
	// Preserve the original Triton assignment: the 0x40 bit is Start/Menu and
	// the 0x4000 bit is Back/View despite the legacy protocol constant names.
	if (state.view) output->wButtons |= XINPUT_GAMEPAD_START;
	if (state.menu) output->wButtons |= XINPUT_GAMEPAD_BACK;
	if (state.rightStick) output->wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
	if (state.leftStick) output->wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
	if (state.leftShoulder) output->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
	if (state.rightShoulder) output->wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
	if (state.dpadLeft) output->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
	if (state.dpadRight) output->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
	if (state.dpadUp) output->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
	if (state.dpadDown) output->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
	output->sThumbLX = state.leftX;
	output->sThumbLY = state.leftY;
	output->sThumbRX = state.rightX;
	output->sThumbRY = state.rightY;
	output->bLeftTrigger = state.leftTrigger ? state.leftTrigger : (state.leftTriggerClick ? 255 : 0);
	output->bRightTrigger = state.rightTrigger ? state.rightTrigger : (state.rightTriggerClick ? 255 : 0);
	if (packetNumber) *packetNumber = (DWORD)InterlockedIncrement(&controller->packetNumber);
	if (unknown) *unknown = FALSE;
	return STATUS_SUCCESS;
}

bool InitializeFunctionPointers() {
	g_isDevkit = *(uint32_t*)0x8010D334 == 0;
	HANDLE kernel = GetModuleHandleA("xboxkrnl.exe");
	HANDLE xam = GetModuleHandleA("xam.xex");
	if (!kernel || !xam) return false;
	XexGetProcedureAddress(kernel, 759, &UsbdGetDeviceDescriptor);
	XexGetProcedureAddress(kernel, 744, &UsbdGetEndpointDescriptor);
	XexGetProcedureAddress(kernel, 740, &UsbdAddDeviceComplete);
	XexGetProcedureAddress(kernel, 746, &UsbdOpenDefaultEndpoint);
	XexGetProcedureAddress(kernel, 747, &UsbdOpenEndpoint);
	XexGetProcedureAddress(kernel, 748, &UsbdQueueAsyncTransfer);
	XexGetProcedureAddress(kernel, 751, &UsbdRemoveDeviceComplete);
	XexGetProcedureAddress(kernel, 189, &g_freePhysicalMemory);
	XexGetProcedureAddress(kernel, 486, &g_xinputReadState);
	XexGetProcedureAddress(xam, 685, &g_xamInputGetCapabilities);
	XexGetProcedureAddress(xam, 402, &g_xamInputSetState);
	if (g_isDevkit) {
		UsbdGetInterfaceDescriptor = (UsbInterfaceDescriptorFn)0x8010D2D0;
		XamUserBindDeviceCallback = (XamBindDeviceFn)0x817A34B8;
		g_usbdPowerDownNotification = (UsbNotificationFn)0x8010E140;
		g_usbdDriverEntry = (UsbNotificationFn)0x8010DE48;
		*(DWORD*)0x80116298 = 0x48000018;
		*(DWORD*)0x801132A4 = 0x48000018;
		*(DWORD*)0x8010E04C = 0x60000000;
		*(DWORD*)0x8010E05C = 0x60000000;
		g_usbPhysicalPage = 0x8020A9B8;
	} else {
		UsbdGetInterfaceDescriptor = (UsbInterfaceDescriptorFn)0x800D8500;
		XamUserBindDeviceCallback = (XamBindDeviceFn)0x816D9060;
		g_usbdPowerDownNotification = (UsbNotificationFn)0x800D8FC8;
		g_usbdDriverEntry = (UsbNotificationFn)0x800D8D08;
		*(DWORD*)0x800E05E4 = 0x48000018;
		*(DWORD*)0x800DD8E0 = 0x48000018;
		*(DWORD*)0x800D8F00 = 0x60000000;
		*(DWORD*)0x800D8EF0 = 0x60000000;
		g_usbPhysicalPage = 0x801A8098;
	}
	return UsbdGetDeviceDescriptor && UsbdGetInterfaceDescriptor && UsbdGetEndpointDescriptor &&
		UsbdAddDeviceComplete && UsbdOpenDefaultEndpoint && UsbdOpenEndpoint &&
		UsbdQueueAsyncTransfer && UsbdRemoveDeviceComplete && XamUserBindDeviceCallback &&
		g_usbdPowerDownNotification && g_usbdDriverEntry && g_freePhysicalMemory &&
		g_xinputReadState && g_xamInputGetCapabilities && g_xamInputSetState;
}

void ProteusPublishState(uint8_t interfaceNumber,
	const TritonProtocol::ControllerState& state) {
	if (interfaceNumber < TritonProtocol::kFirstSlotInterface ||
		interfaceNumber > TritonProtocol::kLastSlotInterface) return;
	ProteusRoutingSlot& slot = g_proteusSlots[interfaceNumber - TritonProtocol::kFirstSlotInterface];
	InterlockedIncrement(&slot.stateSequence);
	LONG next = 1 - slot.publishedStateIndex;
	slot.stateBuffers[next] = state;
	MemoryBarrier();
	InterlockedExchange(&slot.publishedStateIndex, next);
	InterlockedIncrement(&slot.stateSequence);
	InterlockedExchange(&slot.connected, 1);
}

void ProteusDisconnectController(uint8_t interfaceNumber) {
	if (interfaceNumber < TritonProtocol::kFirstSlotInterface ||
		interfaceNumber > TritonProtocol::kLastSlotInterface) return;
	ProteusRoutingSlot& slot = g_proteusSlots[interfaceNumber - TritonProtocol::kFirstSlotInterface];
	InterlockedExchange(&slot.connected, 0);
	InterlockedExchange(&slot.disconnectPending, 1);
}

BOOL APIENTRY DllMain(HANDLE, DWORD reason, PVOID) {
	if (reason != DLL_PROCESS_ATTACH) return TRUE;
	if ((XboxKrnlVersion->Build != 17559 && XboxKrnlVersion->Build != 17489) || IsTrayOpen()) {
		DbgPrint("TritonDriver: unsupported dashboard or disc tray open; aborting\n");
		return FALSE;
	}
	DbgPrint("TritonDriver: starting Triton-over-Proteus driver\n");
	if (!InitializeFunctionPointers()) return FALSE;
	InitializeRouting();
	if (g_isDevkit) {
		g_hidAddDeviceDetour = Detour((void*)0x8011AE38, (void*)HidAddDeviceHook);
		g_hidRemoveDeviceDetour = Detour((void*)0x8011ADF8, (void*)HidRemoveDeviceHook);
	} else {
		g_hidAddDeviceDetour = Detour((void*)0x800E4D68, (void*)HidAddDeviceHook);
		g_hidRemoveDeviceDetour = Detour((void*)0x800E4D28, (void*)HidRemoveDeviceHook);
	}
	g_hidAddDeviceDetour.Install();
	g_hidRemoveDeviceDetour.Install();
	g_xamInputGetCapabilitiesDetour = Detour(g_xamInputGetCapabilities, (void*)XamInputGetCapabilitiesHook);
	g_xamInputSetStateDetour = Detour(g_xamInputSetState, (void*)XamInputSetStateHook);
	g_xinputReadStateDetour = Detour(g_xinputReadState, (void*)XInputdReadStateHook);
	g_xamInputSetStateDetour.Install();
	g_xamInputGetCapabilitiesDetour.Install();
	g_xinputReadStateDetour.Install();
	g_usbdPowerDownNotification();
	g_freePhysicalMemory(0, *(DWORD*)g_usbPhysicalPage);
	g_usbdDriverEntry();
	HANDLE serviceThread = MakeSystemThread((LPTHREAD_START_ROUTINE)ProteusServiceThreadProc, 0);
	if (!serviceThread) return FALSE;
	CloseHandle(serviceThread);
	return TRUE;
}
