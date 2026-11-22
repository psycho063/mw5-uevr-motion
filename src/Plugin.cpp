#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include "uevr/Plugin.hpp"
#include "Telemetry.hpp"
#include "SimHub.hpp"
#include "Gait.hpp"
#include "GaitTable.hpp"
#include "ShakerBuffer.hpp"
#include "Panel.hpp"
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using API=uevr::API;
using Object=API::UObject;
namespace {
constexpr const char* kVersion="0.9.0";
// Set when GaitLearning is on: the reader then also fetches the mech's root
// transform. Off by default, and nothing of the learner runs while it is off.
std::atomic_bool g_learning{false};
// Notes from the game thread for the debug log; the sender thread writes them out.
std::atomic_bool g_notesEnabled{false};
std::mutex g_notesMutex;std::vector<std::string> g_notes;
void note(std::string text) {
    if(!g_notesEnabled)return;
    std::scoped_lock lock(g_notesMutex);g_notes.push_back(std::move(text));
}
std::string narrow(const std::wstring& w) {
    if(w.empty())return {};
    int n=WideCharToMultiByte(CP_UTF8,0,w.data(),static_cast<int>(w.size()),nullptr,0,nullptr,nullptr);
    std::string s(static_cast<size_t>(n),'\0');
    WideCharToMultiByte(CP_UTF8,0,w.data(),static_cast<int>(w.size()),s.data(),n,nullptr,nullptr);
    return s;
}
bool copy_memory(const void* src,void* dst,size_t size) {
    if(!src) return false;
    __try { std::memcpy(dst,src,size);return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template<class T> bool read(Object* o,const wchar_t* name,T& result) {
    return o&&copy_memory(o->get_property_data(name),&result,sizeof(T));
}
Object* object(Object* o,const wchar_t* name) { Object* v{};read(o,name,v);return v; }
bool chain_has(Object* o,const wchar_t* text) {
    for(API::UStruct* c=o?o->get_class():nullptr;c;c=c->get_super())
        if(c->get_fname()->to_string().find(text)!=std::wstring::npos)return true;
    return false;
}
// A blueprint-callable function that takes no inputs and returns exactly `size`
// bytes, or null. The size check stands in for a type check before the call.
API::UFunction* getter(Object* o,const wchar_t* name,int32_t size) {
    auto fn=o?o->get_class()->find_function(name):nullptr;
    if(!fn||fn->get_properties_size()!=size)return nullptr;
    auto ret=fn->find_property(L"ReturnValue");
    return ret&&ret->get_offset()==0?fn:nullptr;
}
template<class T> T call(Object* o,API::UFunction* fn,T fallback={}) {
    if(!o||!fn)return fallback;
    alignas(16) std::array<uint8_t,64> params{};
    o->process_event(fn,params.data());
    T result;std::memcpy(&result,params.data(),sizeof(T));return result;
}
std::vector<Object*> components(Object* actor) {
    std::vector<Object*> out;
    auto fn=actor->get_class()->find_function(L"K2_GetComponentsByClass");
    auto cls=API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.ActorComponent");
    if(!fn||!cls||fn->get_properties_size()>256)return out;
    auto arg=fn->find_property(L"ComponentClass"),ret=fn->find_property(L"ReturnValue");
    if(!arg||!ret)return out;
    std::array<uint8_t,256> params{};
    std::memcpy(params.data()+arg->get_offset(),&cls,sizeof(cls));
    actor->process_event(fn,params.data());
    struct {Object** data;int32_t count,capacity;} list{};
    std::memcpy(&list,params.data()+ret->get_offset(),sizeof(list));
    if(list.count<0||list.count>4096)return out;
    for(int i=0;i<list.count;++i){Object* c{};if(copy_memory(list.data+i,&c,sizeof(c))&&c)out.push_back(c);}
    return out;
}
bool paused(Object* context) {
    auto cls=API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.GameplayStatics");
    if(!cls) return true; // fail closed until the engine is ready
    auto fn=cls->find_function(L"IsGamePaused");
    if(!fn||fn->get_properties_size()>256) return true;
    auto arg=fn->find_property(L"WorldContextObject"),ret=fn->find_property(L"ReturnValue");
    if(!arg||!ret) return true;
    std::array<uint8_t,256> params{};
    std::memcpy(params.data()+arg->get_offset(),&context,sizeof(context));
    cls->get_class_default_object()->process_event(fn,params.data());
    return reinterpret_cast<API::FBoolProperty*>(ret)->get_value_from_object(params.data());
}
// UE4 FTransform: float quaternion, translation and scale, 16-byte aligned.
struct Transform { float qx,qy,qz,qw,tx,ty,tz,pad,sx,sy,sz,pad2; };
static_assert(sizeof(Transform)==48);

enum class Reader:int { NoPawn,NotAMech,NoCockpit,NoTransform,Ok };
const char* text(Reader r) {
    switch(r) {
    case Reader::NoPawn:return "no pawn";
    case Reader::NotAMech:return "pawn is not a mech";
    case Reader::NoCockpit:return "cockpit component not found yet";
    case Reader::NoTransform:return "cockpit transform unavailable";
    default:return "ok";
    }
}
// Game objects for the current mech. Everything comes from base-game classes
// (MWMech, MWMASCComponent, MWJumpJetComponent, MWHeatComponent) by name, so it
// works with and without mods such as YetAnotherMechlab.
struct Mech {
    Object *pawn{},*cockpit{},*masc{},*jets{},*heat{},*root{};
    API::UFunction *toWorld{},*rootToWorld{},*speed{},*maxSpeed{},*throttle{};
    API::UFunction *hasMasc{},*engaged{},*gauge{},*hasJets{},*jetState{},*heatPercent{};
    int32_t warningOffset{-1},damageOffset{-1},typeOffset{-1};
    bool isMech{};double retryIn{},tagRetryIn{};
    std::string tag; // "Mech.<WeightClass>.<Chassis>.<Variant>", empty until known

    // The mech's gameplay tag, from its data asset: MechData.MechType.TagName.
    void find_tag() {
        tagRetryIn=1;
        auto asset=call<Object*>(pawn,getter(pawn,L"GetMechDataAsset",sizeof(Object*)));
        auto data=asset?asset->get_class()->find_property(L"MechData"):nullptr;
        if(!data||data->get_class()->get_name()!=L"StructProperty")return;
        auto type=reinterpret_cast<API::FStructProperty*>(data)->get_struct()->find_property(L"MechType");
        if(!type||type->get_class()->get_name()!=L"StructProperty")return;
        API::FName name{};
        if(!copy_memory(reinterpret_cast<const uint8_t*>(asset)+data->get_offset()+type->get_offset(),&name,8))return;
        auto text=narrow(name.to_string());
        if(text.rfind("Mech.",0)==0)tag=text;
    }
    void resolve(Object* current) {
        *this={};pawn=current;
        isMech=pawn&&chain_has(pawn,L"MWMech");
        if(!isMech)return;
        speed=getter(pawn,L"GetMechSpeed",4);maxSpeed=getter(pawn,L"GetMechMaxSpeed",4);throttle=getter(pawn,L"GetMechThrottle",4);
        for(auto c:components(pawn)) {
            if(!masc&&chain_has(c,L"MWMASCComponent"))masc=c;
            else if(!jets&&chain_has(c,L"MWJumpJetComponent"))jets=c;
            else if(!heat&&chain_has(c,L"MWHeatComponent"))heat=c;
        }
        hasMasc=getter(masc,L"HasMASC",1);engaged=getter(masc,L"IsEngaged",1);gauge=getter(masc,L"GetGaugeValue",4);
        hasJets=getter(jets,L"HasJumpJets",1);jetState=getter(jets,L"GetJumpJetState",1);
        heatPercent=getter(heat,L"GetHeatAsPercent",4);
        if(auto stats=masc?masc->get_class()->find_property(L"MASCStats"):nullptr;stats&&stats->get_class()->get_name()==L"StructProperty") {
            auto type=reinterpret_cast<API::FStructProperty*>(stats)->get_struct();
            auto offset=[&](const wchar_t* name,const wchar_t* kind) {
                auto p=type->find_property(name);
                return p&&p->get_class()->get_name()==kind?stats->get_offset()+p->get_offset():-1;
            };
            warningOffset=offset(L"GaugeWarningPoint",L"FloatProperty");
            damageOffset=offset(L"GaugeDamagePoint",L"FloatProperty");
            typeOffset=offset(L"Type",L"EnumProperty");
        }
        if(g_learning) {
            // The mech's own root follows the ground; the cockpit's height above it
            // is the animation's bob alone.
            root=object(pawn,L"RootComponent");
            rootToWorld=getter(root,L"K2_GetComponentToWorld",sizeof(Transform));
        }
        find_cockpit();find_tag();
        note("mech "+narrow(pawn->get_full_name())+": MASC component "+(masc?"found":"missing")+", jump jet component "+(jets?"found":"missing")+
             ", heat component "+(heat?"found":"missing")+", cockpit "+(cockpit?toWorld?"found":"found without transform":"not found yet"));
    }
    // The route MechWarriorVR itself uses. The component sits on the mesh's
    // "Cockpit" socket and can appear a moment after the pawn.
    void find_cockpit() {
        cockpit=object(object(pawn,L"MechMeshComponent"),L"FirstPersonCockpitComponent");
        toWorld=getter(cockpit,L"K2_GetComponentToWorld",sizeof(Transform));
        retryIn=0.5;
    }
    float stat(int32_t offset) const {
        float v{};return offset>=0&&copy_memory(reinterpret_cast<const uint8_t*>(masc)+offset,&v,sizeof(v))&&std::isfinite(v)?v:0;
    }
};
Mech g_mech;
Reader sample_game(mw5::Sample* out,float dt) {
    auto pawn=API::get()->get_local_pawn(0);
    if(pawn!=g_mech.pawn)g_mech.resolve(pawn);
    if(!pawn)return Reader::NoPawn;
    if(!g_mech.isMech)return Reader::NotAMech;
    if(g_mech.tag.empty()&&(g_mech.tagRetryIn-=dt)<=0)g_mech.find_tag();
    if(!g_mech.cockpit||!g_mech.toWorld) {
        if((g_mech.retryIn-=dt)<=0) {
            g_mech.find_cockpit();
            if(g_mech.cockpit)note(g_mech.toWorld?"cockpit found":"cockpit found without transform");
        }
        if(!g_mech.cockpit)return Reader::NoCockpit;
        if(!g_mech.toWorld)return Reader::NoTransform;
    }
    auto& m=g_mech;
    auto t=call<Transform>(m.cockpit,m.toWorld);
    out->identity=reinterpret_cast<uint64_t>(pawn);
    out->valid=true;out->dt=dt;out->controlled=true; // get_local_pawn is the player's own pawn
    out->paused=paused(pawn);
    out->position=mw5::Vec3{t.tx,t.ty,t.tz}*0.01;
    out->rotation={t.qx,t.qy,t.qz,t.qw};
    if(m.rootToWorld) {
        auto base=call<Transform>(m.root,m.rootToWorld);
        out->bodyHeightM=(t.tz-base.tz)*0.01f;out->bodyHeightValid=true;
    }
    out->throttle=call<float>(pawn,m.throttle);
    out->gameSpeedKmh=call<float>(pawn,m.speed)*0.036f; // cm/s
    out->maxSpeedKmh=call<float>(pawn,m.maxSpeed)*0.036f;
    out->heatPercent=call<float>(m.heat,m.heatPercent);
    out->mascInstalled=call<uint8_t>(m.masc,m.hasMasc)!=0;
    out->mascEngaged=out->mascInstalled&&call<uint8_t>(m.masc,m.engaged)!=0;
    out->mascGauge=call<float>(m.masc,m.gauge);
    out->mascWarningPoint=m.stat(m.warningOffset);out->mascDamagePoint=m.stat(m.damageOffset);
    if(uint8_t type{};m.typeOffset>=0&&copy_memory(reinterpret_cast<const uint8_t*>(m.masc)+m.typeOffset,&type,1))out->mascType=type;
    out->jetsInstalled=call<uint8_t>(m.jets,m.hasJets)!=0;
    out->jetState=out->jetsInstalled?call<uint8_t>(m.jets,m.jetState):0;
    return Reader::Ok;
}
bool sample_guarded(mw5::Sample* out,float dt,Reader* state) {
    __try {*state=sample_game(out,dt);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
struct Feed {
    bool enabled{true};std::string ip{"127.0.0.1"};int port{},hz{};
    SOCKET socket{INVALID_SOCKET};sockaddr_in address{};double nextDue{};uint64_t sent{},errors{};
    void open() {
        if(!enabled)return;
        address.sin_family=AF_INET;address.sin_port=htons(static_cast<u_short>(port));
        if(inet_pton(AF_INET,ip.c_str(),&address.sin_addr)!=1){enabled=false;API::get()->log_error("[MW5 Motion] Invalid IPv4 destination: %s",ip.c_str());return;}
        socket=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        if(socket==INVALID_SOCKET){enabled=false;API::get()->log_error("[MW5 Motion] UDP socket failed: %d",WSAGetLastError());return;}
        u_long nonblocking=1;ioctlsocket(socket,FIONBIO,&nonblocking);
    }
    void send(const std::string& payload,uint64_t now) {
        if(!enabled||socket==INVALID_SOCKET||static_cast<double>(now)<nextDue)return;
        if(nextDue==0)nextDue=static_cast<double>(now);
        nextDue=std::max(nextDue+1000.0/hz,static_cast<double>(now));
        if(sendto(socket,payload.data(),static_cast<int>(payload.size()),0,reinterpret_cast<sockaddr*>(&address),sizeof(address))==SOCKET_ERROR) ++errors;
        else ++sent;
    }
    void close(){if(socket!=INVALID_SOCKET)closesocket(socket);socket=INVALID_SOCKET;}
};
// Read-only view of MechWarriorVR's MechShaker event buffer. It exists only when
// MechWarriorVR's bridge and the MechShakerRelay mod are running, and may appear
// some time after this plugin starts, so opening is retried.
struct ShakerReader {
    HANDLE mapping{};const uint8_t* view{};int64_t seen{};uint64_t nextTry{};
    bool open(uint64_t now) {
        if(view)return true;
        if(now<nextTry)return false;
        nextTry=now+3000;
        mapping=OpenFileMappingW(FILE_MAP_READ,FALSE,mw5::kShakerMemoryName);
        if(!mapping)return false;
        view=static_cast<const uint8_t*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,mw5::kShakerMemoryBytes));
        if(!view){CloseHandle(mapping);mapping=nullptr;return false;}
        mw5::ShakerControl control;std::memcpy(&control,view,sizeof(control));
        seen=control.packetNumber; // skip whatever happened before we looked
        return true;
    }
    void close() {
        if(view)UnmapViewOfFile(view);
        if(mapping)CloseHandle(mapping);
        view=nullptr;mapping=nullptr;
    }
    // Calls `handle` for every event that arrived since the last poll, oldest first.
    template<class Handler> void poll(uint64_t now,Handler&& handle) {
        if(!open(now))return;
        mw5::ShakerControl control;std::memcpy(&control,view,sizeof(control));
        if(control.packetNumber<seen)seen=control.packetNumber; // the writer started over
        auto span=mw5::shaker_new_events(seen,control);
        seen=control.packetNumber;
        bool closed=false;
        for(int i=0;i<span.count;++i) {
            mw5::ShakerEvent e;
            std::memcpy(&e,view+sizeof(control)+sizeof(e)*static_cast<size_t>((span.first+i)%mw5::kShakerSlots),sizeof(e));
            if(e.code==mw5::ShakerBridgeClosed)closed=true;
            else handle(e);
        }
        if(closed){close();nextTry=now+3000;} // the bridge went away; look for a new one later
    }
};
std::string lower(std::string s) { for(auto& c:s)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return s; }
std::string trim(std::string s) {
    auto a=s.find_first_not_of(" \t\r\n");auto b=s.find_last_not_of(" \t\r\n");
    return a==s.npos?std::string{}:s.substr(a,b-a+1);
}
// "Key = value" lines. A "[Chassis]" line starts a block whose keys apply to that
// chassis only; they are stored as "chassis.Key" with the chassis in lower case.
using Config=std::map<std::string,std::string>;
Config config_read(const std::filesystem::path& path) {
    std::ifstream input(path);Config values;std::string line,section;
    while(std::getline(input,line)) {
        line=trim(line.substr(0,line.find('#')));
        if(line.size()>2&&line.front()=='['&&line.back()==']'){section=lower(trim(line.substr(1,line.size()-2)))+".";continue;}
        auto n=line.find('=');
        if(n!=line.npos)values[section+trim(line.substr(0,n))]=trim(line.substr(n+1));
    }return values;
}
// The chassis block wins over the general value.
const std::string* lookup(const Config& v,const std::string& key,const std::string& chassis={}) {
    if(!chassis.empty())if(auto i=v.find(lower(chassis)+"."+key);i!=v.end())return &i->second;
    auto i=v.find(key);return i==v.end()?nullptr:&i->second;
}
double setting(const Config& v,const char* key,double fallback,double min,double max,const std::string& chassis={},bool quiet=false) {
    auto value=lookup(v,key,chassis);if(!value)return fallback;
    try {size_t used{};double n=std::stod(*value,&used);if(used!=value->size()||!std::isfinite(n)||n<min||n>max)throw 0;return n;}
    catch(...){if(!quiet)API::get()->log_warn("[MW5 Motion] Invalid config %s; using default",key);return fallback;}
}
// A setting the panel can change: its default and limits come from the panel's table.
double live_setting(const Config& v,const char* key,const std::string& chassis={},bool quiet=false) {
    auto k=mw5::panel_key(key);
    return setting(v,key,k->fallback,k->fileMin,k->fileMax,chassis,quiet);
}
std::string json_text(const std::string& s) {
    std::string out="\"";
    for(char c:s){if(c=='"'||c=='\\')out+='\\';if(static_cast<unsigned char>(c)>=0x20)out+=c;}
    return out+"\"";
}
constexpr const char* kPanelCommandEvent="mw5.motion.command.v1";
constexpr const char* kPanelSnapshotEvent="mw5.motion.snapshot.v1";
void panel_event(const char* name,const char* data);
std::vector<std::string> list(const Config& v,const char* key) {
    std::vector<std::string> out;auto value=lookup(v,key);if(!value)return out;
    size_t start=0;
    while(start<=value->size()) {
        auto comma=value->find(',',start);if(comma==value->npos)comma=value->size();
        if(auto item=trim(value->substr(start,comma-start));!item.empty())out.push_back(item);
        start=comma+1;
    }return out;
}
class MW5Motion final:public uevr::Plugin {
    std::filesystem::path profile;
    Config cfg;
    // Game thread. `target` is what the configuration asks for; `settings` is what
    // the sampler uses and follows it smoothly after a change from the panel.
    mw5::Settings settings,target;mw5::Sampler sampler;mw5::ProfileSettings profiles;
    mw5::Identity identity;Object* appliedPawn{};std::string appliedTag;
    // Names already given this mission, so switching between lance mechs, or
    // damage that lowers a mech's top speed, does not rename a mech.
    std::map<Object*,mw5::Identity> named;
    // Gaits from mw5_mech_gait.csv in the profile folder, keyed by lower-case
    // chassis; they take precedence over the table compiled into the plugin.
    struct NamedGait { std::string chassis;mw5::Gait gait; };
    std::map<std::string,NamedGait> gaitFile;
    std::string gaitSource;
    // Optional learning of chassis that are in neither (GaitLearning = 1).
    bool learning{},learnThis{};mw5::GaitLearner learner;double learnCheckIn{};
    // Shared, under mutex.
    std::mutex mutex;mw5::Frame latest{};uint64_t tickTime{},sessionId{};double sessionSeconds{};
    mw5::Identity sharedIdentity;uint32_t identityVersion{};
    std::string gaitFileText; // complete new file content for the sender thread to write
    // Settings panel. Commands arrive on whichever thread runs the script and
    // wait in the inbox for the game thread; the results below go to the sender
    // thread under `mutex`: new effect settings, edits for mw5_motion.cfg and
    // the reply for the script.
    std::mutex panelMutex;std::vector<std::string> panelInbox;
    mw5::HapticSettings sharedHaptics;uint32_t hapticsVersion{};
    std::vector<mw5::ConfigEdit> configEdits;std::string snapshotText;
    std::atomic_bool shakerFound{false};
    // Sender thread.
    mw5::HapticSettings hapticSettings;mw5::HapticState haptics;uint32_t hapticSequence{},sentHapticsVersion{};
    mw5::Identity sentIdentity;uint32_t sentIdentityVersion{};
    Feed external{true,"127.0.0.1",20779,120}; // SimHub External Sim
    Feed debug{false,"127.0.0.1",20780,60};    // JSON copy of each frame, for tools
    uint64_t emitterId{},lastStatus{};
    ShakerReader shaker;bool shakerLogAll{};
    // Debug recording, sender thread only.
    std::ofstream rows,events;int rowHz{20};
    uint64_t started{},nextRow{},recorded{},lastFaults{};
    mw5::Frame seen{};int seenReader{-1};
    static constexpr uint64_t kRecordLimit=50ull*1024*1024;

    std::atomic_bool enabled{true},stop{false},initialized{false};
    std::thread worker;
    std::atomic<uint64_t> samples{},validSamples{},readFaults{};
    std::atomic<int> reader{static_cast<int>(Reader::NoPawn)};
    // What the sender last put in a SimHub packet, for the settings panel.
    std::atomic<float> sentRpm{};std::atomic<int> sentGear{1},sentAbs{};std::atomic<uint64_t> sentPackets{};

    // Motion settings; a "[Chassis]" block in the configuration overrides any of them.
    // `immediately` is for a new mech or the start; a change from the panel is
    // eased in by ease_settings on the following ticks.
    void load_motion(const std::string& chassis,bool immediately) {
        auto get=[&](const char* key,double fallback,double min,double max){return setting(cfg,key,fallback,min,max,chassis);};
        auto live=[&](const char* key){return live_setting(cfg,key,chassis);};
        target.yawOffsetDeg=get("CockpitYawOffsetDeg",-90,-180,180);
        target.positionHz=live("PositionSmoothingHz");
        target.velocityHz=live("VelocitySmoothingHz");
        target.accelerationHz=live("AccelerationSmoothingHz");
        target.angularHz=live("AngularSmoothingHz");
        target.heaveGain=live("HeaveGain");
        target.heaveLimitG=live("HeaveLimitG");
        target.heaveComfortG=live("HeaveComfortG");
        target.heaveSafetyNet=live("HeaveSafetyNet")!=0;
        target.bounceRatio=get("CockpitBounceRatio",0.75,0.1,2);
        // The chassis's gait: the profile folder's file first, then the built-in table.
        target.gait={};gaitSource="none";
        if(auto found=gaitFile.find(lower(chassis));!chassis.empty()&&found!=gaitFile.end()){target.gait=found->second.gait;gaitSource="mw5_mech_gait.csv";}
        else if(mw5::builtin_gait(chassis,target.gait))gaitSource="built-in table";
        target.maxAccelerationG=get("MaxAccelerationG",10,0.1,100);
        target.maxAngularSpeedDeg=get("MaxAngularSpeedDeg",720,1,3600);
        target.teleportDistanceM=get("TeleportDistanceM",10,1,1000);
        target.maxDeltaSeconds=get("MaxDeltaSeconds",0.1,0.01,0.25);
        target.eventHoldSeconds=get("EventHoldMs",100,0,2000)/1000;
        auto sign=[&](const char* name){return get(name,1,-1,1)<0?-1.0:1.0;};
        target.rollSign=sign("RollSign");target.pitchSign=sign("PitchSign");target.yawSign=sign("YawSign");
        target.lateralSign=sign("LateralSign");target.verticalSign=sign("VerticalSign");target.longitudinalSign=sign("LongitudinalSign");
        if(immediately)settings=target;
    }
    // The effect settings; all of them can change while the game runs.
    mw5::HapticSettings load_haptics() const {
        auto live=[&](const char* key){return live_setting(cfg,key);};
        mw5::HapticSettings h;
        h.gear=live("HapticGearEnable")!=0;
        h.engine=live("HapticEngineEnable")!=0;
        h.engineMaxRpm=setting(cfg,"EngineMaxRpm",10000,1,1e6);
        h.jetLevel=live("JumpJetLevel");
        h.jetWarmupLevel=live("JumpJetWarmupLevel");
        h.mascEngine=live("MascVibrationEnable")!=0;
        h.mascRpmStart=live("MascVibrationStartLevel");
        h.mascRpmDamage=live("MascVibrationDamageLevel");
        h.abs=live("MascWarningAbsEnable")!=0;
        h.absMinHz=live("MascWarningAbsMinHz");
        h.absMaxHz=live("MascWarningAbsMaxHz");
        h.powerEngine=live("PowerVibrationEnable")!=0;
        h.powerUpStart=live("PowerUpStartLevel");
        h.powerUpEnd=live("PowerUpEndLevel");
        h.powerDownStart=live("PowerDownStartLevel");
        h.powerDownEnd=live("PowerDownEndLevel");
        h.powerInitSeconds=live("PowerInitSeconds");
        h.powerUpSeconds=live("PowerUpSeconds");
        h.powerDownSeconds=live("PowerDownSeconds");
        return h;
    }
    // Game thread. Carries out what the settings panel asked for and prepares
    // the reply: the current values, the mech and what the panel may offer.
    void drain_panel() {
        std::vector<std::string> inbox;
        {std::scoped_lock lock(panelMutex);inbox.swap(panelInbox);}
        if(inbox.empty())return;
        int request=0;std::string error;bool changed=false;std::vector<mw5::ConfigEdit> edits;
        auto store=[&](const std::string& section,const std::pair<std::string,std::string>& entry,bool chassisOnly) {
            auto key=mw5::panel_key(entry.first);double number{};
            if(!key||(chassisOnly&&!mw5::panel_chassis_key(entry.first))||!mw5::panel_number(entry.second,number)){error="The plugin does not know the setting "+entry.first+".";return;}
            auto value=mw5::panel_value(*key,number);
            cfg[(section.empty()?std::string{}:lower(section)+".")+key->name]=value;
            edits.push_back({section,key->name,value,false});changed=true;
            note("settings panel: "+(section.empty()?std::string{}:"["+section+"] ")+key->name+" = "+value);
        };
        for(auto& text:inbox) {
            mw5::PanelCommand command;
            if(!mw5::parse_panel_command(text,command)||command.protocol!=1){error="The panel script and the plugin are different versions.";continue;}
            request=command.request;
            if(command.action=="set")for(auto& entry:command.values)store({},entry,false);
            else if(command.action=="set_chassis") {
                if(identity.chassis.empty())error="There is no mech to save this for.";
                else for(auto& entry:command.values)store(identity.chassis,entry,true);
            } else if(command.action=="clear_chassis") {
                for(auto key:mw5::kPanelChassisKeys)if(!identity.chassis.empty()&&cfg.erase(lower(identity.chassis)+"."+key)) {
                    edits.push_back({identity.chassis,key,{},true});changed=true;
                    note("settings panel: ["+identity.chassis+"] "+key+" removed");
                }
            } else if(command.action=="reload") {
                cfg=config_read(profile/L"mw5_motion.cfg");changed=true;
                note("settings panel: mw5_motion.cfg read again");
            } else if(command.action!="get")error="The panel script and the plugin are different versions.";
        }
        if(changed)load_motion(identity.chassis,false);
        auto reply=snapshot(request,error);
        std::scoped_lock lock(mutex);
        if(changed){sharedHaptics=load_haptics();++hapticsVersion;}
        configEdits.insert(configEdits.end(),edits.begin(),edits.end());
        snapshotText=std::move(reply);
    }
    std::string snapshot(int request,const std::string& error) const {
        auto number=[](double v){char text[32];snprintf(text,sizeof(text),"%.6g",v);return std::string(text);};
        std::string s="{\"protocol\":1,\"request\":"+std::to_string(request)+",\"version\":"+json_text(kVersion)+",\"error\":"+json_text(error)
            +",\"reader\":"+json_text(text(static_cast<Reader>(reader.load())))+",\"chassis\":"+json_text(identity.chassis)
            +",\"variant\":"+json_text(identity.variant)+",\"vehicle\":"+json_text(identity.profile)
            +",\"cap\":"+json_text(identity.chassis.empty()?std::string{}:target.gait.known()?cap_text(target):"no bounce data for this chassis: no comfort cap")
            +",\"shaker\":"+(shakerFound?"true":"false")
            +",\"weight\":"+json_text(identity.weightClass)+",\"baseMaxSpeedKmh\":"+number(identity.baseMaxSpeedKmh)
            +",\"simhub\":{\"enabled\":"+(external.enabled?"true":"false")+",\"address\":"+json_text(external.ip+":"+std::to_string(external.port))
            +",\"packets\":"+std::to_string(sentPackets.load())+",\"gear\":"+std::to_string(sentGear.load())+",\"abs\":"+std::to_string(sentAbs.load())
            +",\"rpm\":"+number(sentRpm.load())+",\"maxRpm\":"+number(setting(cfg,"EngineMaxRpm",10000,1,1e6,{},true))+"}"
            // The frame of the previous tick; this thread is the only one that writes it.
            +",\"frame\":"+mw5::json(latest)+",\"settings\":{";
        bool first=true;
        for(auto& k:mw5::kPanelKeys) {
            double v=live_setting(cfg,k.name,{},true);
            s+=(first?"":",")+json_text(k.name)+":"+(k.flag?(v!=0?"true":"false"):number(v));first=false;
        }
        s+="},\"chassis_settings\":{";first=true;
        if(!identity.chassis.empty())for(auto key:mw5::kPanelChassisKeys)if(cfg.count(lower(identity.chassis)+"."+key)) {
            s+=(first?"":",")+json_text(key)+":"+number(live_setting(cfg,key,identity.chassis,true));first=false;
        }
        s+="},\"keys\":{";first=true;
        for(auto& k:mw5::kPanelKeys) {
            s+=(first?"":",")+json_text(k.name)+":{\"min\":"+number(k.panelMin)+",\"max\":"+number(k.panelMax)+",\"def\":"+(k.flag?(k.fallback!=0?"true":"false"):number(k.fallback))+"}";first=false;
        }
        return s+"}}";
    }
    // Sender thread. Applies the panel's edits to mw5_motion.cfg through a
    // temporary file, so a crash cannot leave half a configuration behind.
    void write_config(const std::vector<mw5::ConfigEdit>& edits,uint64_t now) {
        auto path=profile/L"mw5_motion.cfg",temporary=profile/L"mw5_motion.cfg.tmp";
        std::string content;
        {std::ifstream in(path);std::stringstream all;all<<in.rdbuf();content=all.str();}
        for(auto& e:edits)content=e.remove?mw5::config_remove(content,e.section,e.key):mw5::config_set(content,e.section,e.key,e.value);
        bool written=false;
        {std::ofstream out(temporary,std::ios::trunc);out<<content;out.flush();written=out.good();}
        std::error_code failed;
        if(written)std::filesystem::rename(temporary,path,failed);
        if(!written||failed) {
            API::get()->log_error("[MW5 Motion] Could not save mw5_motion.cfg; the new settings apply until the game closes");
            if(events.is_open())event(now,"could not save mw5_motion.cfg");
        }
    }
    // Lowest speed at which the comfort cap acts for the current mech, for the log.
    // 0 when it never does below 400 km/h.
    static double cap_speed(const mw5::Settings& settings) {
        for(double kmh=1;kmh<=400;kmh+=1)if(mw5::comfort_factor(kmh,settings)<1)return kmh;
        return 0;
    }
    std::string cap_text() const {return cap_text(settings);}
    static std::string cap_text(const mw5::Settings& settings) {
        double kmh=cap_speed(settings);
        if(settings.heaveComfortG<=0)return "the comfort cap is off";
        if(kmh<=0)return "the comfort cap never acts for this mech";
        char text[80];snprintf(text,sizeof(text),"the comfort cap acts from about %.0f km/h",kmh);return text;
    }
    // Game thread, GaitLearning only. Feeds the learner and, once it knows the
    // bounce well enough, uses the result and queues it for mw5_mech_gait.csv.
    // Returns true when a new file is ready to hand to the sender thread.
    bool learn(const mw5::Sample& s,const mw5::Frame& f,float dt,std::string& file) {
        if(!learnThis||!s.bodyHeightValid)return false;
        const mw5::Gait timing{};
        learner.update(dt,s.bodyHeightM,f.GroundSpeedKmh,f.OutputActive&&s.jetState==mw5::JetsInactive,timing,settings.bounceRatio);
        if((learnCheckIn-=dt)>0||!learner.ready())return false;
        learnCheckIn=5;
        auto g=learner.gait(timing);
        // Only bother when it moved by more than a few percent.
        auto moved=[](double a,double b){return std::abs(a-b)>0.03*std::max(a,b);};
        if(!g.known()||(!moved(g.walkBounceM,settings.gait.walkBounceM)&&!moved(g.runBounceM,settings.gait.runBounceM)))return false;
        target.gait=settings.gait=g;gaitSource="learning";
        gaitFile[lower(identity.chassis)]={identity.chassis,g};
        file="# Gaits for chassis the plugin's built-in table does not cover. Written by the plugin\n"
             "# when GaitLearning = 1; rows can also be added or corrected by hand.\n"+std::string(mw5::kGaitHeader)+"\n";
        for(auto& entry:gaitFile)file+=mw5::format_gait_line(entry.second.chassis,entry.second.gait)+"\n";
        char line[240];
        snprintf(line,sizeof(line),"learned gait for %s after %d steps: walk bounce %.0f cm, run bounce %.0f cm; %s",
            identity.chassis.c_str(),learner.steps(),g.walkBounceM*100,g.runBounceM*100,cap_text().c_str());
        note(line);
        return true;
    }
    // Game thread. Returns true when the identity changed and must be shared.
    bool update_identity(const mw5::Sample& s) {
        bool changed=false;
        if(g_mech.pawn!=appliedPawn||g_mech.tag!=appliedTag) {
            appliedPawn=g_mech.pawn;appliedTag=g_mech.tag;
            identity=mw5::parse_mech_tag(appliedTag);
            if(!appliedPawn)named.clear(); // between missions; pawn addresses get reused
            else if(auto known=named.find(appliedPawn);known!=named.end()&&known->second.chassis==identity.chassis&&known->second.variant==identity.variant) {
                identity=known->second;
                note("back in a mech named earlier: \""+identity.profile+"\"");
            }
            load_motion(identity.chassis,true);changed=true;
            learnThis=learning&&!identity.chassis.empty()&&!settings.gait.known();
            learner.reset();learnCheckIn=5;
            if(!appliedTag.empty()) {
                char line[400];
                snprintf(line,sizeof(line),"mech tag %s: chassis %s, variant %s; heave gain %g, comfort %g G, ceiling %g G",appliedTag.c_str(),
                    identity.chassis.c_str(),identity.variant.c_str(),settings.heaveGain,settings.heaveComfortG,settings.heaveLimitG);
                note(line);
                const auto& g=settings.gait;
                if(g.known())snprintf(line,sizeof(line),"gait from %s: walk bounce %.0f cm at %.1f km/h, run bounce %.0f cm from %.1f km/h; %s",
                    gaitSource.c_str(),g.walkBounceM*100,g.walkSpeedKmh,g.runBounceM*100,g.runSpeedKmh,cap_text().c_str());
                else snprintf(line,sizeof(line),"no gait known for this chassis: no comfort cap, safety net %s%s",
                    settings.heaveSafetyNet?"on":"off",learnThis?", learning its bounce":"");
                note(line);
            }
        }
        // The name is chosen once per mech, from the top speed without the MASC
        // boost, and then kept so SimHub does not change profile mid-mission.
        if(identity.profile.empty()&&s.valid&&!s.mascEngaged&&s.maxSpeedKmh>1) {
            identity.baseMaxSpeedKmh=s.maxSpeedKmh;
            identity.profile=mw5::profile_name(identity.chassis,s.maxSpeedKmh,profiles);changed=true;
            named[appliedPawn]=identity;
            char line[200];
            snprintf(line,sizeof(line),"SimHub vehicle name \"%s\" (base top speed %.1f km/h)",identity.profile.c_str(),s.maxSpeedKmh);
            note(line);
        }
        return changed;
    }
    void status(const mw5::Frame& f) {
        // Written once per second; disk I/O runs only on the sender thread.
        std::ofstream out(profile/L"mw5_motion_status.json");
        out<<"{\"version\":\""<<kVersion<<"\",\"reader\":\""<<text(static_cast<Reader>(reader.load()))
           <<"\",\"samples\":"<<samples.load()<<",\"validSamples\":"<<validSamples.load()
           <<",\"readFaults\":"<<readFaults.load()
           <<",\"simHubPackets\":"<<external.sent<<",\"debugJsonPackets\":"<<debug.sent<<",\"udpErrors\":"<<external.errors+debug.errors
           <<",\"vehicleName\":\""<<sentIdentity.profile<<"\",\"chassis\":\""<<sentIdentity.chassis<<"\",\"variant\":\""<<sentIdentity.variant
           <<"\",\"baseMaxSpeedKmh\":"<<sentIdentity.baseMaxSpeedKmh
           <<",\"gear\":"<<(haptics.mascGear?2:1)<<",\"absActive\":"<<haptics.absActive<<",\"engineRpm\":"<<haptics.rpm
           <<",\"frame\":"<<mw5::json(f)<<"}";
    }
    void event(uint64_t now,const std::string& message) {
        char time[32];snprintf(time,sizeof(time),"%10.3f  ",(now-started)/1000.0);
        events<<time<<message<<"\n";events.flush();
    }
    void drain_notes(uint64_t now) {
        std::vector<std::string> notes;
        {std::scoped_lock lock(g_notesMutex);notes.swap(g_notes);}
        for(auto& n:notes)event(now,n);
        if(int r=reader.load();r!=seenReader){seenReader=r;event(now,std::string("reader: ")+text(static_cast<Reader>(r)));}
        if(auto faults=readFaults.load();faults!=lastFaults){lastFaults=faults;event(now,"a game read failed with an access violation ("+std::to_string(faults)+" so far)");}
    }
    // State changes go to the event log, samples to the CSV.
    void record(const mw5::Frame& f,uint64_t now) {
        if(!events.is_open())return;
        drain_notes(now);
        static const char* jets[]={"inactive","warm-up","firing","cooldown"};
        if(f.OutputActive!=seen.OutputActive)event(now,f.OutputActive?"output active":f.Stale?"output off: game thread stalled":f.Paused?"output off: paused":"output off");
        if(f.PhysicsDiscontinuityCounter!=seen.PhysicsDiscontinuityCounter)event(now,"motion history reset (discontinuity "+std::to_string(f.PhysicsDiscontinuityCounter)+")");
        if(f.MascEngaged!=seen.MascEngaged)event(now,std::string("MASC ")+(f.MascEngaged?"engaged":"disengaged")+", gauge "+std::to_string(f.MascGauge));
        if(f.JumpJetState!=seen.JumpJetState)event(now,std::string("jump jets ")+jets[f.JumpJetState&3]);
        if(f.MaxSpeedKmh!=seen.MaxSpeedKmh&&f.OutputActive)event(now,"top speed "+std::to_string(f.MaxSpeedKmh)+" km/h");
        bool sameFrame=f.Sequence==seen.Sequence;
        seen=f;
        if(!rows.is_open()||now<nextRow||sameFrame)return;
        nextRow=std::max(nextRow+1000/rowHz,now);
        char line[700];
        int n=snprintf(line,sizeof(line),"%.3f,%u,%.5f,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.2f,%.2f,%.2f,%.3f,%d,%.4f,%d,%.4f,%u,%d,%d,%.0f,%.2f,%.3f,%.4f,%.4f\n",
            (now-started)/1000.0,f.Sequence,f.DeltaTime,f.OutputActive,f.Paused,f.PositionX,f.PositionY,f.PositionZ,
            f.RollDegrees,f.PitchDegrees,f.YawDegrees,f.RollSpeed,f.PitchSpeed,f.YawSpeed,
            f.LocalVelocityX,f.LocalVelocityY,f.LocalVelocityZ,f.LocalAccelerationX,f.LocalAccelerationY,f.LocalAccelerationZ,
            f.SpeedKmh,f.GameSpeedKmh,f.MaxSpeedKmh,f.Throttle,f.MascEngaged,f.MascGauge,f.JumpJetState,f.HeatPercent,f.PhysicsDiscontinuityCounter,
            hapticSettings.gear&&haptics.mascGear?2:1,hapticSettings.abs&&haptics.absActive?1:0,hapticSettings.engine?haptics.rpm:0.0,
            f.GroundSpeedKmh,f.BouncePeakG,f.HeaveFactor,f.SafetyGain);
        if(n<=0)return;
        rows.write(line,n);recorded+=static_cast<uint64_t>(n);
        if(recorded>=kRecordLimit){rows.close();event(now,"sample recording stopped: size limit reached");}
    }
    void publish(mw5::Frame f,uint64_t now,uint64_t session,double seconds) {
        f.TimestampMS=static_cast<uint32_t>(now);
        // Heartbeats repeat a frame; advance the haptic state once per active sample.
        if(f.Sequence!=hapticSequence||!f.OutputActive){haptics.update(f,hapticSettings);hapticSequence=f.Sequence;}
        if(external.enabled) {
            auto packet=mw5::external_packet(f,emitterId,session,external.sent+1,seconds,haptics,hapticSettings,sentIdentity);
            external.send(std::string(reinterpret_cast<const char*>(&packet),sizeof(packet)),now);
        }
        sentRpm=hapticSettings.engine?static_cast<float>(haptics.rpm):0;sentGear=hapticSettings.gear&&haptics.mascGear?2:1;
        sentAbs=hapticSettings.abs&&haptics.absActive?1:0;sentPackets=external.sent;
        if(debug.enabled)debug.send(mw5::json(f),now);
        record(f,now);
    }
    void run() {
        uint32_t lastSequence{};uint64_t lastNeutral{};
        while(!stop) {
            uint64_t now=GetTickCount64(),tick{},session{};double seconds{};mw5::Frame f{};
            std::string gaitText,reply;std::vector<mw5::ConfigEdit> edits;
            {std::scoped_lock lock(mutex);
                f=latest;tick=tickTime;session=sessionId;seconds=sessionSeconds;gaitText.swap(gaitFileText);
                if(identityVersion!=sentIdentityVersion){sentIdentity=sharedIdentity;sentIdentityVersion=identityVersion;}
                if(hapticsVersion!=sentHapticsVersion){hapticSettings=sharedHaptics;sentHapticsVersion=hapticsVersion;}
                edits.swap(configEdits);reply.swap(snapshotText);
            }
            // Settings panel: save what it changed, then answer the script.
            if(!edits.empty())write_config(edits,now);
            if(!reply.empty())API::get()->param()->functions->dispatch_lua_event(kPanelSnapshotEvent,reply.c_str());
            if(!gaitText.empty()) {
                // Learned gaits; disk I/O stays on this thread.
                std::ofstream out(profile/L"mw5_mech_gait.csv",std::ios::trunc);out<<gaitText;
            }
            poll_shaker(now);
            // A frozen game thread must not leave the last cues standing.
            bool stale=!tick||now-tick>250;
            if(stale)f=mw5::neutral(f,true);
            // Emit once per real game tick; never invent higher-rate frames. While
            // inactive, keep an explicitly inactive heartbeat so SimHub sees the game.
            if((f.Sequence!=lastSequence)||((stale||!f.OutputActive)&&now-lastNeutral>=50)) {
                publish(f,now,session,seconds);lastSequence=f.Sequence;if(!f.OutputActive)lastNeutral=now;
            }
            if(now-lastStatus>=1000){status(f);if(events.is_open()){drain_notes(now);rows.flush();}lastStatus=now;}
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    // Sender thread. Events MechWarriorVR publishes for MechShaker; here they
    // start the power-up and shutdown vibration.
    void poll_shaker(uint64_t now) {
        bool wasOpen=shaker.view!=nullptr;
        shaker.poll(now,[&](const mw5::ShakerEvent& e) {
            if(shakerLogAll&&events.is_open()) {
                char line[200];
                snprintf(line,sizeof(line),"MechShaker event %d: int %d, floats %.3f %.3f %.3f %.3f %.3f %.3f",e.code,e.int0,e.f[0],e.f[1],e.f[2],e.f[3],e.f[4],e.f[5]);
                event(now,line);
            }
            if(e.code!=mw5::ShakerPowering)return;
            haptics.power_event(e.int0);
            static const char* phases[]={"initialising","powering up","shutting down"};
            if(events.is_open())event(now,std::string("mech power: ")+(e.int0>=0&&e.int0<=2?phases[e.int0]:"unknown phase"));
        });
        if(!wasOpen&&shaker.view&&events.is_open())event(now,"MechShaker event buffer found: power-up and shutdown vibration available");
        shakerFound=shaker.view!=nullptr;
    }
public:
    ~MW5Motion() override {
        stop=true;if(worker.joinable())worker.join();external.close();debug.close();shaker.close();
    }
    void on_initialize() override {
        if(initialized.exchange(true))return;
        auto getDir=API::get()->param()->functions->get_persistent_dir;
        int n=getDir(nullptr,0);std::vector<wchar_t> buf(static_cast<size_t>(n)+2);getDir(buf.data(),static_cast<uint32_t>(buf.size()));profile=buf.data();
        cfg=config_read(profile/L"mw5_motion.cfg");
        enabled=setting(cfg,"Enable",1,0,1)!=0;
        auto feed=[&](Feed& f,const std::string& prefix,double on){
            f.enabled=setting(cfg,(prefix+"Enable").c_str(),on,0,1)!=0;
            f.port=static_cast<int>(setting(cfg,(prefix+"Port").c_str(),f.port,1,65535));
            f.hz=static_cast<int>(setting(cfg,(prefix+"RateHz").c_str(),f.hz,1,240));
            if(auto ip=lookup(cfg,prefix+"Ip"))f.ip=*ip;};
        feed(external,"SimHub",1);feed(debug,"DebugJson",0);
        // Optional per-chassis gaits next to the configuration: corrections, modded
        // mechs, and whatever GaitLearning wrote earlier.
        {
            std::ifstream in(profile/L"mw5_mech_gait.csv");std::string line,chassis;mw5::Gait gait;
            while(std::getline(in,line))if(mw5::parse_gait_line(line,chassis,gait))gaitFile[lower(chassis)]={chassis,gait};
        }
        learning=setting(cfg,"GaitLearning",0,0,1)!=0;g_learning=learning;
        load_motion({},true);
        hapticSettings=sharedHaptics=load_haptics();
        shakerLogAll=setting(cfg,"DebugShakerEvents",0,0,1)!=0;
        for(auto old:{"MascVibrationMinLevel","MascVibrationMaxLevel"})if(lookup(cfg,old))API::get()->log_warn("[MW5 Motion] %s is no longer used; see MascVibrationStartLevel and MascVibrationDamageLevel",old);
        profiles.specificChassis=list(cfg,"SpecificChassis");
        if(auto limits=list(cfg,"SpeedBandLimitsKmh");!limits.empty()) {
            try {
                if(limits.size()!=3)throw 0;
                std::array<double,3> parsed{std::stod(limits[0]),std::stod(limits[1]),std::stod(limits[2])};
                if(!(parsed[0]>0&&parsed[0]<parsed[1]&&parsed[1]<parsed[2]))throw 0;
                profiles.limitsKmh=parsed;
            } catch(...){API::get()->log_warn("[MW5 Motion] SpeedBandLimitsKmh needs three rising numbers; using 60, 75, 100");}
        }
        if(!enabled){API::get()->log_info("[MW5 Motion] Disabled in configuration");return;}
        if(setting(cfg,"DebugLog",0,0,1)!=0) {
            rowHz=static_cast<int>(setting(cfg,"DebugLogRateHz",20,1,240));
            started=GetTickCount64();
            events.open(profile/L"mw5_motion_debug.log",std::ios::trunc);
            rows.open(profile/L"mw5_motion_debug.csv",std::ios::trunc);
            rows<<"TimeS,Sequence,DeltaTime,Active,Paused,PosX,PosY,PosZ,RollDeg,PitchDeg,YawDeg,RollSpeed,PitchSpeed,YawSpeed,"
                  "VelLateral,VelVertical,VelForward,AccLateral,AccVertical,AccForward,SpeedKmh,GameSpeedKmh,MaxSpeedKmh,Throttle,"
                  "MascEngaged,MascGauge,JumpJetState,HeatPercent,Discontinuities,Gear,AbsActive,EngineRpm,"
                  "GroundSpeedKmh,BouncePeakG,HeaveFactor,SafetyGain\n";
            g_notesEnabled=events.is_open();
            char line[400];
            snprintf(line,sizeof(line),"MW5 Motion %s debug log. SimHub %s %s:%d. CockpitYawOffsetDeg=%g, smoothing Hz: position=%g velocity=%g acceleration=%g angular=%g, %d rows/s",
                kVersion,external.enabled?"on":"off",external.ip.c_str(),external.port,settings.yawOffsetDeg,settings.positionHz,settings.velocityHz,settings.accelerationHz,settings.angularHz,rowHz);
            if(events.is_open())event(started,line);
            snprintf(line,sizeof(line),"heave: comfort cap %g G, safety net %s, soft ceiling %g G, cockpit bounce ratio %g; %d chassis in the built-in gait table, %d from mw5_mech_gait.csv, gait learning %s",
                settings.heaveComfortG,settings.heaveSafetyNet?"on":"off",settings.heaveLimitG,settings.bounceRatio,static_cast<int>(std::size(mw5::kGaitTable)),static_cast<int>(gaitFile.size()),learning?"on":"off");
            if(events.is_open())event(started,line);
        }
        WSADATA wsa{};
        if(WSAStartup(MAKEWORD(2,2),&wsa)==0){external.open();debug.open();}
        else {external.enabled=debug.enabled=false;API::get()->log_error("[MW5 Motion] WSAStartup failed");}
        emitterId=(GetTickCount64()<<32)^GetCurrentProcessId();sessionId=emitterId;
        worker=std::thread([this]{run();});
        // The settings panel talks to the plugin through events from its Lua
        // script. UEVR's plugin interface 2.39 added the callback for them, right
        // after the callbacks the vendored 2.34 header describes; on an older
        // UEVR the plugin works as before and the panel stays empty.
        auto param=API::get()->param();
        if(param->version->major==UEVR_PLUGIN_VERSION_MAJOR&&param->version->minor>=39) {
            using CustomEventCallback=void(*)(const char*,const char*);
            bool(*add)(CustomEventCallback){};
            std::memcpy(&add,reinterpret_cast<const char*>(param->callbacks)+sizeof(UEVR_PluginCallbacks),sizeof(add));
            if(add&&add(&panel_event))API::get()->log_info("[MW5 Motion] Settings panel available: LuaLoader > MW5 Motion (needs scripts\\mw5_motion.lua)");
        } else API::get()->log_info("[MW5 Motion] This UEVR version is too old for the settings panel; settings come from mw5_motion.cfg only");
        API::get()->log_info("[MW5 Motion] v%s initialized; SimHub=%s:%d",kVersion,external.ip.c_str(),external.port);
    }
    // Any thread: a command from the settings panel's script.
    void panel_command(const char* data) {
        if(!enabled||!initialized||!data)return;
        std::scoped_lock lock(panelMutex);
        if(panelInbox.size()<64)panelInbox.emplace_back(data);
    }
    void on_post_engine_tick(API::UGameEngine*,float dt) override {
        if(!enabled||!initialized)return;
        mw5::Sample s{};Reader state{};
        if(!sample_guarded(&s,dt,&state)){s={};g_mech={};++readFaults;}
        else reader=static_cast<int>(state);
        s.timestampMs=GetTickCount64();++samples;if(s.valid)++validSamples;
        bool renamed=update_identity(s);
        drain_panel();
        mw5::ease_settings(settings,target,dt);
        auto f=sampler.update(s,settings);
        std::string newGaitFile;
        bool learned=learning&&learn(s,f,dt,newGaitFile);
        std::scoped_lock lock(mutex);
        if(learned)gaitFileText=std::move(newGaitFile);
        if(s.identity&&sessionId!=(emitterId^s.identity)){sessionId=emitterId^s.identity;sessionSeconds=0;}
        if(s.valid&&!s.paused&&std::isfinite(dt)&&dt>0)sessionSeconds+=dt;
        latest=f;tickTime=s.timestampMs;
        if(renamed){sharedIdentity=identity;++identityVersion;}
    }
};
// Lifetime is the game process. UEVR has no safe callback removal for DLL unload.
auto plugin=std::make_unique<MW5Motion>();
void panel_event(const char* name,const char* data) {
    if(name&&std::strcmp(name,kPanelCommandEvent)==0&&plugin)plugin->panel_command(data);
}
}
