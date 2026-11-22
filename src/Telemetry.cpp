#include "Telemetry.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace mw5 {
double Vec3::length() const { return std::sqrt(x*x+y*y+z*z); }
bool Vec3::finite() const { return std::isfinite(x)&&std::isfinite(y)&&std::isfinite(z); }
Vec3 Quat::rotate(Vec3 v) const {
    Vec3 t{2*(y*v.z-z*v.y),2*(z*v.x-x*v.z),2*(x*v.y-y*v.x)};
    return v+t*w+Vec3{y*t.z-z*t.y,z*t.x-x*t.z,x*t.y-y*t.x};
}
Quat Quat::operator*(Quat b) const {
    return {w*b.x+x*b.w+y*b.z-z*b.y, w*b.y-x*b.z+y*b.w+z*b.x,
            w*b.z+x*b.y-y*b.x+z*b.w, w*b.w-x*b.x-y*b.y-z*b.z};
}
Quat Quat::yaw(double radians) { return {0,0,std::sin(radians/2),std::cos(radians/2)}; }
bool Quat::normalize() {
    double n=std::sqrt(x*x+y*y+z*z+w*w);
    if (!std::isfinite(n)||n<0.5||n>1.5) return false;
    x/=n;y/=n;z/=n;w/=n;return true;
}
static float bounded(double v,double limit) {
    return std::isfinite(v)?static_cast<float>(std::clamp(v,-limit,limit)):0.0f;
}
static Vec3 bounded(Vec3 v,double limit) { return {bounded(v.x,limit),bounded(v.y,limit),bounded(v.z,limit)}; }
// Low-pass gain for one step of dt seconds. Time-based, so the result does not
// depend on the frame rate.
static double gain(double hz,double dt) { return hz>0?1-std::exp(-2*pi*hz*dt):1; }

Stride stride(const Gait& g,double speedKmh) {
    Stride s;
    double speed=std::abs(speedKmh),span=g.runSpeedKmh-g.walkSpeedKmh;
    // The game cross-fades the walk and the run cycle by speed and plays the mix
    // at the blended cycle length, without scaling playback to the speed.
    s.blend=span>0?std::clamp((speed-g.walkSpeedKmh)/span,0.0,1.0):speed>=g.runSpeedKmh?1.0:0.0;
    double cycle=(1-s.blend)*g.walkCycleS+s.blend*g.runCycleS;
    s.stepsPerSecond=cycle>0?g.stepsPerCycle/cycle:0;
    s.bounceM=(1-s.blend)*g.walkBounceM+s.blend*g.runBounceM;
    // Below walking speed the walk cycle fades in from standing still.
    if(g.walkSpeedKmh>0&&speed<g.walkSpeedKmh)s.bounceM*=speed/g.walkSpeedKmh;
    return s;
}
double smoothing_loss(double hz,const Settings& cfg) {
    double loss=1;
    for(double cutoff:{cfg.positionHz,cfg.velocityHz,cfg.accelerationHz})
        if(cutoff>0)loss/=std::sqrt(1+(hz/cutoff)*(hz/cutoff));
    return loss;
}
double bounce_peak(double speedKmh,const Settings& cfg) {
    if(!cfg.gait.known())return 0;
    Stride s=stride(cfg.gait,speedKmh);
    // A bounce of height h at f steps a second peaks at (h/2)(2 pi f)^2.
    double swing=2*pi*s.stepsPerSecond;
    return s.bounceM*cfg.bounceRatio/2*swing*swing*smoothing_loss(s.stepsPerSecond,cfg)*std::abs(cfg.heaveGain);
}
double comfort_factor(double speedKmh,const Settings& cfg) {
    double level=cfg.heaveComfortG*gravity,peak=bounce_peak(speedKmh,cfg);
    return level>0&&peak>level?level/peak:1;
}

