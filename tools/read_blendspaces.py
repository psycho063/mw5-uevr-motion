# Reads the first-person locomotion blend spaces of MW5 mechs: at which speed the
# game plays the walk cycle and the run cycle. Between the two it cross-fades.
# Runs inside the MW5 mod editor (UE 4.26), like read_animations.py:
#
#   UE4Editor-Cmd.exe <MW5Mercs.uproject> -run=pythonscript -script=<this file>
#
#   MW5_ANIM_LIST  text file, one blend space asset path per line, for example
#                  /Game/Objects/Mechs/Dragon/Animation/DRG_FP_Locomotion_BLS
#   MW5_ANIM_OUT   folder for blendspaces.csv and blendspace_samples.csv
#
# Read-only: nothing in the project is changed.
import os
import unreal

out_dir = os.environ["MW5_ANIM_OUT"]
paths = [p.strip() for p in open(os.environ["MW5_ANIM_LIST"]) if p.strip()]

summary = open(os.path.join(out_dir, "blendspaces.csv"), "w")
summary.write("Asset,Origin,SpeedAxis,WalkSpeedCms,RunSpeedCms,ScalesPlaybackAlong,InputSmoothing,Samples\n")
samples = open(os.path.join(out_dir, "blendspace_samples.csv"), "w")
samples.write("Asset,Animation,Turn,Speed,RateScale\n")

for path in paths:
    space = unreal.load_asset(path)
    if space is None:
        summary.write("%s,MISSING,,,,,,\n" % path)
        continue
    try:
        source = unreal.SystemLibrary.get_system_path(space)
    except Exception:
        source = ""
    axes = space.get_editor_property("blend_parameters")
    speed_axis = str(axes[1].get_editor_property("display_name"))
    walk = run = ""
    count = 0
    for sample in space.get_editor_property("sample_data"):
        anim = sample.get_editor_property("animation")
        name = anim.get_name() if anim else "None"
        value = sample.get_editor_property("sample_value")
        samples.write("%s,%s,%.1f,%.1f,%.3f\n" % (path, name, value.x, value.y, sample.get_editor_property("rate_scale")))
        count += 1
        # The straight cycles sit at turn 0; that is the column the mech runs on.
        if abs(value.x) < 0.5 and name.endswith("_FP_WalkForward_Straight_ANI"):
            walk = "%.0f" % value.y
        if abs(value.x) < 0.5 and name.endswith("_FP_RunForward_Straight_ANI"):
            run = "%.0f" % value.y
    # Playback is only rescaled when the input is smoothed; record both settings.
    smoothing = max(p.get_editor_property("interpolation_time") for p in space.get_editor_property("interpolation_param"))
    summary.write("%s,%s,%s,%s,%s,%s,%.3f,%d\n" % (
        path, "Mod" if "/ModOverride/" in source else "Game", speed_axis, walk, run,
        str(space.get_editor_property("axis_to_scale_animation")).split(".")[-1].split(":")[0], smoothing, count))

summary.close()
samples.close()
