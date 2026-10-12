#include "radio.hpp"
#include <iostream>
#include <atomic>
#include <stdexcept>
// Inject only PTT-off failures; every other operation uses real Hamlib Dummy.
static std::atomic<unsigned> failures{0}, releases{0};
static int checkedPtt(RIG* rig, vfo_t vfo, ptt_t status) {
    if (status == RIG_PTT_OFF) {
        ++releases;
        if (failures.load()) { --failures; return -RIG_EIO; }
    }
    return rig_set_ptt(rig,vfo,status);
}
#define rig_set_ptt checkedPtt
#include "../src/radio.cpp"
#include "../src/rigctl_protocol.cpp"
#undef rig_set_ptt
int main() {
    using namespace std::chrono_literals;
    std::mutex mutex; radio::Snapshot state;
    radio::Worker worker([&](const auto& s) { std::lock_guard lock(mutex); state=s; },[](const auto&) {});
    auto snapshot=[&] { std::lock_guard lock(mutex); return state; };
    auto wait=[&](auto condition) { const auto deadline=std::chrono::steady_clock::now()+6s;
        while(std::chrono::steady_clock::now()<deadline) { if(condition(snapshot()))return; std::this_thread::sleep_for(10ms); }
        throw std::runtime_error("PTT recovery timed out"); };
    try {
        radio::Command connect; connect.action=radio::Action::Connect;connect.connection.type=radio::ConnectionType::Demo;worker.submit(connect);
        wait([](const auto& s){return s.online;});auto generation=snapshot().generation;
        auto request=[&](const std::string& name,std::vector<std::string> args={}) { auto job=std::make_shared<radio::CatJob>();job->request={name,args,"currVFO",99,generation,false};auto future=worker.request(job);if(future.wait_for(5s)!=std::future_status::ready)throw std::runtime_error("CAT job timed out");return future.get(); };
        if(request("set_ptt",{"1"}).result!=RIG_OK)throw std::runtime_error("Dummy did not key");
        failures=2;
        if(request("__release_client").result!= -RIG_EIO)throw std::runtime_error("Release failure not injected");
        wait([](const auto& s){return s.releasePending;});
        if(request("set_ptt",{"1"}).result==RIG_OK)throw std::runtime_error("Re-key accepted during release retry");
        wait([](const auto& s){return !s.releasePending&&s.pttKnown&&!s.transmitting;});
        if(releases<3)throw std::runtime_error("CAT release was not retried");
        radio::Command key;key.action=radio::Action::PttOn;key.generation=generation;worker.submit(key);
        wait([](const auto& s){return s.dashboardPtt;});failures=1;
        key.action=radio::Action::PttOff;worker.submit(key);
        wait([](const auto& s){return s.releasePending;});
        wait([](const auto& s){return !s.dashboardPtt&&!s.releasePending&&!s.transmitting;});
        // Old client-disconnect requests cannot affect a replacement connection.
        worker.submit(connect);wait([&](const auto& s){return s.online&&s.generation!=generation;});
        if(request("__release_client").result==RIG_OK)throw std::runtime_error("Obsolete release accepted");
        worker.stop();std::cout<<"PTT release checks passed: repeated CAT failure, dashboard failure, no re-key, obsolete generations.\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
