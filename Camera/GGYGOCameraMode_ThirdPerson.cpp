/**
 * @file GGYGOCameraMode_ThirdPerson.cpp
 * @brief 第三人称相机实现
 */
#include "Camera/GGYGOCameraMode_ThirdPerson.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Character/Data/GGYGOLocomotionSteeringTypes.h"
#include "GameFramework/Character.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOCameraMode_ThirdPerson)

UGGYGOCameraMode_ThirdPerson::UGGYGOCameraMode_ThirdPerson()
{
	// 动作游戏的默认视野比射击游戏宽一些，便于看清周围的敌人位置。
	FieldOfView = 85.0f;

	// 俯仰上限收紧：完全俯视会让角色被自己遮住，完全仰视则看不到地面敌人。
	ViewPitchMin = -70.0f;
	ViewPitchMax = 70.0f;
}

void UGGYGOCameraMode_ThirdPerson::OnActivation()
{
	Super::OnActivation();
	ResetSteeringPresentation();
}

void UGGYGOCameraMode_ThirdPerson::OnDeactivation()
{
	ResetSteeringPresentation();
	Super::OnDeactivation();
}

void UGGYGOCameraMode_ThirdPerson::ResetSteeringPresentation()
{
	SteeringLateralOffset = 0.0f;
	ObservedSteeringMovement.Reset();
	ObservedSteeringSourceEpoch = 0;
}

FGGYGOCameraEvaluationResult UGGYGOCameraMode_ThirdPerson::SteeringResponseFailure(const TCHAR* Reason) const
{
	const FString CurveSource = SteeringOffsetResponse.ExternalCurve
		? SteeringOffsetResponse.ExternalCurve->GetPathName() : GetClass()->GetPathName() + TEXT(".SteeringOffsetResponse[inline]");
	const FString Detail = FString::Printf(TEXT("%s Curve=[%s]"), Reason, *CurveSource);
	return FGGYGOCameraEvaluationResult::Failure(this, TEXT("SteeringOffsetResponse"), *Detail);
}

FGGYGOCameraEvaluationResult UGGYGOCameraMode_ThirdPerson::ValidateModeConfiguration() const
{
	if (!FMath::IsFinite(TargetOffset.X))
	{
		return FGGYGOCameraEvaluationResult::Failure(this, TEXT("TargetOffset.X"), TEXT("non-finite"));
	}
	if (!FMath::IsFinite(TargetOffset.Y))
	{
		return FGGYGOCameraEvaluationResult::Failure(this, TEXT("TargetOffset.Y"), TEXT("non-finite"));
	}
	if (!FMath::IsFinite(TargetOffset.Z))
	{
		return FGGYGOCameraEvaluationResult::Failure(this, TEXT("TargetOffset.Z"), TEXT("non-finite"));
	}
	if (bEnableWalkRunSteeringOffset)
	{
		const struct { FName Field; float Value; } Parameters[] = {
			{ TEXT("SteeringOffsetAmplitude"), SteeringOffsetAmplitude },
			{ TEXT("SteeringOffsetMaxDistance"), SteeringOffsetMaxDistance },
			{ TEXT("WalkSteeringOffsetScale"), WalkSteeringOffsetScale },
			{ TEXT("RunSteeringOffsetScale"), RunSteeringOffsetScale },
			{ TEXT("SteeringOffsetEnterSpeed"), SteeringOffsetEnterSpeed },
			{ TEXT("SteeringOffsetReturnSpeed"), SteeringOffsetReturnSpeed }
		};
		for (const auto& Parameter : Parameters)
		{
			if (!FMath::IsFinite(Parameter.Value) || Parameter.Value < 0.0f)
			{
				return FGGYGOCameraEvaluationResult::Failure(this, Parameter.Field,
					FMath::IsFinite(Parameter.Value) ? TEXT("negative") : TEXT("non-finite"));
			}
		}
		if (WalkSteeringOffsetScale > RunSteeringOffsetScale)
		{
			return FGGYGOCameraEvaluationResult::Failure(this, TEXT("WalkSteeringOffsetScale/RunSteeringOffsetScale"),
				TEXT("walk-gain-exceeds-run-gain"));
		}
		if (SteeringOffsetResponse.ExternalCurve && !IsValid(SteeringOffsetResponse.ExternalCurve))
		{
			return SteeringResponseFailure(TEXT("invalid-external-response-asset"));
		}
		const FRichCurve* Response = SteeringOffsetResponse.GetRichCurveConst();
		if (!Response || Response->GetNumKeys() < 2)
		{
			return SteeringResponseFailure(TEXT("requires-authored-response-keys"));
		}
		if (Response->PreInfinityExtrap.GetValue() > RCCE_None || Response->PostInfinityExtrap.GetValue() > RCCE_None)
		{
			return SteeringResponseFailure(TEXT("unknown-response-extrapolation"));
		}
		const TArray<FRichCurveKey>& Keys = Response->GetConstRefOfKeys();
		if (Keys[0].Time != 0.0f || Keys[0].Value != 0.0f)
		{
			return SteeringResponseFailure(TEXT("must-start-at-zero-rate-zero-response"));
		}
		float PreviousTime = -1.0f;
		for (const FRichCurveKey& Key : Keys)
		{
			if ((Key.InterpMode != RCIM_Constant && Key.InterpMode != RCIM_Linear && Key.InterpMode != RCIM_Cubic)
				|| Key.TangentWeightMode.GetValue() > RCTWM_WeightedBoth
				|| !FMath::IsFinite(Key.Time) || Key.Time <= PreviousTime
				|| !FMath::IsFinite(Key.Value) || Key.Value < 0.0f || Key.Value > 1.0f
				|| !FMath::IsFinite(Key.ArriveTangent) || !FMath::IsFinite(Key.LeaveTangent)
				|| !FMath::IsFinite(Key.ArriveTangentWeight) || Key.ArriveTangentWeight < 0.0f
				|| !FMath::IsFinite(Key.LeaveTangentWeight) || Key.LeaveTangentWeight < 0.0f)
			{
				return SteeringResponseFailure(TEXT("invalid-response-key"));
			}
			PreviousTime = Key.Time;
		}
	}
	// Disabled prevention intentionally does not use either collision parameter.
	if (bPreventPenetration)
	{
		if (!FMath::IsFinite(PenetrationProbeRadius) || PenetrationProbeRadius < 0.0f)
		{
			return FGGYGOCameraEvaluationResult::Failure(this, TEXT("PenetrationProbeRadius"),
				FMath::IsFinite(PenetrationProbeRadius) ? TEXT("negative") : TEXT("non-finite"));
		}
		if (!FMath::IsFinite(PenetrationRecoverySpeed) || PenetrationRecoverySpeed < 0.0f)
		{
			return FGGYGOCameraEvaluationResult::Failure(this, TEXT("PenetrationRecoverySpeed"),
				FMath::IsFinite(PenetrationRecoverySpeed) ? TEXT("negative") : TEXT("non-finite"));
		}
	}
	return FGGYGOCameraEvaluationResult::Success();
}

