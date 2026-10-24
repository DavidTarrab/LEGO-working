#include "TaskScheduler.h"
#include "LegoAdmission.h"
#include "LegoReservoir.h"
#include "TaskDetector.h"
#include "PerformanceDetector.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include <chrono>
#include <thread>
DEFINE_LOG_CATEGORY_STATIC(LogLEGOScheduler, Log, All);
HighPrecisionTimer::HighPrecisionTimer() {}
HighPrecisionTimer& HighPrecisionTimer::GetInstance() { static HighPrecisionTimer Timer; return Timer; }
void HighPrecisionTimer::SpinSleepMicroseconds(int64_t Us) { if (Us > 0) std::this_thread::sleep_for(std::chrono::microseconds(Us)); }
FTaskScheduler::FTaskScheduler()
    : Performance(0), ExitLayerNum(29), TokenDecodePhase(0), TokenBeginTime(0), TokenDecodingTime(0),
      IsRenderingEnd(false), AllGraphNodes(0), ExeGraphNodes(0), FirstLayerNum(0), NowLayerNum(0),
      SLOTime(50), UpdateInPhase2(false), OpPolicy(2), SkipPolicy(1), SkipDepth(0) {}
FTaskScheduler::~FTaskScheduler() { CancelExperiment(); }
FTaskScheduler& FTaskScheduler::GetInstance()
{
    // Process lifetime; model/module teardown explicitly cancels before unload.
    static FTaskScheduler* Instance = new FTaskScheduler(); return *Instance;
}
int FTaskScheduler::GetExitLayerNum() { return ExitLayerNum; }
int FTaskScheduler::GetSkipDepth() { return SkipDepth; }
int FTaskScheduler::GetDecodePhase() { return TokenDecodePhase; }
int FTaskScheduler::GetSkipPolicy() { return SkipPolicy; }
int FTaskScheduler::GetOpPolicy() { return OpPolicy; }
void FTaskScheduler::SetDecodePhase(const int Phase) { TokenDecodePhase = Phase; }
void FTaskScheduler::SetSLOTime(const int Time) { SLOTime = FMath::Max(1, Time); }
void FTaskScheduler::SetIsRenderingEnd(const bool Flag) { IsRenderingEnd = Flag; Wake.notify_all(); }
void FTaskScheduler::SetTokenBeginTime(const double Time) { TokenBeginTime = Time; }
void FTaskScheduler::SetAllGraphNodes(const int Nodes) { AllGraphNodes = Nodes; }
void FTaskScheduler::SetExeGraphNodes(const int Nodes) { ExeGraphNodes = Nodes; }
void FTaskScheduler::SetFirstLayerNum(const int Num) { FirstLayerNum = Num; }
void FTaskScheduler::SetNowLayerNum(const int Num) { NowLayerNum = Num; }
void FTaskScheduler::SetUpdateInPhase2(const bool Flag) { UpdateInPhase2 = Flag; }
void FTaskScheduler::SetSkipPolicy(const int Policy) { SkipPolicy = Policy; }
void FTaskScheduler::SetSkipDepth(const int Depth)
{
    if (Depth != 0) UE_LOG(LogLEGOScheduler, Error, TEXT("Skip depth rejected: trained adaptors/calibration unavailable."));
    SkipDepth = 0;
}
void FTaskScheduler::SetOpPolicy(const int Policy)
{
    if (Policy < 0 || Policy > 2) { CancelExperiment(); return; }
    OpPolicy = Policy;
}
// Keep full depth until supported adaptors and calibration are recovered.
void FTaskScheduler::UpdateSkipDepth1() { SkipDepth = 0; }
void FTaskScheduler::UpdateSkipDepth2() { SkipDepth = 0; }
void FTaskScheduler::UpdateSkipDepthAfterPrefilling(double, double, double, double, double) { SkipDepth = 0; }
void FTaskScheduler::BeginExperiment(double Timeout, bool ResetCalibration)
{
    std::lock_guard<std::mutex> Lock(AdmissionMutex);
    AdmissionTimeoutSeconds = FMath::Max(.001, Timeout);
    AdmissionWaitMs.store(0); EstimatedChunkMs = .1; OriginSeconds = FPlatformTime::Seconds();
    if (ResetCalibration) { CalibrationMaxMs.Reset(); CalibrationSamples.Reset(); }
    ChunkRecords.Reset();
    DroppedChunkRecords.store(0);
    TotalChunks.store(0); FrameAdmissions.store(0); GPUAdmissions.store(0); RHIAdmissions.store(0);
    SampleState = 0x9E3779B97F4A7C15ull;
    ChunkRecords.Add(TEXT("chunk,node,submit_elapsed_ms,complete_elapsed_ms,host_completion_ms,operation_policy,frame_at_admit,gpu_pending_at_admit,rhi_pending_at_admit"));
    bCancelled.store(false);
}
void FTaskScheduler::CancelExperiment() { bCancelled.store(true); Wake.notify_all(); }
bool FTaskScheduler::WaitForAdmission(const char* Node)
{
    const double Begin = FPlatformTime::Seconds();
    std::unique_lock<std::mutex> Lock(AdmissionMutex);
    const FString Key = FString(FTaskDetector::GetInstance().GetIsPrefilling() ? TEXT("prefill:") : TEXT("decode:")) + UTF8_TO_TCHAR(Node ? Node : "");
    const double* Calibrated = CalibrationMaxMs.Find(Key);
    // Unknown operations use a conservative reference estimate. Observed values
    // are high-water marks for this phase/node, never a promise of future time.
    const double Estimate = Calibrated ? *Calibrated : FMath::Max(EstimatedChunkMs, 1.0);
    while (!bCancelled.load())
    {
        const int Policy = GetOpPolicy();
        const bool InFrame = FTaskDetector::GetInstance().GetIsInFrameRendering();
        const bool GPUActive = FTaskDetector::GetInstance().GetRenderingTaskIsRunning();
        const bool RHIActive = FTaskDetector::GetInstance().IsRHIWorkPending();
        const double Remaining = InFrame ? 0 : FPerformanceDetector::GetInstance().GetRemainTimeSlice();
        if (LegoCanAdmit(Policy, InFrame, GPUActive, RHIActive, Remaining, Estimate, .05))
        {
            AdmissionWaitMs.store(AdmissionWaitMs.load() + 1000 * (FPlatformTime::Seconds() - Begin));
            return true;
        }
        if (FPlatformTime::Seconds() - Begin > AdmissionTimeoutSeconds)
        {
            UE_LOG(LogLEGOScheduler, Error, TEXT("Admission timeout: verify completion/frame-end hooks. Cancelling instead of hanging."));
            bCancelled.store(true); break;
        }
        Wake.wait_for(Lock, std::chrono::microseconds(200));
    }
    AdmissionWaitMs.store(AdmissionWaitMs.load() + 1000 * (FPlatformTime::Seconds() - Begin));
    return false;
}
void FTaskScheduler::WaitForRenderingTaskCompletion() { WaitForAdmission(); }
void FTaskScheduler::RecordChunk(const char* Name, double Submit, double Complete, bool FrameAtAdmit, bool GPUAtAdmit, bool RHIAtAdmit)
{
    std::lock_guard<std::mutex> Lock(AdmissionMutex);
    const double Duration = 1000 * (Complete - Submit);
    const FString Key = FString(FTaskDetector::GetInstance().GetIsPrefilling() ? TEXT("prefill:") : TEXT("decode:")) + UTF8_TO_TCHAR(Name ? Name : "");
    double& Maximum = CalibrationMaxMs.FindOrAdd(Key);
    Maximum = FMath::Max(Maximum, Duration);
    ++CalibrationSamples.FindOrAdd(Key);
    EstimatedChunkMs = FMath::Max(.8 * EstimatedChunkMs + .2 * Duration, .001);
    const int64 Total = TotalChunks.fetch_add(1) + 1;
    if (FrameAtAdmit) FrameAdmissions.fetch_add(1);
    if (GPUAtAdmit) GPUAdmissions.fetch_add(1);
    if (RHIAtAdmit) RHIAdmissions.fetch_add(1);
    // A deterministic reservoir covers the entire experiment rather than only
    // its first few actions. Exact admission counts are maintained separately.
    const int32 Candidate = LegoReservoirSlot(static_cast<uint64>(Total), 100000, SampleState);
    if (Total > 100000)
    {
        DroppedChunkRecords.fetch_add(1);
    }
    if (Candidate < 0) return;
    const int32 Slot = Candidate + 1;
    FString Node = UTF8_TO_TCHAR(Name ? Name : ""); Node.ReplaceInline(TEXT("\""), TEXT("\"\""));
    const FString Row = FString::Printf(TEXT("%lld,\"%s\",%.6f,%.6f,%.6f,%d,%d,%d,%d"), Total, *Node,
        1000 * (Submit - OriginSeconds), 1000 * (Complete - OriginSeconds), Duration, GetOpPolicy(), FrameAtAdmit ? 1 : 0, GPUAtAdmit ? 1 : 0, RHIAtAdmit ? 1 : 0);
    if (Slot == ChunkRecords.Num()) ChunkRecords.Add(Row); else ChunkRecords[Slot] = Row;
}
void FTaskScheduler::SaveChunkRecords(const FString& File)
{
    std::lock_guard<std::mutex> Lock(AdmissionMutex);
    if (!FFileHelper::SaveStringArrayToFile(ChunkRecords, *File)) UE_LOG(LogLEGOScheduler, Error, TEXT("Cannot write %s"), *File);
}
void FTaskScheduler::SaveCalibration(const FString& File)
{
    std::lock_guard<std::mutex> Lock(AdmissionMutex);
    TArray<FString> Rows;
    Rows.Add(TEXT("phase_node,observed_max_host_ms,samples"));
    for (const auto& Entry : CalibrationMaxMs)
    {
        FString Name = Entry.Key.Replace(TEXT("\""), TEXT("\"\""));
        Rows.Add(FString::Printf(TEXT("\"%s\",%.6f,%d"), *Name, Entry.Value, CalibrationSamples.FindRef(Entry.Key)));
    }
    if (!FFileHelper::SaveStringArrayToFile(Rows, *File)) UE_LOG(LogLEGOScheduler, Error, TEXT("Cannot write %s"), *File);
}
