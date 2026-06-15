/**
 * @file GASArbiter.h
 * @brief GAS 状态仲裁器
 *
 * 每帧读取 ASC 的 GameplayTag → 写入 RuntimeData 仲裁标记。
 * GE 施加的状态（眩晕、死亡等）通过这里影响整个管线系统。
 */
#pragma once

#include "Pipeline/Interfaces/IArbiter.h"
#include "GameplayTagContainer.h"

class UAbilitySystemComponent;

class FGASArbiter : public IArbiter
{
public:
	/**
	 * 注入 ASC 指针
	 * @param InASC AbilitySystemComponent（不拥有，UObject 由 GC 管理）
	 */
	void Init(UAbilitySystemComponent* InASC);

	virtual void Arbitrate(FRuntimeData& RuntimeData, float DeltaTime) override;

private:
	/** ASC 指针（不拥有，UObject 由 GC 管理） */
	UAbilitySystemComponent* ASC = nullptr;

	/** 缓存的眩晕 Tag（Init 时查找一次，避免每帧字符串查找） */
	FGameplayTag Tag_Stunned;

	/** 缓存的死亡 Tag */
	FGameplayTag Tag_Dead;
};
