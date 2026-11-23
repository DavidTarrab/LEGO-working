#include "TaskScheduler.h"
#include "LegoAdmission.h"
#include "LegoReservoir.h"
#include "TaskDetector.h"
#include "PerformanceDetector.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
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
    bAdmissionProfile = FParse::Param(FCommandLine::Get(), TEXT("LegoAdmissionProfile"));
    bFreezeCalibration = !ResetCalibration && FParse::Param(FCommandLine::Get(), TEXT("LegoProfileFreezeCalibration"));
    bOmitChunkRecords = FParse::Param(FCommandLine::Get(), TEXT("LegoProfileNoChunkRecords"));
    AdmissionPollUs = 200;
    FParse::Value(FCommandLine::Get(), TEXT("LegoAdmissionPollUs="), AdmissionPollUs);
    AdmissionPollUs = FMath::Clamp(AdmissionPollUs, 1, 5000);
    bEventWake = FParse::Param(FCommandLine::Get(), TEXT("LegoAdmissionEventWake"));
    bNoFit = FParse::Param(FCommandLine::Get(), TEXT("LegoProfileNoFit"));
    FTaskDetector::GetInstance().AdmissionSignal.Enable(bEventWake);
    ProfileCalls = ProfileBlockedCalls = ProfilePolls = ProfileUnknown = ProfileDroppedRows = 0;
    ProfileSleepMs = ProfileMaxSleepMs = 0;
    for (int I = 0; I < 16; ++I) ProfileMasks[I] = 0;
    for (int I = 0; I < 6; ++I) ProfileSleepHistogram[I] = 0;
    AdmissionRecords.Reset();
    if (bAdmissionProfile) AdmissionRecords.Add(TEXT("node,policy,start_elapsed_ms,end_elapsed_ms,wait_ms,sleeps,sleep_ms,frame_polls,gpu_polls,rhi_polls,fit_only_polls,estimate_ms,remaining_at_admit_ms"));
}
void FTaskScheduler::CancelExperiment() { bCancelled.store(true); Wake.notify_all(); FTaskDetector::GetInstance().AdmissionSignal.Notify(); }
bool FTaskScheduler::WaitForAdmission(const char* Node)
{
    const double Begin = FPlatformTime::Seconds();
    std::unique_lock<std::mutex> Lock(AdmissionMutex);
    const FString Key = FString(FTaskDetector::GetInstance().GetIsPrefilling() ? TEXT("prefill:") : TEXT("decode:")) + UTF8_TO_TCHAR(Node ? Node : "");
    const double* Calibrated = CalibrationMaxMs.Find(Key);
    // Unknown operations use a conservative reference estimate. Observed values
    // are high-water marks for this phase/node, never a promise of future time.
    const double Estimate = Calibrated ? *Calibrated : FMath::Max(EstimatedChunkMs, 1.0);
    uint64 Sleeps = 0, FramePolls = 0, GPUPolls = 0, RHIPolls = 0, FitOnlyPolls = 0;
    double SleepMs = 0;
    if (bAdmissionProfile) { ++ProfileCalls; if (!Calibrated) ++ProfileUnknown; }
    while (!bCancelled.load())
    {
        const uint64 Generation = FTaskDetector::GetInstance().AdmissionSignal.Snapshot();
        const int Policy = GetOpPolicy();
        const bool InFrame = FTaskDetector::GetInstance().GetIsInFrameRendering();
        const bool GPUActive = FTaskDetector::GetInstance().GetRenderingTaskIsRunning();
        const bool RHIActive = FTaskDetector::GetInstance().IsRHIWorkPending();
        const double Remaining = InFrame ? 0 : FPerformanceDetector::GetInstance().GetRemainTimeSlice();
        // NoFit is a diagnostic ablation; it removes frame-overrun protection.
        const double FitRemaining = bNoFit ? 1.e9 : Remaining;
        if (LegoCanAdmit(Policy, InFrame, GPUActive, RHIActive, FitRemaining, Estimate, .05))
        {
            const double End = FPlatformTime::Seconds();
            const double WaitMs = 1000 * (End - Begin);
            AdmissionWaitMs.store(AdmissionWaitMs.load() + WaitMs);
            if (bAdmissionProfile && Sleeps > 0)
            {
                ++ProfileBlockedCalls;
                if (AdmissionRecords.Num() <= 50000)
                {
                    FString SafeNode = UTF8_TO_TCHAR(Node ? Node : ""); SafeNode.ReplaceInline(TEXT("\""), TEXT("\"\""));
                    AdmissionRecords.Add(FString::Printf(TEXT("\"%s\",%d,%.6f,%.6f,%.6f,%llu,%.6f,%llu,%llu,%llu,%llu,%.6f,%.6f"),
                        *SafeNode, Policy, 1000 * (Begin - OriginSeconds), 1000 * (End - OriginSeconds), WaitMs,
                        Sleeps, SleepMs, FramePolls, GPUPolls, RHIPolls, FitOnlyPolls, Estimate, Remaining));
                }
                else ++ProfileDroppedRows;
            }
            return true;
        }
        if (FPlatformTime::Seconds() - Begin > AdmissionTimeoutSeconds)
        {
            UE_LOG(LogLEGOScheduler, Error, TEXT("Admission timeout: verify completion/frame-end hooks. Cancelling instead of hanging."));
            bCancelled.store(true); break;
        }
        const double SleepBegin = bAdmissionProfile ? FPlatformTime::Seconds() : 0;
        if (bAdmissionProfile)
        {
            const int Mask = ((Policy == 1 && InFrame) ? 1 : 0) | (GPUActive ? 2 : 0) | (RHIActive ? 4 : 0) |
                ((!bNoFit && !InFrame && Remaining <= Estimate + .05) ? 8 : 0);
            ++ProfileMasks[Mask]; ++ProfilePolls; ++Sleeps;
            if (InFrame) ++FramePolls;
            if (GPUActive) ++GPUPolls;
            if (RHIActive) ++RHIPolls;
            if (Mask == 8) ++FitOnlyPolls;
        }
        if (bEventWake)
        {
            Lock.unlock();
            FTaskDetector::GetInstance().AdmissionSignal.Wait(Generation, AdmissionPollUs);
            Lock.lock();
        }
        else Wake.wait_for(Lock, std::chrono::microseconds(AdmissionPollUs));
        if (bAdmissionProfile)
        {
            const double Duration = 1000 * (FPlatformTime::Seconds() - SleepBegin);
            SleepMs += Duration; ProfileSleepMs += Duration;
            ProfileMaxSleepMs = FMath::Max(ProfileMaxSleepMs, Duration);
            const int Bucket = Duration <= .25 ? 0 : Duration <= .5 ? 1 : Duration <= 1 ? 2 : Duration <= 2 ? 3 : Duration <= 5 ? 4 : 5;
            ++ProfileSleepHistogram[Bucket];
        }
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
    if (!bFreezeCalibration)
    {
        double& Maximum = CalibrationMaxMs.FindOrAdd(Key);
        Maximum = FMath::Max(Maximum, Duration);
        ++CalibrationSamples.FindOrAdd(Key);
        EstimatedChunkMs = FMath::Max(.8 * EstimatedChunkMs + .2 * Duration, .001);
    }
    const int64 Total = TotalChunks.fetch_add(1) + 1;
    if (FrameAtAdmit) FrameAdmissions.fetch_add(1);
    if (GPUAtAdmit) GPUAdmissions.fetch_add(1);
    if (RHIAtAdmit) RHIAdmissions.fetch_add(1);
    if (bOmitChunkRecords) { DroppedChunkRecords.fetch_add(1); return; }
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

void FTaskScheduler::SaveAdmissionProfile(const FString& Directory)
{
    std::lock_guard<std::mutex> Lock(AdmissionMutex);
    if (!bAdmissionProfile) return;
    FFileHelper::SaveStringArrayToFile(AdmissionRecords, *FPaths::Combine(Directory, TEXT("admission-waits.csv")));
    TArray<FString> Summary;
    Summary.Add(TEXT("metric,value"));
    auto Count = [&Summary](const TCHAR* Name, uint64 Value) { Summary.Add(FString::Printf(TEXT("%s,%llu"), Name, Value)); };
    Count(TEXT("admission_calls"), ProfileCalls); Count(TEXT("blocked_calls"), ProfileBlockedCalls);
    Count(TEXT("blocked_polls"), ProfilePolls); Count(TEXT("unknown_estimate_calls"), ProfileUnknown);
    Count(TEXT("omitted_wait_records"), ProfileDroppedRows); Count(TEXT("poll_requested_us"), AdmissionPollUs);
    Count(TEXT("calibration_frozen"), bFreezeCalibration); Count(TEXT("chunk_records_disabled"), bOmitChunkRecords);
    Count(TEXT("event_wake_enabled"), bEventWake); Count(TEXT("fit_disabled"), bNoFit);
    Summary.Add(FString::Printf(TEXT("sleep_total_ms,%.6f"), ProfileSleepMs));
    Summary.Add(FString::Printf(TEXT("sleep_max_ms,%.6f"), ProfileMaxSleepMs));
    for (int I = 0; I < 16; ++I) Count(*FString::Printf(TEXT("reason_mask_%d"), I), ProfileMasks[I]);
    const TCHAR* Names[] = {TEXT("sleep_le_0_25_ms"),TEXT("sleep_le_0_5_ms"),TEXT("sleep_le_1_ms"),TEXT("sleep_le_2_ms"),TEXT("sleep_le_5_ms"),TEXT("sleep_gt_5_ms")};
    for (int I = 0; I < 6; ++I) Count(Names[I], ProfileSleepHistogram[I]);
    FFileHelper::SaveStringArrayToFile(Summary, *FPaths::Combine(Directory, TEXT("admission-summary.csv")));
}
