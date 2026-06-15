/**
 * @file HealthArbiter.h
 * @brief 血量仲裁器
 *
 * 维护伤害队列，每帧统一结算。血量 ≤ 0 → 设置死亡标记 + 全面阻断。
 *
 * 为什么用队列而不是直接扣血：
 * 同一帧可能有多个伤害源（比如爆炸波及），队列保证所有伤害在同一时刻统一结算，
 * 避免中间状态不一致（比如第一个伤害已致死但第二个伤害还在执行）。
 */
#pragma once

#include "Pipeline/Interfaces/IArbiter.h"

class UAbilitySystemComponent;

/**
 * 伤害请求
 * 外部系统（伤害发放组件、碰撞检测）通过 RequestDamage 入队
 */
struct FDamageRequest
{
	float Damage = 0.f;

	/** 伤害来源（未来用于伤害类型、属性穿透等） */
	// TSubclassOf<UGameplayEffect> DamageEffectClass;
};

class FHealthArbiter : public IArbiter
{
public:
	/**
	 * 注入 ASC 指针（读取 Health 属性、应用 GE 扣血）
	 * @param InASC AbilitySystemComponent
	 */
	void Init(UAbilitySystemComponent* InASC);

	virtual void Arbitrate(FRuntimeData& RuntimeData, float DeltaTime) override;

	/**
	 * 外部请求伤害（入队，不立即扣血）
	 * 在每帧 Arbitrate 中统一结算
	 * @param Request 伤害请求（包含伤害值）
	 */
	void RequestDamage(const FDamageRequest& Request);

private:
	/** ASC 指针（不拥有） */
	UAbilitySystemComponent* ASC = nullptr;

	// ============================================================
	// 伤害队列（环形数组，固定容量，零 GC 开销）
	// ============================================================

	/** 最大排队伤害数 */
	static constexpr int32 MaxDamageQueue = 16;

	/** 伤害队列 */
	FDamageRequest DamageQueue[MaxDamageQueue];

	/** 当前队列中的伤害数量 */
	int32 DamageCount = 0;
};
