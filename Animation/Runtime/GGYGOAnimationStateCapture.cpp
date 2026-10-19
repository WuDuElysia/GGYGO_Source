/**
 * @file GGYGOAnimationStateCapture.cpp
 * @brief 动画语义帧抓取实现
 */
#include "Animation/Runtime/GGYGOAnimationStateCapture.h"

#include "Animation/Debug/GGYGOAnimationDebugFrame.h"
#include "Animation/Runtime/GGYGOAnimationStateFrame.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"

#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Character.h"

namespace
{
	void CaptureMotionExecution(const UGGYGOCharacterMovementComponent& Movement,
		const ACharacter& Owner, FGGYGOAnimationMotionFrame& Frame)
	{
		const FGGYGOMotionExecutionSnapshot Snapshot = Movement.GetMotionExecutionSnapshot();
		Frame.Status = Snapshot.Status;
		Frame.SourceKind = Snapshot.SourceKind;
		Frame.MotionType = Snapshot.MotionType;
		Frame.TurnBackPhase = Snapshot.TurnBackPhase;
		Frame.RotationMode = Snapshot.RotationMode;
		Frame.bEnded = Snapshot.bEnded;
		Frame.EndReason = Snapshot.EndReason;
		Frame.Diagnostic = Snapshot.Diagnostic;
		if (Snapshot.Status != EGGYGOMotionExecutionStatus::Valid)
		{
			return;
		}
		const auto Reject = [&Frame, &Owner](const TCHAR* Reason)
		{
			Frame.Status = EGGYGOMotionExecutionStatus::Invalid;
			Frame.Diagnostic = FString::Printf(TEXT("[Animation][MotionCapture] Character=%s Reason=%s"), *Owner.GetPathName(), Reason);
		};
		if (Snapshot.OriginalMovement.Get() != &Movement || Snapshot.OriginalCharacter.Get() != &Owner
			|| Snapshot.OriginalUpdatedComponent.Get() != Movement.UpdatedComponent
			|| Snapshot.CompletedIntervalSerial == 0
			|| !FMath::IsFinite(Snapshot.NativeDeltaSeconds) || Snapshot.NativeDeltaSeconds <= 0.0f
			|| !FMath::IsFinite(Snapshot.MotionStartSeconds) || !FMath::IsFinite(Snapshot.MotionEndSeconds)
			|| !FMath::IsFinite(Snapshot.RequestedYawDeltaDegrees)
			|| Snapshot.RotationBefore.ContainsNaN() || Snapshot.RotationAfter.ContainsNaN()
			|| !Snapshot.RotationBefore.IsNormalized() || !Snapshot.RotationAfter.IsNormalized())
		{
			Reject(TEXT("完成区间身份、原时间或实际旋转无效"));
			return;
		}
		UAnimSequence* Sequence = nullptr;
		UAnimMontage* Montage = nullptr;
		if (Snapshot.SourceKind == EGGYGOMotionExecutionSource::Locomotion)
		{
			if (!Snapshot.OriginalLocomotionBinding.IsValid())
			{
				Reject(TEXT("原Locomotion源绑定缺失"));
				return;
			}
		}
		else if (Snapshot.SourceKind == EGGYGOMotionExecutionSource::Montage)
		{
			if (!Snapshot.OriginalActionBinding.IsValid() || !Snapshot.OriginalActionBinding->Montage.IsValid()
				|| Snapshot.bHasSingleSequenceTime)
			{
				Reject(TEXT("原Montage绑定缺失或复合源被错误压成单Sequence时间"));
				return;
			}
			Montage = Snapshot.OriginalActionBinding->Montage.Get();
		}
		else
		{
			Reject(TEXT("Valid区间没有明确原源类型"));
			return;
		}
		if (Snapshot.bHasSingleSequenceTime)
		{
			const UAnimSequence* OriginalSequence = Snapshot.OriginalSequence.Get();
			if (!OriginalSequence || !FMath::IsFinite(Snapshot.SequenceStartSeconds) || !FMath::IsFinite(Snapshot.SequenceEndSeconds)
				|| Snapshot.SequenceStartSeconds < 0.0 || Snapshot.SequenceEndSeconds < Snapshot.SequenceStartSeconds
				|| Snapshot.SequenceEndSeconds > OriginalSequence->GetPlayLength())
			{
				Reject(TEXT("同源单Sequence终点已失效或超出真实原片范围"));
				return;
			}
			// 播放器的资产参数要求非const指针；这里只保留GC引用，不修改原资产。
			Sequence = const_cast<UAnimSequence*>(OriginalSequence);
		}
		Frame.SingleSequence = Sequence;
		Frame.Montage = Montage;
		Frame.bHasSingleSequenceTime = Snapshot.bHasSingleSequenceTime;
		Frame.SequenceStartSeconds = Snapshot.SequenceStartSeconds;
		Frame.SequenceEndSeconds = Snapshot.SequenceEndSeconds;
		Frame.MotionStartSeconds = Snapshot.MotionStartSeconds;
		Frame.MotionEndSeconds = Snapshot.MotionEndSeconds;
		Frame.NativeDeltaSeconds = Snapshot.NativeDeltaSeconds;
		Frame.RequestedYawDeltaDegrees = Snapshot.RequestedYawDeltaDegrees;
		Frame.bCurveYawApplied = Snapshot.bCurveYawApplied;
		Frame.RotationBefore = Snapshot.RotationBefore;
		Frame.RotationAfter = Snapshot.RotationAfter;
		Frame.bNativeSourceFinished = Snapshot.bNativeSourceFinished;
		Frame.ExecutionRequestSerial = Snapshot.ExecutionRequestSerial;
		Frame.MotionSequence = Snapshot.MotionSequence;
		Frame.ActionHandle = Snapshot.ActionHandle;
		Frame.CompletedIntervalSerial = Snapshot.CompletedIntervalSerial;
		Frame.CompletedFrame = Snapshot.CompletedFrame;
		Frame.SourceIntervals = Snapshot.SourceIntervals;
	}
}

