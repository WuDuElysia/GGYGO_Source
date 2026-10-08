/**
 * @file ZZZAnimInstance.cpp
 * @brief ZZZ 动画迁移期兼容实现
 */

#include "Animation/zzzAnim/ZZZAnimInstance.h"
#include "Animation/zzzAnim/ZZZAnimLog.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Components/SkeletalMeshComponent.h"

namespace
{
	FName GetSingleSourceKey(const FZZZAnimSet& AnimSet, EGGYGOLocomotionMotionType MotionType)
	{
		switch (MotionType)
		{
		case EGGYGOLocomotionMotionType::WalkStart: return AnimSet.WalkStartSourceKey;
		case EGGYGOLocomotionMotionType::StartStop: return AnimSet.StartStopSourceKey;
		case EGGYGOLocomotionMotionType::WalkStop: return AnimSet.WalkStopSourceKey;
		case EGGYGOLocomotionMotionType::RunStop: return AnimSet.RunStopSourceKey;
		case EGGYGOLocomotionMotionType::TurnBack: return AnimSet.TurnBackSourceKey;
		default: return NAME_None;
		}
	}

	bool ValidateWalkRunLeanTuning(const FZZZWalkRunLeanTuning& Config, FString& OutError)
	{
		OutError.Reset();
		if (!FMath::IsFinite(Config.FullLeanDirectionErrorDegrees) || Config.FullLeanDirectionErrorDegrees < 180.0f)
		{
			OutError = TEXT("FullLeanDirectionErrorDegrees must be finite and >= 180; author the angle explicitly, legacy yaw rate is not an angle");
			return false;
		}
		if (!FMath::IsFinite(Config.WalkMaxAngleDegrees) || !FMath::IsFinite(Config.RunMaxAngleDegrees)
			|| Config.WalkMaxAngleDegrees < 0.0f || Config.RunMaxAngleDegrees < Config.WalkMaxAngleDegrees
			|| Config.RunMaxAngleDegrees > 90.0f)
		{
			OutError = TEXT("Walk/Run maximum angles must be finite with 0 <= Walk <= Run <= 90 degrees");
			return false;
		}
		if (!FMath::IsFinite(Config.RecoveryResponseSpeed) || Config.RecoveryResponseSpeed <= 0.0f)
		{
			OutError = TEXT("RecoveryResponseSpeed must be finite and positive for normal exit recovery");
			return false;
		}
		return true;
	}

}

// ============================================================================
// AnimInstance 生命周期
// ============================================================================

void UZZZAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
	Super::NativeUpdateAnimation(DeltaSeconds);
	RefreshDecisionContext();
	UpdateWalkRunLeanPresentation(DeltaSeconds);
}

void UZZZAnimInstance::OnAnimationLifecycleReset()
{
	StateMemory = {};
	Snap = {};
	LocomotionEvents.Reset();

	AnimBlendX = 0.f;
	AnimBlendY = 0.f;
	AnimCurveVelocity = FVector::ZeroVector;
	AnimCurveVelocityDirection = FVector::ZeroVector;
	AnimCurveVelocityAngle = 0.f;
	ActualVelocityDirection = FVector::ZeroVector;
	ActualVelocityBlendX = 0.f;
	ActualVelocityBlendY = 0.f;
	ActualVelocityAngle = 0.f;
	bTurnBackRunOut = false;
	ResetWalkRunLeanPresentation();
}

void UZZZAnimInstance::ResetWalkRunLeanPresentation()
{
	WalkRunLeanAngleDegrees = 0.0f;
	bWalkRunLeanPresentationValid = false;
	WalkRunLeanFailureReason.Reset();
	LastWalkRunLeanFailureCode = NAME_None;
	WalkRunLeanSource.Reset();
	WalkRunLeanSourceEpoch = 0;
}

void UZZZAnimInstance::FailWalkRunLeanPresentation(FName FailureCode, const FString& Reason)
{
	WalkRunLeanAngleDegrees = 0.0f;
	bWalkRunLeanPresentationValid = false;
	WalkRunLeanFailureReason = Reason;
	if (LastWalkRunLeanFailureCode != FailureCode)
	{
		UE_LOG(LogZZZAnim, Error,
			TEXT("[Animation][WalkRunLean] Instance=%s Mesh=%s Config=Tuning.WalkRunLean Code=%s Reason=%s"),
			*GetPathName(), *GetPathNameSafe(GetSkelMeshComponent()), *FailureCode.ToString(), *Reason);
		LastWalkRunLeanFailureCode = FailureCode;
	}
}

