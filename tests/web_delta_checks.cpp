#include "web_delta.hpp"
#include <iostream>
int main() {
    const std::string base=R"({"type":"state","generation":1,"online":true,"settings":[{"id":"level.AF","value":"0.5","age":10,"name":"AF"}],"activity":["one","two"],"audio":{"devices":[{"id":"USB"}],"receiveWanted":true},"vfos":[{"frequency":7100000}]})";
    auto emit=[&](std::string before,std::string after){std::cout<<"{\"before\":"<<before<<",\"after\":"<<after<<",\"patch\":"<<web::delta::state(before,after)<<"}\n";};
    emit(base,base);
    auto change=base;change.replace(change.find("\"0.5\""),5,"\"0.8\"");change.replace(change.find("\"age\":10"),8,"\"age\":20");emit(base,change);
    change=base;change.replace(change.find("\"two\"]"),6,"\"two\",\"three \\\"quoted\\\": [ok]\"]");emit(base,change);
    change=base;change.replace(change.find("[\"one\",\"two\"]"),13,"[\"two\",\"three\"]");emit(base,change);
    change=base;change.replace(change.find("[\"one\",\"two\"]"),13,"[]");emit(base,change);
    change=base;change.replace(change.find("\"generation\":1"),14,"\"generation\":2");emit(base,change);
    change=base;change.replace(change.find("\"receiveWanted\":true"),20,"\"receiveWanted\":false");emit(base,change);
    change=base;change.replace(change.find("\"level.AF\""),10,"\"level.RF\"");emit(base,change);
}
