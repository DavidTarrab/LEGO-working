#include "LegoExperimentGameMode.h"
#include "LlamaDialogueWidget.h"
#include "LegoUpstreamModel.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "GameFramework/Pawn.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

ALegoExperimentGameMode::ALegoExperimentGameMode()
{
    PrimaryActorTick.bCanEverTick = true;
    static ConstructorHelpers::FClassFinder<APawn> Pawn(TEXT("/Game/ThirdPersonBP/Blueprints/ThirdPersonCharacter"));
    if (Pawn.Class) DefaultPawnClass = Pawn.Class;
}
void ALegoExperimentGameMode::BeginPlay()
{
    Super::BeginPlay();
    bQuitAfterRun = FParse::Param(FCommandLine::Get(), TEXT("LegoQuitAfterRun"));
    if (FParse::Param(FCommandLine::Get(), TEXT("LegoRenderOnly")))
    {
        float FPS = 60;
        FParse::Value(FCommandLine::Get(), TEXT("LegoFPS="), FPS);
        if (auto* Cap = IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS"))) Cap->Set(FPS);
        if (auto* VSync = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VSync"))) VSync->Set(0);
        GetWorldTimerManager().SetTimer(StartTimer, this, &ALegoExperimentGameMode::BeginRenderWindow, 5.0f, false);
        return;
    }
    if (FParse::Param(FCommandLine::Get(), TEXT("LegoAutoStart")))
        GetWorldTimerManager().SetTimer(StartTimer, this, &ALegoExperimentGameMode::StartExperiment, 3.0f, false);
}
void ALegoExperimentGameMode::StartExperiment()
{
    Widget = CreateWidget<ULlamaDialogueWidget>(GetWorld(), ULlamaDialogueWidget::StaticClass());
    if (Widget) { Widget->AddToViewport(); Widget->GetNextPrompt(); Widget->LlamaInference(); }
    bExperimentStarted = FLegoUpstreamModel::GetInstance().IsRunning();
    if (!bExperimentStarted && bQuitAfterRun) FPlatformMisc::RequestExit(false);
}
void ALegoExperimentGameMode::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (bExperimentStarted && bQuitAfterRun && !FLegoUpstreamModel::GetInstance().IsRunning())
    {
        bExperimentStarted = false;
        FPlatformMisc::RequestExit(false);
    }
}
void ALegoExperimentGameMode::BeginRenderWindow()
{
    RenderOutput = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("LEGO/render"));
    FParse::Value(FCommandLine::Get(), TEXT("LegoRenderOutput="), RenderOutput);
    RenderOutput = FPaths::ConvertRelativePathToFull(RenderOutput);
    IFileManager::Get().MakeDirectory(*RenderOutput, true);
    RenderStartQPC = FPlatformTime::Cycles64();
    SaveRenderWindow(false);
    float Seconds = 120;
    FParse::Value(FCommandLine::Get(), TEXT("LegoSeconds="), Seconds);
    GetWorldTimerManager().SetTimer(EndTimer, this, &ALegoExperimentGameMode::EndRenderWindow, FMath::Clamp(Seconds, 1.0f, 86400.0f), false);
}
void ALegoExperimentGameMode::EndRenderWindow()
{
    SaveRenderWindow(true);
    if (bQuitAfterRun) FPlatformMisc::RequestExit(false);
}
void ALegoExperimentGameMode::SaveRenderWindow(bool Complete)
{
    auto Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("start_qpc_ticks"), FString::Printf(TEXT("%llu"), RenderStartQPC));
    Data->SetNumberField(TEXT("qpc_frequency"), 1.0 / FPlatformTime::GetSecondsPerCycle64());
    Data->SetBoolField(TEXT("complete"), Complete);
    if (Complete) Data->SetStringField(TEXT("end_qpc_ticks"), FString::Printf(TEXT("%llu"), FPlatformTime::Cycles64()));
    FString Text;
    auto Writer = TJsonWriterFactory<>::Create(&Text);
    if (FJsonSerializer::Serialize(Data, Writer)) FFileHelper::SaveStringToFile(Text, *FPaths::Combine(RenderOutput, TEXT("measurement.json")));
}
void ALegoExperimentGameMode::EndPlay(const EEndPlayReason::Type Reason)
{
    GetWorldTimerManager().ClearTimer(StartTimer);
    GetWorldTimerManager().ClearTimer(EndTimer);
    if (Widget) { Widget->StopInference(); Widget->RemoveFromParent(); Widget = nullptr; }
    Super::EndPlay(Reason);
}
