/**
 * @file GGYGOAnimInstance.cpp
 * @brief NTE 风格移动动画 C++ 决策层实现
 *
 * 设计哲学：拓扑在蓝图，决策在 C++，求值在 AnimGraph。
 * 本类不维护状态机循环，只提供决策函数、状态回调、输出变量、快照与配置。
 *
 * 线程模型：
 *   NativeInitializeAnimation       动画系统初始化时调用一次，缓存 Owner 弱引用、
 *                                   从 AnimSet 初始化常量循环资产输出变量
 *   NativeUpdateAnimation           游戏线程，唯一能安全读 Owner 的位置：
 *                                   抓取快照、按脚+步态选一次性资产
 *   NativeThreadSafeUpdateAnimation worker 线程，只读快照，计算数值型输出变量
 *
 * Owner 无效（编辑器预览等）时静默返回，不做任何读取。
 */

#include "Animation/GGYGOAnimInstance.h"

#include "BaseCharacter.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimTypes.h"   // FMarkerSyncAnimPosition

// 动画决策层日志分类：状态回调等辅助调试信息以 Verbose 级别输出，
// 默认不刷屏（动画层每帧调用），需要排查时提升 verbosity 观察。
DEFINE_LOG_CATEGORY_STATIC(LogGGYGOAnim, Log, All);

// ============================================================================
// AnimInstance 生命周期入口（三线程阶段）
// ============================================================================

void UGGYGOAnimInstance::NativeInitializeAnimation()
{
	// 调用父类实现（UAnimInstance 基类有初始化处理）
	Super::NativeInitializeAnimation();

	// 缓存 Owner 为 ABaseCharacter 弱引用（只在游戏线程访问）
	// TryGetPawnOwner 返回当前拥有此 Mesh/AnimInstance 的 Pawn
	// 关联失败（编辑器预览等场景）时 Owner 保持无效，后续入口静默返回
	Owner = Cast<ABaseCharacter>(TryGetPawnOwner());

	// 从 AnimSet 初始化常量循环资产输出变量
	// Idle 是不随脚/步态变化的常量循环，初始化时取一次即可
	// key 不存在时 FindRef 返回空指针（AnimSet 未配置时的安全降级）
	Out_IdleSeq = AnimSet.LoopSequences.FindRef(FName("Idle"));

	// ---- Decision Module 初始化 ----

	// 绑定 Curve 代理：lambda 捕获 this 调用 UAnimInstance::GetCurveValue
	FCurveValueDelegate CurveDelegate = [this](FName Name, float& OutVal) -> bool
	{
		return GetCurveValue(Name, OutVal);
	};

	// 绑定 Weight 代理：lambda 捕获 this 调用 UAnimInstance::GetInstanceStateWeight
	FStateWeightDelegate WeightDelegate = [this](int32 Machine, int32 State) -> float
	{
		return GetInstanceStateWeight(Machine, State);
	};

	// 绑定 AnimTime 代理：lambda 捕获 this 调用 GetRelevantAnimTimeRemaining
	FAnimTimeRemainingDelegate AnimTimeDelegate =
		[this](int32 Machine, int32 State) -> float
		{
			return GetRelevantAnimTimeRemaining(Machine, State);
		};

	// 初始化需要 Tuning / Delegate 的模块
	GroundedDecisions.Init(&Tuning, MoveTemp(AnimTimeDelegate));
	LocoDecisions.Init(&Tuning, MoveTemp(CurveDelegate), MoveTemp(WeightDelegate));
	DetailDecisions.Init(&Tuning);
	CyclesDecisions.Init(&Tuning);
}

void UGGYGOAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
	// 调用父类实现（UAnimInstance 基类有少量内部处理）
	Super::NativeUpdateAnimation(DeltaSeconds);

	// 游戏线程每帧更新：唯一能安全读 Owner 的位置。
	// 从 Owner 与 RuntimeData 抓取只读快照，供 worker 线程并行消费。
	CaptureSnapshot();

	// 按当前脚+步态+方向选一次性资产（Enter/Stop）写入 UObject 指针型输出变量。
	// 改 UObject 指针在 worker 线程不安全，故放在游戏线程；须在 CaptureSnapshot 之后调用
	// （CurrentFoot 已在快照中解析完毕）。
	RefreshOneShotAssets();
}

void UGGYGOAnimInstance::NativeThreadSafeUpdateAnimation(float DeltaSeconds)
{
	// 调用父类实现（可与游戏线程并行）
	Super::NativeThreadSafeUpdateAnimation(DeltaSeconds);

	// worker 线程每帧更新：只读快照，计算数值型输出变量
	UpdateOutputs(DeltaSeconds);
}

// ============================================================================
// Push Interface（游戏线程，外部系统每帧调用）
// ============================================================================

void UGGYGOAnimInstance::SetAnimRuntimeData(const FAnimRuntimeData& InData)
{
	StoredRuntimeData = InData;
}

// ============================================================================
// 游戏线程私有方法（唯一能安全读 Owner/Actor 的位置）
// ============================================================================