void UZZZAnimInstance::RecoverWalkRunLeanPresentation(float DeltaSeconds, const FZZZWalkRunLeanTuning& Config)
{
	if (!FMath::IsFinite(DeltaSeconds) || DeltaSeconds < 0.0f)
	{
		FailWalkRunLeanPresentation(TEXT("AnimationDelta"), TEXT("normal recovery requires a finite nonnegative native animation delta"));
		return;
	}
	if (!FMath::IsFinite(WalkRunLeanAngleDegrees))
	{
		FailWalkRunLeanPresentation(TEXT("PresentationState"), TEXT("the previous normal-recovery pose contains a non-finite angle"));
		return;
	}

	WalkRunLeanAngleDegrees = FMath::Clamp(static_cast<float>(WalkRunLeanAngleDegrees
		* FMath::Exp(-static_cast<double>(Config.RecoveryResponseSpeed) * DeltaSeconds)),
		-Config.RunMaxAngleDegrees, Config.RunMaxAngleDegrees);
	if (FMath::IsNearlyZero(WalkRunLeanAngleDegrees))
	{
		WalkRunLeanAngleDegrees = 0.0f;
		bWalkRunLeanPresentationValid = false;
	}
	WalkRunLeanFailureReason.Reset();
	LastWalkRunLeanFailureCode = NAME_None;
}

void UZZZAnimInstance::UpdateWalkRunLeanPresentation(float DeltaSeconds)
{
	check(IsInGameThread());
	const FZZZWalkRunLeanTuning& Config = Tuning.WalkRunLean;
	const FGGYGOAnimationStateFrame& Frame = GetAnimationStateFrame();
	if (!Config.bEnabled || !Frame.bLocomotionSteeringCaptured)
	{
		// Explicit disabled mode, or legal native initialization before a Character exists.
		ResetWalkRunLeanPresentation();
		return;
	}

	FString ConfigError;
	if (!ValidateWalkRunLeanTuning(Config, ConfigError))
	{
		FailWalkRunLeanPresentation(TEXT("Configuration"), ConfigError);
		return;
	}
	const FGGYGOLocomotionSteeringSnapshot& Steering = Frame.LocomotionSteering;
	if (Steering.Status == EGGYGOLocomotionSteeringStatus::Invalid)
	{
		WalkRunLeanSource.Reset();
		WalkRunLeanSourceEpoch = 0;
		FailWalkRunLeanPresentation(TEXT("MovementSnapshot"), Steering.Diagnostic.IsEmpty()
			? FString(TEXT("Movement returned Invalid without a diagnostic")) : Steering.Diagnostic);
		return;
	}
	if (!Steering.OriginalMovement.IsValid() || !Steering.OriginalCharacter.IsValid()
		|| !Steering.OriginalUpdatedComponent.IsValid() || Steering.SourceEpoch == 0)
	{
		WalkRunLeanSource.Reset();
		WalkRunLeanSourceEpoch = 0;
		FailWalkRunLeanPresentation(TEXT("OriginalSource"), TEXT("captured steering result has no live original identity or source epoch"));
		return;
	}
	if (WalkRunLeanSource != Steering.OriginalMovement || WalkRunLeanSourceEpoch != Steering.SourceEpoch)
	{
		ResetWalkRunLeanPresentation();
		WalkRunLeanSource = Steering.OriginalMovement;
		WalkRunLeanSourceEpoch = Steering.SourceEpoch;
	}

	switch (Steering.Status)
	{
	case EGGYGOLocomotionSteeringStatus::Initial:
		WalkRunLeanAngleDegrees = 0.0f;
		bWalkRunLeanPresentationValid = false;
		WalkRunLeanFailureReason.Reset();
		LastWalkRunLeanFailureCode = NAME_None;
		return;
	case EGGYGOLocomotionSteeringStatus::NotApplicable:
		// Movement owns exclusion of Action/TurnBack/no input. This is exit recovery, not a valid zero deviation.
		RecoverWalkRunLeanPresentation(DeltaSeconds, Config);
		return;
	case EGGYGOLocomotionSteeringStatus::Valid:
		if (Steering.CompletedIntervalSerial == 0 || !FMath::IsFinite(Steering.NativeDeltaSeconds)
			|| Steering.NativeDeltaSeconds <= 0.0f)
		{
			FailWalkRunLeanPresentation(TEXT("CompletedInterval"), TEXT("Valid steering requires a completed serial and finite positive native delta"));
			return;
		}
		break;
	default:
		FailWalkRunLeanPresentation(TEXT("SteeringStatus"), TEXT("captured steering status is outside the shared contract"));
		return;
	}

	if (!FMath::IsFinite(Steering.DesiredDirectionError) || Steering.DesiredDirectionError < -180.0f
		|| Steering.DesiredDirectionError > 180.0f || !FMath::IsFinite(Frame.WalkRunBlendAlpha)
		|| Frame.WalkRunBlendAlpha < 0.0f || Frame.WalkRunBlendAlpha > 1.0f)
	{
		FailWalkRunLeanPresentation(TEXT("DirectionValues"),
			TEXT("DesiredDirectionError must be finite in [-180,180] degrees and WalkRun alpha must be in [0,1]"));
		return;
	}

	// The full native signed-angle domain is linear. No yaw-rate gate, saturation or temporal entry filter.
	const float MaxAngle = FMath::Lerp(Config.WalkMaxAngleDegrees, Config.RunMaxAngleDegrees, Frame.WalkRunBlendAlpha);
	WalkRunLeanAngleDegrees = static_cast<float>(static_cast<double>(Steering.DesiredDirectionError)
		/ Config.FullLeanDirectionErrorDegrees * MaxAngle);
	bWalkRunLeanPresentationValid = true;
	WalkRunLeanFailureReason.Reset();
	LastWalkRunLeanFailureCode = NAME_None;
}

