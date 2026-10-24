#include "LegoUpstreamModel.h"
#include "LegoInference.h"
#include "TaskScheduler.h"
#include "TaskDetector.h"
#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include <chrono>
#include <cmath>
#include <cstring>

DEFINE_LOG_CATEGORY_STATIC(LogLEGO, Log, All);
FLegoUpstreamModel& FLegoUpstreamModel::GetInstance() { static FLegoUpstreamModel Instance; return Instance; }
FLegoUpstreamModel::~FLegoUpstreamModel() { Exit(); }

bool FLegoUpstreamModel::LoadConfig()
{
    Config = FLegoUpstreamExperimentConfig();
    Config.OutputDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("LEGO"));
    if (GConfig)
    {
        GConfig->GetString(TEXT("LEGO"), TEXT("ModelPath"), Config.ModelPath, GGameIni);
        GConfig->GetString(TEXT("LEGO"), TEXT("PromptFile"), Config.PromptFile, GGameIni);
        GConfig->GetString(TEXT("LEGO"), TEXT("OutputDirectory"), Config.OutputDirectory, GGameIni);
        GConfig->GetDouble(TEXT("LEGO"), TEXT("Seconds"), Config.Seconds, GGameIni);
        GConfig->GetDouble(TEXT("LEGO"), TEXT("APM"), Config.APM, GGameIni);
        GConfig->GetDouble(TEXT("LEGO"), TEXT("FPS"), Config.FPS, GGameIni);
        GConfig->GetDouble(TEXT("LEGO"), TEXT("AdmissionTimeoutSeconds"), Config.AdmissionTimeoutSeconds, GGameIni);
        GConfig->GetInt(TEXT("LEGO"), TEXT("Tokens"), Config.Tokens, GGameIni);
        GConfig->GetInt(TEXT("LEGO"), TEXT("Context"), Config.Context, GGameIni);
        GConfig->GetInt(TEXT("LEGO"), TEXT("OperationPolicy"), Config.OperationPolicy, GGameIni);
        GConfig->GetInt(TEXT("LEGO"), TEXT("WarmupActions"), Config.WarmupActions, GGameIni);
    }
    const TCHAR* Command = FCommandLine::Get();
    auto ReadDouble = [Command](const TCHAR* Key, double& Value)
    {
        FString Text;
        if (FParse::Value(Command, Key, Text)) Value = FCString::Atod(*Text);
    };
    FParse::Value(Command, TEXT("LegoModel="), Config.ModelPath);
    FParse::Value(Command, TEXT("LegoPrompt="), Config.PromptFile);
    FParse::Value(Command, TEXT("LegoOutput="), Config.OutputDirectory);
    ReadDouble(TEXT("LegoSeconds="), Config.Seconds);
    ReadDouble(TEXT("LegoAPM="), Config.APM);
    ReadDouble(TEXT("LegoFPS="), Config.FPS);
    FParse::Value(Command, TEXT("LegoPolicy="), Config.OperationPolicy);
    FParse::Value(Command, TEXT("LegoTokens="), Config.Tokens);
    FParse::Value(Command, TEXT("LegoWarmup="), Config.WarmupActions);
    int32 Skip = 0;
    FParse::Value(Command, TEXT("LegoSkipDepth="), Skip);
    if (Skip != 0 || FParse::Param(Command, TEXT("LegoAdaptive")))
    {
        UE_LOG(LogLEGO, Error, TEXT("Adaptive skipping is unavailable: matching trained adaptors/calibration are required. Full depth only."));
        return false;
    }
    if (!std::isfinite(Config.APM) || Config.APM <= 0 || Config.APM > 6000 ||
        !std::isfinite(Config.FPS) || Config.FPS <= 0 || Config.FPS > 1000 ||
        !std::isfinite(Config.Seconds) || Config.Seconds <= 0 || Config.Seconds > 86400 ||
        !std::isfinite(Config.AdmissionTimeoutSeconds) || Config.AdmissionTimeoutSeconds <= 0 ||
        Config.OperationPolicy < 0 || Config.OperationPolicy > 2 || Config.Tokens <= 0 ||
        Config.Tokens >= Config.Context || Config.Context > 131072 || Config.ModelPath.IsEmpty() || Config.WarmupActions < 1 || Config.WarmupActions > 100)
    {
        UE_LOG(LogLEGO, Error, TEXT("Invalid [LEGO] configuration: require ModelPath and positive bounded APM/FPS/duration/context/tokens."));
        return false;
    }
    Config.ModelPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Config.ModelPath);
    Config.OutputDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Config.OutputDirectory);
    if (!Config.PromptFile.IsEmpty()) Config.PromptFile = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Config.PromptFile);
    if (!FPaths::FileExists(Config.ModelPath))
    {
        UE_LOG(LogLEGO, Error, TEXT("Model not found: %s"), *Config.ModelPath);
        return false;
    }
    return true;
}

