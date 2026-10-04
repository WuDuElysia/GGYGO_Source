/**
 * @file GGYGOPlayerController.cpp
 * @brief 玩家控制器实现
 */
#include "Player/GGYGOPlayerController.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Engine/Level.h"
#include "Engine/Player.h"
#include "Engine/World.h"
#include "GameFramework/InputSettings.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerInput.h"
#include "Input/GGYGOMovementInputOriginResource.h"
#include "Misc/ScopeExit.h"
#include "Player/GGYGOLocalPlayer.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

#if !UE_BUILD_SHIPPING
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "Character/Data/GGYGOPawnData.h"
#include "Engine/AssetManager.h"
#include "Teams/GGYGOSquadPresets.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOPlayerController)

DEFINE_LOG_CATEGORY_STATIC(LogGGYGONativeInputInitialization, Log, All);

namespace
{
	/** Read-only copy of the exact native class selection; not a source or qualification state. */
	struct FNativeInputClassSelection
	{
		TWeakObjectPtr<UClass> Class;
		FString Configuration;
	};

	bool ReadNativeInputClassSelection(const APlayerController* Controller,
		FNativeInputClassSelection& OutSelection, FString& OutError)
	{
		// Inspect the returned configuration copy before TSubclassOf::Get can turn a
		// wrong-base override into null and accidentally select the default mode.
		TSubclassOf<UPlayerInput> OverrideClass = Controller->GetOverridePlayerInputClass();
		UClass* SelectedClass = OverrideClass.GetGCPtr().Get();
		if (SelectedClass)
		{
			OutSelection.Configuration = FString::Printf(TEXT("Controller.OverridePlayerInputClass=%s"), *GetPathNameSafe(SelectedClass));
		}
		else
		{
			const UInputSettings* Settings = UInputSettings::GetInputSettings();
			if (!IsValid(Settings))
			{
				OutSelection.Configuration = TEXT("InputSettings.DefaultPlayerInputClass=<missing settings>");
				OutError = TEXT("InputSettings is unavailable; no replacement input class is selected.");
				return false;
			}
			// The native field is C++ private but exposed as a config property. Read that
			// exact reflected configuration, not GetDefaultPlayerInputClass's fallback.
			const FSoftClassProperty* DefaultClassProperty = FindFProperty<FSoftClassProperty>(
				Settings->GetClass(), TEXT("DefaultPlayerInputClass"));
			if (!DefaultClassProperty || !DefaultClassProperty->HasAnyPropertyFlags(CPF_Config)
				|| DefaultClassProperty->MetaClass != UPlayerInput::StaticClass())
			{
				OutSelection.Configuration = TEXT("InputSettings.DefaultPlayerInputClass=<invalid config property>");
				OutError = TEXT("The native reflected default class configuration contract is unavailable.");
				return false;
			}
			const FSoftObjectPtr DefaultClassReference = DefaultClassProperty->GetPropertyValue_InContainer(Settings);
			OutSelection.Configuration = FString::Printf(TEXT("InputSettings.DefaultPlayerInputClass=%s"),
				*DefaultClassReference.ToSoftObjectPath().ToString());
			if (!DefaultClassReference.IsValid())
			{
				OutError = TEXT("The configured default class is missing or not loaded; the engine's base PlayerInput fallback is forbidden.");
				return false;
			}
			SelectedClass = Cast<UClass>(DefaultClassReference.Get());
		}
		if (!IsValid(SelectedClass) || !SelectedClass->IsChildOf(UPlayerInput::StaticClass())
			|| SelectedClass->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = TEXT("The exact configured class must be a live, concrete PlayerInput class.");
			return false;
		}
		OutSelection.Class = SelectedClass;
		OutError.Reset();
		return true;
	}
}