FGGYGOCameraEvaluationResult UGGYGOCameraMode_ThirdPerson::ComputeSteeringLateralTarget(
	float ActualSignedTrajectoryYawRate, const FVector& WorldVelocityDirection, float WalkRunBlendAlpha,
	const FRotator& CameraRotation, float& OutLateralTarget) const
{
	if (!FMath::IsFinite(ActualSignedTrajectoryYawRate) || !FMath::IsFinite(WalkRunBlendAlpha)
		|| WalkRunBlendAlpha < 0.0f || WalkRunBlendAlpha > 1.0f
		|| WorldVelocityDirection.ContainsNaN() || CameraRotation.ContainsNaN())
	{
		return FGGYGOCameraEvaluationResult::Failure(this, TEXT("SteeringSample"), TEXT("invalid-composition-input"));
	}
	if (ActualSignedTrajectoryYawRate == 0.0f)
	{
		OutLateralTarget = 0.0f;
		return FGGYGOCameraEvaluationResult::Success();
	}
	const double DirectionSizeSquared = WorldVelocityDirection.SizeSquared2D();
	if (!FMath::IsNearlyZero(WorldVelocityDirection.Z) || !FMath::IsNearlyEqual(DirectionSizeSquared, 1.0, 0.001))
	{
		return FGGYGOCameraEvaluationResult::Failure(this, TEXT("SteeringSample.WorldVelocityDirection"),
			TEXT("requires-horizontal-unit-direction-for-nonzero-yaw-rate"));
	}
	const FRichCurve* ResponseCurve = SteeringOffsetResponse.GetRichCurveConst();
	if (!ResponseCurve || ResponseCurve->GetNumKeys() < 2)
	{
		return SteeringResponseFailure(TEXT("requires-authored-response-keys"));
	}
	const float Response = ResponseCurve->Eval(FMath::Abs(ActualSignedTrajectoryYawRate));
	if (!FMath::IsFinite(Response) || Response < 0.0f || Response > 1.0f)
	{
		return SteeringResponseFailure(TEXT("evaluated-response-outside-[0,1]"));
	}
	const double WorldAmplitude = static_cast<double>(SteeringOffsetAmplitude) * Response
		* FMath::Lerp(WalkSteeringOffsetScale, RunSteeringOffsetScale, WalkRunBlendAlpha);
	if (!FMath::IsFinite(WorldAmplitude))
	{
		return FGGYGOCameraEvaluationResult::Failure(this, TEXT("SteeringOffsetAmplitude"), TEXT("non-finite-composed-amplitude"));
	}
	// UE 的正 Yaw 向右；真实右弯的外侧在实际速度左侧，不用胶囊修正方向猜弯道。
	const FVector VelocityDirection = WorldVelocityDirection / FMath::Sqrt(DirectionSizeSquared);
	const FVector WorldOutside = FVector::CrossProduct(FVector::UpVector, VelocityDirection)
		* -FMath::Sign(ActualSignedTrajectoryYawRate);
	// 构图投影需要保持正交：Win64 RotationMatrix 的向量路径使用 float 度数因子，
	// 即使矩阵为 double，90 度仍会留下可观测残差。这里只用 double 标量计算原右轴，
	// 不改变原相机旋转、视图生成或曲线语义，也不对侧视角度特判归零。
	double SinPitch, CosPitch, SinYaw, CosYaw, SinRoll, CosRoll;
	FMath::SinCos(&SinPitch, &CosPitch, FMath::DegreesToRadians(CameraRotation.Pitch));
	FMath::SinCos(&SinYaw, &CosYaw, FMath::DegreesToRadians(CameraRotation.Yaw));
	FMath::SinCos(&SinRoll, &CosRoll, FMath::DegreesToRadians(CameraRotation.Roll));
	const FVector CameraRight(SinRoll * SinPitch * CosYaw - CosRoll * SinYaw,
		SinRoll * SinPitch * SinYaw + CosRoll * CosYaw, -SinRoll * CosPitch);
	const float CandidateTarget = static_cast<float>(FMath::Min(WorldAmplitude, static_cast<double>(SteeringOffsetMaxDistance))
		* FVector::DotProduct(WorldOutside, CameraRight));
	if (!FMath::IsFinite(CandidateTarget))
	{
		return FGGYGOCameraEvaluationResult::Failure(this, TEXT("SteeringPresentation"), TEXT("non-finite-projected-target"));
	}
	OutLateralTarget = CandidateTarget;
	return FGGYGOCameraEvaluationResult::Success();
}

