/**
 * @file ZZZAnimInstance.h
 * @brief ZZZ 动画的 C++ 决策层
 *
 * 设计哲学：拓扑在蓝图，决策在 C++，求值在 AnimGraph。
 *
 * ## ABP_Pyrios 实际用到的东西
 * 状态机嵌套是 `MainStateMachine → MainGroundState → LocomotionState`，
 * 最内层的 locomotion 状态机有五个状态与一个 Conduit：
 *
 * ```text
 * NotMoving ──Locomotion_NotMoving_To_Conduit──┐
 * Stop ───────Locomotion_Stop_To_Conduit───────┤
 *                                          Conduit
 *                          ┌──Conduit_To_EnterMove──→ EnterMove
 *                          └──Conduit_To_Moving_Direct──→ Moving
 * EnterMove ──动画剩余 <= 0.03──→ Moving
 * EnterMove ──EnterMove_To_Stop──→ Stop
 * Moving ─────Moving_To_Stop─────→ Stop
 * Stop ───────动画剩余 <= 0.5────→ NotMoving
 *
 * Moving 内层：WalkRun ──WalkRun_To_TurnBack──→ TurnBack
 *                      ←──动画剩余 <= 0.03────┘
 * ```
 *
 * 各状态取的资产：`NotMoving` = `IdleLoop`，`EnterMove` = `WalkStart`，
 * `TurnBack` = `TurnBack`，`Stop` 用 `StateMemory.StopValue` 在
 * `WalkStartEnd` / `WalkEnd` / `RunEnd` 之间选，`WalkRun` 用 `WalkRun` BlendSpace
 * 并以 `StateMemory.GaitBlendY` 驱动它的 **X 轴**（BlendSpace1D 的单轴是 X）。
 *
 * AnimGraph 顶层在状态机之后还接了一个作用于 `Bip001` 的 `Transform (Modify) Bone`
 * （组件空间、只有 Z 平移 50.802）用于把骨架对齐胶囊体，以及一个惯性化节点。
 * 那两个与 root motion 无关，动画的 in-place 化只扣水平位移与 yaw，不影响它们。
 *
 * ## 蓝图当前只读这三个成员
 * `StateMemory.GaitBlendY`、`StateMemory.StopValue`，以及两个查表函数。
 * 其余暴露出去的属性（`AnimBlend*`、`ActualVelocity*`、`bTurnBackRunOut`）
 * 目前**没有**任何 AnimGraph 节点消费，保留它们是为了给表现层留钩子；
 * 逐个属性的现状见各自的注释。
 *
 * 本类不维护状态机循环，也不解算步态：步态由移动层给出，动画层只消费快照、
 * 推进 GaitBlendY 与 StopValue。EnterMove 早停计时与 Moving 子状态由
 * `FZZZLocomotionEvents` 每帧维护。
 */
#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/zzzAnim/Data/ZZZAnimSet.h"
#include "Animation/zzzAnim/Data/ZZZAnimTuning.h"
#include "Animation/zzzAnim/Data/ZZZAnimContext.h"
#include "Animation/zzzAnim/Locomotion/ZZZLocomotionDecisions.h"
#include "Animation/zzzAnim/Data/ZZZAnimSnapshot.h"
#include "Animation/zzzAnim/Capture/ZZZAnimSnapshotCapture.h"
#include "Animation/zzzAnim/Data/ZZZAnimStateMemory.h"
#include "Animation/zzzAnim/Locomotion/ZZZLocomotionEvents.h"
#include "ZZZAnimInstance.generated.h"

