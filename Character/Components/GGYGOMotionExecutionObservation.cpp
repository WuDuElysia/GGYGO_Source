/**
 * @file GGYGOMotionExecutionObservation.cpp
 * @brief 原 CMC 区间的源关联、真实旋转认证和只读发布；不另建调度或时钟。
 */
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Character/Components/GGYGOActionCurveRootMotionSource.h"
#include "Character/Components/GGYGOCurveRootMotionSource.h"
#include "Character/Data/GGYGOActionMotionEvaluation.h"
#include "Character/Data/GGYGOMovementSet.h"
#include "Animation/AnimSequence.h"
#include "GameFramework/Character.h"

namespace
{
	/** 与原生 GetOverrideRootMotionRotation 的选择一致，不把所有 Prepared 当获选源。 */
	TSharedPtr<FRootMotionSource> FindRotationSource(const FRootMotionSourceGroup& Group)
	{
		for (const auto& Source : Group.RootMotionSources)
			if (Source.IsValid() && Source->AccumulateMode == ERootMotionAccumulateMode::Override) return Source;
		return nullptr;
	}

	void SetSingleSequenceTime(FGGYGOMotionExecutionSnapshot& Snapshot,
		const FGGYGOLocomotionSequenceSource& Source)
	{
		Snapshot.OriginalSequence = Source.Sequence.Get();
		Snapshot.bHasSingleSequenceTime = true;
		Snapshot.SequenceStartSeconds = FMath::Min(static_cast<double>(Snapshot.MotionStartSeconds) * Source.SequenceRateScale, static_cast<double>(Source.PlayLength));
		Snapshot.SequenceEndSeconds = FMath::Min(static_cast<double>(Snapshot.MotionEndSeconds) * Source.SequenceRateScale, static_cast<double>(Source.PlayLength));
		Snapshot.bEnded = !Source.bLoop && Snapshot.SequenceEndSeconds >= Source.PlayLength;
		if (Snapshot.bEnded) Snapshot.EndReason = EGGYGOMotionExecutionEndReason::Completed;
	}
}

void UGGYGOCharacterMovementComponent::BeginMotionExecutionInterval()
{
	if (MotionExecutionSnapshot.SourceKind != EGGYGOMotionExecutionSource::None
		&& (GFrameCounter < MotionExecutionSnapshot.CompletedFrame || GFrameCounter - MotionExecutionSnapshot.CompletedFrame > 1))
		MotionExecutionSnapshot = {};
	PendingMotionExecution = {};
	bHasPendingMotionExecution = false;
	bMotionExecutionIntervalOpen = IsValid(CharacterOwner) && IsValid(UpdatedComponent)
		&& !CharacterOwner->bClientUpdating && CharacterOwner->GetLocalRole() != ROLE_SimulatedProxy;
}

void UGGYGOCharacterMovementComponent::PrepareOrdinaryMotionExecution(
	const FLocomotionUpdateCandidate& Candidate, const FGGYGOLocomotionSourceBindingPtr& Binding,
	float StartTime, float EndTime, float StartCycle, float EndCycle)
{
	if (!bMotionExecutionIntervalOpen || !Candidate.Motion.bHasCurveSource) return;
	FGGYGOMotionExecutionSnapshot Pending;
	Pending.OriginalMovement = this;
	Pending.OriginalCharacter = CharacterOwner;
	Pending.OriginalUpdatedComponent = UpdatedComponent.Get();
	Pending.SourceKind = EGGYGOMotionExecutionSource::Locomotion;
	// 普通 CMC 路径先只声明平移；原 SourceYaw 合同由精确的原源身份继承。
	Pending.RotationMode = EGGYGOMotionRotationMode::TranslationOnly;
	if (IsLocomotionCurveOriginForCandidate(CompletedLocomotionCurveOrigin, Candidate, Binding))
	{
		Pending.OriginalLocomotionOrigin = CompletedLocomotionCurveOrigin;
		Pending.RotationMode = CompletedLocomotionCurveOrigin->RotationMode;
	}
	Pending.OriginalLocomotionBinding = Binding;
	Pending.ExecutionRequestSerial = LocomotionRequestSerial;
	Pending.MotionSequence = Candidate.MotionSequence;
	Pending.MotionType = Candidate.MotionType;
	Pending.TurnBackPhase = Candidate.TurnPhase;
	Pending.MotionStartSeconds = StartTime;
	Pending.MotionEndSeconds = EndTime;
	Pending.StartCyclePosition = StartCycle;
	Pending.EndCyclePosition = EndCycle;
	Pending.RequestedYawDeltaDegrees = Candidate.Motion.YawDeltaDegrees;
	if (const auto* Source = Binding->GetSingleSource(Candidate.MotionType)) SetSingleSequenceTime(Pending, *Source);
	PendingMotionExecution = MoveTemp(Pending);
	bHasPendingMotionExecution = true;
}