bool FLegoUpstreamModel::Init()
{
    std::lock_guard<std::mutex> Lock(LifecycleMutex);
    if (Model) return true;
    if (!LoadConfig()) return false;
    const std::string Path(TCHAR_TO_UTF8(*Config.ModelPath));
    lego_config Params{Path.c_str(), Config.Context, 99, 1};
    char BackendError[1024] = {};
    Model = lego_create(&Params, BackendError, sizeof(BackendError));
    if (!Model)
    {
        UE_LOG(LogLEGO, Error, TEXT("Upstream backend initialization failed: %s"), UTF8_TO_TCHAR(BackendError));
        return false;
    }
    FTaskScheduler::GetInstance().SetOpPolicy(Config.OperationPolicy);
    FTaskScheduler::GetInstance().SetSkipPolicy(1);
    FTaskScheduler::GetInstance().SetSkipDepth(0);
    UE_LOG(LogLEGO, Display, TEXT("Model=%s upstream=%s policy=%d skip=0 checkpoints=1 (experimental)"),
           *Config.ModelPath, UTF8_TO_TCHAR(lego_upstream_commit()), Config.OperationPolicy);
    return true;
}
void FLegoUpstreamModel::JoinWorker() { if (Worker.joinable()) Worker.join(); }
bool FLegoUpstreamModel::StartInference(const std::string& Prompt, double PeriodMs, TFunction<void(const FString&)> Update)
{
    if (!Init()) return false;
    std::lock_guard<std::mutex> Lock(LifecycleMutex);
    if (bRunning.load() || !std::isfinite(PeriodMs) || PeriodMs < 10) return false;
    JoinWorker();
    if (FTaskScheduler::GetInstance().GetSkipDepth() != 0) return false;
    FString Text = UTF8_TO_TCHAR(Prompt.c_str());
    if (!Config.PromptFile.IsEmpty() && !FFileHelper::LoadFileToString(Text, *Config.PromptFile)) return false;
    if (Text.IsEmpty()) return false;
    bStop.store(false);
    FTaskScheduler::GetInstance().BeginExperiment(Config.AdmissionTimeoutSeconds);
    bRunning.store(true);
    Worker = std::thread(&FLegoUpstreamModel::Run, this, std::string(TCHAR_TO_UTF8(*Text)), PeriodMs, MoveTemp(Update));
    return true;
}
void FLegoUpstreamModel::StopInference()
{
    std::lock_guard<std::mutex> Lock(LifecycleMutex);
    bStop.store(true);
    FTaskScheduler::GetInstance().CancelExperiment();
    if (Model) lego_request_stop(Model);
    JoinWorker();
}
void FLegoUpstreamModel::Exit()
{
    std::lock_guard<std::mutex> Lock(LifecycleMutex);
    bStop.store(true);
    FTaskScheduler::GetInstance().CancelExperiment();
    if (Model) lego_request_stop(Model);
    JoinWorker();
    if (Model) { lego_destroy(Model); Model = nullptr; }
}

