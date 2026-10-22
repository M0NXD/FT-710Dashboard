#include "web_server.hpp"
#include <filesystem>
#include <iostream>
#include <thread>
#include <syncstream>
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    try {
        web::Server server([](auto request) {
            std::osyncstream(std::cout) << (request->fields["action"] == "browserGone" ? "INACTIVE " : "CONTROL ") << request->fields["seq"] << '\n' << std::flush;
            request->reply.set_value("\"ok\":true,\"fixture\":true");
        },[](const auto& message){ std::osyncstream(std::cout) << "LOG " << message << '\n' << std::flush; });
        server.start(std::filesystem::path(argv[1]).wstring(),"offline-fixture-passcode",true);
        server.publish(R"({"type":"state","generation":1,"settings":[],"activity":[],"online":false})");
        std::atomic<bool> done{false}, flood{false};
        std::thread samples([&]{unsigned sequence=0;const std::string padding(262144,'x');while(!done){
            if(flood)server.publish("{\"type\":\"state\",\"generation\":1,\"settings\":[],\"activity\":[],\"online\":false,\"padding\":\"" + std::to_string(++sequence) + padding + "\"}");
            audio::SpectrumFrame audio{};audio.valid=true;audio.sampleRate=48000;audio.generation=1;server.spectrum(audio);rf_scope::Frame rf{};rf.valid=true;rf.generation=1;rf.sequence=1;server.rfSpectrum(rf,7100000,200000);std::this_thread::sleep_for(std::chrono::milliseconds(50));}});
        std::cout<<"READY\n"<<std::flush;
        std::string line;while(std::getline(std::cin,line)){
            if(line=="FLOOD"){flood=true;std::osyncstream(std::cout)<<"FLOODING\n"<<std::flush;}
            if(line=="QUIET"){flood=false;std::osyncstream(std::cout)<<"QUIET\n"<<std::flush;}
            if(line=="ROTATE"){server.setAccessCode("changed-fixture-passcode");std::osyncstream(std::cout)<<"ROTATED\n"<<std::flush;}
            if(line=="STOP")break;
        }
        done=true;samples.join();server.stop();return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
