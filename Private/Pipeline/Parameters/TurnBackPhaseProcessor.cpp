/**
 * @file TurnBackPhaseProcessor.cpp
 * @brief TurnBack 逻辑时间轴处理器实现
 */
#include "Pipeline/Parameters/TurnBackPhaseProcessor.h"
#include "BaseCharacter.h"
#include "Data/Config/UCharConfigData.h"
#include "Data/Runtime/RuntimeData.h"
#include "Animation/zzzAnim/Locomotion/ZZZLocomotionRules.h"
#include "Animation/zzzAnim/ZZZAnimLog.h"
#include "GameFramework/Character.h"

namespace
{
	constexpr float DefaultReleaseTimeSeconds = 0.17f;
	constexpr float DefaultDurationSeconds = 2.40f;
}

void FTurnBackPhaseProcessor::Init(ACharacter* InOwner)
{
	Owner = InOwner;
	ReleaseTimeSeconds = DefaultReleaseTimeSeconds;
	DurationSeconds = DefaultDurationSeconds;
	bTurnBackInputLatched = false;
	bCanYawNotified = false;

	const ABaseCharacter* BaseOwner = Cast<ABaseCharacter>(Owner);
	const UCharConfigData* CharacterConfig = BaseOwner
		? BaseOwner->GetCharacterConfig()
		: nullptr;
	if (!CharacterConfig)
	{
		return;
	}

	const FMovementConfig& MovementConfig = CharacterConfig->MovementConfig;
	const float ConfigDuration = MovementConfig.TurnBackDurationSeconds;
	DurationSeconds = FMath::IsFinite(ConfigDuration)
		? FMath::Max(ConfigDuration, KINDA_SMALL_NUMBER)
		: DefaultDurationSeconds;

	const float ConfigRelease = MovementConfig.TurnBackReleaseTimeSeconds;
	ReleaseTimeSeconds = FMath::IsFinite(ConfigRelease)
		? FMath::Clamp(ConfigRelease, 0.f, DurationSeconds)
		: FMath::Min(DefaultReleaseTimeSeconds, DurationSeconds);
}

void FTurnBackPhaseProcessor::NotifyCanYaw()
{
	bCanYawNotified = true;
}

