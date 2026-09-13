/**
 * @file GGYGOHealthSet.h
 * @brief 承受侧属性集 —— 生命 + 韧性
 *
 * 这是**状态落地层**。ExecutionCalculation 不直接改 Health，只把结果写进
 * `Damage` / `Healing` / `PoiseDamage` 三个一次性元属性，真正的加减、Clamp、
 * 消息广播和死亡/破韧边沿判定全在本类完成。
 *
 * ## 为什么用元属性中转
 * 如果 GE 直接改 Health，伤害就无法被拦截、修正或观察。走元属性后：
 *   - `PreGameplayEffectExecute` 可以按免疫 Tag 直接否掉这次伤害
 *   - `PostGameplayEffectExecute` 可以在扣血前广播消息、在扣血后判定死亡
 *   - 同一次 GE 里削韧和扣血能在一个回调内一起处理
 *
 * `Health` 和 `Damage` 都标了 `HideFromModifiers`，防止有人用普通 Modifier 绕过这条链路。
 *
 * ## 韧性为什么并进这里而不是单独一个 Set
 * 削韧和扣血在同一次命中里同时发生，放同一个 Set 可以在一次
 * `PostGameplayEffectExecute` 里处理完；拆成两个 Set 会变成跨 Set 读写，
 * 且 Execution 要多捕获一组属性。
 *
 * ## 两条不同的回调路径
 *   - **服务器**：GE 执行 → Pre/PostGameplayEffectExecute，能拿到完整 EffectSpec 与来源 Actor
 *   - **客户端**：属性复制 → OnRep_*，只有新旧值，来源上下文一律为 nullptr
 * 委托的前三个参数因此必须允许为空。
 */
#pragma once

#include "AbilitySystemComponent.h"
#include "AbilitySystem/Attributes/GGYGOAttributeSet.h"

#include "GGYGOHealthSet.generated.h"

class UObject;
struct FFrame;
struct FGameplayEffectModCallbackData;

UCLASS(BlueprintType)
class GGYGO_API UGGYGOHealthSet : public UGGYGOAttributeSet
{
	GENERATED_BODY()

public:
	UGGYGOHealthSet();

	// ===== 生命 =====
	/** 当前生命值。上界受 MaxHealth 约束，只能由 Execution 经元属性修改。 */
	ATTRIBUTE_ACCESSORS(UGGYGOHealthSet, Health);
	/** 生命上限。可被 GE 修改；下调时会同步压低当前 Health。 */
	ATTRIBUTE_ACCESSORS(UGGYGOHealthSet, MaxHealth);

	// ===== 韧性 =====
	/** 当前韧性。被削到 0 触发破韧硬直；恢复由周期性 GE 负责，不在本类里做。 */
	ATTRIBUTE_ACCESSORS(UGGYGOHealthSet, Poise);
	/** 韧性上限。 */
	ATTRIBUTE_ACCESSORS(UGGYGOHealthSet, MaxPoise);

	// ===== 元属性（一次性输入，消费后清零，不复制） =====
	/** 本次治疗量。映射为 +Health。 */
	ATTRIBUTE_ACCESSORS(UGGYGOHealthSet, Healing);
	/** 本次伤害量。映射为 -Health。 */
	ATTRIBUTE_ACCESSORS(UGGYGOHealthSet, Damage);
	/** 本次削韧量。映射为 -Poise。 */
	ATTRIBUTE_ACCESSORS(UGGYGOHealthSet, PoiseDamage);

	/** 生命值实际变化后广播。客户端路径下前三个参数为 nullptr。 */
	mutable FGGYGOAttributeEvent OnHealthChanged;
	/** 生命上限变化后广播。它只表示上限变了，不代表本次造成了伤害或治疗。 */
	mutable FGGYGOAttributeEvent OnMaxHealthChanged;
	/** 生命值跨过 0 时**只广播一次**。由 bOutOfHealth 边沿锁存保证不重复触发。 */
	mutable FGGYGOAttributeEvent OnOutOfHealth;

