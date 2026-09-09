/**
 * @file GASArbiter.h
 * @brief GAS 状态仲裁器（Phase 7 扩展版）
 *
 * 每帧读取 ASC 的 GameplayTag → 写入 RuntimeData 仲裁标记。
 * GE 施加的状态（眩晕、死亡等）通过这里影响整个管线系统。
 *
 * Phase 7 扩展：
 *   - 使用 GGYGOTags 集中式 Tag 定义
 *   - 从 Restriction.* Tag 推导 bBlock* 标记（不再硬编码状态名）
 *   - 支持所有限制 Tag 类型
 */
#pragma once

#include "Pipeline/Interfaces/IArbiter.h"
#include "GameplayTagContainer.h"
#include "GGYGOTags.h"

class UAbilitySystemComponent;

class FGASArbiter : public IArbiter
{
public:
	/**
	 * 注入 ASC 指针 + 初始化 Tag 缓存
	 * @param InASC AbilitySystemComponent（不拥有，UObject 由 GC 管理）
	 */
	void Init(UAbilitySystemComponent* InASC);

	virtual void Arbitrate(FRuntimeData& RuntimeData, float DeltaTime) override;

private:
	/** ASC 指针（不拥有，UObject 由 GC 管理） */
	UAbilitySystemComponent* ASC = nullptr;

	/** Tag 缓存（Init 时一次性解析） */
	FGameplayTag Tag_Dead;
	FGameplayTag Tag_Stunned;
	FGameplayTag Tag_CantMove;
	FGameplayTag Tag_CantAttack;
	FGameplayTag Tag_CantDodge;
	FGameplayTag Tag_CantJump;
	FGameplayTag Tag_CantInput;
};
