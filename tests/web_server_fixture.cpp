#include "web_server.hpp"
#include <filesystem>
#include <iostream>
#include <thread>
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    try {
        web::Server server([](auto request) { request->reply.set_value("\"ok\":true,\"fixture\":true"); },[](const auto&){});
        server.start(std::filesystem::path(argv[1]).wstring(),"offline-fixture-passcode",true);
        server.publish(R"({"type":"state","generation":1,"settings":[],"activity":[],"online":false})");
        std::atomic<bool> done{false};
        std::thread samples([&]{while(!done){audio::SpectrumFrame audio{};audio.valid=true;audio.sampleRate=48000;audio.generation=1;server.spectrum(audio);rf_scope::Frame rf{};rf.valid=true;rf.generation=1;rf.sequence=1;server.rfSpectrum(rf,7100000,200000);std::this_thread::sleep_for(std::chrono::milliseconds(50));}});
        std::cout<<"READY\n"<<std::flush;
        std::string line;while(std::getline(std::cin,line)){if(line=="ROTATE"){server.setAccessCode("changed-fixture-passcode");std::cout<<"ROTATED\n"<<std::flush;}if(line=="STOP")break;}
        done=true;samples.join();server.stop();return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
