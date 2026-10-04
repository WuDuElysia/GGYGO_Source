#pragma once

#include "AI/Boss/Abilities/GGYGOBossMeleeAbility.h"
#include "GGYGOBossMeleeLifecycleTestAbility.generated.h"

/** 仅隔离激活准备，使用真实 GAS 激活/结束与生产 EndAbility 验证同步重入。 */
UCLASS(Transient)
class UGGYGOBossMeleeLifecycleTestAbility : public UGGYGOBossMeleeAbility
{
	GENERATED_BODY()
public:
	void FinishForTest();
	bool HasActiveMeshForTest() const { return ActiveMesh != nullptr; }
protected:
	virtual void ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
};
