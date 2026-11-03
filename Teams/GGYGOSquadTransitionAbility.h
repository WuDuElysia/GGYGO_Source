/**
 * @file GGYGOSquadTransitionAbility.h
 * @brief 队员进场／退场能力基类
 */
#pragma once

#include "AbilitySystem/Abilities/GGYGOGameplayAbility.h"

#include "GGYGOSquadTransitionAbility.generated.h"

class UAnimMontage;

/**
 * 进场／退场能力基类。
 *
 * 子类把 AbilityTriggers 配成 GameplayEvent `Event.Squad.SwitchIn` 或 `Event.Squad.SwitchOut`，
 * 并配置 TransitionMontage。激活后播放 Montage，自然播完时正常结束，被打断、取消或播放失败时
 * 以取消结束。Squad 只在退场能力结束后隐藏角色，自己不设计时器：时长、能否被打断、进场位置
 * 都由 Montage 与子类配置决定。
 *
 * 蓝图子类实现了 ActivateAbility 事件时由蓝图完全接管，本类不播放 Montage，蓝图负责结束能力。
 *
 * Squad 在服务器上发事件，换下的队员没有本地控制，因此默认 ServerInitiated：
 * LocalPredicted 能力在非本地控制的 Avatar 上会被 GAS 以网络原因拒绝。
 */
UCLASS(Abstract, Blueprintable)
class GGYGO_API UGGYGOSquadTransitionAbility : public UGGYGOGameplayAbility
{
	GENERATED_BODY()

public:
	UGGYGOSquadTransitionAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

protected:
	virtual void ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
		FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;

	/** 进场或退场动作。未配置且蓝图未接管时，能力记录错误并以取消结束，Squad 按无动作继续。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Squad")
	TObjectPtr<UAnimMontage> TransitionMontage;
};
