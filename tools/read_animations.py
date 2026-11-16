# Reads bone curves from MW5 animation sequences and writes them to CSV.
# Runs inside the MW5 mod editor (UE 4.26, Python scripting plugin):
#
#   UE4Editor-Cmd.exe <MW5Mercs.uproject> -run=pythonscript -script=<this file>
#
# Input and output come from environment variables:
#   MW5_ANIM_LIST  text file, one asset path per line, for example
#                  /Game/Objects/Mechs/Dragon/Animation/DRG_FP_RunForward_Straight_ANI
#   MW5_ANIM_OUT   folder for frames.csv (one row per asset path, bone and frame),
#                  sources.csv (which file each asset was loaded from) and
#                  animations.log
#
# It only loads assets and reads them; nothing in the project is changed.
import os
import unreal

BONES = ["Root", "Pelvis", "Torso_Animation", "Torso_Twist", "Torso_Pitch"]

out_dir = os.environ["MW5_ANIM_OUT"]
paths = [p.strip() for p in open(os.environ["MW5_ANIM_LIST"]) if p.strip()]
lib = unreal.AnimationLibrary

log = open(os.path.join(out_dir, "animations.log"), "w")
rows = open(os.path.join(out_dir, "frames.csv"), "w")
rows.write("Asset,Bone,Frame,Frames,LengthSeconds,X,Y,Z,QX,QY,QZ,QW\n")
sources = open(os.path.join(out_dir, "sources.csv"), "w")
sources.write("Asset,Origin,File\n")


def note(text):
    log.write(text + "\n")
    log.flush()
    unreal.log("[MW5 anim] " + text)


for path in paths:
    anim = unreal.load_asset(path)
    if anim is None or not isinstance(anim, unreal.AnimSequence):
        note("MISSING %s" % path)
        continue
    name = path.rsplit("/", 1)[-1]
    frames = lib.get_num_frames(anim)
    length = lib.get_sequence_length(anim)
    # The file the editor really loaded: the game's original or a mod override.
    try:
        source = unreal.SystemLibrary.get_system_path(anim)
    except Exception as error:
        source = "unknown (%s)" % error
    sources.write("%s,%s,%s\n" % (path, "Mod" if "/ModOverride/" in source else "Game", source))
    tracks = [str(t) for t in lib.get_animation_track_names(anim)]
    note("%s: %d frames, %.4f s, %d tracks, loaded from %s" % (name, frames, length, len(tracks), source))
    note("  torso/pelvis tracks: %s" % ", ".join(t for t in tracks if "orso" in t or "elvis" in t or t == "Root" or "ockpit" in t))
    for bone in BONES:
        if bone not in tracks:
            note("  no track for %s" % bone)
            continue
        for frame in range(frames):
            pose = lib.get_bone_pose_for_frame(anim, bone, frame, False)
            t, q = pose.translation, pose.rotation
            rows.write("%s,%s,%d,%d,%.5f,%.5f,%.5f,%.5f,%.6f,%.6f,%.6f,%.6f\n" % (
                path, bone, frame, frames, length, t.x, t.y, t.z, q.x, q.y, q.z, q.w))

rows.close()
sources.close()
note("done: %d assets requested" % len(paths))
log.close()