AGGYGOPlayerController::AGGYGOPlayerController(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void AGGYGOPlayerController::InitInputSystem()
{
	if (NativeInputInitializationStage == ENativeInputInitializationStage::Rejected)
	{
		// The first rejection was diagnosed. Native Tick may call again, but no attempt is repeated.
		return;
	}
	if (NativeInputInitializationStage == ENativeInputInitializationStage::Running)
	{
		if (!bReportedNativeInputInitializationBusy)
		{
			bReportedNativeInputInitializationBusy = true;
			UE_LOG(LogGGYGONativeInputInitialization, Warning,
				TEXT("[Input.NativeCreation] Controller=%s Player=%s Input=%s OverrideClass=%s Reason=NativeInputInitializationBusy: nested Init is rejected without Super or a queued retry; the outer execution is retained."),
				*GetPathNameSafe(this), *GetPathNameSafe(Player.Get()), *GetPathNameSafe(PlayerInput.Get()),
				*GetPathNameSafe(GetOverridePlayerInputClass().Get()));
		}
		return;
	}
	NativeInputInitializationStage = ENativeInputInitializationStage::Running;
	const TWeakObjectPtr<AGGYGOPlayerController> OriginalController(this);
	const FString OriginalControllerPath = GetPathNameSafe(this);
	ON_SCOPE_EXIT
	{
		if (AGGYGOPlayerController* Controller = OriginalController.Get())
		{
			if (!Controller->IsActorBeingDestroyed()
				&& Controller->NativeInputInitializationStage == ENativeInputInitializationStage::Running)
			{
				Controller->NativeInputInitializationStage = ENativeInputInitializationStage::Idle;
			}
		}
	};

	const TWeakObjectPtr<UPlayer> OriginalNativePlayer(Player.Get());
	const TWeakObjectPtr<ULocalPlayer> OriginalLocalPlayer(GetLocalPlayer());
	const TWeakObjectPtr<UPlayerInput> OriginalInput(PlayerInput.Get());
	const TWeakObjectPtr<UWorld> OriginalWorld(GetWorld());
	TWeakObjectPtr<UGGYGOMovementInputOriginResource> OriginalResource;
	FNativeInputClassSelection Selection;
	Selection.Configuration = TEXT("<not selected>");
	bool bAcceptedExistingRegistration = false;
	const auto Reject = [&](FName Reason, const FString& Detail)
	{
		if (AGGYGOPlayerController* Controller = OriginalController.Get())
		{
			if (!Controller->IsActorBeingDestroyed())
			{
				Controller->NativeInputInitializationStage = ENativeInputInitializationStage::Rejected;
			}
		}
		if (bAcceptedExistingRegistration && OriginalResource.IsValid() && OriginalInput.IsValid())
		{
			OriginalResource->RetireInitialQualification(OriginalInput.Get(), Reason);
		}
		UE_LOG(LogGGYGONativeInputInitialization, Warning,
			TEXT("[Input.NativeCreation] Controller=%s Player=%s Resource=%s OriginalInput=%s Class=%s Configuration=%s Reason=%s: %s Correct the dependency and recreate this Controller; automatic retry is disabled."),
			*OriginalControllerPath, *GetPathNameSafe(OriginalNativePlayer.Get()), *GetPathNameSafe(OriginalResource.Get()),
			*GetPathNameSafe(OriginalInput.Get()), *GetPathNameSafe(Selection.Class.Get()),
			*Selection.Configuration, *Reason.ToString(), *Detail);
	};
	if (!IsValid(this) || IsTemplate() || IsActorBeingDestroyed() || !OriginalWorld.IsValid()
		|| !IsValid(OriginalWorld->PersistentLevel))
	{
		Reject(TEXT("InvalidNativeInputOwner"), TEXT("A live non-template Controller and world/persistent level are required."));
		return;
	}
	const bool bHadInput = !OriginalInput.IsExplicitlyNull();
	FString Error;
	if (bHadInput)
	{
		if (!OriginalInput.IsValid() || OriginalInput->IsTemplate() || OriginalInput->GetOuter() != this)
		{
			Reject(TEXT("InvalidExistingNativeInput"), TEXT("The existing native input object is invalid, a template, or owned by another Controller."));
			return;
		}
		// Native Super does not reselect configuration when its input object already exists.
		Selection.Class = OriginalInput->GetClass();
		Selection.Configuration = TEXT("ExistingPlayerInput.ActualClass (native selection skipped)");
	}
	else if (!ReadNativeInputClassSelection(this, Selection, Error))
	{
		Reject(TEXT("InvalidNativeInputConfiguration"), Error);
		return;
	}

	UGGYGOMovementInputOriginResource::FCreationTicket Ticket;
	bool bNeedsCreation = false;
	const bool bHasLocalPlayer = !OriginalLocalPlayer.IsExplicitlyNull();
	if (bHasLocalPlayer)
	{
		UGGYGOLocalPlayer* Host = Cast<UGGYGOLocalPlayer>(OriginalLocalPlayer.Get());
		if (!IsValid(Host) || Host->IsTemplate() || Host->PlayerController != this)
		{
			Reject(TEXT("MissingNativeInputOriginHost"), TEXT("The original local player must be the live project host with the original native Controller relationship."));
			return;
		}
		OriginalResource = Host->GetMovementInputOriginResource();
		if (!OriginalResource.IsValid() || OriginalResource->IsTemplate() || OriginalResource->GetOuter() != Host)
		{
			Reject(TEXT("MissingNativeInputOriginResource"), TEXT("Real PlayerAdded did not provide a valid original resource; this caller cannot create one."));
			return;
		}
		if (!OriginalResource->BeginNativeCreation(this, Selection.Class.Get(), Ticket, bNeedsCreation, Error))
		{
			Reject(TEXT("NativeInputOriginBeginRejected"), Error);
			return;
		}
		bAcceptedExistingRegistration = !bNeedsCreation;
	}
	const UGGYGOMovementInputOriginResource::FCreationTicket OriginalTicket = Ticket;
	const TWeakObjectPtr<UGGYGOMovementInputOriginResource> ResourceForClose = OriginalResource;
	ON_SCOPE_EXIT
	{
		if (bNeedsCreation)
		{
			if (UGGYGOMovementInputOriginResource* Resource = ResourceForClose.Get())
			{
				FString CloseError;
				Resource->CloseNativeCreation(OriginalTicket, TEXT("NativeInputInitializationScopeExit"), CloseError);
			}
			// An expired resource already sealed its scope in BeginDestroy.
		}
	};

	// Running remains active across every native external call, including nonlocal/existing paths.
	Super::InitInputSystem();
	if (!OriginalController.IsValid() || IsActorBeingDestroyed() || !OriginalWorld.IsValid()
		|| GetWorld() != OriginalWorld.Get()
		|| !OriginalNativePlayer.HasSameIndexAndSerialNumber(TWeakObjectPtr<UPlayer>(Player.Get()))
		|| !OriginalLocalPlayer.HasSameIndexAndSerialNumber(TWeakObjectPtr<ULocalPlayer>(GetLocalPlayer()))
		|| !IsValid(PlayerInput.Get()) || PlayerInput->IsTemplate() || PlayerInput->GetOuter() != this
		|| !Selection.Class.IsValid() || PlayerInput->GetClass() != Selection.Class.Get()
		|| (bHadInput && !OriginalInput.HasSameIndexAndSerialNumber(TWeakObjectPtr<UPlayerInput>(PlayerInput.Get()))))
	{
		Reject(TEXT("NativeInputReturnContextChanged"), TEXT("Native Init returned after an original owner/world/input/class identity changed or was destroyed."));
		return;
	}
	if (!bHadInput)
	{
		FNativeInputClassSelection CurrentSelection;
		if (!ReadNativeInputClassSelection(this, CurrentSelection, Error)
			|| !Selection.Class.HasSameIndexAndSerialNumber(CurrentSelection.Class)
			|| Selection.Configuration != CurrentSelection.Configuration)
		{
			Reject(TEXT("NativeInputConfigurationChanged"), FString::Printf(
				TEXT("Configuration after native calls=%s; validation=%s. The original selection cannot be replaced."),
				*CurrentSelection.Configuration, *Error));
			return;
		}
	}
	if (bHasLocalPlayer)
	{
		const UGGYGOLocalPlayer* Host = Cast<UGGYGOLocalPlayer>(OriginalLocalPlayer.Get());
		if (!IsValid(Host) || Host->PlayerController != this || !OriginalResource.IsValid()
			|| !OriginalResource.HasSameIndexAndSerialNumber(
				TWeakObjectPtr<UGGYGOMovementInputOriginResource>(Host->GetMovementInputOriginResource())))
		{
			Reject(TEXT("NativeInputOriginContextChanged"), TEXT("The original LocalPlayer/resource relationship was lost; no successor is adopted."));
			return;
		}
		if (bNeedsCreation)
		{
			if (!OriginalResource->CompleteNativeCreation(OriginalTicket, PlayerInput.Get(), Error))
			{
				Reject(TEXT("NativeInputOriginCompletionRejected"), Error);
				return;
			}
		}
		else if (OriginalResource->GetQualification(OriginalInput.Get()) == EGGYGOMovementInputOriginQualification::Unavailable)
		{
			Reject(TEXT("ExistingNativeInputRegistrationLost"), TEXT("The original existing producer is no longer a current live registration."));
			return;
		}
	}
	// No LocalPlayer is a legitimate native path, including pre-SetPlayer Tick. It grants no qualification.
}

UGGYGOAbilitySystemComponent* AGGYGOPlayerController::GetGGYGOAbilitySystemComponent() const
{
	// 经 PawnExtension 取而不是直接 FindComponentByClass：
	// ASC 可能不在 Pawn 上（队伍级 ASC 挂 PlayerState），
	// 而 PawnExtension 是"当前该用哪个 ASC"这个问题的唯一答案来源。
	const UGGYGOPawnExtensionComponent* PawnExtComp =
		UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(GetPawn());

	return PawnExtComp ? PawnExtComp->GetGGYGOAbilitySystemComponent() : nullptr;
}

void AGGYGOPlayerController::PostProcessInput(const float DeltaTime, const bool bGamePaused)
{
	// 在 Super 之前消费。Super 会把累积的移动与视角输入交给 Pawn，
	// 而能力激活可能施加 `Restriction.CantMove` —— 先激活能力，
	// 这一帧的移动就能立刻被限制住，不会多走一帧。
	if (UGGYGOAbilitySystemComponent* GGYGOASC = GetGGYGOAbilitySystemComponent())
	{
		GGYGOASC->ProcessAbilityInput(DeltaTime, bGamePaused);
	}

	Super::PostProcessInput(DeltaTime, bGamePaused);
}

#if !UE_BUILD_SHIPPING

namespace
{
	/** 取原本地玩家宿主的唯一编队存档，取得前后校验来源。调试命令共用。 */
	UGGYGOSquadPresets* GetSquadPresetsForExec(const APlayerController* PlayerController)
	{
		FString ControllerPath;
		FString LocalPlayerPath;
		const auto Reject = [&](const FString& Reason) -> UGGYGOSquadPresets*
		{
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("[Teams][Exec] Controller=%s (%p) LocalPlayer=%s: %s"),
				*ControllerPath, static_cast<const void*>(PlayerController), *LocalPlayerPath, *Reason);
			return nullptr;
		};
		if (!IsInGameThread() || !IsValid(PlayerController)
			|| PlayerController->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
		{
			return Reject(TEXT("Preset access requires a live original Controller on the game thread."));
		}
		ControllerPath = PlayerController->GetPathName();
		const TWeakObjectPtr<const APlayerController> OriginalController(PlayerController);
		const ULocalPlayer* const LocalPlayer = PlayerController->GetLocalPlayer();
		if (OriginalController.Get() != PlayerController
			|| PlayerController->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			|| PlayerController->GetLocalPlayer() != LocalPlayer)
		{
			return Reject(TEXT("The original Controller/source qualification changed during local-player selection."));
		}
		if (!IsValid(LocalPlayer) || LocalPlayer->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
		{
			return Reject(TEXT("The original Controller has no live local player for preset commands."));
		}
		LocalPlayerPath = LocalPlayer->GetPathName();
		const TWeakObjectPtr<const ULocalPlayer> OriginalLocalPlayer(LocalPlayer);
		const UGGYGOLocalPlayer* const PresetsHost = Cast<UGGYGOLocalPlayer>(LocalPlayer);
		if (!PresetsHost)
		{
			return Reject(TEXT("The original local player is not GGYGOLocalPlayer; check DefaultEngine.ini LocalPlayerClassName."));
		}
		const auto CheckOriginalSource = [OriginalController, OriginalLocalPlayer]() -> FString
		{
			const APlayerController* const CurrentController = OriginalController.Get();
			const ULocalPlayer* const CurrentLocalPlayer = OriginalLocalPlayer.Get();
			if (!CurrentController || CurrentController->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
				|| !CurrentLocalPlayer || CurrentLocalPlayer->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
				|| CurrentController->GetLocalPlayer() != CurrentLocalPlayer)
			{
				return TEXT("The original Controller/local player became unavailable or the Controller now has another source.");
			}
			return FString();
		};

		FString SourceError = CheckOriginalSource();
		if (!SourceError.IsEmpty())
		{
			return Reject(FString::Printf(TEXT("Before TryGetSquadPresets: %s"), *SourceError));
		}
		UGGYGOSquadPresets* Presets = nullptr;
		FString AcquisitionError;
		const bool bAcquiredPresets = PresetsHost->TryGetSquadPresets(Presets, AcquisitionError);
		SourceError = CheckOriginalSource();
		if (!SourceError.IsEmpty())
		{
			return Reject(FString::Printf(TEXT("After TryGetSquadPresets: %s; AcquisitionError=%s"),
				*SourceError, *AcquisitionError));
		}
		if (!bAcquiredPresets)
		{
			return Reject(FString::Printf(TEXT("TryGetSquadPresets rejected the original source; AcquisitionError=%s"),
				*AcquisitionError));
		}
		if (!IsValid(Presets) || Presets->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
		{
			return Reject(FString::Printf(TEXT("TryGetSquadPresets returned success without valid presets; AcquisitionError=%s"),
				*AcquisitionError));
		}
		return Presets;
	}

	/** Diagnostic copy only, captured before native callbacks; no save execution or retained source. */
	FString DescribeSquadPresetSaveSourceForExec(const APlayerController* PlayerController,
		const UGGYGOSquadPresets* Presets)
	{
		return FString::Printf(TEXT("Controller=%s Presets=%s LocalPlayer=%s Slot=%s RequiredSlot=%s"),
			*GetPathNameSafe(PlayerController), *GetPathNameSafe(Presets),
			*GetPathNameSafe(Presets->ULocalPlayerSaveGame::GetLocalPlayer()),
			*Presets->ULocalPlayerSaveGame::GetSaveSlotName(), *UGGYGOSquadPresets::SaveSlotName);
	}
}

void AGGYGOPlayerController::GGYGODumpSquadPresets()
{
	UGGYGOSquadPresets* Presets = GetSquadPresetsForExec(this);
	if (!Presets)
	{
		return;
	}

	const int32 PresetCount = Presets->GetPresetCount();
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("编队存档：共 %d 套，出战序号 %d。"), PresetCount, Presets->GetActivePresetIndex());

	for (int32 PresetIndex = 0; PresetIndex < PresetCount; ++PresetIndex)
	{
		const FGGYGOSquadPreset* Preset = Presets->GetPreset(PresetIndex);
		if (!Preset)
		{
			continue;
		}

		FString MemberList;
		for (const FPrimaryAssetId& Member : Preset->Members)
		{
			MemberList += FString::Printf(TEXT("[%s]"), Member.IsValid() ? *Member.ToString() : TEXT("空位"));
		}

		UE_LOG(LogGGYGOAbilitySystem, Display,
			TEXT("  编队 %d%s 名称 [%s] 成员 %s"),
			PresetIndex,
			PresetIndex == Presets->GetActivePresetIndex() ? TEXT(" (出战)") : TEXT(""),
			*Preset->DisplayName,
			MemberList.IsEmpty() ? TEXT("（无）") : *MemberList);
	}
}

void AGGYGOPlayerController::GGYGOSetSquadMember(int32 PresetIndex, int32 MemberIndex, const FString& PawnDataName)
{
	UGGYGOSquadPresets* Presets = GetSquadPresetsForExec(this);
	if (!Presets)
	{
		return;
	}

	if (PresetIndex < 0 || MemberIndex < 0)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("GGYGOSetSquadMember: 序号不能为负。"));
		return;
	}

	// 编队不够就补齐到目标序号，省得调试时要先建编队再设成员。
	while (Presets->GetPresetCount() <= PresetIndex)
	{
		Presets->AddPreset(FString::Printf(TEXT("编队 %d"), Presets->GetPresetCount() + 1));
	}

	const FGGYGOSquadPreset* Preset = Presets->GetPreset(PresetIndex);
	TArray<FPrimaryAssetId> Members = Preset->Members;

	if (MemberIndex >= Members.Num())
	{
		Members.SetNum(MemberIndex + 1);
	}

	if (PawnDataName.IsEmpty())
	{
		Members[MemberIndex] = FPrimaryAssetId();
	}
	else
	{
		// 类型名固定为 PawnData 的 PrimaryAssetType，只让调用者输资产名。
		const FPrimaryAssetId MemberId(UGGYGOPawnData::StaticClass()->GetFName(), FName(*PawnDataName));
		if (UAssetManager::Get().GetPrimaryAssetPath(MemberId).IsNull())
		{
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("GGYGOSetSquadMember: 找不到角色 [%s]。资产名要与 PrimaryAssetId 一致，"
					 "且 GGYGOPawnData 已在 DefaultGame.ini 注册扫描目录。"),
				*PawnDataName);
			return;
		}

		Members[MemberIndex] = MemberId;
	}

	Presets->SetPresetMembers(PresetIndex, Members);
	const FString SaveSource = DescribeSquadPresetSaveSourceForExec(this, Presets);
	const bool bSaveRequestAccepted = Presets->SaveGameToSlotForLocalPlayer();
	if (!bSaveRequestAccepted)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("GGYGOSetSquadMember: 保存请求被拒绝，当前内存数据保留。%s；具体原因见原生保存诊断。"),
			*SaveSource);
		return;
	}

	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("GGYGOSetSquadMember: 编队 %d 第 %d 位请求值 [%s]；保存请求已接受，此提示未确认持久化结果。"),
		PresetIndex, MemberIndex, PawnDataName.IsEmpty() ? TEXT("空位") : *PawnDataName);
}

