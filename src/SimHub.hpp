#pragma once
#include "Telemetry.hpp"
#include "SimHubGenerated.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace mw5 {
// SimHub's stock effects read standard fields only. A mech has no gearbox, ABS or
// engine RPM, so those fields carry MASC and jump jet cues.
struct HapticSettings {
    bool gear{true}, engine{true}, abs{true};
    // SimHub offers RPM effects only with a max RPM. Jump jets report a fixed
    // fraction of it while firing and a lower one during warm-up.
    double engineMaxRpm{10000}, jetLevel{0.8}, jetWarmupLevel{0.4};
    // MASC is an engine vibration too, but only as a warning: nothing while the
    // gauge is below the game's warning point, so normal movement is undisturbed.
    // On a platform SimHub's engine effect is a fast, smooth buzz at high RPM and
    // slow, rough shakes at low RPM, so the vibration starts at mascRpmStart at
    // the warning point and falls to mascRpmDamage at the damage point: rougher
    // the closer it is to damage. Jump jets replace it while they run.
    bool mascEngine{true};
    double mascRpmStart{0.9}, mascRpmDamage{0.5};
    // ABS is the MASC warning: short taps, one SimHub ABS pulse each, at absMinHz
    // from the game's warning point rising to absMaxHz at the damage point and
    // staying there. The flag is never held: SimHub would then repeat its pulse at
    // its own top rate (about 15/s) for as long as it stayed set.
    double absMinHz{2}, absMaxHz{6};
    double absTapSeconds{0.03}; // long enough for SimHub to sample each tap
    // The mech powering up or down is an engine vibration as well: a ramp that
    // climbs from a rough low RPM to a smooth high one on start-up, and winds down
    // the other way on shutdown. The phases arrive as MechShaker "Powering" events
    // (MechWarriorVR with the MechShakerRelay mod); without them nothing plays.
    bool powerEngine{true};
    double powerUpStart{0.45}, powerUpEnd{0.85}, powerDownStart{0.85}, powerDownEnd{0.45};
    double powerInitSeconds{5}, powerUpSeconds{4}, powerDownSeconds{3};
    // A part of the mech being destroyed is a kick in the motion and a rough burst
    // of engine vibration. The kick is a short lean of the roll angle towards
    // the side that was lost, because platforms follow the angles; legs and side
    // torsos also drop in heave, and the head tips the nose up. A mech's roll
    // stays within about 3 degrees while it runs and its pitch within about 12,
    // so the roll kick is small and the pitch kick larger: partRollDeg and
    // partPitchDeg are the peaks of a kick with factor 1 at strength 1,
    // partDropMs2 that of the drop. No kick exceeds the three limits whatever
    // the settings. The parts arrive as MechShaker "PartDestruction" events.
    bool parts{true}, partVibration{true};
    double partStrength{1}, partArm{0.4}, partLeg{0.6}, partSideTorso{0.8}, partCore{1};
    double partRollDeg{4}, partRollLimitDeg{8}, partPitchDeg{15}, partPitchLimitDeg{25};
    double partDropMs2{6}, partDropLimitMs2{25};
    // A side torso is mostly a tilt forwards or backwards, away from the blast: a
    // hit from the front tips the nose up, a hit from the rear tips it down.
    // partTorsoPitch is its share of the pitch kick. The lean to the side is
    // much smaller, partTorsoRoll of the roll kick: towards the lost side for a
    // hit from the front, away from it for a hit from the rear.
    double partTorsoPitch{0.5}, partTorsoRoll{0.35};
    // Footsteps are a thump through SimHub's Suspension effect, which has a wheel
    // at each corner: the left wheels for the left foot, the right wheels for the
    // right. The strength is a share of stompMps (a suspension velocity) by the
    // weight class of the mech; no thump exceeds stompLimitMps. The steps arrive
    // as MechShaker "Footstep" events.
    bool stomps{true};
    double stompLight{0.25}, stompMedium{0.5}, stompHeavy{0.75}, stompAssault{1};
    double stompMps{0.5}, stompLimitMps{1};
};
// The phase a MechShaker "Powering" event carries in its first value.
enum PowerPhase : int { PowerInitialising=0, PowerUp=1, PowerDown=2 };
// The part a MechShaker "PartDestruction" event carries in its first value.
enum Part : int { PartHead=0, PartCenterTorso=1, PartLeftTorso=2, PartLeftArm=3, PartLeftLeg=4, PartRightTorso=5, PartRightArm=6, PartRightLeg=7 };
// A kick: a shove that eases out to its peak in 0.3 s, holds briefly and eases
// back to rest over `back` seconds without swinging past it, as a mech's punch
// does. Gentle enough in its rate for a platform to follow without a jerk.
constexpr double kKickRise=0.3, kKickHold=0.15, kKickBack=0.75, kKickBackLeg=1.5;
inline double kick_shape(double t,double back=kKickBack) {
    if(t<0||t>=kKickRise+kKickHold+back)return 0;
    if(t<kKickRise)return 0.5*(1-std::cos(pi*t/kKickRise));
    if(t<kKickRise+kKickHold)return 1;
    return 0.5*(1+std::cos(pi*(t-kKickRise-kKickHold)/back));
}
// How a mech died, for the vibration that goes with it.
enum Death : int {
    DeathExplosion=0, // cored, or the engine went with a side torso: a long rough rumble
    DeathEjection=1,  // the pilot left a mech that is still in one piece: a short, lighter one
};
// The game's EMechCauseOfDeath flags.
constexpr int kCauseCored=0x02, kCauseLegged=0x04, kCauseNoPilot=0x08;
struct HapticState {
    bool mascGear{}, absActive{};
    double rpm{}, phase{};
    int powerPhase{-1};       // PowerPhase while a ramp plays, otherwise -1
    double powerElapsed{};
    // Part destruction: the peak of the kick in roll and pitch (degrees; roll
    // right side down, pitch nose up) and in heave (m/s^2, up), and the
    // vibration burst that goes with it.
    double kickRoll{}, kickPitch{}, kickHeave{}, kickBack{kKickBack}, kickElapsed{-1};
    double burstFrom{}, burstTo{}, burstSeconds{}, burstElapsed{-1};
    // Effects started from the test buttons of the settings panel, seconds left.
    double testGear{}, testAbs{}, testJets{}, testMasc{};
    // Footsteps, through the Suspension effect. Front left, front right, rear
    // left, rear right wheel: seconds since the thump started (-1 when idle), its
    // size and the suspension velocity reported, both m/s.
    double thump[4]{-1,-1,-1,-1},thumpSize[4]{};float suspension[4]{};
    static constexpr double kThumpSeconds=0.5;
    // One foot coming down, `strength` a share of the full thump.
    void stomp(bool left,double strength,const HapticSettings& cfg) {
        if(!cfg.stomps)return;
        double size=std::clamp(cfg.stompMps*strength,0.0,cfg.stompLimitMps);
        for(int wheel:{left?0:1,left?2:3}){thump[wheel]=0;thumpSize[wheel]=size;}
    }
    static constexpr double kTestMascSeconds=3;
    // A "Powering" event arrived: start that ramp, replacing any still playing.
    void power_event(int phaseCode) {
        if(phaseCode<PowerInitialising||phaseCode>PowerDown)return;
        powerPhase=phaseCode;powerElapsed=0;
    }
    // Starts a kick with a vibration burst from rpmFrom to rpmTo (shares of the
    // max RPM). Parts that go within a fifth of a second of each other, such as a
    // side torso and its arm, are one hit: only the strongest plays.
    void impact(double roll,double pitch,double heave,double rpmFrom,double rpmTo,double seconds,double back=kKickBack) {
        auto size=[](double a,double b,double c){return std::max({std::abs(a),std::abs(b),std::abs(c)});};
        if(kickElapsed>=0&&kickElapsed<0.2&&size(roll,pitch,heave)<=size(kickRoll,kickPitch,kickHeave))return;
        kickRoll=roll;kickPitch=pitch;kickHeave=heave;kickBack=back;kickElapsed=0;
        burstFrom=rpmFrom;burstTo=rpmTo;burstSeconds=seconds;burstElapsed=0;
    }
    // A "PartDestruction" event. `wreck` is for parts that break off after the
    // mech was destroyed: a light rumble only. The centre torso has no kick of
    // its own: losing it is the mech being destroyed, see mech_destroyed().
    // fromRear: the last armour hit on a side torso came from behind, so its
    // kick tips the nose down where a hit from the front tips it up.
    void part_destroyed(int part,const HapticSettings& cfg,bool wreck=false,bool fromRear=false) {
        if(!cfg.parts||part<PartHead||part>PartRightLeg)return;
        if(wreck){burstFrom=burstTo=0.4;burstSeconds=0.3;burstElapsed=0;return;}
        if(part==PartCenterTorso)return;
        // Towards the side that was lost: left side down for a left part.
        double side=part==PartLeftTorso||part==PartLeftArm||part==PartLeftLeg?-1:part==PartHead?0:1;
        // jolt: the share of the pitch kick, nose up as if knocked back. sway: the
        // share of the roll kick.
        double factor{},drop{},jolt{},sway=1,level{},seconds{},back=kKickBack;
        switch(part) {
        case PartLeftArm:case PartRightArm:factor=cfg.partArm;level=0.35;seconds=0.3;break;
        case PartLeftLeg:case PartRightLeg:factor=cfg.partLeg;drop=-1;level=0.3;seconds=0.5;back=kKickBackLeg;break; // the mech sags and recovers slowly
        case PartLeftTorso:case PartRightTorso:
            factor=cfg.partSideTorso;jolt=fromRear?-cfg.partTorsoPitch:cfg.partTorsoPitch;sway=fromRear?-cfg.partTorsoRoll:cfg.partTorsoRoll;
            drop=-1;level=0.25;seconds=0.8;break;
        default:factor=cfg.partCore;jolt=1;level=0.2;seconds=0.6;break; // head: knocked back, nose up
        }
        double lean=std::clamp(cfg.partRollDeg*cfg.partStrength*factor,0.0,cfg.partRollLimitDeg);
        double tip=std::clamp(cfg.partPitchDeg*cfg.partStrength*factor,0.0,cfg.partPitchLimitDeg);
        double fall=std::clamp(cfg.partDropMs2*cfg.partStrength*factor,0.0,cfg.partDropLimitMs2);
        impact(lean*side*sway,tip*jolt,fall*drop,level,level,seconds,back);
    }
    // The mech is destroyed. Vibration only: the fall of the mech is already in
    // the motion. It replaces the shutdown ramp the game also reports.
    void mech_destroyed(const HapticSettings& cfg,int kind=DeathExplosion) {
        if(!cfg.parts)return;
        if(kind==DeathEjection){burstFrom=0.5;burstTo=0.3;burstSeconds=1;}
        else {burstFrom=0.3;burstTo=0.08;burstSeconds=3;}
        burstElapsed=0;powerPhase=-1;
    }
    // Adds the kick to a frame that is about to be sent. Call after update().
    void apply_kick(Frame& f) const {
        if(!f.OutputActive||kickElapsed<0)return;
        double s=kick_shape(kickElapsed,kickBack);
        f.RollDegrees+=static_cast<float>(kickRoll*s);
        f.PitchDegrees+=static_cast<float>(kickPitch*s);
        f.LocalAccelerationY+=static_cast<float>(kickHeave*s);
    }
    // Call once per new frame, on the thread that builds the packets.
    void update(const Frame& f,const HapticSettings& cfg) {
        if(!f.OutputActive) {
            // The gear holds through pauses and discontinuities: no shift pulse on resume.
            // A power ramp or a kick waits through a pause and is dropped when the mech is gone.
            if(!f.FrameValid){mascGear=false;powerPhase=-1;kickElapsed=burstElapsed=-1;}
            testGear=testAbs=testJets=testMasc=0;
            for(int i=0;i<4;++i){thump[i]=-1;suspension[i]=0;}
            absActive=false;rpm=phase=0;return;
        }
        auto running=[&](double& left){bool on=left>0;left=std::max(0.0,left-f.DeltaTime);return on;};
        double mascTestLeft=testMasc;
        bool gearTest=running(testGear),absTest=running(testAbs),jetTest=running(testJets),mascTest=running(testMasc);
        mascGear=(f.MascEngaged!=0)!=gearTest;
        // A thump is a suspension velocity that jumps up and dies away.
        for(int i=0;i<4;++i) {
            suspension[i]=thump[i]<0?0:static_cast<float>(thumpSize[i]*std::exp(-thump[i]/0.1));
            if(thump[i]>=0&&(thump[i]+=f.DeltaTime)>=kThumpSeconds)thump[i]=-1;
        }
        double ramp=0;
        if(powerPhase>=0) {
            double length=powerPhase==PowerInitialising?cfg.powerInitSeconds:powerPhase==PowerUp?cfg.powerUpSeconds:cfg.powerDownSeconds;
            if(length<=0||powerElapsed>=length)powerPhase=-1;
            else {
                double from=powerPhase==PowerDown?cfg.powerDownStart:cfg.powerUpStart,to=powerPhase==PowerDown?cfg.powerDownEnd:cfg.powerUpEnd;
                ramp=cfg.engineMaxRpm*(from+(to-from)*powerElapsed/length);
                powerElapsed+=f.DeltaTime;
            }
        }
        double burst=-1;
        if(burstElapsed>=0) {
            if(burstSeconds<=0||burstElapsed>=burstSeconds)burstElapsed=-1;
            else {burst=cfg.engineMaxRpm*(burstFrom+(burstTo-burstFrom)*burstElapsed/burstSeconds);burstElapsed+=f.DeltaTime;}
        }
        // MASC vibrates only from the warning point on. A fitting that reports no
        // warning point vibrates all the time it runs, as there is nothing to wait for.
        bool mascKnown=f.MascWarningPoint>0;
        bool mascRuns=cfg.mascEngine&&f.MascEngaged&&(!mascKnown||f.MascGauge>=f.MascWarningPoint);
        // A part being destroyed outranks everything; then jump jets, MASC and the power ramp.
        if(burst>=0&&cfg.partVibration)rpm=burst;
        else if(f.JumpJetState==JetsActive||jetTest)rpm=cfg.engineMaxRpm*cfg.jetLevel;
        else if(f.JumpJetState==JetsWarmup)rpm=cfg.engineMaxRpm*cfg.jetWarmupLevel;
        else if(mascRuns||mascTest) {
            double range=f.MascDamagePoint-f.MascWarningPoint;
            double fill=mascTest?1-mascTestLeft/kTestMascSeconds:mascKnown?(range>0?(f.MascGauge-f.MascWarningPoint)/range:1):
                f.MascDamagePoint>0?f.MascGauge/f.MascDamagePoint:0;
            rpm=cfg.engineMaxRpm*(cfg.mascRpmStart+(cfg.mascRpmDamage-cfg.mascRpmStart)*std::clamp(fill,0.0,1.0));
        }
        else rpm=cfg.powerEngine?ramp:0;
        bool warned=f.MascInstalled&&f.MascEngaged&&f.MascWarningPoint>0&&f.MascGauge>=f.MascWarningPoint;
        if(!warned&&!absTest){absActive=false;phase=0;return;}
        double span=f.MascDamagePoint-f.MascWarningPoint;
        // A test taps at the middle rate.
        double level=!warned?0.5:span>0?std::min(1.0,(f.MascGauge-f.MascWarningPoint)/span):1;
        double hz=cfg.absMinHz+(cfg.absMaxHz-cfg.absMinHz)*level;
        absActive=phase<std::min(0.5,hz*cfg.absTapSeconds);
        phase=std::fmod(phase+f.DeltaTime*hz,1.0);
    }
    // Moves the kick on by one frame. Call after apply_kick(), so the first frame
    // of a kick starts at its beginning.
    void advance_kick(const Frame& f) {
        if(f.OutputActive&&kickElapsed>=0&&(kickElapsed+=f.DeltaTime)>=kKickRise+kKickHold+kickBack)kickElapsed=-1;
    }
};

