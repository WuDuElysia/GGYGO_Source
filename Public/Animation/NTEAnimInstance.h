/**
 * @file NTEAnimInstance.h
 * @brief NTE 风格移动动画的 C++ 决策层
 *
 * 设计哲学：拓扑在蓝图，决策在 C++，求值在 AnimGraph。
 *
 * 本类只提供五类产物供 AnimBP 状态机与 AnimGraph 引用，不维护任何状态机循环：
 *   1. 决策函数（UFUNCTION const → bool）：被蓝图过渡条件 Can Enter Transition 引用
 *   2. 状态回调（UFUNCTION → void）：被蓝图状态的 On Entry 引用
 *   3. 输出变量（UPROPERTY BlueprintReadOnly）：被 AnimGraph 节点 Bind
 *   4. 快照（FAnimSnapshot）：游戏线程抓取，worker 线程只读
 *   5. 配置（FLocomotionAnimSet / FLocomotionTuning）：美术在细节面板填值
 *
 * 线程模型：
 *   NativeUpdateAnimation           游戏线程，唯一能安全读 Owner 的位置，写快照、选一次性资产
 *   NativeThreadSafeUpdateAnimation worker 线程，只读快照，算数值型输出变量
 *   决策函数                        worker 线程，全部 const 且只读快照，绝不触碰 Actor
 *
 * 谁在哪个状态、什么时候切，全部由 UE 的 AnimBP 状态机节点管理。
 * C++ 只在被状态机问到"这条过渡现在能不能走"时回答 true / false。
 */
#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "StateMachine/CharacterStateType.h"  // EMovementGait
#include "Animation/LocomotionConfig.h"       // EAnimFoot, FLocomotionAnimSet, FLocomotionTuning
#include "Animation/Decisions/MainMovementDecisions.h"
#include "Animation/Decisions/GroundedDecisions.h"
#include "Animation/Decisions/LocomotionDecisions.h"
#include "Animation/Decisions/DirectionDecisions.h"
#include "Animation/Decisions/DetailDecisions.h"
#include "Animation/Decisions/StopGaitDecisions.h"
#include "Animation/Decisions/CyclesDecisions.h"
#include "Animation/Decisions/MotionMatchDecisions.h"
#include "Animation/Decisions/FullBodyIKDecisions.h"
#include "NTEAnimInstance.generated.h"

// 前向声明（减少编译依赖）
class ABaseCharacter;
class UBlendSpace;
class UAnimSequence;

/**
 * 动画决策快照
 *
 * 游戏线程在 NativeUpdateAnimation 中从 Owner_Character 与 RuntimeData 抓取，
 * worker 线程的输出计算与全部决策函数只读该快照，从而无需接触 Actor 即可并行执行。
 *
 * 每个字段的注释标注其数据来源映射。纯 C++ 结构体，不参与反射。
 */
struct FAnimSnapshot
{
	/** 移动意图（摇杆是否推开） ← ABaseCharacter::IsMoving() */
	bool bWantMove = false;

	/**
	 * 停步动画是否已播完（动画层自算，非外部推入）。
	 * 由 AnimInstance 用"停步计时器 vs 停步动画时长"计算：进 Stop 那刻起计时，累计 ≥ 停步动画长度即为真。
	 * 供 Stop→NotMoving 出边判定"播完才走"，避免 return true 导致的秒过渡（停步动画被瞬间跳过）。
	 */
	bool bStopFinished = false;

	/**
	 * 起步动画是否已播完（动画层自算，非外部推入）。
	 * 由 AnimInstance 用"起步计时器 vs 起步动画时长"计算：起步那刻起计时，累计 ≥ 起步动画长度即为真。
	 * 供 EnterMoveState→Moving 判定"起步动画播完才进移动循环"，取代被 FModel 剥掉的 ExitEnterMoveStateCurve。
	 */
	bool bEnterFinished = false;

	/** 是否在地面 ← RuntimeData.bIsGrounded */
	bool bGrounded = true;

	/** 是否刚落地 ← 恒 false（无公开来源，需 BaseCharacter 新增 getter，超出本层范围） */
	bool bJustLanded = false;

	/** 跳跃意图 ← 恒 false（RuntimeData 无对应字段，管线暴露来源前保持 false） */
	bool bWantJump = false;

	/** 滑翔意图 ← 恒 false（RuntimeData 无对应字段，管线暴露来源前保持 false） */
	bool bWantGlide = false;

	/** 是否处于水中 ← 恒 false（RuntimeData 无对应字段，管线暴露来源前保持 false） */
	bool bInWater = false;

	/** 水平速度（cm/s） ← ABaseCharacter::GetCurrentSpeed() */
	float Speed = 0.f;

	/** 垂直速度（cm/s） ← ABaseCharacter::GetVelocity().Z */
	float VerticalVelocity = 0.f;

	/** 落地冲击速度（cm/s） ← 本帧 |GetVelocity().Z| 近似 */
	float LandImpactSpeed = 0.f;

	/** 移动角度（度，[-180,180]） ← ABaseCharacter::GetMoveAngle() */
	float MoveAngleDeg = 0.f;

	/** 当前循环归一化相位（[0,1)） ← SyncGroup 相位查询 */
	float LocomotionPhase = 0.f;

	/** 解析步态 ← ABaseCharacter::GetResolvedGait() */
	EMovementGait DesiredGait = EMovementGait::None;

	/** 当前支撑脚 ← QueryCurrentFoot()（由 LocomotionPhase 推导） */
	EAnimFoot CurrentFoot = EAnimFoot::Left;

	// ---- 地面移动层决策数据 ----

	bool bEntryMovingOrNotMoving = false;
	bool bSkillInterruptMove = false;
	bool bIsPatrolMoveAnim = false;
	bool bIsPatrolState = false;
	bool bIsCanRunStop = false;
	bool bShouldMove = false;
	bool bIsSprintStop = false;
	bool bNeedMotionMatching = false;
	bool bIsLeftFootC = false;
	bool bNotMovingToMoving = false;
	bool bIsHasInStandIdlePose = false;
	bool bMovingToNotMoving = false;
	bool bIsCanEnterMoveState = false;
	bool bIsHoldingHands = false;
	bool bIsIgnoreMoveInput = false;
	EMovementGait Gait = EMovementGait::None;
	float VelocityLength = 0.f;
	float LastInputDirectionAngle = 0.f;

	// ============================================================
	// 顶层决策补全字段（Layer 1 Main / Layer 2 Grounded / MM / IK）
	//
	// 供 anim-toplevel-decisions-completion 新增的 70 个决策函数读取。
	// 绝大多数字段当前无确认的外部来源（NTE 蓝图变量表未暴露 getter），
	// 依 Requirement 6.7 给安全默认（bool→false、int→0、float→0.f），
	// 待管线暴露来源后再接线。bWantJump / bShouldMove 复用上方既有字段。
	// ============================================================

	// ---- 整型/枚举状态字段（int32，默认 0） ----

	/** 移动动画状态枚举值 ← 无确认来源，默认 0 */
	int32 MoveAnimState = 0;

	/** 藤蔓子动画状态 ← 无确认来源，默认 0 */
	int32 VinesSubAnimState = 0;

	/** 当前跳跃计数（二段跳判定） ← 无确认来源，默认 0 */
	int32 JumpCurrentCount = 0;

	/** 地面动画子状态（0x0 Normal / 0x1 Landed / 0x7 FromRoll / 0x8 Vault /
	    0xA VinesOver / 0xB RunOnWallsOver / 0xC SprintVinesOver）
	    ← 无确认来源，默认 0x0 Normal */
	int32 GroundAnimState = 0;

	/** 翻越子状态 ← 无确认来源，默认 0 */
	int32 VaultSubState = 0;

	// ---- 浮点字段（float，默认 0.f） ----

	/** 水平面 (XY) 速度标量长度 (cm/s) ← 可由 GetVelocity().Size2D() 接线，暂默认 0 */
	float Velocity2DLength = 0.f;

	// ---- Main 层布尔字段（默认 false） ----

	/** 是否使用起跳前摇（#346） ← 无确认来源 */
	bool bUseJumpTakeOff = false;

	/** 二段跳触发（#349） ← 无确认来源 */
	bool bSecondJump = false;

	/** 强制跳跃（#349） ← 无确认来源 */
	bool bForceJump = false;

	/** 离地意图（#345） ← 无确认来源 */
	bool bWantsToLeaveGround = false;

	/** 空中动作意图（#350） ← 无确认来源 */
	bool bWantsAirAction = false;

	/** 正在落地（#351/#354） ← 无确认来源 */
	bool bIsLanding = false;

	/** 到达跳跃顶点（#353） ← 无确认来源 */
	bool bJumpApexReached = false;

	/** 处于藤蔓（#356） ← 无确认来源 */
	bool bOnVines = false;

