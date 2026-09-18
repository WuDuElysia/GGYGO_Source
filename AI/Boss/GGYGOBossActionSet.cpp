/** @file GGYGOBossActionSet.cpp */
#include "AI/Boss/GGYGOBossActionSet.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOBossActionSet)

const FGGYGOBossActionDefinition* UGGYGOBossActionSet::FindAction(FGameplayTag ActionTag) const
{
	return Actions.FindByPredicate(
		[ActionTag](const FGGYGOBossActionDefinition& Action) { return Action.ActionTag == ActionTag; });
}
