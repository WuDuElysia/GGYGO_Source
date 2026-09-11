/**
 * @file GGYGOSquadPresets.cpp
 * @brief 编队预设集合实现
 */
#include "Teams/GGYGOSquadPresets.h"

#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "Character/Data/GGYGOPawnData.h"
#include "Engine/AssetManager.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"
#include "Player/GGYGOLocalPlayer.h"
#include "Teams/GGYGOSquadTypes.h"

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

UGGYGOSquadPresets* UGGYGOSquadPresets::LoadOrCreateForLocalPlayer(const ULocalPlayer* LocalPlayer)
{
	if (!LocalPlayer)
	{
		return nullptr;
	}

	return Cast<UGGYGOSquadPresets>(
		ULocalPlayerSaveGame::LoadOrCreateSaveGameForLocalPlayer(
			UGGYGOSquadPresets::StaticClass(), LocalPlayer, SaveSlotName));
}

UGGYGOSquadPresets* UGGYGOSquadPresets::GetForPlayerController(const APlayerController* PlayerController)
{
	if (!PlayerController)
	{
		return nullptr;
	}

	const ULocalPlayer* LocalPlayer = PlayerController->GetLocalPlayer();
	if (!LocalPlayer)
	{
		// 服务器上远程玩家的 Controller 没有本地玩家。他们的编队存在自己的
		// 磁盘上，服务器读不到，只能靠客户端上报。正常路径，不报错。
		return nullptr;
	}

	const UGGYGOLocalPlayer* GGYGOLocalPlayer = Cast<UGGYGOLocalPlayer>(LocalPlayer);
	if (!GGYGOLocalPlayer)
	{
		// 有本地玩家但类型不对：`DefaultEngine.ini` 的 `LocalPlayerClassName`
		// 没指向 GGYGOLocalPlayer。缺了缓存点，各调用方会各持一份副本互相覆盖，
		// 与其静默退化成每次读盘，不如把配置问题报出来。
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("本地玩家不是 GGYGOLocalPlayer，编队预设不可用。"
				 "请检查 DefaultEngine.ini 的 [/Script/Engine.Engine] 段里的 LocalPlayerClassName。"));
		return nullptr;
	}

	return GGYGOLocalPlayer->GetSquadPresets();
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

	FGGYGOSquadPreset& Preset = Presets[PresetIndex];
	Preset.Members = NewMembers;

	if (Preset.Members.Num() > GGYGO_MAX_SQUAD_SIZE)
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning,
			TEXT("SetPresetMembers: 编队 [%d] 有 %d 名成员，超过上限 %d，多出的部分被截断。"),
			PresetIndex, Preset.Members.Num(), GGYGO_MAX_SQUAD_SIZE);

		Preset.Members.SetNum(GGYGO_MAX_SQUAD_SIZE);
	}

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

int32 UGGYGOSquadPresets::ResolveActivePresetRoster(TArray<UGGYGOPawnData*>& OutRoster) const
{
	return ResolvePresetRoster(ActivePresetIndex, OutRoster);
}

int32 UGGYGOSquadPresets::ResolvePresetRoster(int32 PresetIndex, TArray<UGGYGOPawnData*>& OutRoster) const
{
	OutRoster.Reset();

	const FGGYGOSquadPreset* Preset = GetPreset(PresetIndex);
	if (!Preset)
	{
		return 0;
	}

	UAssetManager& AssetManager = UAssetManager::Get();

	for (const FPrimaryAssetId& Member : Preset->Members)
	{
		// 空 Id 是面板上的空位，不是错误。
		if (!Member.IsValid())
		{
			continue;
		}

		const FSoftObjectPath MemberPath = AssetManager.GetPrimaryAssetPath(Member);
		if (MemberPath.IsNull())
		{
			// Id 有效但索引里找不到：角色资产被删，或 GGYGOPawnData 没注册
			// 成 PrimaryAssetType（见 DefaultGame.ini 的 PrimaryAssetTypesToScan）。
			UE_LOG(LogGGYGOAbilitySystem, Warning,
				TEXT("ResolvePresetRoster: 编队 [%d] 的成员 [%s] 在资产索引里不存在，已跳过。"),
				PresetIndex, *Member.ToString());
			continue;
		}

		if (UGGYGOPawnData* PawnData = Cast<UGGYGOPawnData>(MemberPath.TryLoad()))
		{
			OutRoster.Add(PawnData);
		}
		else
		{
			UE_LOG(LogGGYGOAbilitySystem, Warning,
				TEXT("ResolvePresetRoster: 编队 [%d] 的成员 [%s] 加载失败或类型不是 GGYGOPawnData，已跳过。"),
				PresetIndex, *Member.ToString());
		}
	}

	return OutRoster.Num();
}

int32 UGGYGOSquadPresets::GetLatestDataVersion() const
{
	// 改动 Presets 的字段含义时递增，并在 HandlePostLoad 里迁移旧数据。
	return 1;
}

void UGGYGOSquadPresets::HandlePostLoad()
{
	Super::HandlePostLoad();

	// 存档可能来自旧版本或被外部改坏：出战序号越界会让 GetActivePreset
	// 一直返回空，表现为"选了编队但进关卡还是默认阵容"。这里收敛到有效范围。
	if (Presets.IsEmpty())
	{
		ActivePresetIndex = INDEX_NONE;
	}
	else if (!Presets.IsValidIndex(ActivePresetIndex))
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning,
			TEXT("编队存档的出战序号 [%d] 越界（共 %d 套），已改为 0。"),
			ActivePresetIndex, Presets.Num());

		ActivePresetIndex = 0;
	}

	// 每套编队的人数上限可能因为上限常量调小而失效，截断避免装配阶段才报错。
	for (int32 PresetIndex = 0; PresetIndex < Presets.Num(); ++PresetIndex)
	{
		FGGYGOSquadPreset& Preset = Presets[PresetIndex];
		if (Preset.Members.Num() > GGYGO_MAX_SQUAD_SIZE)
		{
			UE_LOG(LogGGYGOAbilitySystem, Warning,
				TEXT("编队存档 [%d] 有 %d 名成员，超过上限 %d，已截断。"),
				PresetIndex, Preset.Members.Num(), GGYGO_MAX_SQUAD_SIZE);

			Preset.Members.SetNum(GGYGO_MAX_SQUAD_SIZE);
		}
	}
}
