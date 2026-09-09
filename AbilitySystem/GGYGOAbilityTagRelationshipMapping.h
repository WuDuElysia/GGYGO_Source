/**
 * @file GGYGOAbilityTagRelationshipMapping.h
 * @brief Ability 之间的 Tag 级阻断 / 取消关系表
 *
 * 解决的问题：Ability 自己的 `BlockAbilitiesWithTag` / `CancelAbilitiesWithTag` 配在 GA 资产上，
 * 意味着"谁打断谁"的规则分散在几十个 GA 里，改一条规则要翻遍所有资产。
 * 本资产把这些关系集中到一处配置。
 *
 * ## 与组优先级仲裁的分工（阶段 3 实现后）
 * 两套机制并存且互补，不要用一套去替代另一套：
 *   - **本表（Tag 关系）**：表达"语义上的互斥"。例如"任何持有 `Ability.Attack` 的能力都阻断 `Ability.Sprint`"。
 *     它不关心谁强谁弱，只关心标签之间的兼容性。
 *   - **组优先级**：表达"同类动作里谁能顶掉谁"。例如大招（Priority 600）打断技能（400）。
 *
 * 判定顺序是先组优先级、后 Tag 关系（见 `UGGYGOAbilitySystemComponent` 的仲裁流程）。
 *
 * ## 性能
 * 三个查询都是线性扫描，没有建索引。条目数量在几十条量级时无所谓，
 * 但**不要在每帧路径上调用**——GAS 只在激活与结束时查询它。
 */
#pragma once

#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"

#include "GGYGOAbilityTagRelationshipMapping.generated.h"

class UObject;

/** 一条关系记录：以一个 Ability Tag 为键，声明四类后果。 */
USTRUCT()
struct FGGYGOAbilityTagRelationship
{
	GENERATED_BODY()

	/**
	 * 关系键。
	 * 前两个查询用 `HasTag` 做**层级匹配**，所以键写 `Ability.Attack` 时，
	 * 持有 `Ability.Attack.Light` 的能力也会命中。
	 */
	UPROPERTY(EditAnywhere, Category = Ability)
	FGameplayTag AbilityTag;

	/** 命中本关系的能力激活后，阻断持有这些 Tag 的其他能力。 */
	UPROPERTY(EditAnywhere, Category = Ability)
	FGameplayTagContainer AbilityTagsToBlock;

	/** 命中本关系的能力激活后，取消持有这些 Tag 的正在运行的能力。 */
	UPROPERTY(EditAnywhere, Category = Ability)
	FGameplayTagContainer AbilityTagsToCancel;

	/** 隐式追加到命中能力的"激活必需 Tag"。缺任一个则激活失败。 */
	UPROPERTY(EditAnywhere, Category = Ability)
	FGameplayTagContainer ActivationRequiredTags;

	/** 隐式追加到命中能力的"激活阻断 Tag"。持有任一个则激活失败。 */
	UPROPERTY(EditAnywhere, Category = Ability)
	FGameplayTagContainer ActivationBlockedTags;
};

UCLASS()
class GGYGO_API UGGYGOAbilityTagRelationshipMapping : public UDataAsset
{
	GENERATED_BODY()

private:
	/** 关系表。按配置顺序线性遍历，一个能力可以同时命中多条关系并累加结果。 */
	UPROPERTY(EditAnywhere, Category = Ability, meta = (TitleProperty = "AbilityTag"))
	TArray<FGGYGOAbilityTagRelationship> AbilityTagRelationships;

public:
	/**
	 * 收集某组 Ability Tag 带来的阻断与取消集合。
	 * @param AbilityTags    待查询能力的 Tag 集合。
	 * @param OutTagsToBlock 输出，可为 nullptr 表示不关心。**追加**写入，不清空。
	 * @param OutTagsToCancel 输出，可为 nullptr。同样是追加。
	 * 只读查询，服务器与客户端可共用同一份映射。
	 */
	void GetAbilityTagsToBlockAndCancel(const FGameplayTagContainer& AbilityTags, FGameplayTagContainer* OutTagsToBlock, FGameplayTagContainer* OutTagsToCancel) const;

	/**
	 * 收集某组 Ability Tag 隐式带来的激活必需 / 阻断 Tag。
	 * 两个输出都是追加语义，与能力自身配置的条件叠加而不是覆盖。
	 */
	void GetRequiredAndBlockedActivationTags(const FGameplayTagContainer& AbilityTags, FGameplayTagContainer* OutActivationRequired, FGameplayTagContainer* OutActivationBlocked) const;

	/**
	 * 判断某个动作 Tag 是否会取消这组能力。
	 * @return 存在关系键**精确等于** ActionTag、且其取消集合与 AbilityTags 有交集时返回 true。
	 * 注意这里用精确相等而不是上面两个查询的层级匹配：取消是强动作，不做层级放宽。
	 */
	bool IsAbilityCancelledByTag(const FGameplayTagContainer& AbilityTags, const FGameplayTag& ActionTag) const;
};
