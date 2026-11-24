# MW5 Motion

MW5 Motion is a [UEVR](https://github.com/praydog/UEVR) plugin that sends motion telemetryb
and haptic effects from MechWarrior 5: Mercenaries to
[SimHub](https://www.simhubdash.com/) for motion platforms.

The current version is 0.11.3, and the plugin is still in development. It was
tested on one 3DOF platform with the MechWarriorVR mod. The plugin needs no MW5
mod and works with or without mods such as YetAnotherMechlab.

## You use this plugin at your own risk

**Motion platforms can cause serious injury and damage to property.** This
plugin controls the movement of a machine that carries a person. A software
error, a wrong setting or an unexpected event in the game can cause sudden and
violent movement.

- You alone are **responsible for the safe use** of your platform and for
  everyone who is near it.
- You should keep an **emergency stop within reach** and keep people, pets and
  objects away from the platform while it moves.
- You should **start with low gains and low limits** in SimHub and raise them in
  small steps.
- You should **test every new version and every changed setting** at low
  strength before normal use.
- You should **not use the plugin** if you are pregnant or have a heart, back or
  neck condition, or any condition that sudden movement could make worse.

This software is provided "as is", without warranty of any kind. To the fullest
extent that the law allows, the authors and contributors accept no liability for
any injury, death, damage or loss that results from its use. The full terms are
in the [LICENSE](LICENSE) file. If you do not accept these terms, do not use the
software.

## The plugin sends motion and five kinds of effect to SimHub

| SimHub | Source in the game |
| --- | --- |
| Motion | The movement of the cockpit, with torso twist and aiming up or down |
| Gear Change effect | MASC or a supercharger switches on or off |
| Engine Vibrations effect | Jump jets, running MASC, power-up and shutdown |
| ABS effect | Short warning taps while MASC is close to damage |
| Kick in the motion | A part of the mech is destroyed |
| Vehicle name | A speed band or the chassis, which selects the motion profile |

The MASC vibration becomes rougher as the gauge fills. The warning taps become
faster from the warning point of the game to the damage point. The power-up and
shutdown vibrations need MechWarriorVR with the MechShakerRelay mod.

A destroyed part tilts the platform for a moment towards the side that was
lost, with a rough burst of engine vibration. An arm gives the lightest kick, a
leg a stronger one, and a side torso the strongest of the three. A side torso
also tips the nose up when it was hit from the front, and down when it was hit
from the rear. The part effects need MechWarriorVR with the MechShakerRelay mod.

A destroyed mech only vibrates, because its fall is already in the motion. The
plugin asks the game whether the mech is dead and why. A cored mech, or one that
loses its engine with a side torso, gets a rough rumble of three seconds. A
pilot who ejects from an intact mech gets a short, lighter one.

SimHub also receives the MASC gauge, the jump jet state, heat, chassis and
variant as custom properties for dashboards.

## SimHub selects a motion profile by vehicle name

| Vehicle name | Top speed without MASC |
| --- | --- |
| `MW5 Slow` | up to 60 km/h |
| `MW5 Standard` | 61 to 75 km/h |
| `MW5 Fast` | 76 to 100 km/h |
| `MW5 Very Fast` | over 100 km/h |

A chassis that is listed under `SpecificChassis` is sent as `MW5 <Chassis>`, so
that it can have a profile of its own.

## Installing takes five steps

1. Download the zip file of the latest release from the Releases page, extract it,
   and close the game.
2. Copy `plugins\MW5Motion.dll` to
   `%APPDATA%\UnrealVRMod\MechWarrior-Win64-Shipping\plugins`.
3. Copy `simhub\MechWarrior5.simdef` to
   `%LOCALAPPDATA%\SimHub\ExternalSims\Definitions\MechWarrior5`, and restart
   SimHub. The game appears as "MechWarrior 5 Mercenaries (UEVR)" on UDP port
   20779. SimHub 9.11.5 or later is required.
4. Copy `mw5_motion.cfg` to
   `%APPDATA%\UnrealVRMod\MechWarrior-Win64-Shipping` if you want to change
   settings. The defaults apply without it.
5. Copy `scripts\mw5_motion.lua` to
   `%APPDATA%\UnrealVRMod\MechWarrior-Win64-Shipping\scripts` if you want the
   settings panel in the UEVR menu.

An optional sixth step gives you four starting motion profiles. The folder
`simhub\profiles` contains one `.shmotionprofile` file for each vehicle name:
`MW5 Slow`, `MW5 Standard`, `MW5 Fast` and `MW5 Very Fast`. Install the SimHub
definition from step 3 first, and then import the profiles in SimHub:

1. Open the **Motion effects** tab and click **Profiles manager** below the
   profile name.
2. Click **Import profile** at the bottom of the window and select one file.
3. Repeat step 2 for the other three files.
4. Select **Automatic** under "Automatic profile switching" in the same window.

Each profile is already assigned to its vehicle name, so SimHub then switches
between the profiles by itself.

These profiles were tuned for one 3DOF platform. Check the motion range and the
limits of every profile against your own platform before you use it, and start
with low gains.

Remove SpaceMonkey's UEVR plugin first, because SimHub can use only one game at
a time. To remove MW5 Motion, close the game and delete the DLL.

## The settings panel changes values while you play

The panel is in the UEVR menu under **LuaLoader**, in the section **MW5 Motion**.
It shows the current mech, its vehicle name and the speed at which the comfort
cap starts. A slider applies its value when you release it, and the plugin saves
every change to `mw5_motion.cfg`.

- The **Bounce** group sets the comfort level, the heave gain and the heave limit.
- The **This mech** group stores a comfort level and a gain for the chassis that
  you are in.
- The **Smoothing** group sets the four smoothing values.
- The **Jump jets and MASC** and **Power-up and shutdown** groups switch each
  effect on or off and set its levels and times.
- The **Part destruction** group sets the kick strength and the share for each
  kind of part.
- The **Test effects** group has one button for every effect, so that you can
  feel and tune an effect without waiting for it in a fight. The buttons work
  while you sit in a mech in a mission.
- The **Telemetry** group shows the live values: the mech and the vehicle name
  that are reported to SimHub, speed, angles, forces, the comfort cap and the
  effects that are sent.

Changes to the bounce and smoothing values reach the platform over about one
second, so that the platform does not jump. The sliders have a narrower range
than the file, and all other settings can be changed only in the file.

The panel needs UEVR with plugin interface 2.39 or later. With an older UEVR the
plugin works as before, and the panel stays empty.

## The configuration file explains every setting

Each setting in [mw5_motion.cfg](mw5_motion.cfg) has a comment that states what a
higher and a lower value does. These settings matter most:

| Setting | Higher value | Lower value |
| --- | --- | --- |
| `HeaveComfortG` | Running bounces harder | Running is softer, and the cap starts at a lower speed |
| `HeaveGain` | All vertical motion is stronger | All vertical motion is weaker |
| `HeaveLimitG` | Jumps and landings hit harder | Jumps and landings are softer |
| `...SmoothingHz` | Motion reacts faster with more jitter | Motion is smoother but slower |

A `[Chassis]` block at the end of the file changes motion settings for one
chassis:

```ini
[Phoenixhawk]
HeaveComfortG = 2.5
```

## The comfort cap limits the running bounce of every mech

Fast mechs with a tall stride bounce the cockpit much harder than slow mechs. At
top speed a Mauler sends about 1.4 G, and a Phoenix Hawk sends about 4.3 G. The
plugin calculates the bounce of each mech and scales the vertical force, so that
no mech exceeds `HeaveComfortG`.

- The cap **follows the current speed**. The bounce is unchanged at low speed
  and is held at the comfort level above the speed where the mech reaches it.
- The cap **works per chassis**. All variants share the animations of their
  chassis and differ only in speed, which is measured during play.
- The cap **does not limit jumps and landings**. It fades out while the jump
  jets are in use.
- A **safety net** slowly reduces any bounce that stays above 1.25 times the
  comfort level. It covers mechs without data and limping mechs.

Bounce data for 103 chassis is built in. For a modded chassis, set
`GaitLearning = 1`, and the plugin measures the bounce while you walk and run.
You can also add a row to `mw5_mech_gait.csv` in the UEVR profile folder, in the
format of `data/mech_gait.csv`. Rows in that file replace the built-in data.

## The status file shows whether the plugin works

While the game runs, the plugin writes `mw5_motion_status.json` to the UEVR
profile folder once per second. The value of `"reader"` is `ok` while you are in
a mech, and `"vehicleName"` and `"chassis"` show the mech that was found.

`DebugLog = 1` records a whole session to two files in the profile folder:

| File | Contents |
| --- | --- |
| `mw5_motion_debug.csv` | The motion values and the values that were sent to SimHub, 20 rows per second |
| `mw5_motion_debug.log` | Events with a time stamp, such as the mech found, MASC changes and motion resets |

## Building requires Visual Studio 2022

Install Visual Studio 2022 with the C++ and CMake components, and run this
command. It builds the plugin, runs the unit tests and fills `dist\`. The script
`Package.ps1` also writes the release zip file to `release\`.

```powershell
.\Build.ps1
```

## A changed SimHub definition needs a rebuild

The file `simhub\MechWarrior5.simdef` defines the packet. After you edit its
field list, run these commands with Windows PowerShell 5.1. The folder must
contain `SimHub.Plugins.dll` and its dependencies.

```powershell
powershell.exe -File tools\Generate-SimHub.ps1 -SimHubDirectory <folder>
.\Build.ps1
```

Install the rebuilt DLL and the `.simdef` together, because a changed definition
has a new telemetry signature.

## The source is split by task

| Path | Contents |
| --- | --- |
| `src/Telemetry.*` | Motion calculations and the comfort cap |
| `src/Gait.*`, `src/GaitTable.hpp`, `data/mech_gait.csv` | Bounce data per chassis and the optional measuring |
| `src/SimHub.hpp`, `src/SimHubGenerated.hpp` | Effects, vehicle names and the SimHub packet |
| `src/ShakerBuffer.hpp` | The event buffer of MechWarriorVR, which supplies the power events |
| `src/Panel.hpp`, `scripts/mw5_motion.lua` | The settings panel: the settings that can change during play, and the script that draws the panel |
| `src/Plugin.cpp` | The UEVR plugin: game reader, configuration, sender and debug output |
| `simhub/` | The SimHub definition and the four starting motion profiles |
| `tests/` | Unit tests |
| `tools/` | Scripts that rebuild the packet layout and the bounce table |
| `vendor/uevr/` | UEVR plugin SDK headers |

## Credits

- This plugin was inspired by
  [galactic-racer-uevr-motion](https://github.com/elliotttate/galactic-racer-uevr-motion)
  by elliotttate, and parts of it were adapted from that project. The game
  reader, the motion calculations and the SimHub definition are specific to
  MechWarrior 5.
- The starting SimHub profiles in `simhub\profiles` are based on the great work
  of [@LimitlessHorizonVRSimrigs](https://www.youtube.com/@LimitlessHorizonVRSimrigs).
- [UEVR](https://github.com/praydog/UEVR) was made by praydog.
- The [MechWarriorVR](https://www.nexusmods.com/mechwarrior5mercenaries/mods/1009)
  mod and [MW5-UEVR-Plugins](https://github.com/sicsix/MW5-UEVR-Plugins) were made
  by sicsix, perq and myself (psycho063). This plugin follows the cockpit component of that mod.

## Licence

The plugin is released under the MIT licence, which is in the [LICENSE](LICENSE)
file. The UEVR headers in `vendor/uevr/` keep their own MIT notices.
