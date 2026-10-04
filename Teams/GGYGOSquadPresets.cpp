/**
 * @file GGYGOSquadPresets.cpp
 * @brief 编队预设集合实现
 */
#include "Teams/GGYGOSquadPresets.h"

#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "Character/Data/GGYGOPawnData.h"
#include "Engine/AssetManager.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Kismet/GameplayStatics.h"
#include "PlatformFeatures.h"
#include "SaveGameSystem.h"
#include "Serialization/Archive.h"
#include "Teams/GGYGOSquadTypes.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOSquadPresets)

const FString UGGYGOSquadPresets::SaveSlotName = TEXT("GGYGOSquadPresets");

bool FGGYGOSquadPreset::HasAnyMember() const
{
	for (const FPrimaryAssetId& Member : Members)
	{
		if (Member.IsValid())
		{
			return true;
		}
	}
	return false;
}

FGGYGOSquadPresetLoadResult UGGYGOSquadPresets::TryLoadForLocalPlayer(const ULocalPlayer* LocalPlayer)
{
	FGGYGOSquadPresetLoadResult Result;
	Result.SlotName = SaveSlotName;
	const auto Reject = [&Result](const FString& Reason) -> FGGYGOSquadPresetLoadResult
	{
		Result.Status = EGGYGOSquadPresetLoadStatus::Invalid;
		Result.Candidate.Reset();
		Result.Error = FString::Printf(
			TEXT("[Teams][SquadPresets] LocalPlayer=%s GameInstance=%s PlatformUser=%d UserIndex=%d Slot=%s Presets=%s: %s"),
			*Result.LocalPlayerPath, *Result.GameInstancePath, Result.OriginalPlatformUserId.GetInternalId(),
			Result.OriginalPlatformUserIndex, *Result.SlotName, *Result.PresetsPath, *Reason);
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("%s"), *Result.Error);
		return MoveTemp(Result);
	};

	if (!IsInGameThread())
	{
		return Reject(TEXT("Synchronous preset loading requires the game thread."));
	}
	if (!IsValid(LocalPlayer) || LocalPlayer->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		return Reject(TEXT("The supplied local player is invalid or being destroyed."));
	}
	Result.OriginalLocalPlayer = LocalPlayer;
	Result.LocalPlayerPath = LocalPlayer->GetPathName();

	UGameInstance* const GameInstance = LocalPlayer->GetGameInstance();
	if (!IsValid(GameInstance) || GameInstance->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		return Reject(TEXT("The supplied local player has no live game instance."));
	}
	Result.OriginalGameInstance = GameInstance;
	Result.GameInstancePath = GameInstance->GetPathName();
	if (GameInstance->GetFirstGamePlayer() != LocalPlayer)
	{
		return Reject(TEXT("Only the primary local player may use the retained save slot; secondary players are rejected before storage access."));
	}

	Result.OriginalPlatformUserId = LocalPlayer->GetPlatformUserId();
	Result.OriginalPlatformUserIndex = LocalPlayer->GetPlatformUserIndex();
	if (!Result.OriginalPlatformUserId.IsValid() || Result.OriginalPlatformUserIndex < 0)
	{
		return Reject(TEXT("The original primary local player has no valid platform user or user index."));
	}

	const auto CheckOriginalContext = [&Result]() -> FString
	{
		const ULocalPlayer* const CurrentPlayer = Result.OriginalLocalPlayer.Get();
		UGameInstance* const CurrentGameInstance = Result.OriginalGameInstance.Get();
		if (!IsValid(CurrentPlayer) || CurrentPlayer->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			|| !IsValid(CurrentGameInstance) || CurrentGameInstance->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
		{
			return TEXT("The original local player or game instance became unavailable.");
		}

		const int32 CurrentUserIndex = CurrentPlayer->GetPlatformUserIndex();
		if (Result.OriginalLocalPlayer.Get() != CurrentPlayer
			|| !IsValid(CurrentPlayer) || CurrentPlayer->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			|| Result.OriginalGameInstance.Get() != CurrentGameInstance
			|| !IsValid(CurrentGameInstance) || CurrentGameInstance->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			|| CurrentPlayer->GetGameInstance() != CurrentGameInstance
			|| CurrentGameInstance->GetFirstGamePlayer() != CurrentPlayer
			|| CurrentPlayer->GetPlatformUserId() != Result.OriginalPlatformUserId
			|| CurrentUserIndex != Result.OriginalPlatformUserIndex)
		{
			return TEXT("The original local player/game instance/primary player/platform user/index identity changed.");
		}
		return FString();
	};
	FString ContextError = CheckOriginalContext();
	if (!ContextError.IsEmpty())
	{
		return Reject(ContextError);
	}

	ISaveGameSystem* const SaveSystem = IPlatformFeaturesModule::Get().GetSaveGameSystem();
	ContextError = CheckOriginalContext();
	if (!ContextError.IsEmpty())
	{
		return Reject(ContextError);
	}
	if (!SaveSystem)
	{
		return Reject(TEXT("The native platform save system is unavailable; absence was not established."));
	}

	const ISaveGameSystem::ESaveExistsResult ExistsResult =
		SaveSystem->DoesSaveGameExistWithResult(*Result.SlotName, Result.OriginalPlatformUserIndex);
	ContextError = CheckOriginalContext();
	if (!ContextError.IsEmpty())
	{
		return Reject(ContextError);
	}

	TStrongObjectPtr<USaveGame> Candidate;
	const bool bWasLoaded = ExistsResult == ISaveGameSystem::ESaveExistsResult::OK;
	if (bWasLoaded)
	{
		TArray<uint8> SaveData;
		const bool bReadSucceeded = SaveSystem->LoadGame(false, *Result.SlotName,
			Result.OriginalPlatformUserIndex, SaveData);
		ContextError = CheckOriginalContext();
		if (!ContextError.IsEmpty())
		{
			return Reject(ContextError);
		}
		if (!bReadSucceeded || SaveData.IsEmpty())
		{
			return Reject(TEXT("An existing save could not be read or contained no bytes; no replacement was created."));
		}
		USaveGame* const LoadedObject = UGameplayStatics::LoadGameFromMemory(SaveData);
		if (IsValid(LoadedObject) && !LoadedObject->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
		{
			Candidate.Reset(LoadedObject);
		}
	}
	else if (ExistsResult == ISaveGameSystem::ESaveExistsResult::DoesNotExist)
	{
		// Honor the native backend's explicit absence result; do not infer absence from a failed read.
		USaveGame* const CreatedObject = UGameplayStatics::CreateSaveGameObject(StaticClass());
		if (IsValid(CreatedObject) && !CreatedObject->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
		{
			Candidate.Reset(CreatedObject);
		}
	}
	else
	{
		return Reject(FString::Printf(TEXT("The native save existence query returned error result %d; no replacement was created."),
			static_cast<int32>(ExistsResult)));
	}

	ContextError = CheckOriginalContext();
	if (!ContextError.IsEmpty())
	{
		return Reject(ContextError);
	}
	if (!IsValid(Candidate.Get()) || Candidate->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		return Reject(bWasLoaded
			? TEXT("The existing save did not produce a live object; no replacement was created.")
			: TEXT("Creating the in-memory object for the missing slot failed."));
	}
	Result.PresetsPath = Candidate->GetPathName();
	UGGYGOSquadPresets* const Presets = Cast<UGGYGOSquadPresets>(Candidate.Get());
	if (!Presets)
	{
		return Reject(TEXT("The existing save object is not a squad presets object; the original file was retained."));
	}
	const auto CheckCandidateData = [Presets, bWasLoaded]() -> FString
	{
		if (!IsValid(Presets) || Presets->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
		{
			return TEXT("The original candidate became unavailable or began destruction.");
		}
		if (bWasLoaded && (!Presets->bObservedLoadingArchive || Presets->bLoadingArchiveHadError))
		{
			return TEXT("The existing presets object did not report a completed loading archive without errors; no replacement was created.");
		}
		if (Presets->Presets.IsEmpty()
			? Presets->ActivePresetIndex != INDEX_NONE
			: !Presets->Presets.IsValidIndex(Presets->ActivePresetIndex))
		{
			return FString::Printf(TEXT("Original ActivePresetIndex=%d is invalid for %d presets; the raw value and file are retained."),
				Presets->ActivePresetIndex, Presets->Presets.Num());
		}
		return FString();
	};
	FString CandidateError = CheckCandidateData();
	if (!CandidateError.IsEmpty())
	{
		return Reject(CandidateError);
	}

	Presets->InitializeSaveGame(Result.OriginalLocalPlayer.Get(), Result.SlotName, bWasLoaded);
	ContextError = CheckOriginalContext();
	if (!ContextError.IsEmpty())
	{
		return Reject(ContextError);
	}
	CandidateError = CheckCandidateData();
	if (!CandidateError.IsEmpty())
	{
		return Reject(CandidateError);
	}

	const bool bCandidateHasOriginalIdentity = Presets->GetLocalPlayer() == Result.OriginalLocalPlayer.Get()
		&& Presets->GetSaveSlotName() == Result.SlotName
		&& Presets->GetPlatformUserId() == Result.OriginalPlatformUserId
		&& Presets->GetPlatformUserIndex() == Result.OriginalPlatformUserIndex;
	ContextError = CheckOriginalContext();
	if (!ContextError.IsEmpty())
	{
		return Reject(ContextError);
	}
	CandidateError = CheckCandidateData();
	if (!CandidateError.IsEmpty())
	{
		return Reject(CandidateError);
	}
	if (!bCandidateHasOriginalIdentity)
	{
		return Reject(TEXT("Native initialization did not preserve the original candidate's local player/user/slot association."));
	}

	Result.Candidate.Reset(Presets);
	Result.Status = bWasLoaded
		? EGGYGOSquadPresetLoadStatus::Loaded
		: EGGYGOSquadPresetLoadStatus::CreatedForMissingSlot;
	return Result;
}

UGGYGOSquadPresets* UGGYGOSquadPresets::LoadOrCreateForLocalPlayer(const ULocalPlayer* LocalPlayer)
{
	FGGYGOSquadPresetLoadResult Result = TryLoadForLocalPlayer(LocalPlayer);
	return Result.Candidate.Get();
}

bool UGGYGOSquadPresets::SaveGameToSlotForLocalPlayer()
{
	return RequestSaveForOriginalSource(false);
}

bool UGGYGOSquadPresets::AsyncSaveGameToSlotForLocalPlayer()
{
	return RequestSaveForOriginalSource(true);
}

bool UGGYGOSquadPresets::RequestSaveForOriginalSource(bool bAsync)
{
	FString ModelPath;
	FString LocalPlayerPath;
	FString GameInstancePath;
	FString RequestSlotName;
	FPlatformUserId OriginalUserId = PLATFORMUSERID_NONE;
	int32 OriginalUserIndex = INDEX_NONE;
	const auto Reject = [&](const FString& Reason) -> bool
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[Teams][SquadPresets] SaveRequest=%s Presets=%s (%p) LocalPlayer=%s GameInstance=%s PlatformUser=%d UserIndex=%d Slot=%s RequiredSlot=%s ActivePresetIndex=%d PresetCount=%d: %s"),
			bAsync ? TEXT("Async") : TEXT("Sync"), *ModelPath, static_cast<const void*>(this),
			*LocalPlayerPath, *GameInstancePath, OriginalUserId.GetInternalId(), OriginalUserIndex,
			*RequestSlotName, *SaveSlotName, ActivePresetIndex, Presets.Num(), *Reason);
		return false;
	};
	if (!IsInGameThread() || !IsValid(this) || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		// No object fields are sampled from an invalid host or off the game thread.
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[Teams][SquadPresets] SaveRequest=%s Presets=%p RequiredSlot=%s: saving requires a live original model on the game thread."),
			bAsync ? TEXT("Async") : TEXT("Sync"), static_cast<const void*>(this), *SaveSlotName);
		return false;
	}
	ModelPath = GetPathName();
	const TWeakObjectPtr<UGGYGOSquadPresets> OriginalModel(this);
	const TStrongObjectPtr<UGGYGOSquadPresets> ModelLifetime(this);

	const ULocalPlayer* const OriginalPlayer = Super::GetLocalPlayer();
	RequestSlotName = Super::GetSaveSlotName();
	if (!IsValid(OriginalPlayer) || OriginalPlayer->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		return Reject(TEXT("The original native local-player association is unavailable."));
	}
	LocalPlayerPath = OriginalPlayer->GetPathName();
	const TWeakObjectPtr<const ULocalPlayer> OriginalLocalPlayer(OriginalPlayer);
	const TStrongObjectPtr<const ULocalPlayer> PlayerLifetime(OriginalPlayer);

	UGameInstance* const OriginalGameInstance = OriginalPlayer->GetGameInstance();
	if (!IsValid(OriginalGameInstance) || OriginalGameInstance->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		return Reject(TEXT("The original local player has no live game instance."));
	}
	GameInstancePath = OriginalGameInstance->GetPathName();
	const TWeakObjectPtr<UGameInstance> OriginalGameInstanceIdentity(OriginalGameInstance);
	const TStrongObjectPtr<UGameInstance> GameInstanceLifetime(OriginalGameInstance);
	OriginalUserId = OriginalPlayer->GetPlatformUserId();
	if (bLoadingArchiveHadError)
	{
		return Reject(TEXT("The original model has an observed archive error; no save request was submitted."));
	}
	if (Presets.IsEmpty() ? ActivePresetIndex != INDEX_NONE : !Presets.IsValidIndex(ActivePresetIndex))
	{
		return Reject(TEXT("The original raw active preset index is invalid; the value and existing file are retained."));
	}
	OriginalUserIndex = OriginalPlayer->GetPlatformUserIndex();

	const auto CheckOriginalSource = [&]() -> FString
	{
		if (OriginalModel.Get() != this || !IsValid(this) || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			|| OriginalLocalPlayer.Get() != OriginalPlayer || !IsValid(OriginalPlayer)
			|| OriginalPlayer->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			|| OriginalGameInstanceIdentity.Get() != OriginalGameInstance || !IsValid(OriginalGameInstance)
			|| OriginalGameInstance->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
		{
			return TEXT("The original model/local player/game instance became unavailable during save admission.");
		}
		if (!OriginalUserId.IsValid() || OriginalUserIndex < 0 || RequestSlotName != SaveSlotName)
		{
			return TEXT("The original platform user/index is invalid or the native association does not identify the retained save slot.");
		}
		if (OriginalPlayer->GetGameInstance() != OriginalGameInstance
			|| OriginalGameInstance->GetFirstGamePlayer() != OriginalPlayer)
		{
			return TEXT("Only the original game instance's current primary local player may persist squad presets.");
		}

		const int32 CurrentUserIndex = OriginalPlayer->GetPlatformUserIndex();
		if (OriginalModel.Get() != this || !IsValid(this) || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			|| OriginalLocalPlayer.Get() != OriginalPlayer || !IsValid(OriginalPlayer)
			|| OriginalPlayer->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			|| OriginalGameInstanceIdentity.Get() != OriginalGameInstance || !IsValid(OriginalGameInstance)
			|| OriginalGameInstance->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			|| OriginalPlayer->GetGameInstance() != OriginalGameInstance
			|| OriginalGameInstance->GetFirstGamePlayer() != OriginalPlayer
			|| OriginalPlayer->GetPlatformUserId() != OriginalUserId || CurrentUserIndex != OriginalUserIndex
			|| ULocalPlayerSaveGame::GetLocalPlayer() != OriginalPlayer
			|| ULocalPlayerSaveGame::GetSaveSlotName() != RequestSlotName)
		{
			return TEXT("The original native local player/game instance/primary player/user/slot identity changed during save admission.");
		}
		return FString();
	};
	FString SourceError = CheckOriginalSource();
	if (!SourceError.IsEmpty())
	{
		return Reject(SourceError);
	}

	const bool bRoutingMatchesOriginal = GetLocalPlayer() == OriginalPlayer
		&& GetSaveSlotName() == RequestSlotName
		&& GetPlatformUserId() == OriginalUserId
		&& GetPlatformUserIndex() == OriginalUserIndex;
	const bool bHasObservedLoadedArchive = !Super::WasLoaded() || bObservedLoadingArchive;
	SourceError = CheckOriginalSource();
	if (!SourceError.IsEmpty())
	{
		return Reject(SourceError);
	}
	if (!bRoutingMatchesOriginal)
	{
		return Reject(TEXT("The save getters do not route to the original native local player/user/slot association."));
	}
	if (!bHasObservedLoadedArchive || bLoadingArchiveHadError)
	{
		return Reject(TEXT("The original loaded model has no observed loading archive or has an observed archive error; no save request was submitted."));
	}
	if (Presets.IsEmpty() ? ActivePresetIndex != INDEX_NONE : !Presets.IsValidIndex(ActivePresetIndex))
	{
		return Reject(TEXT("The original raw active preset index is invalid; the value and existing file are retained."));
	}

	// Admission ends at the request boundary. Native code owns key capture, PreSave, execution,
	// counters and completion. True reports an accepted request, not durable write success.
	return bAsync ? Super::AsyncSaveGameToSlotForLocalPlayer() : Super::SaveGameToSlotForLocalPlayer();
}

void UGGYGOSquadPresets::Serialize(FArchive& Ar)
{
	const bool bWasLoading = Ar.IsLoading();
	const bool bHadErrorBeforeSerialize = bWasLoading && Ar.IsError();
	Super::Serialize(Ar);
	if (bWasLoading)
	{
		const bool bHadErrorAfterSerialize = Ar.IsError();
		bObservedLoadingArchive = true;
		bLoadingArchiveHadError = bLoadingArchiveHadError
			|| bHadErrorBeforeSerialize || bHadErrorAfterSerialize;
	}
}

const FGGYGOSquadPreset* UGGYGOSquadPresets::GetPreset(int32 PresetIndex) const
{
	return Presets.IsValidIndex(PresetIndex) ? &Presets[PresetIndex] : nullptr;
}

int32 UGGYGOSquadPresets::AddPreset(const FString& DisplayName)
{
	FGGYGOSquadPreset& NewPreset = Presets.AddDefaulted_GetRef();
	NewPreset.DisplayName = DisplayName;

	const int32 NewIndex = Presets.Num() - 1;

	// 第一套编队自动出战：否则玩家新建了唯一一套编队，进关卡却因为
	// 没选出战而回落到默认编队，看起来像编成没生效。
	if (ActivePresetIndex == INDEX_NONE)
	{
		ActivePresetIndex = NewIndex;
	}

	return NewIndex;
}

bool UGGYGOSquadPresets::RemovePreset(int32 PresetIndex)
{
	if (!Presets.IsValidIndex(PresetIndex))
	{
		return false;
	}

	Presets.RemoveAt(PresetIndex);

	// 修正出战序号：删掉的是出战编队本身时改指向邻近一套，
	// 删掉的在它前面时序号左移。不修正会让出战指向另一套编队。
	if (Presets.IsEmpty())
	{
		ActivePresetIndex = INDEX_NONE;
	}
	else if (ActivePresetIndex == PresetIndex)
	{
		ActivePresetIndex = FMath::Min(PresetIndex, Presets.Num() - 1);
	}
	else if (ActivePresetIndex > PresetIndex)
	{
		--ActivePresetIndex;
	}

	return true;
}

bool UGGYGOSquadPresets::RenamePreset(int32 PresetIndex, const FString& NewDisplayName)
{
	if (!Presets.IsValidIndex(PresetIndex))
	{
		return false;
	}

	Presets[PresetIndex].DisplayName = NewDisplayName;
	return true;
}

bool UGGYGOSquadPresets::SetPresetMembers(int32 PresetIndex, const TArray<FPrimaryAssetId>& NewMembers)
{
	if (!Presets.IsValidIndex(PresetIndex))
	{
		return false;
	}

	if (!GGYGOSquad::IsMemberCountWithinCapacity(NewMembers.Num()))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("SetPresetMembers: 编队 [%d] 请求 %d 名成员，超过上限 %d，已拒绝，原名单不变。"),
			PresetIndex, NewMembers.Num(), GGYGO_MAX_SQUAD_SIZE);
		return false;
	}

	Presets[PresetIndex].Members = NewMembers;
	return true;
}

