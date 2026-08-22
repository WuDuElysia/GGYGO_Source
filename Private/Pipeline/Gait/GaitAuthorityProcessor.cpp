/**
 * @file GaitAuthorityProcessor.cpp
 * @brief 逻辑侧步态决策者实现
 */

#include "Pipeline/Gait/GaitAuthorityProcessor.h"

#include "BaseCharacter.h"
#include "Data/Input/InputData.h"
#include "Data/Runtime/RuntimeData.h"
#include "Data/Config/UCharConfigData.h"
#include "Pipeline/Gait/GaitLog.h"

void FGaitAuthorityProcessor::Init(ACharacter* InOwner)
{
	Owner = Cast<ABaseCharacter>(InOwner);
}

void FGaitAuthorityProcessor::Process(const FInputData& InputData, FRuntimeData& RuntimeData, float DeltaTime)
{
	// ① 采样本帧四项判定输入与边沿。步态解析只使用 Move 的是否近零性质。
	// 当前帧存在非零移动输入；输入方向和具体幅度不参与这里的判定。
	const bool bMoveInputPresent = !InputData.CurrentFrame.Move.IsNearlyZero();
	// 当前帧是否处于移动阻挡状态；阻挡时步态必须被压制。
	const bool bBlockMove = RuntimeData.Arbiter.bBlockMove;
	// 当前逻辑状态是否为 Moving，用于判断是否允许累计 Walk 持续时间。
	const bool bMovingState = RuntimeData.State.CurrentState == ECharacterStateType::Moving;
	// 移动输入从无到有的上升沿；用于识别一次新的移动起步。
	const bool bMoveInputRising = bMoveInputPresent && !bPreviousMoveInputPresent;
	// 移动阻挡从有到无的释放沿；用于识别解除阻挡后的重新起步。
	const bool bBlockReleased = !bBlockMove && bPreviousBlockMove;
	// 逻辑状态从上一帧的 Moving 离开到当前帧非 Moving。
	const bool bLeftMovingState =
		PreviousCurrentState == ECharacterStateType::Moving && !bMovingState;

	// ② 解析并钳制本帧 DeltaTime。
	float ClampedDelta = 0.0f;
	const bool bDeltaValid = ResolveFrameDelta(DeltaTime, ClampedDelta);

	// ③ 按固定优先级解析局部步态，不提前写入 RuntimeData。
	EMovementGait FrameGait = ResolveFrameGait(RuntimeData, bMoveInputPresent, bLeftMovingState);

	// ④ 先执行计时归零，再执行推进；无效 DeltaTime 时两者都跳过。
	UpdateWalkHoldTimer(
		FrameGait,
		bMoveInputPresent,
		bBlockMove,
		bMovingState,
		bMoveInputRising,
		bBlockReleased,
		ClampedDelta,
		bDeltaValid);

	// ⑤ 读取推进后的计时，达到本帧生效阈值时即时升级。
	if (ShouldUpgradeToRun(FrameGait, bMoveInputPresent, bBlockMove))
	{
		FrameGait = EMovementGait::Run;
		WalkHoldTimer = 0.0f;
	}

	// ⑥ 本帧对两个步态通道各写入一次，保证逻辑侧与动画侧同帧一致。
	RuntimeData.Gait.ResolvedGait = FrameGait;
	RuntimeData.ZZZAnim.Gait = FrameGait;

	// ⑦ 更新下一帧边沿判定基线。
	PreviousResolvedGait = FrameGait;
	bPreviousMoveInputPresent = bMoveInputPresent;
	bPreviousBlockMove = bBlockMove;
	PreviousCurrentState = RuntimeData.State.CurrentState;
}

float FGaitAuthorityProcessor::ResolveEffectiveThreshold()
{
	if (!Owner.IsValid())
	{
		if (!bMissingOwnerReported)
		{
			UE_LOG(LogGait, Warning,
				TEXT("Gait authority cannot reach CharacterConfig; using default Walk-to-Run threshold %.1f seconds."),
				DefaultWalkToRunHoldSeconds);
			bMissingOwnerReported = true;
		}
		return DefaultWalkToRunHoldSeconds;
	}

	UCharConfigData* CharacterConfig = Owner->GetCharacterConfig();
	if (CharacterConfig == nullptr)
	{
		if (!bMissingOwnerReported)
		{
			UE_LOG(LogGait, Warning,
				TEXT("Gait authority has no CharacterConfig; using default Walk-to-Run threshold %.1f seconds."),
				DefaultWalkToRunHoldSeconds);
			bMissingOwnerReported = true;
		}
		return DefaultWalkToRunHoldSeconds;
	}

	bMissingOwnerReported = false;
	const float ConfiguredThreshold = CharacterConfig->MovementConfig.WalkToRunHoldSeconds;

	if (!FMath::IsFinite(ConfiguredThreshold) || ConfiguredThreshold <= 0.0f)
	{
		if (!bThresholdFallbackReported)
		{
			UE_LOG(LogGait, Warning,
				TEXT("Invalid WalkToRunHoldSeconds (%.3f); falling back to %.1f seconds."),
				ConfiguredThreshold,
				DefaultWalkToRunHoldSeconds);
			bThresholdFallbackReported = true;
		}
		bThresholdClampReported = false;
		return DefaultWalkToRunHoldSeconds;
	}

	if (ConfiguredThreshold > MaxWalkToRunHoldSeconds)
	{
		if (!bThresholdClampReported)
		{
			UE_LOG(LogGait, Warning,
				TEXT("WalkToRunHoldSeconds (%.3f) exceeds the upper bound; clamping to %.1f seconds."),
				ConfiguredThreshold,
				MaxWalkToRunHoldSeconds);
			bThresholdClampReported = true;
		}
		bThresholdFallbackReported = false;
		return MaxWalkToRunHoldSeconds;
	}

	bThresholdFallbackReported = false;
	bThresholdClampReported = false;
	return ConfiguredThreshold;
}