void UGGYGOAnimInstance::CaptureSnapshot()
{
	// 游戏线程唯一读 Owner 的位置：此处访问 Owner/Actor/RuntimeData 是安全的，
	// worker 线程与决策函数只读游戏线程在此填好的 Snap，绝不触碰 Actor。

	// 空值防御：Owner 无效（编辑器预览、关联失败等）时将 Snap 重置为默认构造值并跳过后续读取，
	// 下游决策函数在默认快照上返回安全值（bWantMove=false → 不进入移动）。
	ABaseCharacter* OwnerPtr = Owner.Get();
	if (!OwnerPtr)
	{
		Snap = FAnimSnapshot();
		// 降级：Owner 无效时相位默认 0 → 支撑脚为 Left，调试字符串同步置 "L" 保持一致
		Out_DebugFoot = FName("L");
		return;
	}

	// 聚合来源字段：从 ABaseCharacter getter 逐项读入，随后交由纯函数 BuildSnapshot 映射。
	// 数据来源映射（每字段 ← 来源 getter/字段）：
	FAnimSourceData Src;

	Src.bWantMove   = OwnerPtr->IsMoving();          // ← ABaseCharacter::IsMoving()（意图层，摇杆是否推开）
	Src.DesiredGait = OwnerPtr->GetResolvedGait();   // ← ABaseCharacter::GetResolvedGait()
	Src.MoveAngleDeg = OwnerPtr->GetMoveAngle();     // ← ABaseCharacter::GetMoveAngle()（[-180,180]）
	Src.Speed       = OwnerPtr->GetCurrentSpeed();   // ← ABaseCharacter::GetCurrentSpeed()（水平标量）
	Src.bGrounded   = OwnerPtr->IsGrounded();        // ← ABaseCharacter::IsGrounded()

	// 垂直速度：以角色速度的 Z 分量作为来源。ABaseCharacter 无专用 VerticalVelocity getter
	// （RuntimeData 为 protected、无公开访问入口），GetVelocity().Z 是唯一不违反非破坏性约束
	// （R15.2 禁止改 BaseCharacter/RuntimeData）即可取得的公开来源，作为顶层运动模式路由的垂直速度输入。
	Src.VerticalVelocity = OwnerPtr->GetVelocity().Z; // ← GetVelocity().Z（垂直速度公开来源）

	// 落地冲击速度：以本帧垂直速度的绝对值近似。RuntimeData.bJustLanded 与落地冲击量无公开 getter，
	// 在不改 BaseCharacter/RuntimeData（R15.2）的前提下，以 |GetVelocity().Z| 作为落地那帧冲击速度的
	// 公开来源近似——下落越快，落地冲击速度读数越大，供 Grounded_Just_Landed 与阈值比较。
	Src.LandImpactSpeed = FMath::Abs(OwnerPtr->GetVelocity().Z); // ← |GetVelocity().Z|（落地冲击速度近似）

	// 是否刚落地：无公开数据来源，恒置 false。RuntimeData.bJustLanded 为 protected 且无公开 getter，
	// 精确落地帧检测需 BaseCharacter 新增只读 getter 暴露（超出本层范围，R15.2 禁止改动 BaseCharacter/RuntimeData）。
	Src.bJustLanded = false;

	// 跳跃/滑翔/入水意图：RuntimeData 中无对应字段，均恒置 false。
	// 这些字段保持 false 直到角色管线对外暴露跳跃/滑翔/入水意图来源；
	// 在来源接入前顶层运动模式不会误切到 Jump/Gliding/Swimming（属既定降级行为，非缺陷）。
	Src.bWantJump  = false;
	Src.bWantGlide = false;
	Src.bInWater   = false;

	// SyncGroup 归一化相位查询（[0,1)）：SyncGroup 名 "Locomotion" 在 AnimGraph 设置（蓝图侧）。
	// 查询失败/无 marker 时相位保持 0（降级），下游 QueryCurrentFoot 据此返回 Left，不崩溃。
	Src.LocomotionPhase = GetLocomotionSyncPhase();

	// Copy stored RuntimeData into source data for BuildSnapshot
	Src.bEntryMovingOrNotMoving = StoredRuntimeData.bEntryMovingOrNotMoving;
	Src.bSkillInterruptMove     = StoredRuntimeData.bSkillInterruptMove;
	Src.bIsPatrolMoveAnim       = StoredRuntimeData.bIsPatrolMoveAnim;
	Src.bIsPatrolState          = StoredRuntimeData.bIsPatrolState;
	Src.bIsCanRunStop           = StoredRuntimeData.bIsCanRunStop;
	Src.bShouldMove             = StoredRuntimeData.bShouldMove;
	Src.bIsSprintStop           = StoredRuntimeData.bIsSprintStop;
	Src.bNeedMotionMatching     = StoredRuntimeData.bNeedMotionMatching;
	Src.bIsLeftFootC            = StoredRuntimeData.bIsLeftFootC;
	Src.bNotMovingToMoving      = StoredRuntimeData.bNotMovingToMoving;
	Src.bIsHasInStandIdlePose   = StoredRuntimeData.bIsHasInStandIdlePose;
	Src.bMovingToNotMoving      = StoredRuntimeData.bMovingToNotMoving;
	Src.bIsCanEnterMoveState    = StoredRuntimeData.bIsCanEnterMoveState;
	Src.bIsHoldingHands         = StoredRuntimeData.bIsHoldingHands;
	Src.bIsIgnoreMoveInput      = StoredRuntimeData.bIsIgnoreMoveInput;
	Src.Gait                    = StoredRuntimeData.Gait;
	Src.VelocityLength          = StoredRuntimeData.VelocityLength;
	Src.LastInputDirectionAngle = StoredRuntimeData.LastInputDirectionAngle;

	// ---- 顶层决策补全字段（Layer 1 Main / Layer 2 Grounded / MM / IK）----
	// 将 StoredRuntimeData 中运行时推入的新字段填入 FAnimSourceData，经 BuildSnapshot 传播进 Snap。
	// 绝大多数字段当前无确认外部来源，保持 SetAnimRuntimeData 推入值（默认安全降级）。

	// 整型/枚举状态字段（int32）
	Src.MoveAnimState        = StoredRuntimeData.MoveAnimState;
	Src.VinesSubAnimState    = StoredRuntimeData.VinesSubAnimState;
	Src.JumpCurrentCount     = StoredRuntimeData.JumpCurrentCount;
	Src.GroundAnimState      = StoredRuntimeData.GroundAnimState;
	Src.VaultSubState        = StoredRuntimeData.VaultSubState;

	// 浮点字段
	Src.Velocity2DLength     = StoredRuntimeData.Velocity2DLength;

	// Main 层布尔字段
	Src.bUseJumpTakeOff      = StoredRuntimeData.bUseJumpTakeOff;
	Src.bSecondJump          = StoredRuntimeData.bSecondJump;
	Src.bForceJump           = StoredRuntimeData.bForceJump;
	Src.bWantsToLeaveGround  = StoredRuntimeData.bWantsToLeaveGround;
	Src.bWantsAirAction      = StoredRuntimeData.bWantsAirAction;
	Src.bIsLanding           = StoredRuntimeData.bIsLanding;
	Src.bJumpApexReached     = StoredRuntimeData.bJumpApexReached;
	Src.bOnVines             = StoredRuntimeData.bOnVines;
	Src.bVaultTriggered      = StoredRuntimeData.bVaultTriggered;
	Src.bStartedDriving      = StoredRuntimeData.bStartedDriving;
	Src.bReturnToGround      = StoredRuntimeData.bReturnToGround;
	Src.bCanDoubleJump       = StoredRuntimeData.bCanDoubleJump;
	Src.bSpecialModeInAir    = StoredRuntimeData.bSpecialModeInAir;
	Src.bLeaveVines          = StoredRuntimeData.bLeaveVines;
	Src.bVinesAnimComplete   = StoredRuntimeData.bVinesAnimComplete;
	Src.bVaultExitCondition  = StoredRuntimeData.bVaultExitCondition;
	Src.bStopGliding         = StoredRuntimeData.bStopGliding;
	Src.bLanding             = StoredRuntimeData.bLanding;
	Src.bForceExitGliding    = StoredRuntimeData.bForceExitGliding;
	Src.bResumeGliding       = StoredRuntimeData.bResumeGliding;
	Src.bLeaveWater          = StoredRuntimeData.bLeaveWater;
	Src.bStopDriving         = StoredRuntimeData.bStopDriving;

	// Main 层 provisional ⚠️ 待验证布尔字段
	Src.bCoyoteTimeJump      = StoredRuntimeData.bCoyoteTimeJump;
	Src.bEnteredFromAir      = StoredRuntimeData.bEnteredFromAir;
	Src.bTakeOffComplete     = StoredRuntimeData.bTakeOffComplete;
	Src.bCancelJump          = StoredRuntimeData.bCancelJump;
	Src.bTransitionComplete  = StoredRuntimeData.bTransitionComplete;

	// Grounded 层布尔字段
	Src.bCanStayingTheGround = StoredRuntimeData.bCanStayingTheGround;
	Src.bIsPlayingAnyMontage = StoredRuntimeData.bIsPlayingAnyMontage;
	Src.bIsGlidingLanded     = StoredRuntimeData.bIsGlidingLanded;
	Src.VaultEndToStopL      = StoredRuntimeData.VaultEndToStopL;
	Src.VaultEndToStopR      = StoredRuntimeData.VaultEndToStopR;

	// MM/IK 层布尔字段
	Src.bForceExitMotionMatching = StoredRuntimeData.bForceExitMotionMatching;
	Src.bMotionMatchComplete     = StoredRuntimeData.bMotionMatchComplete;
	Src.bMotionMatchActive       = StoredRuntimeData.bMotionMatchActive;
	Src.bFullBodyIKNeeded        = StoredRuntimeData.bFullBodyIKNeeded;

	// 纯拷贝映射写入快照（映射恒等）
	Snap = BuildSnapshot(Src);

	// 由循环相位推导当前支撑脚写回快照：相位 ≥ 0.5 → Right，< 0.5 → Left（R13.2）。
	// QueryCurrentFoot 只读 Snap.LocomotionPhase，须在 BuildSnapshot 填好相位之后调用。
	Snap.CurrentFoot = QueryCurrentFoot();

	// 调试输出：当前支撑脚字符串（"L"/"R"），供 AnimGraph/调试面板观察（R13.3）。
	Out_DebugFoot = (Snap.CurrentFoot == EAnimFoot::Left) ? FName("L") : FName("R");

	// ---- 分发快照指针到各 Decision Module ----
	MainDecisions.SetSnap(&Snap);
	GroundedDecisions.SetSnap(&Snap);
	LocoDecisions.SetSnap(&Snap);
	DirectionDecisions.SetSnap(&Snap);
	DetailDecisions.SetSnap(&Snap);
	StopGaitDecisions.SetSnap(&Snap);
	CyclesDecisions.SetSnap(&Snap);
	MotionMatchDecisions.SetSnap(&Snap);
	FullBodyIKDecisions.SetSnap(&Snap);
}

