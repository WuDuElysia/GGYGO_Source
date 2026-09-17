/** @file GGYGOBossDefinition.cpp */
#include "AI/Boss/GGYGOBossDefinition.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOBossDefinition)

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
