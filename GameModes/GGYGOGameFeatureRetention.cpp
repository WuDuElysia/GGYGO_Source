#include "GameModes/GGYGOGameFeatureRetention.h"

#include "Async/Async.h"
#include "Containers/Set.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "GameFeatureStateHandleReferenceController.h"
#include "GameFeaturesSubsystem.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOGameFeatureRetention, Log, All);

namespace
{
	bool CheckNativeExecution(FString& OutError)
	{
		if (!IsInGameThread())
		{
			OutError = TEXT("GameFeature retention requires the game thread.");
			return false;
		}
		if (IsEngineExitRequested())
		{
			OutError = TEXT("Engine exit requested; native cleanup/registration cannot be confirmed.");
			return false;
		}
		if (!FGameFeatureStateHandleReferenceController::IsLifetimeControlEnabled())
		{
			OutError = TEXT("GameFeatureStateHandle.Enabled is disabled; cleanup/registration cannot be confirmed.");
			return false;
		}
		return true;
	}

	UGameFeaturesSubsystem* GetAvailableGameFeaturesSubsystem(FString& OutError)
	{
		if (!GEngine)
		{
			OutError = TEXT("GEngine is unavailable; GameFeature retention admission is rejected.");
			return nullptr;
		}
		UGameFeaturesSubsystem* Subsystem = GEngine->GetEngineSubsystem<UGameFeaturesSubsystem>();
		if (!Subsystem)
		{
			OutError = TEXT("GameFeaturesSubsystem is unavailable; GameFeature retention admission is rejected.");
		}
		return Subsystem;
	}

	bool CheckStableLoadedState(const FString& URL, FString& OutError)
	{
		UGameFeaturesSubsystem* Subsystem = GetAvailableGameFeaturesSubsystem(OutError);
		if (!Subsystem)
		{
			return false;
		}
		const EGameFeaturePluginState State = Subsystem->GetPluginState(URL);
		if (State != EGameFeaturePluginState::Loaded && State != EGameFeaturePluginState::Active)
		{
			OutError = FString::Printf(TEXT("URL [%s] is [%s], not stable Loaded or Active."),
				*URL, *UE::GameFeatures::ToString(State));
			return false;
		}
		return true;
	}
}

struct FGGYGOGameFeatureRetention::FLoadOperation
{
	// Only this request's immutable input and actual outstanding native callbacks are tracked.
	TArray<FString> URLs;
	TSet<FString> PendingURLs;
	FLoadResult Result;
	FLoadCompletion Completion;
	bool bNativeCallReturned = false;
	bool bPublished = false;
};

FGGYGOGameFeatureRetention::FGGYGOGameFeatureRetention(const FString& InOwnerLabel)
	: OwnerLabel(InOwnerLabel)
{
}

TSharedPtr<FGGYGOGameFeatureRetention, ESPMode::ThreadSafe> FGGYGOGameFeatureRetention::TryCreate(
	const FString& InOwnerLabel, FString& OutError)
{
	OutError.Reset();
	if (!CheckNativeExecution(OutError) || !GetAvailableGameFeaturesSubsystem(OutError))
	{
		return nullptr;
	}
	if (InOwnerLabel.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("GameFeature retention OwnerLabel must not be empty.");
		return nullptr;
	}

	TSharedPtr<FGGYGOGameFeatureRetention, ESPMode::ThreadSafe> Resource =
		MakeShareable(new FGGYGOGameFeatureRetention(InOwnerLabel));
	const bool bInitialized = Resource->StateHandle.InitAndRegister(
		InOwnerLabel, EGameFeatureStateHandleOptions::None);
	if (!CheckNativeExecution(OutError) || !GetAvailableGameFeaturesSubsystem(OutError))
	{
		return nullptr;
	}
	if (!bInitialized || !Resource->StateHandle.IsValid())
	{
		OutError = TEXT("Native retention handle did not initialize to a valid GUID.");
		return nullptr;
	}
	return Resource;
}

bool FGGYGOGameFeatureRetention::CheckRetainPrerequisites(FString& OutError) const
{
	if (!CheckNativeExecution(OutError))
	{
		return false;
	}
	if (bClosing)
	{
		OutError = TEXT("Retention resource is closing; new registration is rejected.");
		return false;
	}
	if (!StateHandle.IsValid())
	{
		OutError = TEXT("Retention resource has no valid native handle.");
		return false;
	}
	return GetAvailableGameFeaturesSubsystem(OutError) != nullptr;
}