bool FGaitAuthorityProcessor::ResolveFrameDelta(float InDeltaTime, float& OutClampedDelta)
{
	if (!FMath::IsFinite(InDeltaTime) || InDeltaTime < 0.0f)
	{
		OutClampedDelta = 0.0f;
		if (!bInvalidDeltaReported)
		{
			UE_LOG(LogGait, Warning,
				TEXT("Invalid gait authority frame delta (%.6f); Walk_Hold_Timer will not be modified this frame."),
				InDeltaTime);
			bInvalidDeltaReported = true;
		}
		return false;
	}

	bInvalidDeltaReported = false;
	OutClampedDelta = FMath::Clamp(InDeltaTime, 0.0f, MaxClampedDelta);
	return true;
}

EMovementGait FGaitAuthorityProcessor::ResolveFrameGait(
	FRuntimeData& RuntimeData,
	bool bMoveInputPresent,
	bool bLeftMovingState)
{
	// 阻止移动拥有最高优先级；契约在此分支不消费，等待可移动帧处理。
	if (RuntimeData.Arbiter.bBlockMove)
	{
		return EMovementGait::None;
	}

	// 无方向输入结束本次移动，并消费掉尚未使用的闪避进 Run 契约。
	if (!bMoveInputPresent)
	{
		RuntimeData.Gait.bDodgeRunPending = false;
		return EMovementGait::None;
	}

	// 闪避契约优先于普通起步，消费动作只发生在这里。
	if (RuntimeData.Gait.bDodgeRunPending)
	{
		RuntimeData.Gait.bDodgeRunPending = false;
		return EMovementGait::Run;
	}

	// Run 只在移动状态未离开时保持，避免持续移动期间回落到 Walk。
	if (PreviousResolvedGait == EMovementGait::Run && !bLeftMovingState)
	{
		return EMovementGait::Run;
	}

	// 其余可移动情形均从 Walk 起步。
	return EMovementGait::Walk;
}

void FGaitAuthorityProcessor::UpdateWalkHoldTimer(
	EMovementGait FrameGait,
	bool bMoveInputPresent,
	bool bBlockMove,
	bool bMovingState,
	bool bMoveInputRising,
	bool bBlockReleased,
	float ClampedDelta,
	bool bDeltaValid)
{
	// Requirement 9.3 优先于全部归零条件：DeltaTime 不可信时不做任何计时写入。
	if (!bDeltaValid)
	{
		return;
	}

	if (!FMath::IsFinite(WalkHoldTimer))
	{
		WalkHoldTimer = 0.0f;
	}

	// Z1-Z6：多个条件同时成立时合并为一次归零，且不恢复此前累计量。
	const bool bShouldReset =
		!bMoveInputPresent
		|| bBlockMove
		|| !bMovingState
		|| FrameGait == EMovementGait::Run
		|| bMoveInputRising
		|| bBlockReleased;

	if (bShouldReset)
	{
		WalkHoldTimer = 0.0f;
		return;
	}

	// 仅在 Walk、持续移动且处于逻辑 Moving 状态时推进计时。
	if (FrameGait == EMovementGait::Walk)
	{
		WalkHoldTimer = FMath::Clamp(WalkHoldTimer + ClampedDelta, 0.0f, MaxWalkHoldSeconds);
	}
}

bool FGaitAuthorityProcessor::ShouldUpgradeToRun(
	EMovementGait FrameGait,
	bool bMoveInputPresent,
	bool bBlockMove)
{
	if (FrameGait != EMovementGait::Walk || !bMoveInputPresent || bBlockMove)
	{
		return false;
	}

	return WalkHoldTimer >= ResolveEffectiveThreshold();
}
