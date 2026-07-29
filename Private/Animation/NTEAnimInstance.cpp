/**
 * @file NTEAnimInstance.cpp
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

#include "Animation/NTEAnimInstance.h"

#include "BaseCharacter.h"

#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"  // GetBlendSpaceByKey 返回类型完整定义
#include "Animation/AnimTypes.h"   // FMarkerSyncAnimPosition

// 动画决策层日志分类：状态回调等辅助调试信息以 Verbose 级别输出，
// 默认不刷屏（动画层每帧调用），需要排查时提升 verbosity 观察。
DEFINE_LOG_CATEGORY_STATIC(LogNTEAnim, Log, All);

// ============================================================================
// AnimInstance 生命周期入口（三线程阶段）
// ============================================================================

void UNTEAnimInstance::NativeInitializeAnimation()
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

void UNTEAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
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

void UNTEAnimInstance::NativeThreadSafeUpdateAnimation(float DeltaSeconds)
{
	// 调用父类实现（可与游戏线程并行）
	Super::NativeThreadSafeUpdateAnimation(DeltaSeconds);

	// worker 线程每帧更新：只读快照，计算数值型输出变量
	UpdateOutputs(DeltaSeconds);
}

// ============================================================================
// Push Interface（游戏线程，外部系统每帧调用）
// ============================================================================

void UNTEAnimInstance::SetAnimRuntimeData(const FAnimRuntimeData& InData)
{
	StoredRuntimeData = InData;
}

// ============================================================================
// 游戏线程私有方法（唯一能安全读 Owner/Actor 的位置）
// ============================================================================

void UNTEAnimInstance::CaptureSnapshot()
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

		// 关键：即使 Owner 无效（编辑器预览/关联失败），也必须把默认快照指针分发给各决策模块。
		// 否则决策模块的 Snap 指针为空，Loco3_Conduit_To_* 等全部 `if(!Snap) return false`，
		// 导致 LocomotionStatesMachine 的入口 Conduit 无有效出边 → 退回参考姿势并报警。
		// 分发默认快照后，Loco3_Conduit_To_NotMoving() 返回 true（默认无移动意图）→ 导管落到 NotMoving。
		BindSnapshotToDecisionModules();
		return;
	}

	// 聚合来源字段：从 ABaseCharacter getter 逐项读入，随后交由纯函数 BuildSnapshot 映射。
	// 数据来源映射（每字段 ← 来源 getter/字段）：
	FAnimSourceData Src;

	// 移动意图应来自本帧解析出的步态，而不是实际物理速度。
	// 松手后即使角色仍有惯性，ResolvedGait 也会立即变为 None，状态机可当帧进入 Stop。
	Src.DesiredGait = OwnerPtr->GetResolvedGait();
	Src.bWantMove   = (Src.DesiredGait != EMovementGait::None);
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

	// ============================================================
	// 地面移动层（LocomotionStatesMachine）数据补全
	// 这些标志原靠外部 SetAnimRuntimeData 推入（无来源→恒 false，导致除 NotMoving/Moving
	// 外的状态永远进不去）。此处按分类就地计算：
	//   A类·映射现有移动数据；B类·动画层自算（基于移动意图 + 1 帧滞后）。
	//   C/D类（牵手/忽略输入/技能打断/MM/巡逻）保持外部默认值（无需求时恒 false）。
	// ============================================================
	{
		const bool bWant = Src.bWantMove;

		// —— A 类：映射现有移动数据 ——
		// 移动时使用本帧解析步态；松手后保留上一帧步态，供 StopGait 子机分流。
		// DesiredGait 在无输入时会立即变为 None，不能直接拿它判断 Walk/Run/Sprint Stop。
		Src.Gait                    = bWant ? Src.DesiredGait : Snap.Gait;
		Src.VelocityLength          = Src.Speed;                      // 速度标量 ← GetCurrentSpeed
		Src.LastInputDirectionAngle = Src.MoveAngleDeg;              // 输入方向角 ← GetMoveAngle
		// 用"移动意图"而非残留速度：否则松开摇杆后速度仍在衰减（Speed>1），
		// 会让 Stop 状态经 Conduit_4（含 bShouldMove）立刻复位回移动，停步动画放不完。
		Src.bShouldMove             = bWant;                          // 应该移动 = 有移动意图
		Src.bIsLeftFootC            = (Src.LocomotionPhase < 0.5f);   // 当前左脚（相位<0.5）

		// —— B 类：动画层自算 ——
		Src.bNotMovingToMoving      = bWant;    // 有移动意图 → 该起步
		Src.bMovingToNotMoving      = !bWant;   // 无移动意图 → 该停步
		Src.bEntryMovingOrNotMoving = bWant;    // 进入 Locomotion 那刻的移动意图
		// 1 帧滞后：起步那帧 bWant 已翻真，但上一帧仍待机，故 bIsHasInStandIdlePose 仍为真，
		// 使 NotMoving→Conduit_5（bIsHasInStandIdlePose && bNotMovingToMoving）能在该帧成立。
		Src.bIsHasInStandIdlePose   = !bPrevWantMove;   // 上一帧待机 = 已处于站立待机姿势
		Src.bIsCanEnterMoveState    = !bPrevWantMove;   // 待机稳定后才允许进起步
		Src.bIsCanRunStop           = bPrevWantMove;    // 上一帧在移动 → 现在可跑停
		Src.bIsSprintStop           = (Src.DesiredGait == EMovementGait::Sprint) && !bWant; // 冲刺急停
	}

	// 纯拷贝映射写入快照（映射恒等）
	Snap = BuildSnapshot(Src);

	// 必须在起步/停步计时初始化之前先从本帧同步相位解析脚位。
	// 原顺序在这里仍是 BuildSnapshot 的默认 Left，随后才写入 QueryCurrentFoot，
	// 会导致 EnterDuration/LatchedStopFoot 按错误脚位查表；若左右资产长度或姿势不同，
	// 状态切换帧会出现一次资产/姿势跳变。
	Snap.CurrentFoot = QueryCurrentFoot();

	// 停步支撑脚锁存 + 停步计时：在"移动→停止"的那一帧（本帧无意图、上一帧有意图）
	// 锁定当前支撑脚，并按锁定脚查出停步动画时长、计时清零。
	if (!Snap.bWantMove && bPrevWantMove)
	{
		LatchedStopFoot = Snap.CurrentFoot;
		StopElapsed = 0.f;
		const UAnimSequence* StopSeq = AnimSet.StopSequences.FindRef(ResolveStopKey(Snap.DesiredGait, LatchedStopFoot));
		StopDuration = StopSeq ? StopSeq->GetPlayLength() : 0.f;
	}

	// 无移动意图（停步/待机）期间累加计时；有停步动画时累计到其时长即判定"播完"。
	if (!Snap.bWantMove)
	{
		StopElapsed += GetDeltaSeconds();
	}
	// bStopFinished：无停步动画（长度<=0）视为立即完成；否则累计时间 >= 动画时长即完成。
	// 供 Stop→NotMoving 出边判定"停步动画播完才回待机"，取代原 return true 的秒过渡。
	Snap.bStopFinished = (StopDuration <= 0.f) || (StopElapsed >= StopDuration);

	// 起步计时：在"待机→移动"那一帧（本帧有意图、上一帧无意图）清零，并按方向+脚查出起步动画时长。
	if (Snap.bWantMove && !bPrevWantMove)
	{
		EnterElapsed = 0.f;
		const FName EnterKey = ResolveEnterKey(Snap.MoveAngleDeg, Snap.CurrentFoot);
		const UAnimSequence* EnterSeq = AnimSet.EnterSequences.FindRef(EnterKey);
		EnterDuration = EnterSeq ? EnterSeq->GetPlayLength() : 0.f;
	}
	// 有移动意图期间累加计时；累计到起步动画时长即判定"起步播完"。
	if (Snap.bWantMove)
	{
		EnterElapsed += GetDeltaSeconds();
	}
	// bEnterFinished：无起步动画（长度<=0）视为立即完成；否则累计时间 >= 动画时长即完成。
	// 供 EnterMoveState→Moving 判定"起步动画播完才进移动循环"。
	Snap.bEnterFinished = (EnterDuration <= 0.f) || (EnterElapsed >= EnterDuration);

	// 记录本帧移动意图，供下一帧"动画自算"标志的 1 帧滞后使用。
	bPrevWantMove = Src.bWantMove;

	// 由循环相位推导当前支撑脚写回快照：相位 ≥ 0.5 → Right，< 0.5 → Left（R13.2）。
	// QueryCurrentFoot 只读 Snap.LocomotionPhase，须在 BuildSnapshot 填好相位之后调用。
	Snap.CurrentFoot = QueryCurrentFoot();

	// 调试输出：当前支撑脚字符串（"L"/"R"），供 AnimGraph/调试面板观察（R13.3）。
	Out_DebugFoot = (Snap.CurrentFoot == EAnimFoot::Left) ? FName("L") : FName("R");

	// ---- 分发快照指针到各 Decision Module ----
	BindSnapshotToDecisionModules();
}

void UNTEAnimInstance::BindSnapshotToDecisionModules()
{
	// 把当前 Snap 的地址分发给所有决策模块。无论 Owner 是否有效都要调用，
	// 保证决策函数的 Snap 指针非空（否则 `if(!Snap) return false` 会让入口 Conduit 无出边）。
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

float UNTEAnimInstance::GetLocomotionSyncPhase() const
{
	// 查询 "Locomotion" 同步组的 marker 相位，映射为归一化相位 [0,1)。
	// GetSyncGroupPosition 为 UAnimInstance 的 BlueprintThreadSafe 方法，返回当前夹在哪两个
	// marker 之间（PreviousMarkerName/NextMarkerName）与之间插值比例（PositionBetweenMarkers）。
	// 循环动画上打了 Left / Right 两个 SyncMarker，把一个步态循环切成两个半区：
	//   刚过 Left  marker（Prev==Left）  → 相位落在 [0, 0.5)   → 左脚支撑
	//   刚过 Right marker（Prev==Right） → 相位落在 [0.5, 1)   → 右脚支撑
	// 同步组未配置 / marker 未注入时保留上一帧相位；首次运行时 Snap 默认相位为 0，
	// 因此仍会安全降级为 Left，不会把已经判定出的 Right 强制重置掉。
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

	// 无有效 marker：保留上一帧有效相位，避免 EnterMoveState（使用 RunStart/DoNotSync）
	// 暂时没有 Locomotion 成员时把脚位强制重置为 Left，进而让 Out_BlendSpace 在
	// EnterMoveState→Moving 切换帧从 WalkRun_R/L 之间跳变。首次运行时 Snap 默认相位 0，
	// 仍然是安全的 Left 降级。
	return FMath::Clamp(Snap.LocomotionPhase, 0.f, 0.999999f);
}

EAnimFoot UNTEAnimInstance::QueryCurrentFoot() const
{
	// 由循环归一化相位划分支撑脚：相位 ≥ 0.5 → Right，< 0.5 → Left（R13.2）。
	// 首次没有有效相位时默认 0 → 落在 Left 分支；后续无 marker 时由 GetLocomotionSyncPhase 保留上一帧相位。
	return (Snap.LocomotionPhase >= 0.5f) ? EAnimFoot::Right : EAnimFoot::Left;
}

void UNTEAnimInstance::RefreshOneShotAssets()
{
	// 游戏线程：按快照的步态+脚+方向解析 key，从 AnimSet 查资产写入 UObject 指针型输出变量。
	// 改 UObject 指针在 worker 线程不安全，因此本方法只在游戏线程 NativeUpdateAnimation 调用。
	// key 计算委托给纯函数 ResolveStopKey/ResolveEnterKey（离线可测），本方法只做 AnimSet 查表。

	// 停步资产：按步态 + **锁定的停步脚** 解析 key，从 AnimSet.StopSequences 查表。
	// 用 LatchedStopFoot（进 Stop 那刻锁定）而非实时 Snap.CurrentFoot——否则停步动画播放时
	// 同步相位推进会让 CurrentFoot 左右翻，Out_StopSeq 跟着换、Sequence Player 反复重启、播不完。
	// key 不存在时 FindRef 返回 nullptr → 输出变量置空（安全降级，R12.6）。
	const FName StopKey = ResolveStopKey(Snap.Gait, LatchedStopFoot);
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

void UNTEAnimInstance::UpdateOutputs(float Dt)
{
	// worker 线程：只读游戏线程填好的 Snap 与常量配置 Tuning，写数值型输出变量。
	// 不访问 Owner/Actor/组件——从设计上消除竞态。

	// 速度 → WalkRun 轴目标（0走 1跑），经走速→跑速映射并钳制到 [0,1]
	const float Target = MapSpeedToWalkRun(Snap.Speed, Tuning.WalkSpeed, Tuning.RunSpeed);

	// 以 VelocityBlendInterp 为速率插值平滑，避免速度抖动导致 BlendSpace 采样跳变
	Out_WalkRun = Target;

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

float UNTEAnimInstance::ComputeStride() const
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

FAnimSnapshot UNTEAnimInstance::BuildSnapshot(const FAnimSourceData& InSource)
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

float UNTEAnimInstance::MapSpeedToWalkRun(float InSpeed, float InWalkSpeed, float InRunSpeed)
{
	// 速度 → WalkRun 轴（0走 1跑）：以走速为下界、跑速为上界线性映射并钳制到 [0,1]。
	// GetMappedRangeValueClamped 保证 InSpeed ≤ InWalkSpeed 得 0、InSpeed ≥ InRunSpeed 得 1，
	// 区间内单调不减，输出恒落在 [0,1]（不接触任何引擎对象，可离线测试）。
	return FMath::GetMappedRangeValueClamped(
		FVector2D(InWalkSpeed, InRunSpeed), FVector2D(0.f, 1.f), InSpeed);
}

FName UNTEAnimInstance::ResolveStopKey(EMovementGait InGait, EAnimFoot InFoot)
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

FName UNTEAnimInstance::ResolveEnterKey(float InAngleDeg, EAnimFoot InFoot)
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
// 配表查询（按 key 取 AnimSet 资产）
//
// 路线 2（复刻 NTE 拓扑）下，各末端状态在动画播放节点的资产引脚上 Bind 这些函数，
// 传入该状态对应的 key 字面量，从 AnimSet 配表取资产。key 不存在返回 nullptr（安全降级）。
// 只读初始化后不变的 AnimSet 配置，不接触 Owner/Actor，worker 线程调用安全。
// ============================================================================

UAnimSequence* UNTEAnimInstance::GetEnterSeqByKey(FName Key) const
{
	return AnimSet.EnterSequences.FindRef(Key);
}

UAnimSequence* UNTEAnimInstance::GetStopSeqByKey(FName Key) const
{
	return AnimSet.StopSequences.FindRef(Key);
}

UAnimSequence* UNTEAnimInstance::GetMiscSeqByKey(FName Key) const
{
	return AnimSet.MiscSequences.FindRef(Key);
}

UAnimSequence* UNTEAnimInstance::GetLoopSeqByKey(FName Key) const
{
	return AnimSet.LoopSequences.FindRef(Key);
}

UBlendSpace* UNTEAnimInstance::GetBlendSpaceByKey(FName Key) const
{
	return AnimSet.BlendSpaces.FindRef(Key);
}

// ============================================================================
// Shell 函数委托 — 决策实现已搬移到 Decision_Module，此处仅单行转发。
// 函数签名/UFUNCTION 声明保持不变，蓝图绑定零改动。
// ============================================================================

// ---- Layer 1 MainMovement → MainDecisions ----

bool UNTEAnimInstance::Main_To_Fall() const     { return MainDecisions.Main_To_Fall(); }
bool UNTEAnimInstance::Main_To_Jump() const     { return MainDecisions.Main_To_Jump(); }
bool UNTEAnimInstance::Main_To_Grounded() const { return MainDecisions.Main_To_Grounded(); }
bool UNTEAnimInstance::Main_To_Gliding() const  { return MainDecisions.Main_To_Gliding(); }
bool UNTEAnimInstance::Main_To_Swimming() const { return MainDecisions.Main_To_Swimming(); }

// ---- Layer 2 MainGrounded → GroundedDecisions ----

bool UNTEAnimInstance::Grounded_Just_Landed() const { return GroundedDecisions.Grounded_Just_Landed(); }

// ---- Layer 3 LocomotionStates (Loco_*) → LocoDecisions ----

bool UNTEAnimInstance::Loco_NotMoving_To_Enter() const  { return LocoDecisions.Loco_NotMoving_To_Enter(); }
bool UNTEAnimInstance::Loco_Enter_To_Moving() const     { return LocoDecisions.Loco_Enter_To_Moving(); }
bool UNTEAnimInstance::Loco_Enter_To_NotMoving() const  { return LocoDecisions.Loco_Enter_To_NotMoving(); }
bool UNTEAnimInstance::Loco_Moving_To_LeftStop() const  { return LocoDecisions.Loco_Moving_To_LeftStop(); }
bool UNTEAnimInstance::Loco_Moving_To_RightStop() const { return LocoDecisions.Loco_Moving_To_RightStop(); }
bool UNTEAnimInstance::Loco_Stop_To_Moving() const      { return LocoDecisions.Loco_Stop_To_Moving(); }
bool UNTEAnimInstance::Loco_Stop_To_NotMoving() const   { return LocoDecisions.Loco_Stop_To_NotMoving(); }

// ---- Layer 3.5 Direction Dispatcher → DirectionDecisions ----

bool UNTEAnimInstance::Enter_Dir_Forward() const         { return DirectionDecisions.Enter_Dir_Forward(); }
bool UNTEAnimInstance::Enter_Dir_Left() const            { return DirectionDecisions.Enter_Dir_Left(); }
bool UNTEAnimInstance::Enter_Dir_Right() const           { return DirectionDecisions.Enter_Dir_Right(); }
bool UNTEAnimInstance::Enter_Dir_Forward_Variant() const { return DirectionDecisions.Enter_Dir_Forward_Variant(); }
bool UNTEAnimInstance::Enter_Dir_Back() const            { return DirectionDecisions.Enter_Dir_Back(); }
bool UNTEAnimInstance::Enter_Exit_Check() const          { return DirectionDecisions.Enter_Exit_Check(); }

bool UNTEAnimInstance::EnterForward_Is_Sprint() const    { return DirectionDecisions.EnterForward_Is_Sprint(); }
bool UNTEAnimInstance::EnterForward_Is_Run() const       { return DirectionDecisions.EnterForward_Is_Run(); }
bool UNTEAnimInstance::EnterForwardVar_Is_Sprint() const { return DirectionDecisions.EnterForwardVar_Is_Sprint(); }
bool UNTEAnimInstance::EnterForwardVar_Is_Run() const    { return DirectionDecisions.EnterForwardVar_Is_Run(); }

bool UNTEAnimInstance::EnterBack_L_To_R() const { return DirectionDecisions.EnterBack_L_To_R(); }
bool UNTEAnimInstance::EnterBack_R_To_L() const { return DirectionDecisions.EnterBack_R_To_L(); }

bool UNTEAnimInstance::BackLeft_Start_To_B() const       { return DirectionDecisions.BackLeft_Start_To_B(); }
bool UNTEAnimInstance::BackLeft_Start_To_B_Auto() const  { return DirectionDecisions.BackLeft_Start_To_B_Auto(); }
bool UNTEAnimInstance::BackLeft_B_To_Exit() const        { return DirectionDecisions.BackLeft_B_To_Exit(); }
bool UNTEAnimInstance::BackRight_Start_To_B() const      { return DirectionDecisions.BackRight_Start_To_B(); }
bool UNTEAnimInstance::BackRight_Start_To_B_Auto() const { return DirectionDecisions.BackRight_Start_To_B_Auto(); }
bool UNTEAnimInstance::BackRight_B_To_Exit() const       { return DirectionDecisions.BackRight_B_To_Exit(); }

// ---- Layer 4.1 + 4.2 Detail → DetailDecisions ----

bool UNTEAnimInstance::Detail_EnterRun_To_Run() const   { return DetailDecisions.Detail_EnterRun_To_Run(); }
bool UNTEAnimInstance::Detail_Walk_To_Run() const       { return DetailDecisions.Detail_Walk_To_Run(); }
bool UNTEAnimInstance::Detail_Walk_To_WalkToRun() const { return DetailDecisions.Detail_Walk_To_WalkToRun(); }
bool UNTEAnimInstance::Detail_Run_To_TurnBack() const   { return DetailDecisions.Detail_Run_To_TurnBack(); }
bool UNTEAnimInstance::Detail_WalkToRun_To_Run() const  { return DetailDecisions.Detail_WalkToRun_To_Run(); }
bool UNTEAnimInstance::Detail_TurnBack_To_Run() const   { return DetailDecisions.Detail_TurnBack_To_Run(); }
bool UNTEAnimInstance::Detail_Run_To_Walk() const       { return DetailDecisions.Detail_Run_To_Walk(); }
bool UNTEAnimInstance::Detail_TurnBack_IsLeft() const   { return DetailDecisions.Detail_TurnBack_IsLeft(); }
bool UNTEAnimInstance::Detail_TurnBack_IsRight() const  { return DetailDecisions.Detail_TurnBack_IsRight(); }
bool UNTEAnimInstance::Gait_To_Sprint() const           { return DetailDecisions.Gait_To_Sprint(); }
bool UNTEAnimInstance::Gait_Exit_Sprint() const         { return DetailDecisions.Gait_Exit_Sprint(); }

// ---- Layer 4.3 StopGait → StopGaitDecisions ----

bool UNTEAnimInstance::StopGait_Is_Sprint() const         { return StopGaitDecisions.StopGait_Is_Sprint(); }
bool UNTEAnimInstance::StopGait_Is_Run() const            { return StopGaitDecisions.StopGait_Is_Run(); }
bool UNTEAnimInstance::StopGait_Is_Walk() const           { return StopGaitDecisions.StopGait_Is_Walk(); }
bool UNTEAnimInstance::StopGait_ForceRun_To_State() const { return StopGaitDecisions.StopGait_ForceRun_To_State(); }

// ---- Layer 5 LocomotionCycles → CyclesDecisions ----

bool UNTEAnimInstance::Cycles_To_Left() const            { return CyclesDecisions.Cycles_To_Left(); }
bool UNTEAnimInstance::Cycles_To_Right() const           { return CyclesDecisions.Cycles_To_Right(); }
bool UNTEAnimInstance::Cycles_To_RunStopRotation() const { return CyclesDecisions.Cycles_To_RunStopRotation(); }
bool UNTEAnimInstance::Cycles_To_StopRotation() const    { return CyclesDecisions.Cycles_To_StopRotation(); }
bool UNTEAnimInstance::Cycles_To_WalkStopRotation() const{ return CyclesDecisions.Cycles_To_WalkStopRotation(); }
bool UNTEAnimInstance::Cycles_StopRotation_Done() const  { return CyclesDecisions.Cycles_StopRotation_Done(); }
bool UNTEAnimInstance::Cycles_Conduit_IsLeft() const     { return CyclesDecisions.Cycles_Conduit_IsLeft(); }
bool UNTEAnimInstance::Cycles_Conduit_IsRight() const    { return CyclesDecisions.Cycles_Conduit_IsRight(); }

// ============================================================================
// State Callbacks
// ============================================================================

void UNTEAnimInstance::OnEnter_NotMoving()
{
	UE_LOG(LogNTEAnim, Verbose, TEXT("OnEnter: NotMoving"));
}

void UNTEAnimInstance::OnEnter_Moving()
{
	UE_LOG(LogNTEAnim, Verbose, TEXT("OnEnter: Moving"));
}

void UNTEAnimInstance::OnEnter_LeftStop()
{
	UE_LOG(LogNTEAnim, Verbose, TEXT("OnEnter: LeftStop"));
}

void UNTEAnimInstance::OnEnter_RightStop()
{
	UE_LOG(LogNTEAnim, Verbose, TEXT("OnEnter: RightStop"));
}

void UNTEAnimInstance::OnEnter_EnterMoveState()
{
	UE_LOG(LogNTEAnim, Verbose, TEXT("OnEnter: EnterMoveState"));
}

void UNTEAnimInstance::OnEnterState(FName InStateName)
{
	CurrentAnimStateName = InStateName;
	UE_LOG(LogNTEAnim, Verbose, TEXT("OnEnterState: %s"), *InStateName.ToString());
}

// ============================================================================
// Layer 3 LocomotionStatesMachine Shell 委托 (#662–#704) → LocoDecisions
// ============================================================================

bool UNTEAnimInstance::Loco3_Conduit_To_Moving_Sprint() const { return LocoDecisions.Loco3_Conduit_To_Moving_Sprint(); }
bool UNTEAnimInstance::Loco3_Conduit_To_Moving() const        { return LocoDecisions.Loco3_Conduit_To_Moving(); }
bool UNTEAnimInstance::Loco3_Conduit_To_NotMoving() const     { return LocoDecisions.Loco3_Conduit_To_NotMoving(); }

bool UNTEAnimInstance::Loco3_NotMoving_To_Moving_Auto() const { return LocoDecisions.Loco3_NotMoving_To_Moving_Auto(); }
bool UNTEAnimInstance::Loco3_NotMoving_To_Conduit5() const    { return LocoDecisions.Loco3_NotMoving_To_Conduit5(); }
bool UNTEAnimInstance::Loco3_NotMoving_To_Moving_Alt() const  { return LocoDecisions.Loco3_NotMoving_To_Moving_Alt(); }
bool UNTEAnimInstance::Loco3_NotMoving_To_Stop_MM() const     { return LocoDecisions.Loco3_NotMoving_To_Stop_MM(); }

bool UNTEAnimInstance::Loco3_Moving_To_NotMoving_Auto() const { return LocoDecisions.Loco3_Moving_To_NotMoving_Auto(); }
bool UNTEAnimInstance::Loco3_Moving_To_NotMoving() const      { return LocoDecisions.Loco3_Moving_To_NotMoving(); }
bool UNTEAnimInstance::Loco3_Moving_To_Conduit1() const       { return LocoDecisions.Loco3_Moving_To_Conduit1(); }

bool UNTEAnimInstance::Loco3_Stop_To_NotMoving_Auto1() const { return LocoDecisions.Loco3_Stop_To_NotMoving_Auto1(); }
bool UNTEAnimInstance::Loco3_Stop_To_NotMoving_Auto2() const { return LocoDecisions.Loco3_Stop_To_NotMoving_Auto2(); }
bool UNTEAnimInstance::Loco3_Stop_To_Conduit4() const        { return LocoDecisions.Loco3_Stop_To_Conduit4(); }

bool UNTEAnimInstance::Loco3_NotMoving1_To_Moving1_Patrol() const { return LocoDecisions.Loco3_NotMoving1_To_Moving1_Patrol(); }
bool UNTEAnimInstance::Loco3_NotMoving1_To_LeftStop1() const      { return LocoDecisions.Loco3_NotMoving1_To_LeftStop1(); }
bool UNTEAnimInstance::Loco3_NotMoving1_To_RightStop1() const     { return LocoDecisions.Loco3_NotMoving1_To_RightStop1(); }
bool UNTEAnimInstance::Loco3_NotMoving1_To_Moving1_Should() const { return LocoDecisions.Loco3_NotMoving1_To_Moving1_Should(); }
bool UNTEAnimInstance::Loco3_Moving1_To_NotMoving1() const        { return LocoDecisions.Loco3_Moving1_To_NotMoving1(); }
bool UNTEAnimInstance::Loco3_Moving1_To_CanStop1() const          { return LocoDecisions.Loco3_Moving1_To_CanStop1(); }
bool UNTEAnimInstance::Loco3_AutoRule_Fallback() const           { return LocoDecisions.Loco3_AutoRule_Fallback(); }

bool UNTEAnimInstance::Loco3_Conduit2_To_Moving1_Sprint() const { return LocoDecisions.Loco3_Conduit2_To_Moving1_Sprint(); }
bool UNTEAnimInstance::Loco3_Conduit2_To_Moving1() const        { return LocoDecisions.Loco3_Conduit2_To_Moving1(); }
bool UNTEAnimInstance::Loco3_Conduit2_To_NotMoving1() const     { return LocoDecisions.Loco3_Conduit2_To_NotMoving1(); }

bool UNTEAnimInstance::Loco3_LeftStop1_To_NotMoving1_Auto() const  { return LocoDecisions.Loco3_LeftStop1_To_NotMoving1_Auto(); }
bool UNTEAnimInstance::Loco3_Stop1_To_Moving1_Resume() const       { return LocoDecisions.Loco3_Stop1_To_Moving1_Resume(); }
bool UNTEAnimInstance::Loco3_RightStop1_To_NotMoving1_Auto() const { return LocoDecisions.Loco3_RightStop1_To_NotMoving1_Auto(); }

bool UNTEAnimInstance::Loco3_CanStop1_To_Conduit1_1_1() const { return LocoDecisions.Loco3_CanStop1_To_Conduit1_1_1(); }
bool UNTEAnimInstance::Loco3_CanStop1_To_Conduit1_2() const   { return LocoDecisions.Loco3_CanStop1_To_Conduit1_2(); }

bool UNTEAnimInstance::Loco3_StopConduit_NoSprint() const { return LocoDecisions.Loco3_StopConduit_NoSprint(); }
bool UNTEAnimInstance::Loco3_StopConduit_Sprint() const   { return LocoDecisions.Loco3_StopConduit_Sprint(); }

bool UNTEAnimInstance::Loco3_Conduit1_To_Stop_A() const { return LocoDecisions.Loco3_Conduit1_To_Stop_A(); }
bool UNTEAnimInstance::Loco3_Conduit1_To_Stop_B() const { return LocoDecisions.Loco3_Conduit1_To_Stop_B(); }

bool UNTEAnimInstance::Loco3_EnterMove_To_Moving() const   { return LocoDecisions.Loco3_EnterMove_To_Moving(); }
bool UNTEAnimInstance::Loco3_EnterMove_To_Conduit1() const { return LocoDecisions.Loco3_EnterMove_To_Conduit1(); }

bool UNTEAnimInstance::Loco3_Conduit3_To_Moving() const     { return LocoDecisions.Loco3_Conduit3_To_Moving(); }
bool UNTEAnimInstance::Loco3_Conduit3_To_EnterState() const { return LocoDecisions.Loco3_Conduit3_To_EnterState(); }

bool UNTEAnimInstance::Loco3_EnterState_To_EnterMove() const { return LocoDecisions.Loco3_EnterState_To_EnterMove(); }

bool UNTEAnimInstance::Loco3_Conduit5_To_Conduit3() const   { return LocoDecisions.Loco3_Conduit5_To_Conduit3(); }
bool UNTEAnimInstance::Loco3_Conduit5_To_Moving_Walk() const{ return LocoDecisions.Loco3_Conduit5_To_Moving_Walk(); }
bool UNTEAnimInstance::Loco3_Conduit5_To_EnterWalk() const  { return LocoDecisions.Loco3_Conduit5_To_EnterWalk(); }

bool UNTEAnimInstance::Loco3_EnterWalk_To_Moving() const { return LocoDecisions.Loco3_EnterWalk_To_Moving(); }

bool UNTEAnimInstance::Loco3_Conduit4_To_EnterState() const { return LocoDecisions.Loco3_Conduit4_To_EnterState(); }
bool UNTEAnimInstance::Loco3_Conduit4_To_Moving() const     { return LocoDecisions.Loco3_Conduit4_To_Moving(); }

// ============================================================================
// Layer 5 MoveR / MoveL Sprint transitions → CyclesDecisions
// ============================================================================

bool UNTEAnimInstance::MoveR_Conduit_To_RunWalk() const          { return CyclesDecisions.MoveR_Conduit_To_RunWalk(); }
bool UNTEAnimInstance::MoveR_Conduit_To_SprintToRunWalk() const  { return CyclesDecisions.MoveR_Conduit_To_SprintToRunWalk(); }
bool UNTEAnimInstance::MoveR_RunWalk_To_Sprint() const           { return CyclesDecisions.MoveR_RunWalk_To_Sprint(); }
bool UNTEAnimInstance::MoveR_Sprint_To_RunWalk() const           { return CyclesDecisions.MoveR_Sprint_To_RunWalk(); }
bool UNTEAnimInstance::MoveR_SprintToRunWalk_To_RunWalk() const  { return CyclesDecisions.MoveR_SprintToRunWalk_To_RunWalk(); }

bool UNTEAnimInstance::MoveL_Conduit_To_RunWalk() const          { return CyclesDecisions.MoveL_Conduit_To_RunWalk(); }
bool UNTEAnimInstance::MoveL_Conduit_To_SprintToRunWalk() const  { return CyclesDecisions.MoveL_Conduit_To_SprintToRunWalk(); }
bool UNTEAnimInstance::MoveL_RunWalk_To_Sprint() const           { return CyclesDecisions.MoveL_RunWalk_To_Sprint(); }
bool UNTEAnimInstance::MoveL_Sprint_To_RunWalk() const           { return CyclesDecisions.MoveL_Sprint_To_RunWalk(); }
bool UNTEAnimInstance::MoveL_SprintToRunWalk_To_RunWalk() const  { return CyclesDecisions.MoveL_SprintToRunWalk_To_RunWalk(); }

// ============================================================================
// Layer 1 MainMovement 顶层过渡补全 Shell 委托 (#345–#378) → MainDecisions
// ============================================================================

// ---- Grounded 子机过渡（#345–#349）----
bool UNTEAnimInstance::MainMove_Grounded_To_MovementState() const { return MainDecisions.MainMove_Grounded_To_MovementState(); }
bool UNTEAnimInstance::MainMove_Grounded_To_JumpTakeOff() const   { return MainDecisions.MainMove_Grounded_To_JumpTakeOff(); }
bool UNTEAnimInstance::MainMove_Grounded_To_Jump() const          { return MainDecisions.MainMove_Grounded_To_Jump(); }
bool UNTEAnimInstance::MainMove_Grounded_To_Jump_Instant() const  { return MainDecisions.MainMove_Grounded_To_Jump_Instant(); }

// ---- Fall 子机过渡（#350–#352）----
bool UNTEAnimInstance::MainMove_Fall_To_InAir() const { return MainDecisions.MainMove_Fall_To_InAir(); }
bool UNTEAnimInstance::MainMove_Fall_To_Land() const  { return MainDecisions.MainMove_Fall_To_Land(); }
bool UNTEAnimInstance::MainMove_Fall_To_Jump() const  { return MainDecisions.MainMove_Fall_To_Jump(); }

// ---- Jump 子机过渡（#353–#354）----
bool UNTEAnimInstance::MainMove_Jump_To_InAir() const { return MainDecisions.MainMove_Jump_To_InAir(); }
bool UNTEAnimInstance::MainMove_Jump_To_Land() const  { return MainDecisions.MainMove_Jump_To_Land(); }

// ---- MovementState (<-MS->) 分派（#355–#360）----
bool UNTEAnimInstance::MainMove_MS_To_InAir() const     { return MainDecisions.MainMove_MS_To_InAir(); }
bool UNTEAnimInstance::MainMove_MS_To_Vines() const     { return MainDecisions.MainMove_MS_To_Vines(); }
bool UNTEAnimInstance::MainMove_MS_To_VaultToAir() const{ return MainDecisions.MainMove_MS_To_VaultToAir(); }
bool UNTEAnimInstance::MainMove_MS_To_Swimming() const  { return MainDecisions.MainMove_MS_To_Swimming(); }
bool UNTEAnimInstance::MainMove_MS_To_Driving() const   { return MainDecisions.MainMove_MS_To_Driving(); }
bool UNTEAnimInstance::MainMove_MS_To_Grounded() const  { return MainDecisions.MainMove_MS_To_Grounded(); }

// ---- InAir 导管（#361–#363）----
bool UNTEAnimInstance::MainMove_InAir_To_Jump() const { return MainDecisions.MainMove_InAir_To_Jump(); }
bool UNTEAnimInstance::MainMove_InAir_To_MS() const   { return MainDecisions.MainMove_InAir_To_MS(); }
bool UNTEAnimInstance::MainMove_InAir_To_Fall() const { return MainDecisions.MainMove_InAir_To_Fall(); }

// ---- Land 导管（#364）----
bool UNTEAnimInstance::MainMove_Land_To_Grounded() const { return MainDecisions.MainMove_Land_To_Grounded(); }

// ---- TakeOff 子机（#365–#366）----
bool UNTEAnimInstance::MainMove_TakeOff_To_Jump() const { return MainDecisions.MainMove_TakeOff_To_Jump(); }
bool UNTEAnimInstance::MainMove_TakeOff_To_MS() const   { return MainDecisions.MainMove_TakeOff_To_MS(); }

// ---- Vines 子机（#367–#368）----
bool UNTEAnimInstance::MainMove_Vines_To_MS() const      { return MainDecisions.MainMove_Vines_To_MS(); }
bool UNTEAnimInstance::MainMove_Vines_To_MS_Auto() const { return MainDecisions.MainMove_Vines_To_MS_Auto(); }

// ---- Vault 子机（#369–#370）----
bool UNTEAnimInstance::MainMove_Vault_To_MS() const      { return MainDecisions.MainMove_Vault_To_MS(); }
bool UNTEAnimInstance::MainMove_Vault_To_MS_Auto() const { return MainDecisions.MainMove_Vault_To_MS_Auto(); }

// ---- Gliding / GlidingToFall（#371–#375）----
bool UNTEAnimInstance::MainMove_Gliding_To_GlidingToFall() const    { return MainDecisions.MainMove_Gliding_To_GlidingToFall(); }
bool UNTEAnimInstance::MainMove_Gliding_To_OutGliding() const       { return MainDecisions.MainMove_Gliding_To_OutGliding(); }
bool UNTEAnimInstance::MainMove_GlidingToFall_To_Fall() const       { return MainDecisions.MainMove_GlidingToFall_To_Fall(); }
bool UNTEAnimInstance::MainMove_GlidingToFall_To_Gliding() const    { return MainDecisions.MainMove_GlidingToFall_To_Gliding(); }
bool UNTEAnimInstance::MainMove_GlidingToFall_To_OutGliding() const { return MainDecisions.MainMove_GlidingToFall_To_OutGliding(); }

// ---- Swimming / Driving（#376–#377）----
bool UNTEAnimInstance::MainMove_Swimming_To_MS() const { return MainDecisions.MainMove_Swimming_To_MS(); }
bool UNTEAnimInstance::MainMove_Driving_To_MS() const  { return MainDecisions.MainMove_Driving_To_MS(); }

// ---- OutGliding 导管（#378）----
bool UNTEAnimInstance::MainMove_OutGliding_To_MS() const { return MainDecisions.MainMove_OutGliding_To_MS(); }

// ============================================================================
// Layer 2 MainGrounded 顶层过渡补全 Shell 委托 (#468–#500) → GroundedDecisions
// ============================================================================

// ---- Entry 分派（#468–#477）----
bool UNTEAnimInstance::MG_Entry_To_VaultContinue() const    { return GroundedDecisions.MG_Entry_To_VaultContinue(); }
bool UNTEAnimInstance::MG_Entry_To_Conduit() const          { return GroundedDecisions.MG_Entry_To_Conduit(); }
bool UNTEAnimInstance::MG_Entry_To_FromRoll() const         { return GroundedDecisions.MG_Entry_To_FromRoll(); }
bool UNTEAnimInstance::MG_Entry_To_LandedMobile() const     { return GroundedDecisions.MG_Entry_To_LandedMobile(); }
bool UNTEAnimInstance::MG_Entry_To_Landed() const           { return GroundedDecisions.MG_Entry_To_Landed(); }
bool UNTEAnimInstance::MG_Entry_To_MainGS() const           { return GroundedDecisions.MG_Entry_To_MainGS(); }
bool UNTEAnimInstance::MG_Entry_To_VinesOver() const        { return GroundedDecisions.MG_Entry_To_VinesOver(); }
bool UNTEAnimInstance::MG_Entry_To_RunOnWallsOver() const   { return GroundedDecisions.MG_Entry_To_RunOnWallsOver(); }
bool UNTEAnimInstance::MG_Entry_To_LandedStationary() const { return GroundedDecisions.MG_Entry_To_LandedStationary(); }
bool UNTEAnimInstance::MG_Entry_To_SprintVinesOver() const  { return GroundedDecisions.MG_Entry_To_SprintVinesOver(); }

// ---- FromRoll（#478–#479）----
bool UNTEAnimInstance::MG_FromRoll_To_MainGS() const    { return GroundedDecisions.MG_FromRoll_To_MainGS(); }
bool UNTEAnimInstance::MG_FromRoll_To_RollToRun() const { return GroundedDecisions.MG_FromRoll_To_RollToRun(); }

// ---- LandedStationary（#480–#483）----
bool UNTEAnimInstance::MG_LandedStat_To_MainGS_Auto() const { return GroundedDecisions.MG_LandedStat_To_MainGS_Auto(); }
bool UNTEAnimInstance::MG_LandedStat_To_StatToMove() const  { return GroundedDecisions.MG_LandedStat_To_StatToMove(); }
bool UNTEAnimInstance::MG_LandedStat_To_LandedMob() const   { return GroundedDecisions.MG_LandedStat_To_LandedMob(); }
bool UNTEAnimInstance::MG_LandedStat_To_MainGS() const      { return GroundedDecisions.MG_LandedStat_To_MainGS(); }

// ---- LandedMobile（#484–#485）----
bool UNTEAnimInstance::MG_LandedMob_To_MainGS_Auto() const { return GroundedDecisions.MG_LandedMob_To_MainGS_Auto(); }
bool UNTEAnimInstance::MG_LandedMob_To_MainGS() const      { return GroundedDecisions.MG_LandedMob_To_MainGS(); }

// ---- StatToMove（#486–#487）----
bool UNTEAnimInstance::MG_StatToMove_To_MainGS_Auto() const { return GroundedDecisions.MG_StatToMove_To_MainGS_Auto(); }
bool UNTEAnimInstance::MG_StatToMove_To_MainGS() const      { return GroundedDecisions.MG_StatToMove_To_MainGS(); }

// ---- RollToRun（#488–#489）----
bool UNTEAnimInstance::MG_RollToRun_To_MainGS_Auto() const { return GroundedDecisions.MG_RollToRun_To_MainGS_Auto(); }
bool UNTEAnimInstance::MG_RollToRun_To_MainGS() const      { return GroundedDecisions.MG_RollToRun_To_MainGS(); }

// ---- Landed（#490–#491）----
bool UNTEAnimInstance::MG_Landed_To_LandedStat() const { return GroundedDecisions.MG_Landed_To_LandedStat(); }
bool UNTEAnimInstance::MG_Landed_To_StatToMove() const { return GroundedDecisions.MG_Landed_To_StatToMove(); }

// ---- VaultContinue（#492）----
bool UNTEAnimInstance::MG_VaultCont_To_MainGS() const { return GroundedDecisions.MG_VaultCont_To_MainGS(); }

// ---- VinesOver（#493–#494）----
bool UNTEAnimInstance::MG_VinesOver_To_MainGS_Auto() const { return GroundedDecisions.MG_VinesOver_To_MainGS_Auto(); }
bool UNTEAnimInstance::MG_VinesOver_To_MainGS() const      { return GroundedDecisions.MG_VinesOver_To_MainGS(); }

// ---- RunOnWallsOver（#495–#496）----
bool UNTEAnimInstance::MG_RunWalls_To_MainGS_Auto() const { return GroundedDecisions.MG_RunWalls_To_MainGS_Auto(); }
bool UNTEAnimInstance::MG_RunWalls_To_MainGS() const      { return GroundedDecisions.MG_RunWalls_To_MainGS(); }

// ---- Conduit（#497–#498）----
bool UNTEAnimInstance::MG_Conduit_To_LandedMob() const { return GroundedDecisions.MG_Conduit_To_LandedMob(); }
bool UNTEAnimInstance::MG_Conduit_To_Landed() const    { return GroundedDecisions.MG_Conduit_To_Landed(); }

// ---- SprintVinesOver（#499–#500）----
bool UNTEAnimInstance::MG_SprintVines_To_MainGS() const      { return GroundedDecisions.MG_SprintVines_To_MainGS(); }
bool UNTEAnimInstance::MG_SprintVines_To_MainGS_Auto() const { return GroundedDecisions.MG_SprintVines_To_MainGS_Auto(); }

// ============================================================================
// 顶层辅助 MotionMatch Shell 委托 (#44–#45) → MotionMatchDecisions
// ============================================================================

bool UNTEAnimInstance::MM_To_Base_Instant() const { return MotionMatchDecisions.MM_To_Base_Instant(); }
bool UNTEAnimInstance::MM_To_Base_Smooth() const  { return MotionMatchDecisions.MM_To_Base_Smooth(); }

// ============================================================================
// 顶层辅助 FullBodyIK Shell 委托 (#64–#65) → FullBodyIKDecisions
// ============================================================================

bool UNTEAnimInstance::FullBodyIK_Activate() const   { return FullBodyIKDecisions.FullBodyIK_Activate(); }
bool UNTEAnimInstance::FullBodyIK_Deactivate() const { return FullBodyIKDecisions.FullBodyIK_Deactivate(); }