void FTurnBackPhaseProcessor::Process(FRuntimeData& RuntimeData, float DeltaTime)
{
	FTurnBackRuntimeModel& TurnBack = RuntimeData.Movement.TurnBack;

	const auto ResetTurnBack = [&TurnBack]()
	{
		TurnBack.Phase = ETurnBackPhase::None;
		TurnBack.bCanYaw = false;
		TurnBack.bSecondSegment = false;
		TurnBack.ElapsedSeconds = 0.f;
	};

	if (!Owner)
	{
		ResetTurnBack();
		bTurnBackInputLatched = false;
		bCanYawNotified = false;
		return;
	}

	// 离开 Moving 直接复位，避免相位和边沿锁存残留到下一次移动。
	if (RuntimeData.State.CurrentState != ECharacterStateType::Moving)
	{
		ResetTurnBack();
		bTurnBackInputLatched = false;
		bCanYawNotified = false;
		return;
	}

	const FVector DesiredDir = RuntimeData.Intent.DesiredWorldMoveDir.GetSafeNormal2D();
	const FVector ActorForward = Owner->GetActorForwardVector().GetSafeNormal2D();
	const float ForwardDot = (!DesiredDir.IsNearlyZero() && !ActorForward.IsNearlyZero())
		? FVector::DotProduct(ActorForward, DesiredDir)
		: 1.0f;
	const float ReverseThreshold = ZZZLocomotionRules::DefaultTurnBackReverseInputDotThreshold;

	const bool bReverseRunInput =
		RuntimeData.Gait.ResolvedGait == EMovementGait::Run
		&& RuntimeData.ZZZAnim.bShouldMove
		&& !DesiredDir.IsNearlyZero()
		&& !ActorForward.IsNearlyZero()
		&& ForwardDot <= ReverseThreshold;

	const auto AdvanceElapsed = [this, &TurnBack](float InDeltaTime)
	{
		if (FMath::IsFinite(InDeltaTime) && InDeltaTime > 0.f)
		{
			TurnBack.ElapsedSeconds = FMath::Min(
				TurnBack.ElapsedSeconds + InDeltaTime,
				DurationSeconds);
		}
	};

	const auto ApplyCanYaw = [this, &TurnBack]()
	{
		if (!bCanYawNotified || TurnBack.Phase == ETurnBackPhase::None)
		{
			return;
		}

		TurnBack.bCanYaw = true;
		TurnBack.bSecondSegment = true;
		bCanYawNotified = false;

		UE_LOG(LogZZZAnim, Log,
			TEXT("[TurnBack][Phase] CanYaw->D1 Elapsed=%.3f"),
			TurnBack.ElapsedSeconds);
	};

	if (TurnBack.Phase != ETurnBackPhase::None)
	{
		ApplyCanYaw();
	}

	switch (TurnBack.Phase)
	{
	case ETurnBackPhase::None:
	{
		// CanYaw 只对当前 TurnBack 生效；在 None 阶段丢弃旧动画留下的待处理信号。
		bCanYawNotified = false;

		// TurnBack 完成后，持续按住同一个反向输入只回到 WalkRun，不立即开启下一次转身。
		// 必须先离开反向阈值（松开或改变方向）才能消费下一次反向输入边沿。
		if (!bReverseRunInput)
		{
			bTurnBackInputLatched = false;
			break;
		}

		if (bTurnBackInputLatched)
		{
			break;
		}

		TurnBack.Phase = ETurnBackPhase::Frozen;
		TurnBack.bSecondSegment = false;
		TurnBack.ElapsedSeconds = 0.f;
		bTurnBackInputLatched = true;

		UE_LOG(LogZZZAnim, Log,
			TEXT("[TurnBack][Phase] None->Frozen ForwardDot=%.3f EntryYaw=%.2f InputYaw=%.2f Release=%.3f Duration=%.3f"),
			ForwardDot,
			ActorForward.Rotation().Yaw,
			DesiredDir.IsNearlyZero() ? 0.0f : DesiredDir.Rotation().Yaw,
			ReleaseTimeSeconds,
			DurationSeconds);
		break;
	}

	case ETurnBackPhase::Frozen:
	{
		// 第一段不可打断；即使输入已经松开，也必须推进到 Released/第二段。
		AdvanceElapsed(DeltaTime);

		if (TurnBack.ElapsedSeconds >= ReleaseTimeSeconds)
		{
			TurnBack.Phase = ETurnBackPhase::Released;

			UE_LOG(LogZZZAnim, Log,
				TEXT("[TurnBack][Phase] Frozen->Released Elapsed=%.3f ReleaseTime=%.3f"),
				TurnBack.ElapsedSeconds,
				ReleaseTimeSeconds);
		}

		break;
	}

	case ETurnBackPhase::Released:
	{
		AdvanceElapsed(DeltaTime);

		const bool bNaturalComplete =
			TurnBack.ElapsedSeconds >= DurationSeconds;
		const bool bInterrupted =
			TurnBack.bSecondSegment && !RuntimeData.ZZZAnim.bShouldMove;
		if (!bNaturalComplete && !bInterrupted)
		{
			break;
		}

		const float ElapsedBeforeReset = TurnBack.ElapsedSeconds;
		const bool bSecondSegmentBeforeReset = TurnBack.bSecondSegment;
		ResetTurnBack();

		UE_LOG(LogZZZAnim, Log,
			TEXT("[TurnBack][Phase] Released->None Elapsed=%.3f NaturalComplete=%d SecondSegment=%d Interrupted=%d ShouldMove=%d"),
			ElapsedBeforeReset,
			bNaturalComplete ? 1 : 0,
			bSecondSegmentBeforeReset ? 1 : 0,
			bInterrupted ? 1 : 0,
			RuntimeData.ZZZAnim.bShouldMove ? 1 : 0);
		break;
	}
	}
}
