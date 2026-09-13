/**
 * @file GGYGOAttributeSet.h
 * @brief 所有 GGYGO AttributeSet 的公共基类
 *
 * 只提供两样东西：`GetWorld()`（AttributeSet 不是 Actor，要靠 Outer 拿 World）
 * 和取回项目 ASC 的类型化便利函数。具体属性由派生 Set 定义。
 *
 * ## 拆分依据（按 Lyra 的分法）
 *   - `UGGYGOHealthSet`：目标侧的**承受**属性。生命、韧性，以及一次性的 Damage / Healing / PoiseDamage 元属性。
 *   - `UGGYGOCombatSet`：来源侧的**输出**属性。BaseDamage / BaseHeal / BasePoiseDamage，供 Execution 捕获。
 *
 * 这个拆分不是形式主义：ExecutionCalculation 需要同时捕获"源的 CombatSet"和"目标的 HealthSet"。
 * 两者混在一个 Set 里，同一个 Set 会既作源又作目标被捕获，语义无法区分。
 */
#pragma once

#include "AttributeSet.h"

#include "GGYGOAttributeSet.generated.h"

class AActor;
class UGGYGOAbilitySystemComponent;
class UObject;
class UWorld;
struct FGameplayEffectSpec;

/**
 * 为一个属性批量生成四个访问器。
 *
 * `ATTRIBUTE_ACCESSORS(UGGYGOHealthSet, Health)` 展开为：
 *   static FGameplayAttribute GetHealthAttribute();  // 静态句柄，Execution 捕获属性时用
 *   float GetHealth() const;                         // 读当前值
 *   void  SetHealth(float NewVal);                   // 写当前值
 *   void  InitHealth(float NewVal);                  // 写基础值（初始化用）
 *
 * 静态句柄不只用于读写，回调里判断"这次改的是哪个属性"也依赖它。
 */
#define ATTRIBUTE_ACCESSORS(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_GETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_SETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_INITTER(PropertyName)

/**
 * 属性变化广播委托。
 *
 * @param EffectInstigator 逻辑发起者。**客户端 RepNotify 路径下为 nullptr**。
 * @param EffectCauser     物理造成者。同上，客户端为 nullptr。
 * @param EffectSpec       本次 GE 规格。同上，客户端为 nullptr。
 * @param EffectMagnitude  本次回调携带的原始幅度，**未经 Clamp**，不等于属性实际变化量。
 * @param OldValue         变化前的值。
 * @param NewValue         变化后的值。
 *
 * 前三个参数可空是设计使然，不是缺陷：客户端只能从复制快照差值推断变化，拿不到 GE 上下文。
 * 监听方必须判空，且**不得在回调里反向写属性**。
 */
DECLARE_MULTICAST_DELEGATE_SixParams(FGGYGOAttributeEvent, AActor* /*EffectInstigator*/, AActor* /*EffectCauser*/, const FGameplayEffectSpec* /*EffectSpec*/, float /*EffectMagnitude*/, float /*OldValue*/, float /*NewValue*/);

UCLASS()
class GGYGO_API UGGYGOAttributeSet : public UAttributeSet
{
	GENERATED_BODY()

public:
	UGGYGOAttributeSet();

	/** AttributeSet 不是 Actor，World 通过 Outer 间接取得。 */
	virtual UWorld* GetWorld() const override;

	/** 取回拥有者 ASC 并转成项目类型。类型不匹配时返回 nullptr，调用方按是否必须存在决定要不要 check。 */
	UGGYGOAbilitySystemComponent* GetGGYGOAbilitySystemComponent() const;
};
