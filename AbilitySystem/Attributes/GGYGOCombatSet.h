/**
 * @file GGYGOCombatSet.h
 * @brief 输出侧属性集 —— Execution 的源端输入
 *
 * 这里的属性是**持久的基础输出值**，由 ExecutionCalculation 在源侧捕获，
 * 叠加倍率、衰减、防御等因素后，把结果写进目标 `UGGYGOHealthSet` 的一次性元属性。
 *
 * 不要和 HealthSet 的 `Damage` / `Healing` / `PoiseDamage` 混淆：
 *   - 本类的 `BaseDamage`：源角色"基础伤害是多少"，长期存在，可被 Buff 修改
 *   - HealthSet 的 `Damage`：目标"这一次要掉多少血"，一次性，消费后清零
 *
 * 复制条件是 `COND_OwnerOnly`：别人的攻击力数值对本地客户端没有用处，
 * 伤害结算在服务器完成，客户端只需要知道结果。
 *
 * 当前只有三个基础输出值。攻击力、防御、暴击率等派生属性等
 * `GGYGODamageExecution` 落地、伤害公式确定后再按实际需要添加 ——
 * 提前加只会造出没有读取方的字段，而属性一旦被蓝图 GE 引用就很难再改。
 */
#pragma once

#include "AbilitySystemComponent.h"
#include "AbilitySystem/Attributes/GGYGOAttributeSetBase.h"

#include "GGYGOCombatSet.generated.h"

class UObject;
struct FFrame;

UCLASS(BlueprintType)
class GGYGO_API UGGYGOCombatSet : public UGGYGOAttributeSetBase
{
	GENERATED_BODY()

public:
	UGGYGOCombatSet();

	/** 基础伤害。DamageExecution 捕获后计算最终伤害。 */
	ATTRIBUTE_ACCESSORS(UGGYGOCombatSet, BaseDamage);
	/** 基础治疗量。HealExecution 捕获。 */
	ATTRIBUTE_ACCESSORS(UGGYGOCombatSet, BaseHeal);
	/** 基础削韧量。动作游戏专有：重攻击削韧高、轻攻击削韧低。 */
	ATTRIBUTE_ACCESSORS(UGGYGOCombatSet, BasePoiseDamage);

protected:
	UFUNCTION()
	void OnRep_BaseDamage(const FGameplayAttributeData& OldValue);

	UFUNCTION()
	void OnRep_BaseHeal(const FGameplayAttributeData& OldValue);

	UFUNCTION()
	void OnRep_BasePoiseDamage(const FGameplayAttributeData& OldValue);

private:
	/** 基础伤害值。默认 0，实际数值由角色的初始化 GE 设定。 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_BaseDamage, Category = "GGYGO|Combat", Meta = (AllowPrivateAccess = true))
	FGameplayAttributeData BaseDamage;

	/** 基础治疗值。 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_BaseHeal, Category = "GGYGO|Combat", Meta = (AllowPrivateAccess = true))
	FGameplayAttributeData BaseHeal;

	/** 基础削韧值。 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_BasePoiseDamage, Category = "GGYGO|Combat", Meta = (AllowPrivateAccess = true))
	FGameplayAttributeData BasePoiseDamage;
};
