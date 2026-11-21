#pragma once
#include "Telemetry.hpp"
#include <string>

namespace mw5 {
// The gait of a chassis from the table compiled into the plugin (GaitTable.hpp).
// The name is the chassis id from the mech's gameplay tag; case is ignored.
// Returns false and leaves `out` alone when the chassis is not in the table.
bool builtin_gait(const std::string& chassis, Gait& out);

// One line of mech_gait.csv / mw5_mech_gait.csv:
//   Chassis,WalkBounceCm,RunBounceCm,WalkCycleS,RunCycleS,StepsPerCycle,WalkSpeedKmh,RunSpeedKmh
// Returns false for the header, blank lines and anything malformed.
bool parse_gait_line(const std::string& line, std::string& chassis, Gait& out);
std::string format_gait_line(const std::string& chassis, const Gait& gait);
extern const char* const kGaitHeader;

// Learns the walk and run bounce of a chassis that is not in the table, by
// watching the cockpit bob relative to the mech's own root while it moves.
// Optional and off by default: the table covers every stock chassis.
class GaitLearner {
public:
    // Call once per tick. bodyHeightM is the cockpit's height above the mech's
    // root, so terrain does not show in it. `timing` supplies the cycle lengths
    // and blend speeds to assume (the defaults fit all but three stock chassis);
    // `ratio` is Settings::bounceRatio.
    void update(double dt, double bodyHeightM, double groundKmh, bool onGround, const Gait& timing, double ratio);
    void reset();
    int steps() const { return count; }
    bool ready() const { return count >= kMinimumSteps; }
    // `timing` with the learned walk and run bounce filled in.
    Gait gait(const Gait& timing) const;
    static constexpr int kMinimumSteps = 12;
    // Typical walk bounce as a share of the run bounce (median of the table),
    // used until the mech has been seen at clearly different speeds.
    static constexpr double kWalkShare = 0.64;
private:
    bool started{}, below{}, clean{};
    double mean{}, low{}, high{}, elapsed{}, speedSum{}, speedLow{}, speedHigh{};
    int ticks{}, count{};
    // Least squares for measured = walkPart * walk + runPart * run.
    double ww{}, wr{}, rr{}, wy{}, ry{};
};
}
