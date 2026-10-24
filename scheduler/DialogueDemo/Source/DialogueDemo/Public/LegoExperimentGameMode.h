#pragma once
#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "LegoExperimentGameMode.generated.h"
class ULlamaDialogueWidget;

UCLASS()
class DIALOGUEDEMO_API ALegoExperimentGameMode : public AGameModeBase
{
    GENERATED_BODY()
public:
    ALegoExperimentGameMode();
    virtual void Tick(float DeltaSeconds) override;
protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
    void StartExperiment();
    void BeginRenderWindow();
    void EndRenderWindow();
    void SaveRenderWindow(bool Complete);
    FTimerHandle StartTimer;
    FTimerHandle EndTimer;
    bool bExperimentStarted = false, bQuitAfterRun = false;
    uint64 RenderStartQPC = 0;
    FString RenderOutput;
    UPROPERTY() ULlamaDialogueWidget* Widget = nullptr;
};
