/**
 * @file GGYGODamageExecution.h
 * @brief 按显式输入模式计算伤害与削韧，输出 HealthSet 元属性。
 *
 * GE 的 Executions[].PassedInTags 含 DamageCalculation.AttackPowerMultiplier 时，
 * 必须提供有限非负的 SetByCaller.DamageMultiplier，伤害为源 CombatSet.BaseDamage
 * 的原生 Spec 快照乘该倍率，再应用能力来源衰减。该模式拒绝绝对伤害键。
 *
 * 未选择倍率模式时，保留 SetByCaller.Damage 绝对伤害或缺键时的源属性捕获；
 * 自毁继续使用绝对值。倍率键出现在未选择的模式、与绝对键冲突、缺失或非法时，
 * 本 Execution 明确诊断且不新增输出。原生 Execute 不提供 GE 应用失败返回值。
 *
 * 削韧继续使用独立的 SetByCaller.PoiseDamage 或源属性捕获。
 * Execution 只输出 Damage / PoiseDamage 元属性，HealthSet 负责业务边界与结算。
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
