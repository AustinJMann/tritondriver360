# Configuration

TritonDriver loads `tritonconfig.yml` from the root of a USB storage device or,
if none is found, from `Hdd1:\tritonconfig.yml`. If neither exists, it creates
one on the first writable USB device, or on `Hdd1:`. Changes take effect the
next time a game starts or you return to the dashboard.

The generated file contains the default settings and an example override for
Call of Duty: Black Ops II:

```yaml
version: 1

defaults:
  paddles:
    r4: none
    r5: none
    l4: none
    l5: none
  rumble:
    enabled: true
    left_gain: 2.0
    right_gain: 2.0
    deadzone: 0.10
    curve: cubic
  mouse_joystick:
    sensitivity_x: 0.5
    sensitivity_y: 0.5
    minimum_output_x: 0.2
    minimum_output_y: 0.2
    smoothing_ms: 8
    noise_speed_threshold: 0.2
  right_trackpad:
    mode: mouse_joystick
    click_action: right_stick
    trackball_enabled: true
    friction_curve: ease_out_cubic
    friction_strength: 3
    friction_max_speed: 6
    friction_reference_ms: 150
    friction_min_ms: 30
    friction_max_ms: 300
    friction_vertical_scale: 0.5
    haptics_intensity: 0.25
    click_haptics_intensity: 0.7
    release_haptics_intensity: 0.35
    haptics_max_hz: 80
    haptics_full_speed: 6.0
    physical_stick_threshold: 0.15

games:
  "415608C3": # Call of Duty: Black Ops II
    paddles:
      r4: x
      r5: y
      l4: a
      l5: b
```

`defaults` apply to every game. Entries under `games` use the game's
eight-digit Title ID and override only the settings they list.

Keep the structure shown above; lists, anchors, and multiline values are not
supported. If the file contains an error, the driver leaves it unchanged and
keeps its previous settings (built-in defaults at startup).

Button names for `paddles` and `click_action`: `none`, `a`, `b`, `x`, `y`,
`dpad_up`, `dpad_down`, `dpad_left`, `dpad_right`, `left_shoulder`,
`right_shoulder`, `left_stick`, `right_stick`, `start`, `back`, `guide`,
`left_trigger`, `right_trigger`.

## Paddles and rumble

| Setting | Values | Effect |
| --- | --- | --- |
| `r4`, `r5`, `l4`, `l5` | Button name | Button pressed by each back paddle. |
| `enabled` | `true`, `false` | Turns rumble on or off. |
| `left_gain`, `right_gain` | `0.0`–`2.0` | Rumble strength. |
| `deadzone` | `0.0`–`0.95` | Rumble weaker than this is ignored. |
| `curve` | `linear`, `quadratic`, `cubic` | Rumble response curve. |

## Right-pad aiming

With `right_trackpad.mode: mouse_joystick`, finger speed on the right pad drives
the right stick like a mouse. Speeds are in pad widths per second (w/s). With
`disabled`, the pad, `click_action`, and pad haptics are all off.

Each axis maps finger speed to stick deflection:

```text
deflection = minimum_output + (1 - minimum_output) * min(speed * sensitivity, 1)
```

Full deflection is reached at `1 / sensitivity` w/s. `minimum_output` applies
only to the dominant axis, so small off-axis drift stays inside the game's
deadzone.

With `trackball_enabled`, a swipe keeps coasting after the finger lifts and
slows to a stop over:

```text
duration = clamp(friction_reference_ms * speed / friction_strength,
                 friction_min_ms, friction_max_ms)
```

`speed` is limited to `friction_max_speed` for this calculation. The vertical
duration is first divided by `0.25 + 1.5 * friction_vertical_scale`, then
clamped to the same range.

| Setting | Values | Effect |
| --- | --- | --- |
| `sensitivity_x`, `sensitivity_y` | `0.01`–`20.0` | Deflection per w/s. |
| `minimum_output_x`, `minimum_output_y` | `0.0`–`1.0` | Smallest deflection on the dominant axis while moving. |
| `smoothing_ms` | `0`–`100` | Smooths jitter; higher values add lag. `0` disables it. |
| `noise_speed_threshold` | `0.0`–`1.0` | Finger speeds at or below this are ignored and do not start a coast. |
| `mode` | `disabled`, `mouse_joystick` | Turns right-pad aiming on or off. |
| `click_action` | Button name | Button held while the pad is clicked. |
| `trackball_enabled` | `true`, `false` | Coasting after a swipe. |
| `friction_curve` | `linear`, `ease_out_quadratic`, `ease_out_cubic`, `ease_out_quartic` | How the coast slows down; later curves drop off faster at first. |
| `friction_strength` | `0.1`–`10.0` | Higher values give shorter coasts. |
| `friction_reference_ms` | `1`–`5000` | Coast length per w/s at strength `1`. |
| `friction_max_speed` | `0.0`–`60.0` | Speed limit for the duration calculation; `0` removes it. |
| `friction_min_ms`, `friction_max_ms` | `1`–`5000` | Shortest and longest coast on either axis. |
| `friction_vertical_scale` | `0.0`–`1.0` | `0.5` is equal; lower lengthens vertical coasts, higher shortens them. |
| `physical_stick_threshold` | `0.0`–`1.0` | Physical right stick deflection that overrides the pad and stops a coast. |
| `haptics_intensity` | `0.0`–`1.0` | Strength of ticks while moving or coasting. |
| `click_haptics_intensity` | `0.0`–`1.0` | Strength of the pad-click pulse. |
| `release_haptics_intensity` | `0.0`–`1.0` | Strength of the click-release pulse. |
| `haptics_max_hz` | `1`–`100` | Tick rate at `haptics_full_speed` and above. |
| `haptics_full_speed` | `0.01`–`20.0` | Speed at which ticks reach `haptics_max_hz`; slower motion ticks proportionally slower. |

Tuning notes:

- Set `minimum_output` just above the game's stick deadzone. The minimum moves
  to the other axis when a swipe crosses 45°, which can feel like a small step.
- Only the ratio `friction_reference_ms / friction_strength` matters. The
  generated values give 50 ms of coast per w/s, from 30 ms to 300 ms.
- `friction_vertical_scale` cannot push a coast outside `friction_min_ms` and
  `friction_max_ms`. On fast flicks that already coast for `friction_max_ms`,
  lowering it has no effect; raise `friction_max_ms` instead.
- With a nonzero `minimum_output`, a coast holds at least that deflection
  until it nearly stops, then drops to zero. The curve mostly affects how
  quickly it falls toward the minimum.
- While a finger rests on the pad, small physical stick movements below
  `physical_stick_threshold` are ignored.
- Smoothing restarts with each new movement, so high `smoothing_ms` values
  also soften the start of every swipe.
- Haptic intensity `0` mutes that feedback. Pad haptics are separate from game
  rumble and are not affected by the rumble settings.
