#include "Telemetry.hpp"
#include "SimHub.hpp"
#include "Gait.hpp"
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
    check(sizeof(Packet)==644,"SimHub packet size");
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
    // MASC running is an engine vibration that gets rougher (lower RPM) as the gauge fills.
    hs=HapticSettings{};state={};
    h.MascGauge=0;check(near(packet().EngineRpm,9000),"MASC on, empty gauge: 90% RPM");
    h.MascGauge=0.375f;check(near(packet().EngineRpm,7000),"MASC RPM falls as the gauge fills");
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
    Settings comfort{};comfort.yawOffsetDeg=0;comfort.heaveLimitG=0;comfort.heaveSafetyNet=false;
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
    std::cout<<"All motion tests passed\n";return 0;
}