void UZZZAnimInstance::RefreshDecisionContext()
{
	// 通用基类已在 Super::NativeUpdateAnimation 中完成唯一一次跨层抓取。
	// 这里只做旧数据面的兼容适配，不再访问 Actor、CMC 或 ASC。
	LegacySnapshotAdapter.Capture(Snap, GetAnimationStateFrame(), GetAnimationDebugFrame());
	AnimBlendX = Snap.AnimBlendX;
	AnimBlendY = Snap.AnimBlendY;
	AnimCurveVelocity = Snap.AnimCurveVelocity;
	AnimCurveVelocityDirection = Snap.AnimCurveVelocityDirection;
	AnimCurveVelocityAngle = Snap.AnimCurveVelocityAngle;
	ActualVelocityDirection = Snap.ActualVelocityDirection;
	ActualVelocityBlendX = Snap.ActualVelocityBlendX;
	ActualVelocityBlendY = Snap.ActualVelocityBlendY;
	ActualVelocityAngle = Snap.ActualVelocityAngle;
	bTurnBackRunOut = Snap.bTurnBackRunOut;

	FZZZAnimWriteContext WriteContext;
	WriteContext.Snap = &Snap;
	WriteContext.Memory = &StateMemory;

	LocomotionEvents.SetContext(WriteContext);
	LocomotionEvents.MapMovementState();

#if !UE_BUILD_SHIPPING
	// TurnBack 诊断。全部字段取自快照，不回头读移动层 ——
	// 快照之外再取一次值，两者可能来自不同时刻，日志就会自相矛盾。
	if (Snap.TurnBackPhase != EGGYGOTurnBackPhase::None)
	{
		UE_LOG(LogZZZAnim, Log,
			TEXT("[TurnBack][Snapshot] Phase=%d RunOut=%d Gait=%d ShouldMove=%d Grounded=%d BlockMove=%d InputForwardDot=%.3f Velocity=%.2f"),
			static_cast<uint8>(Snap.TurnBackPhase),
			Snap.bTurnBackRunOut ? 1 : 0,
			static_cast<uint8>(Snap.Gait),
			Snap.bShouldMove ? 1 : 0,
			Snap.bGrounded ? 1 : 0,
			Snap.bBlockMove ? 1 : 0,
			Snap.InputForwardDot,
			Snap.VelocityLength);
	}
#endif
}

// ============================================================================
// 配表查询
// ============================================================================

