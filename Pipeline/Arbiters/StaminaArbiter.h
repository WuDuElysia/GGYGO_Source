/**
 * @file StaminaArbiter.h
 * @brief 体力仲裁器
 *
 * 根据运动档位消耗或恢复体力。体力枯竭 → 阻止 Sprint。
 *
 * 滞后设计：
 * 枯竭后不是体力 > 0 就能重新冲刺，而是要恢复到 MaxStamina × StaminaRecoverThreshold
 * （比如 20%）才解除枯竭。防止玩家在体力边缘反复切换冲刺。
 */
#pragma once

#include "Pipeline/Interfaces/IArbiter.h"

class UAbilitySystemComponent;

class FStaminaArbiter : public IArbiter
{
public:
	/**
	 * 注入 ASC 指针（读取 Stamina 属性）
	 * @param InASC AbilitySystemComponent
	 */
	void Init(UAbilitySystemComponent* InASC);

	virtual void Arbitrate(FRuntimeData& RuntimeData, float DeltaTime) override;

private:
	/** ASC 指针（不拥有） */
	UAbilitySystemComponent* ASC = nullptr;

	/** 体力是否枯竭（滞后标记，枯竭后需要恢复到阈值才解除） */
	bool bIsStaminaDepleted = false;

	/** 体力恢复阈值（枯竭后需恢复到此比例才解除，默认 20%） */
	float StaminaRecoverThreshold = 0.2f;

	/** Sprint 消耗速率（每秒） */
	float StaminaDrainRate = 20.f;

	/** 体力恢复速率（每秒，未 Sprint 时自动恢复） */
	float StaminaRegenRate = 10.f;
};
