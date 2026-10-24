#pragma once
#include "CoreMinimal.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

struct lego_inference;
struct FLegoUpstreamExperimentConfig
{
    FString ModelPath, PromptFile, OutputDirectory;
    double Seconds = 120, APM = 200, FPS = 60, AdmissionTimeoutSeconds = 2;
    int32 Tokens = 16, Context = 2048, OperationPolicy = 2, WarmupActions = 3;
};

// One worker owns context/sampler. Callbacks are dispatched on the game thread.
class LEGOUPSTREAMMODEL_API FLegoUpstreamModel
{
public:
    static FLegoUpstreamModel& GetInstance();
    bool Init();
    void Exit();
    bool StartInference(const std::string& Prompt, double PeriodMs, TFunction<void(const FString&)> Update);
    void StopInference();
    bool IsRunning() const { return bRunning.load(); }
    float GetInferenceTime() const { return AverageTokenMs.load(); }
    float GetTotalInferenceTime() const { return ActionMs.load(); }
    bool CheckAutoUpdateSkipLayers() const { return false; }
    const FLegoUpstreamExperimentConfig& GetConfig() const { return Config; }
    ~FLegoUpstreamModel();
private:
    FLegoUpstreamModel() = default;
    FLegoUpstreamModel(const FLegoUpstreamModel&) = delete;
    FLegoUpstreamModel& operator=(const FLegoUpstreamModel&) = delete;
    bool LoadConfig();
    void Run(std::string Prompt, double PeriodMs, TFunction<void(const FString&)> Update);
    void JoinWorker();
    std::mutex LifecycleMutex;
    std::thread Worker;
    std::atomic<bool> bStop{true}, bRunning{false};
    std::atomic<float> AverageTokenMs{0}, ActionMs{0};
    lego_inference* Model = nullptr;

    FLegoUpstreamExperimentConfig Config;
};