FGGYGOGameFeatureRetention::FRetainResult FGGYGOGameFeatureRetention::RetainAlreadyLoaded(
	TConstArrayView<FString> ValidatedManagedURLs)
{
	FRetainResult Result;
	if (!CheckRetainPrerequisites(Result.Error))
	{
		return Result;
	}
	// Protect this invocation independently of the caller's owner reference.
	const TSharedRef<FGGYGOGameFeatureRetention, ESPMode::ThreadSafe> KeepAlive = AsShared();
	if (ValidatedManagedURLs.IsEmpty())
	{
		Result.Error = TEXT("ValidatedManagedURLs must not be empty.");
		return Result;
	}

	TSet<FString> Seen;
	TArray<FString> UniqueURLs;
	for (const FString& URL : ValidatedManagedURLs)
	{
		if (URL.TrimStartAndEnd().IsEmpty())
		{
			Result.Error = TEXT("ValidatedManagedURLs contains an empty URL.");
			return Result;
		}
		if (!Seen.Contains(URL))
		{
			Seen.Add(URL);
			UniqueURLs.Add(URL);
		}
	}
	// Validate the entire collection before issuing any native reference update.
	for (const FString& URL : UniqueURLs)
	{
		if (!CheckRetainPrerequisites(Result.Error) || !CheckStableLoadedState(URL, Result.Error))
		{
			return Result;
		}
	}

	auto Interrupted = [&Result]()
	{
		Result.Status = Result.NumSubmittedURLs > 0 ? ERetainStatus::Interrupted : ERetainStatus::Rejected;
		Result.Error += TEXT(" Any submitted references remain owned here; no native acknowledgement is available.");
		return Result;
	};
	for (const FString& URL : UniqueURLs)
	{
		if (!CheckRetainPrerequisites(Result.Error) || !CheckStableLoadedState(URL, Result.Error))
		{
			return Interrupted();
		}
		FGameFeatureStateHandleReferenceController::Get().AddOrUpdateReference(
			StateHandle, URL, EGameFeaturePluginState::Loaded, false);
		++Result.NumSubmittedURLs;
		if (!CheckRetainPrerequisites(Result.Error) || !CheckStableLoadedState(URL, Result.Error))
		{
			return Interrupted();
		}
	}
	for (const FString& URL : UniqueURLs)
	{
		if (!CheckRetainPrerequisites(Result.Error) || !CheckStableLoadedState(URL, Result.Error))
		{
			return Interrupted();
		}
	}
	Result.Status = ERetainStatus::Submitted;
	return Result;
}

