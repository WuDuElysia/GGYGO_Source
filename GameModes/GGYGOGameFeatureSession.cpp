#include "GameModes/GGYGOGameFeatureSession.h"

#include "Async/Async.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFeaturePluginOperationResult.h"
#include "GameFeatureStateHandleReferenceController.h"
#include "GameFeaturesSubsystem.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOGameFeatureSession, Log, All);

namespace
{
	UGameFeaturesSubsystem* GetNativeSubsystem(FString& OutError)
	{
		if (!IsInGameThread() || IsEngineExitRequested())
		{
			OutError = TEXT("Native Active operations require GT before engine exit.");
			return nullptr;
		}
		if (!FGameFeatureStateHandleReferenceController::IsLifetimeControlEnabled())
		{
			OutError = TEXT("GameFeatureStateHandle.Enabled is disabled; native ownership cannot be confirmed.");
			return nullptr;
		}
		UGameFeaturesSubsystem* Subsystem = IsValid(GEngine) ? GEngine->GetEngineSubsystem<UGameFeaturesSubsystem>() : nullptr;
		if (!IsValid(Subsystem))
		{
			OutError = TEXT("Current engine/GameFeatures subsystem is unavailable.");
			return nullptr;
		}
		return Subsystem;
	}

	// Orphaned final-resource cleanup only: ownership is moved, never copied or tied to a World.
	struct FDetachedActiveCleanup : TSharedFromThis<FDetachedActiveCleanup, ESPMode::ThreadSafe>
	{
		FGameFeatureStateHandle Handle;
		UGGYGOGameFeatureSubsystem::FLoadedLease Lease;
		FString Label;
		TSharedPtr<FDetachedActiveCleanup, ESPMode::ThreadSafe> KeepAlive;
		TOptional<bool> Succeeded;
		bool bCallReturned = false;
		bool bGuardsMaintained = true;
		bool bUnconfirmedInvalidHandle;

		FDetachedActiveCleanup(FGameFeatureStateHandle InHandle,
			UGGYGOGameFeatureSubsystem::FLoadedLease InLease, FString InLabel, bool bUnconfirmed)
			: Handle(MoveTemp(InHandle)), Lease(MoveTemp(InLease)), Label(MoveTemp(InLabel)),
			  bUnconfirmedInvalidHandle(bUnconfirmed)
		{
		}

		void RetainUnconfirmed(const FString& Reason)
		{
			UE_LOG(LogGGYGOGameFeatureSession, Error,
				TEXT("[%s] Detached Active cleanup unconfirmed; original resource/Loaded lease retained. %s"), *Label, *Reason);
		}

		void Finish()
		{
			if (!bCallReturned || !Succeeded.IsSet()) { return; }
			FString Error;
			if (!bGuardsMaintained || Handle.IsValid() || !GetNativeSubsystem(Error))
			{
				RetainUnconfirmed(Error.IsEmpty() ? TEXT("Native release guards or handle invalidation were not maintained.") : Error);
				return;
			}
			if (!Succeeded.GetValue())
			{
				UE_LOG(LogGGYGOGameFeatureSession, Error, TEXT("[%s] Detached native Active downgrade failed; cleanup is not successful."), *Label);
			}
			// Native removed our references before downgrade completion; false still reports failure.
			Lease.Reset();
			KeepAlive.Reset();
		}

		void OnReleased(bool bSucceeded, bool bArrivedOnGT)
		{
			if (Succeeded.IsSet()) { return; }
			Succeeded = bSucceeded;
			FString Error;
			bGuardsMaintained &= bArrivedOnGT && GetNativeSubsystem(Error) != nullptr;
			Finish();
		}

		void Begin()
		{
			FString Error;
			if (!Handle.IsValid())
			{
				if (bUnconfirmedInvalidHandle)
				{
					RetainUnconfirmed(TEXT("Invalid GUID does not prove cleanup after a lost native lifetime guard."));
					return;
				}
				Lease.Reset();
				KeepAlive.Reset();
				return;
			}
			if (!GetNativeSubsystem(Error)) { RetainUnconfirmed(Error); return; }
			const auto Resource = AsShared();
			Handle.ReleaseAndUnregister([Resource](bool bSucceeded)
			{
				if (IsInGameThread()) { Resource->OnReleased(bSucceeded, true); }
				else
				{
					AsyncTask(ENamedThreads::GameThread, [Resource, bSucceeded]() { Resource->OnReleased(bSucceeded, false); });
				}
			});
			bCallReturned = true;
			bGuardsMaintained &= GetNativeSubsystem(Error) != nullptr && !Handle.IsValid();
			if (!Succeeded.IsSet())
			{
				UE_LOG(LogGGYGOGameFeatureSession, Log, TEXT("[%s] Detached release pending; original Loaded lease remains held."), *Label);
			}
			Finish();
		}
	};
}

