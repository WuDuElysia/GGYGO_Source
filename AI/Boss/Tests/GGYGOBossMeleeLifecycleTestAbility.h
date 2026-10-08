#pragma once

#include "AI/Boss/Abilities/GGYGOBossMeleeAbility.h"
#include "GGYGOBossMeleeLifecycleTestAbility.generated.h"

/** 隔离 Montage 业务，复用生产 Mesh/Motion/Cleanup 与原 GAS 请求。 */
UCLASS(Transient)
class UGGYGOBossMeleeLifecycleTestAbility : public UGGYGOBossMeleeAbility
{
	GENERATED_BODY()
public:
	void FinishForTest(const FGGYGOAbilityActivationHandle& Original);
	bool BeginMotionForTest(const FGGYGOAbilityActivationHandle& Original, const UGGYGOActionMotionProfile* Profile);
	bool HasActiveMeshForTest() const { return HasOriginalMeleeMeshResource(); }
	FGGYGOAbilityActivationHandle GetInitializedOriginalForTest() const { return GetOriginalMeleeResourceActivation(); }
protected:
	virtual void ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
		FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
};