	/** 翻越触发（#357） ← 无确认来源 */
	bool bVaultTriggered = false;

	/** 开始驾驶（#359） ← 无确认来源 */
	bool bStartedDriving = false;

	/** 返回地面 fallback（#360） ← 无确认来源 */
	bool bReturnToGround = false;

	/** 可二段跳（#361） ← 无确认来源 */
	bool bCanDoubleJump = false;

	/** 空中特殊模式（#362） ← 无确认来源 */
	bool bSpecialModeInAir = false;

	/** 离开藤蔓（#367） ← 无确认来源 */
	bool bLeaveVines = false;

	/** 藤蔓动画完成 AutoRule（#368） ← 无确认来源 */
	bool bVinesAnimComplete = false;

	/** 翻越退出条件（#369） ← 无确认来源 */
	bool bVaultExitCondition = false;

	/** 停止滑翔（#371） ← 无确认来源 */
	bool bStopGliding = false;

	/** 落地中（#371/#372/#375） ← 无确认来源 */
	bool bLanding = false;

	/** 强制退出滑翔（#372） ← 无确认来源 */
	bool bForceExitGliding = false;

	/** 恢复滑翔（#374） ← 无确认来源 */
	bool bResumeGliding = false;

	/** 离开水面（#376） ← 无确认来源 */
	bool bLeaveWater = false;

	/** 停止驾驶（#377） ← 无确认来源 */
	bool bStopDriving = false;

	// ---- Main 层 provisional ⚠️ 待验证布尔字段（默认 false，Req 2A.2） ----

	/** ⚠️ 待验证（#352）：郊狼时间跳，语义由 JSON 推导，需蓝图侧验证 ← 无确认来源 */
	bool bCoyoteTimeJump = false;

	/** ⚠️ 待验证（#355）：从空中进入，语义由 JSON 推导，需蓝图侧验证 ← 无确认来源 */
	bool bEnteredFromAir = false;

	/** ⚠️ 待验证（#365）：起跳前摇完成，语义由 JSON 推导，需蓝图侧验证 ← 无确认来源 */
	bool bTakeOffComplete = false;

	/** ⚠️ 待验证（#366）：取消跳跃，语义由 JSON 推导，需蓝图侧验证 ← 无确认来源 */
	bool bCancelJump = false;

	/** ⚠️ 待验证（#373）：过渡完成，语义由 JSON 推导，需蓝图侧验证 ← 无确认来源 */
	bool bTransitionComplete = false;

	// ---- Grounded 层布尔字段（默认 false） ----

	/** 可停留地面（#468） ← 无确认来源 */
	bool bCanStayingTheGround = false;

	/** 正在播放任意蒙太奇（#472） ← 可由 IsAnyMontagePlaying() 接线，暂默认 false */
	bool bIsPlayingAnyMontage = false;

	/** 滑翔落地标记（#472/#476/#482/#483） ← 无确认来源 */
	bool bIsGlidingLanded = false;

	/** 翻越结束到停止（左）（#489） ← 无确认来源 */
	bool VaultEndToStopL = false;

	/** 翻越结束到停止（右）（#489） ← 无确认来源 */
	bool VaultEndToStopR = false;

	// ---- MM/IK 层布尔字段（默认 false） ----

	/** 强制退出 MotionMatching（#44） ← 无确认来源 */
	bool bForceExitMotionMatching = false;

	/** MotionMatching 完成（#45） ← 无确认来源 */
	bool bMotionMatchComplete = false;

	/** MotionMatching 激活中（#45） ← 无确认来源 */
	bool bMotionMatchActive = false;

	/** 需要全身 IK（#64/#65） ← 无确认来源 */
	bool bFullBodyIKNeeded = false;
};

/**
 * 快照来源聚合数据
 *
 * 聚合构建快照所需的全部来源字段，供纯函数 BuildSnapshot 消费。
 * 将快照填充逻辑与引擎运行时解耦，使映射逻辑可在不实例化 Actor 的情况下离线测试。
 * 纯 C++ POD 结构体，不参与反射。字段镜像快照的数据来源。
 */
struct FAnimSourceData
{
	/** 移动意图（摇杆是否推开） */
	bool bWantMove = false;

	/** 是否在地面 */
	bool bGrounded = true;

	/** 是否刚落地 */
	bool bJustLanded = false;

	/** 跳跃意图 */
	bool bWantJump = false;

	/** 滑翔意图 */
	bool bWantGlide = false;

	/** 是否处于水中 */
	bool bInWater = false;

	/** 水平速度（cm/s） */
	float Speed = 0.f;

	/** 垂直速度（cm/s） */
	float VerticalVelocity = 0.f;

	/** 落地冲击速度（cm/s） */
	float LandImpactSpeed = 0.f;

	/** 移动角度（度，[-180,180]） */
	float MoveAngleDeg = 0.f;

	/** 当前循环归一化相位（[0,1)） */
	float LocomotionPhase = 0.f;

	/** 解析步态 */
	EMovementGait DesiredGait = EMovementGait::None;

	/** 当前支撑脚 */
	EAnimFoot CurrentFoot = EAnimFoot::Left;

	// ---- 地面移动层决策数据 ----

	bool bEntryMovingOrNotMoving = false;
	bool bSkillInterruptMove = false;
	bool bIsPatrolMoveAnim = false;
	bool bIsPatrolState = false;
	bool bIsCanRunStop = false;
	bool bShouldMove = false;
	bool bIsSprintStop = false;
	bool bNeedMotionMatching = false;
	bool bIsLeftFootC = false;
	bool bNotMovingToMoving = false;
	bool bIsHasInStandIdlePose = false;
	bool bMovingToNotMoving = false;
	bool bIsCanEnterMoveState = false;
	bool bIsHoldingHands = false;
	bool bIsIgnoreMoveInput = false;
	EMovementGait Gait = EMovementGait::None;
	float VelocityLength = 0.f;
	float LastInputDirectionAngle = 0.f;

	// ============================================================
	// 顶层决策补全字段镜像（Layer 1 Main / Layer 2 Grounded / MM / IK）
	//
	// 镜像 FAnimSnapshot 于 Requirement 6 中新增的全部字段（Req 7.1），
	// 名称、类型、默认值与快照一致。整型状态字段统一用 int32（Req 7.6）。
	// bWantJump / bShouldMove 复用上方既有字段，不重定义。
	// ============================================================

	// ---- 整型/枚举状态字段（int32，默认 0） ----

	/** 移动动画状态枚举值 */
	int32 MoveAnimState = 0;

	/** 藤蔓子动画状态 */
	int32 VinesSubAnimState = 0;

	/** 当前跳跃计数（二段跳判定） */
	int32 JumpCurrentCount = 0;

	/** 地面动画子状态（0x0 Normal / 0x1 Landed / 0x7 FromRoll / 0x8 Vault /
	    0xA VinesOver / 0xB RunOnWallsOver / 0xC SprintVinesOver） */
	int32 GroundAnimState = 0;

	/** 翻越子状态 */
	int32 VaultSubState = 0;

	// ---- 浮点字段（float，默认 0.f） ----

	/** 水平面 (XY) 速度标量长度 (cm/s) */
	float Velocity2DLength = 0.f;

	// ---- Main 层布尔字段（默认 false） ----

	/** 是否使用起跳前摇（#346） */
	bool bUseJumpTakeOff = false;

	/** 二段跳触发（#349） */
	bool bSecondJump = false;

	/** 强制跳跃（#349） */
	bool bForceJump = false;

	/** 离地意图（#345） */
	bool bWantsToLeaveGround = false;

	/** 空中动作意图（#350） */
	bool bWantsAirAction = false;

	/** 正在落地（#351/#354） */
	bool bIsLanding = false;

	/** 到达跳跃顶点（#353） */
	bool bJumpApexReached = false;

	/** 处于藤蔓（#356） */
	bool bOnVines = false;

	/** 翻越触发（#357） */
	bool bVaultTriggered = false;

	/** 开始驾驶（#359） */
	bool bStartedDriving = false;

	/** 返回地面 fallback（#360） */
	bool bReturnToGround = false;

	/** 可二段跳（#361） */
	bool bCanDoubleJump = false;

	/** 空中特殊模式（#362） */
	bool bSpecialModeInAir = false;

	/** 离开藤蔓（#367） */
	bool bLeaveVines = false;

	/** 藤蔓动画完成 AutoRule（#368） */
	bool bVinesAnimComplete = false;

	/** 翻越退出条件（#369） */
	bool bVaultExitCondition = false;

	/** 停止滑翔（#371） */
	bool bStopGliding = false;

	/** 落地中（#371/#372/#375） */
	bool bLanding = false;

	/** 强制退出滑翔（#372） */
	bool bForceExitGliding = false;

	/** 恢复滑翔（#374） */
	bool bResumeGliding = false;

	/** 离开水面（#376） */
	bool bLeaveWater = false;