float UGGYGOAnimInstance::GetLocomotionSyncPhase() const
{
	// 查询 "Locomotion" 同步组的 marker 相位，映射为归一化相位 [0,1)。
	// GetSyncGroupPosition 为 UAnimInstance 的 BlueprintThreadSafe 方法，返回当前夹在哪两个
	// marker 之间（PreviousMarkerName/NextMarkerName）与之间插值比例（PositionBetweenMarkers）。
	// 循环动画上打了 Left / Right 两个 SyncMarker，把一个步态循环切成两个半区：
	//   刚过 Left  marker（Prev==Left）  → 相位落在 [0, 0.5)   → 左脚支撑
	//   刚过 Right marker（Prev==Right） → 相位落在 [0.5, 1)   → 右脚支撑
	// 同步组未配置 / marker 未注入时两个 marker 名均为 NAME_None，返回 0（降级 → Left，不崩溃）。
	const FMarkerSyncAnimPosition Pos = GetSyncGroupPosition(FName("Locomotion"));

	const float Frac = FMath::Clamp(Pos.PositionBetweenMarkers, 0.f, 1.f);
	if (Pos.PreviousMarkerName == FName("Left"))
	{
		return 0.5f * Frac;          // [0, 0.5)
	}
	if (Pos.PreviousMarkerName == FName("Right"))
	{
		return 0.5f + 0.5f * Frac;   // [0.5, 1)
	}

	// 无有效 marker（未接同步组）：降级为 0
	return 0.f;
}

EAnimFoot UGGYGOAnimInstance::QueryCurrentFoot() const
{
	// 由循环归一化相位划分支撑脚：相位 ≥ 0.5 → Right，< 0.5 → Left（R13.2）。
	// 无 marker 时相位默认 0（降级）→ 落在 Left 分支，保证无有效相位时也不崩溃（R13.1）。
	return (Snap.LocomotionPhase >= 0.5f) ? EAnimFoot::Right : EAnimFoot::Left;
}

void UGGYGOAnimInstance::RefreshOneShotAssets()
{
	// 游戏线程：按快照的步态+脚+方向解析 key，从 AnimSet 查资产写入 UObject 指针型输出变量。
	// 改 UObject 指针在 worker 线程不安全，因此本方法只在游戏线程 NativeUpdateAnimation 调用。
	// key 计算委托给纯函数 ResolveStopKey/ResolveEnterKey（离线可测），本方法只做 AnimSet 查表。

	// 停步资产：按步态+脚解析 key，从 AnimSet.StopSequences 查表。
	// key 不存在时 FindRef 返回 nullptr → 输出变量置空（安全降级，R12.6）：
	// 蓝图 SequencePlayer Bind 到空指针时不播放（引擎安全行为）。
	const FName StopKey = ResolveStopKey(Snap.DesiredGait, Snap.CurrentFoot);
	Out_StopSeq = AnimSet.StopSequences.FindRef(StopKey);

	// 起步资产：按移动角度+脚解析 key，从 AnimSet.EnterSequences 查表。
	// 同样 key 不存在 → FindRef 返回 nullptr → 输出变量置空（安全降级，R12.6）。
	const FName EnterKey = ResolveEnterKey(Snap.MoveAngleDeg, Snap.CurrentFoot);
	Out_EnterSeq = AnimSet.EnterSequences.FindRef(EnterKey);

	// 转身资产：按当前脚选左/右转身动画，从 AnimSet.MiscSequences 查表。
	// key: "turnback_L" / "turnback_R"。key 不存在 → 输出变量置空（安全降级）。
	const FName TurnBackKey = (Snap.CurrentFoot == EAnimFoot::Left) ? FName("turnback_L") : FName("turnback_R");
	Out_TurnBackSeq = AnimSet.MiscSequences.FindRef(TurnBackKey);

#if !UE_BUILD_SHIPPING
	// 临时屏幕调试：每帧显示决策层核心值（游戏线程，一定执行，不依赖状态机是否进入）。
	// 定位 Enter/Stop 不生效：看 bWantMove/Gait 是否随推杆变化、EnterKey/StopKey 是否命中资产。
	if (GEngine)
	{
		const TCHAR* GaitStr =
			Snap.DesiredGait == EMovementGait::Walk   ? TEXT("Walk")   :
			Snap.DesiredGait == EMovementGait::Run    ? TEXT("Run")    :
			Snap.DesiredGait == EMovementGait::Sprint ? TEXT("Sprint") : TEXT("None");

		GEngine->AddOnScreenDebugMessage(100, 0.f, FColor::Green,
			FString::Printf(TEXT("[ANIM] State=%s"), *CurrentAnimStateName.ToString()));

		GEngine->AddOnScreenDebugMessage(101, 0.f, FColor::Yellow,
			FString::Printf(TEXT("[ANIM] WantMove=%d Grounded=%d Gait=%s Speed=%.0f Foot=%s Phase=%.2f"),
				Snap.bWantMove ? 1 : 0, Snap.bGrounded ? 1 : 0, GaitStr, Snap.Speed,
				*Out_DebugFoot.ToString(), Snap.LocomotionPhase));

		// 决策函数即时求值：直接看第3层过渡条件当前是否成立（绕开蓝图，验证 C++ 侧）
		GEngine->AddOnScreenDebugMessage(103, 0.f, FColor::Orange,
			FString::Printf(TEXT("[ANIM] Cond: N2Enter=%d Stop_L=%d Stop_R=%d Stop2Move=%d"),
				Loco_NotMoving_To_Enter() ? 1 : 0,
				Loco_Moving_To_LeftStop() ? 1 : 0,
				Loco_Moving_To_RightStop() ? 1 : 0,
				Loco_Stop_To_Moving() ? 1 : 0));

		GEngine->AddOnScreenDebugMessage(102, 0.f, FColor::Cyan,
			FString::Printf(TEXT("[ANIM] EnterKey=%s → %s | StopKey=%s → %s"),
				*EnterKey.ToString(), Out_EnterSeq ? *Out_EnterSeq->GetName() : TEXT("<空>"),
				*StopKey.ToString(),  Out_StopSeq  ? *Out_StopSeq->GetName()  : TEXT("<空>")));
	}
#endif
}

// ============================================================================
// worker 线程私有方法（只读 Snap 与 Tuning，绝不触碰 Owner/Actor/组件）
// ============================================================================

void UGGYGOAnimInstance::UpdateOutputs(float Dt)
{
	// worker 线程：只读游戏线程填好的 Snap 与常量配置 Tuning，写数值型输出变量。
	// 不访问 Owner/Actor/组件——从设计上消除竞态。

	// 速度 → WalkRun 轴目标（0走 1跑），经走速→跑速映射并钳制到 [0,1]
	const float Target = MapSpeedToWalkRun(Snap.Speed, Tuning.WalkSpeed, Tuning.RunSpeed);

	// 以 VelocityBlendInterp 为速率插值平滑，避免速度抖动导致 BlendSpace 采样跳变
	Out_WalkRun = FMath::FInterpTo(Out_WalkRun, Target, Dt, Tuning.VelocityBlendInterp);

	// 按当前脚选活动 BlendSpace（key→value）：左脚取 "WalkRun_L"，右脚取 "WalkRun_R"。
	// AnimSet 缺少对应 key 时 FindRef 返回空指针（未配置时的安全降级，蓝图 Bind 到空不采样）。
	Out_BlendSpace = AnimSet.BlendSpaces.FindRef(
		Snap.CurrentFoot == EAnimFoot::Left ? FName("WalkRun_L") : FName("WalkRun_R"));

	// Lean 横向倾斜：取移动角度正弦作为左右量，钳制到 [-LeanAmountClamp, +LeanAmountClamp]。
	// 钳制保证输出恒落在配置区间内，绝对值不超过 LeanAmountClamp（R3.5）。
	const float LR = FMath::Sin(FMath::DegreesToRadians(Snap.MoveAngleDeg));
	Out_LeanLR = FMath::Clamp(LR, -Tuning.LeanAmountClamp, Tuning.LeanAmountClamp);

	// Lean 纵向倾斜：取移动角度余弦作为前后量（纵向分量，前向为正、后向为负）。
	// 以 WalkRun 轴值作为速度因子缩放（[0,1]，静止趋近 0）：静止时前后倾斜趋近 0，
	// 移动越快倾斜越明显。缩放在钳制之前完成，故钳制后 |Out_LeanFB| ≤ LeanAmountClamp 恒成立。
	const float FB = FMath::Cos(FMath::DegreesToRadians(Snap.MoveAngleDeg)) * Out_WalkRun;
	Out_LeanFB = FMath::Clamp(FB, -Tuning.LeanAmountClamp, Tuning.LeanAmountClamp);

	// 步幅（Stride 轴值），由 ComputeStride 计算
	Out_Stride = ComputeStride();
}

// ============================================================================
// worker 线程私有方法 — 步幅计算（只读常量，不触碰 Owner/Actor/组件）
// ============================================================================

