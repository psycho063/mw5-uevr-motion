#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include "uevr/Plugin.hpp"
#include "Telemetry.hpp"
#include "SimHub.hpp"
#include "Gait.hpp"
#include "GaitTable.hpp"
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
#include <string>
#include <thread>
#include <vector>

using API=uevr::API;
using Object=API::UObject;
namespace {
constexpr const char* kVersion="0.7.1";
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
double setting(const Config& v,const char* key,double fallback,double min,double max,const std::string& chassis={}) {
    auto value=lookup(v,key,chassis);if(!value)return fallback;
    try {size_t used{};double n=std::stod(*value,&used);if(used!=value->size()||!std::isfinite(n)||n<min||n>max)throw 0;return n;}
    catch(...){API::get()->log_warn("[MW5 Motion] Invalid config %s; using default",key);return fallback;}
}
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
    // Game thread.
    mw5::Settings settings;mw5::Sampler sampler;mw5::ProfileSettings profiles;
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
    // Sender thread.
    mw5::HapticSettings hapticSettings;mw5::HapticState haptics;uint32_t hapticSequence{};
    mw5::Identity sentIdentity;uint32_t sentIdentityVersion{};
    Feed external{true,"127.0.0.1",20779,120}; // SimHub External Sim
    Feed debug{false,"127.0.0.1",20780,60};    // JSON copy of each frame, for tools
    uint64_t emitterId{},lastStatus{};
    // Debug recording, sender thread only.
    std::ofstream rows,events;int rowHz{20};
    uint64_t started{},nextRow{},recorded{},lastFaults{};
    mw5::Frame seen{};int seenReader{-1};
    static constexpr uint64_t kRecordLimit=50ull*1024*1024;

    std::atomic_bool enabled{true},stop{false},initialized{false};
    std::thread worker;
    std::atomic<uint64_t> samples{},validSamples{},readFaults{};
    std::atomic<int> reader{static_cast<int>(Reader::NoPawn)};

    // Motion settings; a "[Chassis]" block in the configuration overrides any of them.
    void load_motion(const std::string& chassis) {
        auto get=[&](const char* key,double fallback,double min,double max){return setting(cfg,key,fallback,min,max,chassis);};
        settings.yawOffsetDeg=get("CockpitYawOffsetDeg",-90,-180,180);
        settings.positionHz=get("PositionSmoothingHz",2,0,120);
        settings.velocityHz=get("VelocitySmoothingHz",2,0,120);
        settings.accelerationHz=get("AccelerationSmoothingHz",2,0,120);
        settings.angularHz=get("AngularSmoothingHz",2,0,120);
        settings.heaveGain=get("HeaveGain",1,0,10);
        settings.heaveLimitG=get("HeaveLimitG",6,0,100);
        settings.heaveComfortG=get("HeaveComfortG",3,0,100);
        settings.heaveSafetyNet=get("HeaveSafetyNet",1,0,1)!=0;
        settings.bounceRatio=get("CockpitBounceRatio",0.75,0.1,2);
        // The chassis's gait: the profile folder's file first, then the built-in table.
        settings.gait={};gaitSource="none";
        if(auto found=gaitFile.find(lower(chassis));!chassis.empty()&&found!=gaitFile.end()){settings.gait=found->second.gait;gaitSource="mw5_mech_gait.csv";}
        else if(mw5::builtin_gait(chassis,settings.gait))gaitSource="built-in table";
        settings.maxAccelerationG=get("MaxAccelerationG",10,0.1,100);
        settings.maxAngularSpeedDeg=get("MaxAngularSpeedDeg",720,1,3600);
        settings.teleportDistanceM=get("TeleportDistanceM",10,1,1000);
        settings.maxDeltaSeconds=get("MaxDeltaSeconds",0.1,0.01,0.25);
        settings.eventHoldSeconds=get("EventHoldMs",100,0,2000)/1000;
        auto sign=[&](const char* name){return get(name,1,-1,1)<0?-1.0:1.0;};
        settings.rollSign=sign("RollSign");settings.pitchSign=sign("PitchSign");settings.yawSign=sign("YawSign");
        settings.lateralSign=sign("LateralSign");settings.verticalSign=sign("VerticalSign");settings.longitudinalSign=sign("LongitudinalSign");
    }
    // Lowest speed at which the comfort cap acts for the current mech, for the log.
    // 0 when it never does below 400 km/h.
    double cap_speed() const {
        for(double kmh=1;kmh<=400;kmh+=1)if(mw5::comfort_factor(kmh,settings)<1)return kmh;
        return 0;
    }
    std::string cap_text() const {
        double kmh=cap_speed();
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
        settings.gait=g;gaitSource="learning";
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
            load_motion(identity.chassis);changed=true;
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
        if(debug.enabled)debug.send(mw5::json(f),now);
        record(f,now);
    }
    void run() {
        uint32_t lastSequence{};uint64_t lastNeutral{};
        while(!stop) {
            uint64_t now=GetTickCount64(),tick{},session{};double seconds{};mw5::Frame f{};
            std::string gaitText;
            {std::scoped_lock lock(mutex);
                f=latest;tick=tickTime;session=sessionId;seconds=sessionSeconds;gaitText.swap(gaitFileText);
                if(identityVersion!=sentIdentityVersion){sentIdentity=sharedIdentity;sentIdentityVersion=identityVersion;}
            }
            if(!gaitText.empty()) {
                // Learned gaits; disk I/O stays on this thread.
                std::ofstream out(profile/L"mw5_mech_gait.csv",std::ios::trunc);out<<gaitText;
            }
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
public:
    ~MW5Motion() override {
        stop=true;if(worker.joinable())worker.join();external.close();debug.close();
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
        load_motion({});
        hapticSettings.gear=setting(cfg,"HapticGearEnable",1,0,1)!=0;
        hapticSettings.engine=setting(cfg,"HapticEngineEnable",1,0,1)!=0;
        hapticSettings.engineMaxRpm=setting(cfg,"EngineMaxRpm",10000,1,1e6);
        hapticSettings.jetLevel=setting(cfg,"JumpJetLevel",0.8,0,1);
        hapticSettings.jetWarmupLevel=setting(cfg,"JumpJetWarmupLevel",0.4,0,1);
        hapticSettings.mascEngine=setting(cfg,"MascVibrationEnable",1,0,1)!=0;
        hapticSettings.mascRpmStart=setting(cfg,"MascVibrationStartLevel",0.9,0,1);
        hapticSettings.mascRpmDamage=setting(cfg,"MascVibrationDamageLevel",0.5,0,1);
        hapticSettings.abs=setting(cfg,"MascWarningAbsEnable",1,0,1)!=0;
        hapticSettings.absMinHz=setting(cfg,"MascWarningAbsMinHz",2,0.5,15);
        hapticSettings.absMaxHz=setting(cfg,"MascWarningAbsMaxHz",6,0.5,15);
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
        API::get()->log_info("[MW5 Motion] v%s initialized; SimHub=%s:%d",kVersion,external.ip.c_str(),external.port);
    }
    void on_post_engine_tick(API::UGameEngine*,float dt) override {
        if(!enabled||!initialized)return;
        mw5::Sample s{};Reader state{};
        if(!sample_guarded(&s,dt,&state)){s={};g_mech={};++readFaults;}
        else reader=static_cast<int>(state);
        s.timestampMs=GetTickCount64();++samples;if(s.valid)++validSamples;
        bool renamed=update_identity(s);
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
}
