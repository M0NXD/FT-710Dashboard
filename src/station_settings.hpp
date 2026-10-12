#pragma once
#include "radio.hpp"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <optional>
#include <initializer_list>
#include <stdexcept>

namespace station_settings {
using Fields=std::map<std::string,std::string>;
inline std::string get(const Fields& f,const char* key){auto it=f.find(key);return it==f.end()?"":it->second;}
inline int choice(const std::string& value,std::initializer_list<int> allowed,const char* label){
    for(int candidate:allowed)if(value==std::to_string(candidate))return candidate;
    throw std::invalid_argument(std::string("Unsupported ")+label);
}
inline radio::Connection connection(const Fields& fields){
    radio::Connection result;result.type=static_cast<radio::ConnectionType>(choice(get(fields,"kind"),{0,1,2},"connection type"));
    result.endpoint=get(fields,"endpoint");
    if(result.endpoint.size()>240||result.endpoint.find_first_of("\r\n")!=std::string::npos)throw std::invalid_argument("Invalid endpoint");
    result.baud=choice(get(fields,"baud"),{4800,9600,19200,38400,115200},"CAT baud");
    result.stopBits=choice(get(fields,"stopBits"),{1,2},"stop bits");
    result.pollMs=choice(get(fields,"poll"),{500,750,1000,1500,2000},"poll interval");
    result.handshake=get(fields,"flow");
    if(result.handshake!="None"&&result.handshake!="Hardware"&&result.handshake!="XONXOFF")throw std::invalid_argument("Unsupported flow control");
    return result;
}
struct Scope { std::string port;unsigned span=7,baud=4800;std::optional<double> level; };
inline Scope scope(const Fields& fields,bool requirePort=false){
    Scope result;result.port=get(fields,"rfCat2Port");
    const bool validPort=result.port.size()>=4&&result.port.size()<=15&&
        std::toupper(static_cast<unsigned char>(result.port[0]))=='C'&&std::toupper(static_cast<unsigned char>(result.port[1]))=='O'&&
        std::toupper(static_cast<unsigned char>(result.port[2]))=='M'&&std::all_of(result.port.begin()+3,result.port.end(),[](unsigned char c){return std::isdigit(c);});
    if((requirePort||!result.port.empty())&&!validPort)throw std::invalid_argument("Enter the radio Standard / CAT-2 COM port, for example COM5");
    result.span=choice(get(fields,"rfSpan"),{0,1,2,3,4,5,6,7,8,9},"RF span");
    result.baud=choice(get(fields,"rfCat2Baud"),{4800,9600,19200,38400,115200},"CAT-2 baud");
    if(fields.contains("rfWaterfallLevel")){const auto text=get(fields,"rfWaterfallLevel");std::size_t count=0;double level=std::stod(text,&count);
        if(count!=text.size()||!std::isfinite(level)||level < -30||level > 30||std::abs(level*2-std::round(level*2))>.001)throw std::invalid_argument("Waterfall level must be -30 to +30 dB in 0.5 dB steps");
        result.level=level;}
    return result;
}
}