	/** 停止驾驶（#377） */
	bool bStopDriving = false;

	// ---- Main 层 provisional ⚠️ 待验证布尔字段（默认 false，Req 2A.2） ----

	/** ⚠️ 待验证（#352）：郊狼时间跳 */
	bool bCoyoteTimeJump = false;

	/** ⚠️ 待验证（#355）：从空中进入 */
	bool bEnteredFromAir = false;

	/** ⚠️ 待验证（#365）：起跳前摇完成 */
	bool bTakeOffComplete = false;

	/** ⚠️ 待验证（#366）：取消跳跃 */
	bool bCancelJump = false;

	/** ⚠️ 待验证（#373）：过渡完成 */
	bool bTransitionComplete = false;

	// ---- Grounded 层布尔字段（默认 false） ----

	/** 可停留地面（#468） */
	bool bCanStayingTheGround = false;

	/** 正在播放任意蒙太奇（#472） */
	bool bIsPlayingAnyMontage = false;

	/** 滑翔落地标记（#472/#476/#482/#483） */
	bool bIsGlidingLanded = false;

	/** 翻越结束到停止（左）（#489） */
	bool VaultEndToStopL = false;

	/** 翻越结束到停止（右）（#489） */
	bool VaultEndToStopR = false;

	// ---- MM/IK 层布尔字段（默认 false） ----

	/** 强制退出 MotionMatching（#44） */
	bool bForceExitMotionMatching = false;

	/** MotionMatching 完成（#45） */
	bool bMotionMatchComplete = false;

	/** MotionMatching 激活中（#45） */
	bool bMotionMatchActive = false;

	/** 需要全身 IK（#64/#65） */
	bool bFullBodyIKNeeded = false;
};

/**
 * NTE 风格移动动画的 C++ 决策层
 *
 * 继承自 UAnimInstance，以 GGYGO_API 导出。仅提供决策函数、状态回调、输出变量、
 * 快照与配置，不含 CurrentState 字段、状态机循环或状态切换逻辑。
 *
 * 使用方式：
 *   1. 创建 AnimBP 蓝图，父类设为本类
 *   2. 在细节面板填 AnimSet（key→value）与 Tuning 数值
 *   3. 蓝图状态机过渡条件引用决策函数，AnimGraph 节点 Bind 输出变量
 */