FGGYGOGameFeatureSession::FGGYGOGameFeatureSession(const UWorld& World, UGameInstance& GI, FString Label)
	: OwnerLabel(MoveTemp(Label)), OriginalWorld(&World), OriginalGameInstance(&GI)
{
}

TSharedPtr<FGGYGOGameFeatureSession, ESPMode::ThreadSafe> FGGYGOGameFeatureSession::TryCreate(
	const UWorld& World, const FString& Label, FString& OutError)
{
	OutError.Reset();
	if (!GetNativeSubsystem(OutError))
	{
		OutError = FString::Printf(TEXT("[GameFeature Active owner %s] %s"), *Label, *OutError);
		return nullptr;
	}
	if (Label.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("[GameFeature Active] Owner label must not be empty.");
		return nullptr;
	}
	if (!IsValid(&World))
	{
		OutError = FString::Printf(TEXT("[GameFeature Active owner %s] Request World is invalid."), *Label);
		return nullptr;
	}
	UGameInstance* GI = World.GetGameInstance();
	if (World.bIsTearingDown || World.IsBeingCleanedUp()
		|| (World.WorldType != EWorldType::Game && World.WorldType != EWorldType::PIE)
		|| !IsValid(GI) || GI->GetWorld() != &World)
	{
		OutError = FString::Printf(TEXT("[GameFeature Active owner %s] World [%s] must be its GI's current live Game/PIE World."),
			*Label, *GetPathNameSafe(&World));
		return nullptr;
	}
	const FString ResourceLabel = FString::Printf(TEXT("GameFeature Active [%s] World [%s] GI [%s]"),
		*Label, *World.GetPathName(), *GI->GetPathName());
	return MakeShareable(new FGGYGOGameFeatureSession(World, *GI, ResourceLabel));
}

const UWorld* FGGYGOGameFeatureSession::GetOriginalWorld(FString& OutError) const
{
	if (!IsInGameThread()) { OutError = TEXT("Original World access requires GT."); return nullptr; }
	const UWorld* World = OriginalWorld.Get();
	UGameInstance* GI = OriginalGameInstance.Get();
	if (!IsValid(World) || !IsValid(GI) || World->bIsTearingDown || World->IsBeingCleanedUp()
		|| World->GetGameInstance() != GI || GI->GetWorld() != World)
	{
		OutError = TEXT("Original World/GI is invalid, tearing down, or no longer current.");
		return nullptr;
	}
	return World;
}

UGameFeaturesSubsystem* FGGYGOGameFeatureSession::CheckActiveAdmission(FString& OutError) const
{
	if (bClosing) { OutError = TEXT("This Active resource has closed admission."); return nullptr; }
	const UWorld* World = GetOriginalWorld(OutError);
	if (!World) { return nullptr; }
	if (!LoadedLease.IsValid() || !LoadedLease->IsAdmissionOpenFor(*World))
	{
		OutError = TEXT("Original Loaded lease admission is unavailable/closed.");
		return nullptr;
	}
	if (!ActiveHandle.IsValid()) { OutError = TEXT("Own native Active handle is invalid."); return nullptr; }
	return GetNativeSubsystem(OutError);
}

bool FGGYGOGameFeatureSession::ValidateSnapshots(FString& OutError) const
{
	if (!LoadedLease.IsValid() || LoadedLease->GetRootPluginURLs().IsEmpty()
		|| LoadedLease->GetActivationPluginURLs().IsEmpty())
	{
		OutError = TEXT("Loaded lease must contain nonempty roots and activation request URLs.");
		return false;
	}
	TSet<FString> ActivationURLs;
	for (const FString& URL : LoadedLease->GetActivationPluginURLs())
	{
		if (URL.IsEmpty() || ActivationURLs.Contains(URL))
		{
			OutError = FString::Printf(TEXT("Invalid/duplicate activation request URL [%s]."), *URL);
			return false;
		}
		ActivationURLs.Add(URL);
	}
	TSet<FString> RootURLs;
	for (const FString& URL : LoadedLease->GetRootPluginURLs())
	{
		if (URL.IsEmpty() || RootURLs.Contains(URL) || !ActivationURLs.Contains(URL))
		{
			OutError = FString::Printf(TEXT("Invalid/duplicate root URL [%s] or root missing from activation request set."), *URL);
			return false;
		}
		RootURLs.Add(URL);
	}
	return true;
}

