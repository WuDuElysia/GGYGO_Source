/**
 * @file GGYGODeathAbility.h
 * @brief 死亡演出能力：Event.Death 触发，推进 HealthComponent 的两阶段死亡
 */
#pragma once

#include "AbilitySystem/Abilities/GGYGOGameplayAbility.h"

#include "GGYGODeathAbility.generated.h"

class UAnimMontage;
class UGGYGOHealthComponent;

/**
 * 死亡演出能力。
 *
 * HealthComponent 在生命归零时向 ASC 发 `Event.Death`，本能力由该事件触发（服务器发起）。
 * 服务器上激活时取消 Avatar 其余能力（带 `Ability.Behavior.SurvivesDeath` 的除外）、调用
 * `StartDeath`，然后播放 DeathMontage；倒下动作开始淡出、被打断或播放失败时结束能力，
 * 结束清理里调用 `FinishDeath`。死亡状态由 HealthComponent 复制，客户端不在这里推进。
 *
 * DeathMontage 是角色差异，由蓝图子类配置。未配置时记录错误并立即结束死亡：
 * 死亡本身不能因为缺表现而停在 Dying。
 */
UCLASS(Abstract, Blueprintable)
class GGYGO_API UGGYGODeathAbility : public UGGYGOGameplayAbility
{
	GENERATED_BODY()

public:
	UGGYGODeathAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

protected:
	virtual void ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
		FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	virtual void CleanupAbilityResourcesForTermination(const FGGYGOAbilityTerminationContext& Context) override;

	/** 倒下动作。播到开始淡出即视为倒下完成。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Death")
	TObjectPtr<UAnimMontage> DeathMontage;

private:
	/** 本次激活在服务器上开始死亡的 HealthComponent；结束清理时对它 FinishDeath 后清空。 */
	TWeakObjectPtr<UGGYGOHealthComponent> DyingHealth;
};