float UGGYGOAnimInstance::ComputeStride() const
{
	// 步幅缩放：读取动画的 "Stride" predict 曲线作为步幅缩放值。
	// 曲线本身由外部脚本注入到动画资产（超出本决策层范围）；GetCurveValue 为 UAnimInstance
	// 的 const 只读 API，曲线按帧求值并缓存，可在 worker 线程安全调用，不触碰 Owner/Actor/组件。
	float CurveVal = 0.f;
	if (GetCurveValue(FName("Stride"), CurveVal) && CurveVal > 0.f)
	{
		// 曲线存在且有效（> 0）：以曲线值作为步幅缩放
		return CurveVal;
	}

	// 降级：无曲线（未注入）或曲线无效时回退中性步幅 1（不缩放）
	return 1.f;
}

// ============================================================================
// 纯函数辅助（与引擎运行时解耦，可离线测试）
// ============================================================================

FAnimSnapshot UGGYGOAnimInstance::BuildSnapshot(const FAnimSourceData& InSource)
{
	// 纯拷贝映射：逐字段将来源数据搬入快照，不接触任何引擎对象。
	// 每个字段一一对应（映射恒等），因此 InSource 与返回快照的同名字段值相等。
	FAnimSnapshot Snapshot;

	Snapshot.bWantMove = InSource.bWantMove;
	Snapshot.bGrounded = InSource.bGrounded;
	Snapshot.bJustLanded = InSource.bJustLanded;
	Snapshot.bWantJump = InSource.bWantJump;
	Snapshot.bWantGlide = InSource.bWantGlide;
	Snapshot.bInWater = InSource.bInWater;
	Snapshot.Speed = InSource.Speed;
	Snapshot.VerticalVelocity = InSource.VerticalVelocity;
	Snapshot.LandImpactSpeed = InSource.LandImpactSpeed;
	Snapshot.MoveAngleDeg = InSource.MoveAngleDeg;
	Snapshot.LocomotionPhase = InSource.LocomotionPhase;
	Snapshot.DesiredGait = InSource.DesiredGait;
	Snapshot.CurrentFoot = InSource.CurrentFoot;

	// ---- FAnimRuntimeData extension (Layer 3+ decision data) ----
	Snapshot.bEntryMovingOrNotMoving = InSource.bEntryMovingOrNotMoving;
	Snapshot.bSkillInterruptMove     = InSource.bSkillInterruptMove;
	Snapshot.bIsPatrolMoveAnim       = InSource.bIsPatrolMoveAnim;
	Snapshot.bIsPatrolState          = InSource.bIsPatrolState;
	Snapshot.bIsCanRunStop           = InSource.bIsCanRunStop;
	Snapshot.bShouldMove             = InSource.bShouldMove;
	Snapshot.bIsSprintStop           = InSource.bIsSprintStop;
	Snapshot.bNeedMotionMatching     = InSource.bNeedMotionMatching;
	Snapshot.bIsLeftFootC            = InSource.bIsLeftFootC;
	Snapshot.bNotMovingToMoving      = InSource.bNotMovingToMoving;
	Snapshot.bIsHasInStandIdlePose   = InSource.bIsHasInStandIdlePose;
	Snapshot.bMovingToNotMoving      = InSource.bMovingToNotMoving;
	Snapshot.bIsCanEnterMoveState    = InSource.bIsCanEnterMoveState;
	Snapshot.bIsHoldingHands         = InSource.bIsHoldingHands;
	Snapshot.bIsIgnoreMoveInput      = InSource.bIsIgnoreMoveInput;
	Snapshot.Gait                    = InSource.Gait;
	Snapshot.VelocityLength          = InSource.VelocityLength;
	Snapshot.LastInputDirectionAngle = InSource.LastInputDirectionAngle;

	// ---- 顶层决策补全扩展（Layer 1 Main / Layer 2 Grounded / MM / IK） ----
	// 整型/枚举状态字段
	Snapshot.MoveAnimState           = InSource.MoveAnimState;
	Snapshot.VinesSubAnimState       = InSource.VinesSubAnimState;
	Snapshot.JumpCurrentCount        = InSource.JumpCurrentCount;
	Snapshot.GroundAnimState         = InSource.GroundAnimState;
	Snapshot.VaultSubState           = InSource.VaultSubState;

	// 浮点字段
	Snapshot.Velocity2DLength        = InSource.Velocity2DLength;

	// Main 层布尔字段
	Snapshot.bUseJumpTakeOff         = InSource.bUseJumpTakeOff;
	Snapshot.bSecondJump             = InSource.bSecondJump;
	Snapshot.bForceJump              = InSource.bForceJump;
	Snapshot.bWantsToLeaveGround     = InSource.bWantsToLeaveGround;
	Snapshot.bWantsAirAction         = InSource.bWantsAirAction;
	Snapshot.bIsLanding              = InSource.bIsLanding;
	Snapshot.bJumpApexReached        = InSource.bJumpApexReached;
	Snapshot.bOnVines                = InSource.bOnVines;
	Snapshot.bVaultTriggered         = InSource.bVaultTriggered;
	Snapshot.bStartedDriving         = InSource.bStartedDriving;
	Snapshot.bReturnToGround         = InSource.bReturnToGround;
	Snapshot.bCanDoubleJump          = InSource.bCanDoubleJump;
	Snapshot.bSpecialModeInAir       = InSource.bSpecialModeInAir;
	Snapshot.bLeaveVines             = InSource.bLeaveVines;
	Snapshot.bVinesAnimComplete      = InSource.bVinesAnimComplete;
	Snapshot.bVaultExitCondition     = InSource.bVaultExitCondition;
	Snapshot.bStopGliding            = InSource.bStopGliding;
	Snapshot.bLanding                = InSource.bLanding;
	Snapshot.bForceExitGliding       = InSource.bForceExitGliding;
	Snapshot.bResumeGliding          = InSource.bResumeGliding;
	Snapshot.bLeaveWater             = InSource.bLeaveWater;
	Snapshot.bStopDriving            = InSource.bStopDriving;

	// Main 层 provisional ⚠️ 待验证布尔字段
	Snapshot.bCoyoteTimeJump         = InSource.bCoyoteTimeJump;
	Snapshot.bEnteredFromAir         = InSource.bEnteredFromAir;
	Snapshot.bTakeOffComplete        = InSource.bTakeOffComplete;
	Snapshot.bCancelJump             = InSource.bCancelJump;
	Snapshot.bTransitionComplete     = InSource.bTransitionComplete;

	// Grounded 层布尔字段
	Snapshot.bCanStayingTheGround    = InSource.bCanStayingTheGround;
	Snapshot.bIsPlayingAnyMontage    = InSource.bIsPlayingAnyMontage;
	Snapshot.bIsGlidingLanded        = InSource.bIsGlidingLanded;
	Snapshot.VaultEndToStopL         = InSource.VaultEndToStopL;
	Snapshot.VaultEndToStopR         = InSource.VaultEndToStopR;

	// MM/IK 层布尔字段
	Snapshot.bForceExitMotionMatching = InSource.bForceExitMotionMatching;
	Snapshot.bMotionMatchComplete    = InSource.bMotionMatchComplete;
	Snapshot.bMotionMatchActive      = InSource.bMotionMatchActive;
	Snapshot.bFullBodyIKNeeded       = InSource.bFullBodyIKNeeded;

	return Snapshot;
}

float UGGYGOAnimInstance::MapSpeedToWalkRun(float InSpeed, float InWalkSpeed, float InRunSpeed)
{
	// 速度 → WalkRun 轴（0走 1跑）：以走速为下界、跑速为上界线性映射并钳制到 [0,1]。
	// GetMappedRangeValueClamped 保证 InSpeed ≤ InWalkSpeed 得 0、InSpeed ≥ InRunSpeed 得 1，
	// 区间内单调不减，输出恒落在 [0,1]（不接触任何引擎对象，可离线测试）。
	return FMath::GetMappedRangeValueClamped(
		FVector2D(InWalkSpeed, InRunSpeed), FVector2D(0.f, 1.f), InSpeed);
}

