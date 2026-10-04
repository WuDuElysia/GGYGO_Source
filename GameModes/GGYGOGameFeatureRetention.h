#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "GameFeatureStateHandle.h"
#include "Misc/Optional.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"

/**
 * Owns one native Loaded requirement handle, without a GI or production caller.
 * All API calls require the game thread. ThreadSafe sharing only protects memory lifetime.
 * The future host must keep its owner reference through map travel and pending/Active cleanup.
 */
class GGYGO_API FGGYGOGameFeatureRetention
	: public TSharedFromThis<FGGYGOGameFeatureRetention, ESPMode::ThreadSafe>
{
public:
	enum class ERetainStatus : uint8
	{
		Rejected,
		Submitted,
		Interrupted
	};

	struct FRetainResult
	{
		ERetainStatus Status = ERetainStatus::Rejected;
		/** Native calls submitted in this invocation; not acknowledgements or usage counts. */
		int32 NumSubmittedURLs = 0;
		FString Error;
	};

	enum class ELoadStatus : uint8
	{
		Rejected,
		Loaded,
		Failed,
		Interrupted
	};

	struct FLoadResult
	{
		ELoadStatus Status = ELoadStatus::Rejected;
		/** Copied, deduplicated requested closure; not a persistent plugin state cache. */
		TArray<FString> PluginURLs;
		TArray<FString> Errors;
	};

	using FLoadCompletion = TFunction<void(const FLoadResult&)>;

	enum class EReleaseStatus : uint8
	{
		Released,
		Failed,
		Unconfirmed,
		Pending,
		RejectedWrongThread
	};

	struct FReleaseResult
	{
		EReleaseStatus Status = EReleaseStatus::Unconfirmed;
		FString Error;
	};

	using FReleaseCompletion = TFunction<void(const FReleaseResult&)>;

	/** Null with OutError on rejection. InitAndRegister's bool alone is insufficient. */
	static TSharedPtr<FGGYGOGameFeatureRetention, ESPMode::ThreadSafe> TryCreate(
		const FString& InOwnerLabel, FString& OutError);

	/**
	 * Caller must already prove GFP identity, the complete dependency closure and managed ownership.
	 * Nonempty inputs are deduplicated, then all must be exactly Loaded or Active before any Add.
	 * Submitted only means void native calls were issued with observed prerequisites maintained.
	 * Interrupted may leave submitted references, which remain owned by this resource.
	 */
	FRetainResult RetainAlreadyLoaded(TConstArrayView<FString> ValidatedManagedURLs);

	/**
	 * Caller must prove the complete installed/mapped GFP closure, explicit managed ownership and
	 * a legal native policy lifetime covering loading. A resolver candidate alone is insufficient.
	 * Every URL (including dependencies with shouldActivate=false) is loaded through this same
	 * native handle. Input is copied/deduplicated; empty inputs/URLs or an empty completion reject.
	 * Loaded requires every native result to succeed and all URLs to be observed Loaded/Active;
	 * it is not a per-handle reference acknowledgement. Native errors always remain failures.
	 * Native wrappers can add references even on failure; those references remain owned here.
	 * Synchronous completion is published only after the native call returns, at most once.
	 * Close stops not-yet-issued URLs, drains actual calls, then releases; it does not cancel
	 * shared plugin transitions. An interrupted closure can never report Loaded.
	 * A game-thread close turns an otherwise successful pending result into Interrupted.
	 * Wrong-thread admission rejects on the calling thread without mutating resources.
	 * No policy is acquired/cached here; subsystem existence cannot prove policy readiness.
	 */
	void LoadAndRetainValidatedManagedClosure(
		TConstArrayView<FString> ValidatedManagedURLs, FLoadCompletion Completion);

	/**
	 * Accepted game-thread calls close this resource first. Each completion is invoked at most once.
	 * Accepted loads must finish and their native calls must return before native release starts.
	 * A duplicate while draining/releasing receives Pending immediately, without later notification.
	 * A completed duplicate receives the cached terminal result; invalid handles are not retried.
	 * RejectedWrongThread leaves the resource untouched and calls back on that calling thread.
	 * Released requires native success, return from ReleaseAndUnregister, and an invalidated handle.
	 * The future host, not this core, decides when GI/pending/Active lifetimes permit release.
	 */
	void ReleaseOwnedReferences(FReleaseCompletion Completion = {});

	/** Detached native cleanup only; never calls AsShared or captures this. */
	~FGGYGOGameFeatureRetention();

	FGGYGOGameFeatureRetention(const FGGYGOGameFeatureRetention&) = delete;
	FGGYGOGameFeatureRetention& operator=(const FGGYGOGameFeatureRetention&) = delete;

private:
	struct FLoadOperation;
	explicit FGGYGOGameFeatureRetention(const FString& InOwnerLabel);
	bool CheckRetainPrerequisites(FString& OutError) const;
	void OnNativeLoadCompleted(const TSharedRef<FLoadOperation, ESPMode::ThreadSafe>& Operation,
		const FString& URL, bool bSucceeded, const FString& NativeError, bool bArrivedOnGameThread);
	void FinishLoadIfReady(const TSharedRef<FLoadOperation, ESPMode::ThreadSafe>& Operation);
	void StartNativeReleaseIfReady();
	void OnNativeReleaseCompleted(bool bSucceeded, bool bArrivedOnGameThread);
	void FinishReleaseIfReady();

	FString OwnerLabel;
	FGameFeatureStateHandle StateHandle;
	/** Only owned in-flight call/completion records; no authoritative plugin or usage state. */
	TArray<TSharedRef<FLoadOperation, ESPMode::ThreadSafe>> PendingLoads;
	// These describe this resource's release only, never a plugin's authoritative state.
	bool bClosing = false;
	bool bNativeReleaseStarted = false;
	bool bNativeReleaseCallReturned = false;
	bool bReleasePrerequisitesMaintained = true;
	FString ReleaseError;
	TOptional<bool> NativeReleaseSucceeded;
	TOptional<FReleaseResult> CompletedRelease;
	FReleaseCompletion ReleaseCompletion;
};
