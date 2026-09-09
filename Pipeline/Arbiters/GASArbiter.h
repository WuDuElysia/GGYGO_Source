/**
 * @file GASArbiter.h
 * @brief GAS 状态仲裁器
 *
 * 每帧读取 ASC 的 GameplayTag，翻译成 `RuntimeData.Arbiter` 里的 bool 阻断标记，
 * 供旧 Pipeline 的各阶段查询。GE 施加的状态（眩晕、死亡、硬直）通过这里影响整条管线。
 *
 * ## 这个类正在退役
 * 移动层重建后它的职责会被拆散：
 * - `CantMove` 由 `UGGYGOCharacterMovementComponent` 直接查 ASC，不需要中间的 bool
 * - `CantAttack` / `CantDodge` 由各 GA 的 `ActivationBlockedTags` 表达，GAS 自己就会拦
 *
 * 也就是说「读 Tag 写 bool」这层翻译本身就是多余的，它存在只是因为旧 Pipeline
 * 不认识 GameplayTag。等 Pipeline 退役，本类一并删除。
 *
 * 阶段 2 只把 Tag 来源从运行时字符串解析换成原生 Tag，不改变行为。
 */
#pragma once

#include "Pipeline/Interfaces/IArbiter.h"
#include "GameplayTagContainer.h"

class UAbilitySystemComponent;

class FGASArbiter : public IArbiter
{
public:
	/**
	 * @param InASC AbilitySystemComponent。不拥有，UObject 由 GC 管理。
	 *
	 * 不再需要预解析 Tag：原生 Tag 在模块加载时就注册好了，
	 * 每次使用是直接读全局变量，没有字符串查找开销。
	 */
	void Init(UAbilitySystemComponent* InASC);

	virtual void Arbitrate(FRuntimeData& RuntimeData, float DeltaTime) override;

private:
	/** ASC 指针（不拥有，UObject 由 GC 管理） */
	UAbilitySystemComponent* ASC = nullptr;
};