FName UGGYGOAnimInstance::ResolveStopKey(EMovementGait InGait, EAnimFoot InFoot)
{
	// 停步资产 key = "{gait}_stop{Side}"，纯函数不接触引擎/AnimSet，可离线测试（Property 7）。
	// 脚决定后缀：Left → "L"，Right → "R"。
	const TCHAR* Side = (InFoot == EAnimFoot::Left) ? TEXT("L") : TEXT("R");

	// key 规则（R12.2/R12.3/R12.4）：
	//   Walk        → walk_stop{Side}
	//   Sprint      → sprint_stop{Side}
	//   Run / None  → run_stop{Side}（None 归入 run 分支作降级默认）
	switch (InGait)
	{
	case EMovementGait::Walk:
		return FName(*FString::Printf(TEXT("walk_stop%s"), Side));
	case EMovementGait::Sprint:
		return FName(*FString::Printf(TEXT("sprint_stop%s"), Side));
	default: // Run / None
		return FName(*FString::Printf(TEXT("run_stop%s"), Side));
	}
}

FName UGGYGOAnimInstance::ResolveEnterKey(float InAngleDeg, EAnimFoot InFoot)
{
	// 起步资产 key，纯函数不接触引擎/AnimSet，可离线测试（Property 7）。
	// 脚决定 Forward/Back 分区的后缀：Left → "L"，Right → "R"。
	const TCHAR* Side = (InFoot == EAnimFoot::Left) ? TEXT("L") : TEXT("R");

	// 方向分区（对齐设计 A3 与 Enter_Dir_* 边界，互斥且完备）：
	//   Forward：|angle| ≤ 45          → run_enterF{Side}（run_enterFL / run_enterFR）
	//   Right  ：45 < angle ≤ 135      → run_enterR（无脚后缀）
	//   Left   ：-135 ≤ angle < -45    → run_enterL（无脚后缀）
	//   Back   ：|angle| > 135         → run_enterB_{Side}start（run_enterB_Lstart / run_enterB_Rstart）
	if (FMath::Abs(InAngleDeg) <= 45.f)
	{
		return FName(*FString::Printf(TEXT("run_enterF%s"), Side));
	}
	if (InAngleDeg > 45.f && InAngleDeg <= 135.f)
	{
		return FName("run_enterR");
	}
	if (InAngleDeg < -45.f && InAngleDeg >= -135.f)
	{
		return FName("run_enterL");
	}
	// Back（|angle| > 135）
	return FName(*FString::Printf(TEXT("run_enterB_%sstart"), Side));
}

// ============================================================================
// Shell 函数委托 — 决策实现已搬移到 Decision_Module，此处仅单行转发。
// 函数签名/UFUNCTION 声明保持不变，蓝图绑定零改动。
// ============================================================================

// ---- Layer 1 MainMovement → MainDecisions ----

bool UGGYGOAnimInstance::Main_To_Fall() const     { return MainDecisions.Main_To_Fall(); }
bool UGGYGOAnimInstance::Main_To_Jump() const     { return MainDecisions.Main_To_Jump(); }
bool UGGYGOAnimInstance::Main_To_Grounded() const { return MainDecisions.Main_To_Grounded(); }
bool UGGYGOAnimInstance::Main_To_Gliding() const  { return MainDecisions.Main_To_Gliding(); }
bool UGGYGOAnimInstance::Main_To_Swimming() const { return MainDecisions.Main_To_Swimming(); }

// ---- Layer 2 MainGrounded → GroundedDecisions ----

bool UGGYGOAnimInstance::Grounded_Just_Landed() const { return GroundedDecisions.Grounded_Just_Landed(); }

// ---- Layer 3 LocomotionStates (Loco_*) → LocoDecisions ----

bool UGGYGOAnimInstance::Loco_NotMoving_To_Enter() const  { return LocoDecisions.Loco_NotMoving_To_Enter(); }
bool UGGYGOAnimInstance::Loco_Enter_To_Moving() const     { return LocoDecisions.Loco_Enter_To_Moving(); }
bool UGGYGOAnimInstance::Loco_Enter_To_NotMoving() const  { return LocoDecisions.Loco_Enter_To_NotMoving(); }
bool UGGYGOAnimInstance::Loco_Moving_To_LeftStop() const  { return LocoDecisions.Loco_Moving_To_LeftStop(); }
bool UGGYGOAnimInstance::Loco_Moving_To_RightStop() const { return LocoDecisions.Loco_Moving_To_RightStop(); }
bool UGGYGOAnimInstance::Loco_Stop_To_Moving() const      { return LocoDecisions.Loco_Stop_To_Moving(); }
bool UGGYGOAnimInstance::Loco_Stop_To_NotMoving() const   { return LocoDecisions.Loco_Stop_To_NotMoving(); }

// ---- Layer 3.5 Direction Dispatcher → DirectionDecisions ----

bool UGGYGOAnimInstance::Enter_Dir_Forward() const         { return DirectionDecisions.Enter_Dir_Forward(); }
bool UGGYGOAnimInstance::Enter_Dir_Left() const            { return DirectionDecisions.Enter_Dir_Left(); }
bool UGGYGOAnimInstance::Enter_Dir_Right() const           { return DirectionDecisions.Enter_Dir_Right(); }
bool UGGYGOAnimInstance::Enter_Dir_Forward_Variant() const { return DirectionDecisions.Enter_Dir_Forward_Variant(); }
bool UGGYGOAnimInstance::Enter_Dir_Back() const            { return DirectionDecisions.Enter_Dir_Back(); }
bool UGGYGOAnimInstance::Enter_Exit_Check() const          { return DirectionDecisions.Enter_Exit_Check(); }

bool UGGYGOAnimInstance::EnterForward_Is_Sprint() const    { return DirectionDecisions.EnterForward_Is_Sprint(); }
bool UGGYGOAnimInstance::EnterForward_Is_Run() const       { return DirectionDecisions.EnterForward_Is_Run(); }
bool UGGYGOAnimInstance::EnterForwardVar_Is_Sprint() const { return DirectionDecisions.EnterForwardVar_Is_Sprint(); }
bool UGGYGOAnimInstance::EnterForwardVar_Is_Run() const    { return DirectionDecisions.EnterForwardVar_Is_Run(); }

bool UGGYGOAnimInstance::EnterBack_L_To_R() const { return DirectionDecisions.EnterBack_L_To_R(); }
bool UGGYGOAnimInstance::EnterBack_R_To_L() const { return DirectionDecisions.EnterBack_R_To_L(); }

bool UGGYGOAnimInstance::BackLeft_Start_To_B() const       { return DirectionDecisions.BackLeft_Start_To_B(); }
bool UGGYGOAnimInstance::BackLeft_Start_To_B_Auto() const  { return DirectionDecisions.BackLeft_Start_To_B_Auto(); }
bool UGGYGOAnimInstance::BackLeft_B_To_Exit() const        { return DirectionDecisions.BackLeft_B_To_Exit(); }
bool UGGYGOAnimInstance::BackRight_Start_To_B() const      { return DirectionDecisions.BackRight_Start_To_B(); }
bool UGGYGOAnimInstance::BackRight_Start_To_B_Auto() const { return DirectionDecisions.BackRight_Start_To_B_Auto(); }
bool UGGYGOAnimInstance::BackRight_B_To_Exit() const       { return DirectionDecisions.BackRight_B_To_Exit(); }

// ---- Layer 4.1 + 4.2 Detail → DetailDecisions ----

bool UGGYGOAnimInstance::Detail_EnterRun_To_Run() const   { return DetailDecisions.Detail_EnterRun_To_Run(); }
bool UGGYGOAnimInstance::Detail_Walk_To_Run() const       { return DetailDecisions.Detail_Walk_To_Run(); }
bool UGGYGOAnimInstance::Detail_Walk_To_WalkToRun() const { return DetailDecisions.Detail_Walk_To_WalkToRun(); }
bool UGGYGOAnimInstance::Detail_Run_To_TurnBack() const   { return DetailDecisions.Detail_Run_To_TurnBack(); }
bool UGGYGOAnimInstance::Detail_WalkToRun_To_Run() const  { return DetailDecisions.Detail_WalkToRun_To_Run(); }
bool UGGYGOAnimInstance::Detail_TurnBack_To_Run() const   { return DetailDecisions.Detail_TurnBack_To_Run(); }
bool UGGYGOAnimInstance::Detail_Run_To_Walk() const       { return DetailDecisions.Detail_Run_To_Walk(); }
bool UGGYGOAnimInstance::Detail_TurnBack_IsLeft() const   { return DetailDecisions.Detail_TurnBack_IsLeft(); }
bool UGGYGOAnimInstance::Detail_TurnBack_IsRight() const  { return DetailDecisions.Detail_TurnBack_IsRight(); }
bool UGGYGOAnimInstance::Gait_To_Sprint() const           { return DetailDecisions.Gait_To_Sprint(); }
bool UGGYGOAnimInstance::Gait_Exit_Sprint() const         { return DetailDecisions.Gait_Exit_Sprint(); }

