#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The in-game settings panel (scripts\mw5_motion.lua in the UEVR profile) and
// what it needs from the plugin: which settings can change while the game runs,
// the commands the script sends, and edits to mw5_motion.cfg that keep the
// file's comments and order. No game or UEVR dependency.
namespace mw5 {
// A setting that takes effect while the game runs. The file accepts fileMin to
// fileMax; the panel's sliders offer the narrower panelMin to panelMax, so a
// slip of the hand cannot send an extreme value to a moving platform.
struct PanelKey {
    const char* name;
    double fallback, fileMin, fileMax, panelMin, panelMax;
    bool flag; // 0 or 1, shown as a checkbox
};
inline constexpr PanelKey kPanelKeys[]={
    {"HeaveComfortG",3,0,100,1,6,false},
    {"HeaveGain",1,0,10,0,2,false},
    {"HeaveLimitG",6,0,100,2,10,false},
    {"HeaveSafetyNet",1,0,1,0,1,true},
    {"PositionSmoothingHz",2,0,120,0.5,10,false},
    {"VelocitySmoothingHz",2,0,120,0.5,10,false},
    {"AccelerationSmoothingHz",2,0,120,0.5,10,false},
    {"AngularSmoothingHz",2,0,120,0.5,10,false},
    {"HapticGearEnable",1,0,1,0,1,true},
    {"HapticEngineEnable",1,0,1,0,1,true},
    {"JumpJetLevel",0.8,0,1,0,1,false},
    {"JumpJetWarmupLevel",0.4,0,1,0,1,false},
    {"MascVibrationEnable",1,0,1,0,1,true},
    {"MascVibrationStartLevel",0.9,0,1,0,1,false},
    {"MascVibrationDamageLevel",0.5,0,1,0,1,false},
    {"MascWarningAbsEnable",1,0,1,0,1,true},
    {"MascWarningAbsMinHz",2,0.5,15,0.5,15,false},
    {"MascWarningAbsMaxHz",6,0.5,15,0.5,15,false},
    {"PowerVibrationEnable",1,0,1,0,1,true},
    {"PowerUpStartLevel",0.45,0,1,0,1,false},
    {"PowerUpEndLevel",0.85,0,1,0,1,false},
    {"PowerDownStartLevel",0.85,0,1,0,1,false},
    {"PowerDownEndLevel",0.45,0,1,0,1,false},
    {"PowerInitSeconds",5,0,60,0,10,false},
    {"PowerUpSeconds",4,0,60,0,10,false},
    {"PowerDownSeconds",3,0,60,0,10,false},
    {"PartDestructionEnable",1,0,1,0,1,true},
    {"PartVibrationEnable",1,0,1,0,1,true},
    {"PartKickStrength",1,0,3,0,2,false},
    {"PartArmFactor",0.4,0,2,0,1.5,false},
    {"PartLegFactor",0.6,0,2,0,1.5,false},
    {"PartSideTorsoFactor",0.8,0,2,0,1.5,false},
    {"PartCoreFactor",1,0,2,0,1.5,false},
    {"StompEnable",1,0,1,0,1,true},
    {"StompLightStrength",0.25,0,2,0,2,false},
    {"StompMediumStrength",0.5,0,2,0,2,false},
    {"StompHeavyStrength",0.75,0,2,0,2,false},
    {"StompAssaultStrength",1,0,2,0,2,false},
};
inline const PanelKey* panel_key(std::string_view name) {
    for(auto& k:kPanelKeys)if(name==k.name)return &k;
    return nullptr;
}
// The settings the panel may store for one chassis.
inline constexpr const char* kPanelChassisKeys[]={"HeaveComfortG","HeaveGain"};
inline bool panel_chassis_key(std::string_view name) {
    for(auto k:kPanelChassisKeys)if(name==k)return true;
    return false;
}
// The text written to the file for a value from the panel: inside the panel's
// range, flags as 0 or 1, numbers with at most two decimals.
inline std::string panel_value(const PanelKey& k,double v) {
    if(k.flag)return v!=0?"1":"0";
    v=std::round(std::clamp(v,k.panelMin,k.panelMax)*100)/100;
    char text[32];snprintf(text,sizeof(text),"%.2f",v);
    std::string s=text;
    while(s.back()=='0')s.pop_back();
    if(s.back()=='.')s.pop_back();
    return s;
}
// A number or "true"/"false" as the script sends them.
inline bool panel_number(const std::string& text,double& out) {
    if(text=="true"){out=1;return true;}
    if(text=="false"){out=0;return true;}
    char* end{};out=std::strtod(text.c_str(),&end);
    return !text.empty()&&end==text.c_str()+text.size()&&std::isfinite(out);
}

// A command from the script: lines of protocol, request number, action, then
// any number of "Key=value".
struct PanelCommand {
    int protocol{}, request{};
    std::string action;
    std::vector<std::pair<std::string,std::string>> values;
};
inline bool parse_panel_command(std::string_view text,PanelCommand& out) {
    std::vector<std::string> lines;size_t start=0;
    while(start<=text.size()) {
        auto end=text.find('\n',start);if(end==text.npos)end=text.size();
        lines.emplace_back(text.substr(start,end-start));start=end+1;
    }
    if(lines.size()<3||lines[2].empty())return false;
    double protocol{},request{};
    if(!panel_number(lines[0],protocol)||!panel_number(lines[1],request))return false;
    out={static_cast<int>(protocol),static_cast<int>(request),lines[2],{}};
    for(size_t i=3;i<lines.size();++i) {
        auto equals=lines[i].find('=');
        if(equals==std::string::npos||equals==0)continue;
        out.values.emplace_back(lines[i].substr(0,equals),lines[i].substr(equals+1));
    }
    return true;
}

// One change to mw5_motion.cfg. An empty section is the general part of the
// file; otherwise it is the chassis of a "[Chassis]" block.
struct ConfigEdit {
    std::string section, key, value;
    bool remove{};
};
namespace panel_detail {
inline std::string lower(std::string s){for(auto& c:s)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return s;}
inline std::string trim(const std::string& s) {
    auto a=s.find_first_not_of(" \t\r");auto b=s.find_last_not_of(" \t\r");
    return a==s.npos?std::string{}:s.substr(a,b-a+1);
}
struct Line {
    std::string text;
    std::string section; // lower case, of the block the line is in
    std::string key;     // set for "Key = value" lines
    bool header{};
};
inline std::vector<Line> lines_of(const std::string& text) {
    std::vector<Line> lines;std::string section;size_t start=0;
    while(start<text.size()) {
        auto end=text.find('\n',start);if(end==text.npos)end=text.size();
        Line line{text.substr(start,end-start),section,{},false};
        auto code=trim(line.text.substr(0,line.text.find('#')));
        if(code.size()>2&&code.front()=='['&&code.back()==']') {
            section=lower(trim(code.substr(1,code.size()-2)));line.section=section;line.header=true;
        } else if(auto equals=code.find('=');equals!=code.npos)line.key=trim(code.substr(0,equals));
        lines.push_back(std::move(line));start=end+1;
    }
    return lines;
}
inline std::string join(const std::vector<Line>& lines) {
    std::string text;
    for(auto& line:lines){text+=line.text;text+='\n';}
    return text;
}
}
// Sets Key in the file text and returns the new text. An existing line keeps its
// place and any comment after the value; everything else is left as it is. A
// missing general key is added before the first chassis block; a missing
// chassis key is added to that chassis's block, which is created at the end of
// the file if needed.
inline std::string config_set(const std::string& text,const std::string& section,const std::string& key,const std::string& value) {
    using namespace panel_detail;
    auto lines=lines_of(text);auto wanted=lower(section);bool found=false;
    for(auto& line:lines) {
        if(line.header||line.key!=key||line.section!=wanted)continue;
        auto hash=line.text.find('#');
        line.text=key+" = "+value+(hash==std::string::npos?"":" "+line.text.substr(hash));found=true;
    }
    if(found)return join(lines);
    Line added{key+" = "+value,wanted,key,false};
    if(wanted.empty()) {
        auto header=std::find_if(lines.begin(),lines.end(),[](const Line& l){return l.header;});
        lines.insert(header,std::move(added));
        return join(lines);
    }
    // After the last line with content in the chassis's block.
    size_t at=lines.size();bool has=false;
    for(size_t i=0;i<lines.size();++i)if(lines[i].section==wanted&&(lines[i].header||!trim(lines[i].text).empty())){at=i+1;has=true;}
    if(!has) {
        if(!lines.empty()&&!trim(lines.back().text).empty())lines.push_back({});
        lines.push_back({"["+section+"]",wanted,{},true});at=lines.size();
    }
    lines.insert(lines.begin()+static_cast<std::ptrdiff_t>(at),std::move(added));
    return join(lines);
}
// Removes Key from a chassis block, and the block's heading when nothing else
// is left in it.
inline std::string config_remove(const std::string& text,const std::string& section,const std::string& key) {
    using namespace panel_detail;
    auto lines=lines_of(text);auto wanted=lower(section);
    if(wanted.empty())return text;
    lines.erase(std::remove_if(lines.begin(),lines.end(),[&](const Line& l){return !l.header&&l.key==key&&l.section==wanted;}),lines.end());
    bool content=std::any_of(lines.begin(),lines.end(),[&](const Line& l){return !l.header&&l.section==wanted&&!trim(l.text).empty();});
    if(!content) {
        lines.erase(std::remove_if(lines.begin(),lines.end(),[&](const Line& l){return l.section==wanted;}),lines.end());
        while(!lines.empty()&&trim(lines.back().text).empty())lines.pop_back();
    }
    return join(lines);
}
}
