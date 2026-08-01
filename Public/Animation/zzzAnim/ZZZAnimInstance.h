/**
 * @file ZZZAnimInstance.h
 * @brief ZZZ 动画的 C++ 决策层
 *
 * 设计哲学：拓扑在蓝图，决策在 C++，求值在 AnimGraph。
 *
 * 本类只提供三类产物供 AnimBP 引用：
 *   1. 决策函数（UFUNCTION → bool）：被蓝图过渡条件 Can Enter Transition 引用
 *   2. 状态回调（UFUNCTION → void）：被蓝图状态的 On Entry 引用
 *   3. 配表查询（UFUNCTION → UAnimSequence*）：被 AnimGraph 节点 Bind
 *
 * 本类不维护状态机循环——谁在哪个状态、什么时候切，全部由 AnimBP 状态机管理。
 */
#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/zzzAnim/CombatConfig.h"
#include "Animation/zzzAnim/CombatDecisions/ZZZLocomotionDecisions.h"
#include "Animation/zzzAnim/ZZZAnimSnapshot.h"
#include "Animation/zzzAnim/ZZZAnimSnapshotCapture.h"
#include "ZZZAnimInstance.generated.h"

class ABaseCharacter;

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

	/**
	 * 管线主动驱动（BaseCharacter::Tick 末尾调用）
	 *
	 * 在逻辑管线全部完成之后显式抓取快照。
	 * 此后引擎调 NativeUpdateAnimation 时检测到标记，跳过重复工作。
	 * 保证 AnimBP 读到的 Snap 一定是本帧管线刚写完的最新值。
	 */
	void PipelineDrive();

	// ============================================================
	// ★★ Locomotion 过渡决策函数（AnimBP 过渡条件引用） ★★
	//
	// 来这里加你的移动过渡函数，模板：
	//   UFUNCTION(BlueprintPure, Category="Cond|Locomotion", meta=(BlueprintThreadSafe))
	//   bool Locomotion_Idle_To_WalkStart() const;
	//
	// NTEAnim 参考（MainMovementDecisions / LocomotionDecisions）：
	//   Idle→WalkStart:    bShouldMove && Gait==Walk
	//   Walk→Run:          Gait==Run
	//   WalkStart→WalkLoop: Start 动画播完
	//   WalkLoop→WalkEnd:  !bShouldMove
	//   WalkEnd→Idle:      End 动画播完
	//   RunLoop→RunEnd:    !bShouldMove || Gait==Walk
	//   RunEnd→Idle:       End 动画播完
	// ============================================================

	/**
	 * NotMoving → Conduit：有移动输入且逻辑状态已经是 Moving。
	 *
	 * 该函数只读取 ZZZAnim 快照，不读取 Actor 或逻辑 RuntimeData。
	 */
	UFUNCTION(BlueprintPure, Category = "Cond|Locomotion", meta = (BlueprintThreadSafe))
	bool Locomotion_NotMoving_To_Conduit() const;

	/**
	 * Conduit → EnterMove：当前帧没有冲刺触发。
	 */
	UFUNCTION(BlueprintPure, Category = "Cond|Locomotion", meta = (BlueprintThreadSafe))
	bool Locomotion_Conduit_To_EnterMove() const;

	/**
	 * Conduit → Moving（Sprint）：有冲刺触发，条件成立后消费本帧开关。
	 */
	UFUNCTION(BlueprintPure, Category = "Cond|Locomotion", meta = (BlueprintThreadSafe))
	bool Locomotion_Conduit_To_Moving_Sprint() const;

	// ============================================================
	// ★★ Locomotion 状态回调（AnimBP 状态的 On Entry 引用） ★★
	//
	// 来这里加状态进入时的回调，模板：
	//   UFUNCTION(BlueprintCallable, Category="Event|Locomotion", meta=(BlueprintThreadSafe))
	//   void OnEnter_Idle();
	//
	// NTEAnim 参考：OnEnter_WalkStart 时设置 FootLock 曲线、
	//   OnEnter_WalkLoop 时设置 Enable_FootIK 等
	// ============================================================

	// TODO: 在这里加你的 Locomotion 状态回调

	// ============================================================
	// 配表查询（AnimBP 的 SequencePlayer 节点 Bind 此函数）
	// key 和资产全部由蓝图在 AnimSet 细节面板配置，代码只做查表
	// ============================================================

	UFUNCTION(BlueprintPure, Category = "Query|AnimSet", meta = (BlueprintThreadSafe))
	UAnimSequence* GetSeqByKey(FName Key) const;

	UFUNCTION(BlueprintPure, Category = "Query|AnimSet", meta = (BlueprintThreadSafe))
	UBlendSpace* GetBlendSpaceByKey(FName Key) const;

	// ============================================================
	// 输出变量
	// ============================================================

	// TODO: 在这里加 AnimGraph 输出变量，如:
	//   UPROPERTY(BlueprintReadOnly, Category="Out")
	//   TObjectPtr<UAnimSequence> Out_IdleSeq;

	// ============================================================
	// 配置（全部由蓝图细节面板填写）
	// ============================================================

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "配置|AnimSet")
	FZZZAnimSet AnimSet;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "配置|Tuning")
	FZZZAnimTuning Tuning;

protected:
	/** 动画决策快照（管线捕获；决策函数读取，冲刺分流时消费一次性开关） */
	FZZZAnimSnapshot Snap;

private:
	TWeakObjectPtr<ABaseCharacter> Owner;
	bool bDrivenByPipeline = false;

	FZZZAnimSnapshotCapture SnapshotCapture;
	FZZZLocomotionDecisions LocomotionDecisions;
};
