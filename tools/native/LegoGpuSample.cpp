#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <nvml.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

struct Nvml {
    HMODULE library = nullptr;
    bool initialized = false;
    decltype(&nvmlInit_v2) init;
    decltype(&nvmlShutdown) shutdown;
    decltype(&nvmlDeviceGetCount_v2) count;
    decltype(&nvmlDeviceGetHandleByIndex_v2) device;
    decltype(&nvmlDeviceGetUtilizationRates) utilization;
    decltype(&nvmlDeviceGetMemoryInfo) memory;
    decltype(&nvmlDeviceGetPowerUsage) power;
    template<class T> T load(const char* name) {
        auto result = GetProcAddress(library,name);
        if (!result) throw std::runtime_error(std::string("Missing NVML entry point: ") + name);
        return reinterpret_cast<T>(result);
    }
    Nvml() {
        library = LoadLibraryExW(L"nvml.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library) throw std::runtime_error("Cannot load driver-provided System32/nvml.dll.");
        try {
            init=load<decltype(init)>("nvmlInit_v2"); shutdown=load<decltype(shutdown)>("nvmlShutdown");
            count=load<decltype(count)>("nvmlDeviceGetCount_v2"); device=load<decltype(device)>("nvmlDeviceGetHandleByIndex_v2");
            utilization=load<decltype(utilization)>("nvmlDeviceGetUtilizationRates"); memory=load<decltype(memory)>("nvmlDeviceGetMemoryInfo");
            power=load<decltype(power)>("nvmlDeviceGetPowerUsage");
            if (init()!=NVML_SUCCESS) throw std::runtime_error("NVML initialization failed.");
            initialized=true;
        } catch (...) { FreeLibrary(library); library=nullptr; throw; }
    }
    ~Nvml() { if(initialized) shutdown(); if(library) FreeLibrary(library); }
};
int main(int argc,char** argv) {
    HANDLE stop=nullptr;
    try {
        std::string output,event; int interval=200;
        for(int i=1;i<argc;++i) {
            std::string key=argv[i];
            if(i+1==argc) throw std::runtime_error("Missing sampler option value.");
            std::string value=argv[++i];
            if(key=="--output") output=value;
            else if(key=="--stop-event") event=value;
            else if(key=="--interval-ms") interval=std::stoi(value);
            else throw std::runtime_error("Unknown sampler option.");
        }
        if(output.empty()||event.empty()||interval<20||interval>10000) throw std::runtime_error("Specify --output PATH --stop-event NAME [--interval-ms 200].");
        stop=OpenEventA(SYNCHRONIZE,FALSE,event.c_str());
        if(!stop) throw std::runtime_error("Sampler stop event does not exist.");
        Nvml nvml;
        unsigned int count=0;
        if(nvml.count(&count)!=NVML_SUCCESS||!count) throw std::runtime_error("NVML found no GPU devices.");
        std::ofstream file(std::filesystem::u8path(output));
        if(!file) throw std::runtime_error("Cannot open GPU sample file.");
        file << "timestamp,index,utilization.gpu [%],memory.used [MiB],memory.total [MiB],power.draw [W]\n" << std::fixed << std::setprecision(3);
        do {
            for(unsigned int i=0;i<count;++i) {
                nvmlDevice_t device;
                if(nvml.device(i,&device)!=NVML_SUCCESS) continue;
                SYSTEMTIME t; GetLocalTime(&t);
                char stamp[64]; std::snprintf(stamp,sizeof(stamp),"%04d/%02d/%02d %02d:%02d:%02d.%03d",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);
                nvmlUtilization_t util{}; nvmlMemory_t memory{}; unsigned int power=0;
                bool hasUtil=nvml.utilization(device,&util)==NVML_SUCCESS;
                bool hasMemory=nvml.memory(device,&memory)==NVML_SUCCESS;
                bool hasPower=nvml.power(device,&power)==NVML_SUCCESS;
                file << stamp << ',' << i << ',';
                if(hasUtil) file << util.gpu;
                file << ','; if(hasMemory) file << double(memory.used)/1048576.0;
                file << ','; if(hasMemory) file << double(memory.total)/1048576.0;
                file << ','; if(hasPower) file << double(power)/1000.0;
                file << '\n';
            }
            file.flush();
            if(!file) throw std::runtime_error("GPU sample write failed.");
        } while(WaitForSingleObject(stop,static_cast<DWORD>(interval))==WAIT_TIMEOUT);
        CloseHandle(stop); stop=nullptr;
        return 0;
    } catch(const std::exception& e) { if(stop) CloseHandle(stop); std::cerr << e.what() << '\n'; return 1; }
}