bool UGGYGOCharacterMovementComponent::PrepareNativeMotionExecutionRotation()
{
	const auto NativeSource = FindRotationSource(CurrentRootMotion);
	if (!NativeSource.IsValid() || !UpdatedComponent
		|| !NativeSource->Status.HasFlag(ERootMotionSourceStatusFlags::Prepared)) return false;
	FGGYGOMotionExecutionSnapshot Pending;
	if (NativeSource->GetScriptStruct() == FRootMotionSource_GGYGOActionCurve::StaticStruct())
	{
		const auto& Source = static_cast<const FRootMotionSource_GGYGOActionCurve&>(*NativeSource);
		if (Source.SourceMode != EGGYGOActionCurveSourceMode::OriginalMontage || Source.bExplicitlyCancelled
			|| !Source.OriginalBinding.IsValid()) return false;
		Pending.SourceKind = EGGYGOMotionExecutionSource::Montage;
		Pending.RotationMode = Source.RotationMode;
		Pending.OriginalActionBinding = Source.OriginalBinding;
		Pending.OriginalActionResource = Source.OriginalResource;
		Pending.ActionHandle = Source.OriginalResource.IsValid() ? Source.OriginalResource->Handle : INDEX_NONE;
		Pending.NativeStartSeconds = Source.PreparedNativeStartSeconds;
		Pending.NativeEndSeconds = Source.PreparedNativeEndSeconds;
		Pending.MotionStartSeconds = Source.PreparedMontageStartSeconds;
		Pending.MotionEndSeconds = Source.PreparedMontageEndSeconds;
		Pending.RequestedYawDeltaDegrees = Source.PreparedYawDeltaDegrees;
		Pending.SourceIntervals = Source.PreparedSourceIntervals;
		Pending.bNativeSourceFinished = Source.bPreparedNaturalEnd;
		Pending.bEnded = Source.bPreparedNaturalEnd;
		if (Pending.bEnded) Pending.EndReason = EGGYGOMotionExecutionEndReason::Completed;
	}
	else if (NativeSource->GetScriptStruct() == FRootMotionSource_GGYGOCurve::StaticStruct())
	{
		const auto& Source = static_cast<const FRootMotionSource_GGYGOCurve&>(*NativeSource);
		if (!Source.Origin.IsValid() || !Source.Prepared.IsValid() || !Source.Prepared->Sample.bHasCurveSource) return false;
		Pending.SourceKind = EGGYGOMotionExecutionSource::Locomotion;
		Pending.RotationMode = Source.Origin->RotationMode;
		Pending.OriginalLocomotionOrigin = Source.Origin;
		Pending.OriginalLocomotionBinding = Source.Origin->SourceBinding;
		Pending.ExecutionRequestSerial = Source.Origin->ExecutionRequestSerial;
		Pending.MotionSequence = Source.Origin->MotionSequence;
		Pending.MotionType = Source.Origin->MotionType;
		Pending.TurnBackPhase = TurnBackPhase;
		Pending.NativeStartSeconds = Source.Prepared->NativeStartTime;
		Pending.NativeEndSeconds = Source.Prepared->NativeEndTime;
		Pending.MotionStartSeconds = Source.Prepared->MotionStartTime;
		Pending.MotionEndSeconds = Source.Prepared->MotionEndTime;
		Pending.RequestedYawDeltaDegrees = Source.Prepared->Sample.YawDeltaDegrees;
		Pending.bNativeSourceFinished = Source.Prepared->Result == EGGYGOCurveRootMotionPrepareResult::Finished;
		if (const auto* Sequence = Source.Origin->SourceBinding->GetSingleSource(Source.Origin->MotionType)) SetSingleSequenceTime(Pending, *Sequence);
	}
	else return false;
	// 历史回放与原生代理仍由原 RMS 旋转，不发行一份新的 live 表现事实。
	if (bMotionExecutionIntervalOpen)
	{
		Pending.OriginalMovement = this;
		Pending.OriginalCharacter = CharacterOwner;
		Pending.OriginalUpdatedComponent = UpdatedComponent.Get();
		Pending.OriginalNativeSource = NativeSource;
		Pending.bHasNativeSourceInterval = true;
		Pending.bCurveYawApplied = Pending.RotationMode == EGGYGOMotionRotationMode::SourceYaw;
		Pending.RotationBefore = UpdatedComponent->GetComponentQuat();
		PendingMotionExecution = MoveTemp(Pending);
		bHasPendingMotionExecution = true;
	}
	bLocomotionSteeringIntervalExcluded = true;
	return true;
}

