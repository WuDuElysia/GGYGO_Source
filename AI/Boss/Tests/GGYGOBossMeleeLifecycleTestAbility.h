/** @file GGYGOBossMeleeLifecycleTestAbility.h @brief Test access to original melee resources and native playback. */
#pragma once

#include "AI/Boss/Abilities/GGYGOBossMeleeAbility.h"
#include "Animation/Runtime/GGYGOMontageGuardAnimInstance.h"
#include "GGYGOBossMeleeLifecycleTestAbility.generated.h"

/** 可隔离 Mesh/Profile 生命周期，或通过完整生产 Montage Body 验证真实 XYZ 链。 */
UCLASS(Transient)
class UGGYGOBossMeleeLifecycleTestAbility : public UGGYGOBossMeleeAbility
{
	GENERATED_BODY()
public:
	void FinishForTest(const FGGYGOAbilityActivationHandle& Original);
	/** 原生 ScopeLock 内对 Original 请求结束，并在其之前排队“结束原激活后立即重激活”；返回解锁后的后继。 */
	FGGYGOAbilityActivationHandle DeferredEndAfterReactivationForTest(const FGGYGOAbilityActivationHandle& Original,
		bool& bAcceptedWhileLocked);
	bool BeginMotionForTest(const FGGYGOAbilityActivationHandle& Original, const UGGYGOActionMotionProfile* Profile);
	void ConfigureMontageForTest(UAnimMontage* Montage, FName StartBone, FName EndBone);
	void SetProfileForTest(const UGGYGOActionMotionProfile* Profile) { ActionMotionProfile = Profile; }
	int32 GetActiveTaskCountForTest() const { return ActiveTasks.Num(); }
	bool HasActiveMeshForTest() const { return HasOriginalMeleeMeshResource(); }
	FGGYGOAbilityActivationHandle GetInitializedOriginalForTest() const { return GetOriginalMeleeResourceActivation(); }
	/** 最近一次正式结束清理收到的参数；原生结束广播时读取，用于把结束通知对应到那次激活。 */
	const FGGYGOAbilityTerminationContext& GetLastTerminationForTest() const { return LastTermination; }
protected:
	virtual void CleanupAbilityResourcesForTermination(const FGGYGOAbilityTerminationContext& Context) override;
	virtual void ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
		FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
private:
	FGGYGOAbilityTerminationContext LastTermination;
};

/** Concrete ordinary-slot Guard; native playback and original-instance callbacks remain real. */
UCLASS(Transient)
class UGGYGOBossMeleeLifecycleTestAnimInstance : public UGGYGOMontageGuardAnimInstance
{
	GENERATED_BODY()
};
