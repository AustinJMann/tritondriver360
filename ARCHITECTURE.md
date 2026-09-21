# Driver architecture

TritonDriver is an Xbox 360 system XEX plugin that exposes Triton controllers
connected directly over USB (`28DE:1302`) or through one Valve Proteus wireless
puck (`28DE:1304`) as virtual Xbox 360 controllers. Four puck sources (interfaces
2-5) and four wired sources share four virtual controllers and available XAM
positions. Basic direct USB operation has been confirmed on hardware; mixed
topologies and removal stress testing remain outstanding.

## Source layout

| Path | Responsibility |
| --- | --- |
| `hiddriver/main.cpp` | Plugin startup, kernel and XAM hooks, virtual controller ownership, input publication, title profiles, and worker threads. |
| `hiddriver/controller_usb.cpp`, `controller_usb.h` | USB slot lifecycle, endpoint setup, asynchronous transfers, raw-mode heartbeats, rumble delivery, retries, and removal. |
| `hiddriver/triton_protocol.cpp`, `triton_protocol.h` | Platform-independent report decoding, byte-order conversion, controller state mapping, and feature/rumble report encoding. |
| `hiddriver/controller_routing.h` | Shared slot, binding, generation, and guide-button timing checks. |
| `hiddriver/controller_usb_policy.h`, `triton_hid_descriptor.h` | Transport admission, connection policies, and wired HID report validation. |
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

`controller_usb.cpp` owns USB transport state. `main.cpp` owns virtual controllers
and XAM player assignments. A USB interface is not a player index: sources bind to
available XAM positions, and unsuccessful bindings are retried. Generation
checks prevent stale bindings and rumble requests from reaching a new connection.

## Input and output flow

1. The HID add hook admits a Proteus slot or a wired Triton candidate. The
   transport obtains descriptors, opens endpoints, and queues interrupt input.
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
transfers as fallback. Control transfers share ownership across puck slots;
each wired device has its own arbiter.

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
- Removal waits for the binding worker acknowledgment and a grace period before
  reusing a source record. USB allocations remain quarantined for the session;
  the grace period does not prove the kernel has released storage.
  Kernel-observed structure offsets must stay intact.

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
The protocol suite compiles portable protocol/configuration sources and shared
headers. A separate Win32 harness compiles the real USB transport against a fake
kernel, retaining the 32-bit ABI assertions and controlling completion order.
Neither suite executes Xbox hooks, native storage I/O, or the physical USB stack.
Hardware validation is still required for those paths. `tests/fixtures/README.md`
describes the evidence required for real device captures; synthetic test packets
are not hardware captures.

## USB source identity and lifecycle

`ControllerUsbPolicy::Classify` accepts the existing Proteus interfaces and
non-boot HID candidates on Triton USB. Descriptor length/type and alternate
setting are checked before claiming a candidate. Other products, boot HID, and
non-slot puck interfaces pass through to the native HID driver.

The USB engine has eight source records: 0-3 for the existing single-puck
contract, 4-7 for wired attachments. Interface numbers select descriptors and
control-request `wIndex`; they never identify a wired source or XAM player.
Repeated adds of the same handle are idempotent; occupied slots and exhausted
capacity return failure. Each new attachment receives a source token containing
its pool index and a monotonic epoch. Routing callbacks validate that token.
Binding generations remain separate and protect rumble mailboxes. Epoch and
binding-generation exhaustion stop reuse rather than wrapping to an old identity.

Sources become eligible for XAM binding only after a valid state report. Empty
puck slots reserve no player. The worker tries ready sources in arrival order,
retains existing bindings, and retries unbound sources without nonzero rumble.
The arrival comparison uses modular 32-bit sequence order. It assumes fewer
than 2^31 new connection events while a source waits for a player.

Each wired attachment allocates a device context. The four puck sources share
one context, retaining the repository's one-puck restriction. Contexts own the
configuration and HID descriptor buffers, initialization progress, retry deadline,
and endpoint-zero arbiter. Endpoint descriptors and I/O belong to sources.
The transport-kind policy lives in `controller_usb_policy.h`; both kinds use
the same transfer engine.

There is no verified physical-parent key in the exposed Xbox handle layout.
Consequently multiple pucks and composite wired layouts are unsupported. Do not
extend admission to those layouts by grouping matching VID/PID values. The
current single-puck indexing is not a general physical-device identity system.

## Descriptor validation and initialization

Initialization reads the nine-byte configuration header, then the full bounded
configuration (maximum 1024 bytes), using completed byte counts. Endpoint lookup
requires the admitted interface at alternate setting zero; there is no fallback
to an endpoint from a different interface.

