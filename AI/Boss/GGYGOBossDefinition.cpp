/** @file GGYGOBossDefinition.cpp */
#include "AI/Boss/GGYGOBossDefinition.h"

#include "AI/Boss/GGYGOBossState.h"
#include "System/GGYGOGameplayTags.h"
#include "BehaviorTree/BlackboardData.h"
#include "Misc/DataValidation.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOBossDefinition)

UGGYGOBossDefinition::UGGYGOBossDefinition(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	BossStateClass = AGGYGOBossState::StaticClass();
}

const FGGYGOBossFormDefinition* UGGYGOBossDefinition::FindForm(FGameplayTag FormTag) const
{
	return Forms.FindByPredicate(
		[FormTag](const FGGYGOBossFormDefinition& Form) { return Form.FormTag == FormTag; });
}

const FGGYGOBossPhaseDefinition* UGGYGOBossDefinition::FindPhase(FGameplayTag PhaseTag) const
{
	return Phases.FindByPredicate(
		[PhaseTag](const FGGYGOBossPhaseDefinition& Phase) { return Phase.PhaseTag == PhaseTag; });
}

bool UGGYGOBossDefinition::ValidateCombatConfiguration(FString& OutError) const
{
	OutError.Reset();
	if (BehaviorTree && (CombatTargetKey.IsNone() || CombatTargetKey == TEXT("SelectedAction") || CombatTargetKey == FBlackboard::KeySelf
		|| !FMath::IsFinite(TargetAcquireDistance)
		|| TargetAcquireDistance <= 0.0f || !FMath::IsFinite(TargetLoseDistance)
		|| TargetLoseDistance < TargetAcquireDistance || !FMath::IsFinite(ApproachDistance)
		|| ApproachDistance < 0.0f || ApproachDistance >= TargetAcquireDistance))
	{
		OutError = TEXT("战斗树需要目标 Key、有限正获取距离、不小于获取距离的丢失距离，以及获取范围内的非负接近距离。");
		return false;
	}
	TSet<FGameplayTag> SeenPhases;
	for (int32 Index = 0; Index < Phases.Num(); ++Index)
	{
		const FGGYGOBossPhaseDefinition& Phase = Phases[Index];
		if (!Phase.PhaseTag.IsValid() || Phase.PhaseTag == GGYGOGameplayTags::State_Boss_Phase
			|| !Phase.PhaseTag.MatchesTag(GGYGOGameplayTags::State_Boss_Phase)
			|| SeenPhases.Contains(Phase.PhaseTag))
		{
			OutError = FString::Printf(TEXT("Phases[%d] 需要唯一的 State.Boss.Phase 子标签。"), Index);
			return false;
		}
		SeenPhases.Add(Phase.PhaseTag);
		if (!FMath::IsFinite(Phase.EnterHealthThreshold) || Phase.EnterHealthThreshold < 0.0f || Phase.EnterHealthThreshold > 1.0f)
		{
			OutError = FString::Printf(TEXT("Phases[%d] 的生命阈值必须为0到1的有限数；具体推进条件由蓝图消费。"), Index);
			return false;
		}
	}
	return true;
}

#if WITH_EDITOR
EDataValidationResult UGGYGOBossDefinition::IsDataValid(FDataValidationContext& Context) const
{
	const EDataValidationResult ParentResult = Super::IsDataValid(Context);
	FString Error;
	if (!ValidateCombatConfiguration(Error))
	{
		Context.AddError(FText::FromString(Error));
		return EDataValidationResult::Invalid;
	}
	return ParentResult == EDataValidationResult::Invalid ? ParentResult : EDataValidationResult::Valid;
}
#endif