void AGGYGOPlayerController::GGYGOSetActiveSquadPreset(int32 PresetIndex)
{
	UGGYGOSquadPresets* Presets = GetSquadPresetsForExec(this);
	if (!Presets)
	{
		return;
	}

	if (!Presets->SetActivePresetIndex(PresetIndex))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("GGYGOSetActiveSquadPreset: 序号 %d 越界，共 %d 套编队。"),
			PresetIndex, Presets->GetPresetCount());
		return;
	}

	const FString SaveSource = DescribeSquadPresetSaveSourceForExec(this, Presets);
	const bool bSaveRequestAccepted = Presets->SaveGameToSlotForLocalPlayer();
	if (!bSaveRequestAccepted)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("GGYGOSetActiveSquadPreset: 保存请求被拒绝，当前内存数据保留。%s；具体原因见原生保存诊断。"),
			*SaveSource);
		return;
	}

	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("GGYGOSetActiveSquadPreset: 出战编队选择 %d 的保存请求已接受，此提示未确认持久化结果。"), PresetIndex);
}

void AGGYGOPlayerController::GGYGOClearSquadPresets()
{
	UGGYGOSquadPresets* Presets = GetSquadPresetsForExec(this);
	if (!Presets)
	{
		return;
	}

	while (Presets->GetPresetCount() > 0)
	{
		Presets->RemovePreset(0);
	}

	const FString SaveSource = DescribeSquadPresetSaveSourceForExec(this, Presets);
	const bool bSaveRequestAccepted = Presets->SaveGameToSlotForLocalPlayer();
	if (!bSaveRequestAccepted)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("GGYGOClearSquadPresets: 保存请求被拒绝，当前内存数据保留。%s；具体原因见原生保存诊断。"),
			*SaveSource);
		return;
	}

	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("GGYGOClearSquadPresets: 清空编队后的保存请求已接受，此提示未确认持久化结果。"));
}

#else

// Shipping 下保留空实现：声明无法条件编译（UHT 限制），符号必须存在。
void AGGYGOPlayerController::GGYGODumpSquadPresets() {}
void AGGYGOPlayerController::GGYGOSetSquadMember(int32, int32, const FString&) {}
void AGGYGOPlayerController::GGYGOSetActiveSquadPreset(int32) {}
void AGGYGOPlayerController::GGYGOClearSquadPresets() {}

#endif
