/**
 * @file ZZZLocomotionEvents.cpp
 * @brief ZZZ 动画 Locomotion 状态事件模块实现
 */

#include "Animation/zzzAnim/Locomotion/ZZZLocomotionEvents.h"
#include "Animation/zzzAnim/Data/ZZZAnimSnapshot.h"
#include "Character/Data/GGYGOMovementTypes.h"
#include "Animation/zzzAnim/Locomotion/ZZZLocomotionRules.h"
#include "Animation/zzzAnim/ZZZAnimLog.h"

void FZZZLocomotionEvents::SetContext(const FZZZAnimWriteContext& InContext)
{
	Context = InContext;
}

void FZZZLocomotionEvents::SynchronizeMovingSubState()
{
	if (!Context.Snap || !Context.Memory)
	{
		return;
	}

	if (!Context.Snap->bShouldMove)
	{
		Context.Memory->MovingSubState = EZZZAnimMovingSubState::None;
		return;
	}

	// MovingSubState 只是移动层转身相位的投影，供 AnimBP 读取；反向输入检测与相位切换的
	// 唯一真相在 `UGGYGOCharacterMovementComponent::UpdateTurnBack`，这里不重复判定。
	//
	// 投影口径与 `FZZZLocomotionDecisions::WalkRun_To_TurnBack` 保持一致：只有曲线接管段
	// 算「转身中」，RunOut 段算 WalkRun。两处口径必须相同，否则子状态会与 AnimBP
	// 实际所处的状态对不上，读它做表现分支就会错。
	const EGGYGOTurnBackPhase Phase = Context.Snap->TurnBackPhase;
	const bool bCurveDriven = (Phase == EGGYGOTurnBackPhase::Turning)
		|| (Phase == EGGYGOTurnBackPhase::Braking);

	Context.Memory->MovingSubState = bCurveDriven
		? EZZZAnimMovingSubState::TurnBack
		: EZZZAnimMovingSubState::WalkRun;
}

void FZZZLocomotionEvents::AdvanceStopSelection(float DeltaSeconds)
{
	const bool bIsMoving =
		Context.Snap->bShouldMove;

	// 早停窗口只由逻辑状态首次进入 Moving 且入口步态为 Walk/None 时启动；
	// 直接 Run 入口不启动，避免把直接跑步误判为 EnterMove。
	if (bIsMoving && !bWasMoving)
	{
		const bool bWalkStartEntry =
			Context.Snap->Gait == EGGYGOGait::Walk
			|| Context.Snap->Gait == EGGYGOGait::None;
		bEnterMoveWindowActive = bWalkStartEntry;
		EnterMoveElapsedSeconds = 0.0f;
	}

	// 计时只接受有效的非负 DeltaSeconds，并沿用 GaitBlendY 的单帧上限。
	// 负值或非有限值不推进窗口计时；窗口状态仍可在本帧保持 StopValue=0。
	if (bEnterMoveWindowActive)
	{
		if (!FMath::IsFinite(EnterMoveElapsedSeconds) || EnterMoveElapsedSeconds < 0.0f)
		{
			EnterMoveElapsedSeconds = 0.0f;
		}

		if (FMath::IsFinite(DeltaSeconds) && DeltaSeconds >= 0.0f)
		{
			const float ClampedDelta = FMath::Clamp(
				DeltaSeconds, 0.0f, ZZZLocomotionRules::MaxClampedDelta);
			EnterMoveElapsedSeconds = FMath::Min(
				EnterMoveElapsedSeconds + ClampedDelta,
				ZZZLocomotionRules::EnterMoveStopWindowSeconds);

			if (EnterMoveElapsedSeconds >= ZZZLocomotionRules::EnterMoveStopWindowSeconds)
			{
				bEnterMoveWindowActive = false;
				EnterMoveElapsedSeconds = 0.0f;
			}
		}
	}

	// StopValue 是 Stop Select 的独立动画分支索引，不属于移动步态；合法范围为 0~2，非法存储值规范为 0。
	if (Context.Memory->StopValue < 0 || Context.Memory->StopValue > 2)
	{
		Context.Memory->StopValue = 0;
	}

	// 非 Moving 时保留最近一次合法 StopValue；若早停窗口仍有效则保持窗口优先值 0，
	// 随后重置窗口与相关计时状态，确保下一次移动从全新的窗口起算。
	if (!bIsMoving)
	{
		if (bEnterMoveWindowActive)
		{
			Context.Memory->StopValue = 0;
		}

		bEnterMoveWindowActive = false;
		EnterMoveElapsedSeconds = 0.0f;
		bWasMoving = false;
		return;
	}

	// 只有有效 Moving 步态更新 StopValue；None 或非法值保留已经规范化的最近一次值。
	switch (Context.Snap->Gait)
	{
	case EGGYGOGait::Walk:
		Context.Memory->StopValue = 1;
		break;
	case EGGYGOGait::Run:
		Context.Memory->StopValue = 2;
		break;
	case EGGYGOGait::None:
	default:
		break;
	}

	// 起步早停窗口优先级高于普通 Walk/Run StopValue 写入，避免同帧被覆盖为 1/2。
	if (bEnterMoveWindowActive)
	{
		Context.Memory->StopValue = 0;
	}

	bWasMoving = bIsMoving;
}

