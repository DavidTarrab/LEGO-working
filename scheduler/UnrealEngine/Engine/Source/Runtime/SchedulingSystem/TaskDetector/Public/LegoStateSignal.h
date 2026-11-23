#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

// Capture a generation BEFORE reading admission state. A notification between
// that read and Wait cannot be lost: the generation predicate sees it.
class FLegoStateSignal
{
public:
    void Enable(bool Value) { Enabled.store(Value); }
    uint64_t Snapshot() const { return Generation.load(); }
    void Notify()
    {
        if (!Enabled.load()) return;
        { std::lock_guard<std::mutex> Lock(Mutex); ++Generation; }
        Changed.notify_all();
    }
    void Wait(uint64_t Observed, int Microseconds)
    {
        std::unique_lock<std::mutex> Lock(Mutex);
        Changed.wait_for(Lock, std::chrono::microseconds(Microseconds),
            [&] { return Generation.load() != Observed; });
    }
private:
    std::atomic<bool> Enabled{false};
    std::atomic<uint64_t> Generation{0};
    std::mutex Mutex;
    std::condition_variable Changed;
};