void FGGYGOAnimationStateCapture::Capture(
	FGGYGOAnimationStateFrame& OutState,
	FGGYGOAnimationDebugFrame& OutDebug,
	const ACharacter* InOwner) const
{
	check(IsInGameThread());

	OutState = FGGYGOAnimationStateFrame();
	OutDebug = FGGYGOAnimationDebugFrame();

	if (!InOwner)
	{
		return;
	}

	if (const UAbilitySystemComponent* AbilitySystem =
		UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(InOwner, true))
	{
		AbilitySystem->GetOwnedGameplayTags(OutState.OwnedStateTags);
	}

	const UGGYGOCharacterMovementComponent* MoveComp =
		Cast<UGGYGOCharacterMovementComponent>(InOwner->GetCharacterMovement());
	OutState.bLocomotionSteeringCaptured = true;
	if (!MoveComp)
	{
		// Preserve the existing bare-Character preview fields. An enabled lean consumer
		// still sees the missing required provider as failure rather than a valid zero rate.
		OutState.LocomotionSteering.Status = EGGYGOLocomotionSteeringStatus::Invalid;
		OutState.LocomotionSteering.Diagnostic = FString::Printf(
			TEXT("[Animation][SteeringCapture] Character=%s Provider=GGYGOCharacterMovementComponent Reason=required project CMC is missing"),
			*GetPathNameSafe(InOwner));
		OutState.MotionExecution.Status = EGGYGOMotionExecutionStatus::Invalid;
		OutState.MotionExecution.Diagnostic = OutState.LocomotionSteering.Diagnostic;
		OutState.SteeringExecution.Status = static_cast<uint8>(OutState.LocomotionSteering.Status);
		return;
	}
	OutState.LocomotionSteering = MoveComp->GetLocomotionSteeringSnapshot();
	if (OutState.LocomotionSteering.Status != EGGYGOLocomotionSteeringStatus::Invalid
		&& (OutState.LocomotionSteering.OriginalMovement.Get() != MoveComp
			|| OutState.LocomotionSteering.OriginalCharacter.Get() != InOwner))
	{
		OutState.LocomotionSteering.Status = EGGYGOLocomotionSteeringStatus::Invalid;
		OutState.LocomotionSteering.Diagnostic = FString::Printf(
			TEXT("[Animation][SteeringCapture] Character=%s Provider=%s Reason=returned interval belongs to another original source"),
			*GetPathNameSafe(InOwner), *GetPathNameSafe(MoveComp));
	}
	const FGGYGOLocomotionSteeringSnapshot& Steering = OutState.LocomotionSteering;
	OutState.SteeringExecution.Status = static_cast<uint8>(Steering.Status);
	OutState.SteeringExecution.SourceEpoch = Steering.SourceEpoch;
	OutState.SteeringExecution.CompletedIntervalSerial = Steering.CompletedIntervalSerial;
	OutState.SteeringExecution.CompletedFrame = Steering.CompletedFrame;
	OutState.SteeringExecution.NativeDeltaSeconds = Steering.NativeDeltaSeconds;
	OutState.SteeringExecution.DesiredDirectionError = Steering.DesiredDirectionError;
	OutState.SteeringExecution.ActualSignedYawRate = Steering.ActualSignedYawRate;
	OutState.SteeringExecution.ActualSignedVelocityYawRate = Steering.ActualSignedVelocityYawRate;
	OutState.SteeringExecution.bHasVelocityYawRate = Steering.bHasVelocityYawRate;
	CaptureMotionExecution(*MoveComp, *InOwner, OutState.MotionExecution);
	const USkeletalMeshComponent* Mesh = InOwner->GetMesh();
	const USceneComponent* Capsule = MoveComp->UpdatedComponent;
	if (Mesh && Capsule && !Mesh->GetComponentTransform().ContainsNaN()
		&& !Capsule->GetComponentTransform().ContainsNaN())
	{
		const FQuat MeshRotation = Mesh->GetComponentQuat();
		const FQuat CapsuleRotation = Capsule->GetComponentQuat();
		if (MeshRotation.IsNormalized() && CapsuleRotation.IsNormalized())
		{
			OutState.LeanAxisCS = MeshRotation.UnrotateVector(-CapsuleRotation.GetForwardVector());
			OutState.CapsuleUpCS = MeshRotation.UnrotateVector(CapsuleRotation.GetUpVector());
			OutState.bCapsuleMeshBasisValid = true;
		}
	}

	OutState.WorldVelocity = MoveComp->Velocity;
	OutState.WorldAcceleration = MoveComp->GetCurrentAcceleration();
	OutState.HorizontalSpeed = MoveComp->GetHorizontalSpeed();
	OutState.WorldVelocityDirection = MoveComp->GetHorizontalVelocityDirection();
	OutState.LocalVelocityAngle = MoveComp->GetLocalVelocityAngle();
	float LocalForward = 0.0f;
	float LocalRight = 0.0f;
	MoveComp->GetLocalVelocityAxes(LocalForward, LocalRight);
	OutState.LocalVelocityBlend = FVector2D(LocalForward, LocalRight);
	OutState.WalkRunBlendAlpha = MoveComp->GetWalkRunBlendAlpha();
	OutState.StopMotionType = MoveComp->GetStopMotionType();

	OutState.Gait = MoveComp->GetResolvedGait();
	OutState.MovementMode = MoveComp->MovementMode;
	OutState.TurnBackPhase = MoveComp->GetTurnBackPhase();
	OutState.bHasMoveInput = MoveComp->HasMoveInput();
	OutState.bMovingHorizontally = MoveComp->IsMovingHorizontally();
	OutState.bGrounded = MoveComp->IsMovingOnGround();
	OutState.bMovementBlocked = MoveComp->IsMovementBlockedByTag();
	OutState.bTurnBackRunOut = MoveComp->IsTurnBackRunOut();

	const FVector MoveIntent = OutState.WorldAcceleration.GetSafeNormal2D();
	const FVector ActorForward = InOwner->GetActorForwardVector().GetSafeNormal2D();
	if (!MoveIntent.IsNearlyZero() && !ActorForward.IsNearlyZero())
	{
		OutDebug.InputForwardDot = FMath::Clamp(
			FVector::DotProduct(ActorForward, MoveIntent),
			-1.0f,
			1.0f);
	}

	const FGGYGOLocomotionCurveSample& CurveMotion = MoveComp->GetCurveMotion();
	OutDebug.CurveVelocity = CurveMotion.Velocity;
	OutDebug.CurveVelocityDirection = CurveMotion.Direction;
	OutDebug.CurveVelocityAngle = CurveMotion.DirectionAngle;
}
