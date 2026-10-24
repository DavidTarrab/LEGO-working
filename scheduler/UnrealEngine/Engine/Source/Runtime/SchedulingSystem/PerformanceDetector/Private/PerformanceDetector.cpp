#include "PerformanceDetector.h"
#include "TaskDetector.h"
#include "HAL/PlatformTime.h"

FPerformanceDetector::FPerformanceDetector() : Predictor(3, .98) {}
FPerformanceDetector& FPerformanceDetector::GetInstance()
{
    static FPerformanceDetector* Instance = new FPerformanceDetector(); return *Instance;
}
void FPerformanceDetector::RefreshFramesPerWindow()
{
    FramesPerWindow = FMath::Max(1, FMath::RoundToInt(MaxFPS * 60.0 / TargetAPM));
}
void FPerformanceDetector::UpdateCompletionLocked()
{
    if (!bCompleted && FrameBeginSeconds > 0 &&
        !FTaskDetector::GetInstance().GetIsInFrameRendering() &&
        !FTaskDetector::GetInstance().GetRenderingTaskIsRunning() &&
        !FTaskDetector::GetInstance().IsRHIWorkPending())
    {
        bCompleted = true;
        FrameEndSeconds = FPlatformTime::Seconds();
        FrameMs = 1000 * (FrameEndSeconds - FrameBeginSeconds);
        AverageMs = AverageMs == 0 ? FrameMs : .75 * AverageMs + .25 * FrameMs;
        TimeSliceMs = FMath::Max(0.0, 1000.0 / MaxFPS - FrameMs);
        AccumulatorMs += TimeSliceMs;
        if (++FrameCount >= FramesPerWindow)
        {
            const std::vector<double> Previous(History.begin(), History.end());
            if (Previous.size() >= 3) Predictor.Update(AccumulatorMs, Previous);
            History.push_back(AccumulatorMs);
            if (History.size() > 50) History.pop_front();
            AccumulatorMs = 0; FrameCount = 0;
        }
    }
}
void FPerformanceDetector::SetFrameBeginTime(double)
{
    std::lock_guard<std::mutex> Lock(Mutex);
    UpdateCompletionLocked();
    const double Now = FPlatformTime::Seconds();
    if (FrameBeginSeconds > 0)
    {
        const double Interval = 1000 * (Now - FrameBeginSeconds);
        AverageIntervalMs = AverageIntervalMs == 0 ? Interval : .9 * AverageIntervalMs + .1 * Interval;
    }
    FrameBeginSeconds = Now; bCompleted = false; FrameMs = 0; TimeSliceMs = 0;
}
void FPerformanceDetector::SetFrameTime(double Time)
{
    std::lock_guard<std::mutex> Lock(Mutex); FrameMs = FMath::Max(0.0, Time);
}
void FPerformanceDetector::SetMaxFPS(double FPS)
{
    if (!FMath::IsFinite(FPS) || FPS <= 0) return;
    std::lock_guard<std::mutex> Lock(Mutex); MaxFPS = FPS; RefreshFramesPerWindow();
}
void FPerformanceDetector::SetTargetAPM(int APM)
{
    if (APM <= 0) return;
    std::lock_guard<std::mutex> Lock(Mutex); TargetAPM = APM; RefreshFramesPerWindow();
}
void FPerformanceDetector::SetWindowsCount(int Count) { std::lock_guard<std::mutex> Lock(Mutex); WindowsCount = FMath::Max(1, Count); }
void FPerformanceDetector::SetWindowsLength(int Length) { std::lock_guard<std::mutex> Lock(Mutex); FramesPerWindow = FMath::Max(1, Length); }
void FPerformanceDetector::SetOpPolicy(int Policy) { std::lock_guard<std::mutex> Lock(Mutex); OpPolicy = Policy; }
int FPerformanceDetector::GetWindowsLength() { std::lock_guard<std::mutex> Lock(Mutex); return FramesPerWindow; }
int FPerformanceDetector::GetTargetAPM() { std::lock_guard<std::mutex> Lock(Mutex); return TargetAPM; }
double FPerformanceDetector::GetFrameBeginTime() { std::lock_guard<std::mutex> Lock(Mutex); return FrameBeginSeconds * 1e6; }
double FPerformanceDetector::GetFrameTime() { std::lock_guard<std::mutex> Lock(Mutex); UpdateCompletionLocked(); return FrameMs; }
double FPerformanceDetector::GetTimeSlice() { std::lock_guard<std::mutex> Lock(Mutex); UpdateCompletionLocked(); return TimeSliceMs; }
double FPerformanceDetector::GetRemainTimeSlice()
{
    std::lock_guard<std::mutex> Lock(Mutex); UpdateCompletionLocked();
    return bCompleted ? FMath::Max(0.0, TimeSliceMs - 1000 * (FPlatformTime::Seconds() - FrameEndSeconds)) : 0;
}
double FPerformanceDetector::GetAvgRenderingTime() { std::lock_guard<std::mutex> Lock(Mutex); return AverageMs; }
float FPerformanceDetector::GetAverageFPS() { std::lock_guard<std::mutex> Lock(Mutex); return AverageIntervalMs > 0 ? static_cast<float>(1000 / AverageIntervalMs) : 0; }
double FPerformanceDetector::PredictNextStallTimeWindow()
{
    std::lock_guard<std::mutex> Lock(Mutex);
    if (History.size() < 3) return 0;
    return FMath::Max(0.0, Predictor.Predict(std::vector<double>(History.begin(), History.end())));
}
void FPerformanceDetector::ComputeTimeSlice() { std::lock_guard<std::mutex> Lock(Mutex); UpdateCompletionLocked(); }
void FPerformanceDetector::UpdateStallTimeWindowTime() { ComputeTimeSlice(); }
// Compatibility APIs deliberately do not start the artifact's unbounded workers.
void FPerformanceDetector::ComputeStallTime() {}
void FPerformanceDetector::ComputeStallTimeInRendering() {}
void FPerformanceDetector::ComputeStallTimeInferenceUsed() {}
void FPerformanceDetector::ComputeStallTimeInferenceUnused() {}
void FPerformanceDetector::ComputeStallTimePrefillingUsed() {}
void FPerformanceDetector::WriteStallTimeInfo() {}
void FPerformanceDetector::UpdateFrameStallTime() {}
double FPerformanceDetector::UpdateAllStallTimePrefillingUsed() { return 0; }