UAnimSequence* UZZZAnimInstance::GetSeqByKey(FName Key) const
{
	return AnimSet.Sequences.FindRef(Key);
}

UBlendSpace* UZZZAnimInstance::GetBlendSpaceByKey(FName Key) const
{
	return AnimSet.BlendSpaces.FindRef(Key);
}

void UZZZAnimInstance::ResolveLocomotionSourceBinding(FGGYGOLocomotionSourceBinding& OutBinding) const
{
	check(IsInGameThread());
	OutBinding = {};
	const auto Fail = [this, &OutBinding](EGGYGOLocomotionSourceStatus Status, FName Key, const UObject* Asset, const FString& Reason)
	{
		OutBinding = {};
		OutBinding.Status = Status;
		OutBinding.Error = FString::Printf(TEXT("[Animation][LocomotionSource] Producer=%s Config=AnimSet Key=%s Asset=%s Reason=%s"),
			*GetPathName(), *Key.ToString(), *GetPathNameSafe(Asset), *Reason);
	};

	const EGGYGOLocomotionMotionType SingleMotions[] = {
		EGGYGOLocomotionMotionType::WalkStart, EGGYGOLocomotionMotionType::StartStop,
		EGGYGOLocomotionMotionType::WalkStop, EGGYGOLocomotionMotionType::RunStop,
		EGGYGOLocomotionMotionType::TurnBack
	};
	for (EGGYGOLocomotionMotionType MotionType : SingleMotions)
	{
		const FName Key = GetSingleSourceKey(AnimSet, MotionType);
		UAnimSequence* Sequence = Key.IsNone() ? nullptr : AnimSet.Sequences.FindRef(Key);
		if (Key.IsNone() || !IsValid(Sequence))
		{
			Fail(EGGYGOLocomotionSourceStatus::Missing, Key, Sequence,
				FString::Printf(TEXT("Motion=%d required single source route/sequence is missing"), static_cast<uint8>(MotionType)));
			return;
		}
		if (!FMath::IsFinite(Sequence->RateScale) || Sequence->RateScale <= 0.0f
			|| !FMath::IsFinite(Sequence->GetPlayLength()) || Sequence->GetPlayLength() <= 0.0f)
		{
			Fail(EGGYGOLocomotionSourceStatus::Invalid, Key, Sequence,
				FString::Printf(TEXT("Motion=%d RateScale=%g PlayLength=%g must be finite and positive"),
					static_cast<uint8>(MotionType), Sequence->RateScale, Sequence->GetPlayLength()));
			return;
		}
		FGGYGOLocomotionSequenceSource& Source = OutBinding.SingleSources.AddDefaulted_GetRef();
		Source.MotionType = MotionType;
		Source.RouteKey = Key;
		Source.Sequence = TStrongObjectPtr<UAnimSequence>(Sequence);
		Source.bLoop = false;
		Source.SequenceRateScale = Sequence->RateScale;
		Source.PlayLength = Sequence->GetPlayLength();
	}

	const FName BlendKey = AnimSet.WalkRunSourceKey;
	UBlendSpace* BlendSpace = BlendKey.IsNone() ? nullptr : AnimSet.BlendSpaces.FindRef(BlendKey);
	if (BlendKey.IsNone() || !IsValid(BlendSpace))
	{
		Fail(EGGYGOLocomotionSourceStatus::Missing, BlendKey, BlendSpace, TEXT("required WalkRun route/BlendSpace is missing"));
		return;
	}
	const TArray<FBlendSample>& Samples = BlendSpace->GetBlendSamples();
	if (Samples.IsEmpty())
	{
		Fail(EGGYGOLocomotionSourceStatus::Missing, BlendKey, BlendSpace, TEXT("WalkRun BlendSpace has no source samples"));
		return;
	}
	OutBinding.WalkRunKey = BlendKey;
	OutBinding.WalkRunBlendSpace = TStrongObjectPtr<UBlendSpace>(BlendSpace);
	OutBinding.bWalkRunLoop = true;
	for (int32 Index = 0; Index < Samples.Num(); ++Index)
	{
		const FBlendSample& Sample = Samples[Index];
		UAnimSequence* Sequence = Sample.Animation;
		if (!IsValid(Sequence))
		{
			Fail(EGGYGOLocomotionSourceStatus::Missing, BlendKey, BlendSpace,
				FString::Printf(TEXT("SampleIndex=%d has no valid sequence"), Index));
			return;
		}
		if (!FMath::IsFinite(Sample.SampleValue.X) || !FMath::IsFinite(Sample.SampleValue.Y)
			|| !FMath::IsFinite(Sample.SampleValue.Z) || !FMath::IsFinite(Sample.RateScale) || Sample.RateScale <= 0.0f
			|| !FMath::IsFinite(Sequence->RateScale) || Sequence->RateScale <= 0.0f
			|| !FMath::IsFinite(Sequence->GetPlayLength()) || Sequence->GetPlayLength() <= 0.0f)
		{
			Fail(EGGYGOLocomotionSourceStatus::Invalid, BlendKey, Sequence,
				FString::Printf(TEXT("SampleIndex=%d coordinates=(%g,%g,%g) SampleRateScale=%g SequenceRateScale=%g PlayLength=%g are invalid"),
					Index, Sample.SampleValue.X, Sample.SampleValue.Y, Sample.SampleValue.Z,
					Sample.RateScale, Sequence->RateScale, Sequence->GetPlayLength()));
			return;
		}
		FGGYGOLocomotionBlendSpaceSampleSource& Source = OutBinding.WalkRunSamples.AddDefaulted_GetRef();
		Source.SampleIndex = Index;
		Source.SampleValue = Sample.SampleValue;
		Source.SampleRateScale = Sample.RateScale;
		Source.Source.MotionType = EGGYGOLocomotionMotionType::WalkRun;
		Source.Source.RouteKey = BlendKey;
		Source.Source.Sequence = TStrongObjectPtr<UAnimSequence>(Sequence);
		Source.Source.bLoop = true;
		Source.Source.SequenceRateScale = Sequence->RateScale;
		Source.Source.PlayLength = Sequence->GetPlayLength();
	}
	OutBinding.Status = EGGYGOLocomotionSourceStatus::Available;
}