// ---- Layer 4.3 StopGait → StopGaitDecisions ----

bool UGGYGOAnimInstance::StopGait_Is_Sprint() const         { return StopGaitDecisions.StopGait_Is_Sprint(); }
bool UGGYGOAnimInstance::StopGait_Is_Run() const            { return StopGaitDecisions.StopGait_Is_Run(); }
bool UGGYGOAnimInstance::StopGait_Is_Walk() const           { return StopGaitDecisions.StopGait_Is_Walk(); }
bool UGGYGOAnimInstance::StopGait_ForceRun_To_State() const { return StopGaitDecisions.StopGait_ForceRun_To_State(); }

// ---- Layer 5 LocomotionCycles → CyclesDecisions ----

bool UGGYGOAnimInstance::Cycles_To_Left() const            { return CyclesDecisions.Cycles_To_Left(); }
bool UGGYGOAnimInstance::Cycles_To_Right() const           { return CyclesDecisions.Cycles_To_Right(); }
bool UGGYGOAnimInstance::Cycles_To_RunStopRotation() const { return CyclesDecisions.Cycles_To_RunStopRotation(); }
bool UGGYGOAnimInstance::Cycles_To_StopRotation() const    { return CyclesDecisions.Cycles_To_StopRotation(); }
bool UGGYGOAnimInstance::Cycles_To_WalkStopRotation() const{ return CyclesDecisions.Cycles_To_WalkStopRotation(); }
bool UGGYGOAnimInstance::Cycles_StopRotation_Done() const  { return CyclesDecisions.Cycles_StopRotation_Done(); }
bool UGGYGOAnimInstance::Cycles_Conduit_IsLeft() const     { return CyclesDecisions.Cycles_Conduit_IsLeft(); }
bool UGGYGOAnimInstance::Cycles_Conduit_IsRight() const    { return CyclesDecisions.Cycles_Conduit_IsRight(); }

// ============================================================================
// State Callbacks
// ============================================================================

void UGGYGOAnimInstance::OnEnter_NotMoving()
{
	UE_LOG(LogGGYGOAnim, Verbose, TEXT("OnEnter: NotMoving"));
}

void UGGYGOAnimInstance::OnEnter_Moving()
{
	UE_LOG(LogGGYGOAnim, Verbose, TEXT("OnEnter: Moving"));
}

void UGGYGOAnimInstance::OnEnter_LeftStop()
{
	UE_LOG(LogGGYGOAnim, Verbose, TEXT("OnEnter: LeftStop"));
}

void UGGYGOAnimInstance::OnEnter_RightStop()
{
	UE_LOG(LogGGYGOAnim, Verbose, TEXT("OnEnter: RightStop"));
}

void UGGYGOAnimInstance::OnEnter_EnterMoveState()
{
	UE_LOG(LogGGYGOAnim, Verbose, TEXT("OnEnter: EnterMoveState"));
}

void UGGYGOAnimInstance::OnEnterState(FName InStateName)
{
	CurrentAnimStateName = InStateName;
	UE_LOG(LogGGYGOAnim, Verbose, TEXT("OnEnterState: %s"), *InStateName.ToString());
}

// ============================================================================
// Layer 3 LocomotionStatesMachine Shell 委托 (#662–#704) → LocoDecisions
// ============================================================================

bool UGGYGOAnimInstance::Loco3_Conduit_To_Moving_Sprint() const { return LocoDecisions.Loco3_Conduit_To_Moving_Sprint(); }
bool UGGYGOAnimInstance::Loco3_Conduit_To_Moving() const        { return LocoDecisions.Loco3_Conduit_To_Moving(); }
bool UGGYGOAnimInstance::Loco3_Conduit_To_NotMoving() const     { return LocoDecisions.Loco3_Conduit_To_NotMoving(); }

bool UGGYGOAnimInstance::Loco3_NotMoving_To_Moving_Auto() const { return LocoDecisions.Loco3_NotMoving_To_Moving_Auto(); }
bool UGGYGOAnimInstance::Loco3_NotMoving_To_Conduit5() const    { return LocoDecisions.Loco3_NotMoving_To_Conduit5(); }
bool UGGYGOAnimInstance::Loco3_NotMoving_To_Moving_Alt() const  { return LocoDecisions.Loco3_NotMoving_To_Moving_Alt(); }
bool UGGYGOAnimInstance::Loco3_NotMoving_To_Stop_MM() const     { return LocoDecisions.Loco3_NotMoving_To_Stop_MM(); }

bool UGGYGOAnimInstance::Loco3_Moving_To_NotMoving_Auto() const { return LocoDecisions.Loco3_Moving_To_NotMoving_Auto(); }
bool UGGYGOAnimInstance::Loco3_Moving_To_NotMoving() const      { return LocoDecisions.Loco3_Moving_To_NotMoving(); }
bool UGGYGOAnimInstance::Loco3_Moving_To_Conduit1() const       { return LocoDecisions.Loco3_Moving_To_Conduit1(); }

bool UGGYGOAnimInstance::Loco3_Stop_To_NotMoving_Auto1() const { return LocoDecisions.Loco3_Stop_To_NotMoving_Auto1(); }
bool UGGYGOAnimInstance::Loco3_Stop_To_NotMoving_Auto2() const { return LocoDecisions.Loco3_Stop_To_NotMoving_Auto2(); }
bool UGGYGOAnimInstance::Loco3_Stop_To_Conduit4() const        { return LocoDecisions.Loco3_Stop_To_Conduit4(); }

bool UGGYGOAnimInstance::Loco3_NotMoving1_To_Moving1_Patrol() const { return LocoDecisions.Loco3_NotMoving1_To_Moving1_Patrol(); }
bool UGGYGOAnimInstance::Loco3_NotMoving1_To_LeftStop1() const      { return LocoDecisions.Loco3_NotMoving1_To_LeftStop1(); }
bool UGGYGOAnimInstance::Loco3_NotMoving1_To_RightStop1() const     { return LocoDecisions.Loco3_NotMoving1_To_RightStop1(); }
bool UGGYGOAnimInstance::Loco3_NotMoving1_To_Moving1_Should() const { return LocoDecisions.Loco3_NotMoving1_To_Moving1_Should(); }
bool UGGYGOAnimInstance::Loco3_Moving1_To_NotMoving1() const        { return LocoDecisions.Loco3_Moving1_To_NotMoving1(); }
bool UGGYGOAnimInstance::Loco3_Moving1_To_CanStop1() const          { return LocoDecisions.Loco3_Moving1_To_CanStop1(); }
bool UGGYGOAnimInstance::Loco3_AutoRule_Fallback() const           { return LocoDecisions.Loco3_AutoRule_Fallback(); }

bool UGGYGOAnimInstance::Loco3_Conduit2_To_Moving1_Sprint() const { return LocoDecisions.Loco3_Conduit2_To_Moving1_Sprint(); }
bool UGGYGOAnimInstance::Loco3_Conduit2_To_Moving1() const        { return LocoDecisions.Loco3_Conduit2_To_Moving1(); }
bool UGGYGOAnimInstance::Loco3_Conduit2_To_NotMoving1() const     { return LocoDecisions.Loco3_Conduit2_To_NotMoving1(); }

bool UGGYGOAnimInstance::Loco3_LeftStop1_To_NotMoving1_Auto() const  { return LocoDecisions.Loco3_LeftStop1_To_NotMoving1_Auto(); }
bool UGGYGOAnimInstance::Loco3_Stop1_To_Moving1_Resume() const       { return LocoDecisions.Loco3_Stop1_To_Moving1_Resume(); }
bool UGGYGOAnimInstance::Loco3_RightStop1_To_NotMoving1_Auto() const { return LocoDecisions.Loco3_RightStop1_To_NotMoving1_Auto(); }

bool UGGYGOAnimInstance::Loco3_CanStop1_To_Conduit1_1_1() const { return LocoDecisions.Loco3_CanStop1_To_Conduit1_1_1(); }
bool UGGYGOAnimInstance::Loco3_CanStop1_To_Conduit1_2() const   { return LocoDecisions.Loco3_CanStop1_To_Conduit1_2(); }

