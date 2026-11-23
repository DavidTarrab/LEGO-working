#include "LegoStateSignal.h"
#include <cassert>
#include <future>
#include <thread>

int main()
{
    FLegoStateSignal Signal;
    Signal.Enable(true);
    const auto Before = Signal.Snapshot();
    Signal.Notify(); // Deliberately notify before entering the wait.
    auto Ready = std::async(std::launch::async, [&] { Signal.Wait(Before, 2000000); });
    assert(Ready.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);
    Ready.get();
    const auto Next = Signal.Snapshot();
    auto Blocked = std::async(std::launch::async, [&] { Signal.Wait(Next, 2000000); });
    Signal.Notify(); // Works whether the waiter has acquired its mutex or not.
    assert(Blocked.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);
    Blocked.get();
    const auto Start = std::chrono::steady_clock::now();
    Signal.Wait(Signal.Snapshot(), 2000);
    assert(std::chrono::steady_clock::now() - Start >= std::chrono::microseconds(2000));
    Signal.Enable(false);
    const auto Disabled = Signal.Snapshot();
    Signal.Notify();
    assert(Signal.Snapshot() == Disabled);
}
