/**
 * @file LocomotionConfig.h
 * @brief NTE 风格移动动画的配置类型
 *
 * 定义动画决策层使用的配置数据：
 *   EAnimFoot          支撑脚枚举（Left/Right）
 *   FLocomotionAnimSet key→value 动画资产表（BlendSpace/Loop/Enter/Stop/Misc）
 *   FLocomotionTuning  数值参数（步态速度、停止阈值、转身角度、Lean 钳制等）
 *
 * @NTEAnim: 连接点H - NTEAnim 的配表与参数配置
 * AnimSet 由美术在 AnimBP 细节面板按 key 填 value；Tuning 使用 NTE 实证默认值。
 * 过渡混合时长不在本文件配置，由蓝图过渡的 Blend Duration 承担。
 */
#pragma once

#include "CoreMinimal.h"
#include "StateMachine/CharacterStateType.h"  // EMovementGait
#include "LocomotionConfig.generated.h"

// 前向声明（减少编译依赖，资产指针在运行时解析）
class UBlendSpace;
class UAnimSequence;

/**
 * 支撑脚枚举
 * 表示当前支撑相所在脚，由循环动画的 SyncMarker 相位查询得出
 */
UENUM(BlueprintType)
enum class EAnimFoot : uint8
{
	/** 左脚支撑 */
	Left,

	/** 右脚支撑 */
	Right
};

/**
 * 移动动画资产表
 * 以 key→value 形式存放各类动画资产引用，美术在细节面板按 key 填 value。
 * 五组资产分别对应循环混合、待机循环、起步、停步与杂项一次性动画。
 */
USTRUCT(BlueprintType)
struct FLocomotionAnimSet
{
	GENERATED_BODY()

	/** 循环混合空间：如 "WalkRun_L"、"WalkRun_R"、"Lean_BS" */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlendSpace")
	TMap<FName, TObjectPtr<UBlendSpace>> BlendSpaces;

	/** 循环序列：如 "Idle" */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Loop")
	TMap<FName, TObjectPtr<UAnimSequence>> LoopSequences;

	/** 起步序列：如 "run_enterFL"、"run_enterFR"、"run_enterL"、"run_enterR"、"run_enterB_Lstart"、"run_enterB_Rstart" */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Enter")
	TMap<FName, TObjectPtr<UAnimSequence>> EnterSequences;

	/** 停步序列：如 "run_stopL"、"run_stopR"、"walk_stopL"、"walk_stopR"、"sprint_stopL"、"sprint_stopR" */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Stop")
	TMap<FName, TObjectPtr<UAnimSequence>> StopSequences;

	/** 杂项一次性序列：如 "turn_180"、"land" */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Misc")
	TMap<FName, TObjectPtr<UAnimSequence>> MiscSequences;
};

/**
 * 移动数值参数
 * 存放决策与输出计算所需的数值，默认值复刻 NTE（kuhara）实证数据。
 * 不含过渡混合时长——过渡时长在蓝图过渡的 Blend Duration 中配置。
 */
USTRUCT(BlueprintType)
struct FLocomotionTuning
{
	GENERATED_BODY()

	/** 步行速度阈值（cm/s），WalkRun 轴映射下界 */
	UPROPERTY(EditAnywhere, Category = "Gait")
	float WalkSpeed = 115.f;

	/** 奔跑速度阈值（cm/s），WalkRun 轴映射上界 */
	UPROPERTY(EditAnywhere, Category = "Gait")
	float RunSpeed = 450.f;

	/** 冲刺速度阈值（cm/s） */
	UPROPERTY(EditAnywhere, Category = "Gait")
	float SprintSpeed = 620.f;

	/** 触发急停的速度阈值（cm/s），低于此值判定进入停步 */
	UPROPERTY(EditAnywhere, Category = "Gait")
	float ThresholdToStop = 160.f;

