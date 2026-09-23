# TritonDriver

TritonDriver is an Xbox 360 system plugin for Steam Controller 2026 (Triton)
controllers connected directly over USB (`28DE:1302`) or through a Valve Proteus
wireless puck (`28DE:1304`).

> [!WARNING]  
> AI was heavily utilized in the creation of this driver.

## Requirements

- A modded Xbox 360 capable of loading XEX plugins.
- Triton controllers and USB cable, or one Valve Proteus wireless puck
  with controllers already paired.

TritonDriver does not manage pairing. Pair the controllers before connecting
the puck to the console.

## Installation and use

1. Download `tritondriver.xex` from [here](https://github.com/AustinJMann/tritondriver360/releases/latest) and copy it to the console.
2. Load it at runtime or add it to the plugin list in `launch.ini`.
3. Connect Triton controllers over USB, or connect the Proteus puck
   and power on its paired controllers.

Each connected Triton appears as an Xbox 360 controller. If all four player
positions are taken, it connects automatically once one becomes free.

Supported input includes face buttons, D-pad, shoulders, stick clicks, menu and
view, both sticks, analog triggers, the guide button, back paddles, and
optional right-pad mouse-style aiming.

Rumble is implemented and works, but it might feel weird in some games.
This is a known issue when converting rumble designed for use with a motor to the
Triton controller. I have implemented a curve and cutoff that makes it feel better,
but some games still feel a bit off or cause a rumble pulsing feeling.

## Configuration

TritonDriver reads its settings from `tritonconfig.yml` on a USB storage device
or `Hdd1:`, and creates the file with default settings if none exists. Changes
take effect the next time a game starts or you return to the dashboard.

See [docs/configuration.md](docs/configuration.md) for the file format, paddle
and rumble settings, per-game overrides, and right-pad aiming.

## Current limitations

- Up to four wired controllers plus one Proteus puck can be attached; at most
  four controllers can be active at once.
- Bluetooth and BLE are unsupported.
- The left pad and gyro are not supported.
- Mixing wired and wireless controllers, and using four controllers at once,
  have not been tested on hardware.

## Building

1. Install the official Xbox 360 SDK with the full feature set, Visual Studio
   2010 Ultimate, and Visual Studio 2022.
2. Run `vs2022\Install-Xbox360Platform.ps1` from an elevated PowerShell prompt.
   If automatic detection fails, pass `-VsPath` with the Visual Studio path.
3. Open `TritonDriver.sln` in Visual Studio 2022 and decline upgrade or retarget
   prompts. The project must retain the `2010-01` Xbox 360 toolset.
4. Build `Release Retail|Xbox 360`. The deployable output is
   `tritondriver.xex`.

Developer tests are described in `ARCHITECTURE.md`.

## Attributions

- [EinTim23](https://github.com/EinTim23/) for the original Xbox 360 USB and
  virtual-controller driver work.
- [localcc](https://github.com/localcc/) for low-level USB assistance.
- [SDL](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_steam_triton.c)
  for the public Triton HID protocol reference.
- [iMoD1998](https://github.com/iMoD1998) for the retained Detours library.