// Which mech is being piloted, and the SimHub vehicle name chosen for it.
struct Identity {
    std::string weightClass, chassis, variant, profile;
    float baseMaxSpeedKmh{};
};
// The game tags a mech as "Mech.<WeightClass>.<Chassis>.<Variant>".
inline Identity parse_mech_tag(const std::string& tag) {
    std::vector<std::string> parts;size_t start=0;
    while(start<=tag.size()){auto dot=tag.find('.',start);if(dot==tag.npos)dot=tag.size();parts.push_back(tag.substr(start,dot-start));start=dot+1;}
    Identity id;
    if(parts.size()>=3&&parts[0]=="Mech"){id.weightClass=parts[1];id.chassis=parts[2];}
    // Variants can contain dots of their own; keep everything after the chassis.
    for(size_t i=3;i<parts.size();++i)id.variant+=(i>3?".":"")+parts[i];
    return id;
}
inline bool same_text(const std::string& a,const std::string& b) {
    return a.size()==b.size()&&std::equal(a.begin(),a.end(),b.begin(),[](unsigned char x,unsigned char y){return std::tolower(x)==std::tolower(y);});
}
// How hard a mech of this weight class stomps, as a share of the full thump.
inline double stomp_strength(const HapticSettings& cfg,const std::string& weightClass) {
    return same_text(weightClass,"Light")?cfg.stompLight:same_text(weightClass,"Heavy")?cfg.stompHeavy:
           same_text(weightClass,"Assault")?cfg.stompAssault:cfg.stompMedium;
}
struct ProfileSettings {
    std::array<double,3> limitsKmh{60,75,100};   // upper limits of Slow, Standard and Fast
    std::vector<std::string> specificChassis;    // these get a profile of their own
};
// The vehicle name SimHub uses to pick a motion profile: one of four speed bands,
// or the chassis itself when it is listed. baseMaxSpeedKmh excludes the MASC boost.
inline std::string profile_name(const std::string& chassis,double baseMaxSpeedKmh,const ProfileSettings& cfg) {
    for(auto& s:cfg.specificChassis)if(!chassis.empty()&&same_text(s,chassis))return "MW5 "+chassis;
    // Whole km/h, so a tabletop speed such as 97.2 lands on the same side as its limit reads.
    double speed=std::floor(baseMaxSpeedKmh+0.5);
    return speed<=cfg.limitsKmh[0]?"MW5 Slow":speed<=cfg.limitsKmh[1]?"MW5 Standard":speed<=cfg.limitsKmh[2]?"MW5 Fast":"MW5 Very Fast";
}

