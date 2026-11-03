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

	bool ValidateLeanResponse(const FRuntimeFloatCurve& Curve, const TCHAR* FieldName, FString& OutError)
	{
		OutError.Reset();
		const FRichCurve* Response = Curve.GetRichCurveConst();
		if (!Response || Response->GetNumKeys() < 2)
		{
			OutError = FString::Printf(TEXT("%s must contain an authored response covering [0,180] camera direction degrees"), FieldName);
			return false;
		}
		float FirstInput = 0.0f;
		float LastInput = 0.0f;
		Response->GetTimeRange(FirstInput, LastInput);
		if (FirstInput != 0.0f || LastInput != 180.0f || Response->Eval(0.0f) != 0.0f)
		{
			OutError = FString::Printf(TEXT("%s must cover [0,180] exactly and map zero deviation to zero lean"), FieldName);
			return false;
		}
		for (const FRichCurveKey& Key : Response->GetConstRefOfKeys())
		{
			if (!FMath::IsFinite(Key.Time) || !FMath::IsFinite(Key.Value)
				|| Key.Value < 0.0f || Key.Value >= 90.0f
				|| !FMath::IsFinite(Key.ArriveTangent) || !FMath::IsFinite(Key.LeaveTangent)
				|| !FMath::IsFinite(Key.ArriveTangentWeight) || !FMath::IsFinite(Key.LeaveTangentWeight))
			{
				OutError = FString::Printf(TEXT("%s keys/tangents must be finite with lean in [0,90) degrees"), FieldName);
				return false;
			}
		}
		return true;
	}

	bool ValidateWalkRunLeanTuning(const FZZZWalkRunLeanTuning& Config, FString& OutError)
	{
		return ValidateLeanResponse(Config.WalkDirectionErrorToLeanDegrees, TEXT("WalkDirectionErrorToLeanDegrees"), OutError)
			&& ValidateLeanResponse(Config.RunDirectionErrorToLeanDegrees, TEXT("RunDirectionErrorToLeanDegrees"), OutError);
	}

}

// ============================================================================
// AnimInstance 生命周期
// ============================================================================

void UZZZAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
	Super::NativeUpdateAnimation(DeltaSeconds);
	RefreshDecisionContext();
	UpdateWalkRunLeanTarget();
}

bool UZZZAnimInstance::TryGetWalkRunLeanGait(EGGYGOGait& OutGait, float& OutBlendAlpha) const
{
	OutGait = EGGYGOGait::None;
	OutBlendAlpha = 0.0f;
	if (!bWalkRunLeanPresentationValid || !bWalkRunLeanTargetApplicable) { return false; }
	// 双资格由本次目标求值发布；图与目标只读同一捕获帧，不再读取独立复制的步态对。
	const FGGYGOLocomotionSteeringSnapshot& Steering = GetAnimationStateFrame().LocomotionSteering;
	OutGait = Steering.CameraTurnGait;
	OutBlendAlpha = Steering.CameraTurnWalkRunBlendAlpha;
	return true;
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
	ResetWalkRunLeanTarget();
}

void UZZZAnimInstance::ResetWalkRunLeanTarget()
{
	WalkRunLeanAngleDegrees = 0.0f;
	WalkRunLeanAxisCS = FVector::ZeroVector;
	WalkRunLeanUpCS = FVector::ZeroVector;
	bWalkRunLeanPresentationValid = false;
	bWalkRunLeanTargetApplicable = false;
	WalkRunLeanFailureReason.Reset();
	LastWalkRunLeanFailureCode = NAME_None;
}

void UZZZAnimInstance::FailWalkRunLeanPresentation(FName FailureCode, const FString& Reason)
{
	WalkRunLeanAngleDegrees = 0.0f;
	WalkRunLeanAxisCS = FVector::ZeroVector;
	WalkRunLeanUpCS = FVector::ZeroVector;
	bWalkRunLeanPresentationValid = false;
	bWalkRunLeanTargetApplicable = false;
	WalkRunLeanFailureReason = Reason;
	if (LastWalkRunLeanFailureCode != FailureCode)
	{
		UE_LOG(LogZZZAnim, Error,
			TEXT("[Animation][WalkRunLean] Instance=%s Mesh=%s Config=Tuning.WalkRunLean Code=%s Reason=%s"),
			*GetPathName(), *GetPathNameSafe(GetSkelMeshComponent()), *FailureCode.ToString(), *Reason);
		LastWalkRunLeanFailureCode = FailureCode;
	}
}