void FZZZLocomotionEvents::AdvanceGaitBlend(float DeltaSeconds)
{
	// 上下文不可用时不执行任何写入；Tuning 为空由配置解析辅助函数使用默认值。
	if (!Context.Snap || !Context.Memory)
	{
		return;
	}

	// Stop helper 必须先执行，随后才按原有顺序维护 GaitValue 并推进 GaitBlendY。
	AdvanceStopSelection(DeltaSeconds);

	const bool bIsMoving =
		Context.Snap->bShouldMove;

	// GaitValue 只保存最近一次有效 Moving 步态的离散选择值。
	// 外部若写入非法值，先规范为 None，确保后续不会把越域值带入步态选择。
	if (Context.Memory->GaitValue < 0 || Context.Memory->GaitValue > 2)
	{
		Context.Memory->GaitValue = 0;
	}

	// 非 Moving 时只复位 GaitBlendY 与目标值；GaitValue 和 StopValue 的既有记录保留。
	if (!bIsMoving)
	{
		Context.Memory->GaitBlendY = 0.0f;
		LastGaitBlendTarget = 0.0f;
		Context.Memory->MovingSubState = EZZZAnimMovingSubState::None;
		bInvalidDeltaReported = false;
		bInvalidSnapshotGaitReported = false;
		return;
	}

	// 只有有效 Moving 步态更新离散选择值：不改写 const Snapshot_Gait；None 或非法值
	// 保留已经规范化的最近一次有效值。
	switch (Context.Snap->Gait)
	{
	case EGGYGOGait::Walk:
		Context.Memory->GaitValue = 1;
		break;
	case EGGYGOGait::Run:
		Context.Memory->GaitValue = 2;
		break;
	case EGGYGOGait::None:
	default:
		break;
	}

	// 只诊断非法源值，不改写 const Snapshot_Gait。
	ReportInvalidSnapshotGaitIfNeeded();

	const float Target = ResolveTargetFromSnapshot();
	LastGaitBlendTarget = Target;

	if (!FMath::IsFinite(DeltaSeconds))
	{
		if (!bInvalidDeltaReported)
		{
			UE_LOG(LogZZZAnim, Warning,
				TEXT("Invalid gait blend DeltaSeconds (non-finite: %f); GaitBlendY was set to target."),
				DeltaSeconds);
			bInvalidDeltaReported = true;
		}

		Context.Memory->GaitBlendY = Target;
		return;
	}

	if (DeltaSeconds < 0.0f)
	{
		if (!bInvalidDeltaReported)
		{
			UE_LOG(LogZZZAnim, Warning,
				TEXT("Invalid gait blend DeltaSeconds (negative: %f); GaitBlendY was not changed."),
				DeltaSeconds);
			bInvalidDeltaReported = true;
		}

		// 负 Delta 不能改变 GaitBlendY。
		return;
	}

	bInvalidDeltaReported = false;
	const float ClampedDelta = FMath::Clamp(
		DeltaSeconds, 0.0f, ZZZLocomotionRules::MaxClampedDelta);
	AdvanceGaitBlendY(Target, ClampedDelta);
}

float FZZZLocomotionEvents::ResolveTargetFromSnapshot() const
{
	if (Context.Snap->Gait == EGGYGOGait::None)
	{
		return LastGaitBlendTarget;
	}

	return ZZZLocomotionRules::ResolveGaitBlendTarget(Context.Snap->Gait);
}

void FZZZLocomotionEvents::ReportInvalidSnapshotGaitIfNeeded()
{
	const EGGYGOGait SnapshotGait = Context.Snap->Gait;
	const bool bIsValid = SnapshotGait == EGGYGOGait::None
		|| SnapshotGait == EGGYGOGait::Walk
		|| SnapshotGait == EGGYGOGait::Run;

	if (bIsValid)
	{
		bInvalidSnapshotGaitReported = false;
		return;
	}

	if (!bInvalidSnapshotGaitReported)
	{
		UE_LOG(LogZZZAnim, Warning,
			TEXT("Invalid Snapshot_Gait value %d; treating it as Walk for blend targeting."),
			static_cast<uint8>(SnapshotGait));
		bInvalidSnapshotGaitReported = true;
	}
}

void FZZZLocomotionEvents::AdvanceGaitBlendY(float Target, float ClampedDelta)
{
	if (!Context.Memory)
	{
		return;
	}

	const float ClampedTarget = FMath::Clamp(Target, 0.0f, 1.0f);
	const float Current = FMath::IsFinite(Context.Memory->GaitBlendY)
		? FMath::Clamp(Context.Memory->GaitBlendY, 0.0f, 1.0f)
		: 0.0f;
	const float Speed = ZZZLocomotionRules::ResolveGaitBlendInterpSpeed(Context.Tuning);

	// 速率小于等于 0 或非有限时直接吸附到目标值。
	if (!FMath::IsFinite(Speed) || Speed <= 0.0f)
	{
		Context.Memory->GaitBlendY = ClampedTarget;
		return;
	}

	if (FMath::Abs(ClampedTarget - Current) <= ZZZLocomotionRules::GaitBlendSnapTolerance)
	{
		Context.Memory->GaitBlendY = ClampedTarget;
		return;
	}

	const float Interpolated = FMath::FInterpConstantTo(
		Current, ClampedTarget, ClampedDelta, Speed);
	const float ClampedResult = FMath::IsFinite(Interpolated)
		? FMath::Clamp(Interpolated, 0.0f, 1.0f)
		: ClampedTarget;

	// FInterpConstantTo 不应越过目标；再次按目标方向钳制，防止异常输入造成越界。
	const float NonOvershootingResult = ClampedTarget >= Current
		? FMath::Min(ClampedResult, ClampedTarget)
		: FMath::Max(ClampedResult, ClampedTarget);
	Context.Memory->GaitBlendY = FMath::Abs(ClampedTarget - NonOvershootingResult)
		<= ZZZLocomotionRules::GaitBlendSnapTolerance
		? ClampedTarget
		: NonOvershootingResult;
}
