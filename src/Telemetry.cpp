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
    double heave=acceleration.z*cfg.heaveGain,ceiling=cfg.heaveLimitG*gravity;
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
    FIELD(SpeedMps);FIELD(SpeedKmh);FIELD(GameSpeedKmh);FIELD(MaxSpeedKmh);FIELD(Throttle);
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
