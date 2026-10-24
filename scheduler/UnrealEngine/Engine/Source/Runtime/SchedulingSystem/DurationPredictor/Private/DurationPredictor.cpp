#include "../Public/DurationPredictor.h"

#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

FDurationPredictor& FDurationPredictor::GetInstance(FString& ModelName)
{
	FString Add("add");
	FString Silu("silu");
	FString Mul("mul");
	FString Cpy("cpy");
	FString RmsNorm("rms_norm");
	FString Rope("rope");
	FString SoftMax("soft_max");
	FString MulMat("mul_mat");
	FString Empty("");
	static FDurationPredictor AddInstance(Add);
	static FDurationPredictor SiluInstance(Silu);
	static FDurationPredictor MulInstance(Mul);
	static FDurationPredictor CpyInstance(Cpy);
	static FDurationPredictor RmsNormInstance(RmsNorm);
	static FDurationPredictor RopeInstance(Rope);
	static FDurationPredictor SoftMaxInstance(SoftMax);
	static FDurationPredictor MulMatInstance(MulMat);
	static FDurationPredictor EmptyInstance(Empty);

	if      (ModelName == Add)      return AddInstance;
	else if (ModelName == Silu)     return SiluInstance;
	else if (ModelName == Mul)      return MulInstance;
	else if (ModelName == Cpy)      return CpyInstance;
	else if (ModelName == RmsNorm)  return RmsNormInstance;
	else if (ModelName == Rope)     return RopeInstance;
	else if (ModelName == SoftMax)  return SoftMaxInstance;
	else if (ModelName == MulMat)   return MulMatInstance;
	return EmptyInstance;
}

int FDurationPredictor::GetDuration(TArray<int> Input)
{
	FString dim;
	for (int i = 0; i < Input.Num(); i++)
	{
		FString num((std::to_string(Input[i])).c_str());
		dim += num;
		if (i != Input.Num() - 1) dim += TEXT(",");
	}
	if (DurationMap.Contains(dim)) return DurationMap[dim];
	else return 1000;
}

FDurationPredictor::FDurationPredictor(FString& ModelName)
{

	FString FileName = "duration_" + ModelName + ".txt";
    FString ResourceRoot;
    if (GConfig) GConfig->GetString(TEXT("LEGO"), TEXT("DurationResources"), ResourceRoot, GGameIni);
    FParse::Value(FCommandLine::Get(), TEXT("LegoDurationResources="), ResourceRoot);
    if (ResourceRoot.IsEmpty()) ResourceRoot = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("LEGO/DurationResources"));
    const FString FilePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(ResourceRoot, FileName));
	TArray<FString> FileLines;

	int idx = 13;
	if (ModelName == "rms_norm" || ModelName == "silu") idx = 10;

	if (FFileHelper::LoadFileToStringArray(FileLines, *FilePath))
	{
		for (FString Line : FileLines)
		{
			TArray<FString> array;
			Line.ParseIntoArray(array, TEXT(","), true);
			FString SDuration = array.Last();
			Line = Line.LeftChop(SDuration.Len() + 1);
			int Duration = FCString::Atoi(*SDuration);
			DurationMap.Add(Line, Duration);
			UE_LOG(LogTemp, Warning, TEXT("%s"), *Line);
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("Failed to read file"));
	}
}
