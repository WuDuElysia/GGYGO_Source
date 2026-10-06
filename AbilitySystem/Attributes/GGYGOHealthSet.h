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
 *   - 每个 GE Modifier 分别进入 Pre/Post；Health 和 Poise 不是同一 GE 内的一次原子事务
 *   - meta 值按各自 Modifier 的实际写入贡献消费，属性 Clamp 与边沿在同步属性回调内落地
 *
 * `HideFromModifiers` 是编辑器属性列表的过滤元数据，不是运行时权限控制。
 * HealthSet 仍会处理直接 Health/Poise Modifier 的 Clamp 与真实边沿。
 *
 * ## 韧性为什么并进这里而不是单独一个 Set
 * 削韧和扣血的规则都归 HealthSet；Execution 可以输出多个 Modifier，GAS 会逐个
 * 执行它们的 Pre/Apply/Post，因此这不构成同一次原子结算。
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
	/** 聚合后的当前生命值限制为 [0, MaxHealth]；内部有限 Base 可越界以保留持续 GE 的结算语义。 */
	ATTRIBUTE_ACCESSORS(UGGYGOHealthSet, Health);
	/** 生命上限。可被 GE 修改；下调时会同步压低当前 Health。 */
	ATTRIBUTE_ACCESSORS(UGGYGOHealthSet, MaxHealth);

	// ===== 韧性 =====
	/** 当前韧性。正值变为 0 触发破韧边沿；meta 削韧与直接 Poise GE 都由本类 Clamp。 */
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
	/** 韧性跨过 0 时**只广播一次**。具体后续能力接入由玩法层处理。 */
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
	 * 本实现捕获 Modifier 来源与原始幅度；属性实际 Old/New 由 PostAttributeChange 捕获。
	 */
	virtual bool PreGameplayEffectExecute(FGameplayEffectModCallbackData& Data) override;

	/**
	 * GE 的 modifier 已写入后调用。
	 * 消费三个元属性并结算属性；边沿由 PostAttributeChange 同步提交，结果在 root Modifier 退出后同步发布。
	 */
	virtual void PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data) override;

	/** Health/Poise 允许有限 Base 越界；其它属性仍保留各自的 Base 边界。 */
	virtual void PreAttributeBaseChange(const FGameplayAttribute& Attribute, float& NewValue) const override;
	/** 聚合值变更入口的 Clamp。与上面是两条不同入口，都必须处理才能覆盖所有修改方式。 */
	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;
	/** 变更完成后的联动：上限下调时压低当前值，值恢复时清除边沿锁存。 */
	virtual void PostAttributeChange(const FGameplayAttribute& Attribute, float OldValue, float NewValue) override;
	/** 本次 RepNotify 内迟到创建聚合器时，只记录匹配未判定帧的阶段。 */
	virtual void OnAttributeAggregatorCreated(const FGameplayAttribute& Attribute, FAggregator* NewAggregator) const override;

	/** 复制批处理的请求只存活于原生 Pre/PostNetReceive 生命周期。 */
	virtual void PreNetReceive() override;
	virtual void PostNetReceive() override;

	/** 聚合后的可见属性边界。 */
	void ClampAttribute(const FGameplayAttribute& Attribute, float& NewValue) const;