void FGGYGOGameFeatureSession::RecordStartFailure(EStartStatus Status, FString Error)
{
	if (!StartFailure.IsSet() || Status == EStartStatus::Failed) { StartFailure = Status; }
	Error = FString::Printf(TEXT("[%s] %s"), *OwnerLabel, *Error);
	StartResult.Errors.Add(Error);
	UE_LOG(LogGGYGOGameFeatureSession, Error, TEXT("%s"), *Error);
}

void FGGYGOGameFeatureSession::Start(FGGYGOGameFeatureClosureResolver::FInput Input, FStartCompletion Completion)
{
	const auto Resource = AsShared();
	if (!IsInGameThread() || bStarted || bClosing || !Completion)
	{
		const FStartResult Rejected{EStartStatus::Rejected, {},
			{FString::Printf(TEXT("[%s] Start requires GT, an unused open session and a completion receiver."), *OwnerLabel)}};
		UE_LOG(LogGGYGOGameFeatureSession, Error, TEXT("%s"), *Rejected.Errors[0]);
		if (Completion) { Completion(Rejected); }
		return;
	}
	bStarted = true;
	WorkKeepAlive = Resource;
	StartCompletion = MoveTemp(Completion);
	FString Error;
	const UWorld* World = GetOriginalWorld(Error);
	if (!World || Input.RootPluginNames.IsEmpty())
	{
		RecordStartFailure(EStartStatus::Rejected, World
			? FString(TEXT("Start requires nonempty explicit roots; successful no-GF configuration belongs to the caller.")) : MoveTemp(Error));
		FinishStartIfReady();
		return;
	}
	UGGYGOGameFeatureSubsystem* Host = OriginalGameInstance.Get()->GetSubsystem<UGGYGOGameFeatureSubsystem>();
	if (!IsValid(Host))
	{
		RecordStartFailure(EStartStatus::Rejected, TEXT("Original GI GameFeature subsystem is unavailable."));
		FinishStartIfReady();
		return;
	}
	bPreparingLoaded = true;
	bPrepareCallReturned = false;
	Host->PrepareManagedLoadedClosure(*World, MoveTemp(Input),
		[Resource](FLoadedLease Lease, const FGGYGOGameFeatureRetention::FLoadResult& Result)
	{
		if (IsInGameThread()) { Resource->OnLoaded(MoveTemp(Lease), Result, true); }
		else
		{
			AsyncTask(ENamedThreads::GameThread, [Resource, Lease = MoveTemp(Lease), Result]() mutable
			{
				Resource->OnLoaded(MoveTemp(Lease), Result, false);
			});
		}
	});
	bPrepareCallReturned = true;
	FinishStartIfReady();
}

void FGGYGOGameFeatureSession::OnLoaded(FLoadedLease Lease,
	const FGGYGOGameFeatureRetention::FLoadResult& Result, bool bArrivedOnGT)
{
	if (!bPreparingLoaded)
	{
		UE_LOG(LogGGYGOGameFeatureSession, Error, TEXT("[%s] Duplicate GI preparation completion ignored."), *OwnerLabel);
		return;
	}
	bPreparingLoaded = false;
	LoadedLease = MoveTemp(Lease);
	using ELoadStatus = FGGYGOGameFeatureRetention::ELoadStatus;
	if (Result.Status != ELoadStatus::Loaded)
	{
		const EStartStatus Status = Result.Status == ELoadStatus::Failed ? EStartStatus::Failed
			: Result.Status == ELoadStatus::Rejected ? EStartStatus::Rejected : EStartStatus::Interrupted;
		if (Result.Errors.IsEmpty()) { RecordStartFailure(Status, TEXT("GI preparation ended without Loaded or a diagnostic.")); }
		for (const FString& Error : Result.Errors) { RecordStartFailure(Status, Error); }
	}
	else if (!LoadedLease.IsValid()) { RecordStartFailure(EStartStatus::Failed, TEXT("GI reported Loaded without its original lease.")); }
	if (!bArrivedOnGT) { RecordStartFailure(EStartStatus::Interrupted, TEXT("GI completion arrived off GT; native Active admission is refused.")); }
	if (!StartFailure.IsSet())
	{
		bActivationDispatchReturned = false;
		BeginActivations();
		bActivationDispatchReturned = true;
	}
	FinishStartIfReady();
}

