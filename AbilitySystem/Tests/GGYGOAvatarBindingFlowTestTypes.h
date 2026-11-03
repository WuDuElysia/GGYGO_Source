#pragma once

#include "AbilitySystem/Abilities/GGYGOGameplayAbility.h"

#include "GGYGOAvatarBindingFlowTestTypes.generated.h"

/** 激活后保持运行，直到被取消；记录清理次数。服务器执行，测试世界有权威。 */
UCLASS(Transient)
class UGGYGOAvatarBindingHeldTestAbility : public UGGYGOGameplayAbility
{
	GENERATED_BODY()

public:
	UGGYGOAvatarBindingHeldTestAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	int32 GetCleanupCallsForTest() const { return CleanupCalls; }

protected:
	virtual void ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
		FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	virtual void CleanupAbilityResourcesForTermination(const FGGYGOAbilityTerminationContext& Context) override;

private:
	int32 CleanupCalls = 0;
};

/** 带 Ability.Behavior.SurvivesDeath 资产标签：释放 Avatar 时不被取消。 */
UCLASS(Transient)
class UGGYGOAvatarBindingSurvivingTestAbility : public UGGYGOAvatarBindingHeldTestAbility
{
	GENERATED_BODY()

public:
	UGGYGOAvatarBindingSurvivingTestAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};