bool UGGYGOCharacterMovementComponent::ApplyOrdinaryMotionCurveYaw()
{
	if (!UpdatedComponent || !CharacterOwner || CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy
		|| !CurveMotion.bHasCurveSource || HasActiveActionMotion() || CurrentRootMotion.HasOverrideVelocity()
		|| CurrentRootMotion.HasAdditiveVelocity()) return false;
	const auto OriginalOrigin = CompletedLocomotionCurveOrigin;
	const FLocomotionUpdateCandidate Candidate = CaptureLocomotionCandidate();
	const bool bOriginalSourceCurrent = !IsMovementInputRequestBlocked()
		&& IsLocomotionCurveOriginForCandidate(OriginalOrigin, Candidate, LocomotionSourceBinding);
	// 观测仅防旧栈串入后继；执行权限取 CMC 的原 Origin，不从观测或本帧 Yaw 发放。
	if (bHasPendingMotionExecution
		&& (PendingMotionExecution.ExecutionRequestSerial != LocomotionRequestSerial
			|| PendingMotionExecution.OriginalLocomotionBinding != LocomotionSourceBinding
			|| PendingMotionExecution.MotionType != Candidate.MotionType
			|| PendingMotionExecution.MotionSequence != Candidate.MotionSequence
			|| (PendingMotionExecution.OriginalLocomotionOrigin.IsValid()
				&& PendingMotionExecution.OriginalLocomotionOrigin != OriginalOrigin)))
		return true;
	if (bHasPendingMotionExecution) PendingMotionExecution.RotationBefore = UpdatedComponent->GetComponentQuat();
	const float Yaw = CurveMotion.YawDeltaDegrees;
	if (!FMath::IsFinite(Yaw))
	{
		RejectLocomotionEvaluation(LocomotionRequestSerial, TEXT("original curve Yaw interval is non-finite"));
		return true;
	}
	const bool bOriginalSourceYaw = bOriginalSourceCurrent
		&& OriginalOrigin->RotationMode == EGGYGOMotionRotationMode::SourceYaw;
	if (!bOriginalSourceYaw)
	{
		if (Yaw == 0.0f) return false; // 无原源旋转合同的普通路径保留既有输入朝向。
		RejectLocomotionEvaluation(LocomotionRequestSerial, TEXT("curve Yaw requires an admitted original source rotation contract"));
		return true;
	}
	if (CharacterOwner->bUseControllerRotationYaw)
	{
		RejectLocomotionEvaluation(LocomotionRequestSerial, TEXT("source Yaw requires sole CMC rotation ownership; original Character enables Controller Yaw"));
		return true;
	}
	if (bHasPendingMotionExecution) PendingMotionExecution.bCurveYawApplied = true;
	bLocomotionSteeringIntervalExcluded = true;
	// 完整原 SourceYaw 的有效零区间也是独占旋转消费；不重挂已经结束的 RMS。
	if (Yaw == 0.0f) return true;
	const FQuat Rotation = FRotator(0.0f, Yaw, 0.0f).Quaternion() * UpdatedComponent->GetComponentQuat();
	MoveUpdatedComponent(FVector::ZeroVector, Rotation, true);
	return true;
}