private:
	struct FModifierFrame;
	struct FQueuedResult;
	struct FExpectedAttributeChange;
	struct FRepNotifyFrame;
	struct FNetReceiveFrame;
	enum class EQueuedResultType : uint8;
	enum class EMessageDeliveryResult : uint8;

	/** 只在当前同步 GE Modifier 调用栈非空；嵌套时以回调数据地址配对 Pre/Post。 */
	TArray<TSharedPtr<FModifierFrame>> ModifierFrames;
	/** 根 Modifier 仍在运行时暂存的逐次结果，根栈退出后 swap 到局部数组并同步发布。 */
	TArray<TSharedPtr<FQueuedResult>> PendingResults;
	/** 标记 HealthSet 自己发起的单次属性写入，避免把原生委托中的后续重入归给旧 Modifier。 */
	TArray<TSharedPtr<FExpectedAttributeChange>> ExpectedAttributeChanges;
	/** 记录 RepNotify 的调用阶段、弱来源及最终聚合值，仅存活于该同步宏调用期间。 */
	TArray<TSharedPtr<FRepNotifyFrame>> RepNotifyFrames;
	/** 仅保存本次复制要求重算的属性和弱来源，不保存 Base/Current 快照。 */
	TArray<TSharedPtr<FNetReceiveFrame>> NetReceiveFrames;
	/** 将 PostAttributeChange 内部的联动写入一起完成后再发布同步结果。 */
	int32 AttributeChangeDepth = 0;
	/** Diagnostic bits only: report each missing message dependency once until routing recovers. */
	uint8 ReportedMessageDependencyFailures = 0;

	TSharedPtr<FModifierFrame> FindFrame(const FGameplayEffectModCallbackData& Data) const;
	TSharedPtr<FModifierFrame> FindAwaitingFrame(const FGameplayAttribute& Attribute) const;
	TSharedPtr<FRepNotifyFrame> BeginRepNotifyFrame(const FGameplayAttribute& Attribute);
	void EndRepNotifyFrame(const TSharedPtr<FRepNotifyFrame>& Frame);
	bool CanClassifyRepNotifyFrame(const TSharedPtr<FRepNotifyFrame>& Frame) const;
	TSharedPtr<FRepNotifyFrame> FindRepNotifyFrame(const FGameplayAttribute& Attribute) const;
	TSharedPtr<FExpectedAttributeChange> PushExpectedAttributeChange(const TSharedPtr<FModifierFrame>& Frame, const FGameplayAttribute& Attribute);
	void PopExpectedAttributeChange();
	TSharedPtr<FExpectedAttributeChange> ConsumeExpectedAttributeChange(const FGameplayAttribute& Attribute);
	/** 权威 Current 目标通过 ASC 换算为 Base，实际 Post 回调证明提交。 */
	bool TrySetCurrentValue(const FGameplayAttribute& Attribute, float DesiredCurrent, const TSharedPtr<FModifierFrame>& Frame);
	void RequestClientMaxReevaluation(const FGameplayAttribute& Attribute);
	void ReevaluateClientMax(const FGameplayAttribute& Attribute, const TWeakObjectPtr<AActor>& Owner, const TWeakObjectPtr<UAbilitySystemComponent>& ASC);
	bool IsCurrentAttributeSource(const FGameplayAttribute& Attribute, const AActor* Owner, const UAbilitySystemComponent* ASC) const;
	void QueueAttributeResult(EQueuedResultType ResultType, const TSharedPtr<FModifierFrame>& Frame, float Magnitude, float OldValue, float NewValue);
	void QueueMessageResult(const FGameplayTag& Verb, const TSharedPtr<FModifierFrame>& Frame, float Magnitude);
	EMessageDeliveryResult PublishMessageResult(const FQueuedResult& Result);
	void FlushPendingResults();
	void ApplyModifierMinimumHealth(const FGameplayAttribute& Attribute, float& NewValue) const;
	/** 当前生命值。HideFromModifiers 只影响编辑器属性列表；运行时直接 GE 仍经 Clamp 与边沿处理。 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_Health, Category = "GGYGO|Health", Meta = (HideFromModifiers, AllowPrivateAccess = true))
	FGameplayAttributeData Health;

	/** 生命上限。 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_MaxHealth, Category = "GGYGO|Health", Meta = (AllowPrivateAccess = true))
	FGameplayAttributeData MaxHealth;

	/** 当前韧性。HideFromModifiers 只影响编辑器属性列表；运行时直接 GE 仍经 Clamp 与边沿处理。 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_Poise, Category = "GGYGO|Poise", Meta = (HideFromModifiers, AllowPrivateAccess = true))
	FGameplayAttributeData Poise;

	/** 韧性上限。 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_MaxPoise, Category = "GGYGO|Poise", Meta = (AllowPrivateAccess = true))
	FGameplayAttributeData MaxPoise;

	/** 死亡边沿锁存。避免每次低于 0 的修改都重复广播 OnOutOfHealth。 */
	bool bOutOfHealth;

	/** 破韧边沿锁存。韧性回到正值后由 PostAttributeChange 清除。 */
	bool bPoiseBroken;

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
