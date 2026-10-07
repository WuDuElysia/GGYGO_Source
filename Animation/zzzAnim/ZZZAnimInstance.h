/**
 * @file ZZZAnimInstance.h
 * @brief 现有 ZZZ AnimBP 的迁移期兼容实例
 *
 * 新架构由 `UGGYGOAnimInstanceBase` 发布单向只读的 AnimationStateFrame。
 * 本类只保留 Pyrios 的表现记忆和资产查询。状态迁移条件由 AnimBP 直接组合
 * `AnimationState` 与通用语义查询，不再通过按状态名命名的 C++ 决策函数。
 *
 * ## ABP_Pyrios 实际用到的东西
 * 状态机嵌套是 `MainStateMachine → MainGroundState → LocomotionState`，
 * 最内层的 locomotion 状态机有五个状态与一个 Conduit：
 *
 * ```text
 * NotMoving ──bHasMoveInput────────────────────┐
 * Stop ───────bHasMoveInput────────────────────┤
 *                                          Conduit
 *                          ┌──!IsAnimationRunGait──→ EnterMove
 *                          └── IsAnimationRunGait──→ Moving
 * EnterMove ──动画剩余 <= 0.03──→ Moving
 * EnterMove ──!bHasMoveInput─────→ Stop
 * Moving ─────!bHasMoveInput && !IsTurnBackCurveDriven──→ Stop
 * Stop ───────动画剩余 <= 0.5────→ NotMoving
 *
 * Moving 内层：WalkRun ──IsTurnBackCurveDriven──→ TurnBack
 *                      ←──动画剩余 <= 0.03────┘
 * ```
 *
 * 各状态取的资产：`NotMoving` = `IdleLoop`，`EnterMove` = `WalkStart`，
 * `TurnBack` = `TurnBack`，`Stop` 用 `StateMemory.StopValue` 在
 * `WalkStartEnd` / `WalkEnd` / `RunEnd` 之间选，`WalkRun` 用 `WalkRun` BlendSpace
 * 并以 `StateMemory.GaitBlendY` 驱动它的 **X 轴**（BlendSpace1D 的单轴是 X）。
 *
 * AnimGraph 顶层由 ActionPoseSlot 保留 Body XYZ 余量及原组件对齐，再进入原惯性化。
 * WalkRun 倾身仅作用于其内部姿态分支，不修改动作轨迹、Body 平移或胶囊。
 *
 * ## 蓝图当前读取面
 * 过渡图读取 `AnimationState.bHasMoveInput` 与两个通用语义查询；状态图读取
 * `StateMemory.GaitBlendY`（旧序列化名，显示为 Walk Run Blend Alpha）、
 * `StateMemory.StopValue`，以及两个查表函数。
 * 其余暴露出去的属性（`AnimBlend*`、`ActualVelocity*`、`bTurnBackRunOut`）
 * 目前**没有**任何 AnimGraph 节点消费，保留它们是为了给表现层留钩子；
 * 逐个属性的现状见各自的注释。
 *
 * 本类不维护状态机循环，也不解算步态：Movement 在只读帧中发布走跑混合与停止语义，
 * 动画层只把它们映射到现有 GaitBlendY 序列化存储和 StopValue。动画层不再插值走跑值，
 * 也不再计时选择停止分支。
 */
#pragma once

#include "CoreMinimal.h"
#include "Animation/Runtime/GGYGOAnimInstanceBase.h"
#include "Animation/zzzAnim/Data/ZZZAnimSet.h"
#include "Animation/zzzAnim/Data/ZZZAnimTuning.h"
#include "Animation/zzzAnim/Data/ZZZAnimContext.h"
#include "Animation/zzzAnim/Data/ZZZAnimSnapshot.h"
#include "Animation/zzzAnim/Capture/ZZZAnimSnapshotCapture.h"
#include "Animation/zzzAnim/Data/ZZZAnimStateMemory.h"
#include "Animation/zzzAnim/Locomotion/ZZZLocomotionEvents.h"
#include "ZZZAnimInstance.generated.h"

class UBlendSpace;

UCLASS()
class GGYGO_API UZZZAnimInstance : public UGGYGOAnimInstanceBase
{
	GENERATED_BODY()

public:
	// ============================================================
	// AnimInstance 生命周期
	// ============================================================

	virtual void NativeUpdateAnimation(float DeltaSeconds) override;

	// ============================================================
	// 配表查询（AnimBP 的 SequencePlayer 节点 Bind 此函数）
	// key 和资产全部由蓝图在 AnimSet 细节面板配置，代码只做查表
	// ============================================================

	UFUNCTION(BlueprintPure, Category = "Query|AnimSet", meta = (BlueprintThreadSafe))
	UAnimSequence* GetSeqByKey(FName Key) const;

	UFUNCTION(BlueprintPure, Category = "Query|AnimSet", meta = (BlueprintThreadSafe))
	UBlendSpace* GetBlendSpaceByKey(FName Key) const;