void UGGYGOCharacterMovementComponent::CompleteMotionExecutionInterval(float DeltaSeconds, const FVector& OldLocation)
{
	if (!bMotionExecutionIntervalOpen) return;
	bMotionExecutionIntervalOpen = false;
	FGGYGOMotionExecutionSnapshot Completed = MoveTemp(PendingMotionExecution);
	PendingMotionExecution = {};
	const bool bHasInterval = bHasPendingMotionExecution;
	bHasPendingMotionExecution = false;
	const bool bObservationSerialAvailable = MotionExecutionIntervalSerial != MAX_uint64;
	Completed.CompletedIntervalSerial = bObservationSerialAvailable ? ++MotionExecutionIntervalSerial : 0;
	Completed.CompletedFrame = GFrameCounter;
	Completed.NativeDeltaSeconds = DeltaSeconds;
	Completed.Status = EGGYGOMotionExecutionStatus::NotApplicable;
	if (!bHasInterval)
	{
		Completed.Diagnostic = TEXT("[Movement.MotionExecution] native interval had no original curve motion source");
		MotionExecutionSnapshot = MoveTemp(Completed);
		return;
	}
	if (Completed.OriginalCharacter.Get() != CharacterOwner || Completed.OriginalUpdatedComponent.Get() != UpdatedComponent
		|| !IsValid(CharacterOwner) || !IsValid(UpdatedComponent) || CharacterOwner->bClientUpdating)
	{
		Completed.Diagnostic = TEXT("[Movement.MotionExecution] original owner/component was replaced during the interval");
		MotionExecutionSnapshot = MoveTemp(Completed);
		return;
	}
	Completed.RotationAfter = UpdatedComponent->GetComponentQuat();
	Completed.LocationBefore = OldLocation;
	Completed.LocationAfter = UpdatedComponent->GetComponentLocation();
	Completed.WorldVelocity = Velocity;
	const auto OriginalAction = Completed.OriginalActionResource;
	const bool bActionRetired = Completed.SourceKind == EGGYGOMotionExecutionSource::Montage && OriginalAction.IsValid()
		&& ActionMotionExecution.ActiveMontageActionResource != OriginalAction && ActionMotionExecution.CompletedMontageActionResource != OriginalAction;
	const bool bLocomotionRetired = Completed.SourceKind == EGGYGOMotionExecutionSource::Locomotion
		&& (Completed.ExecutionRequestSerial != LocomotionRequestSerial || Completed.OriginalLocomotionBinding != LocomotionSourceBinding
			|| IsMovementInputRequestBlocked()
			|| (!Completed.bEnded && (Completed.MotionType != LocomotionMotionType
				|| Completed.MotionSequence != LocomotionMotionSequence))
			|| (!Completed.bEnded && Completed.OriginalLocomotionOrigin.IsValid()
				&& !IsLocomotionCurveOriginForCandidate(Completed.OriginalLocomotionOrigin,
					CaptureLocomotionCandidate(), LocomotionSourceBinding)));
	if (bActionRetired || bLocomotionRetired)
	{
		Completed.bEnded = true;
		Completed.EndReason = EGGYGOMotionExecutionEndReason::Cancelled;
		Completed.Diagnostic = TEXT("[Movement.MotionExecution] original request retired during native movement; no successor is certified");
		MotionExecutionSnapshot = MoveTemp(Completed);
		return;
	}
	const auto NativeSource = FindRotationSource(CurrentRootMotion);
	const bool bOriginalNativeSource = !Completed.bHasNativeSourceInterval || NativeSource.Get() == Completed.OriginalNativeSource.Get();
	const FQuat RequestedRotation = Completed.bHasNativeSourceInterval && Completed.OriginalNativeSource.IsValid()
		? Completed.OriginalNativeSource->RootMotionParams.GetRootMotionTransform().GetRotation()
		: FRotator(0.0f, Completed.RequestedYawDeltaDegrees, 0.0f).Quaternion();
	const bool bRotationConsumed = Completed.RotationMode == EGGYGOMotionRotationMode::SourceYaw
		? Completed.bCurveYawApplied
			&& Completed.RotationAfter.Equals(RequestedRotation * Completed.RotationBefore, 1.e-4)
		: Completed.RotationMode == EGGYGOMotionRotationMode::TranslationOnly
			&& !Completed.bCurveYawApplied && Completed.RequestedYawDeltaDegrees == 0.0f
			&& RequestedRotation.Equals(FQuat::Identity, 1.e-4);
	if (!FMath::IsFinite(DeltaSeconds) || DeltaSeconds <= 0.0f || !FMath::IsFinite(Completed.RequestedYawDeltaDegrees)
		|| Completed.RotationBefore.ContainsNaN() || Completed.RotationAfter.ContainsNaN()
		|| Completed.LocationBefore.ContainsNaN() || Completed.LocationAfter.ContainsNaN() || Velocity.ContainsNaN()
		|| (Completed.bCurveYawApplied && CharacterOwner->bUseControllerRotationYaw)
		|| !bOriginalNativeSource || !bRotationConsumed)
	{
		Completed.Status = EGGYGOMotionExecutionStatus::Invalid;
		Completed.bEnded = true;
		Completed.EndReason = EGGYGOMotionExecutionEndReason::Failed;
		Completed.Diagnostic = FString::Printf(TEXT("[Movement.MotionExecution] Owner='%s' Motion=%d Action=%d: original interval or requested rotation was not consumed"),
			*GetPathNameSafe(CharacterOwner), static_cast<int32>(Completed.MotionType), Completed.ActionHandle);
		const auto OriginalOrigin = Completed.OriginalLocomotionOrigin;
		const FString Error = Completed.Diagnostic;
		MotionExecutionSnapshot = MoveTemp(Completed);
		if (OriginalAction.IsValid()) FailMontageActionMotion(OriginalAction, Error);
		else if (OriginalOrigin.IsValid()) ReportLocomotionCurveRootMotionFailure(OriginalOrigin, Error);
		else RejectLocomotionEvaluation(MotionExecutionSnapshot.ExecutionRequestSerial, Error);
		return;
	}
	Completed.Status = bObservationSerialAvailable ? EGGYGOMotionExecutionStatus::Valid : EGGYGOMotionExecutionStatus::Invalid;
	if (!bObservationSerialAvailable) Completed.Diagnostic = TEXT("[Movement.MotionExecution] completed observation serial exhausted");
	// 原 native 旋转认证不依赖表现观测编号；编号不能变成另一份执行许可。
	if (NativeSource.IsValid() && NativeSource->GetScriptStruct() == FRootMotionSource_GGYGOActionCurve::StaticStruct())
		static_cast<FRootMotionSource_GGYGOActionCurve*>(NativeSource.Get())->bPreparedRotationConsumed = true;
	MotionExecutionSnapshot = MoveTemp(Completed);
}

