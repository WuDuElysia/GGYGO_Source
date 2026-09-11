/**
 * @file GGYGOPlayerController.cpp
 * @brief 玩家控制器实现
 */
#include "Player/GGYGOPlayerController.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "GameFramework/Pawn.h"

#if !UE_BUILD_SHIPPING
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "Character/Data/GGYGOPawnData.h"
#include "Engine/AssetManager.h"
#include "Teams/GGYGOSquadPresets.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOPlayerController)

AGGYGOPlayerController::AGGYGOPlayerController(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
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
	/** 取本地玩家的编队存档，失败时报错。调试命令共用。 */
	UGGYGOSquadPresets* GetSquadPresetsForExec(const APlayerController* PlayerController)
	{
		UGGYGOSquadPresets* Presets = UGGYGOSquadPresets::GetForPlayerController(PlayerController);
		if (!Presets)
		{
			UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("编队存档不可用：这个 Controller 没有本地玩家。"));
		}
		return Presets;
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
	Presets->SaveGameToSlotForLocalPlayer();

	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("GGYGOSetSquadMember: 编队 %d 第 %d 位设为 [%s]，已存盘。下一次进关卡生效。"),
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

	Presets->SaveGameToSlotForLocalPlayer();

	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("GGYGOSetActiveSquadPreset: 出战编队设为 %d，已存盘。下一次进关卡生效。"), PresetIndex);
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

	Presets->SaveGameToSlotForLocalPlayer();

	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("GGYGOClearSquadPresets: 编队已清空并存盘，下一次进关卡会回落到默认编队。"));
}

#else

// Shipping 下保留空实现：声明无法条件编译（UHT 限制），符号必须存在。
void AGGYGOPlayerController::GGYGODumpSquadPresets() {}
void AGGYGOPlayerController::GGYGOSetSquadMember(int32, int32, const FString&) {}
void AGGYGOPlayerController::GGYGOSetActiveSquadPreset(int32) {}
void AGGYGOPlayerController::GGYGOClearSquadPresets() {}

#endif
