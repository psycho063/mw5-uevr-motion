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
    // MASC running is an engine vibration too. On a platform SimHub's engine effect
    // is a fast, smooth buzz at high RPM and slow, rough shakes at low RPM, so MASC
    // starts high (empty gauge) and falls to mascRpmDamage at the damage point:
    // rougher the closer it is to damage. Jump jets replace it while they run.
    bool mascEngine{true};
    double mascRpmStart{0.9}, mascRpmDamage{0.5};
    // ABS is the MASC warning: short taps, one SimHub ABS pulse each, at absMinHz
    // from the game's warning point rising to absMaxHz at the damage point and
    // staying there. The flag is never held: SimHub would then repeat its pulse at
    // its own top rate (about 15/s) for as long as it stayed set.
    double absMinHz{2}, absMaxHz{6};
    double absTapSeconds{0.03}; // long enough for SimHub to sample each tap
};
struct HapticState {
    bool mascGear{}, absActive{};
    double rpm{}, phase{};
    // Call once per new frame, on the thread that builds the packets.
    void update(const Frame& f,const HapticSettings& cfg) {
        if(!f.OutputActive) {
            // The gear holds through pauses and discontinuities: no shift pulse on resume.
            if(!f.FrameValid)mascGear=false;
            absActive=false;rpm=phase=0;return;
        }
        mascGear=f.MascEngaged!=0;
        if(f.JumpJetState==JetsActive)rpm=cfg.engineMaxRpm*cfg.jetLevel;
        else if(f.JumpJetState==JetsWarmup)rpm=cfg.engineMaxRpm*cfg.jetWarmupLevel;
        else if(cfg.mascEngine&&f.MascEngaged) {
            double fill=f.MascDamagePoint>0?std::clamp(f.MascGauge/f.MascDamagePoint,0.0f,1.0f):0;
            rpm=cfg.engineMaxRpm*(cfg.mascRpmStart+(cfg.mascRpmDamage-cfg.mascRpmStart)*fill);
        }
        else rpm=0;
        bool warned=f.MascInstalled&&f.MascEngaged&&f.MascWarningPoint>0&&f.MascGauge>=f.MascWarningPoint;
        if(!warned){absActive=false;phase=0;return;}
        double span=f.MascDamagePoint-f.MascWarningPoint;
        double level=span>0?std::min(1.0,(f.MascGauge-f.MascWarningPoint)/span):1;
        double hz=cfg.absMinHz+(cfg.absMaxHz-cfg.absMinHz)*level;
        absActive=phase<std::min(0.5,hz*cfg.absTapSeconds);
        phase=std::fmod(phase+f.DeltaTime*hz,1.0);
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