	/** 韧性实际变化后广播。用于韧性条 UI。 */
	mutable FGGYGOAttributeEvent OnPoiseChanged;
	/** 韧性跨过 0 时**只广播一次**。破韧演出与硬直 GE 由监听方（通常是战斗组件）发起。 */
	mutable FGGYGOAttributeEvent OnPoiseBroken;

protected:
	UFUNCTION()
	void OnRep_Health(const FGameplayAttributeData& OldValue);

	UFUNCTION()
	void OnRep_MaxHealth(const FGameplayAttributeData& OldValue);

	UFUNCTION()
	void OnRep_Poise(const FGameplayAttributeData& OldValue);

	UFUNCTION()
	void OnRep_MaxPoise(const FGameplayAttributeData& OldValue);

	/**
	 * GE 的 modifier 即将写入属性前调用。
	 * @return false 表示否掉这次修改（免疫命中时）。
	 * 本实现还负责保存变更前快照，供 Post 阶段计算真实变化量。
	 */
	virtual bool PreGameplayEffectExecute(FGameplayEffectModCallbackData& Data) override;

	/**
	 * GE 的 modifier 已写入后调用。
	 * 消费三个元属性、执行 Clamp、广播消息与委托、维护死亡与破韧边沿。
	 */
	virtual void PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data) override;

	/** 基础值变更入口的 Clamp。 */
	virtual void PreAttributeBaseChange(const FGameplayAttribute& Attribute, float& NewValue) const override;
	/** 聚合值变更入口的 Clamp。与上面是两条不同入口，都必须处理才能覆盖所有修改方式。 */
	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;
	/** 变更完成后的联动：上限下调时压低当前值，值恢复时清除边沿锁存。 */
	virtual void PostAttributeChange(const FGameplayAttribute& Attribute, float OldValue, float NewValue) override;

	/** 集中定义各属性边界。被两个 PreAttribute 入口共用。 */
	void ClampAttribute(const FGameplayAttribute& Attribute, float& NewValue) const;

private:
	/** 当前生命值。HideFromModifiers 强制伤害走元属性链路。 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_Health, Category = "GGYGO|Health", Meta = (HideFromModifiers, AllowPrivateAccess = true))
	FGameplayAttributeData Health;

	/** 生命上限。 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_MaxHealth, Category = "GGYGO|Health", Meta = (AllowPrivateAccess = true))
	FGameplayAttributeData MaxHealth;

	/** 当前韧性。同样禁止普通 Modifier 直接修改。 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_Poise, Category = "GGYGO|Poise", Meta = (HideFromModifiers, AllowPrivateAccess = true))
	FGameplayAttributeData Poise;

	/** 韧性上限。 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_MaxPoise, Category = "GGYGO|Poise", Meta = (AllowPrivateAccess = true))
	FGameplayAttributeData MaxPoise;

	/** 死亡边沿锁存。避免每次低于 0 的修改都重复广播 OnOutOfHealth。 */
	bool bOutOfHealth;

	/** 破韧边沿锁存。韧性回到正值后由 PostAttributeChange 清除。 */
	bool bPoiseBroken;

	/** Pre 阶段保存的快照，供 Post 阶段判断是否真实变化并提供委托的 OldValue。 */
	float HealthBeforeAttributeChange;
	float MaxHealthBeforeAttributeChange;
	float PoiseBeforeAttributeChange;

	// -------------------------------------------------------------------
	// 以下是元属性：一次性输入，不是持久状态，不复制，消费后必须清零
	// -------------------------------------------------------------------

	/** 本次治疗量。HealExecution 写入，Post 阶段转为 +Health 后清零。 */
	UPROPERTY(BlueprintReadOnly, Category = "GGYGO|Health", Meta = (AllowPrivateAccess = true))
	FGameplayAttributeData Healing;

	/** 本次伤害量。DamageExecution 写入，Post 阶段转为 -Health 后清零。 */
	UPROPERTY(BlueprintReadOnly, Category = "GGYGO|Health", Meta = (HideFromModifiers, AllowPrivateAccess = true))
	FGameplayAttributeData Damage;

	/** 本次削韧量。Post 阶段转为 -Poise 后清零。 */
	UPROPERTY(BlueprintReadOnly, Category = "GGYGO|Poise", Meta = (HideFromModifiers, AllowPrivateAccess = true))
	FGameplayAttributeData PoiseDamage;
};
