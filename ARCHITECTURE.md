# Driver architecture

TritonDriver is an Xbox 360 system XEX plugin that exposes Triton controllers
connected through a Valve Proteus wireless puck as virtual Xbox 360 controllers.
It supports one puck (`28DE:1304`) with four controller slot interfaces (2-5).
Other USB devices and non-slot interfaces remain with the native USB stack.

## Source layout

| Path | Responsibility |
| --- | --- |
| `hiddriver/main.cpp` | Plugin startup, kernel and XAM hooks, virtual controller ownership, input publication, title profiles, and worker threads. |
| `hiddriver/proteus.cpp`, `proteus.h` | USB slot lifecycle, endpoint setup, asynchronous transfers, raw-mode heartbeats, rumble delivery, retries, and removal. |
| `hiddriver/triton_protocol.cpp`, `triton_protocol.h` | Platform-independent report decoding, byte-order conversion, controller state mapping, and feature/rumble report encoding. |
| `hiddriver/proteus_routing.h` | Shared slot, binding, generation, and guide-button timing checks. |
| `hiddriver/rumble_output.h` | Rumble routing policy, configurable intensity scaling, generation-tagged requests, refresh timing, and retry state. |
| `hiddriver/controller_capabilities.h` | Virtual gamepad capability reporting. |
| `hiddriver/triton_config.cpp`, `triton_config.h` | Platform-independent restricted YAML parser, defaults, per-title profiles, and paddle mappings. |
| `hiddriver/config_storage.cpp`, `config_storage.h` | Xbox native file I/O for loading or creating `tritonconfig.yml`. |
| `hiddriver/usb.h`, `usb_descriptors.h` | USB definitions and descriptor parsing helpers. |
| `hiddriver/driver_types.h` | Kernel-facing USB transfer and device extension layouts, with compile-time offset checks. |
| `hiddriver/Detours.cpp`, `Detours.h` | PowerPC function interception and trampolines. |
| `hiddriver/xkelib/` | Xbox kernel and XAM declarations and supporting library. |
| `hiddriver/xex.xml` | System-plugin image settings. |
| `tests/` | Desktop tests for portable logic and a manifest for future hardware captures. |
| `vs2022/` | Visual Studio integration for the Xbox 360 SDK toolchain. |

## Startup and ownership

`DllMain` checks the dashboard build (17489 or 17559) and disc tray state,
resolves kernel/XAM functions, initializes routing, and loads configuration.
Configuration is read before restarting the USB stack so storage is still
available. Missing configuration is created on the first writable USB volume,
with the hard drive as fallback; invalid existing files are preserved.

Startup creates the USB service and binding workers before installing hooks.
It then installs HID add/remove and input hooks, restarts the USB stack to
enumerate devices, and resumes the workers. Kernel integration includes fixed
addresses and patches selected for retail or devkit systems.

`proteus.cpp` owns USB transport state. `main.cpp` owns virtual controllers and
XAM player assignments. A puck interface is not a player index: slots bind to
available XAM positions, and unsuccessful bindings are retried. Generation
checks prevent stale bindings and rumble requests from reaching a new connection.

## Input and output flow

1. The HID add hook admits a supported Proteus slot. The transport obtains
   descriptors, opens endpoints, and queues interrupt input.
2. USB completion callbacks decode reports through `TritonProtocol` and publish
   controller state to `main.cpp`. Wireless disconnects schedule unbinding.
3. The binding worker assigns connected slots to virtual controllers. Input
   hooks read a consistent state snapshot, apply the active paddle mappings,
   and provide Xbox input state. Capability hooks describe supported controls.
4. The XAM rumble hook finds the owning virtual controller, applies the active
   rumble settings, and publishes an atomic, generation-tagged request.
5. USB maintenance sends encoded rumble reports, refreshes active effects, and
   retries failures. It also sends periodic lizard-off feature reports to keep
   the controller in raw input mode.

Native input calls are forwarded when the request does not belong to a virtual
controller. Rumble uses an interrupt output endpoint when available, with control
transfers as fallback. Control transfers share ownership across puck slots.

## Threading and lifecycle constraints

- USB maintenance runs on hardware thread 2 at IRQL 2, serialized with USB
  completion DPCs. It must remain nonblocking and retain transfer ownership until
  completion callbacks release it.
- A separate binding worker on hardware thread 4 performs potentially blocking
  XAM operations and checks title changes. It does not hold up rumble refreshes.
- Input publication uses two state buffers, a sequence counter, memory barriers,
  and atomic index updates so readers can obtain consistent snapshots.
- Rumble requests use atomic 64-bit mailboxes. Binding generations reject stale
  requests, and title changes stop prior effects before applying new settings.
- Removal waits for outstanding transfers and the removal grace period before
  releasing slot resources. Kernel-observed structure offsets must stay intact.

Configuration is loaded once at startup. The binding worker selects the global
or per-title profile as titles change; editing the file requires a plugin reload
or console restart. Parsing and mapping stay independent of Xbox file I/O.

## Build and validation

`TritonDriver.sln` builds `hiddriver/tritondriver.vcxproj` as
`Release Retail|Xbox 360`, producing `tritondriver.xex`. It uses the official Xbox
360 SDK and the `2010-01` toolset. `vs2022/Install-Xbox360Platform.ps1` installs
the integration needed to use that toolset from Visual Studio 2022. Keep driver
code compatible with this compiler and the Xbox ABI.

The separate desktop project uses Visual Studio 2022's `v143` toolset:

```powershell
msbuild tests\triton_protocol_tests.vcxproj /p:Configuration=Release /p:Platform=x64
if ($LASTEXITCODE -eq 0) { & .\tests\x64\Release\triton_protocol_tests.exe }
```

Tests cover protocol validation and encoding, USB descriptors, routing,
capabilities, configuration, paddle bindings, and rumble scaling/scheduling.
They compile the portable protocol and configuration sources plus shared header
logic; they do not execute the Xbox hooks, native storage I/O, or USB transport.
Hardware validation is still required for those paths. `tests/fixtures/README.md`
describes the evidence required for real device captures; synthetic test packets
are not hardware captures.
