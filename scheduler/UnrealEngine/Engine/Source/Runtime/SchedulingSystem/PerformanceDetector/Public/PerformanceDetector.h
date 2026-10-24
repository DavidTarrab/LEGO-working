#pragma once
#include "CoreMinimal.h"
#include "OnlinePredictor.h"
#include <deque>
#include <mutex>
#include <vector>

// Event/lazy-update timing state: no immortal polling workers or hot-path I/O.
// Completion observations are conservative host timestamps, not GPU queries.
class PERFORMANCEDETECTOR_API FPerformanceDetector
{
public:
    static FPerformanceDetector& GetInstance();
    float GetAverageFPS();
    double GetFrameBeginTime();
    double GetFrameTime();
    double GetTimeSlice();
    double GetRemainTimeSlice();
    double GetAvgRenderingTime();
    double PredictNextStallTimeWindow();
    void SetFrameBeginTime(double LegacyTimestamp);
    void SetFrameTime(double Time);
    void SetMaxFPS(double FPS);
    void SetWindowsCount(int Count);
    void SetWindowsLength(int Length);
    int GetWindowsLength();
    int GetTargetAPM();
    void SetTargetAPM(int APM);
    void SetOpPolicy(int Policy);
    void ComputeTimeSlice();
    void ComputeStallTime();
    void ComputeStallTimeInRendering();
    void ComputeStallTimeInferenceUsed();
    void ComputeStallTimeInferenceUnused();
    void ComputeStallTimePrefillingUsed();
    void WriteStallTimeInfo();
    void UpdateFrameStallTime();
    void UpdateStallTimeWindowTime();
    double UpdateAllStallTimePrefillingUsed();
private:
    FPerformanceDetector();
    void UpdateCompletionLocked();
    void RefreshFramesPerWindow();
    std::mutex Mutex;
    double FrameBeginSeconds = 0, FrameEndSeconds = 0;
    double FrameMs = 0, TimeSliceMs = 0, AverageMs = 0;
    double AverageIntervalMs = 0;
    double MaxFPS = 60, AccumulatorMs = 0;
    int TargetAPM = 200, WindowsCount = 3, FramesPerWindow = 18, FrameCount = 0;
    int OpPolicy = 2;
    bool bCompleted = false;
    std::deque<double> History;
    FOnlinePredictor Predictor;
};