template<size_t N> void copy_text(char (&to)[N],const std::string& from) {
    std::memset(to,0,N);std::memcpy(to,from.data(),std::min(from.size(),N-1));
}
inline SimHubExternalTelemetrySender::SHTelemetryPacket external_packet(
    const Frame& f,uint64_t emitter,uint64_t session,uint64_t counter,double seconds,
    const HapticState& haptics={},const HapticSettings& cfg={},const Identity& id={}) {
    using namespace SimHubExternalTelemetrySender;
    SHTelemetryPacket p{};
    p.GameSignature=SHTelemetryConstants::GameSignature;
    p.TelemetrySignature=SHTelemetryConstants::TelemetrySignature;
    p.LayoutMajorVersion=SHTelemetryConstants::LayoutMajorVersion;p.LayoutMinorVersion=SHTelemetryConstants::LayoutMinorVersion;
    p.EmitterInstanceId=emitter;p.PacketId=0;p.PacketsCounter=counter;
    p.IsSessionRunning=f.FrameValid!=0;p.IsSessionPaused=f.Paused||f.Stale;
    p.SessionId=session;p.IsUserInControl=f.OutputActive!=0;
    p.SessionTimeSeconds=seconds;p.PhysicsDiscontinuityCounter=f.PhysicsDiscontinuityCounter;
    // SimHub counts pitch positive nose-down; the frame counts it positive nose-up.
    constexpr float deg=static_cast<float>(180/pi);
    p.YawDegrees=f.YawDegrees;p.PitchDegrees=-f.PitchDegrees;p.RollDegrees=f.RollDegrees;
    p.YawRateDegreesPerSecond=f.YawSpeed*deg;p.PitchRateDegreesPerSecond=-f.PitchSpeed*deg;p.RollRateDegreesPerSecond=f.RollSpeed*deg;
    p.LocalSurgeMs2=f.LocalAccelerationZ;p.LocalSwayMs2=f.LocalAccelerationX;p.LocalHeaveMs2=f.LocalAccelerationY;
    p.LocalVelocityForwardMps=f.LocalVelocityZ;p.LocalVelocityLateralMps=f.LocalVelocityX;p.LocalVelocityUpMps=f.LocalVelocityY;
    p.VehiclePositionNorth=f.PositionX;p.VehiclePositionEast=f.PositionY;p.VehiclePositionUp=f.PositionZ;
    p.SpeedKmh=f.SpeedKmh;p.GroundSpeedKmh=std::hypot(f.LocalVelocityX,f.LocalVelocityZ)*3.6f;
    copy_text(p.VehicleName,id.profile);copy_text(p.VehicleId,id.profile);
    // Engine Vibrations follows EngineRpm: silent unless the jump jets run.
    p.EngineMaxRpm=static_cast<float>(cfg.engineMaxRpm);
    p.EngineRpm=cfg.engine?static_cast<float>(haptics.rpm):0;
    p.Throttle=f.Throttle;
    // Gear Change fires on any gear change: 1 -> 2 when MASC engages, 2 -> 1 when it stops.
    p.Gear[0]=cfg.gear&&haptics.mascGear?'2':'1';p.MaxGears=2;
    p.AbsActive=cfg.abs&&haptics.absActive;
    // Suspension: the footsteps.
    p.SuspensionVelocityFrontLeftMps=haptics.suspension[0];p.SuspensionVelocityFrontRightMps=haptics.suspension[1];
    p.SuspensionVelocityRearLeftMps=haptics.suspension[2];p.SuspensionVelocityRearRightMps=haptics.suspension[3];
    p.MascInstalled=static_cast<uint8_t>(f.MascInstalled);p.MascEngaged=static_cast<uint8_t>(f.MascEngaged);
    p.MascStarted=static_cast<uint8_t>(f.MascStarted);p.MascEnded=static_cast<uint8_t>(f.MascEnded);
    p.MascType=static_cast<uint8_t>(f.MascType);
    p.MascGauge=f.MascGauge;p.MascWarningPoint=f.MascWarningPoint;p.MascDamagePoint=f.MascDamagePoint;
    p.JumpJetState=static_cast<uint8_t>(f.JumpJetState);
    p.JumpStarted=static_cast<uint8_t>(f.JumpStarted);p.JumpEnded=static_cast<uint8_t>(f.JumpEnded);
    p.HeatPercent=f.HeatPercent;p.GameSpeedKmh=f.GameSpeedKmh;p.MaxSpeedKmh=f.MaxSpeedKmh;
    p.BaseMaxSpeedKmh=id.baseMaxSpeedKmh;
    copy_text(p.Chassis,id.chassis);copy_text(p.Variant,id.variant);
    return p;
}
}
