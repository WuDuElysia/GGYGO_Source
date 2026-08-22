/**
 * @file TurnBackPhaseProcessor.cpp
 * @brief TurnBack 相位处理器实现
 */
#include "Pipeline/Parameters/TurnBackPhaseProcessor.h"
#include "Pipeline/Parameters/AnimSignalParameterProcessor.h"
#include "Data/Runtime/RuntimeData.h"
#include "Animation/zzzAnim/Locomotion/ZZZLocomotionRules.h"
#include "Animation/zzzAnim/ZZZAnimLog.h"
#include "GameFramework/Character.h"

namespace
{
	/** sig_turnback 视为"已解冻"的阈值；0/1 阶梯曲线用 0.5 兜住关键帧间插值。 */
	constexpr float SignalReleaseThreshold = 0.5f;

	/** Released 阶段角色前向与输入方向的点积达到此值即视为转向完成，回到 None（约 11°）。 */
	constexpr float AlignedExitDot = 0.98f;
}

void FTurnBackPhaseProcessor::Init(ACharacter* InOwner)
{
	Owner = InOwner;
}

void FTurnBackPhaseProcessor::Process(FRuntimeData& RuntimeData, float /*DeltaTime*/)
{
	if (!Owner)
	{
		RuntimeData.Movement.TurnBack.Phase = ETurnBackPhase::None;
		RuntimeData.Movement.TurnBack.EntryDirection = FVector::ZeroVector;
		return;
	}

	// 离开 Moving 直接复位，避免相位残留到下一次移动。
	if (RuntimeData.State.CurrentState != ECharacterStateType::Moving)
	{
		if (RuntimeData.Movement.TurnBack.Phase != ETurnBackPhase::None)
		{
			RuntimeData.Movement.TurnBack.Phase = ETurnBackPhase::None;
			RuntimeData.Movement.TurnBack.EntryDirection = FVector::ZeroVector;
		}
		return;
	}

	const FVector DesiredDir = RuntimeData.Intent.DesiredWorldMoveDir.GetSafeNormal2D();
	const FVector ActorForward = Owner->GetActorForwardVector().GetSafeNormal2D();
	const float ForwardDot = (!DesiredDir.IsNearlyZero() && !ActorForward.IsNearlyZero())
		? FVector::DotProduct(ActorForward, DesiredDir)
		: 1.0f;

	const float ReverseThreshold = ZZZLocomotionRules::DefaultTurnBackReverseInputDotThreshold;
	const float TurnBackSignal = RuntimeData.GetAnimSignal(GGYGOAnimSignals::TurnBack());

	switch (RuntimeData.Movement.TurnBack.Phase)
	{
	case ETurnBackPhase::None:
	{
		// 只有 Run + 有移动输入 + 输入接近角色前向的反方向时才进入转身。
		const bool bReverseRunInput =
			RuntimeData.Gait.ResolvedGait == EMovementGait::Run
			&& RuntimeData.ZZZAnim.bShouldMove
			&& !DesiredDir.IsNearlyZero()
			&& !ActorForward.IsNearlyZero()
			&& ForwardDot <= ReverseThreshold;

		if (bReverseRunInput)
		{
			// 冻结进入转身前的实际移动方向：此刻 Actor 仍朝旧移动方向，前向即为要保持的方向。
			RuntimeData.Movement.TurnBack.EntryDirection = ActorForward;
			RuntimeData.Movement.TurnBack.Phase = ETurnBackPhase::Frozen;

			UE_LOG(LogZZZAnim, Log,
				TEXT("[TurnBack][Phase] None->Frozen ForwardDot=%.3f EntryYaw=%.2f InputYaw=%.2f"),
				ForwardDot,
				ActorForward.Rotation().Yaw,
				DesiredDir.IsNearlyZero() ? 0.0f : DesiredDir.Rotation().Yaw);
		}
		break;
	}

	case ETurnBackPhase::Frozen:
	{
		// 动画信号越过阈值 → 解冻，交给 MotionDriver 在 Released 首个 Commit 中解析方向并转向。
		if (TurnBackSignal >= SignalReleaseThreshold)
		{
			RuntimeData.Movement.TurnBack.Phase = ETurnBackPhase::Released;

			UE_LOG(LogZZZAnim, Log,
				TEXT("[TurnBack][Phase] Frozen->Released Signal=%.3f InputYaw=%.2f"),
				TurnBackSignal,
				DesiredDir.IsNearlyZero() ? 0.0f : DesiredDir.Rotation().Yaw);
		}
		break;
	}

	case ETurnBackPhase::Released:
	{
		// 无输入，或角色前向已对齐输入方向（转向完成）→ 回到普通移动。
		const bool bAligned = !DesiredDir.IsNearlyZero()
			&& !ActorForward.IsNearlyZero()
			&& ForwardDot >= AlignedExitDot;

		if (!RuntimeData.ZZZAnim.bShouldMove || bAligned)
		{
			RuntimeData.Movement.TurnBack.Phase = ETurnBackPhase::None;
			RuntimeData.Movement.TurnBack.EntryDirection = FVector::ZeroVector;

			UE_LOG(LogZZZAnim, Log,
				TEXT("[TurnBack][Phase] Released->None ForwardDot=%.3f ShouldMove=%d"),
				ForwardDot,
				RuntimeData.ZZZAnim.bShouldMove ? 1 : 0);
		}
		break;
	}
	}
}
