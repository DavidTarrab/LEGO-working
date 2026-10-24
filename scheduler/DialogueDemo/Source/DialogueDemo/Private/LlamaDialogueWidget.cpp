#include "LlamaDialogueWidget.h"
#include "LegoUpstreamModel.h"
#include "TaskScheduler.h"
#include "PerformanceDetector.h"
#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"
#include "HAL/IConsoleManager.h"

void ULlamaDialogueWidget::NativeConstruct()
{
    Super::NativeConstruct();
    Content = TEXT("LEGO experiment ready (full depth).");
    // Native UI removes the dependency on absent Widget Blueprint assets.
    if (WidgetTree && !WidgetTree->RootWidget)
    {
        OutputText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
        WidgetTree->RootWidget = OutputText;
        OutputText->SetText(FText::FromString(Content));
    }
}
bool ULlamaDialogueWidget::GetNextPrompt()
{
    Prompt = TEXT("You control a fighter. Health 60, enemy health 40, distance 3 meters. Available actions: dodge, attack, retreat. Choose an action and explain briefly.");
    return true;
}
void ULlamaDialogueWidget::LlamaInference()
{
    auto& Model = FLegoUpstreamModel::GetInstance();
    if (Model.IsRunning() || !Model.Init()) { Content = TEXT("Cannot start: model unavailable or another experiment is running. See log."); return; }
    const auto& Config = Model.GetConfig();
    const double TargetAPM = bAPMOverridden ? static_cast<double>(APM) : Config.APM;
    if (!bPolicyOverridden) OperationPolicy = Config.OperationPolicy;
    FTaskScheduler::GetInstance().SetOpPolicy(OperationPolicy);
    FTaskScheduler::GetInstance().SetSkipDepth(0);
    FPerformanceDetector::GetInstance().SetOpPolicy(OperationPolicy);
    FPerformanceDetector::GetInstance().SetMaxFPS(Config.FPS);
    FPerformanceDetector::GetInstance().SetTargetAPM(FMath::RoundToInt(TargetAPM));
    if (auto* Cap = IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS"))) Cap->Set(static_cast<float>(Config.FPS));
    if (auto* VSync = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VSync"))) VSync->Set(0);
    if (Prompt.IsEmpty()) GetNextPrompt();
    const TWeakObjectPtr<ULlamaDialogueWidget> WeakThis(this);
    bOwnsExperiment = Model.StartInference(TCHAR_TO_UTF8(*Prompt), 60000.0 / TargetAPM,
        [WeakThis](const FString& Text)
        {
            if (auto* Widget = WeakThis.Get())
            {
                if (!Widget->IsInViewport()) return;
                Widget->Content = Text;
                ++Widget->Order;
                if (Widget->OutputText) Widget->OutputText->SetText(FText::FromString(Text));
            }
        });
    Content = bOwnsExperiment ? TEXT("Experiment running...") : TEXT("Experiment could not start. See log.");
    if (OutputText) OutputText->SetText(FText::FromString(Content));
}
void ULlamaDialogueWidget::StopInference()
{
    if (bOwnsExperiment) { FLegoUpstreamModel::GetInstance().StopInference(); bOwnsExperiment = false; }
}
void ULlamaDialogueWidget::NativeDestruct() { StopInference(); Super::NativeDestruct(); }
void ULlamaDialogueWidget::SetAPM(int32 Value) { if (Value > 0 && Value <= 6000) { APM = Value; bAPMOverridden = true; } }
void ULlamaDialogueWidget::SetOpPolicy(int32 Value) { if (Value >= 0 && Value <= 2) { OperationPolicy = Value; bPolicyOverridden = true; } }
void ULlamaDialogueWidget::SetSkipPolicy(int32 Value) { FTaskScheduler::GetInstance().SetSkipPolicy(Value); }
void ULlamaDialogueWidget::SetSkipDepth(int32 Value) { FTaskScheduler::GetInstance().SetSkipDepth(Value); }
float ULlamaDialogueWidget::GetAverageFPS() { return FPerformanceDetector::GetInstance().GetAverageFPS(); }
float ULlamaDialogueWidget::GetInferenceTime() { return FLegoUpstreamModel::GetInstance().GetInferenceTime(); }
float ULlamaDialogueWidget::GetTotalInferenceTime() { return FLegoUpstreamModel::GetInstance().GetTotalInferenceTime(); }
