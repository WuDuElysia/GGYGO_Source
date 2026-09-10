/**
 * @file GGYGODamageExecution.h
 * @brief 伤害结算
 *
 * 把攻击方的输出属性与防御方的承受属性合成为最终伤害，写入
 * `UGGYGOHealthSet` 的 `Damage` 与 `PoiseDamage` 元属性。
 *
 * ## 为什么用 Execution 而不是普通 Modifier
 * 普通 Modifier 只能做"属性 A 加减一个数"，无法同时读取源与目标的多个属性
 * 再按公式合成。伤害需要 `攻击方输出 × 倍率 - 防御方减免`，
 * 而且倍率与减免各自还要看 Tag（弱点部位、无敌帧、属性克制），
 * 这些只能在 Execution 里做。
 *
 * ## 为什么伤害要经元属性
 * Execution 不直接扣 `Health`，而是写入 `Damage` 元属性，由 HealthSet 的
 * `PostGameplayEffectExecute` 转成扣血。这样免疫判定、钳制、死亡边沿检测
 * 全部集中在 HealthSet 一处 —— 若各个 Execution 各自扣血，
 * 每加一种伤害来源就要重复实现一遍那些规则。
 */
#pragma once

#include "GameplayEffectExecutionCalculation.h"

#include "GGYGODamageExecution.generated.h"

class UObject;

UCLASS()
class GGYGO_API UGGYGODamageExecution : public UGameplayEffectExecutionCalculation
{
	GENERATED_BODY()

public:
	UGGYGODamageExecution();

	virtual void Execute_Implementation(const FGameplayEffectCustomExecutionParameters& ExecutionParams, FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const override;
};