void FGGYGOGameFeatureRetention::LoadAndRetainValidatedManagedClosure(
	TConstArrayView<FString> ValidatedManagedURLs, FLoadCompletion Completion)
{
	const TSharedRef<FGGYGOGameFeatureRetention, ESPMode::ThreadSafe> KeepAlive = AsShared();
	auto Reject = [this, &Completion](const FString& Error)
	{
		FLoadResult Result;
		Result.Errors.Add(FString::Printf(TEXT("[%s] %s"), *OwnerLabel, *Error));
		UE_LOG(LogGGYGOGameFeatureRetention, Error, TEXT("%s"), *Result.Errors[0]);
		if (Completion)
		{
			Completion(Result);
		}
	};
	if (!Completion)
	{
		Reject(TEXT("Managed closure loading requires a completion receiver; no request was issued."));
		return;
	}
	FString Error;
	if (!CheckRetainPrerequisites(Error))
	{
		Reject(Error);
		return;
	}
	if (ValidatedManagedURLs.IsEmpty())
	{
		Reject(TEXT("Validated managed closure must not be empty."));
		return;
	}

	TSet<FString> Seen;
	TArray<FString> UniqueURLs;
	for (const FString& URL : ValidatedManagedURLs)
	{
		if (URL.TrimStartAndEnd().IsEmpty())
		{
			Reject(TEXT("Validated managed closure contains an empty URL; no request was issued."));
			return;
		}
		if (!Seen.Contains(URL))
		{
			Seen.Add(URL);
			UniqueURLs.Add(URL);
		}
	}

	const TSharedRef<FLoadOperation, ESPMode::ThreadSafe> Operation =
		MakeShared<FLoadOperation, ESPMode::ThreadSafe>();
	Operation->URLs = MoveTemp(UniqueURLs);
	Operation->Result.Status = ELoadStatus::Loaded;
	Operation->Result.PluginURLs = Operation->URLs;
	Operation->Completion = MoveTemp(Completion);
	// Admission and lifetime protection precede every possible native observer/completion reentry.
	PendingLoads.Add(Operation);
	for (const FString& URL : Operation->URLs)
	{
		if (!CheckRetainPrerequisites(Error))
		{
			if (Operation->Result.Status != ELoadStatus::Failed)
			{
				Operation->Result.Status = ELoadStatus::Interrupted;
			}
			Operation->Result.Errors.Add(FString::Printf(
				TEXT("Stopped before submitting URL [%s]: %s"), *URL, *Error));
			break;
		}
		Operation->PendingURLs.Add(URL);
		// Use each actual native callback, not the multi-URL context's destructor notification.
		FGameFeatureStateHandleReferenceController::Get().LoadGameFeaturePlugin(
			StateHandle, URL, FGameFeatureProtocolOptions(),
			UGameFeatureStateHandleLoadComplete::CreateLambda(
				[KeepAlive, Operation, URL](const UE::GameFeatures::FResult& NativeResult)
			{
				// The native wrapper has already updated this handle, even for a failed URL.
				const bool bSucceeded = NativeResult.HasValue() && !NativeResult.HasError();
				FString NativeError;
				if (NativeResult.HasError())
				{
					NativeError = FString::Printf(TEXT("%s (%s)"),
						*NativeResult.GetError(), *NativeResult.OptionalErrorText.ToString());
				}
				else if (!bSucceeded)
				{
					NativeError = TEXT("Native load completion has no success value.");
				}
				if (IsInGameThread())
				{
					KeepAlive->OnNativeLoadCompleted(Operation, URL, bSucceeded, NativeError, true);
				}
				else
				{
					// Only marshal this native completion through Core's existing queue.
					AsyncTask(ENamedThreads::GameThread,
						[KeepAlive, Operation, URL, bSucceeded, NativeError]()
					{
						KeepAlive->OnNativeLoadCompleted(Operation, URL, bSucceeded, NativeError, false);
					});
				}
			}));
	}
	Operation->bNativeCallReturned = true;
	FinishLoadIfReady(Operation);
}

void FGGYGOGameFeatureRetention::OnNativeLoadCompleted(
	const TSharedRef<FLoadOperation, ESPMode::ThreadSafe>& Operation,
	const FString& URL, bool bSucceeded, const FString& NativeError, bool bArrivedOnGameThread)
{
	if (Operation->bPublished || Operation->PendingURLs.Remove(URL) == 0)
	{
		UE_LOG(LogGGYGOGameFeatureRetention, Error,
			TEXT("[%s] Duplicate native load completion ignored for URL [%s]."), *OwnerLabel, *URL);
		return;
	}
	if (!bSucceeded)
	{
		Operation->Result.Status = ELoadStatus::Failed;
		Operation->Result.Errors.Add(FString::Printf(
			TEXT("URL [%s] native load failed: %s"), *URL, *NativeError));
	}
	if (!bArrivedOnGameThread)
	{
		if (Operation->Result.Status != ELoadStatus::Failed)
		{
			Operation->Result.Status = ELoadStatus::Interrupted;
		}
		Operation->Result.Errors.Add(FString::Printf(
			TEXT("URL [%s] completion arrived off the game thread; loading is unconfirmed."), *URL));
	}
	FinishLoadIfReady(Operation);
}

void FGGYGOGameFeatureRetention::FinishLoadIfReady(
	const TSharedRef<FLoadOperation, ESPMode::ThreadSafe>& Operation)
{
	if (!Operation->bNativeCallReturned || !Operation->PendingURLs.IsEmpty() || Operation->bPublished)
	{
		return;
	}
	const TSharedRef<FGGYGOGameFeatureRetention, ESPMode::ThreadSafe> KeepAlive = AsShared();
	FLoadResult Result = Operation->Result;
	if (Result.Status == ELoadStatus::Loaded)
	{
		FString Error;
		if (!CheckRetainPrerequisites(Error))
		{
			Result.Status = ELoadStatus::Interrupted;
			Result.Errors.Add(MoveTemp(Error));
		}
		else
		{
			for (const FString& URL : Operation->URLs)
			{
				if (!CheckStableLoadedState(URL, Error))
				{
					Result.Status = ELoadStatus::Interrupted;
					Result.Errors.Add(MoveTemp(Error));
					break;
				}
			}
			if (Result.Status == ELoadStatus::Loaded && !CheckRetainPrerequisites(Error))
			{
				Result.Status = ELoadStatus::Interrupted;
				Result.Errors.Add(MoveTemp(Error));
			}
		}
	}
	else if (bClosing)
	{
		Result.Errors.Add(TEXT("Retention closed while this batch was pending; native errors remain failures."));
	}
	Operation->bPublished = true;
	PendingLoads.RemoveSingle(Operation);
	for (FString& Error : Result.Errors)
	{
		Error = FString::Printf(TEXT("[%s] %s"), *OwnerLabel, *Error);
		UE_LOG(LogGGYGOGameFeatureRetention, Error, TEXT("%s"), *Error);
	}
	FLoadCompletion Notify = MoveTemp(Operation->Completion);
	if (Notify)
	{
		Notify(Result);
	}
	// A user completion may close or admit another batch; consult current owned records afterwards.
	StartNativeReleaseIfReady();
}

