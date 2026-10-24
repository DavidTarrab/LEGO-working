#pragma once
#include <cmath>

// Pure admission predicate shared by the engine scheduler and native tests.
// This is an experimental conservative operator gate, not a calibrated proof
// that the next rendering task cannot arrive while a CUDA operation executes.
inline bool LegoCanAdmit(int Policy, bool InFrame, bool GPUActive, bool RHIActive,
                         double RemainingMs, double EstimatedMs, double MarginMs)
{
    if (Policy == 2) return true;
    if (Policy < 0 || Policy > 2 || GPUActive || RHIActive) return false;
    if (InFrame) return Policy == 0;
    return std::isfinite(RemainingMs) && std::isfinite(EstimatedMs) && std::isfinite(MarginMs) &&
        EstimatedMs >= 0 && MarginMs >= 0 && RemainingMs > EstimatedMs + MarginMs;
}
