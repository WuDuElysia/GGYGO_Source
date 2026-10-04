/**
 * @file GGYGOLocalPlayer.cpp
 * @brief 本地玩家实现
 */
#include "Player/GGYGOLocalPlayer.h"

#include "Engine/GameInstance.h"
#include "GameFramework/PlayerController.h"
#include "Input/GGYGOMovementInputOriginResource.h"
#include "Teams/GGYGOSquadPresets.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/Class.h"
#include "UObject/StrongObjectPtr.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOLocalPlayer)

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOLocalPlayerOrigin, Log, All);
DEFINE_LOG_CATEGORY_STATIC(LogGGYGOLocalPlayerSquad, Log, All);

bool UGGYGOLocalPlayer::TryGetSquadPresets(UGGYGOSquadPresets*& OutPresets, FString& OutError) const
{
	OutPresets = nullptr;
	OutError.Reset();
	if (!IsInGameThread() || !IsValid(this) || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		OutError = FString::Printf(TEXT("[Teams][LocalPlayer] LocalPlayer=%p Slot=%s: preset access requires a live host on the game thread."),
			static_cast<const void*>(this), *UGGYGOSquadPresets::SaveSlotName);
		return false;
	}

	const FString HostPath = GetPathName();
	FString SourceGameInstancePath = SquadPresetsSourceGameInstancePath;
	FString SourceObjectPath = SquadPresetsSourceObjectPath;
	FPlatformUserId SourcePlatformUserId = SquadPresetsSourcePlatformUserId;
	int32 SourcePlatformUserIndex = SquadPresetsSourcePlatformUserIndex;
	FString SourceSlotName = SquadPresetsSourceSlotName;
	const auto Reject = [&](const FString& Reason) -> bool
	{
		OutError = FString::Printf(
			TEXT("[Teams][LocalPlayer] LocalPlayer=%s GameInstance=%s PlatformUser=%d UserIndex=%d Slot=%s Presets=%s: %s"),
			*HostPath, *SourceGameInstancePath, SourcePlatformUserId.GetInternalId(),
			SourcePlatformUserIndex, *SourceSlotName, *SourceObjectPath, *Reason);
		return false;
	};
	if (bSquadPresetsAccessBusy)
	{
		return Reject(TEXT("The same host's synchronous preset access is busy; reentrant access is rejected without queuing."));
	}

	// Keep the host alive until its scope guard has restored Busy, including destruction callbacks.
	const TStrongObjectPtr<const UGGYGOLocalPlayer> HostLifetime(this);
	const TGuardValue<bool> AccessScope(bSquadPresetsAccessBusy, true);
	const auto CheckSourceAndObject = [this](const TWeakObjectPtr<const ULocalPlayer>& OriginalPlayer,
		const TWeakObjectPtr<UGameInstance>& OriginalGameInstance, FPlatformUserId OriginalUserId,
		int32 OriginalUserIndex, const FString& OriginalSlotName, UGGYGOSquadPresets* OriginalPresets) -> FString
	{
		const auto CheckHostContext = [&]() -> FString
		{
			UGameInstance* const GameInstance = OriginalGameInstance.Get();
			if (OriginalPlayer.Get() != this || !IsValid(this) || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
				|| !IsValid(GameInstance) || GameInstance->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
			{
				return TEXT("The original local player or game instance became unavailable.");
			}
			if (!OriginalUserId.IsValid() || OriginalUserIndex < 0
				|| OriginalSlotName != UGGYGOSquadPresets::SaveSlotName)
			{
				return TEXT("The original publication has an invalid platform user/index or does not identify the retained save slot.");
			}
			if (GetGameInstance() != GameInstance || GameInstance->GetFirstGamePlayer() != this)
			{
				return TEXT("The host is no longer the original game instance's primary local player.");
			}

			const int32 CurrentUserIndex = GetPlatformUserIndex();
			if (OriginalPlayer.Get() != this || !IsValid(this) || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
				|| OriginalGameInstance.Get() != GameInstance
				|| !IsValid(GameInstance) || GameInstance->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
				|| GetGameInstance() != GameInstance || GameInstance->GetFirstGamePlayer() != this
				|| GetPlatformUserId() != OriginalUserId || CurrentUserIndex != OriginalUserIndex)
			{
				return TEXT("The original primary local player/game instance/platform user/index identity changed.");
			}
			return FString();
		};

		FString Error = CheckHostContext();
		if (!Error.IsEmpty())
		{
			return Error;
		}
		if (!IsValid(OriginalPresets) || OriginalPresets->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
		{
			return TEXT("The original presets object is unavailable or being destroyed.");
		}

		const TWeakObjectPtr<UGGYGOSquadPresets> OriginalObject(OriginalPresets);
		const ULocalPlayer* const AssociatedPlayer = OriginalPresets->GetLocalPlayer();
		const FString AssociatedSlot = OriginalPresets->GetSaveSlotName();
		const FPlatformUserId AssociatedUserId = OriginalPresets->GetPlatformUserId();
		const int32 AssociatedUserIndex = OriginalPresets->GetPlatformUserIndex();
		Error = CheckHostContext();
		if (!Error.IsEmpty())
		{
			return Error;
		}
		if (OriginalObject.Get() != OriginalPresets || !IsValid(OriginalPresets)
			|| OriginalPresets->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
		{
			return TEXT("The original presets object became unavailable during association validation.");
		}
		if (AssociatedPlayer != this || AssociatedSlot != OriginalSlotName
			|| AssociatedUserId != OriginalUserId || AssociatedUserIndex != OriginalUserIndex
			|| OriginalPresets->ULocalPlayerSaveGame::GetLocalPlayer() != this
			|| OriginalPresets->ULocalPlayerSaveGame::GetSaveSlotName() != OriginalSlotName)
		{
			return TEXT("The original presets object's local player/user/slot association no longer matches its publication.");
		}
		return FString();
	};

	UGGYGOSquadPresets* const CachedPresets = SquadPresets.Get();
	if (CachedPresets != nullptr || !SquadPresetsSourceObject.IsExplicitlyNull())
	{
		// A stale weak publication identity still proves that this host had an original cache.
		if (!IsValid(CachedPresets) || CachedPresets->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			|| SquadPresetsSourceObject.Get() != CachedPresets)
		{
			return Reject(TEXT("The original cache is invalid, lost or mismatched; it is retained and no replacement load is attempted."));
		}
		const TStrongObjectPtr<UGGYGOSquadPresets> CachedLifetime(CachedPresets);
		const FString Error = CheckSourceAndObject(SquadPresetsSourceLocalPlayer, SquadPresetsSourceGameInstance,
			SquadPresetsSourcePlatformUserId, SquadPresetsSourcePlatformUserIndex, SquadPresetsSourceSlotName, CachedPresets);
		if (!Error.IsEmpty())
		{
			return Reject(Error);
		}
		if (SquadPresets.Get() != CachedPresets || SquadPresetsSourceObject.Get() != CachedPresets)
		{
			return Reject(TEXT("The original cache identity changed during validation; no new source was adopted."));
		}
		OutPresets = CachedPresets;
		return true;
	}

	FGGYGOSquadPresetLoadResult LoadResult = UGGYGOSquadPresets::TryLoadForLocalPlayer(this);
	SourceGameInstancePath = LoadResult.GameInstancePath;
	SourceObjectPath = LoadResult.PresetsPath;
	SourcePlatformUserId = LoadResult.OriginalPlatformUserId;
	SourcePlatformUserIndex = LoadResult.OriginalPlatformUserIndex;
	SourceSlotName = LoadResult.SlotName;
	if (LoadResult.Status == EGGYGOSquadPresetLoadStatus::Invalid)
	{
		if (LoadResult.Error.IsEmpty())
		{
			return Reject(TEXT("The explicit loader returned Invalid without a diagnostic; no candidate was published."));
		}
		OutError = LoadResult.Error;
		return false;
	}
	if (LoadResult.Status != EGGYGOSquadPresetLoadStatus::Loaded
		&& LoadResult.Status != EGGYGOSquadPresetLoadStatus::CreatedForMissingSlot)
	{
		return Reject(TEXT("The explicit loader returned an unsupported result; no candidate was published."));
	}

	UGGYGOSquadPresets* const Candidate = LoadResult.Candidate.Get();
	const FString Error = CheckSourceAndObject(LoadResult.OriginalLocalPlayer, LoadResult.OriginalGameInstance,
		LoadResult.OriginalPlatformUserId, LoadResult.OriginalPlatformUserIndex, LoadResult.SlotName, Candidate);
	if (!Error.IsEmpty())
	{
		return Reject(Error);
	}
	if (SquadPresets != nullptr || !SquadPresetsSourceObject.IsExplicitlyNull())
	{
		return Reject(TEXT("The cache gained another publication during loading; the candidate did not replace it."));
	}

	// Publish the source snapshot and sole cache while the actual load result still owns the candidate.
	SquadPresetsSourceLocalPlayer = LoadResult.OriginalLocalPlayer;
	SquadPresetsSourceGameInstance = LoadResult.OriginalGameInstance;
	SquadPresetsSourcePlatformUserId = LoadResult.OriginalPlatformUserId;
	SquadPresetsSourcePlatformUserIndex = LoadResult.OriginalPlatformUserIndex;
	SquadPresetsSourceSlotName = LoadResult.SlotName;
	SquadPresetsSourceGameInstancePath = LoadResult.GameInstancePath;
	SquadPresetsSourceObjectPath = LoadResult.PresetsPath;
	SquadPresetsSourceObject = Candidate;
	SquadPresets = Candidate;
	OutPresets = Candidate;
	return true;
}

UGGYGOSquadPresets* UGGYGOLocalPlayer::GetSquadPresets() const
{
	UGGYGOSquadPresets* Presets = nullptr;
	FString Error;
	if (!TryGetSquadPresets(Presets, Error))
	{
		UE_LOG(LogGGYGOLocalPlayerSquad, Error, TEXT("%s"), *Error);
	}
	return Presets;
}

UGGYGOMovementInputOriginResource* UGGYGOLocalPlayer::GetMovementInputOriginResource() const
{
	return MovementInputOriginResource.Get();
}

void UGGYGOLocalPlayer::ReportMovementInputOriginFailure(FName Reason,
	const UGGYGOMovementInputOriginResource* Resource) const
{
	UE_LOG(LogGGYGOLocalPlayerOrigin, Warning,
		TEXT("[Input.OriginHost] Player=%s Resource=%s ResourceClass=%s Reason=%s"),
		*GetPathNameSafe(this), *GetPathNameSafe(Resource),
		*GetPathNameSafe(UGGYGOMovementInputOriginResource::StaticClass()), *Reason.ToString());
}

UGGYGOMovementInputOriginResource* UGGYGOLocalPlayer::PrepareMovementInputOriginForPlayerAdded()
{
	if (!IsValid(this) || IsTemplate())
	{
		ReportMovementInputOriginFailure(TEXT("InvalidOrTemplatePlayerAdded"), MovementInputOriginResource.Get());
		return nullptr;
	}
	if (MovementInputOriginResource == nullptr)
	{
		// This helper is called only at a real PlayerAdded boundary, never by the getter.
		MovementInputOriginResource = NewObject<UGGYGOMovementInputOriginResource>(this);
	}
	UGGYGOMovementInputOriginResource* const Resource = MovementInputOriginResource.Get();
	if (!IsValid(Resource) || Resource->IsTemplate() || Resource->GetOuter() != this)
	{
		ReportMovementInputOriginFailure(TEXT("InvalidOriginalResourceAtPlayerAdded"), Resource);
		return nullptr;
	}
	return Resource;
}

void UGGYGOLocalPlayer::FinishMovementInputOriginPlayerAdded(
	const TWeakObjectPtr<UGGYGOMovementInputOriginResource>& OriginalResource,
	TWeakObjectPtr<APlayerController> BeforeSuper)
{
	if (OriginalResource.IsExplicitlyNull())
	{
		// Preparation already diagnosed the failure; do not adopt a callback's resource.
		return;
	}
	UGGYGOMovementInputOriginResource* const Resource = OriginalResource.Get();
	if (!Resource)
	{
		ReportMovementInputOriginFailure(TEXT("OriginalResourceLostDuringPlayerAdded"), nullptr);
		return;
	}
	FString Error;
	if (!Resource->InitializeFromPlayerAdded(this, BeforeSuper, Error))
	{
		// The resource diagnoses and retires the failed attempt; there is no host retry.
		return;
	}
}

void UGGYGOLocalPlayer::PlayerAdded(UGameViewportClient* InViewportClient, int32 InControllerID)
{
	const TWeakObjectPtr<APlayerController> BeforeSuper(PlayerController.Get());
	const TWeakObjectPtr<UGGYGOMovementInputOriginResource> OriginalResource(PrepareMovementInputOriginForPlayerAdded());
	Super::PlayerAdded(InViewportClient, InControllerID);
	FinishMovementInputOriginPlayerAdded(OriginalResource, BeforeSuper);
}

void UGGYGOLocalPlayer::PlayerAdded(UGameViewportClient* InViewportClient, FPlatformUserId InUserId)
{
	const TWeakObjectPtr<APlayerController> BeforeSuper(PlayerController.Get());
	const TWeakObjectPtr<UGGYGOMovementInputOriginResource> OriginalResource(PrepareMovementInputOriginForPlayerAdded());
	Super::PlayerAdded(InViewportClient, InUserId);
	FinishMovementInputOriginPlayerAdded(OriginalResource, BeforeSuper);
}

void UGGYGOLocalPlayer::PlayerRemoved()
{
	UGGYGOMovementInputOriginResource* const OriginalResource = MovementInputOriginResource.Get();
	if (IsValid(OriginalResource))
	{
		OriginalResource->NotifyPlayerRemoved(this);
	}
	Super::PlayerRemoved();
}

void UGGYGOLocalPlayer::ReceivedPlayerController(APlayerController* NewController)
{
	UGGYGOMovementInputOriginResource* const OriginalResource = MovementInputOriginResource.Get();
	if (IsValid(OriginalResource))
	{
		OriginalResource->NotifyControllerChanged(NewController);
	}
	else
	{
		ReportMovementInputOriginFailure(TEXT("MissingOriginalResourceAtControllerNotification"), OriginalResource);
	}
	// Native Super broadcasts to external listeners and subsystems after this boundary.
	Super::ReceivedPlayerController(NewController);
}

bool UGGYGOLocalPlayer::SpawnPlayActor(const FString& URL, FString& OutError, UWorld* InWorld)
{
	const TWeakObjectPtr<UGGYGOMovementInputOriginResource> OriginalResource(MovementInputOriginResource.Get());
	const bool bSpawned = Super::SpawnPlayActor(URL, OutError, InWorld);
	if (UGGYGOMovementInputOriginResource* Resource = OriginalResource.Get())
	{
		// The client dummy path assigns directly and does not call ReceivedPlayerController.
		Resource->NotifyControllerChanged(PlayerController.Get());
	}
	else
	{
		ReportMovementInputOriginFailure(TEXT("MissingOriginalResourceAtSpawnReturn"), nullptr);
	}
	return bSpawned;
}

void UGGYGOLocalPlayer::BeginDestroy()
{
	UGGYGOMovementInputOriginResource* const OriginalResource = MovementInputOriginResource.Get();
	if (IsValid(OriginalResource))
	{
		// The host is terminal even when native PlayerRemoved was not called first.
		OriginalResource->NotifyPlayerRemoved(this);
	}
	Super::BeginDestroy();
}