void FGGYGOGameFeatureRetention::ReleaseOwnedReferences(FReleaseCompletion Completion)
{
	const TSharedRef<FGGYGOGameFeatureRetention, ESPMode::ThreadSafe> KeepAlive = AsShared();
	if (!IsInGameThread())
	{
		if (Completion)
		{
			const FReleaseResult Result{EReleaseStatus::RejectedWrongThread,
				TEXT("Release requires the game thread; resource remains untouched.")};
			Completion(Result);
		}
		return;
	}

	const bool bAlreadyClosing = bClosing;
	bClosing = true;
	if (CompletedRelease.IsSet())
	{
		const FReleaseResult Result = CompletedRelease.GetValue();
		if (Completion)
		{
			Completion(Result);
		}
		return;
	}
	if (bAlreadyClosing)
	{
		if (Completion)
		{
			const FReleaseResult Result{EReleaseStatus::Pending,
				TEXT("Load drain or native release is pending; this duplicate will not be notified again.")};
			Completion(Result);
		}
		return;
	}

	ReleaseCompletion = MoveTemp(Completion);
	if (!PendingLoads.IsEmpty())
	{
		UE_LOG(LogGGYGOGameFeatureRetention, Log,
			TEXT("[%s] Closing; waiting for %d owned native load batches before releasing the handle."),
			*OwnerLabel, PendingLoads.Num());
	}
	StartNativeReleaseIfReady();
}

void FGGYGOGameFeatureRetention::StartNativeReleaseIfReady()
{
	if (!bClosing || !PendingLoads.IsEmpty() || bNativeReleaseStarted || CompletedRelease.IsSet())
	{
		return;
	}
	const TSharedRef<FGGYGOGameFeatureRetention, ESPMode::ThreadSafe> KeepAlive = AsShared();
	if (!CheckNativeExecution(ReleaseError) || !StateHandle.IsValid()
		|| !GetAvailableGameFeaturesSubsystem(ReleaseError))
	{
		if (ReleaseError.IsEmpty())
		{
			ReleaseError = TEXT("Native handle is invalid; reference cleanup cannot be confirmed.");
		}
		CompletedRelease = FReleaseResult{EReleaseStatus::Unconfirmed, ReleaseError};
		UE_LOG(LogGGYGOGameFeatureRetention, Error, TEXT("[%s] %s"), *OwnerLabel, *ReleaseError);
		FReleaseCompletion Notify = MoveTemp(ReleaseCompletion);
		const FReleaseResult Result = CompletedRelease.GetValue();
		if (Notify)
		{
			Notify(Result);
		}
		return;
	}

	bNativeReleaseStarted = true;
	StateHandle.ReleaseAndUnregister([KeepAlive](bool bSucceeded)
	{
		const bool bArrivedOnGameThread = IsInGameThread();
		if (bArrivedOnGameThread)
		{
			KeepAlive->OnNativeReleaseCompleted(bSucceeded, true);
		}
		else
		{
			// Marshal only this native completion; no project scheduling loop or UObject is involved.
			AsyncTask(ENamedThreads::GameThread, [KeepAlive, bSucceeded]()
			{
				KeepAlive->OnNativeReleaseCompleted(bSucceeded, false);
			});
		}
	});
	// A synchronous Reset callback precedes Unregister/Invalidate in the native implementation.
	bNativeReleaseCallReturned = true;
	FString Error;
	if (!CheckNativeExecution(Error) || StateHandle.IsValid())
	{
		bReleasePrerequisitesMaintained = false;
		ReleaseError = Error.IsEmpty()
			? TEXT("Native release returned without invalidating the handle; cleanup is unconfirmed.")
			: MoveTemp(Error);
	}
	FinishReleaseIfReady();
}