void FGGYGOGameFeatureSession::BeginActivations()
{
	FString Error;
	const UWorld* World = GetOriginalWorld(Error);
	if (bClosing || !World || !LoadedLease->IsAdmissionOpenFor(*World) || !GetNativeSubsystem(Error))
	{
		RecordStartFailure(EStartStatus::Interrupted, Error.IsEmpty() ? TEXT("Original Active admission closed before initialization.") : MoveTemp(Error));
		return;
	}
	if (!ValidateSnapshots(Error)) { RecordStartFailure(EStartStatus::Failed, MoveTemp(Error)); return; }
	for (const FString& URL : LoadedLease->GetRootPluginURLs()) { StartResult.RootPluginURLs.Add(URL); }
	const bool bInitialized = ActiveHandle.InitAndRegister(OwnerLabel, EGameFeatureStateHandleOptions::None);
	if (!bInitialized || !ActiveHandle.IsValid())
	{
		RecordStartFailure(EStartStatus::Failed, TEXT("Own native Active handle did not initialize to a valid GUID."));
		return;
	}
	// Flat requests cover all true-reachable dependencies before a root can activate them.
	for (const FString& URL : LoadedLease->GetActivationPluginURLs())
	{
		if (!CheckActiveAdmission(Error)) { RecordStartFailure(EStartStatus::Interrupted, MoveTemp(Error)); return; }
		FGameFeatureStateHandleReferenceController::Get().AddOrUpdateReference(
			ActiveHandle, URL, EGameFeaturePluginState::Active, false);
		if (!CheckActiveAdmission(Error)) { RecordStartFailure(EStartStatus::Interrupted, MoveTemp(Error)); return; }
	}
	const auto Resource = AsShared();
	for (const FString& URL : StartResult.RootPluginURLs)
	{
		if (StartFailure.IsSet()) { break; }
		if (!CheckActiveAdmission(Error)) { RecordStartFailure(EStartStatus::Interrupted, MoveTemp(Error)); break; }
		PendingRoots.Add(URL);
		FGameFeatureStateHandleReferenceController::Get().LoadAndActivateGameFeaturePlugin(
			ActiveHandle, URL, FGameFeatureProtocolOptions(),
			UGameFeatureStateHandleLoadComplete::CreateLambda([Resource, URL](const UE::GameFeatures::FResult& Result)
		{
			const bool bSucceeded = Result.HasValue() && !Result.HasError();
			const FString ErrorText = bSucceeded ? FString() : Result.HasError()
				? Result.GetError() + TEXT(" ") + Result.OptionalErrorText.ToString()
				: FString(TEXT("Native activation completion has no successful value."));
			if (IsInGameThread()) { Resource->OnRootActivated(URL, bSucceeded, ErrorText, true); }
			else
			{
				AsyncTask(ENamedThreads::GameThread, [Resource, URL, bSucceeded, ErrorText]()
				{
					Resource->OnRootActivated(URL, bSucceeded, ErrorText, false);
				});
			}
		}));
		if (!CheckActiveAdmission(Error)) { RecordStartFailure(EStartStatus::Interrupted, MoveTemp(Error)); break; }
	}
}

void FGGYGOGameFeatureSession::OnRootActivated(const FString& URL, bool bSucceeded, FString NativeError, bool bArrivedOnGT)
{
	if (PendingRoots.Remove(URL) == 0)
	{
		UE_LOG(LogGGYGOGameFeatureSession, Error, TEXT("[%s] Duplicate/unissued root completion [%s] ignored."), *OwnerLabel, *URL);
		return;
	}
	if (!bSucceeded) { RecordStartFailure(EStartStatus::Failed, FString::Printf(TEXT("Root [%s] activation failed: %s"), *URL, *NativeError)); }
	if (!bArrivedOnGT) { RecordStartFailure(EStartStatus::Interrupted, FString::Printf(TEXT("Root [%s] completion arrived off GT."), *URL)); }
	FString Error;
	if (!CheckActiveAdmission(Error)) { RecordStartFailure(EStartStatus::Interrupted, MoveTemp(Error)); }
	FinishStartIfReady();
}

