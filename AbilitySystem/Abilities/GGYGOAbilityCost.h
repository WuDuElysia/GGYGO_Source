/**
 * @file GGYGOAbilityCost.h
 * @brief 可插拔的 Ability 额外消耗
 *
 * GAS 原生的 Cost 只支持"一个 GE 扣一个属性"。动作游戏经常需要更复杂的消耗：
 * 耐力、能量、连段计数、弹药、充能次数，而且有的消耗只在命中后才扣。
 *
 * 这个基类把消耗抽象成可在 GA 资产上内嵌配置的对象数组
 * （`UGGYGOGameplayAbility::AdditionalCosts`），一个 GA 可以挂多个不同类型的消耗。
 *
 * 派生类只需实现两个函数，不用关心：
 *   - `Ability` 和 `ActorInfo` 进入时保证非空
 *   - `ShouldOnlyApplyCostOnHit()` 的判定由调用方统一处理，实现里不用再查命中
 *
 * 实际的资源扣除必须是服务器权威行为。
 */
#pragma once

#include "Abilities/GameplayAbility.h"
#include "GameplayAbilitySpec.h"

#include "GGYGOAbilityCost.generated.h"

class UGGYGOGameplayAbility;

UCLASS(DefaultToInstanced, EditInlineNew, Abstract)
class GGYGO_API UGGYGOAbilityCost : public UObject
{
	GENERATED_BODY()

public:
	UGGYGOAbilityCost()
	{
	}

	/**
	 * 判断是否付得起这份消耗。在能力激活前调用。
	 *
	 * @param Ability             发起检查的能力，保证非空。
	 * @param Handle              被检查的能力规格句柄。
	 * @param ActorInfo           角色上下文，保证非空。
	 * @param OptionalRelevantTags 可为 nullptr。失败时往里写原因 Tag，
	 *                            上层可据此给出反馈（例如耐力不足时播一声闷响）。
	 * @return true 表示付得起。返回 false 会阻止激活。
	 */
	virtual bool CheckCost(const UGGYGOGameplayAbility* Ability, const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, FGameplayTagContainer* OptionalRelevantTags) const
	{
		// 基类不掌握具体资源，默认认为付得起。
		return true;
	}

	/**
	 * 实际扣除消耗。在基础 Cost 通过之后调用。
	 * 派生类应在此改变服务器权威的资源状态。
	 */
	virtual void ApplyCost(const UGGYGOGameplayAbility* Ability, const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo)
	{
		// 基类没有可扣的资源。
	}

	/** 这份消耗是否只在能力实际命中目标后才扣。 */
	bool ShouldOnlyApplyCostOnHit() const { return bOnlyApplyCostOnHit; }

protected:
	/**
	 * 为 true 时，只有服务器确认本次能力命中了目标才扣除。
	 * 典型用途：连段计数只在打中时才推进，空挥不消耗。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Costs)
	bool bOnlyApplyCostOnHit = false;
};
