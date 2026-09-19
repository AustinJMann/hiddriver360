# TritonDriver

TritonDriver is an Xbox 360 system plugin for Steam Controller 2026 (Triton)
controllers connected through a Valve Proteus wireless puck (`28DE:1304`).

```diff
! AI was heavily utilized in the creation of this driver.
```

## Requirements

- A modded Xbox 360 capable of loading XEX plugins.
- One Valve Proteus wireless puck.
- One to four Triton controllers already paired.

TritonDriver does not manage pairing. Pair the controllers before connecting
the puck to the console.

## Installation and use

1. Download `tritondriver.xex` from [here](https://github.com/AustinJMann/tritondriver360/releases/latest) and copy it to the console.
2. Load it at runtime or add it to the plugin list in `launch.ini`.
3. Connect the Proteus puck and power on the paired Triton controllers.

Each connected Triton is registered as a virtual Xbox 360 controller when an
XAM player position is available. A connected slot that cannot bind immediately
is retried automatically.

Supported input includes face buttons, D-pad, shoulders, stick clicks, menu and
view, both sticks, analog triggers, and the guide button.

Rumble is implemented and works, but it might feel weird in some games.
This is a know issue when converting rumble designed for use with a motor to the
Trition controller. I have implemented a curve and cutoff that makes it feel better,
but some games still feel a bit off or cause a rumble pulsing feeling.

## Current limitations

- Triton-over-Proteus only; direct USB, Bluetooth, and BLE are unsupported.
- One Proteus puck, with up to four paired controllers.
- No touch or IMU input.
- Four simultaneous physical Tritons have not yet been hardware-validated.

Other HID devices and non-slot Proteus interfaces are delegated to the Xbox 360
USB stack and are not claimed by TritonDriver.

## Building

1. Install the official Xbox 360 SDK with the full feature set, Visual Studio
   2010 Ultimate, and Visual Studio 2022.
2. Run `vs2022\Install-Xbox360Platform.ps1` from an elevated PowerShell prompt.
   If automatic detection fails, pass `-VsPath` with the Visual Studio path.
3. Open `TritonDriver.sln` in Visual Studio 2022 and decline upgrade or retarget
   prompts. The project must retain the `2010-01` Xbox 360 toolset.
4. Build `Release Retail|Xbox 360`. The deployable output is
   `tritondriver.xex`.

Host protocol, rumble routing and scheduling, and USB descriptor tests are in
`tests/triton_protocol_tests.vcxproj`. Run them from a Visual Studio 2022
Developer PowerShell prompt with the desktop C++ tools installed:

```powershell
msbuild tests\triton_protocol_tests.vcxproj /p:Configuration=Release /p:Platform=x64
if ($LASTEXITCODE -eq 0) { & .\tests\x64\Release\triton_protocol_tests.exe }
```

The executable exits with code 0 when all checks pass. These host tests do not
exercise the Xbox USB stack or replace testing with a physical puck.

## Attributions

- [EinTim23](https://github.com/EinTim23/) for the original Xbox 360 USB and
  virtual-controller driver work.
- [localcc](https://github.com/localcc/) for low-level USB assistance.
- [SDL](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_steam_triton.c)
  for the public Triton HID protocol reference.
- [iMoD1998](https://github.com/iMoD1998) for the retained Detours library.