bool UZZZAnimInstance::IsLocomotionSourceConfigurationCurrent(const FGGYGOLocomotionSourceBinding& Binding) const
{
	check(IsInGameThread());
	if (Binding.Status != EGGYGOLocomotionSourceStatus::Available || Binding.SingleSources.Num() != 5
		|| Binding.WalkRunKey != AnimSet.WalkRunSourceKey || !Binding.bWalkRunLoop)
	{
		return false;
	}
	for (const FGGYGOLocomotionSequenceSource& Source : Binding.SingleSources)
	{
		const FName Key = GetSingleSourceKey(AnimSet, Source.MotionType);
		UAnimSequence* Sequence = Key.IsNone() ? nullptr : AnimSet.Sequences.FindRef(Key);
		if (Source.RouteKey != Key || !IsValid(Sequence) || Source.Sequence.Get() != Sequence || Source.bLoop
			|| Source.SequenceRateScale != Sequence->RateScale || Source.PlayLength != Sequence->GetPlayLength())
		{
			return false;
		}
	}
	UBlendSpace* BlendSpace = AnimSet.BlendSpaces.FindRef(AnimSet.WalkRunSourceKey);
	if (!IsValid(BlendSpace) || Binding.WalkRunBlendSpace.Get() != BlendSpace
		|| Binding.WalkRunSamples.Num() != BlendSpace->GetBlendSamples().Num())
	{
		return false;
	}
	const TArray<FBlendSample>& Samples = BlendSpace->GetBlendSamples();
	for (int32 Index = 0; Index < Samples.Num(); ++Index)
	{
		const FBlendSample& Sample = Samples[Index];
		const FGGYGOLocomotionBlendSpaceSampleSource& Source = Binding.WalkRunSamples[Index];
		UAnimSequence* Sequence = Sample.Animation;
		if (!IsValid(Sequence) || Source.SampleIndex != Index || Source.Source.Sequence.Get() != Sequence
			|| Source.SampleValue.X != Sample.SampleValue.X || Source.SampleValue.Y != Sample.SampleValue.Y
			|| Source.SampleValue.Z != Sample.SampleValue.Z || Source.SampleRateScale != Sample.RateScale
			|| Source.Source.SequenceRateScale != Sequence->RateScale || Source.Source.PlayLength != Sequence->GetPlayLength())
		{
			return false;
		}
	}
	return true;
}
