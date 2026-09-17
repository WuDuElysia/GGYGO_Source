/** @file GGYGOBossState.cpp */
#include "AI/Boss/GGYGOBossState.h"

#include "AI/Boss/GGYGOBossDefinition.h"
#include "AbilitySystem/GGYGOAbilitySet.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "Character/Data/GGYGOPawnData.h"
#include "Net/UnrealNetwork.h"
#include "System/GGYGOGameplayTags.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOBossState)

AGGYGOBossState::AGGYGOBossState(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bAlwaysRelevant = true;
	SetNetUpdateFrequency(100.0f);

	// Boss 没有拥有客户端，不做本地预测；只复制观察者需要的属性、Tag 与 Cue。
	AbilitySystemComponent->SetReplicationMode(EGameplayEffectReplicationMode::Minimal);
}

bool AGGYGOBossState::InitializeFromDefinition(const UGGYGOBossDefinition* InDefinition)
{
	if (!HasAuthority() || !InDefinition)
	{
		return false;
	}

	if (BossDefinition)
	{
		if (BossDefinition != InDefinition)
		{
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("InitializeFromDefinition: BossState [%s] 已绑定 [%s]，拒绝改为 [%s]。"),
				*GetNameSafe(this), *GetNameSafe(BossDefinition), *GetNameSafe(InDefinition));
		}
		return BossDefinition == InDefinition;
	}

	const FGGYGOBossFormDefinition* InitialForm = InDefinition->FindForm(InDefinition->InitialFormTag);
	const FGGYGOBossPhaseDefinition* InitialPhase = InDefinition->FindPhase(InDefinition->InitialPhaseTag);
	if (!InitialForm || !InitialForm->AvatarPawnData || !InitialPhase)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("InitializeFromDefinition: 定义 [%s] 的初始 Form/Phase 或 PawnData 无效。"),
			*GetNameSafe(InDefinition));
		return false;
	}

	if (!InDefinition->InitialFormTag.MatchesTag(GGYGOGameplayTags::State_Boss_Form) ||
		!InDefinition->InitialPhaseTag.MatchesTag(GGYGOGameplayTags::State_Boss_Phase))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("InitializeFromDefinition: [%s] 的初始标签必须位于 State.Boss.Form / State.Boss.Phase 下。"),
			*GetNameSafe(InDefinition));
		return false;
	}

	BossDefinition = InDefinition;

	// 组规则与 Tag 关系由初始形态 PawnData 提供。阶段 E 若允许形态间不同，需先定义合并规则。
	AbilitySystemComponent->SetAbilityGroupConfig(InitialForm->AvatarPawnData->AbilityGroupConfig);
	AbilitySystemComponent->SetTagRelationshipMapping(InitialForm->AvatarPawnData->TagRelationshipMapping);

	TSet<const UGGYGOAbilitySet*> GrantedSets;
	auto GrantSetOnce = [this, InDefinition, &GrantedSets](const UGGYGOAbilitySet* AbilitySet)
	{
		if (AbilitySet && !GrantedSets.Contains(AbilitySet))
		{
			GrantedSets.Add(AbilitySet);
			AbilitySet->GiveToAbilitySystem(
				AbilitySystemComponent, nullptr, const_cast<UGGYGOBossDefinition*>(InDefinition));
		}
	};

	for (const TObjectPtr<UGGYGOAbilitySet>& AbilitySet : InDefinition->PersistentAbilitySets)
	{
		GrantSetOnce(AbilitySet);
	}
	for (const FGGYGOBossFormDefinition& Form : InDefinition->Forms)
	{
		if (Form.AvatarPawnData)
		{
			for (const TObjectPtr<UGGYGOAbilitySet>& AbilitySet : Form.AvatarPawnData->AbilitySets)
			{
				GrantSetOnce(AbilitySet);
			}
		}
	}

	SetReplicatedStateTag(CurrentFormTag, InDefinition->InitialFormTag);
	SetReplicatedStateTag(CurrentPhaseTag, InDefinition->InitialPhaseTag);
	ForceNetUpdate();
	return true;
}

const UGGYGOPawnData* AGGYGOBossState::GetInitialPawnData() const
{
	if (BossDefinition)
	{
		if (const FGGYGOBossFormDefinition* Form = BossDefinition->FindForm(BossDefinition->InitialFormTag))
		{
			return Form->AvatarPawnData;
		}
	}
	return nullptr;
}

void AGGYGOBossState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AGGYGOBossState, BossDefinition);
	DOREPLIFETIME(AGGYGOBossState, CurrentFormTag);
	DOREPLIFETIME(AGGYGOBossState, CurrentPhaseTag);
}

void AGGYGOBossState::SetReplicatedStateTag(FGameplayTag& CurrentTag, FGameplayTag NewTag)
{
	if (CurrentTag == NewTag)
	{
		return;
	}

	if (CurrentTag.IsValid())
	{
		AbilitySystemComponent->SetLooseGameplayTagCount(
			CurrentTag, 0, EGameplayTagReplicationState::TagAndCountToAll);
	}
	CurrentTag = NewTag;
	if (CurrentTag.IsValid())
	{
		AbilitySystemComponent->SetLooseGameplayTagCount(
			CurrentTag, 1, EGameplayTagReplicationState::TagAndCountToAll);
	}
}
