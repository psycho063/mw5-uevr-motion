#pragma once
#include <cstdint>
#include <string>

namespace mw5 {
constexpr double pi = 3.14159265358979323846;
constexpr double gravity = 9.80665;
struct Vec3 {
    double x{}, y{}, z{};
    Vec3 operator+(Vec3 b) const { return {x+b.x,y+b.y,z+b.z}; }
    Vec3 operator-(Vec3 b) const { return {x-b.x,y-b.y,z-b.z}; }
    Vec3 operator*(double k) const { return {x*k,y*k,z*k}; }
    double length() const;
    bool finite() const;
};
struct Quat {
    double x{}, y{}, z{}, w{1};
    Vec3 rotate(Vec3 v) const;
    Vec3 unrotate(Vec3 v) const { return conjugate().rotate(v); }
    Quat conjugate() const { return {-x,-y,-z,w}; }
    Quat operator*(Quat b) const; // applies b first, then this
    static Quat yaw(double radians);
    bool normalize();
};
// Jump jet states as the game reports them (EJumpJetState).
enum JumpJetState : int32_t { JetsInactive=0, JetsWarmup=1, JetsActive=2, JetsCooldown=3 };

// One game tick. The game gives no velocities: the sampler derives them from
// the cockpit transform. Unreal world axes: X=forward, Y=right, Z=up.
struct Sample {
    uint64_t identity{}, timestampMs{};
    double dt{};
    bool valid{}, paused{}, controlled{};
    Vec3 position{}; // metres
    Quat rotation{};
    float throttle{}, gameSpeedKmh{}, maxSpeedKmh{}, heatPercent{};
    bool mascInstalled{}, mascEngaged{}, jetsInstalled{};
    int32_t mascType{}, jetState{};
    float mascGauge{}, mascWarningPoint{}, mascDamagePoint{};
};
struct Settings {
    // The cockpit socket's forward axis is not the mech's; this turns it to match.
    double yawOffsetDeg{-90};
    // Low-pass cutoffs in Hz, 0 = off. The 2 Hz defaults approximate SpaceMonkey's
    // stock UEVR filter (a per-frame gain of about 0.13 at 90 fps) at any frame rate.
    double positionHz{2}, velocityHz{2}, accelerationHz{2}, angularHz{2};
    // The walk cycle bounces the cockpit hard on fast mechs. heaveGain scales the
    // vertical acceleration; heaveLimitG then bends it towards a ceiling it never
    // exceeds, so footfalls stay but peaks are squeezed. 0 = no ceiling.
    double heaveGain{1}, heaveLimitG{3};
    double maxAccelerationG{10}, maxAngularSpeedDeg{720};
    double teleportDistanceM{10}, maxDeltaSeconds{0.1};
    double eventHoldSeconds{0.1}; // minimum time event flags stay set; 0 = one sample
    double rollSign{1}, pitchSign{1}, yawSign{1};
    double lateralSign{1}, verticalSign{1}, longitudinalSign{1};
};
struct Frame {
    uint32_t Sequence{}, TimestampMS{};
    float DeltaTime{};
    int32_t Paused{}, OutputActive{}, FrameValid{}, Stale{};
    uint32_t PhysicsDiscontinuityCounter{};
    float SpeedMps{}, SpeedKmh{}; // derived from cockpit movement
    float GameSpeedKmh{}, MaxSpeedKmh{}, Throttle{}; // as the game reports them
    // X=lateral/right, Y=vertical/up, Z=longitudinal/forward.
    float LocalVelocityX{}, LocalVelocityY{}, LocalVelocityZ{};
    float LocalAccelerationX{}, LocalAccelerationY{}, LocalAccelerationZ{};
    float GForceLateral{}, GForceVertical{}, GForceLongitudinal{};
    float RollDegrees{}, PitchDegrees{}, YawDegrees{}; // pitch nose-up, roll right-down
    float RollSpeed{}, PitchSpeed{}, YawSpeed{}; // rad/s
    float RollAcceleration{}, PitchAcceleration{}, YawAcceleration{}; // rad/s^2
    float PositionX{}, PositionY{}, PositionZ{}; // Unreal world, metres
    int32_t MascInstalled{}, MascEngaged{}, MascStarted{}, MascEnded{}, MascType{};
    float MascGauge{}, MascWarningPoint{}, MascDamagePoint{};
    int32_t JumpJetsInstalled{}, JumpJetState{}, JumpStarted{}, JumpEnded{};
    float HeatPercent{};
};
class Sampler {
public:
    Frame update(Sample s, const Settings& settings);
    void reset();
private:
    Sample previous{};
    Vec3 position{}, velocity{}, acceleration{}, rates{};
    bool hasPrevious{}, hasVelocity{}, hasRates{};
    double mascStartHold{}, mascEndHold{}, jumpStartHold{}, jumpEndHold{};
    uint32_t sequence{}, discontinuities{};
};
Frame neutral(Frame f, bool stale=false);
std::string json(const Frame& f);
}
