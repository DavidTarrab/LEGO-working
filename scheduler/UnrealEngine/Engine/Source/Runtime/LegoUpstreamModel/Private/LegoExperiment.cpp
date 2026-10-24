#include "LegoUpstreamModel.h"
#include "LegoInference.h"
#include "TaskScheduler.h"
#include "TaskDetector.h"
#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include <chrono>
#include <cstring>

DEFINE_LOG_CATEGORY_STATIC(LogLEGOExperiment, Log, All);
namespace {
using Clock = std::chrono::steady_clock;
double Ms(Clock::duration Value) { return std::chrono::duration<double, std::milli>(Value).count(); }
void WriteJson(const FString& File, const TSharedRef<FJsonObject>& Object)
{
    FString Text;
    const auto Writer = TJsonWriterFactory<>::Create(&Text);
    if (!FJsonSerializer::Serialize(Object, Writer) || !FFileHelper::SaveStringToFile(Text, *File))
        UE_LOG(LogLEGOExperiment, Error, TEXT("Cannot write %s"), *File);
}
struct FAction {
    lego_inference* Inference;
    std::atomic<bool>* Stop;
    bool AdmissionFailed = false;
    bool FrameAtAdmit = false, GPUAtAdmit = false, RHIAtAdmit = false;
    double PhaseStartMs = 0, ChunkStart = 0, PromptMs = 0, DecodeMs = 0;
    int32 Generated = 0;
    std::string Response, Node;
    static void Event(const lego_event* Event, void* Data)
    {
        auto& Action = *static_cast<FAction*>(Data);
        auto& Detector = FTaskDetector::GetInstance();
        auto& Scheduler = FTaskScheduler::GetInstance();
        if (std::strcmp(Event->kind, "decode_begin") == 0)
        {
            Detector.SetIsPrefilling(Event->decode_index == 0);
            Action.PhaseStartMs = Event->elapsed_ms;
        }
        else if (std::strcmp(Event->kind, "decode_end") == 0)
        {
            const double Duration = Event->elapsed_ms - Action.PhaseStartMs;
            if (Event->decode_index == 0) Action.PromptMs += Duration; else Action.DecodeMs += Duration;
            Detector.SetIsInInferring(false);
        }
        else if (std::strcmp(Event->kind, "admission_check") == 0)
        {
            Detector.SetIsInInferring(false);
            if (Action.Stop->load() || !Scheduler.WaitForAdmission(Event->node))
            {
                Action.AdmissionFailed = !Action.Stop->load();
                Action.ChunkStart = 0;
                lego_request_stop(Action.Inference);
                return;
            }
            Detector.SetIsInInferring(true);
            Action.FrameAtAdmit = Detector.GetIsInFrameRendering();
            Action.GPUAtAdmit = Detector.GetRenderingTaskIsRunning();
            Action.RHIAtAdmit = Detector.IsRHIWorkPending();
            Action.Node = Event->node;
            Action.ChunkStart = FPlatformTime::Seconds();
        }
        else if (std::strcmp(Event->kind, "chunk_complete") == 0 && Action.ChunkStart > 0)
        {
            Scheduler.RecordChunk(Action.Node.c_str(), Action.ChunkStart, FPlatformTime::Seconds(), Action.FrameAtAdmit, Action.GPUAtAdmit, Action.RHIAtAdmit);
            Action.ChunkStart = 0;
        }
    }
    static void Token(int, const char* Text, void* Data)
    {
        auto& Action = *static_cast<FAction*>(Data);
        ++Action.Generated; Action.Response += Text;
    }
};
}
void FLegoUpstreamModel::Run(std::string Prompt, double PeriodMs, TFunction<void(const FString&)> Update)
{
    FString Error;
    const FString RunDir = FPaths::Combine(Config.OutputDirectory, FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S")) + TEXT("-") + FGuid::NewGuid().ToString());
    if (!IFileManager::Get().MakeDirectory(*RunDir, true)) Error = TEXT("Cannot create output directory.");
    const int32 PromptCount = lego_prompt_token_count(Model, Prompt.c_str());
    if (PromptCount < 1 || PromptCount + Config.Tokens > Config.Context) Error = TEXT("Prompt/output exceeds context.");
    auto& Scheduler = FTaskScheduler::GetInstance();
    auto& Detector = FTaskDetector::GetInstance();
    const int32 Policy = Scheduler.GetOpPolicy();
    if (Policy != 2 && (Detector.GetFrameCount() == 0 || Detector.GetGPUWorkCount() == 0))
        Error = TEXT("Scheduling requires the hooked D3D12 source engine. Rendering hooks have not reported frames and GPU submissions.");
    if (Policy != 2 && Config.WarmupActions < 2) Error = TEXT("Scheduled runs require at least two warmup actions: initialize kernels, then calibrate.");
    Detector.SetTotalInferenceIsRunning(true);
    // Calibrate with unrestricted admission before the measured interval. All
    // comparison policies use the same backend, checkpoints and warmup count.
    Scheduler.BeginExperiment(Config.AdmissionTimeoutSeconds, true);
    Scheduler.SetOpPolicy(2);
    char BackendError[1024] = {};
    for (int32 Warmup = 0; Warmup < Config.WarmupActions && Error.IsEmpty() && !bStop.load(); ++Warmup)
    {
        // Do not let first-use CUDA/module initialization inflate every future
        // admission estimate. The remaining warmups calibrate steady execution.
        if (Warmup == 1) Scheduler.BeginExperiment(Config.AdmissionTimeoutSeconds, true);
        FAction Action{Model, &bStop};
        if (lego_run(Model, Prompt.c_str(), Config.Tokens, FAction::Event, FAction::Token, &Action, BackendError, sizeof(BackendError)) < 0)
            Error = UTF8_TO_TCHAR(BackendError);
    }
    Scheduler.SaveCalibration(FPaths::Combine(RunDir, TEXT("calibration-initial.csv")));
    Scheduler.SetOpPolicy(Policy);
    if (!bStop.load()) Scheduler.BeginExperiment(Config.AdmissionTimeoutSeconds, false);
    const auto Origin = Clock::now();
    const auto UTC = FDateTime::UtcNow();
    const uint64 StartQPC = FPlatformTime::Cycles64();
    const auto Window = MakeShared<FJsonObject>();
    Window->SetStringField(TEXT("start_qpc_ticks"), FString::Printf(TEXT("%llu"), StartQPC));
    Window->SetNumberField(TEXT("qpc_frequency"), 1.0 / FPlatformTime::GetSecondsPerCycle64());
    Window->SetStringField(TEXT("start_utc"), UTC.ToIso8601());
    Window->SetBoolField(TEXT("complete"), false);
    WriteJson(FPaths::Combine(RunDir, TEXT("measurement.json")), Window);
    const int64 InitialFrames = Detector.GetFrameCount(), InitialGPUWork = Detector.GetGPUWorkCount();
    const double EndMs = Config.Seconds * 1000;
    double Scheduled = 0;
    int32 Completed = 0, Skipped = 0, Misses = 0, Interrupted = 0;
    TArray<double> Latencies;
    TArray<FString> Records;
    Records.Add(TEXT("action,start_utc,start_elapsed_ms,latency_ms,deadline_ms,deadline_missed,prompt_tokens,prompt_ms,generated_tokens,decode_ms,decode_tokens_per_second,service_ms,arrival_lateness_ms,admission_wait_ms,skip_depth"));
    auto Publish = [&Update](const FString& Text) {
        if (Update) AsyncTask(ENamedThreads::GameThread, [Callback = Update, Text]() { Callback(Text); });
    };
    while (Error.IsEmpty() && !bStop.load() && !Scheduler.IsCancelled())
    {
        double Now = Ms(Clock::now() - Origin);
        if (Now >= EndMs) break;
        if (Scheduled >= EndMs || Now < Scheduled)
        {
            const double Next = FMath::Min(Scheduled, EndMs);
            std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(FMath::Min(1.0, Next - Now)));
            continue;
        }
        const auto Start = Clock::now();
        const double ActualStart = Ms(Start - Origin), WaitBefore = Scheduler.GetAdmissionWaitMs();
        Detector.SetActionIsRunning(true);
        FAction Action{Model, &bStop};
        const int Result = lego_run(Model, Prompt.c_str(), Config.Tokens, FAction::Event, FAction::Token, &Action, BackendError, sizeof(BackendError));
        Detector.SetActionIsRunning(false);
        const double Service = Ms(Clock::now() - Start), Latency = Ms(Clock::now() - Origin) - Scheduled;
        if (Result < 0) Error = UTF8_TO_TCHAR(BackendError);
        if (Action.AdmissionFailed) Error = TEXT("Render admission timed out or was cancelled; inspect completion hooks and scheduler log.");
        if (!Error.IsEmpty() || Result != 0 || bStop.load() || Scheduler.IsCancelled()) { ++Interrupted; break; }
        ++Completed;
        const bool Missed = Latency > PeriodMs;
        if (Missed) ++Misses;
        Latencies.Add(Latency);
        ActionMs.store(static_cast<float>(Service));
        AverageTokenMs.store(Action.Generated > 0 ? static_cast<float>(Service / Action.Generated) : 0);
        const auto Arrival = UTC + FTimespan::FromMilliseconds(Scheduled);
        const double Rate = Action.DecodeMs > 0 ? 1000.0 * FMath::Max(Action.Generated - 1, 0) / Action.DecodeMs : 0;
        Records.Add(FString::Printf(TEXT("%d,%s,%.6f,%.6f,%.6f,%s,%d,%.6f,%d,%.6f,%.6f,%.6f,%.6f,%.6f,0"),
            Completed, *Arrival.ToIso8601(), Scheduled, Latency, PeriodMs, Missed ? TEXT("True") : TEXT("False"),
            PromptCount, Action.PromptMs, Action.Generated, Action.DecodeMs, Rate, Service, ActualStart - Scheduled, Scheduler.GetAdmissionWaitMs() - WaitBefore));
        Publish(UTF8_TO_TCHAR(Action.Response.c_str()));
        Scheduled += PeriodMs;
        Now = Ms(Clock::now() - Origin);
        while (Scheduled < Now && Scheduled < EndMs) { Scheduled += PeriodMs; ++Skipped; }
        Detector.SetActionCount(Completed);
    }
    const double Elapsed = Ms(Clock::now() - Origin) / 1000;
    Window->SetStringField(TEXT("end_qpc_ticks"), FString::Printf(TEXT("%llu"), FPlatformTime::Cycles64()));
    Window->SetBoolField(TEXT("complete"), true);
    WriteJson(FPaths::Combine(RunDir, TEXT("measurement.json")), Window);
    Detector.SetIsPrefilling(false); Detector.SetIsInInferring(false);
    Detector.SetActionIsRunning(false); Detector.SetTotalInferenceIsRunning(false);
    if (!FFileHelper::SaveStringArrayToFile(Records, *FPaths::Combine(RunDir, TEXT("llm.csv"))))
        UE_LOG(LogLEGOExperiment, Error, TEXT("Cannot write inference CSV."));
    Latencies.Sort();
    const auto Summary = MakeShared<FJsonObject>();
    Summary->SetNumberField(TEXT("actions"), Completed);
    Summary->SetNumberField(TEXT("elapsed_seconds"), Elapsed);
    Summary->SetNumberField(TEXT("achieved_apm"), Elapsed > 0 ? 60 * Completed / Elapsed : 0);
    Summary->SetNumberField(TEXT("skipped_arrivals"), Skipped);
    Summary->SetNumberField(TEXT("interrupted_actions"), Interrupted);
    Summary->SetNumberField(TEXT("deadline_misses"), Misses);
    Summary->SetNumberField(TEXT("dropped_chunk_records"), static_cast<double>(Scheduler.GetDroppedChunkRecords()));
    Summary->SetNumberField(TEXT("total_chunks"), static_cast<double>(Scheduler.GetTotalChunkCount()));
    Summary->SetNumberField(TEXT("admissions_during_frame"), static_cast<double>(Scheduler.GetFrameAdmissionCount()));
    Summary->SetNumberField(TEXT("admissions_with_gpu_pending"), static_cast<double>(Scheduler.GetGPUAdmissionCount()));
    Summary->SetNumberField(TEXT("admissions_with_rhi_pending"), static_cast<double>(Scheduler.GetRHIAdmissionCount()));
    if (Latencies.Num()) Summary->SetNumberField(TEXT("p99_latency_ms"), Latencies[FMath::CeilToInt(.99 * Latencies.Num()) - 1]);
    Summary->SetBoolField(TEXT("cancelled"), bStop.load());
    Summary->SetBoolField(TEXT("success"), Error.IsEmpty() && !bStop.load());
    Summary->SetStringField(TEXT("error"), Error);
    Summary->SetNumberField(TEXT("observed_render_frames"), static_cast<double>(Detector.GetFrameCount() - InitialFrames));
    Summary->SetNumberField(TEXT("observed_gpu_submissions"), static_cast<double>(Detector.GetGPUWorkCount() - InitialGPUWork));
    WriteJson(FPaths::Combine(RunDir, TEXT("summary.json")), Summary);
    const auto Metadata = MakeShared<FJsonObject>();
    Metadata->SetStringField(TEXT("baseline"), TEXT("integrated-upstream"));
    Metadata->SetStringField(TEXT("timing_source"), TEXT("integrated-host-wall"));
    Metadata->SetStringField(TEXT("latency_origin"), TEXT("scheduled-arrival"));
    Metadata->SetStringField(TEXT("start_utc"), UTC.ToIso8601());
    Metadata->SetStringField(TEXT("upstream_commit"), UTF8_TO_TCHAR(lego_upstream_commit()));
    Metadata->SetStringField(TEXT("model"), Config.ModelPath);
    Metadata->SetStringField(TEXT("prompt"), UTF8_TO_TCHAR(Prompt.c_str()));
    Metadata->SetNumberField(TEXT("tokens"), Config.Tokens);
    Metadata->SetNumberField(TEXT("seconds"), Config.Seconds);
    Metadata->SetNumberField(TEXT("requested_apm"), 60000 / PeriodMs);
    Metadata->SetNumberField(TEXT("operation_policy"), Policy);
    Metadata->SetNumberField(TEXT("warmup_actions"), Config.WarmupActions);
    Metadata->SetNumberField(TEXT("calibration_warmup_actions"), FMath::Max(0, Config.WarmupActions - 1));
    Metadata->SetNumberField(TEXT("context_tokens"), Config.Context);
    Metadata->SetNumberField(TEXT("target_fps"), Config.FPS);
    Metadata->SetNumberField(TEXT("checkpoint_nodes"), 1);
    Metadata->SetNumberField(TEXT("skip_depth"), 0);
    Metadata->SetNumberField(TEXT("parallel_requests"), 1);
    Metadata->SetBoolField(TEXT("cache_prompt"), false);
    Metadata->SetBoolField(TEXT("cuda_graphs"), false);
    Metadata->SetStringField(TEXT("traversal"), TEXT("upstream-eval-callback"));
    Metadata->SetStringField(TEXT("chunk_sampling"), TEXT("deterministic-reservoir-100000; aggregate admission counts are exact"));
    Metadata->SetStringField(TEXT("decode_token_accounting"), TEXT("First generated token comes from prompt processing; decode rate counts subsequent output tokens."));
    WriteJson(FPaths::Combine(RunDir, TEXT("config.json")), Metadata);
    Scheduler.SaveChunkRecords(FPaths::Combine(RunDir, TEXT("chunks.csv")));
    Scheduler.SaveCalibration(FPaths::Combine(RunDir, TEXT("calibration-final.csv")));
    if (!Error.IsEmpty()) { UE_LOG(LogLEGOExperiment, Error, TEXT("%s"), *Error); Publish(Error); }
    UE_LOG(LogLEGOExperiment, Display, TEXT("Finished actions=%d skipped=%d misses=%d output=%s"), Completed, Skipped, Misses, *RunDir);
    bRunning.store(false);
}
