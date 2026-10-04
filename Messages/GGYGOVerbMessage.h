/**
 * @file GGYGOVerbMessage.h
 * @brief 通用"谁对谁做了什么"消息载荷
 *
 * 配合 `UGameplayMessageSubsystem`（来自 Plugins/GameplayMessageRouter）向**只读观察者**广播事件：
 * 伤害数字、命中音效、击杀播报、成就统计等。
 *
 * 与 GameplayEvent 的区别（不要混用）：
 *   - GameplayEvent：发给特定 Actor 的 ASC，用来**驱动 Ability 逻辑**，载荷是固定的 `FGameplayEventData`。
 *   - GameplayMessage：当前 GameInstance 内按通道同步广播，用来**通知旁观系统**，载荷是任意 USTRUCT。
 *     路由本身不提供网络传输；HealthSet 的 RepNotify 只发属性委托，不补发本消息。
 *
 * 收到本消息的一方不得反向修改属性或 ASC 状态，那属于 GAS 的权威链路。
 */
#pragma once

#include "GameplayTagContainer.h"

#include "GGYGOVerbMessage.generated.h"

/**
 * 一次已发生的本地游戏事件。HealthSet 在本次 Modifier 结算后发布来源/Tag/幅度快照。
 * 参与者允许为空：EffectContext 不一定包含来源，不用目标或当前 Avatar 补造攻击者。
 * 快照不代表订阅回调期间 ASC 状态被冻结；其他合法 GAS 调用可能已经产生后续变化。
 * 消费者若跨回调保存消息应复制需要的数据，并处理对象失效；不得保存栈上载荷地址。
 */
USTRUCT(BlueprintType)
struct FGGYGOVerbMessage
{
	GENERATED_BODY()

	/** 事件类型。取 `GGYGOGameplayTags::Message_*`，接收方按它过滤。 */
	UPROPERTY(BlueprintReadWrite, Category = "Message")
	FGameplayTag Verb;

	/** 发起者。伤害消息里通常填 EffectCauser（实际造成伤害的物体）。 */
	UPROPERTY(BlueprintReadWrite, Category = "Message")
	TObjectPtr<UObject> Instigator = nullptr;

	/** 承受者。HealthSet 发布时为 ASC 的 OwningActor（持久宿主），不保证是 Pawn/Avatar。 */
	UPROPERTY(BlueprintReadWrite, Category = "Message")
	TObjectPtr<UObject> Target = nullptr;

	/** 事件发生时发起者侧的聚合 Tag 快照。 */
	UPROPERTY(BlueprintReadWrite, Category = "Message")
	FGameplayTagContainer InstigatorTags;

	/** 事件发生时承受者侧的聚合 Tag 快照。 */
	UPROPERTY(BlueprintReadWrite, Category = "Message")
	FGameplayTagContainer TargetTags;

	/** 上下文 Tag。用于携带命中部位、暴击、弱点等附加信息。 */
	UPROPERTY(BlueprintReadWrite, Category = "Message")
	FGameplayTagContainer ContextTags;

	/**
	 * Message.Damage：本次 Modifier 的原始幅度，未 Clamp，可大于实际生命损失。
	 * Message.PoiseBreak：主语义为韧性从正值到零的边沿，不是每次削韧。
	 * 由 PoiseDamage 触发时保留本次原始削韧幅度；直接 Poise GE 触发时使用
	 * 本次可确认的 Poise 损失（OldValue - NewValue，正数），不把赋值目标当伤害量。
	 * 普通削韧和治疗当前不发布此类消息；RepNotify 差值不作为本字段来源。
	 */
	UPROPERTY(BlueprintReadWrite, Category = "Message")
	float Magnitude = 0.0f;
};
