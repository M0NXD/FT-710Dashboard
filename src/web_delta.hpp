#pragma once
#include <map>
#include <string>
#include <vector>

namespace web::delta {
// Operates only on JSON produced by publishWeb, never on client input.
inline std::vector<std::string> elements(const std::string& json) {
    std::vector<std::string> values; if(json.size()<2)return values;
    bool quoted=false,escape=false;int depth=0;std::size_t begin=1;
    for(std::size_t i=1;i+1<json.size();++i){const char c=json[i];
        if(quoted){if(escape)escape=false;else if(c=='\\')escape=true;else if(c=='"')quoted=false;continue;}
        if(c=='"')quoted=true;else if(c=='{'||c=='[')++depth;else if(c=='}'||c==']')--depth;
        else if(c==','&&!depth){values.push_back(json.substr(begin,i-begin));begin=i+1;}}
    if(begin+1<json.size())values.push_back(json.substr(begin,json.size()-1-begin));
    return values;
}
inline std::map<std::string,std::string> fields(const std::string& json) {
    std::map<std::string,std::string> result;
    for(const auto& item:elements(json)){auto colon=item.find(':');if(colon!=std::string::npos)result[item.substr(0,colon)]=item.substr(colon+1);}return result;
}
inline std::string object(const std::map<std::string,std::string>& values) {
    std::string out="{";for(const auto& [key,value]:values){if(out.size()>1)out+=',';out+=key+':'+value;}return out+'}';
}
inline std::string difference(const std::string& before,const std::string& after) {
    const auto a=fields(before),b=fields(after);std::map<std::string,std::string> changed;
    for(const auto& [key,value]:b){auto previous=a.find(key);if(previous!=a.end()&&previous->second==value)continue;
        if(previous!=a.end()&&!value.empty()&&value.front()=='{'&&!previous->second.empty()&&previous->second.front()=='{')changed[key]=difference(previous->second,value);
        else changed[key]=value;}
    return object(changed);
}
inline std::string state(const std::string& before,const std::string& after) {
    if(before.empty())return after;
    const auto a=fields(before),b=fields(after);
    if(!a.contains("\"generation\"")||a.at("\"generation\"")!=b.at("\"generation\""))return after;
    auto changed=fields(difference(before,after));changed.erase("\"settings\"");changed.erase("\"activity\"");changed.erase("\"type\"");
    auto oldSettings=elements(a.at("\"settings\"")),newSettings=elements(b.at("\"settings\""));
    bool sameCatalog=oldSettings.size()==newSettings.size();
    for(std::size_t i=0;sameCatalog&&i<newSettings.size();++i)sameCatalog=fields(oldSettings[i]).at("\"id\"")==fields(newSettings[i]).at("\"id\"");
    std::string updates="[";
    if(sameCatalog){for(std::size_t i=0;i<newSettings.size();++i){if(oldSettings[i]==newSettings[i])continue;auto patch=fields(difference(oldSettings[i],newSettings[i]));patch["\"id\""]=fields(newSettings[i]).at("\"id\"");if(updates.size()>1)updates+=',';updates+=object(patch);}}
    else changed["\"settings\""]=b.at("\"settings\"");
    updates+=']';
    const auto oldLog=elements(a.at("\"activity\"")),newLog=elements(b.at("\"activity\""));std::size_t remove=0,keep=0;
    if(!newLog.empty()){while(remove<oldLog.size()&&oldLog[remove]!=newLog[0])++remove;while(remove+keep<oldLog.size()&&keep<newLog.size()&&oldLog[remove+keep]==newLog[keep])++keep;}
    else remove=oldLog.size();
    // Append-only logs normally retain their complete prefix. A clear/trim
    // removes the old prefix; unrelated replacements reset the log.
    if(remove+keep!=oldLog.size()){remove=oldLog.size();keep=0;}
    std::string append="[";for(std::size_t i=keep;i<newLog.size();++i){if(append.size()>1)append+=',';append+=newLog[i];}append+=']';
    return "{\"type\":\"delta\",\"fields\":"+object(changed)+",\"settings\":"+updates+",\"activity\":{\"remove\":"+std::to_string(remove)+",\"append\":"+append+"}}";
}
}