void UZZZAnimInstance::UpdateWalkRunLeanTarget()
{
	check(IsInGameThread());
	const FZZZWalkRunLeanTuning& Config = Tuning.WalkRunLean;
	const FGGYGOAnimationStateFrame& Frame = GetAnimationStateFrame();
	if (!Config.bEnabled || !Frame.bLocomotionSteeringCaptured)
	{
		// Explicit disabled mode, or legal native initialization before a Character exists.
		ResetWalkRunLeanTarget();
		return;
	}

	FString ConfigError;
	if (!ValidateWalkRunLeanTuning(Config, ConfigError))
	{
		FailWalkRunLeanPresentation(TEXT("Configuration"), ConfigError);
		return;
	}
	const FGGYGOLocomotionSteeringSnapshot& Steering = Frame.LocomotionSteering;
	if (!Steering.bCameraTurnRemote && Steering.Status == EGGYGOLocomotionSteeringStatus::Invalid)
	{
		FailWalkRunLeanPresentation(TEXT("MovementSnapshot"), Steering.Diagnostic.IsEmpty()
			? FString(TEXT("Movement returned Invalid without a diagnostic")) : Steering.Diagnostic);
		return;
	}
	if (!Steering.OriginalMovement.IsValid() || !Steering.OriginalCharacter.IsValid()
		|| !Steering.OriginalUpdatedComponent.IsValid() || (!Steering.bCameraTurnRemote && Steering.SourceEpoch == 0))
	{
		FailWalkRunLeanPresentation(TEXT("OriginalSource"), TEXT("captured steering result has no live original identity or source epoch"));
		return;
	}
	if (!Frame.bCapsuleMeshBasisValid || Frame.LeanAxisCS.ContainsNaN() || Frame.CapsuleUpCS.ContainsNaN())
	{
		FailWalkRunLeanPresentation(TEXT("PoseBasis"), TEXT("the original capsule and mesh have no valid captured pose basis"));
		return;
	}
	WalkRunLeanAxisCS = Frame.LeanAxisCS;
	WalkRunLeanUpCS = Frame.CapsuleUpCS;

	// 远端 camera 资格来自同一 accepted native 样本，不冒充本机普通转向完成区间。
	switch (Steering.CameraTurnStatus)
	{
	case EGGYGOLocomotionSteeringStatus::Initial:
		WalkRunLeanAngleDegrees = 0.0f;
		bWalkRunLeanPresentationValid = false;
		bWalkRunLeanTargetApplicable = false;
		WalkRunLeanFailureReason.Reset();
		LastWalkRunLeanFailureCode = NAME_None;
		return;
	case EGGYGOLocomotionSteeringStatus::NotApplicable:
		// 不适用是独立事实，不伪装成有效方向零观测；图持有显示角及恢复策略。
		WalkRunLeanAngleDegrees = 0.0f;
		bWalkRunLeanTargetApplicable = false;
		bWalkRunLeanPresentationValid = true;
		WalkRunLeanFailureReason.Reset();
		LastWalkRunLeanFailureCode = NAME_None;
		return;
	case EGGYGOLocomotionSteeringStatus::Valid:
		if (Steering.bCameraTurnRemote)
		{
			if (Steering.CameraTurnPublisherEpoch == 0 || Steering.CameraTurnPublicationSerial == 0
				|| !FMath::IsFinite(Steering.CameraTurnNativeSampleKey))
			{
				FailWalkRunLeanPresentation(TEXT("CameraTurnRemoteSource"), TEXT("Valid remote camera turn requires its accepted publisher/publication identity and finite native sample key"));
				return;
			}
		}
		else if (Steering.Status != EGGYGOLocomotionSteeringStatus::Valid || Steering.CompletedIntervalSerial == 0
			|| !FMath::IsFinite(Steering.NativeDeltaSeconds) || Steering.NativeDeltaSeconds <= 0.0f)
		{
			FailWalkRunLeanPresentation(TEXT("CompletedInterval"), TEXT("Valid local camera turn requires the original completed steering interval and finite positive native delta"));
			return;
		}
		break;
	case EGGYGOLocomotionSteeringStatus::Invalid:
		FailWalkRunLeanPresentation(TEXT("CameraTurnSnapshot"), Steering.CameraTurnDiagnostic.IsEmpty()
			? FString(TEXT("Movement returned Invalid camera turn without a diagnostic")) : Steering.CameraTurnDiagnostic);
		return;
	default:
		FailWalkRunLeanPresentation(TEXT("SteeringStatus"), TEXT("captured steering or camera turn status is outside the shared contract"));
		return;
	}

	if (!FMath::IsFinite(Steering.CameraTurnDemandDegrees) || Steering.CameraTurnDemandDegrees < -180.0f
		|| Steering.CameraTurnDemandDegrees > 180.0f)
	{
		FailWalkRunLeanPresentation(TEXT("DirectionValues"),
			TEXT("CameraTurnDemandDegrees must be finite in [-180,180] degrees"));
		return;
	}

	if (!FMath::IsFinite(Steering.CameraTurnWalkRunBlendAlpha) || Steering.CameraTurnWalkRunBlendAlpha < 0.0f
		|| Steering.CameraTurnWalkRunBlendAlpha > 1.0f)
	{
		FailWalkRunLeanPresentation(TEXT("CameraTurnGait"), TEXT("CameraTurnWalkRunBlendAlpha must be the same camera record's finite [0,1] gait alpha"));
		return;
	}
	const FRuntimeFloatCurve* Response = nullptr;
	switch (Steering.CameraTurnGait)
	{
	case EGGYGOGait::Walk: Response = &Config.WalkDirectionErrorToLeanDegrees; break;
	case EGGYGOGait::Run: Response = &Config.RunDirectionErrorToLeanDegrees; break;
	default:
		FailWalkRunLeanPresentation(TEXT("TargetGait"), TEXT("Valid camera steering requires the same camera record's target gait Walk or Run"));
		return;
	}
	// 只发布目标步态端点；从当前显示角开始的步态过渡由 AnimBP 唯一持有。
	const float Magnitude = Response->GetRichCurveConst()->Eval(FMath::Abs(Steering.CameraTurnDemandDegrees));
	if (!FMath::IsFinite(Magnitude) || Magnitude < 0.0f || Magnitude >= 90.0f)
	{
		FailWalkRunLeanPresentation(TEXT("ResponseValue"), TEXT("the authored response must evaluate to a finite lean in [0,90) degrees"));
		return;
	}
	WalkRunLeanAngleDegrees = Steering.CameraTurnDemandDegrees < 0.0f ? -Magnitude : Magnitude;
	bWalkRunLeanTargetApplicable = true;
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
