# Turns the CSV files from read_animations.py and read_blendspaces.py into one
# row per chassis, ranked by the heave the plugin would send at top speed.
#
#   awk -F, -f summarise_animations.awk mech_chassis.csv blendspaces.csv sources.csv frames.csv > animation_bounce.csv
#
# mech_chassis.csv   tonnage and top speed per chassis (from extract_mech_stats.ps1)
# blendspaces.csv    speed at which the game plays the walk and the run cycle
# sources.csv        whether each animation came from the active mod or the game
# frames.csv         bone curves per frame
#
# Vertical movement is the Z value of the Pelvis track, in centimetres. Only the
# travel within a cycle matters for motion; the absolute height does not.
#
# The chassis is the folder above the animation folder; clan mechs sit one level
# deeper (Mechs/CLANS/<Chassis>/Animations), so count from the file name.
function chassis_of(path,   parts, n) {
    n = split(path, parts, "/")
    return n > 2 ? parts[n - 2] : path
}
# Heave in m/s^2 RMS that reaches SimHub from a mech moving at `kmh`, before any
# per-mech factor. The game cross-fades the walk and run cycles by speed; the
# cockpit travels about 0.75 of the pelvis travel; the plugin's three 2 Hz
# low-pass stages then remove part of it. See docs: ANIMATION_PLAN.md.
function heave(kmh, c,   v, blend, cycle, rate, bounce, loss) {
    v = kmh / 0.036
    blend = (v - walkSpeed[c]) / (runSpeed[c] - walkSpeed[c])
    if (blend < 0) blend = 0; if (blend > 1) blend = 1
    cycle = (1 - blend) * walkSeconds[c] + blend * runSeconds[c]
    rate = 2 / cycle
    bounce = ((1 - blend) * walkTravel[c] + blend * runTravel[c]) * 0.75 / 100
    loss = 1 / sqrt(1 + (rate / 2) ^ 2)
    return 0.7071 * (bounce / 2) * (6.283185 * rate) ^ 2 * loss ^ 3
}
{ sub(/\r$/, "") }
FILENAME ~ /mech_chassis/ {
    if (FNR > 1) { c = $1; t = $3; s = $5; gsub(/"/, "", c); gsub(/"/, "", t); gsub(/"/, "", s); tons[tolower(c)] = t; speed[tolower(c)] = s }
    next
}
FILENAME ~ /blendspaces\.csv/ {
    if (FNR > 1) { c = chassis_of($1); walkSpeed[c] = $4; runSpeed[c] = $5 }
    next
}
FILENAME ~ /sources\.csv/ { if (FNR > 1) origin[$1] = $2; next }
FNR > 1 && $2 == "Pelvis" {
    a = $1; frames[a] = $4; seconds[a] = $5; z[a, $3 + 0] = $8
    for (k = 6; k <= 11; k++) {
        if (!((a, k) in low) || $k < low[a, k]) low[a, k] = $k
        if (!((a, k) in high) || $k > high[a, k]) high[a, k] = $k
    }
}
# The turn lean is a constant rotation on one torso bone, so frame 0 is enough.
# It is measured against the same bone in the straight run cycle: some skeletons
# carry a large rest rotation on a torso bone that is not a lean.
FNR > 1 && ($2 == "Torso_Animation" || $2 == "Torso_Twist" || $2 == "Torso_Pitch") && $3 == 0 {
    torso[$1, $2] = $9 "," $10 "," $11 "," $12
}
function lean_of(turn, straight,   bones, b, t, s, dot, degrees) {
    split("Torso_Animation,Torso_Twist,Torso_Pitch", bones, ",")
    for (b = 1; b <= 3; b++) {
        if (!((turn, bones[b]) in torso) || !((straight, bones[b]) in torso)) continue
        split(torso[turn, bones[b]], t, ","); split(torso[straight, bones[b]], s, ",")
        dot = t[1] * s[1] + t[2] * s[2] + t[3] * s[3] + t[4] * s[4]
        if (dot < 0) dot = -dot; if (dot > 1) dot = 1
        degrees = 2 * atan2(sqrt(1 - dot * dot), dot) * 57.29578
        if (degrees > turnLean[chassis_of(turn)]) { turnLean[chassis_of(turn)] = degrees; turnBone[chassis_of(turn)] = bones[b] }
    }
}
END {
    for (a in frames) if (a ~ /RunForward_Straight/) straightRun[chassis_of(a)] = a
    for (a in frames) {
        c = chassis_of(a); n = frames[a] - 1; dt = seconds[a] / n
        peak = 0; squares = 0; steps = 0
        middle = (high[a, 8] + low[a, 8]) / 2
        for (i = 0; i < n; i++) {   # the cycle loops, so wrap around
            p = (i - 1 + n) % n; q = (i + 1) % n
            acc = (z[a, p] - 2 * z[a, i] + z[a, q]) / (dt * dt) / 100
            if (acc > peak) peak = acc; if (-acc > peak) peak = -acc
            squares += acc * acc
            # One step per rise through the middle height; a wobble at the top
            # of a step is not another step.
            if (z[a, p] < middle && z[a, i] >= middle) steps++
        }
        travel = high[a, 8] - low[a, 8]
        if (a ~ /RunForward_Straight/) {
            runTravel[c] = travel; runSeconds[c] = seconds[a]; runSteps[c] = steps; runOrigin[c] = origin[a]
            runRms[c] = sqrt(squares / n); runPeak[c] = peak / 9.80665
            runX[c] = high[a, 6] - low[a, 6]; runY[c] = high[a, 7] - low[a, 7]
            rotates[c] = (high[a, 9] - low[a, 9]) + (high[a, 10] - low[a, 10]) + (high[a, 11] - low[a, 11]) > 0.0005 ? "yes" : "no"
        } else if (a ~ /WalkForward_Straight/) {
            walkTravel[c] = travel; walkSeconds[c] = seconds[a]; walkRms[c] = sqrt(squares / n)
        } else lean_of(a, straightRun[c])
    }
    print "Chassis,Tons,TopSpeedKmh,HeaveAtTopSpeedMs2,HeaveAt150PctSpeedMs2,RunBounceCm,RunCycleS,RunSteps,WalkBounceCm,WalkCycleS,WalkSpeedKmh,RunSpeedKmh,RunRmsAccelMs2,RunPeakG,WalkRmsAccelMs2,RunPelvisRangeXCm,RunPelvisRangeYCm,PelvisRotates,TurnLeanDeg,TurnLeanBone,RunAnimationFrom"
    for (c in runTravel) {
        top = speed[tolower(c)]
        known = top != "" && (c in walkSpeed) && (c in walkTravel) && runSpeed[c] > walkSpeed[c]
        printf "%s,%s,%s,%s,%s,%.1f,%.2f,%d,%.1f,%.2f,%s,%s,%.1f,%.1f,%.1f,%.1f,%.1f,%s,%.2f,%s,%s\n", c, tons[tolower(c)], top,
            known ? sprintf("%.1f", heave(top, c)) : "", known ? sprintf("%.1f", heave(top * 1.5, c)) : "",
            runTravel[c], runSeconds[c], runSteps[c], walkTravel[c], walkSeconds[c],
            (c in walkSpeed) ? sprintf("%.1f", walkSpeed[c] * 0.036) : "", (c in runSpeed) ? sprintf("%.1f", runSpeed[c] * 0.036) : "",
            runRms[c], runPeak[c], walkRms[c], runX[c], runY[c], rotates[c], turnLean[c], turnBone[c], runOrigin[c]
    }
}
