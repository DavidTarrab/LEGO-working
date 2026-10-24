#pragma once

#include "DurationPredictor.h"
#include "TaskDetector.h"
#include "PerformanceDetector.h"
#include "CoreMinimal.h"
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <mutex>

class TASKSCHEDULER_API FTaskScheduler
{
public:
	static FTaskScheduler& GetInstance();
	void WaitForRenderingTaskCompletion();
    bool WaitForAdmission(const char* Node = nullptr);
    void BeginExperiment(double TimeoutSeconds, bool ResetCalibration = true);
    void SaveCalibration(const FString& File);
    void CancelExperiment();
    bool IsCancelled() const { return bCancelled.load(); }
    double GetAdmissionWaitMs() const { return AdmissionWaitMs.load(); }
    void RecordChunk(const char* Name, double SubmitSeconds, double CompleteSeconds, bool FrameAtAdmit = false, bool GPUAtAdmit = false, bool RHIAtAdmit = false);
    void SaveChunkRecords(const FString& File);
    int64 GetDroppedChunkRecords() const { return DroppedChunkRecords.load(); }
    int64 GetTotalChunkCount() const { return TotalChunks.load(); }
    int64 GetFrameAdmissionCount() const { return FrameAdmissions.load(); }
    int64 GetGPUAdmissionCount() const { return GPUAdmissions.load(); }
    int64 GetRHIAdmissionCount() const { return RHIAdmissions.load(); }

	int GetExitLayerNum();
	int GetDecodePhase();
	int GetSkipDepth();
	void SetSkipDepth(const int SkipDepth);
	void SetDecodePhase(const int Phase);
	void SetSLOTime(const int Time);
	void SetTokenBeginTime(const double Time);
	void SetAllGraphNodes(const int Nodes);
	void SetExeGraphNodes(const int Nodes);
	void SetFirstLayerNum(const int Num);
	void SetNowLayerNum(const int Num);
	void SetIsRenderingEnd(const bool flag);
	void SetUpdateInPhase2(const bool flag);

	int GetSkipPolicy();
	void SetSkipPolicy(const int Policy);

	int GetOpPolicy();
	void SetOpPolicy(const int Policy);

	void UpdateSkipDepth1();
	void UpdateSkipDepth2();

	void UpdateSkipDepthAfterPrefilling(const double StallTimeForAction, const double DecodeTimePerLayer, const double DecodeTimePerToken, const double PrefillTimePerLayer, const double PrefillTimePerToken);

	~FTaskScheduler();
private:
	FTaskScheduler();
	TAtomic<int> Performance;
	TAtomic<int> ExitLayerNum;
	TAtomic<int> TokenDecodePhase;
	TAtomic<double> TokenBeginTime;
	TAtomic<double> TokenDecodingTime;
	TAtomic<bool> IsRenderingEnd;
	TAtomic<int>  AllGraphNodes;
	TAtomic<int>  ExeGraphNodes;
	TAtomic<int> FirstLayerNum;
	int NowLayerNum;
	TAtomic<int> SLOTime;
	bool UpdateInPhase2;

	TAtomic<int> OpPolicy;
	TAtomic<int> SkipPolicy;
	TAtomic<int> SkipDepth;
    std::atomic<bool> bCancelled{true};
    std::atomic<double> AdmissionWaitMs{0};
    double AdmissionTimeoutSeconds = 2;
    double EstimatedChunkMs = 0.1;
    TMap<FString, double> CalibrationMaxMs;
    TMap<FString, int32> CalibrationSamples;
    double OriginSeconds = 0;
    std::mutex AdmissionMutex;
    std::condition_variable Wake;
    TArray<FString> ChunkRecords;
    std::atomic<int64> DroppedChunkRecords{0};
    std::atomic<int64> TotalChunks{0}, FrameAdmissions{0}, GPUAdmissions{0}, RHIAdmissions{0};
    uint64 SampleState = 0x9E3779B97F4A7C15ull;


};

class HighPrecisionTimer {
public:
	void SpinSleepMicroseconds(int64_t microseconds);
	static HighPrecisionTimer& GetInstance();
private:
	HighPrecisionTimer();
};


