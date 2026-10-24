#pragma once
#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "LlamaDialogueWidget.generated.h"
class UTextBlock;

UCLASS()
class DIALOGUEDEMO_API ULlamaDialogueWidget : public UUserWidget
{
    GENERATED_BODY()
public:
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="LEGO") FString Content;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="LEGO") int32 Order = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="LEGO") int32 APM = 200;
    UFUNCTION(BlueprintCallable, Category="LEGO") bool GetNextPrompt();
    UFUNCTION(BlueprintCallable, Category="LEGO") void LlamaInference();
    UFUNCTION(BlueprintCallable, Category="LEGO") void StopInference();
    UFUNCTION(BlueprintCallable, Category="LEGO") void SetAPM(int32 Value);
    UFUNCTION(BlueprintCallable, Category="LEGO") void SetOpPolicy(int32 Value);
    UFUNCTION(BlueprintCallable, Category="LEGO") void SetSkipPolicy(int32 Value);
    UFUNCTION(BlueprintCallable, Category="LEGO") void SetSkipDepth(int32 Value);
    UFUNCTION(BlueprintCallable, Category="LEGO") float GetAverageFPS();
    UFUNCTION(BlueprintCallable, Category="LEGO") float GetInferenceTime();
    UFUNCTION(BlueprintCallable, Category="LEGO") float GetTotalInferenceTime();
protected:
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;
private:
    FString Prompt;
    UPROPERTY() UTextBlock* OutputText = nullptr;
    int32 OperationPolicy = 2;
    bool bPolicyOverridden = false, bAPMOverridden = false, bOwnsExperiment = false;
};
