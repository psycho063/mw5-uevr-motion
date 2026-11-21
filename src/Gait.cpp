#include "Gait.hpp"
#include "GaitTable.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <vector>

namespace mw5 {
const char* const kGaitHeader="Chassis,WalkBounceCm,RunBounceCm,WalkCycleS,RunCycleS,StepsPerCycle,WalkSpeedKmh,RunSpeedKmh";

static bool same_name(const std::string& a,const char* b) {
    size_t i=0;
    for(;i<a.size()&&b[i];++i)
        if(std::tolower(static_cast<unsigned char>(a[i]))!=std::tolower(static_cast<unsigned char>(b[i])))return false;
    return i==a.size()&&!b[i];
}
bool builtin_gait(const std::string& chassis,Gait& out) {
    for(const auto& row:kGaitTable) {
        if(!same_name(chassis,row.chassis))continue;
        out.walkBounceM=row.walkBounceCm/100.0;out.runBounceM=row.runBounceCm/100.0;
        out.walkCycleS=row.walkCycleS;out.runCycleS=row.runCycleS;out.stepsPerCycle=row.stepsPerCycle;
        out.walkSpeedKmh=row.walkSpeedKmh;out.runSpeedKmh=row.runSpeedKmh;
        return true;
    }
    return false;
}
bool parse_gait_line(const std::string& line,std::string& chassis,Gait& out) {
    std::vector<std::string> cells;std::string cell;std::istringstream in(line);
    while(std::getline(in,cell,',')) {
        auto a=cell.find_first_not_of(" \t\r\n\""),b=cell.find_last_not_of(" \t\r\n\"");
        cells.push_back(a==cell.npos?std::string{}:cell.substr(a,b-a+1));
    }
    if(cells.size()<8||cells[0].empty()||cells[0][0]=='#')return false;
    double v[7];
    try {
        for(int i=0;i<7;++i){size_t used{};v[i]=std::stod(cells[i+1],&used);if(used!=cells[i+1].size()||!std::isfinite(v[i]))return false;}
    } catch(...){return false;} // the header line lands here
    // Reject values no animation has, so a bad file cannot drive the cap.
    if(v[0]<0||v[1]<0||v[0]>2000||v[1]>2000||(v[0]==0&&v[1]==0))return false;
    if(v[2]<0.1||v[2]>20||v[3]<0.1||v[3]>20||v[4]<1||v[4]>8)return false;
    if(v[5]<0||v[6]<=v[5]||v[6]>1000)return false;
    chassis=cells[0];
    out.walkBounceM=v[0]/100;out.runBounceM=v[1]/100;out.walkCycleS=v[2];out.runCycleS=v[3];
    out.stepsPerCycle=static_cast<int>(v[4]+0.5);out.walkSpeedKmh=v[5];out.runSpeedKmh=v[6];
    return true;
}
std::string format_gait_line(const std::string& chassis,const Gait& g) {
    char line[256];
    snprintf(line,sizeof(line),"%s,%.1f,%.1f,%.2f,%.2f,%d,%.1f,%.1f",chassis.c_str(),g.walkBounceM*100,g.runBounceM*100,
        g.walkCycleS,g.runCycleS,g.stepsPerCycle,g.walkSpeedKmh,g.runSpeedKmh);
    return line;
}

void GaitLearner::reset() { *this={}; }
void GaitLearner::update(double dt,double height,double groundKmh,bool onGround,const Gait& timing,double ratio) {
    if(!(dt>0)||!std::isfinite(height)||!std::isfinite(groundKmh)||ratio<=0){started=false;return;}
    if(!started){started=true;mean=height;below=false;clean=false;ticks=0;}
    // The slow average is the height the cockpit bobs around.
    mean+=(height-mean)*(1-std::exp(-dt/1.5));
    bool nowBelow=height<mean;
    if(below&&!nowBelow) {
        // Rising through the average: one full bob has ended and the next begins.
        double speed=ticks?speedSum/ticks:0;
        bool steady=speedHigh-speedLow<=0.15*speed+2;
        if(clean&&steady&&ticks>=4&&elapsed>=0.2&&elapsed<=2.5&&speed>=3&&high-low>0.02) {
            Stride s=stride(Gait{1,1,timing.walkCycleS,timing.runCycleS,timing.walkSpeedKmh,timing.runSpeedKmh,timing.stepsPerCycle},speed);
            // stride() of a gait with both bounces 1 gives the low-speed fade in bounceM.
            double walkPart=(1-s.blend)*s.bounceM,runPart=s.blend*s.bounceM,measured=(high-low)/ratio;
            ww+=walkPart*walkPart;wr+=walkPart*runPart;rr+=runPart*runPart;wy+=walkPart*measured;ry+=runPart*measured;
            ++count;
        }
        low=high=height;elapsed=0;speedSum=0;speedLow=speedHigh=groundKmh;ticks=0;clean=true;
    }
    below=nowBelow;
    if(!onGround)clean=false; // a jump or a pause spoils the bob in progress
    low=std::min(low,height);high=std::max(high,height);
    elapsed+=dt;speedSum+=groundKmh;++ticks;
    speedLow=std::min(speedLow,groundKmh);speedHigh=std::max(speedHigh,groundKmh);
}
Gait GaitLearner::gait(const Gait& timing) const {
    Gait g=timing;g.walkBounceM=g.runBounceM=0;
    if(!ready())return g;
    double det=ww*rr-wr*wr;
    // Walk and run can only be told apart once the mech has been seen at clearly
    // different blends; until then assume the usual proportion between them.
    if(det>0.02*ww*rr) {
        double walk=(wy*rr-ry*wr)/det,run=(ry*ww-wy*wr)/det;
        if(walk>0&&run>0&&walk<20&&run<20){g.walkBounceM=walk;g.runBounceM=run;return g;}
    }
    double k=kWalkShare,denominator=k*k*ww+2*k*wr+rr;
    if(denominator>0){g.runBounceM=std::clamp((k*wy+ry)/denominator,0.0,20.0);g.walkBounceM=k*g.runBounceM;}
    return g;
}
}
