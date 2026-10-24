#pragma once

#include "CoreMinimal.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>

class DURATIONPREDICTOR_API FDurationPredictor {
public:
	static FDurationPredictor& GetInstance(FString& ModelName);

	int GetDuration(TArray<int> Input);

private:
	FDurationPredictor(FString& ModelName);

	TMap<FString, int> DurationMap;
};
