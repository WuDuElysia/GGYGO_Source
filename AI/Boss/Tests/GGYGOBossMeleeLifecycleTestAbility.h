#pragma once

#include "AI/Boss/Abilities/GGYGOBossMeleeAbility.h"
#include "GGYGOBossMeleeLifecycleTestAbility.generated.h"

/** 仅隔离 Montage 业务，使用原请求及生产 Mesh/Cleanup hook 验证原结束同步重入。 */
UCLASS(Transient)
class UGGYGOBossMeleeLifecycleTestAbility : public UGGYGOBossMeleeAbility
{
	GENERATED_BODY()
public:
	void FinishForTest(const FGGYGOAbilityActivationHandle& Original);
	bool HasActiveMeshForTest() const { return HasOriginalMeleeMeshResource(); }
	FGGYGOAbilityActivationHandle GetInitializedOriginalForTest() const { return GetOriginalMeleeResourceActivation(); }
protected:
	virtual void ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
		FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
};
