/**
 * @file GGYGOVerbMessage.h
 * @brief 通用"谁对谁做了什么"消息载荷
 *
 * 配合 `UGameplayMessageSubsystem`（来自 Plugins/GameplayMessageRouter）向**只读观察者**广播事件：
 * 伤害数字、命中音效、击杀播报、成就统计等。
 *
 * 与 GameplayEvent 的区别（不要混用）：
 *   - GameplayEvent：发给特定 Actor 的 ASC，用来**驱动 Ability 逻辑**，载荷是固定的 `FGameplayEventData`。
 *   - GameplayMessage：全局广播、无指定目标，用来**通知旁观系统**，载荷是任意 USTRUCT。
 *
 * 收到本消息的一方不得反向修改属性或 ASC 状态，那属于 GAS 的权威链路。
 */
#pragma once

#include "GameplayTagContainer.h"

#include "GGYGOVerbMessage.generated.h"

/** 一次已发生的游戏事件。字段允许为空：客户端从 RepNotify 路径广播时拿不到完整来源上下文。 */
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

	/** 承受者。 */
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
	 * 事件数值。伤害消息里是本次 Damage 元属性的原始幅度，
	 * **未经过 Clamp**，因此可能大于目标实际损失的生命值（例如残血被一击打死）。
	 */
	UPROPERTY(BlueprintReadWrite, Category = "Message")
	float Magnitude = 0.0f;
};
