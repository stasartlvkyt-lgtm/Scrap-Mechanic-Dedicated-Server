#include "steam/probe.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
template<typename F>void rejects(F f){try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid probe request accepted");}
struct Temporary {
    std::filesystem::path path=std::filesystem::temp_directory_path()/("scrapds-steam-guards-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temporary(){std::filesystem::create_directories(path/"Release");}
    ~Temporary(){std::error_code ignored;std::filesystem::remove_all(path,ignored);}
};
}

int main(){
    try{
        Temporary temporary;
        rejects([&]{scrapds::probe_steam_identity(temporary.path,0);});
        rejects([&]{scrapds::probe_steam_identity(temporary.path,31);});
        rejects([&]{scrapds::run_steam_listener(temporary.path,temporary.path/"output",0,1);});
        rejects([&]{scrapds::run_steam_listener(temporary.path,temporary.path/"output",1ULL,301);});
        rejects([&]{scrapds::run_steam_listener(temporary.path,temporary.path/"output",1ULL,0,1,true,true,{}, {},false,true);});
        rejects([&]{scrapds::run_steam_listener(temporary.path,temporary.path/"output",1ULL,0,1,true,true,{}, {},false,false,true);});
        {std::ofstream(temporary.path/"Release/ScrapMechanic.exe")<<"unsupported executable";std::ofstream(temporary.path/"Release/steam_api64.dll")<<"not a DLL";}
#ifdef _WIN32
        bool reached_binary_guard=false;
        try {scrapds::run_steam_listener(temporary.path,temporary.path/"indefinite",1ULL,0,0);}
        catch(const std::runtime_error& error) {reached_binary_guard=std::string(error.what()).find("Unsupported ScrapMechanic.exe SHA256")!=std::string::npos;}
        if(!reached_binary_guard)throw std::runtime_error("indefinite listener did not reach binary validation");
#endif
        // Valid timeout enters the stdout guard, then the hash check must reject
        // this fixture before any DLL loading or Steam/network initialization.
        rejects([&]{scrapds::probe_steam_identity(temporary.path,1);});
        if(std::filesystem::exists(temporary.path/"output"))throw std::runtime_error("Invalid listener created output");
#ifdef _WIN32
        // Exercise restoration of the OS handle independently from CRT output.
        constexpr char prefix[]="{\"native_stdout_restored\":true,";
        DWORD written{};
        if(!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),prefix,sizeof(prefix)-1,&written,nullptr)||written!=sizeof(prefix)-1)throw std::runtime_error("OS stdout handle was not restored after probe exception");
        std::cout<<"\"crt_stdout_restored\":true,\"tests\":\"passed\"}\n";
#else
        std::cout<<"{\"tests\":\"passed\",\"steam_platform\":\"unsupported\"}\n";
#endif
        return 0;
    }catch(const std::exception& error){std::cerr<<"steam_probe_tests: "<<error.what()<<'\n';return 1;}
}
