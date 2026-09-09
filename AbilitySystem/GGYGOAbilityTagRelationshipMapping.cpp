/**
 * @file GGYGOAbilityTagRelationshipMapping.cpp
 * @brief Tag 关系表实现
 *
 * 三个函数都是纯只读查询，不激活能力、不改 AbilitySpec、不产生网络句柄。
 */
#include "AbilitySystem/GGYGOAbilityTagRelationshipMapping.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOAbilityTagRelationshipMapping)

void UGGYGOAbilityTagRelationshipMapping::GetAbilityTagsToBlockAndCancel(const FGameplayTagContainer& AbilityTags, FGameplayTagContainer* OutTagsToBlock, FGameplayTagContainer* OutTagsToCancel) const
{
	for (const FGGYGOAbilityTagRelationship& Relationship : AbilityTagRelationships)
	{
		// HasTag 是层级匹配：键为 Ability.Attack 时，Ability.Attack.Light 也命中。
		if (AbilityTags.HasTag(Relationship.AbilityTag))
		{
			// 追加而非覆盖，让一个能力的多个 Tag 可以叠加多条关系。
			if (OutTagsToBlock)
			{
				OutTagsToBlock->AppendTags(Relationship.AbilityTagsToBlock);
			}

			if (OutTagsToCancel)
			{
				OutTagsToCancel->AppendTags(Relationship.AbilityTagsToCancel);
			}
		}
	}
}

void UGGYGOAbilityTagRelationshipMapping::GetRequiredAndBlockedActivationTags(const FGameplayTagContainer& AbilityTags, FGameplayTagContainer* OutActivationRequired, FGameplayTagContainer* OutActivationBlocked) const
{
	for (const FGGYGOAbilityTagRelationship& Relationship : AbilityTagRelationships)
	{
		if (AbilityTags.HasTag(Relationship.AbilityTag))
		{
			if (OutActivationRequired)
			{
				OutActivationRequired->AppendTags(Relationship.ActivationRequiredTags);
			}

			if (OutActivationBlocked)
			{
				OutActivationBlocked->AppendTags(Relationship.ActivationBlockedTags);
			}
		}
	}
}

bool UGGYGOAbilityTagRelationshipMapping::IsAbilityCancelledByTag(const FGameplayTagContainer& AbilityTags, const FGameplayTag& ActionTag) const
{
	for (const FGGYGOAbilityTagRelationship& Relationship : AbilityTagRelationships)
	{
		// 这里刻意用精确相等，而不是上面两个查询的层级 HasTag。
		// 取消是不可逆的强动作，用层级匹配容易误伤（配了 Ability.Attack 却取消了所有子类攻击）。
		if (Relationship.AbilityTag == ActionTag && Relationship.AbilityTagsToCancel.HasAny(AbilityTags))
		{
			return true;
		}
	}

	return false;
}