void FGGYGOGameFeatureSession::FinishStartIfReady()
{
	if (!bStarted || bStartPublished || bPreparingLoaded || !bPrepareCallReturned
		|| !bActivationDispatchReturned || !PendingRoots.IsEmpty()) { return; }
	const auto Resource = AsShared();
	FString Error;
	if (!StartFailure.IsSet())
	{
		UGameFeaturesSubsystem* Subsystem = CheckActiveAdmission(Error);
		if (!Subsystem) { RecordStartFailure(EStartStatus::Interrupted, MoveTemp(Error)); }
		else
		{
			for (const FString& URL : LoadedLease->GetActivationPluginURLs())
			{
				const EGameFeaturePluginState State = Subsystem->GetPluginState(URL);
				if (State != EGameFeaturePluginState::Active)
				{
					RecordStartFailure(EStartStatus::Failed, FString::Printf(TEXT("Activation URL [%s] is [%s] after root completion."),
						*URL, *UE::GameFeatures::ToString(State)));
					break;
				}
			}
		}
	}
	const UWorld* World = GetOriginalWorld(Error);
	const bool bOriginalAdmission = World && (!LoadedLease.IsValid() || LoadedLease->IsAdmissionOpenFor(*World));
	if (!bOriginalAdmission && !StartFailure.IsSet())
	{
		RecordStartFailure(EStartStatus::Interrupted,
			Error.IsEmpty() ? FString(TEXT("Original lease admission closed before publication.")) : MoveTemp(Error));
	}
	StartResult.Status = StartFailure.IsSet() ? StartFailure.GetValue() : EStartStatus::Ready;
	const bool bNotify = !bClosing && bOriginalAdmission;
	bStartPublished = true;
	FStartCompletion Notify = MoveTemp(StartCompletion);
	if (StartResult.Status != EStartStatus::Ready) { bClosing = true; }
	if (bNotify && Notify) { Notify(StartResult); }
	if (bClosing) { StartReleaseIfReady(); }
	else { WorkKeepAlive.Reset(); }
}

bool FGGYGOGameFeatureSession::IsReadyFor(const UWorld& World) const
{
	if (!IsInGameThread() || !bStartPublished || StartResult.Status != EStartStatus::Ready
		|| OriginalWorld.Get() != &World) { return false; }
	FString Error;
	UGameFeaturesSubsystem* Subsystem = CheckActiveAdmission(Error);
	if (!Subsystem) { return false; }
	for (const FString& URL : LoadedLease->GetActivationPluginURLs())
	{
		if (Subsystem->GetPluginState(URL) != EGameFeaturePluginState::Active) { return false; }
	}
	return true;
}

void FGGYGOGameFeatureSession::Close(FCloseCompletion Completion)
{
	const auto Resource = AsShared();
	if (!IsInGameThread())
	{
		const FCloseResult Result{ECloseStatus::RejectedWrongThread, TEXT("Close requires GT; resource remains untouched.")};
		UE_LOG(LogGGYGOGameFeatureSession, Error, TEXT("[%s] %s"), *OwnerLabel, *Result.Error);
		if (Completion) { Completion(Result); }
		return;
	}
	if (CompletedClose.IsSet()) { if (Completion) { Completion(CompletedClose.GetValue()); } return; }
	if (bCloseRequested)
	{
		if (Completion) { Completion({ECloseStatus::Pending, TEXT("Original work/release is pending; this duplicate is not retained.")}); }
		return;
	}
	bCloseRequested = true;
	bClosing = true;
	WorkKeepAlive = Resource;
	StartCompletion = {};
	CloseCompletion = MoveTemp(Completion);
	if (bStarted && !bStartPublished) { RecordStartFailure(EStartStatus::Interrupted, TEXT("Session explicitly closed; start observer withdrawn.")); }
	if (bPreparingLoaded || !bPrepareCallReturned || !bActivationDispatchReturned || !PendingRoots.IsEmpty())
	{
		UE_LOG(LogGGYGOGameFeatureSession, Log, TEXT("[%s] Closing; actual preparation/root callbacks and native call return must drain."), *OwnerLabel);
	}
	FinishStartIfReady();
	StartReleaseIfReady();
}

