#pragma once
#include <cstdint>

// Uniform reservoir slot for a 1-based observation count. The caller records
// every observation in aggregate counters even when no timeline slot is kept.
inline int32_t LegoReservoirSlot(uint64_t Total, uint32_t Capacity, uint64_t& State)
{
    if (!Total || !Capacity) return -1;
    if (Total <= Capacity) return static_cast<int32_t>(Total - 1);
    State ^= State << 13; State ^= State >> 7; State ^= State << 17;
    const uint64_t Candidate = State % Total;
    return Candidate < Capacity ? static_cast<int32_t>(Candidate) : -1;
}
