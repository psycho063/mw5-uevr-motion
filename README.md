# MW5 Motion

A [UEVR](https://github.com/praydog/UEVR) plugin that sends motion and haptic
telemetry from MechWarrior 5: Mercenaries to [SimHub](https://www.simhubdash.com/),
for motion platforms.

**Status: v0.7.1, work in progress.** Motion, the SimHub effects and automatic
profile switching have been used on a 3DOF platform. The comfort cap on the
running bounce is new in this version: its formula was checked against recorded
missions, but the cap itself has so far only run in unit tests.

No MW5 mod is needed. The plugin reads base-game classes only, so it works with
or without mods such as YetAnotherMechlab. It is meant to be used with the
MechWarriorVR mod's UEVR profile.

## What SimHub receives

| SimHub | From the game |
| --- | --- |
| Motion: pitch, roll, yaw, surge, sway, heave | Movement of the cockpit, including torso twist and aiming up or down |
| Gear Change effect | MASC or supercharger switched on (gear 1 → 2) or off (2 → 1) |
| Engine Vibrations effect | Jump jets: 40% RPM during warm-up, 80% while firing. MASC running: 90% falling to 50% as the gauge fills, which feels rougher as it nears damage |
| ABS effect | MASC warning while MASC is on: 2 taps a second from the game's warning point, rising to 6 at the damage point; never held on. Can be switched off |
| Vehicle name | A speed band or the chassis, used to pick a motion profile |

The gauge thresholds are read from the game for the fitted MASC or supercharger.
Extra values (MASC gauge, jump jet state, heat, chassis, variant and more) are
sent as custom properties for dashboards and custom effects.

The game exposes no velocities for the cockpit. The plugin derives velocity,
acceleration and rotation rates from the cockpit transform and smooths them with
time-based low-pass filters, so the result does not depend on the frame rate.

### Vehicle names

| Name | Top speed without MASC |
| --- | --- |
| `MW5 Slow` | up to 60 km/h |
| `MW5 Standard` | 61 to 75 km/h |
| `MW5 Fast` | 76 to 100 km/h |
| `MW5 Very Fast` | over 100 km/h |

Chassis listed under `SpecificChassis` in the configuration are reported as
`MW5 <Chassis>` instead, so they can have a motion profile of their own. The name
is chosen once per mech and does not change during a mission.

## Build

Requires Visual Studio 2022 with the C++ and CMake components.

```powershell
.\Build.ps1
```

This builds the plugin, runs the unit tests and fills `dist\`.

## Install

1. Close the game.
2. Copy `dist\plugins\MW5Motion.dll` to
   `%APPDATA%\UnrealVRMod\MechWarrior-Win64-Shipping\plugins`.
3. Optional: copy `dist\mw5_motion.cfg` to
   `%APPDATA%\UnrealVRMod\MechWarrior-Win64-Shipping`. Without it the defaults apply.
4. Import `dist\simhub\MechWarrior5.simdef` into SimHub as an External Sim game
   (SimHub 9.11.5 or later; see the
   [External Sim documentation](https://manual.simhubdash.com/external-sim-integration)).
   The game appears as "MechWarrior 5 Mercenaries (UEVR)" on UDP port 20779.

Do not run this plugin together with SpaceMonkey's UEVR plugin for the same
purpose; SimHub reads one game at a time.

To remove the plugin, close the game and delete the DLL.

## Configuration

See the comments in [mw5_motion.cfg](mw5_motion.cfg). The settings worth knowing:

- **`HeaveComfortG`** (default 3): the most running bounce any mech may send, as
  a peak in G. See the next section.
- **`HeaveGain`** and **`HeaveLimitG`**: an overall scale for the vertical force,
  and a soft ceiling for everything that is not running bounce, such as jumps.
- **Per-chassis overrides.** A `[Chassis]` block at the end of the file overrides
  motion settings for one chassis, for example:

  ```ini
  [Phoenixhawk]
  HeaveComfortG = 2.5
  ```

## Comfort cap on the running bounce

Fast mechs with a tall stride bounce the cockpit several times harder than slow
ones: at top speed a Mauler sends about 1.4 G and a Phoenix Hawk about 4.3 G.
The plugin predicts the bounce and scales the vertical force so that no mech
exceeds `HeaveComfortG`.

- **It follows speed.** The game cross-fades each chassis's walk and run
  animation by ground speed. From the bounce height of the two animations and
  the mech's current speed the plugin works out the step rate and how hard the
  bounce is. Below the speed where a mech reaches the comfort level its bounce is
  untouched; above it the bounce is held at the level. Mechs that never reach it
  are never changed.
- **Per chassis, not per variant.** Variants and engine swaps share a chassis's
  animations and differ only in speed, which is read live.
- **Jumps and landings are not capped.** The cap eases off while the jump jets
  are busy.
- **Safety net.** For anything the prediction cannot cover (a chassis without
  data, a limping mech), a slow limiter eases the vertical force down while it
  stays above 1.25 times the comfort level. `HeaveSafetyNet = 0` turns it off.

The bounce data for 103 chassis is built in (`data/mech_gait.csv`, generated from
the game's first-person walk and run animations). The prediction was checked
against recorded missions for nine chassis: step rate to within about 0.05 steps
per second, and the vertical force sent to SimHub to within about 10%.

For a chassis that is not covered, such as a modded mech:

- add a row for it to `mw5_mech_gait.csv` in the UEVR profile folder, in the same
  format as `data/mech_gait.csv`; or
- set `GaitLearning = 1`, and the plugin measures the cockpit's bob while you walk
  and run in that mech and writes the row itself. It is off by default and does
  nothing for chassis that already have data.

Rows in `mw5_mech_gait.csv` take precedence over the built-in table.

## Checking that it works

While the game runs, the plugin rewrites
`%APPDATA%\UnrealVRMod\MechWarrior-Win64-Shipping\mw5_motion_status.json` once per
second. `"reader"` says what the plugin currently sees (`ok` in a mech),
`"vehicleName"` and `"chassis"` show the mech's identity, and `"frame"` holds the
latest values.

### Debug recording

Set `DebugLog = 1` in the configuration to record a whole session. Two files are
written to the profile folder and restarted each time the game starts:

| File | Contents |
| --- | --- |
| `mw5_motion_debug.csv` | Samples: position, angles, rates, velocities, accelerations, speed, MASC and jump jet state, the gear, ABS and RPM values sent to SimHub, and the comfort cap's ground speed, predicted bounce, factor and safety-net gain. `DebugLogRateHz` rows per second (default 20); recording stops at 50 MB. |
| `mw5_motion_debug.log` | Timestamped events: mech found, vehicle name chosen, which bounce data is used and from what speed the cap acts, MASC and jump jet changes, pauses, motion resets, read failures. |

Set `DebugJsonEnable = 1` to also receive every frame as JSON over UDP
(port 20780 by default), for live tools.

## Changing the SimHub definition

`simhub\MechWarrior5.simdef` defines the packet. After editing its field list,
regenerate the packet layout with Windows PowerShell 5.1 and a folder containing
SimHub's `SimHub.Plugins.dll` and its dependencies, then rebuild:

```powershell
powershell.exe -File tools\Generate-SimHub.ps1 -SimHubDirectory <folder>
.\Build.ps1
```

A changed definition has a new telemetry signature, so install the rebuilt DLL
and the `.simdef` together.

```text
UniqueId: 27a2e5cd-aa4c-431e-8c6e-d9a5dcd9f71f
GameSignature: 0xEB0A2F3D
TelemetrySignature: 0xD77B2249
Layout: 2.1
Packet length: 644
```

## Layout

| Path | Contents |
| --- | --- |
| `src/Telemetry.*` | Motion maths and the comfort cap; no game or UEVR dependency |
| `src/Gait.*`, `src/GaitTable.hpp` | Bounce data per chassis (generated table, file format) and the optional learner |
| `data/mech_gait.csv` | The bounce table in reviewable form |
| `src/SimHub.hpp` | Haptic mapping, vehicle naming and the SimHub packet |
| `src/SimHubGenerated.hpp` | Packet layout, extracted from SimHub's generated sender |
| `src/Plugin.cpp` | UEVR plugin: game reader, configuration, sender, status and debug output |
| `simhub/` | SimHub External Sim definition and SimHub's generated reference sender |
| `tests/` | Unit tests for the motion maths, comfort cap, haptics and naming |
| `tools/Generate-SimHub.ps1` | Regenerates the packet layout from the definition |
| `tools/read_animations.py`, `read_blendspaces.py`, `summarise_animations.awk`, `make_gait_table.awk`, `extract_mech_stats.ps1` | Rebuild the bounce table from the game's animations with the MW5 mod editor |
| `vendor/uevr/` | UEVR plugin SDK headers (MIT, unchanged) |

## Credits

- Inspired by, and with parts adapted from,
  [galactic-racer-uevr-motion](https://github.com/elliotttate/galactic-racer-uevr-motion)
  by elliotttate: the idea of a UEVR plugin that feeds SimHub directly, and the
  starting point for the configuration reader, UDP sender and packet building.
  The game reader, the motion maths and the SimHub definition are specific to
  MechWarrior 5.
- [UEVR](https://github.com/praydog/UEVR) by praydog, and the
  [MechWarriorVR](https://www.nexusmods.com/mechwarrior5mercenaries/mods/1009) mod
  and [MW5-UEVR-Plugins](https://github.com/sicsix/MW5-UEVR-Plugins) by sicsix,
  whose cockpit component this plugin follows.

## Licence

MIT; see [LICENSE](LICENSE). The UEVR headers in `vendor/uevr/` keep their own
MIT notices.
