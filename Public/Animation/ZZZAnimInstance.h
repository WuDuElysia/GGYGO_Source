/**
 * @file ZZZAnimInstance.h
 * @brief ZZZ 风格战斗动画的 C++ 决策层（新建，待实现）
 *
 * 与 NTEAnim（移动动画）并行存在，负责战斗动画状态机的过渡决策。
 * 拓扑在蓝图（ABP_ZZZ），决策在 C++，求值在 AnimGraph。
 *
 * 线程模型：与 NTEAnim 一致
 *   NativeUpdateAnimation           游戏线程 — 抓快照、选资产
 *   NativeThreadSafeUpdateAnimation worker 线程 — 只读快照，算输出
 */
#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "ZZZAnimInstance.generated.h"

// 前向声明
class ABaseCharacter;

/**
 * ZZZ 战斗动画决策层
 *
 * 继承自 UAnimInstance。与 NTEAnim（UNTEAnimInstance）并行：
 *   - NTEAnim 管理移动层（Idle/Walk/Run/Sprint/Enter/Stop）
 *   - ZZZAnim 管理战斗层（Attack/Hit/Dodge/Skill/Switch）
 *
 * 当前为空框架，待后续实现具体决策函数与输出变量。
 */
UCLASS()
class GGYGO_API UZZZAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	// ============================================================
	// AnimInstance 生命周期
	// ============================================================

	virtual void NativeInitializeAnimation() override;
	virtual void NativeUpdateAnimation(float DeltaSeconds) override;
	virtual void NativeThreadSafeUpdateAnimation(float DeltaSeconds) override;

	// ============================================================
	// Push Interface — 运行时数据注入
	// ============================================================

	/** 外部系统每帧调用，推入战斗运行时数据（待定义 USTRUCT） */
	// UFUNCTION(BlueprintCallable, Category = "Runtime")
	// void SetCombatRuntimeData(const FCombatRuntimeData& InData);

private:
	/** 所属角色弱引用（只在游戏线程访问） */
	TWeakObjectPtr<ABaseCharacter> Owner;
};