UCLASS()
class GGYGO_API UNTEAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	// ============================================================
	// AnimInstance 生命周期入口（三线程阶段）
	// ============================================================

	/**
	 * 初始化入口（UE 动画系统调用一次）
	 * 缓存 Owner 弱引用，从 AnimSet 初始化常量循环资产输出变量（如 Out_IdleSeq）
	 */
	virtual void NativeInitializeAnimation() override;

	/**
	 * 游戏线程每帧更新入口
	 * 唯一能安全读 Owner 的位置：抓取快照、按脚+步态选一次性资产
	 * @param DeltaSeconds 本帧时间增量（秒）
	 */
	virtual void NativeUpdateAnimation(float DeltaSeconds) override;

	/**
	 * worker 线程每帧更新入口（可与游戏线程并行）
	 * 只读快照，计算数值型输出变量（WalkRun/Stride/Lean/BlendSpace）
	 * @param DeltaSeconds 本帧时间增量（秒）
	 */
	virtual void NativeThreadSafeUpdateAnimation(float DeltaSeconds) override;

	/**
	 * 管线主动驱动（BaseCharacter::Tick 末尾调用）
	 *
	 * 在逻辑管线全部完成之后，显式抓取快照并刷新一次性资产。
	 * 此后引擎调 NativeUpdateAnimation 时检测到标记，跳过重复工作。
	 */
	void PipelineDrive();

	// ============================================================
	// 决策函数 — 第1层 MainMovement（顶层运动模式路由）
	// ============================================================

	/** 切换到下落：不在地面且垂直速度小于 0 且无滑翔意图 */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool Main_To_Fall() const;

	/** 切换到跳跃：有跳跃意图 */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool Main_To_Jump() const;

	/** 切换到地面：在地面 */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool Main_To_Grounded() const;

	/** 切换到滑翔：不在地面且有滑翔意图 */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool Main_To_Gliding() const;

	/** 切换到游泳：处于水中 */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool Main_To_Swimming() const;

	// ============================================================
	// 决策函数 — 第1层 MainMovement 顶层过渡补全（NTE_05 B.2.7 #345–#378）
	//
	// 33 个 Shell，单行委托 MainDecisions。函数名与 B.2.7 明细表逐字一致。
	// ============================================================

	// ---- Grounded 子机过渡（#345–#349）----

	/** #345 Grounded → MovementState：bWantsToLeaveGround */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_Grounded_To_MovementState() const;

	/** #346 Grounded → JumpTakeOff：bWantJump && bUseJumpTakeOff */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_Grounded_To_JumpTakeOff() const;

	/** #347 Grounded → Jump：bWantJump && !bUseJumpTakeOff */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_Grounded_To_Jump() const;

	/** #349 Grounded → Jump(Instant)：bSecondJump || bForceJump */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_Grounded_To_Jump_Instant() const;

	// ---- Fall 子机过渡（#350–#352）----

	/** #350 Fall → InAir：bWantsAirAction */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_Fall_To_InAir() const;

	/** #351 Fall → Land：bIsLanding */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_Fall_To_Land() const;

	/** #352 ⚠️ 待验证 Fall → Jump：provisional bCoyoteTimeJump */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_Fall_To_Jump() const;

	// ---- Jump 子机过渡（#353–#354）----

	/** #353 Jump → InAir：bJumpApexReached */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_Jump_To_InAir() const;

	/** #354 Jump → Land：bIsLanding */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_Jump_To_Land() const;

	// ---- MovementState (<-MS->) 分派（#355–#360）----

	/** #355 ⚠️ 待验证 MS → InAir：provisional bEnteredFromAir */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_MS_To_InAir() const;

	/** #356 MS → Vines：bOnVines */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_MS_To_Vines() const;

	/** #357 MS → VaultToAir：bVaultTriggered */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_MS_To_VaultToAir() const;

	/** #358 MS → Swimming：bInWater */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_MS_To_Swimming() const;

	/** #359 MS → Driving：bStartedDriving */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_MS_To_Driving() const;

	/** #360 MS → Grounded：bReturnToGround */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_MS_To_Grounded() const;

	// ---- InAir 导管（#361–#363）----

	/** #361 InAir → Jump：bCanDoubleJump */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_InAir_To_Jump() const;

	/** #362 InAir → MS：bSpecialModeInAir */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_InAir_To_MS() const;

	/** #363 InAir → Fall：true（fallback 单出边） */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_InAir_To_Fall() const;

	// ---- Land 导管（#364）----

	/** #364 Land → Grounded：true（唯一出边） */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_Land_To_Grounded() const;

	// ---- TakeOff 子机（#365–#366）----

	/** #365 ⚠️ 待验证 TakeOff → Jump：provisional bTakeOffComplete */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_TakeOff_To_Jump() const;

	/** #366 ⚠️ 待验证 TakeOff → MS：provisional bCancelJump */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_TakeOff_To_MS() const;

	// ---- Vines 子机（#367–#368）----

	/** #367 Vines → MS：bLeaveVines */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_Vines_To_MS() const;

	/** #368 Vines → MS (AutoRule)：bVinesAnimComplete */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_Vines_To_MS_Auto() const;

	// ---- Vault 子机（#369–#370）----

	/** #369 Vault → MS：bVaultExitCondition */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_Vault_To_MS() const;

	/** #370 Vault → MS (AutoRule)：true */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_Vault_To_MS_Auto() const;

	// ---- Gliding / GlidingToFall（#371–#375）----

	/** #371 Gliding → GlidingToFall：bStopGliding && !bLanding */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_Gliding_To_GlidingToFall() const;

	/** #372 Gliding → OutGliding：bLanding || bForceExitGliding */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_Gliding_To_OutGliding() const;

	/** #373 ⚠️ 待验证 GlidingToFall → Fall：provisional bTransitionComplete */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_GlidingToFall_To_Fall() const;

	/** #374 GlidingToFall → Gliding：bResumeGliding */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_GlidingToFall_To_Gliding() const;

	/** #375 GlidingToFall → OutGliding：bLanding */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_GlidingToFall_To_OutGliding() const;

	// ---- Swimming / Driving（#376–#377）----

	/** #376 Swimming → MS：bLeaveWater */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_Swimming_To_MS() const;

	/** #377 Driving → MS：bStopDriving */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_Driving_To_MS() const;

	// ---- OutGliding 导管（#378）----

	/** #378 OutGliding → MS：true（唯一出边） */
	UFUNCTION(BlueprintPure, Category = "Cond|Main", meta = (BlueprintThreadSafe))
	bool MainMove_OutGliding_To_MS() const;

	// ============================================================
	// 决策函数 — 第2层 MainGrounded（地面入口/落地）
	// ============================================================

	/** 刚落地：标记刚落地且落地冲击速度大于 Tuning.JumpLandedThreshold */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool Grounded_Just_Landed() const;

	// ============================================================
	// 决策函数 — 第2层 MainGrounded 顶层过渡补全（NTE_05 B.2.8 #468–#500）
	//
	// 33 个 Shell，单行委托 GroundedDecisions。函数名与 B.2.8 明细表逐字一致。
	// ============================================================

	// ---- Entry 分派（#468–#477）----

	/** #468 Entry → VaultContinue：(GroundAnimState==0x8)&&(VaultSubState!=0x0)&&bCanStayingTheGround */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_Entry_To_VaultContinue() const;

	/** #469 Entry → Conduit：true */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_Entry_To_Conduit() const;

	/** #470 Entry → FromRoll：GroundAnimState==0x7 */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_Entry_To_FromRoll() const;

	/** #471 Entry → LandedMobile：(GroundAnimState==0x1)&&bShouldMove */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_Entry_To_LandedMobile() const;

	/** #472 Entry → Landed：(GroundAnimState==0x1)&&!bShouldMove&&!bIsPlayingAnyMontage&&!bIsGlidingLanded */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_Entry_To_Landed() const;

	/** #473 Entry → MainGS：true（fallback） */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_Entry_To_MainGS() const;

	/** #474 Entry → VinesOver：GroundAnimState==0xA */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_Entry_To_VinesOver() const;

	/** #475 Entry → RunOnWallsOver：GroundAnimState==0xB */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_Entry_To_RunOnWallsOver() const;

	/** #476 Entry → LandedStationary：bIsGlidingLanded&&(Velocity2DLength<10)&&(GroundAnimState==0x1) */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_Entry_To_LandedStationary() const;

	/** #477 Entry → SprintVinesOver：GroundAnimState==0xC */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_Entry_To_SprintVinesOver() const;

	// ---- FromRoll（#478–#479）----

	/** #478 FromRoll → MainGS：true */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_FromRoll_To_MainGS() const;

	/** #479 FromRoll → RollToRun：GetAnimTimeRemainingSafe(410,2)==0 */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_FromRoll_To_RollToRun() const;

	// ---- LandedStationary（#480–#483）----

	/** #480 LandedStat → MainGS (AutoRule)：true */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_LandedStat_To_MainGS_Auto() const;

	/** #481 LandedStat → StatToMove：Velocity2DLength>=10 */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_LandedStat_To_StatToMove() const;

	/** #482 LandedStat → LandedMob：(GroundAnimState==0x1)&&(Velocity2DLength>10)&&!bIsGlidingLanded */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_LandedStat_To_LandedMob() const;

	/** #483 LandedStat → MainGS：(GroundAnimState==0x1)&&(Velocity2DLength>10)&&bIsGlidingLanded */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_LandedStat_To_MainGS() const;

	// ---- LandedMobile（#484–#485）----

	/** #484 LandedMob → MainGS (AutoRule)：true */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_LandedMob_To_MainGS_Auto() const;

	/** #485 LandedMob → MainGS：true */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_LandedMob_To_MainGS() const;

	// ---- StatToMove（#486–#487）----

	/** #486 StatToMove → MainGS (AutoRule)：true */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_StatToMove_To_MainGS_Auto() const;

	/** #487 StatToMove → MainGS：!bShouldMove && (GetAnimTimeRemainingSafe(410,5)<0.2) */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_StatToMove_To_MainGS() const;

	// ---- RollToRun（#488–#489）----

	/** #488 RollToRun → MainGS (AutoRule)：true */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_RollToRun_To_MainGS_Auto() const;

	/** #489 RollToRun → MainGS：VaultEndToStopL || VaultEndToStopR */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_RollToRun_To_MainGS() const;

	// ---- Landed（#490–#491）----

	/** #490 Landed → LandedStat：(GetAnimTimeRemainingSafe(410,7)==0) && !bShouldMove */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_Landed_To_LandedStat() const;

	/** #491 Landed → StatToMove：true */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_Landed_To_StatToMove() const;

	// ---- VaultContinue（#492）----

	/** #492 VaultCont → MainGS：(GroundAnimState==0x0) || (VaultSubState==0x0) */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_VaultCont_To_MainGS() const;

	// ---- VinesOver（#493–#494）----

	/** #493 VinesOver → MainGS (AutoRule)：true */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_VinesOver_To_MainGS_Auto() const;

	/** #494 VinesOver → MainGS：GroundAnimState==0x0 */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_VinesOver_To_MainGS() const;

	// ---- RunOnWallsOver（#495–#496）----

	/** #495 RunWalls → MainGS (AutoRule)：true */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_RunWalls_To_MainGS_Auto() const;

	/** #496 RunWalls → MainGS：(GroundAnimState==0x0) || (GroundAnimState==0x1) */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_RunWalls_To_MainGS() const;

	// ---- Conduit（#497–#498）----

	/** #497 Conduit → LandedMob：true */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_Conduit_To_LandedMob() const;

	/** #498 Conduit → Landed：true */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_Conduit_To_Landed() const;

	// ---- SprintVinesOver（#499–#500）----

	/** #499 SprintVines → MainGS：GroundAnimState==0x0 */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_SprintVines_To_MainGS() const;

	/** #500 SprintVines → MainGS (AutoRule)：true */
	UFUNCTION(BlueprintPure, Category = "Cond|Grounded", meta = (BlueprintThreadSafe))
	bool MG_SprintVines_To_MainGS_Auto() const;

	// ============================================================
	// 决策函数 — 第3层 LocomotionStates（待机/进入/移动/停止）
	//
	// 全部 const 且只读快照，被蓝图过渡条件 Can Enter Transition 引用。
	// ============================================================

	/** 待机 → 进入：有移动意图且在地面 */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco", meta = (BlueprintThreadSafe))
	bool Loco_NotMoving_To_Enter() const;

	/** 进入 → 移动：Enter 动画播完（供不用 Auto Rule 时引用） */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco", meta = (BlueprintThreadSafe))
	bool Loco_Enter_To_Moving() const;

	/** 进入 → 待机：无移动意图（起步被打断） */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco", meta = (BlueprintThreadSafe))
	bool Loco_Enter_To_NotMoving() const;

	/** 移动 → 左脚停：无移动意图且速度低于阈值且当前左脚支撑 */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco", meta = (BlueprintThreadSafe))
	bool Loco_Moving_To_LeftStop() const;

	/** 移动 → 右脚停：无移动意图且速度低于阈值且当前右脚支撑 */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco", meta = (BlueprintThreadSafe))
	bool Loco_Moving_To_RightStop() const;

	/** 停止 → 移动：重新有移动意图（急停被打断） */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco", meta = (BlueprintThreadSafe))
	bool Loco_Stop_To_Moving() const;

	/** 停止 → 待机：Stop 动画播完（供不用 Auto Rule 时引用） */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco", meta = (BlueprintThreadSafe))
	bool Loco_Stop_To_NotMoving() const;

	// ============================================================
	// 决策函数 — 第3层 LocomotionStatesMachine（NTE Layer 3 全拓扑）
	//
	// 43 个决策委托，覆盖 NTE_05 delegate #662–#704。
	// 全部 const 且只读 Snap + GetCurveValue，worker 线程安全。
	// ============================================================

	// ---- Conduit entry (#662-#664) ----

	/** #662 Conduit → Moving(Sprint)：Sprint 曲线 > 0.1 */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_Conduit_To_Moving_Sprint() const;

	/** #663 Conduit → Moving：有移动意图或技能打断移动 */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_Conduit_To_Moving() const;

	/** #664 Conduit → NotMoving：无移动意图且无技能打断 */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_Conduit_To_NotMoving() const;

	// ---- NotMoving transitions (#665-#668) ----

	/** #665 NotMoving → Moving (AutoRule guard) */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_NotMoving_To_Moving_Auto() const;

	/** #666 NotMoving → Conduit5：有站立 Idle Pose 且 NotMoving→Moving 标记 */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_NotMoving_To_Conduit5() const;

	/** #667 NotMoving → Moving (Alt)：站立 Idle Pose 且转换标记变体 */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_NotMoving_To_Moving_Alt() const;

	/** #668 NotMoving → Stop_MM：需要 MotionMatching */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_NotMoving_To_Stop_MM() const;

	// ---- Moving transitions (#669-#671) ----

	/** #669 Moving → NotMoving (AutoRule guard, shared) */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_Moving_To_NotMoving_Auto() const;

	/** #670 Moving → NotMoving：可跑停且 Moving→NotMoving 标记 */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_Moving_To_NotMoving() const;

	/** #671 Moving → Conduit1：可跑停 + Moving→NotMoving + 技能打断 */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_Moving_To_Conduit1() const;

	// ---- Stop transitions (#672-#674) ----

	/** #672 Stop → NotMoving (AutoRule guard 1) */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_Stop_To_NotMoving_Auto1() const;

	/** #673 Stop → NotMoving (AutoRule guard 2) */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_Stop_To_NotMoving_Auto2() const;

	/** #674 Stop → Conduit4：巡逻移动 || 巡逻状态 || 应该移动 */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_Stop_To_Conduit4() const;

	// ---- Patrol cycle (#675-#691) ----

	/** #675 NotMoving_1 → Moving_1 (Patrol)：巡逻移动动画 && 巡逻状态 */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_NotMoving1_To_Moving1_Patrol() const;

	/** #676 NotMoving_1 → LeftStop_1：巡逻左脚停条件 */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_NotMoving1_To_LeftStop1() const;

	/** #677 NotMoving_1 → RightStop_1：巡逻右脚停条件 */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_NotMoving1_To_RightStop1() const;

	/** #678 NotMoving_1 → Moving_1 (Should)：巡逻状态 && 应该移动 */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_NotMoving1_To_Moving1_Should() const;

	/** #679 Moving_1 → NotMoving_1：可跑停 && 巡逻移动动画 && 巡逻状态 */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_Moving1_To_NotMoving1() const;

	/** #680 Moving_1 → CanStop_1：非巡逻条件（patrol 取反） */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_Moving1_To_CanStop1() const;

	/** #681 AutoRule fallback (shared, return true) */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_AutoRule_Fallback() const;

	/** #682 Conduit_2 → Moving_1 (Sprint)：Sprint 曲线 > 0.1 */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_Conduit2_To_Moving1_Sprint() const;

	/** #683 Conduit_2 → Moving_1：速度+移动意图 || ToMoving曲线 || sprintturnback曲线 */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_Conduit2_To_Moving1() const;

	/** #684 Conduit_2 → NotMoving_1：#683 取反 */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_Conduit2_To_NotMoving1() const;

	/** #685 LeftStop_1 → NotMoving_1 (AutoRule guard) */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_LeftStop1_To_NotMoving1_Auto() const;

	/** #686 Stop_1 → Moving_1 (Resume)：巡逻移动 || 巡逻状态 || 应该移动 (shared) */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_Stop1_To_Moving1_Resume() const;

	/** #687 RightStop_1 → NotMoving_1 (AutoRule guard) */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_RightStop1_To_NotMoving1_Auto() const;

	/** #688 CanStop_1 → Conduit_1_1_1：输入方向角绝对值 < 5° */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_CanStop1_To_Conduit1_1_1() const;

	/** #689 CanStop_1 → Conduit_1_2：输入方向角绝对值 < 5° */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_CanStop1_To_Conduit1_2() const;

	/** #690 StopConduit NoSprint：Sprint 曲线 ≤ 0.1 (shared) */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_StopConduit_NoSprint() const;

	/** #691 StopConduit Sprint：Sprint 曲线 > 0.1 (shared) */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_StopConduit_Sprint() const;

	// ---- Stop route (#692-#693) ----

	/** #692 Conduit_1 → Stop_A：无条件 */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_Conduit1_To_Stop_A() const;

	/** #693 Conduit_1 → Stop_B：无条件 */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_Conduit1_To_Stop_B() const;

	// ---- EnterMoveState (#694-#695) ----

	/** #694 EnterMove → Moving：ExitEnterMoveStateCurve != 0 || Gait != Run */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_EnterMove_To_Moving() const;

	/** #695 EnterMove → Conduit1：Moving→NotMoving && 可跑停 (shared) */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_EnterMove_To_Conduit1() const;

	// ---- Conduit_3 (#696-#697) ----

	/** #696 Conduit_3 → Moving：非(CanEnterMoveState && Run && !IgnoreInput && !HoldHands) */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_Conduit3_To_Moving() const;

	/** #697 Conduit_3 → EnterState：CanEnterMoveState && Run && !IgnoreInput && !HoldHands */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_Conduit3_To_EnterState() const;

	// ---- EnterState/EnterWalk (#698-#702) ----

	/** #698 EnterState → EnterMove：状态权重 >= 1.0 (播放完毕) */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_EnterState_To_EnterMove() const;

	/** #699 Conduit_5 → Conduit_3：Gait != Walk */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_Conduit5_To_Conduit3() const;

	/** #700 Conduit_5 → Moving_Walk：Gait == Walk && 牵手模式 */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_Conduit5_To_Moving_Walk() const;

	/** #701 Conduit_5 → EnterWalk：Gait == Walk && 非牵手 */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_Conduit5_To_EnterWalk() const;

	/** #702 EnterWalk → Moving：状态权重 >= 1.0 (播放完毕) */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_EnterWalk_To_Moving() const;

	// ---- Conduit_4 (#703-#704) ----

	/** #703 Conduit_4 → EnterState：CanEnterMoveState && Run && !SprintStop && !HoldHands && HasStandIdlePose */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_Conduit4_To_EnterState() const;

	/** #704 Conduit_4 → Moving：#703 取反 */
	UFUNCTION(BlueprintPure, Category = "Cond|Loco3", meta = (BlueprintThreadSafe))
	bool Loco3_Conduit4_To_Moving() const;

	// ============================================================
	// 决策函数 — 第3.5层 Direction Dispatcher（方向分派）
	// ============================================================

	/** 前向起步：移动角绝对值不大于 45°（简化四分区） */
	UFUNCTION(BlueprintPure, Category = "Cond|Enter", meta = (BlueprintThreadSafe))
	bool Enter_Dir_Forward() const;

	/** 左向起步：移动角小于 -45° 且不小于 -135°（delegate #646） */
	UFUNCTION(BlueprintPure, Category = "Cond|Enter", meta = (BlueprintThreadSafe))
	bool Enter_Dir_Left() const;

	/** 右向起步：移动角大于 45° 且不大于 135°（delegate #647） */
	UFUNCTION(BlueprintPure, Category = "Cond|Enter", meta = (BlueprintThreadSafe))
	bool Enter_Dir_Right() const;

	/** Direction Dispatcher → Forward Variant 分支（delegate #648） */
	UFUNCTION(BlueprintPure, Category = "Cond|Enter", meta = (BlueprintThreadSafe))
	bool Enter_Dir_Forward_Variant() const;

	/** 后向起步：移动角绝对值大于 135°（delegate #649） */
	UFUNCTION(BlueprintPure, Category = "Cond|Enter", meta = (BlueprintThreadSafe))
	bool Enter_Dir_Back() const;

	/** Enter 子机退出检查：进入动画播完可切出（delegate #650/#651/#652） */
	UFUNCTION(BlueprintPure, Category = "Cond|Enter", meta = (BlueprintThreadSafe))
	bool Enter_Exit_Check() const;

	// ============================================================
	// 决策函数 — 第3.5层 Forward Gait（§2.1.1.1.1）
	// ============================================================

	/** Forward Gait → Sprint 分支：步态等于 Sprint（delegate #597） */
	UFUNCTION(BlueprintPure, Category = "Cond|Enter", meta = (BlueprintThreadSafe))
	bool EnterForward_Is_Sprint() const;

	/** Forward Gait → Run 分支：步态等于 Run 或 None（delegate #598） */
	UFUNCTION(BlueprintPure, Category = "Cond|Enter", meta = (BlueprintThreadSafe))
	bool EnterForward_Is_Run() const;

	// ============================================================
	// 决策函数 — 第3.5层 Forward Variant Gait（§2.1.1.1.2）
	// ============================================================

	/** Forward Variant Gait → Sprint 分支：步态等于 Sprint（delegate #644） */
	UFUNCTION(BlueprintPure, Category = "Cond|Enter", meta = (BlueprintThreadSafe))
	bool EnterForwardVar_Is_Sprint() const;

	/** Forward Variant Gait → Run 分支：步态等于 Run 或 None（delegate #645） */
	UFUNCTION(BlueprintPure, Category = "Cond|Enter", meta = (BlueprintThreadSafe))
	bool EnterForwardVar_Is_Run() const;

	// ============================================================
	// 决策函数 — 第3.5层 Enter Back（§2.1.1.1.3）
	// ============================================================

	/** Enter Back → 左右脚分派：当前左脚支撑切到右脚子机（delegate #631） */
	UFUNCTION(BlueprintPure, Category = "Cond|Enter", meta = (BlueprintThreadSafe))
	bool EnterBack_L_To_R() const;

	/** Enter Back → 右左脚分派：当前右脚支撑切到左脚子机（delegate #632） */
	UFUNCTION(BlueprintPure, Category = "Cond|Enter", meta = (BlueprintThreadSafe))
	bool EnterBack_R_To_L() const;

	// ============================================================
	// 决策函数 — 第3.5层 BackLeft（§2.1.1.1.3.1）
	// ============================================================

	/** BackLeft: Start → B（中间过渡）（delegate #615） */
	UFUNCTION(BlueprintPure, Category = "Cond|Enter", meta = (BlueprintThreadSafe))
	bool BackLeft_Start_To_B() const;

	/** BackLeft: Start → B AutoRule guard（动画播完自动切）（delegate #616） */
	UFUNCTION(BlueprintPure, Category = "Cond|Enter", meta = (BlueprintThreadSafe))
	bool BackLeft_Start_To_B_Auto() const;

	/** BackLeft: B → Exit（退出过渡）（delegate #617） */
	UFUNCTION(BlueprintPure, Category = "Cond|Enter", meta = (BlueprintThreadSafe))
	bool BackLeft_B_To_Exit() const;

	// ============================================================
	// 决策函数 — 第3.5层 BackRight（§2.1.1.1.3.2）
	// ============================================================

	/** BackRight: Start → B（中间过渡）（delegate #628） */
	UFUNCTION(BlueprintPure, Category = "Cond|Enter", meta = (BlueprintThreadSafe))
	bool BackRight_Start_To_B() const;

	/** BackRight: Start → B AutoRule guard（动画播完自动切）（delegate #629） */
	UFUNCTION(BlueprintPure, Category = "Cond|Enter", meta = (BlueprintThreadSafe))
	bool BackRight_Start_To_B_Auto() const;

	/** BackRight: B → Exit（退出过渡）（delegate #630） */
	UFUNCTION(BlueprintPure, Category = "Cond|Enter", meta = (BlueprintThreadSafe))
	bool BackRight_B_To_Exit() const;

	// ============================================================
	// 决策函数 — 第4.1层 Detail 步态（Walk/Run/转身）（§2.1.1.2）
	// ============================================================

	/** EnterRunState → Run：进入 Moving 后立即过渡到 Run（delegate #734） */
	UFUNCTION(BlueprintPure, Category = "Cond|Detail", meta = (BlueprintThreadSafe))
	bool Detail_EnterRun_To_Run() const;

	/** 步行 → 奔跑：解析步态达到 Run 或更高（delegate #735） */
	UFUNCTION(BlueprintPure, Category = "Cond|Detail", meta = (BlueprintThreadSafe))
	bool Detail_Walk_To_Run() const;

	/** Walk → WalkToRun：步态升级且需要过渡动画（delegate #736） */
	UFUNCTION(BlueprintPure, Category = "Cond|Detail", meta = (BlueprintThreadSafe))
	bool Detail_Walk_To_WalkToRun() const;

	/** 奔跑 → 转身：移动角绝对值大于 Tuning.TurnBackAngle（delegate #737） */
	UFUNCTION(BlueprintPure, Category = "Cond|Detail", meta = (BlueprintThreadSafe))
	bool Detail_Run_To_TurnBack() const;

	/** WalkToRun → Run（打断）：过渡动画播放中步态降回 Walk（delegate #738） */
	UFUNCTION(BlueprintPure, Category = "Cond|Detail", meta = (BlueprintThreadSafe))
	bool Detail_WalkToRun_To_Run() const;

	/** RunTurnBack → Run：转身结束，角度回归正常（delegate #740） */
	UFUNCTION(BlueprintPure, Category = "Cond|Detail", meta = (BlueprintThreadSafe))
	bool Detail_TurnBack_To_Run() const;

	/** 奔跑 → 步行：解析步态等于 Walk（无 NTE delegate，保留灵活性） */
	UFUNCTION(BlueprintPure, Category = "Cond|Detail", meta = (BlueprintThreadSafe))
	bool Detail_Run_To_Walk() const;

	// ============================================================
	// 决策函数 — 第4.2层 RunTurnBackState（§2.1.1.2.1）
	// ============================================================

	/** RunTurnBackState Conduit → Left：当前支撑脚为左脚（delegate #730） */
	UFUNCTION(BlueprintPure, Category = "Cond|Detail", meta = (BlueprintThreadSafe))
	bool Detail_TurnBack_IsLeft() const;

	/** RunTurnBackState Conduit → Right：当前支撑脚为右脚（delegate #731） */
	UFUNCTION(BlueprintPure, Category = "Cond|Detail", meta = (BlueprintThreadSafe))
	bool Detail_TurnBack_IsRight() const;

	// ============================================================
	// 决策函数 — 第4.2层 Sprint（MoveR/MoveL 子机共用）
	// ============================================================

	/** 进入冲刺：解析步态等于 Sprint */
	UFUNCTION(BlueprintPure, Category = "Cond|Gait", meta = (BlueprintThreadSafe))
	bool Gait_To_Sprint() const;

	/** 退出冲刺：解析步态低于 Sprint */
	UFUNCTION(BlueprintPure, Category = "Cond|Gait", meta = (BlueprintThreadSafe))
	bool Gait_Exit_Sprint() const;

	// ============================================================
	// 决策函数 — 第4.3层 StopGait（急停步态分支，§2.1.1.3/§2.1.1.4）
	// ============================================================

	/** 急停为冲刺：解析步态等于 Sprint（delegate #556/#577） */
	UFUNCTION(BlueprintPure, Category = "Cond|StopGait", meta = (BlueprintThreadSafe))
	bool StopGait_Is_Sprint() const;

	/** 急停为奔跑：解析步态等于 Run 或等于 None（delegate #554/#557/#576/#578） */
	UFUNCTION(BlueprintPure, Category = "Cond|StopGait", meta = (BlueprintThreadSafe))
	bool StopGait_Is_Run() const;

	/** 急停为步行：解析步态等于 Walk（delegate #558/#579） */
	UFUNCTION(BlueprintPure, Category = "Cond|StopGait", meta = (BlueprintThreadSafe))
	bool StopGait_Is_Walk() const;

	/** ForceRunStop 回退到 State 路由：非 Run 步态时从 ForceRunStop 退出（delegate #555/#575） */
	UFUNCTION(BlueprintPure, Category = "Cond|StopGait", meta = (BlueprintThreadSafe))
	bool StopGait_ForceRun_To_State() const;

	// ============================================================
	// 决策函数 — 第5层 LocomotionCycles（左右脚循环 + StopRotation，§2.1.1.2.2）
	// ============================================================

	/** 入口→左脚循环：当前支撑脚为左脚（delegate #811） */
	UFUNCTION(BlueprintPure, Category = "Cond|Cycles", meta = (BlueprintThreadSafe))
	bool Cycles_To_Left() const;

	/** 入口→右脚循环：当前支撑脚为右脚（delegate #812） */
	UFUNCTION(BlueprintPure, Category = "Cond|Cycles", meta = (BlueprintThreadSafe))
	bool Cycles_To_Right() const;

	/** 循环→停止旋转触发（Run 步态）（delegate #813） */
	UFUNCTION(BlueprintPure, Category = "Cond|Cycles", meta = (BlueprintThreadSafe))
	bool Cycles_To_RunStopRotation() const;

	/** 循环→停止旋转触发（通用）（delegate #814） */
	UFUNCTION(BlueprintPure, Category = "Cond|Cycles", meta = (BlueprintThreadSafe))
	bool Cycles_To_StopRotation() const;

	/** 循环→停止旋转触发（Walk 步态） */
	UFUNCTION(BlueprintPure, Category = "Cond|Cycles", meta = (BlueprintThreadSafe))
	bool Cycles_To_WalkStopRotation() const;

	/** StopRotation 结束→回到循环（delegate #815–#824） */
	UFUNCTION(BlueprintPure, Category = "Cond|Cycles", meta = (BlueprintThreadSafe))
	bool Cycles_StopRotation_Done() const;

	/** Conduit 按脚分发→左脚（delegate #817/#822/#826） */
	UFUNCTION(BlueprintPure, Category = "Cond|Cycles", meta = (BlueprintThreadSafe))
	bool Cycles_Conduit_IsLeft() const;

	/** Conduit 按脚分发→右脚（delegate #816/#821/#825） */
	UFUNCTION(BlueprintPure, Category = "Cond|Cycles", meta = (BlueprintThreadSafe))
	bool Cycles_Conduit_IsRight() const;

	// ============================================================
	// 决策函数 — 第5层 MoveR（§2.1.1.2.2.1，右脚循环内 Sprint/RunWalk 切换）
	// ============================================================

	/** MoveR Conduit → RunWalk：非冲刺步态进入走跑循环（delegate #754） */
	UFUNCTION(BlueprintPure, Category = "Cond|MoveR", meta = (BlueprintThreadSafe))
	bool MoveR_Conduit_To_RunWalk() const;

	/** MoveR Conduit → SprintToRunWalk：从冲刺减速进入走跑（delegate #755） */
	UFUNCTION(BlueprintPure, Category = "Cond|MoveR", meta = (BlueprintThreadSafe))
	bool MoveR_Conduit_To_SprintToRunWalk() const;

	/** MoveR RunWalk → Sprint：步态升级到冲刺（delegate #756） */
	UFUNCTION(BlueprintPure, Category = "Cond|MoveR", meta = (BlueprintThreadSafe))
	bool MoveR_RunWalk_To_Sprint() const;

	/** MoveR Sprint → RunWalk：步态从冲刺降回走跑（delegate #757） */
	UFUNCTION(BlueprintPure, Category = "Cond|MoveR", meta = (BlueprintThreadSafe))
	bool MoveR_Sprint_To_RunWalk() const;

	/** MoveR SprintToRunWalk → RunWalk：减速过渡完成（delegate #758） */
	UFUNCTION(BlueprintPure, Category = "Cond|MoveR", meta = (BlueprintThreadSafe))
	bool MoveR_SprintToRunWalk_To_RunWalk() const;

	// ============================================================
	// 决策函数 — 第5层 MoveL（§2.1.1.2.2.2，左脚循环内 Sprint/RunWalk 切换）
	// ============================================================

	/** MoveL Conduit → RunWalk：非冲刺步态进入走跑循环（delegate #771） */
	UFUNCTION(BlueprintPure, Category = "Cond|MoveL", meta = (BlueprintThreadSafe))
	bool MoveL_Conduit_To_RunWalk() const;

	/** MoveL Conduit → SprintToRunWalk：从冲刺减速进入走跑（delegate #772） */
	UFUNCTION(BlueprintPure, Category = "Cond|MoveL", meta = (BlueprintThreadSafe))
	bool MoveL_Conduit_To_SprintToRunWalk() const;

	/** MoveL RunWalk → Sprint：步态升级到冲刺（delegate #773） */
	UFUNCTION(BlueprintPure, Category = "Cond|MoveL", meta = (BlueprintThreadSafe))
	bool MoveL_RunWalk_To_Sprint() const;

	/** MoveL Sprint → RunWalk：步态从冲刺降回走跑（delegate #774） */
	UFUNCTION(BlueprintPure, Category = "Cond|MoveL", meta = (BlueprintThreadSafe))
	bool MoveL_Sprint_To_RunWalk() const;

	/** MoveL SprintToRunWalk → RunWalk：减速过渡完成（delegate #775） */
	UFUNCTION(BlueprintPure, Category = "Cond|MoveL", meta = (BlueprintThreadSafe))
	bool MoveL_SprintToRunWalk_To_RunWalk() const;

	// ============================================================
	// 决策函数 — 顶层辅助 MotionMatch（NTE_05 B.2.9 #44–#45）
	// ============================================================

	/** #44 MM → Base(0s)：bForceExitMotionMatching */
	UFUNCTION(BlueprintPure, Category = "Cond|MM", meta = (BlueprintThreadSafe))
	bool MM_To_Base_Instant() const;

	/** #45 MM → Base(0.1s)：bMotionMatchComplete || !bMotionMatchActive */
	UFUNCTION(BlueprintPure, Category = "Cond|MM", meta = (BlueprintThreadSafe))
	bool MM_To_Base_Smooth() const;

	// ============================================================
	// 决策函数 — 顶层辅助 FullBodyIK（NTE_05 B.2.9 #64–#65）
	// ============================================================

	/** #64 Inactive → Activated：bFullBodyIKNeeded */
	UFUNCTION(BlueprintPure, Category = "Cond|IK", meta = (BlueprintThreadSafe))
	bool FullBodyIK_Activate() const;

	/** #65 Activated → Inactive：!bFullBodyIKNeeded */
	UFUNCTION(BlueprintPure, Category = "Cond|IK", meta = (BlueprintThreadSafe))
	bool FullBodyIK_Deactivate() const;

	// ============================================================
	// 状态回调 — 生命周期函数（只做副作用，不改数据来源）
	//
	// 被蓝图状态的 On Entry 引用。状态图在 worker 线程求值，故标 BlueprintThreadSafe；
	// 回调内只做线程安全的副作用（Verbose 日志），不读写 Owner/Actor 或改快照数据来源。
	// ============================================================

	/** 进入待机状态 */
	UFUNCTION(BlueprintCallable, Category = "Event|Loco", meta = (BlueprintThreadSafe))
	void OnEnter_NotMoving();

	/** 进入移动状态 */
	UFUNCTION(BlueprintCallable, Category = "Event|Loco", meta = (BlueprintThreadSafe))
	void OnEnter_Moving();

	/** 进入左脚停止状态 */
	UFUNCTION(BlueprintCallable, Category = "Event|Loco", meta = (BlueprintThreadSafe))
	void OnEnter_LeftStop();

	/** 进入右脚停止状态 */
	UFUNCTION(BlueprintCallable, Category = "Event|Loco", meta = (BlueprintThreadSafe))
	void OnEnter_RightStop();

	/** 进入起步过渡状态 */
	UFUNCTION(BlueprintCallable, Category = "Event|Enter", meta = (BlueprintThreadSafe))
	void OnEnter_EnterMoveState();

	/**
	 * 通用状态进入回调：记录当前动画状态名并屏显。
	 * 每个蓝图状态的 On Entry 都调用它，传入该状态的名字字符串即可实时观察当前处于哪个状态。
	 * @param InStateName 状态名（如 "Idle"/"EnterMove"/"Moving"/"LeftStop"），蓝图里填字面量
	 */
	UFUNCTION(BlueprintCallable, Category = "Event|Debug", meta = (BlueprintThreadSafe))
	void OnEnterState(FName InStateName);

	// ============================================================
	// 查询辅助（蓝图可读）
	// ============================================================

	/** 获取当前解析步态 */
	UFUNCTION(BlueprintPure, Category = "Query", meta = (BlueprintThreadSafe))
	EMovementGait GetDesiredGait() const { return Snap.DesiredGait; }

	/** 获取当前支撑脚 */
	UFUNCTION(BlueprintPure, Category = "Query", meta = (BlueprintThreadSafe))
	EAnimFoot GetCurrentFoot() const { return Snap.CurrentFoot; }

	// ============================================================
	// 配表查询（按 key 取 AnimSet 资产，供 NTE 拓扑下各末端状态 Bind）
	//
	// 路线 2（复刻 NTE 拓扑）：方向/脚/步态由蓝图状态机分支决定，
	// 每个末端状态在 Sequence/BlendSpace Player 的资产引脚上 Bind 下列函数，
	// 传入该状态对应的 key 字面量，即可从 AnimSet 配表取到资产。
	// 改动画只改细节面板的 AnimSet 配表，不改蓝图、不改 C++。
	// key 不存在时返回 nullptr（Player 播放空指针 → 不播放，安全降级）。
	// 只读初始化后不变的 AnimSet 配置，故 BlueprintThreadSafe。
	// ============================================================

	/** 按 key 取起步序列（AnimSet.EnterSequences），如 "run_enterFL" / "sprint_enterFL" */
	UFUNCTION(BlueprintPure, Category = "Query|AnimSet", meta = (BlueprintThreadSafe))
	UAnimSequence* GetEnterSeqByKey(FName Key) const;

	/** 按 key 取停步序列（AnimSet.StopSequences），如 "run_stopL" / "walk_stopR" / "sprint_stopL" */
	UFUNCTION(BlueprintPure, Category = "Query|AnimSet", meta = (BlueprintThreadSafe))
	UAnimSequence* GetStopSeqByKey(FName Key) const;

	/** 按 key 取杂项一次性序列（AnimSet.MiscSequences），如 "turnback_L" / "turnback_R" */
	UFUNCTION(BlueprintPure, Category = "Query|AnimSet", meta = (BlueprintThreadSafe))
	UAnimSequence* GetMiscSeqByKey(FName Key) const;

	/** 按 key 取循环序列（AnimSet.LoopSequences），如 "Idle" */
	UFUNCTION(BlueprintPure, Category = "Query|AnimSet", meta = (BlueprintThreadSafe))
	UAnimSequence* GetLoopSeqByKey(FName Key) const;

	/** 按 key 取混合空间（AnimSet.BlendSpaces），如 "WalkRun_L" / "WalkRun_R" */
	UFUNCTION(BlueprintPure, Category = "Query|AnimSet", meta = (BlueprintThreadSafe))
	UBlendSpace* GetBlendSpaceByKey(FName Key) const;

	// ============================================================
	// 输出变量 — BlendSpace 遥控（被 AnimGraph 节点 Bind）
	// ============================================================

	/** 活动 BlendSpace 资产（按当前脚选 WalkRun_L / WalkRun_R） */
	UPROPERTY(BlueprintReadOnly, Category = "Out|BlendSpace")
	TObjectPtr<UBlendSpace> Out_BlendSpace;

	/** WalkRun 轴值（Y：0 走 1 跑），速度映射后插值平滑 */
	UPROPERTY(BlueprintReadOnly, Category = "Out|BlendSpace")
	float Out_WalkRun = 0.f;

	/** Stride 轴值（X：步幅） */
	UPROPERTY(BlueprintReadOnly, Category = "Out|BlendSpace")
	float Out_Stride = 1.f;

	// ============================================================
	// 输出变量 — Lean 倾斜
	// ============================================================

	/** Lean 左右量（横向倾斜），钳制在 ±Tuning.LeanAmountClamp */
	UPROPERTY(BlueprintReadOnly, Category = "Out|Lean")
	float Out_LeanLR = 0.f;

	/** Lean 前后量（纵向倾斜） */
	UPROPERTY(BlueprintReadOnly, Category = "Out|Lean")
	float Out_LeanFB = 0.f;

	// ============================================================
	// 输出变量 — 一次性动画（供状态里的 SequencePlayer/Evaluator Bind）
	// ============================================================

	/** 起步序列（按方向+脚选出） */
	UPROPERTY(BlueprintReadOnly, Category = "Out|OneShot")
	TObjectPtr<UAnimSequence> Out_EnterSeq;

	/** 停步序列（按步态+脚选出） */
	UPROPERTY(BlueprintReadOnly, Category = "Out|OneShot")
	TObjectPtr<UAnimSequence> Out_StopSeq;

	/** 转身序列（按当前脚选出） */
	UPROPERTY(BlueprintReadOnly, Category = "Out|OneShot")
	TObjectPtr<UAnimSequence> Out_TurnBackSeq;

	/** 待机序列（常量循环，初始化时从 AnimSet 取） */
	UPROPERTY(BlueprintReadOnly, Category = "Out|OneShot")
	TObjectPtr<UAnimSequence> Out_IdleSeq;

	// ============================================================
	// 输出变量 — 调试
	// ============================================================

	/** 当前支撑脚调试字符串（"L" / "R"） */
	UPROPERTY(BlueprintReadOnly, Category = "Out|Debug")
	FName Out_DebugFoot;

	// ============================================================
	// 配置（美术在 AnimBP 细节面板填值）
	// ============================================================

	/** 动画资产表（key→value） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "配置|AnimSet")
	FLocomotionAnimSet AnimSet;

	/** 数值参数（步态速度、停止阈值、转身角度、Lean 钳制等） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "配置|Tuning")
	FLocomotionTuning Tuning;

private:
	// ============================================================
	// 私有成员
	// ============================================================

	/** 动画决策快照（游戏线程写入，worker 线程与决策函数只读） */
	FAnimSnapshot Snap;

	/** 所属角色弱引用（只在游戏线程访问） */
	TWeakObjectPtr<ABaseCharacter> Owner;

	/** 本帧已由管线驱动过（NativeUpdateAnimation 跳过重复工作） */
	bool bDrivenByPipeline = false;

	/** 当前动画状态名（由 OnEnterState 写入，仅用于调试屏显） */
	FName CurrentAnimStateName = FName("?");

	/** 上一帧的移动意图（用于地面移动层"动画自算"标志的 1 帧滞后：如 bIsHasInStandIdlePose/bIsCanRunStop） */
	bool bPrevWantMove = false;

	/**
	 * 停步支撑脚锁存：在"移动→停止"那一刻锁定当前支撑脚，整个停步过程沿用此脚。
	 * 否则 Out_StopSeq 每帧按实时 CurrentFoot 重算，停步动画播放时同步相位推进会让脚左右翻，
	 * 导致停步动画中途切换、Sequence Player 反复重启、永远播不完（AutoRule 无法在播完时触发）。
	 */
	EAnimFoot LatchedStopFoot = EAnimFoot::Left;

	/** 停步计时器：进 Stop 那刻清零，之后（无移动意图时）每帧累加，用于判定停步动画是否播完。 */
	float StopElapsed = 0.f;

	/** 停步动画时长：进 Stop 那刻从锁定脚对应的 StopSequences 资产读取 GetPlayLength()；0 表示无停步动画。 */
	float StopDuration = 0.f;

	/** 起步计时器：起步那刻（待机→移动）清零，之后（有移动意图时）每帧累加，用于判定起步动画是否播完。 */
	float EnterElapsed = 0.f;

	/** 起步动画时长：起步那刻按方向+脚从 EnterSequences 资产读取 GetPlayLength()；0 表示无起步动画（立即完成）。 */
	float EnterDuration = 0.f;

	// ============================================================
	// 私有方法
	// ============================================================

	/**
	 * 游戏线程：从 Owner 与 RuntimeData 读取数据填入 Snap
	 * Owner 无效时将 Snap 重置为默认值并跳过后续读取
	 */
	void CaptureSnapshot();

	/**
	 * 把当前 Snap 的地址分发给所有决策模块（SetSnap）。
	 * 无论 Owner 是否有效都必须调用，保证决策函数 Snap 指针非空——
	 * 否则入口 Conduit 三条出边全 `if(!Snap) return false`，导管无出边会退回参考姿势。
	 */
	void BindSnapshotToDecisionModules();

	/**
	 * 从循环归一化相位推导当前支撑脚
	 * @return 相位不小于 0.5 返回 Right，否则返回 Left
	 */
	EAnimFoot QueryCurrentFoot() const;

	/**
	 * 游戏线程：查询 Locomotion 循环的归一化相位（[0,1)）
	 * SyncGroup 名 "Locomotion" 在 AnimGraph 设置（蓝图侧）；查询失败/当前没有有效 marker 时保留上一帧相位，
	 * 首次运行时默认返回 0（降级为 Left），避免 EnterMoveState 使用独立 RunStart/DoNotSync 时脚位被重置。
	 */
	float GetLocomotionSyncPhase() const;

	/**
	 * 计算步幅（Stride 轴值）
	 * @return 步幅缩放值
	 */
	float ComputeStride() const;

	/**
	 * worker 线程：只读 Snap，计算数值型输出变量
	 * @param Dt 本帧时间增量（秒）
	 */
	void UpdateOutputs(float Dt);

	/**
	 * 游戏线程：按当前脚与步态从 AnimSet 选出 Enter/Stop 资产写入输出变量
	 * key 不存在时对应输出变量置空指针
	 */
	void RefreshOneShotAssets();

	// ============================================================
	// 纯函数辅助（与引擎运行时解耦，可离线测试）
	// ============================================================

	/**
	 * 由来源数据构建快照（映射恒等）
	 * @param InSource 聚合来源字段
	 * @return 填充后的快照
	 */
	static FAnimSnapshot BuildSnapshot(const FAnimSourceData& InSource);

	/**
	 * 速度 → WalkRun 轴映射（0走 1跑），钳制在 [0,1]
	 *
	 * 经 InWalkSpeed→InRunSpeed 线性映射到 [0,1] 并钳制：
	 * InSpeed ≤ InWalkSpeed 得 0，InSpeed ≥ InRunSpeed 得 1，区间内单调不减。
	 * 纯函数，不接触引擎运行时，可离线测试。
	 * @param InSpeed     水平速度（cm/s）
	 * @param InWalkSpeed 走速阈值（映射下界，对应输出 0）
	 * @param InRunSpeed  跑速阈值（映射上界，对应输出 1）
	 * @return WalkRun 轴目标值，恒落在 [0,1]
	 */
	static float MapSpeedToWalkRun(float InSpeed, float InWalkSpeed, float InRunSpeed);

	/**
	 * 由步态与当前脚解析 Stop 资产 key
	 * @param InGait 解析步态
	 * @param InFoot 当前支撑脚
	 * @return Stop 资产 key（walk/run/sprint_stop{L/R}）
	 */
	static FName ResolveStopKey(EMovementGait InGait, EAnimFoot InFoot);

	/**
	 * 由移动角度与当前脚解析 Enter 资产 key
	 * @param InAngleDeg 移动角度（度，[-180,180]）
	 * @param InFoot     当前支撑脚
	 * @return Enter 资产 key（按方向分区+脚）
	 */
	static FName ResolveEnterKey(float InAngleDeg, EAnimFoot InFoot);

	// ============================================================
	// @NTEAnim: 连接点F - Decision Modules（9 模块体系，仅 NTEAnim 使用）
	// 这些模块读取 FAnimSnapshot 做移动动画过渡决策。
	// 如果移除 NTEAnim，这 9 个模块（18 文件）可以全部删除。
	// ============================================================

	FMainMovementDecisions  MainDecisions;
	FGroundedDecisions      GroundedDecisions;
	FLocomotionDecisions    LocoDecisions;
	FDirectionDecisions     DirectionDecisions;
	FDetailDecisions        DetailDecisions;
	FStopGaitDecisions      StopGaitDecisions;
	FCyclesDecisions        CyclesDecisions;
	FMotionMatchDecisions   MotionMatchDecisions;
	FFullBodyIKDecisions    FullBodyIKDecisions;
};
