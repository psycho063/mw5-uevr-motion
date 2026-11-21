#pragma once
#include <cstdint>

// The event buffer MechWarriorVR publishes for the MechShaker app: named shared
// memory "MechShakerBridgeMemory", written by the MechShakerBridge part of
// MWVR.dll from events the MechShakerRelay mod picks up in game. Layout and event
// codes as in github.com/sicsix/MW5-UEVR-Plugins and github.com/sicsix/MechShaker.
// Any number of processes can read it; reading does not consume events.
namespace mw5 {
constexpr wchar_t kShakerMemoryName[]=L"MechShakerBridgeMemory";
constexpr int kShakerSlots=128;
struct ShakerControl {
    int64_t writeIndex;   // slot of the newest event
    int64_t packetNumber; // events written so far
};
struct ShakerEvent {
    int32_t code, int0;
    float f[6];
};
static_assert(sizeof(ShakerControl)==16&&sizeof(ShakerEvent)==32);
constexpr size_t kShakerMemoryBytes=sizeof(ShakerControl)+sizeof(ShakerEvent)*kShakerSlots;
enum ShakerCode : int32_t {
    ShakerClearFx=-2, ShakerBridgeClosed=-1, ShakerTorsoTwist=1, ShakerFootstep=2, ShakerTrace=3, ShakerProjectile=4,
    ShakerMissiles=5, ShakerAms=6, ShakerMelee=7, ShakerDropship=8, ShakerJumpJets=9, ShakerAirborne=10, ShakerLanded=11,
    ShakerMasc=12, ShakerPowering=13, ShakerPartDestruction=14, ShakerDamaged=15,
};
// The slots holding events that arrived since `seen` (the packet number at the
// last read), oldest first. If more arrived than the ring holds, only the newest
// ring-full is still there.
struct ShakerSpan { int first, count; };
inline ShakerSpan shaker_new_events(int64_t seen,const ShakerControl& control) {
    int64_t arrived=control.packetNumber-seen;
    if(arrived<=0||control.writeIndex<0||control.writeIndex>=kShakerSlots)return {0,0};
    if(arrived>kShakerSlots)arrived=kShakerSlots;
    return {static_cast<int>(((control.writeIndex-arrived+1)%kShakerSlots+kShakerSlots)%kShakerSlots),static_cast<int>(arrived)};
}
}