class ACharacter;
class UBlendSpace;

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
	// Locomotion 过渡决策函数（AnimBP 过渡条件引用）
	//
	// 全部是快照的只读视图，无副作用，因此蓝图求值多少次、在哪个线程求值都不影响状态。
	// 停止判定看的是「有没有移动输入」而不是速度：起步第一帧速度还是 0，
	// 按速度判定会让起步动画晚一帧，玩家能感觉到输入迟滞。
	//
	// 三个纯动画时序的过渡不在这里：EnterMove → Moving、TurnBack → WalkRun、
	// Stop → NotMoving 都由 AnimBP 自己用 `Time Remaining (ratio)` 判定 ——
	// 那是「这段动画播完了吗」，属于表现层自己的事，C++ 不需要知道。
	// ============================================================

	/**
	 * NotMoving → Conduit：本帧有移动意图。
	 *
	 * 该函数只读取快照，不访问 Actor 或移动层。
	 */
	UFUNCTION(BlueprintPure, Category = "Cond|Locomotion", meta = (BlueprintThreadSafe))
	bool Locomotion_NotMoving_To_Conduit() const;

	/**
	 * Stop → Conduit：重新启动移动入口，仅当本帧有移动输入/意图时返回 true。
	 * 上下文缺失时返回 false；不读取速度、Gait 或 StateMemory。
	 */
	UFUNCTION(BlueprintPure, Category = "Cond|Locomotion", meta = (BlueprintThreadSafe))
	bool Locomotion_Stop_To_Conduit() const;

	/**
	 * Conduit → EnterMove：本帧 Snapshot_Gait 不是 Run，走起步路径；上下文缺失时同样成立。
	 */
	UFUNCTION(BlueprintPure, Category = "Cond|Locomotion", meta = (BlueprintThreadSafe))
	bool Locomotion_Conduit_To_EnterMove() const;

	/**
	 * Conduit → Moving：本帧 Snapshot_Gait 已是 Run，走直接进入路径。
	 *
	 * 只读取快照，不产生副作用。
	 */
	UFUNCTION(BlueprintPure, Category = "Cond|Locomotion", meta = (BlueprintThreadSafe))
	bool Locomotion_Conduit_To_Moving_Direct() const;

	/**
	 * Moving → Stop 的独立 Blueprint 入口。
	 * 本帧没有移动输入时通常返回 true；转身相位为任一非 None 值时保持 false，
	 * 避免转身还没走完就提前离开 Moving。底层转发 ShouldExitMoving()，不是速度为零判断。
	 */
	UFUNCTION(BlueprintPure, Category = "Cond|Locomotion", meta = (BlueprintThreadSafe))
	bool Locomotion_Moving_To_Stop() const;

	/**
	 * EnterMove → Stop 的独立 Blueprint 入口。
	 * 仅当本帧没有移动输入时返回 true；底层直接转发 ShouldStopMoving()，不负责 EnterMove → Moving
	 * 的动画完成判断。
	 */
	UFUNCTION(BlueprintPure, Category = "Cond|Locomotion", meta = (BlueprintThreadSafe))
	bool Locomotion_EnterMove_To_Stop() const;

	/** WalkRun → TurnBack：移动层的转身相位已进入非 None。 */
	UFUNCTION(BlueprintPure, Category = "Cond|Locomotion", meta = (BlueprintThreadSafe))
	bool Locomotion_WalkRun_To_TurnBack() const;

	// 转身相位由 `UGGYGOCharacterMovementComponent::UpdateTurnBack` 从动画曲线判定，
	// 经快照读取；Back → WalkRun 的完整动画播放条件由 AnimBP 自己用动画时间节点判断。

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
	// 动画状态机记忆（C++ pipeline 写入，决策函数与 AnimGraph 只读）
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
	 * **当前没有 AnimGraph 节点消费**：`RunOut` 段 `Locomotion_WalkRun_To_TurnBack`
	 * 为假，状态机自己就切回 WalkRun 了，不需要读这个标记来做分支。
	 * 留着是给「想在跑出段单独做表现」留的钩子（那一段方向来自玩家输入而非曲线，
	 * 朝向交回了 CMC 的自动对齐，玩家改方向时身体朝向会与动画姿势有偏差）。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "State|TurnBack")
	bool bTurnBackRunOut = false;

protected:
	/** 动画决策快照（游戏线程写入，worker 线程与决策函数只读） */
	FZZZAnimSnapshot Snap;

private:
	/** 抓取快照 → 注入上下文（判定层只读视图、事件层可写上下文）→ 推进。 */
	void RefreshDecisionContext(float DeltaSeconds);

	/**
	 * 拥有者。
	 *
	 * 类型是 `ACharacter` 而不是具体角色类，因为动画层需要的一切都通过
	 * `UGGYGOCharacterMovementComponent` 取得，而 CMC 是 `ACharacter` 的既有子对象。
	 * 不绑定具体角色类，同一个 AnimBP 就能挂在任意角色类上；
	 * 没有项目 CMC 的角色只会得到全默认的快照，不会崩。
	 */
	TWeakObjectPtr<ACharacter> Owner;

	FZZZAnimSnapshotCapture SnapshotCapture;
	FZZZLocomotionDecisions LocomotionDecisions;
	FZZZLocomotionEvents LocomotionEvents;
};