bool UGGYGOAnimInstance::Loco3_StopConduit_NoSprint() const { return LocoDecisions.Loco3_StopConduit_NoSprint(); }
bool UGGYGOAnimInstance::Loco3_StopConduit_Sprint() const   { return LocoDecisions.Loco3_StopConduit_Sprint(); }

bool UGGYGOAnimInstance::Loco3_Conduit1_To_Stop_A() const { return LocoDecisions.Loco3_Conduit1_To_Stop_A(); }
bool UGGYGOAnimInstance::Loco3_Conduit1_To_Stop_B() const { return LocoDecisions.Loco3_Conduit1_To_Stop_B(); }

bool UGGYGOAnimInstance::Loco3_EnterMove_To_Moving() const   { return LocoDecisions.Loco3_EnterMove_To_Moving(); }
bool UGGYGOAnimInstance::Loco3_EnterMove_To_Conduit1() const { return LocoDecisions.Loco3_EnterMove_To_Conduit1(); }

bool UGGYGOAnimInstance::Loco3_Conduit3_To_Moving() const     { return LocoDecisions.Loco3_Conduit3_To_Moving(); }
bool UGGYGOAnimInstance::Loco3_Conduit3_To_EnterState() const { return LocoDecisions.Loco3_Conduit3_To_EnterState(); }

bool UGGYGOAnimInstance::Loco3_EnterState_To_EnterMove() const { return LocoDecisions.Loco3_EnterState_To_EnterMove(); }

bool UGGYGOAnimInstance::Loco3_Conduit5_To_Conduit3() const   { return LocoDecisions.Loco3_Conduit5_To_Conduit3(); }
bool UGGYGOAnimInstance::Loco3_Conduit5_To_Moving_Walk() const{ return LocoDecisions.Loco3_Conduit5_To_Moving_Walk(); }
bool UGGYGOAnimInstance::Loco3_Conduit5_To_EnterWalk() const  { return LocoDecisions.Loco3_Conduit5_To_EnterWalk(); }

bool UGGYGOAnimInstance::Loco3_EnterWalk_To_Moving() const { return LocoDecisions.Loco3_EnterWalk_To_Moving(); }

bool UGGYGOAnimInstance::Loco3_Conduit4_To_EnterState() const { return LocoDecisions.Loco3_Conduit4_To_EnterState(); }
bool UGGYGOAnimInstance::Loco3_Conduit4_To_Moving() const     { return LocoDecisions.Loco3_Conduit4_To_Moving(); }

// ============================================================================
// Layer 5 MoveR / MoveL Sprint transitions → CyclesDecisions
// ============================================================================

bool UGGYGOAnimInstance::MoveR_Conduit_To_RunWalk() const          { return CyclesDecisions.MoveR_Conduit_To_RunWalk(); }
bool UGGYGOAnimInstance::MoveR_Conduit_To_SprintToRunWalk() const  { return CyclesDecisions.MoveR_Conduit_To_SprintToRunWalk(); }
bool UGGYGOAnimInstance::MoveR_RunWalk_To_Sprint() const           { return CyclesDecisions.MoveR_RunWalk_To_Sprint(); }
bool UGGYGOAnimInstance::MoveR_Sprint_To_RunWalk() const           { return CyclesDecisions.MoveR_Sprint_To_RunWalk(); }
bool UGGYGOAnimInstance::MoveR_SprintToRunWalk_To_RunWalk() const  { return CyclesDecisions.MoveR_SprintToRunWalk_To_RunWalk(); }

bool UGGYGOAnimInstance::MoveL_Conduit_To_RunWalk() const          { return CyclesDecisions.MoveL_Conduit_To_RunWalk(); }
bool UGGYGOAnimInstance::MoveL_Conduit_To_SprintToRunWalk() const  { return CyclesDecisions.MoveL_Conduit_To_SprintToRunWalk(); }
bool UGGYGOAnimInstance::MoveL_RunWalk_To_Sprint() const           { return CyclesDecisions.MoveL_RunWalk_To_Sprint(); }
bool UGGYGOAnimInstance::MoveL_Sprint_To_RunWalk() const           { return CyclesDecisions.MoveL_Sprint_To_RunWalk(); }
bool UGGYGOAnimInstance::MoveL_SprintToRunWalk_To_RunWalk() const  { return CyclesDecisions.MoveL_SprintToRunWalk_To_RunWalk(); }

// ============================================================================
// Layer 1 MainMovement 顶层过渡补全 Shell 委托 (#345–#378) → MainDecisions
// ============================================================================

// ---- Grounded 子机过渡（#345–#349）----
bool UGGYGOAnimInstance::MainMove_Grounded_To_MovementState() const { return MainDecisions.MainMove_Grounded_To_MovementState(); }
bool UGGYGOAnimInstance::MainMove_Grounded_To_JumpTakeOff() const   { return MainDecisions.MainMove_Grounded_To_JumpTakeOff(); }
bool UGGYGOAnimInstance::MainMove_Grounded_To_Jump() const          { return MainDecisions.MainMove_Grounded_To_Jump(); }
bool UGGYGOAnimInstance::MainMove_Grounded_To_Jump_Instant() const  { return MainDecisions.MainMove_Grounded_To_Jump_Instant(); }

// ---- Fall 子机过渡（#350–#352）----
bool UGGYGOAnimInstance::MainMove_Fall_To_InAir() const { return MainDecisions.MainMove_Fall_To_InAir(); }
bool UGGYGOAnimInstance::MainMove_Fall_To_Land() const  { return MainDecisions.MainMove_Fall_To_Land(); }
bool UGGYGOAnimInstance::MainMove_Fall_To_Jump() const  { return MainDecisions.MainMove_Fall_To_Jump(); }

// ---- Jump 子机过渡（#353–#354）----
bool UGGYGOAnimInstance::MainMove_Jump_To_InAir() const { return MainDecisions.MainMove_Jump_To_InAir(); }
bool UGGYGOAnimInstance::MainMove_Jump_To_Land() const  { return MainDecisions.MainMove_Jump_To_Land(); }

// ---- MovementState (<-MS->) 分派（#355–#360）----
bool UGGYGOAnimInstance::MainMove_MS_To_InAir() const     { return MainDecisions.MainMove_MS_To_InAir(); }
bool UGGYGOAnimInstance::MainMove_MS_To_Vines() const     { return MainDecisions.MainMove_MS_To_Vines(); }
bool UGGYGOAnimInstance::MainMove_MS_To_VaultToAir() const{ return MainDecisions.MainMove_MS_To_VaultToAir(); }
bool UGGYGOAnimInstance::MainMove_MS_To_Swimming() const  { return MainDecisions.MainMove_MS_To_Swimming(); }
bool UGGYGOAnimInstance::MainMove_MS_To_Driving() const   { return MainDecisions.MainMove_MS_To_Driving(); }
bool UGGYGOAnimInstance::MainMove_MS_To_Grounded() const  { return MainDecisions.MainMove_MS_To_Grounded(); }

// ---- InAir 导管（#361–#363）----
bool UGGYGOAnimInstance::MainMove_InAir_To_Jump() const { return MainDecisions.MainMove_InAir_To_Jump(); }
bool UGGYGOAnimInstance::MainMove_InAir_To_MS() const   { return MainDecisions.MainMove_InAir_To_MS(); }
bool UGGYGOAnimInstance::MainMove_InAir_To_Fall() const { return MainDecisions.MainMove_InAir_To_Fall(); }

// ---- Land 导管（#364）----
bool UGGYGOAnimInstance::MainMove_Land_To_Grounded() const { return MainDecisions.MainMove_Land_To_Grounded(); }

// ---- TakeOff 子机（#365–#366）----
bool UGGYGOAnimInstance::MainMove_TakeOff_To_Jump() const { return MainDecisions.MainMove_TakeOff_To_Jump(); }
bool UGGYGOAnimInstance::MainMove_TakeOff_To_MS() const   { return MainDecisions.MainMove_TakeOff_To_MS(); }

// ---- Vines 子机（#367–#368）----
bool UGGYGOAnimInstance::MainMove_Vines_To_MS() const      { return MainDecisions.MainMove_Vines_To_MS(); }
bool UGGYGOAnimInstance::MainMove_Vines_To_MS_Auto() const { return MainDecisions.MainMove_Vines_To_MS_Auto(); }

// ---- Vault 子机（#369–#370）----
bool UGGYGOAnimInstance::MainMove_Vault_To_MS() const      { return MainDecisions.MainMove_Vault_To_MS(); }
bool UGGYGOAnimInstance::MainMove_Vault_To_MS_Auto() const { return MainDecisions.MainMove_Vault_To_MS_Auto(); }

