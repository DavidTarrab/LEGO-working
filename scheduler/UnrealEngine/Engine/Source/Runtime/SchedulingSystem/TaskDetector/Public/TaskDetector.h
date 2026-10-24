#pragma once
#include <mutex>
#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/ThreadSafeCounter.h"
#include "HAL/ThreadSafeCounter64.h"


class FRenderTaskDetector: public FRunnable
{
public:

	virtual bool Init() { return true; }
	virtual uint32_t Run();
	virtual void Exit() {}
};

class TASKDETECTOR_API FTaskDetector
{
public:
	FRenderTaskDetector* Detector;
	int WaitForRenderingCompletionTaskCount;
	std::mutex MutexForWaitForRenderingCompletionTaskCount;
	static FTaskDetector& GetInstance();
	bool GetRenderingTaskIsRunning();
	TAtomic<bool>& GetIsInFrameRendering();
	TAtomic<bool>& GetRHITaskIsRunning();
	TAtomic<bool>& GetIsInInferring();
	TAtomic<bool>& GetIsPrefilling();
	TAtomic<bool>& GetActionIsRunning();
	TAtomic<bool>& GetTotalInferenceIsRunning();
	TAtomic<bool>& GetFirstDrawTask();
	TAtomic<bool>& GetIsInGraphComputing();
	TAtomic<int>& GetNodeCount();
	TAtomic<int>& GetScheduledCount();
	TAtomic<int>& GetActionCount();
	void SetRenderingTaskIsRunning(const bool State);
	void SetIsInFrameRendering(const bool State);
	void SetIsInInferring(const bool State);
	void SetRHITaskIsRunning(const bool State);
	void SetIsPrefilling(const bool State);
	void SetActionIsRunning(const bool State);
	void SetTotalInferenceIsRunning(const bool State);
	void SetFirstDrawTask(const bool State);
	void SetNodeCount(const int State);
	void SetScheduledCount(const int State);
	void SetIsInGraphComputing(const bool State);
	void SetActionCount(const int State);
    void BeginGPUWork() { PendingGPUWork.Increment(); TotalGPUWork.Increment(); }
    void EndGPUWork() { const int32 Remaining = PendingGPUWork.Decrement(); check(Remaining >= 0); }
    void BeginRHIWork() { PendingRHIWork.Increment(); }
    void EndRHIWork() { const int32 Remaining = PendingRHIWork.Decrement(); check(Remaining >= 0); }
    bool IsRHIWorkPending() const { return PendingRHIWork.GetValue() > 0 || bRHITaskIsRunning.Load(); }
    int64 GetGPUWorkCount() const { return TotalGPUWork.GetValue(); }
    int64 GetFrameCount() const { return TotalFrames.GetValue(); }
    int32 GetPendingGPUCount() const { return PendingGPUWork.GetValue(); }
private:
	FTaskDetector();
    FThreadSafeCounter PendingGPUWork;
    FThreadSafeCounter PendingRHIWork;
    FThreadSafeCounter64 TotalGPUWork;
    FThreadSafeCounter64 TotalFrames;
	TAtomic<bool> bRHITaskIsRunning;
	TAtomic<bool> bRenderingTaskIsRunning;
	TAtomic<bool> bIsInFrameRendering;
	TAtomic<bool> bIsInInferring;
	TAtomic<bool> bIsPrefilling;
	TAtomic<bool> bActionIsRunning;
	TAtomic<bool> bTotalInferenceIsRunning;
	TAtomic<bool> bFirstDrawTask;
	TAtomic<bool> bIsInGraphComputing;
	TAtomic<int> NodeCount;
	TAtomic<int> ScheduledCount;
	TAtomic<int> ActionCount;
};

struct FLegoRHIActivityScope
{
    FLegoRHIActivityScope() { FTaskDetector::GetInstance().BeginRHIWork(); }
    ~FLegoRHIActivityScope() { FTaskDetector::GetInstance().EndRHIWork(); }
};