	/** 触发转身的移动角度阈值（度），移动角绝对值大于此值判定转身 */
	UPROPERTY(EditAnywhere, Category = "Turn")
	float TurnBackAngle = 150.f;

	/** Lean 倾斜量钳制范围（±值） */
	UPROPERTY(EditAnywhere, Category = "Lean")
	float LeanAmountClamp = 0.22f;

	/** 速度混合插值速率，用于平滑 WalkRun 轴值 */
	UPROPERTY(EditAnywhere, Category = "Blend")
	float VelocityBlendInterp = 2.f;

	/** 落地重击阈值（cm/s），落地冲击速度大于此值判定为重着地 */
	UPROPERTY(EditAnywhere, Category = "Land")
	float JumpLandedThreshold = 1700.f;
};

/**
 * 运行时动画数据包
 *
 * 外部系统（角色管线/AI/技能系统等）每帧通过 SetAnimRuntimeData 推入，
 * 供 Layer 3+ 全部决策函数读取。字段映射自 NTE 蓝图变量表（NTE_05 §2.1.1）。
 */
USTRUCT(BlueprintType)
struct FAnimRuntimeData
{
	GENERATED_BODY()

	// ---- Bool Fields (Layer 3 Conduit routing & transition logic) ----

	/** 进入时移动/待机标记（Entry Conduit T1/T2 分发） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bEntryMovingOrNotMoving = false;

	/** 技能打断移动标记 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bSkillInterruptMove = false;

	/** 巡逻移动动画激活标记 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bIsPatrolMoveAnim = false;

	/** 巡逻状态标记 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bIsPatrolState = false;

	/** 可以执行跑停过渡 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bIsCanRunStop = false;

	/** 有速度/应该移动 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bShouldMove = false;

	/** 冲刺急停标记 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bIsSprintStop = false;

	/** 需要 MotionMatching */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bNeedMotionMatching = false;

	/** 当前左脚支撑标记（巡逻停步脚分派辅助） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bIsLeftFootC = false;

	/** NotMoving→Moving 转换标记 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bNotMovingToMoving = false;

	/** 是否有站立 Idle Pose */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bIsHasInStandIdlePose = false;

	/** Moving→NotMoving 转换标记 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bMovingToNotMoving = false;

	/** 是否可以进入起步状态 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bIsCanEnterMoveState = false;

	/** 牵手模式标记 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bIsHoldingHands = false;

	/** 忽略移动输入标记 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bIsIgnoreMoveInput = false;

	// ---- Enum Fields ----

	/** 步态枚举（Walk=0x1, Run=0x2, Sprint=0x3） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	EMovementGait Gait = EMovementGait::None;

	// ---- Float Fields ----

	/** 当前速度标量（cm/s） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	float VelocityLength = 0.f;

	/** 最后一次输入方向角度（度） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	float LastInputDirectionAngle = 0.f;

	// ============================================================
	// 顶层决策补全字段（Layer 1 Main / Layer 2 Grounded / MM / IK）
	//
	// 供 anim-toplevel-decisions-completion 新增的 70 个决策函数运行时推入。
	// 绝大多数字段当前无确认的外部来源，依 Requirement 6.7 给安全默认
	// （bool→false、int32→0、float→0.f）。bShouldMove 复用上方既有字段，不重定义。
	// ============================================================

	// ---- 整型/枚举状态字段（int32，默认 0，Req 7.6） ----

	/** 移动动画状态枚举值 ← 无确认来源，默认 0 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	int32 MoveAnimState = 0;

	/** 藤蔓子动画状态 ← 无确认来源，默认 0 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	int32 VinesSubAnimState = 0;

	/** 当前跳跃计数（二段跳判定） ← 无确认来源，默认 0 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	int32 JumpCurrentCount = 0;

	/** 地面动画子状态（0x0 Normal / 0x1 Landed / 0x7 FromRoll / 0x8 Vault /
	    0xA VinesOver / 0xB RunOnWallsOver / 0xC SprintVinesOver）
	    ← 无确认来源，默认 0x0 Normal */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	int32 GroundAnimState = 0;

