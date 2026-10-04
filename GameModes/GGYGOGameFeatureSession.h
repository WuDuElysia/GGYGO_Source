#pragma once

#include "CoreMinimal.h"
#include "Containers/Set.h"
#include "GameFeatureStateHandle.h"
#include "GameModes/GGYGOGameFeatureSubsystem.h"
#include "Misc/Optional.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"

class UGameFeaturesSubsystem;
class UGameInstance;
class UWorld;

/**
 * One original World's native Active resource, not a plugin state/reference-count authority.
 * Save this fully constructed object before Start: completions may be synchronous.
 * All resource operations require GT; ThreadSafe sharing protects memory lifetime only.
 * Supports the frozen GI's normal engine lifetime, with stable metadata/lifetime control.
 */
class GGYGO_API FGGYGOGameFeatureSession final
	: public TSharedFromThis<FGGYGOGameFeatureSession, ESPMode::ThreadSafe>
{
public:
	enum class EStartStatus : uint8 { Ready, Rejected, Failed, Interrupted };
	struct FStartResult
	{
		EStartStatus Status = EStartStatus::Rejected;
		TArray<FString> RootPluginURLs;
		TArray<FString> Errors;
	};
	using FStartCompletion = TFunction<void(const FStartResult&)>;

	enum class ECloseStatus : uint8 { Closed, Failed, Unconfirmed, Pending, RejectedWrongThread };
	struct FCloseResult
	{
		ECloseStatus Status = ECloseStatus::Unconfirmed;
		FString Error;
	};
	using FCloseCompletion = TFunction<void(const FCloseResult&)>;

	/** Binds weak World/GI identity; no native handle/request is created here. */
	static TSharedPtr<FGGYGOGameFeatureSession, ESPMode::ThreadSafe> TryCreate(
		const UWorld& OriginalWorld, const FString& OwnerLabel, FString& OutError);
	/**
	 * Once only, with nonempty explicit input and a receiver. The GI remains the sole admission
	 * for closure/source/Loaded preparation, including rejecting Borrowed. Empty normal no-GF
	 * configuration must be handled by the successful configuration consumer before this call.
	 * Ready needs every real root result, dispatch return and actual full activation-set Active.
	 * Explicit Close/invalid original context suppresses the observer, not native bookkeeping.
	 * Receivers must bind weak observers and revalidate this session/original World identity.
	 */
	void Start(FGGYGOGameFeatureClosureResolver::FInput Input, FStartCompletion Completion);
	/** GT-only query of this completed operation, original lease and current native Active states. */
	bool IsReadyFor(const UWorld& World) const;
	/**
	 * Closes admission/observer first; drains real work, then withdraws only this handle.
	 * Lease stays through release callback AND call return/unregistration. Native false is Failed,
	 * never Closed; unavailable/changed infrastructure is Unconfirmed. Duplicate pending callers
	 * receive Pending once; terminal duplicates replay the original result, without native retry.
	 */
	void Close(FCloseCompletion Completion = {});
	/** Independent moved-handle/lease cleanup; never AsShared or raw-this capture in destruction. */
	~FGGYGOGameFeatureSession();

	FGGYGOGameFeatureSession(const FGGYGOGameFeatureSession&) = delete;
	FGGYGOGameFeatureSession& operator=(const FGGYGOGameFeatureSession&) = delete;

private:
	using FLoadedLease = UGGYGOGameFeatureSubsystem::FLoadedLease;
	FGGYGOGameFeatureSession(const UWorld& World, UGameInstance& GI, FString Label);
	const UWorld* GetOriginalWorld(FString& OutError) const;
	UGameFeaturesSubsystem* CheckActiveAdmission(FString& OutError) const;
	bool ValidateSnapshots(FString& OutError) const;
	void OnLoaded(FLoadedLease Lease, const FGGYGOGameFeatureRetention::FLoadResult& Result, bool bArrivedOnGT);
	void BeginActivations();
	void OnRootActivated(const FString& URL, bool bSucceeded, FString NativeError, bool bArrivedOnGT);
	void RecordStartFailure(EStartStatus Status, FString Error);
	void FinishStartIfReady();
	void StartReleaseIfReady();
	void OnReleased(bool bSucceeded, bool bArrivedOnGT);
	void FinishReleaseIfReady();
	void CompleteClose(FCloseResult Result, bool bReturnLease);

	const FString OwnerLabel;
	TWeakObjectPtr<const UWorld> OriginalWorld;
	TWeakObjectPtr<UGameInstance> OriginalGameInstance;
	FLoadedLease LoadedLease;
	FGameFeatureStateHandle ActiveHandle;
	/** Retains actual unfinished work even if a native delegate is lost; never fakes completion. */
	TSharedPtr<FGGYGOGameFeatureSession, ESPMode::ThreadSafe> WorkKeepAlive;
	TSet<FString> PendingRoots;
	FStartResult StartResult;
	TOptional<EStartStatus> StartFailure;
	FStartCompletion StartCompletion;
	FCloseCompletion CloseCompletion;
	TOptional<FCloseResult> CompletedClose;
	TOptional<bool> NativeReleaseSucceeded;
	FString ReleaseError;
	bool bStarted = false;
	bool bStartPublished = false;
	bool bPreparingLoaded = false;
	bool bPrepareCallReturned = true;
	bool bActivationDispatchReturned = true;
	bool bClosing = false;
	bool bCloseRequested = false;
	bool bReleaseStarted = false;
	bool bReleaseCallReturned = false;
	bool bReleaseGuardsMaintained = true;
};
