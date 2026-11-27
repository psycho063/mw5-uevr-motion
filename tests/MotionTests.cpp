#include "Telemetry.hpp"
#include "SimHub.hpp"
#include "Gait.hpp"
#include "ShakerBuffer.hpp"
#include "Panel.hpp"
#include <cstddef>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace mw5;
void check(bool b,const char* text){if(!b){std::cerr<<"FAIL: "<<text<<'\n';std::exit(1);}}
bool near(double a,double b,double epsilon=0.001){return std::abs(a-b)<epsilon;}
Sample base(){Sample s{};s.valid=true;s.controlled=true;s.identity=1;s.dt=0.01;return s;}
// No smoothing and no cockpit yaw correction: outputs follow the inputs exactly.
Settings raw(){Settings c{};c.yawOffsetDeg=0;c.positionHz=c.velocityHz=c.accelerationHz=c.angularHz=0;c.eventHoldSeconds=0;c.heaveLimitG=0;c.heaveComfortG=0;c.heaveSafetyNet=false;return c;}
int main(){
    Settings cfg=raw();Sample s=base();Sampler sampler;Frame f{};
    check(!sampler.update(s,cfg).OutputActive,"first sample neutral");

    // Constant velocity: 10 m/s forward, no acceleration.
    sampler.reset();s=base();
    for(int i=0;i<5;++i){s.position={0.1*i,0,0};f=sampler.update(s,cfg);}
    check(f.OutputActive&&near(f.LocalVelocityZ,10)&&near(f.LocalVelocityX,0)&&near(f.LocalVelocityY,0),"forward velocity from position");
    check(near(f.SpeedMps,10)&&near(f.SpeedKmh,36),"speed units");
    check(near(f.LocalAccelerationZ,0)&&near(f.LocalAccelerationX,0)&&near(f.LocalAccelerationY,0),"no acceleration at constant velocity");

    // Constant acceleration of 2 m/s^2 forward and 1 m/s^2 up.
    sampler.reset();s=base();
    for(int i=0;i<6;++i){double t=0.01*i;s.position={t*t,0,0.5*t*t};f=sampler.update(s,cfg);}
    check(near(f.LocalAccelerationZ,2,0.01)&&near(f.LocalAccelerationY,1,0.01)&&near(f.LocalAccelerationX,0,0.01),"surge and heave from position");
    check(near(f.GForceLongitudinal,2/gravity,0.001),"G units");

    // Constant-speed turn to the right: 10 m/s at 1 rad/s is 10 m/s^2 of sway.
    sampler.reset();s=base();
    for(int i=0;i<8;++i){
        double a=0.01*i;s.rotation=Quat::yaw(a);s.position={10*std::sin(a),-10*std::cos(a),0};f=sampler.update(s,cfg);
    }
    check(near(f.LocalAccelerationX,10,0.05)&&near(f.LocalAccelerationZ,0,0.1),"constant-speed cornering sway");
    check(near(f.YawSpeed,1)&&near(f.RollSpeed,0)&&near(f.PitchSpeed,0),"yaw rate from rotation");
    check(near(f.YawDegrees,0.07*180/pi,0.01)&&near(f.YawAcceleration,0,0.01),"yaw angle, steady rate");

    // Roll and pitch rates and signs.
    sampler.reset();s=base();
    for(int i=0;i<4;++i){double a=0.01*i;s.rotation={-std::sin(a/2),0,0,std::cos(a/2)};f=sampler.update(s,cfg);}
    check(near(f.RollSpeed,1)&&near(f.RollDegrees,0.03*180/pi,0.01),"roll right-down positive");
    sampler.reset();s=base();
    for(int i=0;i<4;++i){double a=0.01*i;s.rotation={0,-std::sin(a/2),0,std::cos(a/2)};f=sampler.update(s,cfg);}
    check(near(f.PitchSpeed,1)&&near(f.PitchDegrees,0.03*180/pi,0.01),"pitch nose-up positive");
    sampler.reset();s=base();s.rotation={-std::sin(pi/12),0,0,std::cos(pi/12)};sampler.update(s,cfg);f=sampler.update(s,cfg);
    check(near(f.RollDegrees,30)&&near(f.RollSpeed,0),"static roll angle, no rate");

    // The cockpit socket faces 90 degrees off the mech: the offset turns it back.
    Settings turned=raw();turned.yawOffsetDeg=-90;
    sampler.reset();s=base();s.rotation=Quat::yaw(pi/2);
    for(int i=0;i<5;++i){s.position={0.1*i,0,0};f=sampler.update(s,turned);}
    check(near(f.YawDegrees,0,0.01)&&near(f.LocalVelocityZ,10)&&near(f.LocalVelocityX,0),"yaw offset corrects heading and axes");

    // Gaps and bad input give one neutral frame and never a derivative across them.
    sampler.reset();s=base();sampler.update(s,cfg);s.position={0.1,0,0};check(sampler.update(s,cfg).OutputActive,"running");
    s.paused=true;f=sampler.update(s,cfg);check(f.Paused&&!f.OutputActive&&f.LocalVelocityZ==0,"pause neutral");
    s.paused=false;check(!sampler.update(s,cfg).OutputActive,"resume reacquires history");
    check(sampler.update(s,cfg).OutputActive,"running again");
    s.identity=2;check(!sampler.update(s,cfg).OutputActive,"new mech neutral");
    s.position={1000,0,0};check(!sampler.update(s,cfg).OutputActive,"teleport neutral");
    f=sampler.update(s,cfg);check(f.OutputActive&&near(f.LocalVelocityZ,0)&&near(f.LocalAccelerationZ,0),"no spike after teleport");
    s.dt=1;check(!sampler.update(s,cfg).OutputActive,"long frame gap neutral");
    s.dt=0.01;s.controlled=false;check(!sampler.update(s,cfg).OutputActive,"not controlled neutral");
    s.controlled=true;s.position.x=std::numeric_limits<double>::quiet_NaN();f=sampler.update(s,cfg);
    check(!f.FrameValid&&!f.OutputActive,"invalid input neutral");
    check(json(f).find("nan")==std::string::npos,"JSON finite");
    f=neutral(f,true);check(f.Stale&&!f.FrameValid,"watchdog marks stale invalid");

    // Smoothing depends on elapsed time, not on the frame rate.
    Settings smooth=raw();smooth.velocityHz=2;
    double expected=10*(1-std::exp(-2*pi*2*0.2));
    for(int rate:{60,120}) {
        sampler.reset();s=base();s.dt=1.0/rate;
        for(int i=0;i<5;++i)sampler.update(s,smooth); // standing still
        for(int i=1;i<=rate/5;++i){s.position={10.0*i/rate,0,0};f=sampler.update(s,smooth);}
        check(near(f.LocalVelocityZ,expected,0.05),"velocity low-pass independent of frame rate");
    }
    // The defaults smooth a sudden start instead of passing a spike.
    Settings defaults{};defaults.yawOffsetDeg=0;
    sampler.reset();s=base();
    for(int i=0;i<5;++i)sampler.update(s,defaults);
    double peak=0;
    for(int i=1;i<=100;++i){s.position={0.1*i,0,0};f=sampler.update(s,defaults);peak=std::max(peak,(double)std::abs(f.LocalAccelerationZ));}
    check(peak>1&&peak<60,"default smoothing limits a 10 m/s step to a bounded surge");
    check(near(f.LocalVelocityZ,10,0.2),"default smoothing settles on the true velocity");

    // MASC and jump jet state, with held event flags.
    Settings hold=raw();hold.eventHoldSeconds=0.03;
    sampler.reset();s=base();s.mascInstalled=true;s.jetsInstalled=true;s.mascWarningPoint=0.55f;s.mascDamagePoint=0.75f;
    sampler.update(s,hold);f=sampler.update(s,hold);
    check(f.MascInstalled&&!f.MascEngaged&&!f.MascStarted&&near(f.MascWarningPoint,0.55)&&near(f.MascDamagePoint,0.75),"MASC idle");
    s.mascEngaged=true;s.mascGauge=0.6f;f=sampler.update(s,hold);
    check(f.MascEngaged&&f.MascStarted&&!f.MascEnded&&near(f.MascGauge,0.6),"MASC start");
    f=sampler.update(s,hold);f=sampler.update(s,hold);check(f.MascStarted,"MASC start held");
    f=sampler.update(s,hold);f=sampler.update(s,hold);check(f.MascEngaged&&!f.MascStarted,"MASC start hold expires");
    s.mascEngaged=false;f=sampler.update(s,hold);check(!f.MascEngaged&&f.MascEnded,"MASC end");
    s.jetState=JetsWarmup;f=sampler.update(s,hold);check(f.JumpJetState==JetsWarmup&&!f.JumpStarted,"jet warm-up is not a start");
    s.jetState=JetsActive;f=sampler.update(s,hold);check(f.JumpStarted&&!f.JumpEnded,"jets firing");
    s.jetState=JetsCooldown;f=sampler.update(s,hold);check(f.JumpEnded&&f.JumpJetState==JetsCooldown,"jets stopped");
    s.paused=true;f=sampler.update(s,hold);check(!f.MascEngaged&&!f.JumpJetState&&!f.JumpEnded&&near(f.MascGauge,0.6),"pause clears cues, keeps gauge");

    // Heave gain and soft ceiling. 20 m/s^2 upward.
    auto heave=[&](Settings c){
        sampler.reset();s=base();
        for(int i=0;i<6;++i){double t=0.01*i;s.position={0,0,10*t*t};f=sampler.update(s,c);}
        return (double)f.LocalAccelerationY;};
    Settings half=raw();half.heaveGain=0.5;
    check(near(heave(raw()),20,0.05)&&near(heave(half),10,0.05),"heave gain scales vertical acceleration");
    Settings capped=raw();capped.heaveLimitG=1;
    check(near(heave(capped),gravity*std::tanh(20/gravity),0.05),"heave ceiling bends the peak");
    capped.heaveGain=50;check(heave(capped)<=gravity+1e-4&&heave(capped)>gravity*0.99,"heave never exceeds the ceiling");
    check(near(f.LocalAccelerationZ,0,0.01)&&near(f.LocalAccelerationX,0,0.01),"heave settings leave other axes alone");

    // Haptic mapping onto the standard fields SimHub's stock effects read.
    using Packet=SimHubExternalTelemetrySender::SHTelemetryPacket;
    check(sizeof(Packet)==660,"SimHub packet size");
    HapticSettings hs{};HapticState state{};Frame h{};
    h.OutputActive=h.FrameValid=1;h.DeltaTime=0.01f;h.MascInstalled=1;h.MascWarningPoint=0.55f;h.MascDamagePoint=0.75f;
    auto packet=[&]{state.update(h,hs);return external_packet(h,1,2,3,4.0,state,hs);};
    h.MascGauge=0.9f;auto p=packet();
    check(p.Gear[0]=='1'&&p.Gear[1]==0&&p.MaxGears==2&&p.EngineRpm==0&&p.EngineMaxRpm==10000,"idle: gear 1, no RPM");
    h.MascEngaged=1;h.MascGauge=0.5f;p=packet();check(p.Gear[0]=='2'&&p.MascEngaged==1,"MASC on: gear 2");
    // MASC warning: ABS taps (rising edges counted over one second) from the game's
    // warning point, faster towards the damage point, never held on.
    auto taps=[&](float gauge){
        state={};h.MascGauge=gauge;int edges=0,on=0;bool last=false;
        for(int i=0;i<100;++i){auto q=packet();if(q.AbsActive&&!last)++edges;on+=q.AbsActive;last=q.AbsActive!=0;}
        return std::pair<int,int>{edges,on};};
    auto low=taps(0.56f),high=taps(0.74f),full=taps(0.75f),over=taps(1.0f);
    check(taps(0.54f).second==0,"no ABS below the warning point");
    check(low.first>=2&&low.first<=3&&high.first>=5&&high.first<=6,"ABS tap rate follows the gauge between warning and damage point");
    check(full.first==6&&over.first==6&&over.second<=20,"beyond the damage point ABS keeps tapping at the top rate, never held");
    h.MascEngaged=0;check(taps(0.8f).second==0,"ABS stops when MASC is switched off");
    h.MascEngaged=1;h.MascInstalled=0;check(taps(0.8f).second==0,"no ABS without MASC fitted");
    h.MascInstalled=1;hs.abs=false;check(taps(0.8f).second==0,"MascWarningAbsEnable 0 switches the taps off");
    // MASC running is an engine vibration only from the warning point on, and gets rougher (lower RPM) towards the damage point.
    hs=HapticSettings{};state={};
    h.MascGauge=0;check(packet().EngineRpm==0,"MASC on, empty gauge: no vibration");
    h.MascGauge=0.5f;check(packet().EngineRpm==0,"still none just below the warning point");
    h.MascGauge=0.55f;check(near(packet().EngineRpm,9000,1),"at the warning point: 90% RPM");
    h.MascGauge=0.65f;check(near(packet().EngineRpm,7000,1),"MASC RPM falls as the gauge fills towards damage");
    h.MascGauge=0.75f;check(near(packet().EngineRpm,5000),"MASC RPM bottoms out at the damage point");
    h.MascGauge=1;check(near(packet().EngineRpm,5000),"MASC RPM stays there beyond it");
    h.JumpJetState=JetsActive;check(near(packet().EngineRpm,8000),"firing jump jets replace the MASC vibration");
    h.JumpJetState=JetsWarmup;check(near(packet().EngineRpm,4000),"so does their warm-up");
    h.JumpJetState=0;hs.mascEngine=false;check(packet().EngineRpm==0,"MASC vibration can be switched off");
    hs=HapticSettings{};state={};h.MascEngaged=0;h.MascGauge=0.5f;check(packet().EngineRpm==0,"no vibration from a draining gauge");
    h.JumpJetState=JetsWarmup;check(near(packet().EngineRpm,4000),"jet warm-up RPM");
    h.JumpJetState=JetsActive;check(near(packet().EngineRpm,8000),"jets firing RPM");
    h.JumpJetState=JetsCooldown;check(packet().EngineRpm==0,"no RPM in cooldown");
    h.MascEngaged=1;
    h.JumpJetState=JetsActive;hs.engine=false;hs.gear=false;h.MascGauge=0.8f;p=packet();
    check(p.EngineRpm==0&&p.Gear[0]=='1',"each mapping can be switched off");
    hs=HapticSettings{};state={};h.MascGauge=0.8f;packet();
    h.OutputActive=0;h.Paused=1;p=packet();check(p.Gear[0]=='2'&&p.EngineRpm==0&&p.IsSessionPaused,"pause: cues off, gear held");
    h.FrameValid=0;p=packet();check(p.Gear[0]=='1'&&!p.IsSessionRunning,"lost mech: gear reset");
    h=Frame{};h.OutputActive=h.FrameValid=1;h.PitchDegrees=10;h.RollDegrees=5;h.YawDegrees=20;h.PitchSpeed=1;
    h.LocalAccelerationX=1;h.LocalAccelerationY=2;h.LocalAccelerationZ=3;
    Identity id=parse_mech_tag("Mech.Medium.Phoenixhawk.PXH-7S");id.profile="MW5 Fast";id.baseMaxSpeedKmh=97.2f;
    p=external_packet(h,1,2,3,4.0,state,hs,id);
    check(p.PitchDegrees==-10&&p.RollDegrees==5&&p.YawDegrees==20&&p.PitchRateDegreesPerSecond<0,"SimHub pitch is positive nose-down");
    check(p.LocalSwayMs2==1&&p.LocalHeaveMs2==2&&p.LocalSurgeMs2==3,"acceleration axes");
    check(!std::strcmp(p.VehicleName,"MW5 Fast")&&!std::strcmp(p.VehicleId,"MW5 Fast")&&!std::strcmp(p.Chassis,"Phoenixhawk")&&!std::strcmp(p.Variant,"PXH-7S"),"vehicle name and identity");
    check(p.GameSignature==0xEB0A2F3Du&&p.TelemetrySignature==SimHubExternalTelemetrySender::SHTelemetryConstants::TelemetrySignature&&p.IsUserInControl&&p.BaseMaxSpeedKmh==97.2f,"packet header");

    // Mech identity and the SimHub vehicle name.
    check(id.weightClass=="Medium"&&id.chassis=="Phoenixhawk"&&id.variant=="PXH-7S","mech tag parts");
    check(parse_mech_tag("").chassis.empty()&&parse_mech_tag("Vehicle.Tank.Scorpion").chassis.empty(),"non-mech tags give no chassis");
    check(parse_mech_tag("Mech.Light.Locust.LCT-1V.P").variant=="LCT-1V.P","variant keeps its dots");
    ProfileSettings bands{};
    check(profile_name("Atlas",54,bands)=="MW5 Slow"&&profile_name("Mauler",55.8,bands)=="MW5 Slow"&&profile_name("X",60.4,bands)=="MW5 Slow","slow band");
    check(profile_name("Centurion",64.8,bands)=="MW5 Standard"&&profile_name("X",75,bands)=="MW5 Standard","standard band");
    check(profile_name("Dragon",86.4,bands)=="MW5 Fast"&&profile_name("Phoenixhawk",97.2,bands)=="MW5 Fast","fast band");
    check(profile_name("Locust",129.6,bands)=="MW5 Very Fast"&&profile_name("X",101,bands)=="MW5 Very Fast","very fast band");
    bands.specificChassis={"dragon","Locust"};
    check(profile_name("Dragon",86.4,bands)=="MW5 Dragon"&&profile_name("Locust",129.6,bands)=="MW5 Locust","listed chassis get their own name");
    check(profile_name("Phoenixhawk",97.2,bands)=="MW5 Fast"&&profile_name("",86.4,bands)=="MW5 Fast","others stay in their band");

    // Gait table and the stride it predicts, against values measured in game
    // (docs/recordings, raw cockpit height: steps per second and bounce height).
    Gait pxh{},mauler{},jager{},atlas{},jenner{},timber{},none{};
    check(builtin_gait("phoenixhawk",pxh)&&builtin_gait("Mauler",mauler)&&builtin_gait("JAGERMECH",jager)&&builtin_gait("Atlas",atlas)
        &&builtin_gait("Jenner",jenner)&&builtin_gait("TimberWolf",timber),"chassis found in the table, any case");
    check(!builtin_gait("NoSuchMech",none)&&!builtin_gait("",none)&&!none.known()&&pxh.known(),"unknown chassis is not found");
    check(near(pxh.runBounceM,2.093)&&near(pxh.walkBounceM,1.255)&&near(jager.runSpeedKmh,64.8)&&near(atlas.walkSpeedKmh,10.8),"table values and units");
    check(near(stride(mauler,55.0).stepsPerSecond,1.30,0.04),"Mauler 55 km/h: 1.30 steps/s measured");
    check(near(stride(pxh,70.1).stepsPerSecond,1.46,0.04)&&near(stride(pxh,96.3).stepsPerSecond,1.85,0.08),"Phoenix Hawk step rate");
    check(near(stride(jager,33.5).stepsPerSecond,1.23,0.03)&&near(stride(jager,64.6).stepsPerSecond,2.00,0.03),"Jagermech runs from 64.8 km/h");
    check(near(stride(atlas,25.3).stepsPerSecond,1.12,0.03)&&near(stride(atlas,47.8).stepsPerSecond,1.37,0.03),"Atlas uses its own blend speeds");
    check(near(stride(jenner,52.1).stepsPerSecond,1.26,0.04)&&near(stride(jenner,113.1).stepsPerSecond,2.04,0.05),"Jenner step rate");
    check(near(stride(timber,43.7).stepsPerSecond,1.20,0.03)&&near(stride(timber,86.2).stepsPerSecond,1.68,0.05),"Timber Wolf step rate");
    check(near(stride(jager,33.5).bounceM*0.75,0.956,0.03)&&near(stride(jenner,52.1).bounceM*0.75,0.898,0.03)&&near(stride(atlas,47.8).bounceM*0.75,0.894,0.03),"cockpit bounce height");
    check(stride(pxh,200).blend==1&&near(stride(pxh,200).stepsPerSecond,2)&&near(stride(pxh,-96.3).stepsPerSecond,stride(pxh,96.3).stepsPerSecond),"pure run above run speed; reverse uses the same model");
    check(near(stride(pxh,7.2).bounceM,pxh.walkBounceM/2)&&stride(pxh,0).bounceM==0,"walk fades in from standing");

    // Predicted bounce and the comfort factor.
    Settings comfort{};comfort.yawOffsetDeg=0;comfort.heaveLimitG=0;comfort.heaveSafetyNet=false;comfort.heaveComfortG=3;
    check(near(smoothing_loss(2,comfort),std::pow(0.5,1.5),1e-6),"three 2 Hz stages at 2 Hz");
    Settings lighter=comfort;lighter.accelerationHz=0;check(near(smoothing_loss(2,lighter),0.5,1e-6),"a stage set to 0 passes everything");
    comfort.gait=pxh;
    check(near(bounce_peak(97.2,comfort)/gravity,4.3,0.1),"Phoenix Hawk at top speed: 4.3 G");
    check(near(comfort_factor(97.2,comfort),3/(bounce_peak(97.2,comfort)/gravity),1e-9)&&comfort_factor(97.2,comfort)<0.72,"capped to the comfort level");
    comfort.gait=mauler;check(near(bounce_peak(55.8,comfort)/gravity,1.4,0.05)&&comfort_factor(55.8,comfort)==1,"Mauler stays under the level: untouched");
    comfort.gait=none;check(bounce_peak(100,comfort)==0&&comfort_factor(100,comfort)==1,"unknown chassis: no cap");
    comfort.gait=pxh;comfort.heaveComfortG=0;check(comfort_factor(97.2,comfort)==1,"comfort 0 switches the cap off");
    comfort.heaveComfortG=3;
    double before=1;bool reached=false,exact=true;
    for(double kmh=0;kmh<=160;kmh+=0.5) {
        double k=comfort_factor(kmh,comfort);
        if(k>before+1e-12||k>1)exact=false;        // never rises with speed, never above 1
        if(k<1){reached=true;if(!near(bounce_peak(kmh,comfort)*k,3*gravity,1e-9))exact=false;}
        else if(reached)exact=false;                // once capped it stays capped
        before=k;
    }
    check(reached&&exact,"factor is 1 up to a threshold speed, then holds the bounce at the level");
    comfort.heaveGain=0.5;check(near(bounce_peak(97.2,comfort)/gravity,2.15,0.05)&&comfort_factor(97.2,comfort)==1,"HeaveGain counts towards the level");
    comfort.heaveGain=1;

    // The sampler applying it to a cockpit that really bounces. This gait takes two
    // 2 m steps a second at any speed, so the cockpit bobs 1.5 m at 2 Hz.
    struct Run {
        Sampler sampler;Sample s{};double phase{},x{};Frame f{};
        // Returns the largest heave of the last second.
        double go(const Settings& c,double kmh,double heightM,double hz,double seconds,int jets=0) {
            const double dt=1.0/200;double top=0;int n=static_cast<int>(seconds/dt);
            for(int i=0;i<n;++i) {
                x+=kmh/3.6*dt;phase+=2*pi*hz*dt;
                s.valid=s.controlled=true;s.identity=1;s.dt=dt;s.timestampMs+=5;s.jetState=jets;s.jetsInstalled=true;
                s.position={x,0,heightM/2*std::sin(phase)};
                f=sampler.update(s,c);
                if(i>=n-200)top=std::max(top,(double)std::abs(f.LocalAccelerationY));
            }
            return top;
        }
    };
    Gait steady{2.0,2.0,1,1,14.4,100.8,2};
    Settings bob=comfort;bob.gait=steady;bob.heaveComfortG=0;
    double natural=Run{}.go(bob,72,1.5,2,6);
    check(near(natural,bounce_peak(72,bob),0.05*natural)&&natural>4*gravity,"the formula matches what the sampler's filters really do");
    bob.heaveComfortG=3;Run capped3;double held=capped3.go(bob,72,1.5,2,6);
    check(near(held,3*gravity,0.05*3*gravity)&&near(capped3.f.HeaveFactor,3*gravity/bounce_peak(72,bob),0.01)&&capped3.f.GaitKnown,"bounce held at the comfort level");
    check(near(capped3.f.GroundSpeedKmh,72,0.5)&&near(capped3.f.BouncePeakG,bounce_peak(72,bob)/gravity,0.05),"diagnostics: ground speed and predicted bounce");
    bob.heaveComfortG=5;check(near(Run{}.go(bob,72,1.5,2,6),natural,0.01*natural),"a level above the bounce changes nothing");
    // Jumps and landings stay full: the cap eases off while the jets are busy.
    bob.heaveComfortG=3;
    capped3.go(bob,72,1.5,2,1,JetsActive);check(capped3.f.HeaveFactor>0.999,"cap released during a jump");
    capped3.go(bob,72,1.5,2,0.2,JetsCooldown);check(capped3.f.HeaveFactor>0.999,"still released until the jets are inactive");
    capped3.go(bob,72,1.5,2,1);check(near(capped3.f.HeaveFactor,3*gravity/bounce_peak(72,bob),0.01),"cap back after landing");
    // Safety net: an unknown chassis bouncing too hard is eased to 1.25 times the level.
    Settings net=bob;net.gait=none;net.heaveSafetyNet=true;
    Run wild;double eased=wild.go(net,72,1.5,2,25);
    check(near(eased,3.75*gravity,0.08*3.75*gravity)&&wild.f.SafetyGain<0.95&&wild.f.HeaveFactor==1&&!wild.f.GaitKnown,"safety net eases a sustained excess down");
    wild.go(net,72,0.5,2,25);check(wild.f.SafetyGain>0.97,"and lets go again afterwards");
    Run brief;brief.go(net,72,0.5,2,5);brief.go(net,72,1.5,2,0.3);check(brief.f.SafetyGain>0.75,"a short burst barely moves it");
    Settings both=bob;both.heaveSafetyNet=true;Run calm;calm.go(both,72,1.5,2,10);
    check(calm.f.SafetyGain>0.999,"with the cap working the safety net stays idle");
    net.heaveSafetyNet=false;check(near(Run{}.go(net,72,1.5,2,6),natural,0.01*natural),"safety net can be switched off");

    // Gait file lines.
    std::string name;Gait parsed{};
    check(parse_gait_line(format_gait_line("Phoenixhawk",pxh),name,parsed)&&name=="Phoenixhawk"&&near(parsed.runBounceM,pxh.runBounceM)
        &&near(parsed.walkSpeedKmh,14.4)&&parsed.stepsPerCycle==2,"gait line round trip");
    check(!parse_gait_line(kGaitHeader,name,parsed)&&!parse_gait_line("",name,parsed)&&!parse_gait_line("# note",name,parsed),"header and blanks are skipped");
    check(!parse_gait_line("X,100,200,2,1,2,50,50",name,parsed)&&!parse_gait_line("X,0,0,2,1,2,14.4,100.8",name,parsed)
        &&!parse_gait_line("X,100,abc,2,1,2,14.4,100.8",name,parsed)&&!parse_gait_line("X,100,99999,2,1,2,14.4,100.8",name,parsed),"bad values are rejected");
    check(parse_gait_line(" \"Custom\" , 90 , 150 , 2 , 1 , 2 , 14.4 , 100.8 ",name,parsed)&&name=="Custom"&&near(parsed.walkBounceM,0.9),"spaces and quotes are tolerated");

    // Learning the gait of a chassis that is not in the table.
    Gait truth{1.0,2.0,2,1,14.4,100.8,2},timing{};
    auto learn=[&](GaitLearner& l,double kmh,double seconds,bool ground,double& angle) {
        Stride st=stride(truth,kmh);const double dt=1.0/90;
        for(int i=0;i<static_cast<int>(seconds/dt);++i){angle+=2*pi*st.stepsPerSecond*dt;l.update(dt,8+st.bounceM*0.75/2*std::sin(angle),kmh,ground,timing,0.75);}
    };
    GaitLearner learner;double angle=0;
    check(!learner.ready()&&!learner.gait(timing).known(),"nothing learned yet");
    learn(learner,30,20,true,angle);learn(learner,90,20,true,angle);
    Gait learned=learner.gait(timing);
    check(learner.ready()&&near(learned.walkBounceM,1.0,0.1)&&near(learned.runBounceM,2.0,0.2),"walk and run bounce learned from two speeds");
    learner.reset();angle=0;learn(learner,90,20,true,angle);learned=learner.gait(timing);
    check(learner.ready()&&near(stride(learned,90).bounceM,stride(truth,90).bounceM,0.05*stride(truth,90).bounceM),"one speed is enough to predict that speed");
    learner.reset();angle=0;learn(learner,90,20,false,angle);check(learner.steps()==0,"nothing is learned in the air");

    // Power-up and shutdown vibration, started by MechShaker "Powering" events.
    HapticSettings ps{};HapticState power{};Frame on{};
    // Fixed levels and lengths, so the checks do not depend on the shipped defaults.
    ps.powerUpStart=0.15;ps.powerUpEnd=0.7;ps.powerDownStart=0.6;ps.powerDownEnd=0.1;
    ps.powerInitSeconds=5;ps.powerUpSeconds=4;ps.powerDownSeconds=3.3;
    on.OutputActive=on.FrameValid=1;on.DeltaTime=0.1f;
    auto rpm_after=[&](int updates){for(int i=0;i<updates;++i)power.update(on,ps);return power.rpm;};
    check(rpm_after(1)==0,"no vibration without an event");
    power.power_event(PowerUp);
    check(near(rpm_after(1),1500,1),"power-up starts low and rough");
    check(near(rpm_after(20),0.15*10000+0.55*10000*2.0/4,60),"and climbs");
    check(rpm_after(19)>6000&&power.rpm<7000,"to a smooth buzz at the end");
    check(rpm_after(2)==0&&power.powerPhase==-1,"then stops");
    power.power_event(PowerDown);double first=rpm_after(1),later=rpm_after(15);
    check(near(first,6000,1)&&later<first&&later>1000,"shutdown winds down");
    check(rpm_after(20)==0,"and stops after its length");
    power.power_event(PowerInitialising);rpm_after(45);check(power.rpm>5000,"initialising runs for its own, longer time");
    check(rpm_after(6)==0,"and ends");
    power.power_event(PowerUp);rpm_after(5);
    on.JumpJetState=JetsActive;check(near(rpm_after(1),8000),"jump jets take over from a power ramp");
    on.JumpJetState=0;on.MascEngaged=1;on.MascDamagePoint=0.75f;check(near(rpm_after(1),9000),"so does MASC");
    on.MascEngaged=0;
    power.power_event(PowerUp);rpm_after(10);double waited=power.powerElapsed;
    on.OutputActive=0;check(rpm_after(5)==0&&power.powerElapsed==waited&&power.powerPhase==PowerUp,"a pause holds the ramp");
    on.OutputActive=1;check(rpm_after(1)>1500,"and it carries on afterwards");
    on.OutputActive=on.FrameValid=0;rpm_after(1);check(power.powerPhase==-1,"leaving the mech drops it");
    on.OutputActive=on.FrameValid=1;
    power.power_event(7);power.power_event(-1);check(rpm_after(1)==0,"unknown phases are ignored");
    ps.powerEngine=false;power.power_event(PowerUp);check(rpm_after(1)==0,"PowerVibrationEnable 0 switches it off");

    // Which slots of MechWarriorVR's event ring are new.
    auto ring=shaker_new_events(10,ShakerControl{12,13});
    check(ring.first==10&&ring.count==3,"three new events, oldest first");
    ring=shaker_new_events(126,ShakerControl{1,130});check(ring.first==126&&ring.count==4,"wraps round the ring");
    ring=shaker_new_events(0,ShakerControl{5,1000});check(ring.first==6&&ring.count==kShakerSlots,"after an overrun only the newest ring-full is left");
    check(shaker_new_events(13,ShakerControl{12,13}).count==0&&shaker_new_events(20,ShakerControl{12,13}).count==0,"nothing new");
    check(shaker_new_events(0,ShakerControl{500,3}).count==0&&shaker_new_events(0,ShakerControl{-1,3}).count==0,"a nonsense write index is ignored");

    // Settings panel: values, commands and edits to the configuration file.
    check(panel_key("HeaveComfortG")&&!panel_key("SimHubPort")&&!panel_key("CockpitYawOffsetDeg"),"only in-game settings can change live");
    check(panel_chassis_key("HeaveGain")&&!panel_chassis_key("JumpJetLevel"),"per chassis: comfort level and gain only");
    check(panel_value(*panel_key("HeaveComfortG"),2.4999998)=="2.5"&&panel_value(*panel_key("HeaveComfortG"),3)=="3","values are written with at most two decimals");
    check(panel_value(*panel_key("HeaveComfortG"),50)=="6"&&panel_value(*panel_key("HeaveLimitG"),0)=="2","and kept inside the panel's range");
    check(panel_value(*panel_key("HeaveSafetyNet"),1)=="1"&&panel_value(*panel_key("HeaveSafetyNet"),0)=="0","flags are 0 or 1");
    PanelCommand command;
    check(parse_panel_command("1\n7\nset\nHeaveComfortG=2.5\nHeaveSafetyNet=false",command)&&command.protocol==1&&command.request==7&&command.action=="set"
        &&command.values.size()==2&&command.values[0].first=="HeaveComfortG"&&command.values[1].second=="false","a command from the script");
    check(parse_panel_command("1\n8\nget",command)&&command.values.empty()&&!parse_panel_command("hello",command)&&!parse_panel_command("1\nx\nget",command),"malformed commands are refused");
    double panelNumber{};
    check(panel_number("true",panelNumber)&&panelNumber==1&&panel_number("0.25",panelNumber)&&near(panelNumber,0.25)&&!panel_number("1x",panelNumber)&&!panel_number("",panelNumber),"numbers and booleans");
    const std::string file="# Comfort.\nHeaveComfortG = 3\n\nHeaveGain = 1 # as the game\n# [Phoenixhawk]\n# HeaveComfortG = 2.5\n";
    auto edited=config_set(file,"","HeaveComfortG","2.5");
    check(edited=="# Comfort.\nHeaveComfortG = 2.5\n\nHeaveGain = 1 # as the game\n# [Phoenixhawk]\n# HeaveComfortG = 2.5\n","a value changes in place; comments stay");
    check(config_set(file,"","HeaveGain","0.8").find("HeaveGain = 0.8 # as the game\n")!=std::string::npos,"a comment after the value stays");
    edited=config_set(file,"Dragon","HeaveComfortG","2");
    check(edited==file+"\n[Dragon]\nHeaveComfortG = 2\n","a chassis block is added at the end");
    edited=config_set(edited,"dragon","HeaveGain","0.9");
    check(edited==file+"\n[Dragon]\nHeaveComfortG = 2\nHeaveGain = 0.9\n","a second key joins the block, whatever the case of the name");
    auto general=config_set(edited,"","JumpJetLevel","0.7");
    check(general==file+"JumpJetLevel = 0.7\n\n[Dragon]\nHeaveComfortG = 2\nHeaveGain = 0.9\n"||general.find("JumpJetLevel = 0.7\n[Dragon]")!=std::string::npos,"a new general key goes easedBefore the chassis blocks");
    check(general.find("[Dragon]")>general.find("JumpJetLevel"),"so it does not land inside one");
    check(config_set(edited,"","HeaveComfortG","4").find("[Dragon]\nHeaveComfortG = 2\n")!=std::string::npos,"a general change leaves the chassis value alone");
    auto removed=config_remove(edited,"Dragon","HeaveGain");
    check(removed==file+"\n[Dragon]\nHeaveComfortG = 2\n","removing one chassis key keeps the block");
    check(config_remove(removed,"Dragon","HeaveComfortG")==file,"removing the last one removes the block");
    check(config_set("","","HeaveGain","1")=="HeaveGain = 1\n","an empty file gets the key");

    // Changes from the panel ease in.
    Settings easedLive{},easedTarget{};
    easedTarget.heaveComfortG=2;easedTarget.positionHz=4;easedTarget.heaveSafetyNet=false;easedTarget.bounceRatio=0.8;
    ease_settings(easedLive,easedTarget,0.011);
    check(easedLive.heaveComfortG<3&&easedLive.heaveComfortG>2.9&&easedLive.positionHz>2&&easedLive.positionHz<2.2,"motion settings move a little per frame");
    check(!easedLive.heaveSafetyNet&&near(easedLive.bounceRatio,0.8),"other settings apply at once");
    double easedBefore=easedLive.heaveComfortG;bool monotonic=true;
    for(int i=0;i<90;++i){ease_settings(easedLive,easedTarget,0.011);monotonic=monotonic&&easedLive.heaveComfortG<=easedBefore&&easedLive.heaveComfortG>=2;easedBefore=easedLive.heaveComfortG;}
    check(monotonic&&near(easedLive.heaveComfortG,2,0.1)&&near(easedLive.positionHz,4,0.1),"and are nearly there after a second, without overshoot");
    for(int i=0;i<300;++i)ease_settings(easedLive,easedTarget,0.011);
    check(easedLive.heaveComfortG==2&&easedLive.positionHz==4,"then settle exactly");
    easedTarget.heaveLimitG=0;ease_settings(easedLive,easedTarget,0.011);check(easedLive.heaveLimitG==0,"switching a limit off does not slide through small limits");
    easedTarget.heaveLimitG=6;ease_settings(easedLive,easedTarget,0.011);check(easedLive.heaveLimitG==6,"nor does switching it on");

    // Part destruction: a kick in the motion and a burst of engine vibration.
    check(near(kick_shape(0),0)&&kick_shape(0.15)>0.45&&kick_shape(0.15)<0.55&&near(kick_shape(0.3),1)&&near(kick_shape(0.4),1)&&kick_shape(0.8)>0&&kick_shape(0.8)<1&&kick_shape((kKickRise+kKickHold+kKickBack))==0&&kick_shape(-1)==0,"a kick eases out to 1, holds and eases back");
    {bool oneWay=true;double fastest=0;for(double t=0.01;t<(kKickRise+kKickHold+kKickBack);t+=0.01){oneWay=oneWay&&kick_shape(t)>=0;fastest=std::max(fastest,std::abs(kick_shape(t)-kick_shape(t-0.01))/0.01);}
     check(oneWay&&fastest<5.5,"without swinging past rest, and never faster than about five times its size per second");}
    HapticSettings parts{};HapticState hit{};Frame moving{};
    moving.OutputActive=moving.FrameValid=1;moving.DeltaTime=0.01f;
    // Runs the state for `seconds` and returns the largest kick in roll, pitch and heave, with sign.
    struct Peak { double roll{},pitch{},heave{},rpmFirst{},rpmLast{}; };
    auto play=[&](double seconds) {
        Peak p;bool first=true;
        for(double t=0;t<seconds;t+=0.01) {
            Frame f=moving;hit.update(f,parts);hit.apply_kick(f);hit.advance_kick(f);
            if(std::abs(f.RollDegrees)>std::abs(p.roll))p.roll=f.RollDegrees;
            if(std::abs(f.PitchDegrees)>std::abs(p.pitch))p.pitch=f.PitchDegrees;
            if(std::abs(f.LocalAccelerationY)>std::abs(p.heave))p.heave=f.LocalAccelerationY;
            if(f.LocalAccelerationX!=0||f.LocalAccelerationZ!=0)p.roll=1e9; // sideways forces are not used
            if(first){p.rpmFirst=hit.rpm;first=false;}
            p.rpmLast=hit.rpm;
        }
        return p;
    };
    check(play(0.5).roll==0&&hit.rpm==0,"nothing without an event");
    hit.part_destroyed(PartLeftArm,parts);auto leftArm=play(2);
    check(near(leftArm.roll,-1.6,0.05)&&leftArm.pitch==0&&leftArm.heave==0,"left arm: a light lean to the left and nothing else");
    check(near(leftArm.rpmFirst,3500,1)&&leftArm.rpmLast==0,"with a short rough burst that ends");
    hit.part_destroyed(PartRightArm,parts);auto rightArm=play(2);
    check(near(rightArm.roll,1.6,0.05),"right arm: to the right");
    hit.part_destroyed(PartLeftLeg,parts);auto leg=play(2);
    check(near(leg.roll,-2.4,0.05)&&near(leg.heave,-3.6,0.1)&&leg.pitch==0,"a leg is stronger and drops, with no tip");
    hit.part_destroyed(PartLeftLeg,parts);play(1.3);check(hit.kickElapsed>0&&kick_shape(hit.kickElapsed,hit.kickBack)>0.05,"a leg comes back slowly: still leaning after 1.3 s");play(1);check(hit.kickElapsed<0,"and is done after two");
    hit.part_destroyed(PartLeftArm,parts);play(1.3);check(hit.kickElapsed<0,"while an arm is over by then");
    hit.part_destroyed(PartRightTorso,parts);auto torso=play(2);
    check(near(torso.roll,1.12,0.03)&&near(torso.heave,-4.8,0.1)&&near(torso.pitch,6,0.15)&&torso.pitch>5*torso.roll,"right torso hit from the front: mostly knocked back, with a small lean to the right");
    hit.part_destroyed(PartRightTorso,parts,false,true);auto torsoRear=play(2);
    check(near(torsoRear.roll,-1.12,0.03)&&near(torsoRear.pitch,-6,0.15)&&near(torsoRear.heave,-4.8,0.1),"right torso hit from the rear: mostly thrown forward, with a small lean to the left");
    hit.part_destroyed(PartLeftTorso,parts);auto leftFront=play(2);hit.part_destroyed(PartLeftTorso,parts,false,true);auto leftRear=play(2);
    check(near(leftFront.roll,-1.12,0.03)&&near(leftFront.pitch,6,0.15)&&near(leftRear.roll,1.12,0.03)&&near(leftRear.pitch,-6,0.15),"left torso: back and to the left from the front, forward and to the right from the rear");
    hit.part_destroyed(PartLeftArm,parts,false,true);check(play(2).pitch==0,"the direction only matters for side torsos");
    Sample rearHit=base();rearHit.torsoRearHit=2;check(Sampler{}.update(rearHit,raw()).TorsoRearHit==2,"which torso was hit from behind reaches the frame");
    Sample walked=base();walked.leftSteps=7;walked.rightSteps=6;walked.stepsKnown=true;Frame counted=Sampler{}.update(walked,raw());
    check(counted.LeftSteps==7&&counted.RightSteps==6&&counted.StepsKnown==1,"the step counters of each foot reach the frame");
    hit.part_destroyed(PartHead,parts);auto head=play(2);
    check(head.roll==0&&near(head.pitch,15,0.4)&&head.heave==0,"the head knocks the nose up, to neither side");
    hit.part_destroyed(PartCenterTorso,parts);
    check(play(1).roll==0&&hit.rpm==0,"the centre torso has no kick of its own");
    // The mech destroyed: vibration only, by kind of death.
    hit.power_event(PowerDown);hit.mech_destroyed(parts,DeathExplosion);auto death=play(0.9);
    check(death.roll==0&&death.pitch==0&&death.heave==0,"mech destroyed: no kick, the fall is in the motion already");
    check(near(death.rpmFirst,3000,1)&&hit.rpm<death.rpmFirst&&hit.rpm>800&&hit.powerPhase==-1,"an explosion rumbles ever rougher, in place of the shutdown ramp");
    play(2.2);check(hit.rpm==0,"for three seconds");
    hit.mech_destroyed(parts,DeathEjection);auto eject=play(0.5);
    check(near(eject.rpmFirst,5000,1)&&hit.rpm<5000&&hit.rpm>3000,"an ejection is lighter");play(0.6);check(hit.rpm==0,"and lasts a second");
    hit.part_destroyed(PartLeftLeg,parts,true);auto wreck=play(0.5);
    check(wreck.roll==0&&wreck.heave==0&&near(wreck.rpmFirst,4000,1)&&hit.rpm==0,"a part breaking off the wreck only rumbles lightly");
    // A side torso takes its arm with it: one hit, the stronger one, whatever the order.
    hit.part_destroyed(PartLeftTorso,parts);play(0.05);hit.part_destroyed(PartLeftArm,parts);
    check(near(hit.kickPitch,6,0.01),"an arm right after its torso does not replace the torso's kick");play(2);
    hit.part_destroyed(PartLeftArm,parts);play(0.05);hit.part_destroyed(PartLeftTorso,parts);
    check(near(hit.kickPitch,6,0.01),"a torso right after its arm takes over");
    play(2);
    parts.partStrength=10;hit.part_destroyed(PartLeftTorso,parts);check(near(hit.kickRoll,-2.8,0.01)&&near(hit.kickPitch,12.5,0.01)&&near(hit.kickHeave,-25,0.01),"no kick exceeds the limits");play(2);
    parts.partStrength=0.5;hit.part_destroyed(PartLeftTorso,parts);check(near(hit.kickPitch,3,0.01),"strength scales the kick");play(2);
    parts.partStrength=1;parts.partVibration=false;hit.part_destroyed(PartLeftArm,parts);
    auto silent=play(2);check(near(silent.roll,-1.6,0.05)&&silent.rpmFirst==0,"PartVibrationEnable 0 keeps the kick and drops the burst");
    parts.partVibration=true;parts.parts=false;hit.part_destroyed(PartLeftTorso,parts);hit.mech_destroyed(parts);
    check(play(1).roll==0&&hit.rpm==0,"PartDestructionEnable 0 switches it all off");
    parts.parts=true;hit.part_destroyed(PartLeftArm,parts);
    Frame jetsFiring=moving;jetsFiring.JumpJetState=JetsActive;hit.update(jetsFiring,parts);
    check(near(hit.rpm,3500,1),"a part going outranks the jump jets");play(2);
    hit.part_destroyed(PartLeftArm,parts);play(0.1);
    Frame pausedFrame=moving;pausedFrame.OutputActive=0;double heldKick=hit.kickElapsed;
    hit.update(pausedFrame,parts);hit.apply_kick(pausedFrame);hit.advance_kick(pausedFrame);
    check(hit.kickElapsed==heldKick&&pausedFrame.RollDegrees==0,"a pause holds the kick and sends none");
    pausedFrame.FrameValid=0;hit.update(pausedFrame,parts);check(hit.kickElapsed<0&&hit.burstElapsed<0,"leaving the mech drops it");
    hit.part_destroyed(99,parts);hit.part_destroyed(-1,parts);check(hit.kickElapsed<0,"unknown parts are ignored");
    // The game's verdict travels with every frame.
    Sample deadMech=base();deadMech.dead=true;deadMech.deathKnown=true;deadMech.deathCause=kCauseCored|kCauseNoPilot;
    Frame verdict=Sampler{}.update(deadMech,raw());
    check(verdict.MechDead==1&&verdict.DeathKnown==1&&verdict.DeathCause==0x0a,"dead, known and the cause flags reach the frame");

    // Test buttons of the settings panel.
    HapticState tested{};HapticSettings ts{};
    auto step=[&](int n){for(int i=0;i<n;++i)tested.update(moving,ts);};
    tested.testGear=1;step(1);check(tested.mascGear,"gear test: gear 2");step(110);check(!tested.mascGear,"and back to 1 after a second");
    tested.testJets=2;step(1);check(near(tested.rpm,8000),"jet test: the firing level");step(210);check(tested.rpm==0,"for two seconds");
    tested.testMasc=HapticState::kTestMascSeconds;step(1);double mascFirst=tested.rpm;step(280);
    check(near(mascFirst,9000,1)&&tested.rpm<5400&&tested.rpm>=5000,"MASC test: sweeps from the start level to the damage level");step(30);check(tested.rpm==0,"and stops");
    tested.testAbs=2;int testTaps=0;bool wasTapping=false;
    for(int i=0;i<200;++i){tested.update(moving,ts);if(tested.absActive&&!wasTapping)++testTaps;wasTapping=tested.absActive;}
    check(testTaps>=7&&testTaps<=9,"ABS test: testTaps at the middle rate for two seconds");step(5);check(!tested.absActive,"then none");
    tested.testJets=2;Frame away=moving;away.OutputActive=0;tested.update(away,ts);check(tested.testJets==0,"tests are dropped while the output is off");
    // Footsteps: a thump on the wheels of one side, sized by the weight class of the mech.
    check(near(stomp_strength(ts,"Light"),0.25)&&near(stomp_strength(ts,"Medium"),0.5)&&near(stomp_strength(ts,"heavy"),0.75)&&near(stomp_strength(ts,"Assault"),1)&&near(stomp_strength(ts,""),0.5),"heavier classes stomp harder; an unknown class counts as medium");
    HapticState steps{};steps.stomp(true,stomp_strength(ts,"Assault"),ts);steps.update(moving,ts);
    auto wheels=external_packet(moving,1,2,3,4,steps,ts);
    check(near(wheels.SuspensionVelocityFrontLeftMps,0.5)&&near(wheels.SuspensionVelocityRearLeftMps,0.5)&&wheels.SuspensionVelocityFrontRightMps==0&&wheels.SuspensionVelocityRearRightMps==0,"left foot of an assault mech: a full thump on the left wheels only");
    for(int i=0;i<10;++i)steps.update(moving,ts);
    check(steps.suspension[0]>0.1f&&steps.suspension[0]<0.25f,"which dies away within a tenth of a second or two");
    steps.stomp(false,stomp_strength(ts,"Light"),ts);steps.update(moving,ts);
    check(near(steps.suspension[1],0.125,0.001)&&near(steps.suspension[3],0.125,0.001)&&steps.suspension[0]>0,"right foot of a light mech: a quarter of it on the right wheels, while the left still fades");
    for(int i=0;i<60;++i)steps.update(moving,ts);
    check(steps.suspension[0]==0&&steps.suspension[1]==0&&external_packet(moving,1,2,3,4).SuspensionVelocityRearRightMps==0,"all still after half a second, and without steps");
    steps.stomp(true,50,ts);check(near(steps.thumpSize[0],1),"no thump exceeds the limit");
    HapticSettings quiet=ts;quiet.stomps=false;HapticState noSteps{};noSteps.stomp(true,1,quiet);noSteps.update(moving,quiet);
    check(noSteps.suspension[0]==0,"StompEnable 0 switches the footsteps off");
    std::cout<<"All motion tests passed\n";return 0;
}