void ease_settings(Settings& live,const Settings& target,double dt) {
    // About 95% of the way in 0.9 s. zeroIsOff: 0 switches the feature off, so
    // sliding through small values would briefly do the opposite of what is asked.
    auto ease=[dt](double from,double to,bool zeroIsOff) {
        if(from==to||!(dt>0)||!std::isfinite(from)||(zeroIsOff&&(from<=0||to<=0)))return to;
        double share=dt/0.3;
        if(share>=1)return to;
        double value=from+(to-from)*share;
        return std::abs(value-to)<1e-3*std::max(1.0,std::abs(to))?to:value;
    };
    Settings next=target;
    next.heaveComfortG=ease(live.heaveComfortG,target.heaveComfortG,true);
    next.heaveLimitG=ease(live.heaveLimitG,target.heaveLimitG,true);
    next.heaveGain=ease(live.heaveGain,target.heaveGain,false);
    next.positionHz=ease(live.positionHz,target.positionHz,true);
    next.velocityHz=ease(live.velocityHz,target.velocityHz,true);
    next.accelerationHz=ease(live.accelerationHz,target.accelerationHz,true);
    next.angularHz=ease(live.angularHz,target.angularHz,true);
    live=next;
}
Frame neutral(Frame f,bool stale) {
    // Preserve position and the raw MASC gauge for diagnostics; clear all rig cues.
    f.OutputActive=0;f.Stale=stale;f.DeltaTime=0;
    f.SpeedMps=f.SpeedKmh=f.GameSpeedKmh=f.Throttle=0;
    f.LocalVelocityX=f.LocalVelocityY=f.LocalVelocityZ=0;
    f.LocalAccelerationX=f.LocalAccelerationY=f.LocalAccelerationZ=0;
    f.GForceLateral=f.GForceVertical=f.GForceLongitudinal=0;
    f.RollDegrees=f.PitchDegrees=f.YawDegrees=0;
    f.RollSpeed=f.PitchSpeed=f.YawSpeed=0;
    f.RollAcceleration=f.PitchAcceleration=f.YawAcceleration=0;
    f.MascEngaged=f.MascStarted=f.MascEnded=0;
    f.JumpJetState=f.JumpStarted=f.JumpEnded=0;
    if(stale) f.FrameValid=0;
    return f;
}
void Sampler::reset() {
    hasPrevious=hasVelocity=hasRates=false;
    position=velocity=acceleration=rates={};
    mascStartHold=mascEndHold=jumpStartHold=jumpEndHold=0;
    jumpEase=heaveEnvelope=0;safetyGain=1;
}
Frame Sampler::update(Sample s,const Settings& cfg) {
    Frame f{};f.Sequence=++sequence;
    f.TimestampMS=static_cast<uint32_t>(s.timestampMs);f.Paused=s.paused;
    f.FrameValid=s.valid&&s.position.finite()&&s.rotation.normalize();
    f.MascInstalled=s.mascInstalled;f.MascType=s.mascType;f.JumpJetsInstalled=s.jetsInstalled;
    f.MascGauge=bounded(s.mascGauge,1e6);
    f.MascWarningPoint=bounded(s.mascWarningPoint,1e6);f.MascDamagePoint=bounded(s.mascDamagePoint,1e6);
    f.MaxSpeedKmh=bounded(s.maxSpeedKmh,10000);f.HeatPercent=bounded(s.heatPercent,1e6);
    f.PhysicsDiscontinuityCounter=discontinuities;
    bool active=f.FrameValid&&!s.paused&&s.controlled;
    if(!active) { if(hasPrevious)++discontinuities;reset();f.PhysicsDiscontinuityCounter=discontinuities;return neutral(f); }
    s.rotation=s.rotation*Quat::yaw(cfg.yawOffsetDeg*pi/180);
    f.PositionX=bounded(s.position.x,1e9);f.PositionY=bounded(s.position.y,1e9);f.PositionZ=bounded(s.position.z,1e9);
    bool continuous=hasPrevious&&s.identity==previous.identity&&std::isfinite(s.dt)&&s.dt>1e-5&&s.dt<=cfg.maxDeltaSeconds;
    if(continuous&&s.timestampMs-previous.timestampMs>250)continuous=false;
    if(continuous&&(s.position-previous.position).length()>cfg.teleportDistanceM)continuous=false;
    if(!continuous) {
        ++discontinuities;reset();previous=s;hasPrevious=true;position=s.position;
        f.PhysicsDiscontinuityCounter=discontinuities;
        return neutral(f); // one neutral sample; never differentiate across a gap
    }
    f.OutputActive=1;f.DeltaTime=static_cast<float>(s.dt);
    // Position -> velocity -> acceleration, each smoothed before the next derivative.
    Vec3 lastPosition=position,lastVelocity=velocity;
    position=position+(s.position-position)*gain(cfg.positionHz,s.dt);
    Vec3 rawVelocity=(position-lastPosition)*(1/s.dt);
    // The first velocity has no history: take it as is and report no acceleration.
    velocity=hasVelocity?velocity+(rawVelocity-velocity)*gain(cfg.velocityHz,s.dt):rawVelocity;
    double limit=cfg.maxAccelerationG*gravity;
    if(hasVelocity) {
        // Differentiate WORLD velocity, then turn it into the cockpit frame. Local
        // velocity would lose the centripetal part of a turn at constant speed.
        Vec3 raw=bounded(s.rotation.unrotate((velocity-lastVelocity)*(1/s.dt)),limit);
        acceleration=acceleration+(raw-acceleration)*gain(cfg.accelerationHz,s.dt);
    }
    hasVelocity=true;
    Vec3 local=s.rotation.unrotate(velocity);
    f.LocalVelocityX=bounded(local.y,10000);f.LocalVelocityY=bounded(local.z,10000);f.LocalVelocityZ=bounded(local.x,10000);
    f.LocalAccelerationX=bounded(acceleration.y*cfg.lateralSign,limit);
    // Comfort cap: scale the heave by what the gait predicts for this ground
    // speed. It eases off while the jump jets are busy (that state lasts until
    // after touchdown), so lift-off and landing keep their full kick.
    double groundKmh=std::hypot(local.x,local.y)*3.6;
    double factor=comfort_factor(groundKmh,cfg);
    bool jumping=s.jetState!=JetsInactive;
    jumpEase=jumping?std::min(1.0,jumpEase+s.dt/0.3):std::max(0.0,jumpEase-s.dt/0.5);
    factor=1-(1-factor)*(1-jumpEase);
    double heave=acceleration.z*cfg.heaveGain*factor;
    // Safety net: follow the peaks of the heave on the ground and, while they
    // stay above 1.25 times the comfort level, ease a second gain down slowly.
    if(cfg.heaveSafetyNet&&cfg.heaveComfortG>0) {
        heaveEnvelope*=std::exp(-s.dt);
        if(jumpEase<=0)heaveEnvelope=std::max(heaveEnvelope,std::abs(heave));
        double level=cfg.heaveComfortG*gravity*1.25;
        double wanted=heaveEnvelope>level?level/heaveEnvelope:1;
        safetyGain+=(wanted-safetyGain)*(1-std::exp(-s.dt/(wanted<safetyGain?1.5:3.0)));
        heave*=safetyGain;
    }
    else {heaveEnvelope=0;safetyGain=1;}
    f.GroundSpeedKmh=bounded(groundKmh,10000);
    f.BouncePeakG=bounded(bounce_peak(groundKmh,cfg)/gravity,1000);
    f.HeaveFactor=static_cast<float>(factor);f.SafetyGain=static_cast<float>(safetyGain);
    f.GaitKnown=cfg.gait.known();
    double ceiling=cfg.heaveLimitG*gravity;
    if(ceiling>0)heave=ceiling*std::tanh(heave/ceiling);
    f.LocalAccelerationY=bounded(heave*cfg.verticalSign,limit);
    f.LocalAccelerationZ=bounded(acceleration.x*cfg.longitudinalSign,limit);
    f.GForceLateral=f.LocalAccelerationX/static_cast<float>(gravity);
    f.GForceVertical=f.LocalAccelerationY/static_cast<float>(gravity);
    f.GForceLongitudinal=f.LocalAccelerationZ/static_cast<float>(gravity);
    // Angles are not smoothed.
    constexpr double deg=180/pi;
    Vec3 forward=s.rotation.rotate({1,0,0}),right=s.rotation.rotate({0,1,0}),up=s.rotation.rotate({0,0,1});
    f.RollDegrees=static_cast<float>(std::atan2(-right.z,up.z)*cfg.rollSign*deg);
    f.PitchDegrees=static_cast<float>(std::asin(std::clamp(forward.z,-1.0,1.0))*cfg.pitchSign*deg);
    f.YawDegrees=static_cast<float>(std::atan2(forward.y,forward.x)*cfg.yawSign*deg);
    // Angular velocity in the cockpit frame, from the rotation since the last tick.
    Quat step=previous.rotation.conjugate()*s.rotation;
    if(step.w<0)step={-step.x,-step.y,-step.z,-step.w}; // shortest way round
    Vec3 axis{step.x,step.y,step.z};
    double sine=axis.length();
    Vec3 av=sine>1e-12?axis*(2*std::atan2(sine,step.w)/sine/s.dt):Vec3{};
    double maxRate=cfg.maxAngularSpeedDeg/deg;
    Vec3 rawRates{bounded(-av.x*cfg.rollSign,maxRate),bounded(-av.y*cfg.pitchSign,maxRate),bounded(av.z*cfg.yawSign,maxRate)};
    Vec3 lastRates=rates;
    rates=hasRates?rates+(rawRates-rates)*gain(cfg.angularHz,s.dt):rawRates;
    f.RollSpeed=static_cast<float>(rates.x);f.PitchSpeed=static_cast<float>(rates.y);f.YawSpeed=static_cast<float>(rates.z);
    // The first rate sample after a reset has no prior derivative.
    if(hasRates) {
        f.RollAcceleration=bounded((rates.x-lastRates.x)/s.dt,1000);
        f.PitchAcceleration=bounded((rates.y-lastRates.y)/s.dt,1000);
        f.YawAcceleration=bounded((rates.z-lastRates.z)/s.dt,1000);
    }
    hasRates=true;
    f.SpeedMps=bounded(velocity.length(),10000);f.SpeedKmh=f.SpeedMps*3.6f;
    f.GameSpeedKmh=bounded(s.gameSpeedKmh,10000);
    f.Throttle=bounded(s.throttle,1);
    // Event flags stay set for eventHoldSeconds so a rate-capped or slower
    // consumer cannot miss them. With a zero hold they last one sample.
    auto held=[&](double& left,bool edge) {
        left=edge?cfg.eventHoldSeconds:std::max(left-s.dt,0.0);return edge||left>0;
    };
    bool firing=s.jetState==JetsActive,wasFiring=previous.jetState==JetsActive;
    f.MascEngaged=s.mascEngaged;
    f.MascStarted=held(mascStartHold,s.mascEngaged&&!previous.mascEngaged);
    f.MascEnded=held(mascEndHold,!s.mascEngaged&&previous.mascEngaged);
    f.JumpJetState=s.jetState;
    f.JumpStarted=held(jumpStartHold,firing&&!wasFiring);
    f.JumpEnded=held(jumpEndHold,!firing&&wasFiring);
    f.PhysicsDiscontinuityCounter=discontinuities;
    previous=s;return f;
}
std::string json(const Frame& f) {
    std::ostringstream s;s.imbue(std::locale::classic());s<<std::fixed<<std::setprecision(6);
    s<<"{\"Game\":\"MechWarrior 5 Mercenaries\",\"TelemetryVersion\":1";
#define FIELD(name) s<<",\"" #name "\":"<<f.name
    FIELD(Paused);FIELD(OutputActive);FIELD(FrameValid);FIELD(Stale);
    FIELD(Sequence);FIELD(TimestampMS);FIELD(DeltaTime);FIELD(PhysicsDiscontinuityCounter);
    FIELD(SpeedMps);FIELD(SpeedKmh);FIELD(GroundSpeedKmh);FIELD(GameSpeedKmh);FIELD(MaxSpeedKmh);FIELD(Throttle);
    FIELD(GaitKnown);FIELD(BouncePeakG);FIELD(HeaveFactor);FIELD(SafetyGain);
    FIELD(LocalVelocityX);FIELD(LocalVelocityY);FIELD(LocalVelocityZ);
    FIELD(LocalAccelerationX);FIELD(LocalAccelerationY);FIELD(LocalAccelerationZ);
    FIELD(GForceLateral);FIELD(GForceVertical);FIELD(GForceLongitudinal);
    FIELD(RollDegrees);FIELD(PitchDegrees);FIELD(YawDegrees);
    FIELD(RollSpeed);FIELD(PitchSpeed);FIELD(YawSpeed);
    FIELD(RollAcceleration);FIELD(PitchAcceleration);FIELD(YawAcceleration);
    FIELD(PositionX);FIELD(PositionY);FIELD(PositionZ);
    FIELD(MascInstalled);FIELD(MascEngaged);FIELD(MascStarted);FIELD(MascEnded);FIELD(MascType);
    FIELD(MascGauge);FIELD(MascWarningPoint);FIELD(MascDamagePoint);
    FIELD(JumpJetsInstalled);FIELD(JumpJetState);FIELD(JumpStarted);FIELD(JumpEnded);
    FIELD(HeatPercent);
#undef FIELD
    s<<"}";return s.str();
}
}
