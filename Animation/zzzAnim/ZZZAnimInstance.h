/**
 * @file ZZZAnimInstance.h
 * @brief ZZZ 动画的 C++ 决策层
 *
 * 设计哲学：拓扑在蓝图，决策在 C++，求值在 AnimGraph。
 *
 * 本类只提供三类产物供 AnimBP 引用：
 *   1. 移动过渡判定（UFUNCTION → bool）：NotMoving/Stop → Conduit 的入口条件，以及
 *      Conduit 的分流条件、Moving → Stop、EnterMove → Stop 两个独立 Blueprint 入口；两个入口共享
 *      LocomotionDecisions.ShouldStopMoving() 的停止移动输入判定；Direct Conduit
 *      仅依据本帧 Snapshot_Gait == Run。EnterMove → Moving 的动画播放完成条件由
 *      AnimBP 直接使用 Time Remaining (ratio) <= 0 处理，不需要 C++ 函数
 *   2. 表现参数维护：Moving 子状态、StopValue、GaitValue 与 GaitBlendY 的维护由 C++ pipeline 完成；
 *      Back → WalkRun 的完整动画播放条件由 AnimBP 自己使用动画时间节点判断。
 *   3. 配表查询（UFUNCTION → UAnimSequence*）：被 AnimGraph 节点 Bind
 *
 * 本类不维护状态机循环，也不维护步态取值、Walk→Run 升级或 Sprint
 * 触发消费；步态由逻辑侧提供，动画层只消费快照并推进 GaitBlendY，维护 StopValue。
 * EnterMove 早停计时与 Moving 子状态、TurnBack 返回握手均由 FZZZLocomotionEvents 在 C++ 每帧逻辑中集中维护，
 * AnimBP 只保留状态机拓扑、过渡条件调用和动画播放。
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

	/**
	 * 管线主动驱动（BaseCharacter::Tick 末尾调用）
	 *
	 * 在逻辑管线全部完成之后显式抓取快照。
	 * 此后引擎调 NativeUpdateAnimation 时检测到标记，跳过重复工作。
	 * 保证 AnimBP 读到的 Snap 一定是本帧管线刚写完的最新值。
	 */
	void PipelineDrive(float DeltaSeconds);

	/** TurnBack 动画中名为 CanYaw 的 AnimNotify 回调。 */
	UFUNCTION()
	void AnimNotify_CanYaw();

	// ============================================================
	// ★★ Locomotion 过渡决策函数（AnimBP 过渡条件引用） ★★
	//
	// 移动过渡判定保持为快照只读视图：NotMoving → Conduit、Stop → Conduit、Conduit 的
	// Direct/EnterMove 互补分流，以及 Moving → Stop、EnterMove → Stop 两个独立
	// Blueprint 入口；两个 Stop 入口共同转发同一个停止输入判定。
	// 停止判定不读取速度；EnterMove → Moving 的动画播放完成条件由 AnimBP 直接使用
	// Time Remaining (ratio) <= 0 处理，不需要 C++ 函数。Direct 分支仅
	// 依据 Snapshot_Gait == Run；走跑表现由 GaitBlendY 驱动，
	// 不再通过 Moving 内 Walk→Run 状态过渡或动画层计时判定实现。
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
	 * 本帧没有移动输入时通常返回 true；TurnBack 仍处于 Frozen 阶段时保持 false，避免转身尚未解冻就提前离开 Moving。
	 * 底层转发 ShouldExitMoving()，不是速度为零判断。
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

	/** WalkRun → TurnBack：有输入且摄像机修正后的输入接近角色前向的反方向。 */
	UFUNCTION(BlueprintPure, Category = "Cond|Locomotion", meta = (BlueprintThreadSafe))
	bool Locomotion_WalkRun_To_TurnBack() const;

	// TurnBack 相位和第二段标记由逻辑层 FTurnBackPhaseProcessor 维护，经快照读取；
	// Back → WalkRun 的完整动画播放条件由 AnimBP 自己使用动画时间节点判断。

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

	/** 相对 Actor 当前水平朝向的平滑移动方向 X（右）和 Y（前），供 AnimBP BlendSpace 使用。 */
	UPROPERTY(BlueprintReadOnly, Category = "State|Locomotion")
	float AnimBlendX = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "State|Locomotion")
	float AnimBlendY = 0.f;

	/** RM_PosX/RM_PosY 差分得到的原始曲线分量速度（cm/s，X=左右、Y=前后）。 */
	UPROPERTY(BlueprintReadOnly, Category = "State|RootMotion")
	FVector AnimCurveVelocity = FVector::ZeroVector;

	/** RM_PosX/RM_PosY 差分速度的归一化原始曲线分量方向；MotionDriver 转换为 UE 局部 X=前、Y=右。 */
	UPROPERTY(BlueprintReadOnly, Category = "State|RootMotion")
	FVector AnimCurveVelocityDirection = FVector::ZeroVector;

	/** 动画曲线速度方向角（度）。 */
	UPROPERTY(BlueprintReadOnly, Category = "State|RootMotion")
	float AnimCurveVelocityAngle = 0.f;

	/** 角色实际水平速度的世界空间单位方向。 */
	UPROPERTY(BlueprintReadOnly, Category = "State|Locomotion")
	FVector ActualVelocityDirection = FVector::ZeroVector;

	/** 角色实际速度相对 Actor 的 BlendSpace 分量：X=右，Y=前。 */
	UPROPERTY(BlueprintReadOnly, Category = "State|Locomotion")
	float ActualVelocityBlendX = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "State|Locomotion")
	float ActualVelocityBlendY = 0.f;

	/** 角色实际速度相对 Actor 的方向角（度）：0=前，+90=右。 */
	UPROPERTY(BlueprintReadOnly, Category = "State|Locomotion")
	float ActualVelocityAngle = 0.f;

	/** 逻辑参数阶段已消费 TurnBack 的 CanYaw Notify；true 后 AnimBP 只读该输入接管许可。 */
	UPROPERTY(BlueprintReadOnly, Category = "State|TurnBack")
	bool bCanYaw = false;

	/** 当前是否已经进入逻辑第二段；AnimBP 只读逻辑标记。 */
	UPROPERTY(BlueprintReadOnly, Category = "State|TurnBack")
	bool bTurnBackSecondSegment = false;

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
	 * 不绑定具体角色类的好处是新旧角色基类（迁移期间共存）都能用同一个 AnimBP，
	 * 没有项目 CMC 的角色只会得到全默认的快照，不会崩。
	 */
	TWeakObjectPtr<ACharacter> Owner;

	/**
	 * 本帧是否已由外部驱动过。
	 *
	 * 阶段 5 起**不再有外部驱动方** —— 旧 `FCharacterControlPipeline::PublishAnimation`
	 * 会在逻辑全部算完后显式调 `PipelineDrive`，那条链路随 Pipeline 退役。
	 * 现在统一走引擎的 `NativeUpdateAnimation`。
	 *
	 * 保留这个标记与 `PipelineDrive` 是为了给阶段 6 留出手动驱动的入口：
	 * 曲线采样必须在动画求值之后、移动提交之前发生，届时可能需要显式控制时机。
	 */
	bool bDrivenByPipeline = false;

	FZZZAnimSnapshotCapture SnapshotCapture;
	FZZZLocomotionDecisions LocomotionDecisions;
	FZZZLocomotionEvents LocomotionEvents;
};
