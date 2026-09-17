/**
 * @file GGYGOCharacterSlot.cpp
 * @brief 队伍位置的角色数据宿主实现
 */
#include "Teams/GGYGOCharacterSlot.h"

#include "AbilitySystem/GGYGOAbilitySet.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "Character/Data/GGYGOPawnData.h"
#include "Net/UnrealNetwork.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOCharacterSlot)

AGGYGOCharacterSlot::AGGYGOCharacterSlot(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// bAlwaysRelevant 而不是 bOnlyRelevantToOwner：
	// 其他客户端需要收到本角色的 GameplayCue 与 GameplayTag（打击特效、状态表现）。
	// 限制为仅所有者相关会让整个 ASC 对旁观者不可见，别人就看不到你的命中反馈。
	// "属性只给自己、GE 与 Cue 给所有人"这件事由下面的 Mixed 复制模式负责，
	// 不该用相关性来做 —— 相关性是全有或全无的。
	bAlwaysRelevant = true;

	// ASC 承载属性与 GE，变更频繁且直接影响表现，需要高更新频率。
	SetNetUpdateFrequency(100.0f);

	// Mixed：属性明细只发给拥有本队伍的客户端，GE 与 Cue 发给所有人。
	// 这是玩家 Slot 的策略；未来 BossState 会在自己的构造函数里改为 Minimal。
	AbilitySystemComponent->SetReplicationMode(EGameplayEffectReplicationMode::Mixed);

}

void AGGYGOCharacterSlot::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AGGYGOCharacterSlot, PawnData);
}

void AGGYGOCharacterSlot::InitializeForPawnData(const UGGYGOPawnData* InPawnData)
{
	if (!HasAuthority())
	{
		return;
	}

	if (!InPawnData)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("InitializeForPawnData: 位置 [%s] 收到空 PawnData，该位置不会有可用角色。"),
			*GetNameSafe(this));
		return;
	}

	if (PawnData)
	{
		// 换角色应当换位置，理由见头文件：已授予的能力与已生效的 Buff
		// 无法干净地对应到另一份 PawnData。
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("InitializeForPawnData: 位置 [%s] 已装载 [%s]，拒绝改为 [%s]。"),
			*GetNameSafe(this), *GetNameSafe(PawnData), *GetNameSafe(InPawnData));
		return;
	}

	PawnData = InPawnData;

	// 组仲裁与 Tag 关系表随角色定义走，注入时机不依赖 Pawn。
	AbilitySystemComponent->SetAbilityGroupConfig(PawnData->AbilityGroupConfig);
	AbilitySystemComponent->SetTagRelationshipMapping(PawnData->TagRelationshipMapping);

	if (bAbilitiesGranted)
	{
		return;
	}

	for (const TObjectPtr<UGGYGOAbilitySet>& AbilitySet : PawnData->AbilitySets)
	{
		if (AbilitySet)
		{
			// 不保存回收句柄：本位置的能力与本位置同生共死，Slot 销毁时 ASC 一起销毁。
			// SourceObject 传 PawnData，让能力能回溯自己的配置来源。
			AbilitySet->GiveToAbilitySystem(
				AbilitySystemComponent, nullptr, const_cast<UGGYGOPawnData*>(PawnData.Get()));
		}
	}

	bAbilitiesGranted = true;
}