FGGYGOMotionExecutionSnapshot UGGYGOCharacterMovementComponent::GetMotionExecutionSnapshot() const
{
	if (!IsInGameThread())
	{
		FGGYGOMotionExecutionSnapshot Invalid;
		Invalid.Status = EGGYGOMotionExecutionStatus::Invalid;
		Invalid.Diagnostic = TEXT("[Movement.MotionExecution] capture requires the game thread");
		return Invalid;
	}
	FGGYGOMotionExecutionSnapshot Result = MotionExecutionSnapshot;
	const auto Qualify = [&Result](EGGYGOMotionExecutionStatus Status, const TCHAR* Reason)
	{
		Result.Status = Status;
		Result.Diagnostic = FString::Printf(TEXT("[Movement.MotionExecution] %s"), Reason);
		return Result;
	};
	if (!IsValid(CharacterOwner) || !IsValid(UpdatedComponent) || IsBeingDestroyed()
		|| CharacterOwner->IsActorBeingDestroyed() || bMovementOwnerSyncClosed)
	{
		MotionExecutionSnapshot = {};
		return Qualify(EGGYGOMotionExecutionStatus::Invalid, TEXT("original owner/component lifecycle is closed"));
	}
	if (Result.SourceKind != EGGYGOMotionExecutionSource::None
		&& (GFrameCounter < Result.CompletedFrame || GFrameCounter - Result.CompletedFrame > 1))
	{
		MotionExecutionSnapshot = {};
		return Qualify(EGGYGOMotionExecutionStatus::NotApplicable, TEXT("expired source fact released its original references"));
	}
	if (Result.Status != EGGYGOMotionExecutionStatus::Valid) return Result;
	if (CharacterOwner->bClientUpdating || CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy
		|| GFrameCounter < Result.CompletedFrame || GFrameCounter - Result.CompletedFrame > 1)
	{
		MotionExecutionSnapshot = {};
		return Qualify(EGGYGOMotionExecutionStatus::NotApplicable, TEXT("historical replay/proxy or expired completed interval is not a live source"));
	}
	if (Result.OriginalMovement.Get() != this || Result.OriginalCharacter.Get() != CharacterOwner
		|| Result.OriginalUpdatedComponent.Get() != UpdatedComponent
		|| !UpdatedComponent->GetComponentQuat().Equals(Result.RotationAfter, 1.e-4)
		|| !UpdatedComponent->GetComponentLocation().Equals(Result.LocationAfter, UE_KINDA_SMALL_NUMBER)
		|| !Velocity.Equals(Result.WorldVelocity, UE_KINDA_SMALL_NUMBER))
		return Qualify(EGGYGOMotionExecutionStatus::NotApplicable, TEXT("completed physical result or original membership was replaced"));
	FString Error;
	if (Result.SourceKind == EGGYGOMotionExecutionSource::Locomotion)
	{
		const EGGYGOMotionExecutionStatus Membership = QualifyLocomotionExecutionMembership(Result, Error);
		if (Membership == EGGYGOMotionExecutionStatus::NotApplicable)
			return Qualify(EGGYGOMotionExecutionStatus::NotApplicable, *Error);
		if (Membership == EGGYGOMotionExecutionStatus::Invalid)
		{
			Result.Status = EGGYGOMotionExecutionStatus::Invalid;
			Result.Diagnostic = MoveTemp(Error);
		}
	}
	else if (Result.SourceKind == EGGYGOMotionExecutionSource::Montage)
	{
		const auto Resource = Result.OriginalActionResource;
		if (!Resource.IsValid() || (ActionMotionExecution.ActiveMontageActionResource != Resource
			&& ActionMotionExecution.CompletedMontageActionResource != Resource))
			return Qualify(EGGYGOMotionExecutionStatus::NotApplicable, TEXT("original Montage action was retired or replaced"));
		if (Result.OriginalNativeSource.IsValid())
		{
			const auto& Source = static_cast<const FRootMotionSource_GGYGOActionCurve&>(*Result.OriginalNativeSource);
			if (Source.bExplicitlyCancelled)
			{
				Result.bEnded = true;
				Result.EndReason = EGGYGOMotionExecutionEndReason::Cancelled;
				return Qualify(EGGYGOMotionExecutionStatus::NotApplicable, TEXT("cancelled source cannot certify a live interval"));
			}
		}
		EGGYGOMotionRotationMode Mode;
		if (!GGYGOActionMotionEvaluation::ValidateSource(*Result.OriginalActionBinding, Mode, Error) || Mode != Result.RotationMode)
		{
			Result.Status = EGGYGOMotionExecutionStatus::Invalid;
			Result.Diagnostic = Error.IsEmpty() ? TEXT("[Movement.MotionExecution] original rotation capability changed") : MoveTemp(Error);
		}
	}
	return Result;
}

EGGYGOMotionExecutionStatus UGGYGOCharacterMovementComponent::QualifyLocomotionExecutionMembership(
	const FGGYGOMotionExecutionSnapshot& Snapshot, FString& OutError) const
{
	OutError.Reset();
	const bool bSameLiveSegment = Snapshot.MotionSequence == LocomotionMotionSequence && Snapshot.MotionType == LocomotionMotionType;
	const bool bOriginalCompletedEndpoint = Snapshot.bEnded && Snapshot.EndReason == EGGYGOMotionExecutionEndReason::Completed;
	if (Snapshot.ExecutionRequestSerial != LocomotionRequestSerial || (!bSameLiveSegment && !bOriginalCompletedEndpoint)
		|| IsMovementInputRequestBlocked()
		|| Snapshot.OriginalLocomotionBinding != LocomotionSourceBinding)
	{
		OutError = TEXT("original locomotion request/segment was retired or replaced");
		return EGGYGOMotionExecutionStatus::NotApplicable;
	}
	if (!IsLocomotionSourceBindingCurrent(Snapshot.OriginalLocomotionBinding, OutError))
		return EGGYGOMotionExecutionStatus::Invalid;
	return EGGYGOMotionExecutionStatus::Valid;
}