	/** 翻越子状态 ← 无确认来源，默认 0 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	int32 VaultSubState = 0;

	// ---- 浮点字段（float，默认 0.f） ----

	/** 水平面 (XY) 速度标量长度 (cm/s) ← 可由 GetVelocity().Size2D() 接线，暂默认 0 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	float Velocity2DLength = 0.f;

	// ---- Main 层布尔字段（默认 false） ----

	/** 是否使用起跳前摇（#346） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bUseJumpTakeOff = false;

	/** 二段跳触发（#349） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bSecondJump = false;

	/** 强制跳跃（#349） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bForceJump = false;

	/** 离地意图（#345） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bWantsToLeaveGround = false;

	/** 空中动作意图（#350） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bWantsAirAction = false;

	/** 正在落地（#351/#354） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bIsLanding = false;

	/** 到达跳跃顶点（#353） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bJumpApexReached = false;

	/** 处于藤蔓（#356） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bOnVines = false;

	/** 翻越触发（#357） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bVaultTriggered = false;

	/** 开始驾驶（#359） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bStartedDriving = false;

	/** 返回地面 fallback（#360） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bReturnToGround = false;

	/** 可二段跳（#361） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bCanDoubleJump = false;

	/** 空中特殊模式（#362） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bSpecialModeInAir = false;

	/** 离开藤蔓（#367） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bLeaveVines = false;

	/** 藤蔓动画完成 AutoRule（#368） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bVinesAnimComplete = false;

	/** 翻越退出条件（#369） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bVaultExitCondition = false;

	/** 停止滑翔（#371） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bStopGliding = false;

	/** 落地中（#371/#372/#375） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bLanding = false;

	/** 强制退出滑翔（#372） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bForceExitGliding = false;

	/** 恢复滑翔（#374） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bResumeGliding = false;

	/** 离开水面（#376） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bLeaveWater = false;

	/** 停止驾驶（#377） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bStopDriving = false;

	// ---- Main 层 provisional ⚠️ 待验证布尔字段（默认 false，Req 2A.2） ----

	/** ⚠️ 待验证（#352）：郊狼时间跳，语义由 JSON 推导，需蓝图侧验证 ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bCoyoteTimeJump = false;

	/** ⚠️ 待验证（#355）：从空中进入，语义由 JSON 推导，需蓝图侧验证 ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bEnteredFromAir = false;

	/** ⚠️ 待验证（#365）：起跳前摇完成，语义由 JSON 推导，需蓝图侧验证 ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bTakeOffComplete = false;

	/** ⚠️ 待验证（#366）：取消跳跃，语义由 JSON 推导，需蓝图侧验证 ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bCancelJump = false;

	/** ⚠️ 待验证（#373）：过渡完成，语义由 JSON 推导，需蓝图侧验证 ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bTransitionComplete = false;

	// ---- Grounded 层布尔字段（默认 false） ----

	/** 可停留地面（#468） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bCanStayingTheGround = false;

	/** 正在播放任意蒙太奇（#472） ← 可由 IsAnyMontagePlaying() 接线，暂默认 false */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bIsPlayingAnyMontage = false;

	/** 滑翔落地标记（#472/#476/#482/#483） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bIsGlidingLanded = false;

	/** 翻越结束到停止（左）（#489） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool VaultEndToStopL = false;

	/** 翻越结束到停止（右）（#489） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool VaultEndToStopR = false;

	// ---- MM/IK 层布尔字段（默认 false） ----

	/** 强制退出 MotionMatching（#44） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bForceExitMotionMatching = false;

	/** MotionMatching 完成（#45） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bMotionMatchComplete = false;

	/** MotionMatching 激活中（#45） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bMotionMatchActive = false;

	/** 需要全身 IK（#64/#65） ← 无确认来源 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bFullBodyIKNeeded = false;
};