// ---- Gliding / GlidingToFall（#371–#375）----
bool UGGYGOAnimInstance::MainMove_Gliding_To_GlidingToFall() const    { return MainDecisions.MainMove_Gliding_To_GlidingToFall(); }
bool UGGYGOAnimInstance::MainMove_Gliding_To_OutGliding() const       { return MainDecisions.MainMove_Gliding_To_OutGliding(); }
bool UGGYGOAnimInstance::MainMove_GlidingToFall_To_Fall() const       { return MainDecisions.MainMove_GlidingToFall_To_Fall(); }
bool UGGYGOAnimInstance::MainMove_GlidingToFall_To_Gliding() const    { return MainDecisions.MainMove_GlidingToFall_To_Gliding(); }
bool UGGYGOAnimInstance::MainMove_GlidingToFall_To_OutGliding() const { return MainDecisions.MainMove_GlidingToFall_To_OutGliding(); }

// ---- Swimming / Driving（#376–#377）----
bool UGGYGOAnimInstance::MainMove_Swimming_To_MS() const { return MainDecisions.MainMove_Swimming_To_MS(); }
bool UGGYGOAnimInstance::MainMove_Driving_To_MS() const  { return MainDecisions.MainMove_Driving_To_MS(); }

// ---- OutGliding 导管（#378）----
bool UGGYGOAnimInstance::MainMove_OutGliding_To_MS() const { return MainDecisions.MainMove_OutGliding_To_MS(); }

// ============================================================================
// Layer 2 MainGrounded 顶层过渡补全 Shell 委托 (#468–#500) → GroundedDecisions
// ============================================================================

// ---- Entry 分派（#468–#477）----
bool UGGYGOAnimInstance::MG_Entry_To_VaultContinue() const    { return GroundedDecisions.MG_Entry_To_VaultContinue(); }
bool UGGYGOAnimInstance::MG_Entry_To_Conduit() const          { return GroundedDecisions.MG_Entry_To_Conduit(); }
bool UGGYGOAnimInstance::MG_Entry_To_FromRoll() const         { return GroundedDecisions.MG_Entry_To_FromRoll(); }
bool UGGYGOAnimInstance::MG_Entry_To_LandedMobile() const     { return GroundedDecisions.MG_Entry_To_LandedMobile(); }
bool UGGYGOAnimInstance::MG_Entry_To_Landed() const           { return GroundedDecisions.MG_Entry_To_Landed(); }
bool UGGYGOAnimInstance::MG_Entry_To_MainGS() const           { return GroundedDecisions.MG_Entry_To_MainGS(); }
bool UGGYGOAnimInstance::MG_Entry_To_VinesOver() const        { return GroundedDecisions.MG_Entry_To_VinesOver(); }
bool UGGYGOAnimInstance::MG_Entry_To_RunOnWallsOver() const   { return GroundedDecisions.MG_Entry_To_RunOnWallsOver(); }
bool UGGYGOAnimInstance::MG_Entry_To_LandedStationary() const { return GroundedDecisions.MG_Entry_To_LandedStationary(); }
bool UGGYGOAnimInstance::MG_Entry_To_SprintVinesOver() const  { return GroundedDecisions.MG_Entry_To_SprintVinesOver(); }

// ---- FromRoll（#478–#479）----
bool UGGYGOAnimInstance::MG_FromRoll_To_MainGS() const    { return GroundedDecisions.MG_FromRoll_To_MainGS(); }
bool UGGYGOAnimInstance::MG_FromRoll_To_RollToRun() const { return GroundedDecisions.MG_FromRoll_To_RollToRun(); }

// ---- LandedStationary（#480–#483）----
bool UGGYGOAnimInstance::MG_LandedStat_To_MainGS_Auto() const { return GroundedDecisions.MG_LandedStat_To_MainGS_Auto(); }
bool UGGYGOAnimInstance::MG_LandedStat_To_StatToMove() const  { return GroundedDecisions.MG_LandedStat_To_StatToMove(); }
bool UGGYGOAnimInstance::MG_LandedStat_To_LandedMob() const   { return GroundedDecisions.MG_LandedStat_To_LandedMob(); }
bool UGGYGOAnimInstance::MG_LandedStat_To_MainGS() const      { return GroundedDecisions.MG_LandedStat_To_MainGS(); }

// ---- LandedMobile（#484–#485）----
bool UGGYGOAnimInstance::MG_LandedMob_To_MainGS_Auto() const { return GroundedDecisions.MG_LandedMob_To_MainGS_Auto(); }
bool UGGYGOAnimInstance::MG_LandedMob_To_MainGS() const      { return GroundedDecisions.MG_LandedMob_To_MainGS(); }

// ---- StatToMove（#486–#487）----
bool UGGYGOAnimInstance::MG_StatToMove_To_MainGS_Auto() const { return GroundedDecisions.MG_StatToMove_To_MainGS_Auto(); }
bool UGGYGOAnimInstance::MG_StatToMove_To_MainGS() const      { return GroundedDecisions.MG_StatToMove_To_MainGS(); }

// ---- RollToRun（#488–#489）----
bool UGGYGOAnimInstance::MG_RollToRun_To_MainGS_Auto() const { return GroundedDecisions.MG_RollToRun_To_MainGS_Auto(); }
bool UGGYGOAnimInstance::MG_RollToRun_To_MainGS() const      { return GroundedDecisions.MG_RollToRun_To_MainGS(); }

// ---- Landed（#490–#491）----
bool UGGYGOAnimInstance::MG_Landed_To_LandedStat() const { return GroundedDecisions.MG_Landed_To_LandedStat(); }
bool UGGYGOAnimInstance::MG_Landed_To_StatToMove() const { return GroundedDecisions.MG_Landed_To_StatToMove(); }

// ---- VaultContinue（#492）----
bool UGGYGOAnimInstance::MG_VaultCont_To_MainGS() const { return GroundedDecisions.MG_VaultCont_To_MainGS(); }

// ---- VinesOver（#493–#494）----
bool UGGYGOAnimInstance::MG_VinesOver_To_MainGS_Auto() const { return GroundedDecisions.MG_VinesOver_To_MainGS_Auto(); }
bool UGGYGOAnimInstance::MG_VinesOver_To_MainGS() const      { return GroundedDecisions.MG_VinesOver_To_MainGS(); }

// ---- RunOnWallsOver（#495–#496）----
bool UGGYGOAnimInstance::MG_RunWalls_To_MainGS_Auto() const { return GroundedDecisions.MG_RunWalls_To_MainGS_Auto(); }
bool UGGYGOAnimInstance::MG_RunWalls_To_MainGS() const      { return GroundedDecisions.MG_RunWalls_To_MainGS(); }

// ---- Conduit（#497–#498）----
bool UGGYGOAnimInstance::MG_Conduit_To_LandedMob() const { return GroundedDecisions.MG_Conduit_To_LandedMob(); }
bool UGGYGOAnimInstance::MG_Conduit_To_Landed() const    { return GroundedDecisions.MG_Conduit_To_Landed(); }

// ---- SprintVinesOver（#499–#500）----
bool UGGYGOAnimInstance::MG_SprintVines_To_MainGS() const      { return GroundedDecisions.MG_SprintVines_To_MainGS(); }
bool UGGYGOAnimInstance::MG_SprintVines_To_MainGS_Auto() const { return GroundedDecisions.MG_SprintVines_To_MainGS_Auto(); }

// ============================================================================
// 顶层辅助 MotionMatch Shell 委托 (#44–#45) → MotionMatchDecisions
// ============================================================================

bool UGGYGOAnimInstance::MM_To_Base_Instant() const { return MotionMatchDecisions.MM_To_Base_Instant(); }
bool UGGYGOAnimInstance::MM_To_Base_Smooth() const  { return MotionMatchDecisions.MM_To_Base_Smooth(); }

// ============================================================================
// 顶层辅助 FullBodyIK Shell 委托 (#64–#65) → FullBodyIKDecisions
// ============================================================================

bool UGGYGOAnimInstance::FullBodyIK_Activate() const   { return FullBodyIKDecisions.FullBodyIK_Activate(); }
bool UGGYGOAnimInstance::FullBodyIK_Deactivate() const { return FullBodyIKDecisions.FullBodyIK_Deactivate(); }