	// ============================================================
	// 配置（全部由蓝图细节面板填写）
	// ============================================================

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "配置|AnimSet")
	FZZZAnimSet AnimSet;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "配置|Tuning")
	FZZZAnimTuning Tuning;

	// ============================================================
	// 迁移期表现记忆（C++ pipeline 写入，AnimGraph 只读）
	// ============================================================

	UPROPERTY(BlueprintReadOnly, Category = "State|Locomotion")
	FZZZAnimStateMemory StateMemory;

	/**
	 * 相对 Actor 当前水平朝向的移动方向分量：X 为右、Y 为前，均已归一化。
	 *
	 * **当前没有 AnimGraph 节点消费这两个值。** 走跑混合用的是
	 * `StateMemory.GaitBlendY`（速度档位），方向靠 Actor 自身转向解决。
	 * 这两个值要等有了侧向/后退的移动循环动画、把 `WalkRun` 换成二维 BlendSpace
	 * 之后才有去处 —— Pyrios 目前只有 `Walk_Loop` 与 `Run_Loop` 两个前向循环。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "State|Locomotion")
	float AnimBlendX = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "State|Locomotion")
	float AnimBlendY = 0.f;

	/**
	 * `RootMotion_PosX/PosY` 差分得到的曲线分量速度（cm/s）与它的方向、方向角。
	 *
	 * 轴序是 UE 局部空间（X 前、Y 右），但基准是**动画段起点的朝向**而非角色当前朝向 ——
	 * 转身时角色已经转过 180 度，这三个值仍以进入转身那一刻的朝向为基准。
	 * 想拿它们算世界方向必须自己用「进入该段时的朝向」换算，直接当角色局部方向用会错。
	 *
	 * **当前没有 AnimGraph 节点消费。** 曲线速度已经在移动层驱动 `GetMaxSpeed`，
	 * 表现层不需要重复读一遍；留着是为了调试时能在 AnimBP 调试面板直接看到曲线采样结果。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "State|RootMotion")
	FVector AnimCurveVelocity = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "State|RootMotion")
	FVector AnimCurveVelocityDirection = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "State|RootMotion")
	float AnimCurveVelocityAngle = 0.f;

	/**
	 * 角色**实际**速度（`Velocity`，不是曲线）的派生量：世界方向、相对 Actor 的
	 * BlendSpace 分量（X=右、Y=前）、以及相对 Actor 的方向角（0=前，+90=右）。
	 *
	 * `ActualVelocityBlendX/Y` 是上面 `AnimBlendX/Y` 的数据来源，其余两个仅供调试。
	 * **当前都没有 AnimGraph 节点消费。**
	 */
	UPROPERTY(BlueprintReadOnly, Category = "State|Locomotion")
	FVector ActualVelocityDirection = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "State|Locomotion")
	float ActualVelocityBlendX = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "State|Locomotion")
	float ActualVelocityBlendY = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "State|Locomotion")
	float ActualVelocityAngle = 0.f;

	/**
	 * 转身已进入交还输入的 `RunOut` 段。
	 *
	 * **当前没有 AnimGraph 节点消费**：`RunOut` 段 `IsTurnBackCurveDriven`
	 * 为假，状态机自己就切回 WalkRun 了，不需要读这个标记来做分支。
	 * 留着是给「想在跑出段单独做表现」留的钩子（那一段方向来自玩家输入而非曲线，
	 * 朝向交回了 CMC 的自动对齐，玩家改方向时身体朝向会与动画姿势有偏差）。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "State|TurnBack")
	bool bTurnBackRunOut = false;

	/** Filtered presentation angle; positive means lean toward a right turn. AnimBP owns bone axis/sign. */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "State|WalkRun Lean")
	float WalkRunLeanAngleDegrees = 0.0f;

	/** A usable pose result, including normal recovery; false for disabled, initial waiting or failure. */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "State|WalkRun Lean")
	bool bWalkRunLeanPresentationValid = false;

	/** An enabled dependency/configuration failure stays visible; neutral pose is cleanup, not success. */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "State|WalkRun Lean")
	FString WalkRunLeanFailureReason;

protected:
	/** 迁移期表现快照（游戏线程写入，表现记忆只读）。 */
	FZZZAnimSnapshot Snap;

	/** 清理本实例在 Pawn 生命周期之间持有的兼容表现状态。 */
	virtual void OnAnimationLifecycleReset() override;
	virtual void ResolveLocomotionSourceBinding(FGGYGOLocomotionSourceBinding& OutBinding) const override;
	virtual bool IsLocomotionSourceConfigurationCurrent(const FGGYGOLocomotionSourceBinding& Binding) const override;

	/** Consume the already captured frame in the existing native update; never queries movement. */
	void UpdateWalkRunLeanPresentation(float DeltaSeconds);
	/** Retire only this instance's presentation state while preserving authored Tuning. */
	void ResetWalkRunLeanPresentation();

private:
	/** 通用语义帧适配成旧快照，再映射到现有 AnimBP 表现存储。 */
	void RefreshDecisionContext();

	/** 把通用 AnimationStateFrame 转成现有 AnimBP 仍在使用的旧快照。 */
	FZZZAnimSnapshotCapture LegacySnapshotAdapter;
	FZZZLocomotionEvents LocomotionEvents;

	void FailWalkRunLeanPresentation(FName FailureCode, const FString& Reason);
	FName LastWalkRunLeanFailureCode = NAME_None;
	/** Borrowed identity for retiring a presentation filter; never authorizes movement. */
	TWeakObjectPtr<UGGYGOCharacterMovementComponent> WalkRunLeanSource;
	uint64 WalkRunLeanSourceEpoch = 0;
};