void FGGYGOGameFeatureRetention::OnNativeReleaseCompleted(bool bSucceeded, bool bArrivedOnGameThread)
{
	if (NativeReleaseSucceeded.IsSet())
	{
		UE_LOG(LogGGYGOGameFeatureRetention, Error, TEXT("[%s] Duplicate native release completion ignored."), *OwnerLabel);
		return;
	}
	NativeReleaseSucceeded = bSucceeded;
	FString Error;
	if (!bArrivedOnGameThread || !CheckNativeExecution(Error))
	{
		bReleasePrerequisitesMaintained = false;
		ReleaseError = !bArrivedOnGameThread
			? TEXT("Native release completion arrived off the game thread; cleanup is unconfirmed.")
			: MoveTemp(Error);
	}
	FinishReleaseIfReady();
}

void FGGYGOGameFeatureRetention::FinishReleaseIfReady()
{
	if (!bNativeReleaseCallReturned || !NativeReleaseSucceeded.IsSet() || CompletedRelease.IsSet())
	{
		return;
	}

	FString Error;
	if (!CheckNativeExecution(Error) || StateHandle.IsValid())
	{
		bReleasePrerequisitesMaintained = false;
		ReleaseError = Error.IsEmpty()
			? TEXT("Native handle remains valid after release; cleanup is unconfirmed.")
			: MoveTemp(Error);
	}
	if (!bReleasePrerequisitesMaintained)
	{
		CompletedRelease = FReleaseResult{EReleaseStatus::Unconfirmed, ReleaseError};
	}
	else if (!NativeReleaseSucceeded.GetValue())
	{
		CompletedRelease = FReleaseResult{EReleaseStatus::Failed,
			TEXT("Native release reported failure; the invalidated handle will not be retried.")};
	}
	else
	{
		CompletedRelease = FReleaseResult{EReleaseStatus::Released, FString()};
	}
	const FReleaseResult Result = CompletedRelease.GetValue();
	if (Result.Status != EReleaseStatus::Released)
	{
		UE_LOG(LogGGYGOGameFeatureRetention, Error, TEXT("[%s] %s"), *OwnerLabel, *Result.Error);
	}
	FReleaseCompletion Notify = MoveTemp(ReleaseCompletion);
	if (Notify)
	{
		Notify(Result);
	}
}

FGGYGOGameFeatureRetention::~FGGYGOGameFeatureRetention()
{
	if (!StateHandle.IsValid())
	{
		return;
	}

	// Invalidate the member by moving it, so its own native destructor cannot start cleanup twice.
	FGameFeatureStateHandle DetachedHandle(MoveTemp(StateHandle));
	auto Cleanup = [Handle = MoveTemp(DetachedHandle), Label = OwnerLabel]() mutable
	{
		FString Error;
		const bool bExecutionWasAllowed = CheckNativeExecution(Error);
		if (!bExecutionWasAllowed)
		{
			UE_LOG(LogGGYGOGameFeatureRetention, Error, TEXT("[%s] Destructor cleanup unconfirmed: %s"), *Label, *Error);
		}
		Handle.ReleaseAndUnregister([Label, bExecutionWasAllowed](bool bSucceeded)
		{
			FString CompletionError;
			if (!bSucceeded || !bExecutionWasAllowed || !CheckNativeExecution(CompletionError))
			{
				UE_LOG(LogGGYGOGameFeatureRetention, Error,
					TEXT("[%s] Detached native cleanup failed or was skipped; cleanup remains unconfirmed. %s"),
					*Label, *CompletionError);
			}
		});
		if (Handle.IsValid())
		{
			UE_LOG(LogGGYGOGameFeatureRetention, Error,
				TEXT("[%s] Detached handle remained valid; engine-exit cleanup is unconfirmed."), *Label);
		}
	};
	if (IsInGameThread())
	{
		Cleanup();
	}
	else if (!IsEngineExitRequested())
	{
		// Transfer the detached handle to Core's existing game-thread task queue, never this object.
		UE_LOG(LogGGYGOGameFeatureRetention, Warning,
			TEXT("[%s] Last reference destroyed off game thread; detached cleanup transferred to game thread."), *OwnerLabel);
		AsyncTask(ENamedThreads::GameThread, MoveTemp(Cleanup));
	}
	else
	{
		UE_LOG(LogGGYGOGameFeatureRetention, Error,
			TEXT("[%s] Last reference destroyed during engine exit; native cleanup is unconfirmed."), *OwnerLabel);
		// The detached handle destructor follows the native exit guard. No raw API fallback is used.
	}
}
