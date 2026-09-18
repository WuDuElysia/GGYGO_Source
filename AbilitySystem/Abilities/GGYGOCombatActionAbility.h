/** @file GGYGOCombatActionAbility.h @brief AI 和玩家都可请求的语义战斗动作 */
#pragma once

#include "AbilitySystem/Abilities/GGYGOGameplayAbility.h"

#include "GGYGOCombatActionAbility.generated.h"

/** 把 AI 的 ActionTag 与真正的 GAS Ability 关联，不引入第二套执行状态机。 */
UCLASS(Abstract, Blueprintable)
class GGYGO_API UGGYGOCombatActionAbility : public UGGYGOGameplayAbility
{
	GENERATED_BODY()

public:
	UGGYGOCombatActionAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	FGameplayTag GetActionTag() const { return ActionTag; }

protected:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Combat Action", meta = (Categories = "BossAction"))
	FGameplayTag ActionTag;
};
