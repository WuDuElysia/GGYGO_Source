/**
 * @file ArbiterPipeline.h
 * @brief 仲裁管线 - 第二层全局仲裁
 *
 * 在 Tick 最开头执行（比 InputPipeline 还早）。
 * 持有 GASArbiter / ActionArbiter / HealthArbiter / StaminaArbiter 四个仲裁器。
 *
 * 执行顺序固定：
 * GASArbiter → ActionArbiter → HealthArbiter → StaminaArbiter
 *
 * 每个仲裁器独立写 RuntimeData 的不同字段，互不依赖。
 */
#pragma once

#include "CoreMinimal.h"
#include "Pipeline/Interfaces/IArbiter.h"

class UAbilitySystemComponent;
class FGYGOStateManager;
struct FRuntimeData;

class FArbiterPipeline
{
public:
	/**
	 * 初始化所有仲裁器，注入 ASC 和 StateManager
	 * @param InASC AbilitySystemComponent 指针
	 * @param InSM  StateManager 指针
	 */
	void Init(UAbilitySystemComponent* InASC, FGYGOStateManager* InSM);

	/**
	 * 每帧执行所有仲裁器（Tick 第 1 步，先于 InputPipeline）
	 * 前置条件：无（最先执行，其他系统依赖仲裁结果）
	 * @param RuntimeData 运行时黑板（写入仲裁标记）
	 * @param DeltaTime   帧间隔
	 */
	void Process(FRuntimeData& RuntimeData, float DeltaTime);

	/** 获取 HealthArbiter（外部请求伤害时需要） */
	class FHealthArbiter* GetHealthArbiter() const;

private:
	/** 仲裁器列表（按执行顺序：GAS → Action → Health → Stamina） */
	TArray<TUniquePtr<IArbiter>> Arbiters;

	/** HealthArbiter 裸指针（提供给外部调用 RequestDamage） */
	class FHealthArbiter* HealthArbiterPtr = nullptr;
};
