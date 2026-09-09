/**
 * @file GGYGOAbilityGroupConfig.h
 * @brief 组并发规则配置表
 *
 * 把「某一组能力内部怎么并发」这件事从 GA 资产里抽出来，集中到一份 DataAsset。
 *
 * ## 为什么规则配在组上而不是 GA 上
 * 「技能组同时只能激活一个」是**组的属性**，不是某个技能的属性。
 * 如果配在 GA 上，加一个新技能就要记得把这条规则再抄一遍，改规则要翻遍所有技能资产。
 * 配在组上则只有一处。
 *
 * 留在 GA 上的是**个体差异**：属于哪个组（`GroupTag`）、多强（`ActivationPriority`）、
 * 是否压制其它组（`SelfPolicy`）。这三项每个能力都不同，配在 GA 上是对的。
 *
 * ## 注入方式
 * 由 `UGGYGOAbilitySystemComponent::SetAbilityGroupConfig` 注入，
 * 未注入时 ASC 降级为内置默认规则（见该函数注释）。
 * 阶段 4 之后应由 `UGGYGOPawnData` 携带并在初始化时设置。
 */
#pragma once

#include "AbilitySystem/Groups/GGYGOAbilityGroupTypes.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"

#include "GGYGOAbilityGroupConfig.generated.h"

class UObject;

UCLASS(BlueprintType, Const)
class GGYGO_API UGGYGOAbilityGroupConfig : public UDataAsset
{
	GENERATED_BODY()

public:
	UGGYGOAbilityGroupConfig(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/**
	 * 查某个组的规则。
	 * @param GroupTag 组身份。传无效 Tag 或表中没有该组时返回 `DefaultRule`。
	 * @return 规则的常引用，生命周期同本资产。
	 *
	 * 查不到时返回默认规则而不是报错：这样新增一个组不必先来这里登记，
	 * 只有需要偏离默认行为时才配置。
	 */
	const FGGYGOAbilityGroupRule& GetRuleForGroup(FGameplayTag GroupTag) const;

	/** 该组是否显式配置过（区别于走默认规则）。诊断用。 */
	bool HasExplicitRule(FGameplayTag GroupTag) const;

protected:
	/**
	 * 各组的规则。键取 `AbilityGroup.*`。
	 *
	 * 建议配置：
	 * - `AbilityGroup.Attack` → `SingleInstanceQueued`（连段排队，不互相打断）
	 * - `AbilityGroup.Skill` / `.Ultimate` → `SingleInstance`
	 * - `AbilityGroup.HitReact` → `SingleInstance` 且 `bNewcomerWinsOnTie = false`
	 *   （同级受击不该反复重播，先到先得）
	 * - `AbilityGroup.Passive` → `Coexist`
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Ability Groups", meta = (Categories = "AbilityGroup", ForceInlineRow))
	TMap<FGameplayTag, FGGYGOAbilityGroupRule> GroupRules;

	/**
	 * 未在上表中列出的组使用的规则。
	 *
	 * 默认值刻意设成 `SingleInstance` + `bNewcomerWinsOnTie = true`（决策 D4），
	 * 而不是 `Coexist`：漏配一个组时，宁可表现为"动作互相打断"（明显、好排查），
	 * 也不要表现为"所有动作同时播放"（画面混乱且难定位原因）。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Ability Groups")
	FGGYGOAbilityGroupRule DefaultRule;
};