void FGGYGOGameFeatureSession::StartReleaseIfReady()
{
	if (!bClosing || CompletedClose.IsSet() || bReleaseStarted || bPreparingLoaded
		|| !bPrepareCallReturned || !bActivationDispatchReturned || !PendingRoots.IsEmpty()) { return; }
	const auto Resource = AsShared();
	if (!ActiveHandle.IsValid()) { CompleteClose({ECloseStatus::Closed, {}}, true); return; }
	if (!GetNativeSubsystem(ReleaseError)) { CompleteClose({ECloseStatus::Unconfirmed, ReleaseError}, false); return; }
	bReleaseStarted = true;
	ActiveHandle.ReleaseAndUnregister([Resource](bool bSucceeded)
	{
		if (IsInGameThread()) { Resource->OnReleased(bSucceeded, true); }
		else
		{
			AsyncTask(ENamedThreads::GameThread, [Resource, bSucceeded]() { Resource->OnReleased(bSucceeded, false); });
		}
	});
	bReleaseCallReturned = true;
	if (!NativeReleaseSucceeded.IsSet())
	{
		UE_LOG(LogGGYGOGameFeatureSession, Log, TEXT("[%s] Native Active release pending; original Loaded lease remains held."), *OwnerLabel);
	}
	if (!GetNativeSubsystem(ReleaseError) || ActiveHandle.IsValid())
	{
		bReleaseGuardsMaintained = false;
		if (ReleaseError.IsEmpty()) { ReleaseError = TEXT("Native release returned without invalidating our handle."); }
	}
	FinishReleaseIfReady();
}

void FGGYGOGameFeatureSession::OnReleased(bool bSucceeded, bool bArrivedOnGT)
{
	if (NativeReleaseSucceeded.IsSet()) { return; }
	NativeReleaseSucceeded = bSucceeded;
	if (!bArrivedOnGT || !GetNativeSubsystem(ReleaseError))
	{
		bReleaseGuardsMaintained = false;
		if (!bArrivedOnGT) { ReleaseError = TEXT("Native Active release completion arrived off GT; cleanup unconfirmed."); }
	}
	FinishReleaseIfReady();
}

void FGGYGOGameFeatureSession::FinishReleaseIfReady()
{
	if (!bReleaseCallReturned || !NativeReleaseSucceeded.IsSet() || CompletedClose.IsSet()) { return; }
	if (!GetNativeSubsystem(ReleaseError) || ActiveHandle.IsValid()) { bReleaseGuardsMaintained = false; }
	if (!bReleaseGuardsMaintained)
	{
		CompleteClose({ECloseStatus::Unconfirmed, ReleaseError.IsEmpty()
			? FString(TEXT("Native release guards/invalidation were not maintained.")) : ReleaseError}, false);
	}
	else if (!NativeReleaseSucceeded.GetValue())
	{
		CompleteClose({ECloseStatus::Failed, TEXT("Native Active downgrade failed after withdrawal; cleanup is not successful.")}, true);
	}
	else { CompleteClose({ECloseStatus::Closed, {}}, true); }
}

void FGGYGOGameFeatureSession::CompleteClose(FCloseResult Result, bool bReturnLease)
{
	const auto Resource = AsShared();
	CompletedClose = MoveTemp(Result);
	// Retiring a final GI lease can reenter native/user code; publish our terminal record first.
	if (bReturnLease) { LoadedLease.Reset(); }
	if (CompletedClose->Status != ECloseStatus::Closed)
	{
		UE_LOG(LogGGYGOGameFeatureSession, Error, TEXT("[%s] %s"), *OwnerLabel, *CompletedClose->Error);
	}
	FCloseCompletion Notify = MoveTemp(CloseCompletion);
	WorkKeepAlive.Reset();
	if (Notify) { Notify(CompletedClose.GetValue()); }
}

FGGYGOGameFeatureSession::~FGGYGOGameFeatureSession()
{
	if (!ActiveHandle.IsValid() && !LoadedLease.IsValid()) { return; }
	const bool bUnconfirmed = CompletedClose.IsSet() && CompletedClose->Status == ECloseStatus::Unconfirmed;
	auto Cleanup = MakeShared<FDetachedActiveCleanup, ESPMode::ThreadSafe>(
		MoveTemp(ActiveHandle), MoveTemp(LoadedLease), OwnerLabel, bUnconfirmed);
	Cleanup->KeepAlive = Cleanup;
	if (IsInGameThread()) { Cleanup->Begin(); }
	else if (!IsEngineExitRequested())
	{
		UE_LOG(LogGGYGOGameFeatureSession, Warning, TEXT("[%s] Final destruction off GT; original handle/lease transferred to GT cleanup."), *OwnerLabel);
		AsyncTask(ENamedThreads::GameThread, [Cleanup]() { Cleanup->Begin(); });
	}
	else { Cleanup->RetainUnconfirmed(TEXT("Final resource destruction during engine exit.")); }
}