FGGYGOCameraEvaluationResult UGGYGOCameraMode_ThirdPerson::UpdateSteeringPresentation(float DeltaTime, float LateralTarget)
{
	if (!FMath::IsFinite(DeltaTime) || DeltaTime < 0.0f || !FMath::IsFinite(LateralTarget)
		|| !FMath::IsFinite(SteeringLateralOffset))
	{
		return FGGYGOCameraEvaluationResult::Failure(this, TEXT("SteeringPresentation"), TEXT("invalid-filter-input"));
	}
	const float Speed = FMath::Abs(LateralTarget) < FMath::Abs(SteeringLateralOffset)
		? SteeringOffsetReturnSpeed : SteeringOffsetEnterSpeed;
	const float Candidate = FMath::FInterpTo(SteeringLateralOffset, LateralTarget, DeltaTime, Speed);
	if (!FMath::IsFinite(Candidate))
	{
		return FGGYGOCameraEvaluationResult::Failure(this, TEXT("SteeringPresentation"), TEXT("non-finite-filter-result"));
	}
	SteeringLateralOffset = Candidate;
	return FGGYGOCameraEvaluationResult::Success();
}

FGGYGOCameraEvaluationResult UGGYGOCameraMode_ThirdPerson::UpdateSteeringFromMovement(
	float DeltaTime, const FRotator& CameraRotation)
{
	if (!bEnableWalkRunSteeringOffset)
	{
		ResetSteeringPresentation();
		return FGGYGOCameraEvaluationResult::Success();
	}
	ACharacter* Character = Cast<ACharacter>(GetTargetActor());
	UGGYGOCharacterMovementComponent* Movement = Character
		? Cast<UGGYGOCharacterMovementComponent>(Character->GetCharacterMovement()) : nullptr;
	const auto SourceFailure = [this, Movement](FName Field, const TCHAR* Reason)
	{
		ResetSteeringPresentation();
		const FString Detail = FString::Printf(TEXT("Target=[%s] Movement=[%s] Reason=[%s]"),
			*GetPathNameSafe(GetTargetActor()), *GetPathNameSafe(Movement), Reason);
		return FGGYGOCameraEvaluationResult::Failure(this, Field, *Detail);
	};
	if (!IsValid(Character) || !IsValid(Movement))
	{
		return SourceFailure(TEXT("SteeringSource"), TEXT("requires-original-character-movement-component"));
	}

	// GT 同一次抓取，期间没有外部回调；资格、区间有效期和来源换代仅由 CMC 解释。
	const FGGYGOLocomotionSteeringSnapshot Snapshot = Movement->GetLocomotionSteeringSnapshot();
	if (Snapshot.Status == EGGYGOLocomotionSteeringStatus::Invalid)
	{
		return SourceFailure(TEXT("SteeringSnapshot"), Snapshot.Diagnostic.IsEmpty()
			? TEXT("invalid-snapshot-without-diagnostic") : *Snapshot.Diagnostic);
	}
	if (Snapshot.OriginalMovement.Get() != Movement || Snapshot.OriginalCharacter.Get() != Character
		|| !Snapshot.OriginalUpdatedComponent.IsValid())
	{
		return SourceFailure(TEXT("SteeringSnapshot.Identity"), TEXT("snapshot-source-does-not-match-original-target"));
	}
	if (ObservedSteeringMovement.Get() != Movement || ObservedSteeringSourceEpoch != Snapshot.SourceEpoch)
	{
		ResetSteeringPresentation();
		ObservedSteeringMovement = Movement;
		ObservedSteeringSourceEpoch = Snapshot.SourceEpoch;
	}

	float LateralTarget = 0.0f;
	switch (Snapshot.Status)
	{
	case EGGYGOLocomotionSteeringStatus::Initial:
		// 首次 Ready 可以早于原 native move；保初始构图，不制造零角速度观测。
		SteeringLateralOffset = 0.0f;
		return FGGYGOCameraEvaluationResult::Success();
	case EGGYGOLocomotionSteeringStatus::NotApplicable:
		break; // 接管、混合或正常过期，只回收本模式展示量。
	case EGGYGOLocomotionSteeringStatus::Valid:
		if (Snapshot.bHasVelocityYawRate)
		{
			// 真实轨迹的转向方向可能与胶囊追转相反，绝不拿胶囊 Yawrate 替代。
			const FGGYGOCameraEvaluationResult Result = ComputeSteeringLateralTarget(
				Snapshot.ActualSignedVelocityYawRate, Movement->GetHorizontalVelocityDirection(),
				Movement->GetWalkRunBlendAlpha(), CameraRotation, LateralTarget);
			if (!Result.IsSuccess())
			{
				ResetSteeringPresentation();
				return Result;
			}
		}
		break; // 无合法速度两端不是零角速度样本，正常回收。
	default:
		return SourceFailure(TEXT("SteeringSnapshot.Status"), TEXT("unknown-steering-status"));
	}
	return UpdateSteeringPresentation(DeltaTime, LateralTarget);
}