bool UGGYGOSquadPresets::SetActivePresetIndex(int32 PresetIndex)
{
	if (!Presets.IsValidIndex(PresetIndex))
	{
		return false;
	}

	ActivePresetIndex = PresetIndex;
	return true;
}

FGGYGOSquadPresetRosterResolveResult UGGYGOSquadPresets::ResolveActivePresetRoster(
	TArray<UGGYGOPawnData*>& OutRoster) const
{
	return ResolvePresetRosterInternal(INDEX_NONE, true, OutRoster);
}

FGGYGOSquadPresetRosterResolveResult UGGYGOSquadPresets::ResolvePresetRoster(
	int32 PresetIndex, TArray<UGGYGOPawnData*>& OutRoster) const
{
	return ResolvePresetRosterInternal(PresetIndex, false, OutRoster);
}

FGGYGOSquadPresetRosterResolveResult UGGYGOSquadPresets::ResolvePresetRosterInternal(
	int32 RequestedPresetIndex, bool bResolveActivePreset, TArray<UGGYGOPawnData*>& OutRoster) const
{
	OutRoster.Reset();

	FGGYGOSquadPresetRosterResolveResult Result;
	Result.PresetsPath = GetPathNameSafe(this);
	Result.PresetIndex = RequestedPresetIndex;
	const auto Reject = [&Result, &OutRoster](FString Error)
	{
		// A synchronous load callback may have reentered with the same output array.
		OutRoster.Reset();
		Result.Status = EGGYGOSquadPresetRosterResolveStatus::Invalid;
		Result.Error = MoveTemp(Error);
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[Teams.PresetResolver] Presets=%s PresetIndex=%d MemberIndex=%d MemberId=%s MemberPath=%s Error=%s"),
			*Result.PresetsPath, Result.PresetIndex, Result.MemberIndex,
			*Result.MemberId.ToString(), *Result.MemberPath.ToString(), *Result.Error);
		return Result;
	};

	if (!IsInGameThread())
	{
		return Reject(TEXT("Synchronous preset resolution requires the game thread."));
	}
	if (!IsValid(this) || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		return Reject(TEXT("The original presets object is invalid or being destroyed."));
	}

	const TWeakObjectPtr<const UGGYGOSquadPresets> OriginalPresets(this);
	Result.PresetIndex = bResolveActivePreset ? ActivePresetIndex : RequestedPresetIndex;
	if (bResolveActivePreset && Presets.IsEmpty() && Result.PresetIndex == INDEX_NONE)
	{
		Result.Status = EGGYGOSquadPresetRosterResolveStatus::Unconfigured;
		return Result;
	}
	if (!Presets.IsValidIndex(Result.PresetIndex))
	{
		return Reject(FString::Printf(TEXT("Requested preset index is invalid for %d saved presets."), Presets.Num()));
	}

	const int32 OriginalPresetIndex = Result.PresetIndex;
	const int32 RawMemberCount = Presets[OriginalPresetIndex].Members.Num();
	if (!GGYGOSquad::IsMemberCountWithinCapacity(RawMemberCount))
	{
		return Reject(FString::Printf(TEXT("Saved raw member count %d exceeds capacity %d; source data is unchanged."),
			RawMemberCount, GGYGO_MAX_SQUAD_SIZE));
	}

	// No saved container reference crosses asset lookup/loading or any project callback.
	const TArray<FPrimaryAssetId> OriginalMembers = Presets[OriginalPresetIndex].Members;
	int32 NonemptyMemberCount = 0;
	for (int32 InputIndex = 0; InputIndex < OriginalMembers.Num(); ++InputIndex)
	{
		const FPrimaryAssetId& InputId = OriginalMembers[InputIndex];
		if (InputId == FPrimaryAssetId())
		{
			continue;
		}
		if (!InputId.IsValid())
		{
			Result.MemberIndex = InputIndex;
			Result.MemberId = InputId;
			return Reject(TEXT("Member Id is malformed; only a default FPrimaryAssetId is a legal empty slot."));
		}
		++NonemptyMemberCount;
	}
	if (NonemptyMemberCount == 0)
	{
		Result.Status = EGGYGOSquadPresetRosterResolveStatus::Unconfigured;
		return Result;
	}

	UAssetManager* const AssetManager = UAssetManager::GetIfInitialized();
	if (!IsValid(AssetManager) || AssetManager->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		return Reject(TEXT("The required AssetManager is unavailable or being destroyed."));
	}
	const TWeakObjectPtr<UAssetManager> OriginalAssetManager(AssetManager);
	const auto CheckOriginalContext = [OriginalPresets, OriginalAssetManager, OriginalPresetIndex,
		bResolveActivePreset, &OriginalMembers]() -> FString
	{
		const UGGYGOSquadPresets* const Source = OriginalPresets.Get();
		if (!Source || Source->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
		{
			return TEXT("The original presets object became invalid or began destruction during resolution.");
		}
		if (!Source->Presets.IsValidIndex(OriginalPresetIndex)
			|| Source->Presets[OriginalPresetIndex].Members != OriginalMembers
			|| (bResolveActivePreset && Source->ActivePresetIndex != OriginalPresetIndex))
		{
			return TEXT("The original selected preset input changed during resolution; no new source was adopted.");
		}
		UAssetManager* const CurrentAssetManager = OriginalAssetManager.Get();
		if (!CurrentAssetManager || CurrentAssetManager->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			|| UAssetManager::GetIfInitialized() != CurrentAssetManager)
		{
			return TEXT("The original required AssetManager became unavailable or was replaced during resolution.");
		}
		return FString();
	};

	struct FResolvedMember
	{
		FResolvedMember(int32 InMemberIndex, const FSoftObjectPath& InMemberPath, UGGYGOPawnData* InPawnData)
			: MemberIndex(InMemberIndex), MemberPath(InMemberPath), PawnData(InPawnData)
		{
		}

		int32 MemberIndex;
		FSoftObjectPath MemberPath;
		// Retention belongs only to this synchronous stack, never a saved/cache authority.
		TStrongObjectPtr<UGGYGOPawnData> PawnData;
	};
	TArray<FResolvedMember> Candidate;
	Candidate.Reserve(NonemptyMemberCount);
	for (int32 InputIndex = 0; InputIndex < OriginalMembers.Num(); ++InputIndex)
	{
		const FPrimaryAssetId& InputId = OriginalMembers[InputIndex];
		if (InputId == FPrimaryAssetId())
		{
			continue;
		}

		Result.MemberIndex = InputIndex;
		Result.MemberId = InputId;
		Result.MemberPath = FSoftObjectPath();
		FString ContextError = CheckOriginalContext();
		if (!ContextError.IsEmpty())
		{
			return Reject(MoveTemp(ContextError));
		}
		Result.MemberPath = OriginalAssetManager->GetPrimaryAssetPath(InputId);
		ContextError = CheckOriginalContext();
		if (!ContextError.IsEmpty())
		{
			return Reject(MoveTemp(ContextError));
		}
		if (Result.MemberPath.IsNull() || !Result.MemberPath.IsValid())
		{
			return Reject(TEXT("Member Id has no valid indexed asset path; the entire preset is rejected."));
		}

		UObject* const LoadedAsset = Result.MemberPath.TryLoad();
		ContextError = CheckOriginalContext();
		if (!ContextError.IsEmpty())
		{
			return Reject(MoveTemp(ContextError));
		}
		if (!IsValid(LoadedAsset) || LoadedAsset->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
		{
			return Reject(TEXT("Member asset failed to load or became invalid; the entire preset is rejected."));
		}
		TStrongObjectPtr<UObject> LoadedObject(LoadedAsset);
		UGGYGOPawnData* const PawnData = Cast<UGGYGOPawnData>(LoadedObject.Get());
		if (!PawnData)
		{
			return Reject(FString::Printf(TEXT("Member asset class %s is not GGYGOPawnData; the entire preset is rejected."),
				*LoadedObject->GetClass()->GetPathName()));
		}
		Candidate.Emplace(InputIndex, Result.MemberPath, PawnData);
	}

	const FString FinalContextError = CheckOriginalContext();
	if (!FinalContextError.IsEmpty())
	{
		return Reject(FinalContextError);
	}
	TArray<UGGYGOPawnData*> ResolvedRoster;
	ResolvedRoster.Reserve(Candidate.Num());
	for (const FResolvedMember& Member : Candidate)
	{
		if (!IsValid(Member.PawnData.Get())
			|| Member.PawnData->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
		{
			Result.MemberIndex = Member.MemberIndex;
			Result.MemberId = OriginalMembers[Member.MemberIndex];
			Result.MemberPath = Member.MemberPath;
			return Reject(TEXT("An earlier resolved member became invalid before complete publication."));
		}
		ResolvedRoster.Add(Member.PawnData.Get());
	}

	OutRoster = MoveTemp(ResolvedRoster);
	Result.Status = EGGYGOSquadPresetRosterResolveStatus::Resolved;
	Result.MemberIndex = INDEX_NONE;
	Result.MemberId = FPrimaryAssetId();
	Result.MemberPath = FSoftObjectPath();
	return Result;
}

int32 UGGYGOSquadPresets::GetLatestDataVersion() const
{
	// 改动 Presets 的字段含义时递增，并在 HandlePostLoad 里迁移旧数据。
	return 1;
}

void UGGYGOSquadPresets::HandlePostLoad()
{
	Super::HandlePostLoad();

	// 非法出战序号保留原值，由名单解析明确拒绝，不能在加载时替换为正常选择。
	const bool bHasValidActivePresetIndex = Presets.IsEmpty()
		? ActivePresetIndex == INDEX_NONE
		: Presets.IsValidIndex(ActivePresetIndex);
	if (!bHasValidActivePresetIndex)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[Teams][SquadPresets] 存档 [%s] 的出战序号 [%d] 无效（共 %d 套）：%s。原值保留，名单解析将拒绝该选择。"),
			*GetPathName(), ActivePresetIndex, Presets.Num(),
			Presets.IsEmpty()
				? TEXT("空编队列表要求出战序号为 INDEX_NONE")
				: TEXT("出战序号不在编队列表范围内"));
	}

	// 上限调整或外部修改可能使旧名单超限。只诊断并保留原数据，解析时拒绝。
	for (int32 PresetIndex = 0; PresetIndex < Presets.Num(); ++PresetIndex)
	{
		const FGGYGOSquadPreset& Preset = Presets[PresetIndex];
		if (!GGYGOSquad::IsMemberCountWithinCapacity(Preset.Members.Num()))
		{
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("编队存档 [%d] 有 %d 名成员，超过上限 %d，原数据保留，该编队不可解析。"),
				PresetIndex, Preset.Members.Num(), GGYGO_MAX_SQUAD_SIZE);
		}
	}
}
