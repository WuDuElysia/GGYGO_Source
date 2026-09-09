/**
 * @file GGYGOAbilityGroupConfig.cpp
 * @brief 组并发规则配置表实现
 */
#include "AbilitySystem/Groups/GGYGOAbilityGroupConfig.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOAbilityGroupConfig)

UGGYGOAbilityGroupConfig::UGGYGOAbilityGroupConfig(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// DefaultRule 的字段默认值由 FGGYGOAbilityGroupRule 自身给出
	// （SingleInstance + bNewcomerWinsOnTie = true），这里不重复设置。
}

const FGGYGOAbilityGroupRule& UGGYGOAbilityGroupConfig::GetRuleForGroup(FGameplayTag GroupTag) const
{
	if (GroupTag.IsValid())
	{
		// 精确匹配，不做层级回退。
		// 组身份是离散标签，`AbilityGroup.Skill.Fire` 不应该自动继承
		// `AbilityGroup.Skill` 的并发规则 —— 那会让配置的实际生效范围变得难以预料。
		if (const FGGYGOAbilityGroupRule* Found = GroupRules.Find(GroupTag))
		{
			return *Found;
		}
	}

	return DefaultRule;
}

bool UGGYGOAbilityGroupConfig::HasExplicitRule(FGameplayTag GroupTag) const
{
	return GroupTag.IsValid() && GroupRules.Contains(GroupTag);
}