FGGYGOCameraEvaluationResult UGGYGOCameraMode_ThirdPerson::UpdateView(float DeltaTime)
{
	const FVector PivotLocation = GetPivotLocation();
	FRotator PivotRotation = GetPivotRotation();

	PivotRotation.Pitch = FMath::ClampAngle(PivotRotation.Pitch, ViewPitchMin, ViewPitchMax);
	const FGGYGOCameraEvaluationResult SteeringResult = UpdateSteeringFromMovement(DeltaTime, PivotRotation);
	if (!SteeringResult.IsSuccess())
	{
		return SteeringResult;
	}

	View.Rotation = PivotRotation;
	View.ControlRotation = View.Rotation;
	View.FieldOfView = FieldOfView;

	// 偏移在**视线空间**而不是角色局部空间里应用：镜头应当始终在视线后方，
	// 与角色自身朝向无关。用角色朝向会让原地转身时镜头绕着角色转。
	FVector ComposedTargetOffset = TargetOffset;
	ComposedTargetOffset.Y += SteeringLateralOffset;
	const FVector DesiredOffset = PivotRotation.RotateVector(ComposedTargetOffset);
	const FVector DesiredLocation = PivotLocation + DesiredOffset;
	View.Location = DesiredLocation;

	CameraPenetrationRequest.bEnabled = bPreventPenetration;
	if (bPreventPenetration)
	{
		CameraPenetrationRequest.PivotLocation = PivotLocation;
		CameraPenetrationRequest.ProbeRadius = PenetrationProbeRadius;
		CameraPenetrationRequest.RecoverySpeed = PenetrationRecoverySpeed;
	}
	return FGGYGOCameraEvaluationResult::Success();
}