Wired admission additionally requires one interface in the configuration and a
bounded HID report descriptor. The HID parser validates item boundaries, global
push/pop state, collection nesting, feature report 1 (63 payload bytes), rumble
output report 0x80 (9 payload bytes), and a supported state report with at least
the 17-byte input payload. Unknown or malformed layouts stop initialization
before controller input, raw-mode commands, or rumble. Such a candidate remains
owned but inactive until unplugged; asynchronous rejection does not hand an
already-completed add back to the native driver.

The engine queries the current configuration. It leaves the expected active
configuration alone, selects the descriptor's configuration value only when
unconfigured, and rejects a different active configuration. Transient USB errors
retry after 250 ms; structurally unsupported descriptors remain inactive.
Initialization ownership is established before queueing, including when the
kernel delivers synchronous completion. No global configuration lock stalls
unrelated devices.

After endpoint setup, wired sources queue interrupt input and enable raw-mode
heartbeats immediately, without waiting for wireless activity or XAM binding.
Wired heartbeat failures retry with bounded backoff and do not trigger the
empty-puck pause policy. Puck slots still wait for activity and pause on wireless
disconnect. Wireless status reports do not change a wired source's lifecycle.
Input failures invalidate connectivity and schedule a retry; valid input can
bind again. Silence alone does not disconnect a device.

Input uses the completed byte count at TRB + 0x1c, bounded by buffer capacity.
In the embedded input TRB this occupies extension + 0x20, previously modeled as
an interface byte and padding. Interface identity now lives only in the source
record. Control completion has an explicit byte-count field at the same TRB
relative offset. Extension size and other kernel-observed offsets stay unchanged.
Basic wired input has been confirmed on console hardware. Cancellation and
rapid reconnect still require dedicated hardware testing.

Rumble prefers interrupt-OUT when available and uses HID output SET_REPORT as
fallback, with the source's actual interface number. Existing encoding, scaling,
refresh, retries, and per-title settings are shared. Descriptor validation checks
that wired output/feature report sizes match the shared encoders.

## Removal and reference evidence

Removal prevents new requests, clears connectivity and rumble, and asks the
binding worker to retire the attachment. The worker publishes the requested
retirement epoch only after unbinding, so a delayed acknowledgment cannot retire
a new attachment. Reuse requires that acknowledgment and the removal grace period.
A pending control transfer also prevents source reuse while another live puck
slot still shares its arbiter.

The kernel removes/cancels endpoints. TRBs, input buffers, feature/rumble payloads,
and device descriptor buffers are allocated per attachment and quarantined for
the rest of the session. Old callback addresses cannot resolve to new source
records; late completions cannot release a new attachment's arbiter. This retains
memory on each reconnect, as the previous USB implementation did. Do not replace
quarantine with a timeout-based free without proving the kernel lifetime contract.

Protocol references inspected on 2026-09-21:

- [SDL Triton driver, revision 09b86eb7](https://github.com/libsdl-org/SDL/blob/09b86eb7df3cfae1f0c3bab5b948f6120f1e386b/src/joystick/hidapi/SDL_hidapi_steam_triton.c): common input and output paths, immediate wired connectivity, and periodic raw-mode refresh.
- [SDL controller IDs](https://github.com/libsdl-org/SDL/blob/09b86eb7df3cfae1f0c3bab5b948f6120f1e386b/src/joystick/controller_list.h): Triton USB 1302, BLE 1303, and Proteus 1304.
- [Linux HID Steam driver, revision 93f51579](https://github.com/torvalds/linux/blob/93f51579e7df248780214094418f205253383cc5/drivers/hid/hid-steam.c): descriptor-based interface selection, numbered controller feature reports, and HID output handling. No Linux implementation code is copied.

The Win32 transport harness is built separately:

```powershell
msbuild tests\controller_usb_tests.vcxproj /p:Configuration=Release /p:Platform=Win32
if ($LASTEXITCODE -eq 0) { & .\tests\Release\controller_usb_tests.exe }
```

It covers wired startup without input, repeated heartbeat failures, short input,
wireless-status isolation, input recovery, eight USB sources, independent device
arbiters, puck control sharing, rumble paths, descriptor rejection, configuration
selection/retry, retirement acknowledgment, and late completions. Test descriptors
are synthetic and are not stored as hardware fixtures.

Basic direct USB operation was confirmed on 2026-09-21. Remaining hardware
coverage includes cold boot/hotplug, multiple wired controllers,
mixed puck/wired/native controllers, all controls and profiles,
rumble isolation/stop/refresh, unplug during each transfer type, repeated
reconnects and memory growth, and recovery when player positions become free.
Cable insertion is a reconnect; stable player migration and cross-transport
identity deduplication are not implemented. If one physical controller streams
on USB and wireless simultaneously, it may occupy two player positions.
