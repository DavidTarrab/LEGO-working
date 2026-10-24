#include "LegoAdmission.h"
#include "LegoReservoir.h"
#include "OnlinePredictor.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

int main()
{
    assert(LegoCanAdmit(2, true, true, true, 0, 1, .1));
    assert(!LegoCanAdmit(0, true, true, false, 10, 1, .1));
    assert(!LegoCanAdmit(0, true, false, true, 10, 1, .1));
    assert(LegoCanAdmit(0, true, false, false, 0, 1, .1));
    assert(!LegoCanAdmit(1, true, false, false, 10, 1, .1));
    assert(LegoCanAdmit(1, false, false, false, 2, 1, .1));
    assert(!LegoCanAdmit(0, false, false, false, 1.1, 1, .1));
    assert(!LegoCanAdmit(-1, false, false, false, 10, 1, .1));
    assert(!LegoCanAdmit(0, false, false, false, std::numeric_limits<double>::quiet_NaN(), 1, .1));
    uint64_t SampleState = 0x9E3779B97F4A7C15ull;
    std::vector<int> Sample(1000);
    for (int i = 1; i <= 1000000; ++i)
    {
        const int Slot = LegoReservoirSlot(i, static_cast<uint32_t>(Sample.size()), SampleState);
        if (i <= static_cast<int>(Sample.size())) assert(Slot == i - 1);
        if (Slot >= 0) { assert(Slot < static_cast<int>(Sample.size())); Sample[Slot] = i; }
    }
    int Early = 0, Late = 0;
    for (int Id : Sample) { assert(Id > 0 && Id <= 1000000); if (Id <= 500000) ++Early; else ++Late; }
    assert(Early > 400 && Late > 400); // ensure the timeline covers both halves
    FOnlinePredictor Predictor(3, .98);
    assert(Predictor.Predict({}) == 0);
    const std::vector<double> History{10, 10, 10};
    for (int i = 0; i < 200; ++i) Predictor.Update(10, History);
    assert(std::abs(Predictor.Predict(History) - 10) < .01);
    const double Before = Predictor.Predict(History);
    Predictor.Update(std::numeric_limits<double>::quiet_NaN(), History);
    Predictor.Update(-1, History);
    assert(Predictor.Predict(History) == Before);
    FOnlinePredictor Invalid(0, 0);
    Invalid.Update(5, {5});
    assert(std::isfinite(Invalid.Predict({5})));
    std::cout << "Admission, timeline sampling and online-prediction tests passed.\n";
}
