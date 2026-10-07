/**
 * @file GGYGOCameraMode.cpp
 * @brief 相机模式与模式栈实现
 */
#include "Camera/GGYGOCameraMode.h"

#include "Camera/GGYGOCameraComponent.h"
#include "GameFramework/Actor.h"
#include "UObject/StrongObjectPtr.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOCameraMode)

FGGYGOCameraEvaluationResult FGGYGOCameraEvaluationResult::Success()
{
	FGGYGOCameraEvaluationResult Result;
	Result.Status = EGGYGOCameraEvaluationStatus::Success;
	return Result;
}

FGGYGOCameraEvaluationResult FGGYGOCameraEvaluationResult::Failure(
	const UGGYGOCameraMode* OriginalMode, FName InvalidField, const TCHAR* InvalidReason)
{
	FGGYGOCameraEvaluationResult Result;
	// Weak provenance is identity only; configuration validation never mutates the const source.
	Result.Mode = const_cast<UGGYGOCameraMode*>(OriginalMode);
	Result.ModePath = GetPathNameSafe(OriginalMode);
	Result.ModeClassPath = GetPathNameSafe(OriginalMode ? OriginalMode->GetClass() : nullptr);
	Result.Field = InvalidField;
	Result.Reason = InvalidReason;
	return Result;
}

namespace
{
	struct FCameraFiniteField
	{
		double Value;
		const TCHAR* Name;
	};

	FGGYGOCameraEvaluationResult ValidateCameraView(
		UGGYGOCameraMode* OriginalMode, const FGGYGOCameraModeView& View)
	{
		const FCameraFiniteField Fields[] =
		{
			{ View.Location.X, TEXT("View.Location.X") },
			{ View.Location.Y, TEXT("View.Location.Y") },
			{ View.Location.Z, TEXT("View.Location.Z") },
			{ View.Rotation.Pitch, TEXT("View.Rotation.Pitch") },
			{ View.Rotation.Yaw, TEXT("View.Rotation.Yaw") },
			{ View.Rotation.Roll, TEXT("View.Rotation.Roll") },
			{ View.ControlRotation.Pitch, TEXT("View.ControlRotation.Pitch") },
			{ View.ControlRotation.Yaw, TEXT("View.ControlRotation.Yaw") },
			{ View.ControlRotation.Roll, TEXT("View.ControlRotation.Roll") },
			{ View.FieldOfView, TEXT("View.FieldOfView") }
		};
		for (const FCameraFiniteField& Field : Fields)
		{
			if (!FMath::IsFinite(Field.Value))
			{
				return FGGYGOCameraEvaluationResult::Failure(OriginalMode, FName(Field.Name), TEXT("non-finite"));
			}
		}
		return FGGYGOCameraEvaluationResult::Success();
	}

	FGGYGOCameraEvaluationResult ValidateCameraBlendArithmetic(
		UGGYGOCameraMode* OriginalMode, const FGGYGOCameraModeView& Base,
		const FGGYGOCameraModeView& Other, float Weight)
	{
		// Check scalars before FVector/FRotator diagnostics can replace a non-finite result.
		const auto IsFiniteLerp = [Weight](auto A, auto B)
		{
			const auto Difference = B - A;
			const auto WeightedDifference = Weight * Difference;
			return FMath::IsFinite(Difference) && FMath::IsFinite(WeightedDifference)
				&& FMath::IsFinite(A + WeightedDifference);
		};
		if (!IsFiniteLerp(Base.Location.X, Other.Location.X))
		{
			return FGGYGOCameraEvaluationResult::Failure(OriginalMode, TEXT("View.Location.X"), TEXT("non-finite-blend-arithmetic"));
		}
		if (!IsFiniteLerp(Base.Location.Y, Other.Location.Y))
		{
			return FGGYGOCameraEvaluationResult::Failure(OriginalMode, TEXT("View.Location.Y"), TEXT("non-finite-blend-arithmetic"));
		}
		if (!IsFiniteLerp(Base.Location.Z, Other.Location.Z))
		{
			return FGGYGOCameraEvaluationResult::Failure(OriginalMode, TEXT("View.Location.Z"), TEXT("non-finite-blend-arithmetic"));
		}
		const FCameraFiniteField RotationDifferences[] =
		{
			{ Other.Rotation.Pitch - Base.Rotation.Pitch, TEXT("View.Rotation.Pitch") },
			{ Other.Rotation.Yaw - Base.Rotation.Yaw, TEXT("View.Rotation.Yaw") },
			{ Other.Rotation.Roll - Base.Rotation.Roll, TEXT("View.Rotation.Roll") },
			{ Other.ControlRotation.Pitch - Base.ControlRotation.Pitch, TEXT("View.ControlRotation.Pitch") },
			{ Other.ControlRotation.Yaw - Base.ControlRotation.Yaw, TEXT("View.ControlRotation.Yaw") },
			{ Other.ControlRotation.Roll - Base.ControlRotation.Roll, TEXT("View.ControlRotation.Roll") }
		};
		for (const FCameraFiniteField& Field : RotationDifferences)
		{
			if (!FMath::IsFinite(Field.Value))
			{
				return FGGYGOCameraEvaluationResult::Failure(OriginalMode, FName(Field.Name), TEXT("non-finite-blend-arithmetic"));
			}
		}
		if (!IsFiniteLerp(Base.FieldOfView, Other.FieldOfView))
		{
			return FGGYGOCameraEvaluationResult::Failure(OriginalMode, TEXT("View.FieldOfView"), TEXT("non-finite-blend-arithmetic"));
		}
		return FGGYGOCameraEvaluationResult::Success();
	}
}

// ============================================================================
// FGGYGOCameraModeView
// ============================================================================

FGGYGOCameraModeView::FGGYGOCameraModeView()
	: Location(ForceInit)
	, Rotation(ForceInit)
	, ControlRotation(ForceInit)
	, FieldOfView(80.0f)
{
}

void FGGYGOCameraModeView::Blend(const FGGYGOCameraModeView& Other, float OtherWeight)
{
	if (OtherWeight <= 0.0f)
	{
		return;
	}

	if (OtherWeight >= 1.0f)
	{
		*this = Other;
		return;
	}

	Location = FMath::Lerp(Location, Other.Location, OtherWeight);

	// 旋转分量先归一化到 [-180, 180] 再插值。
	// 直接对原始欧拉角插值会在跨越 ±180 度时绕远路，表现为镜头猛甩一圈。
	const FRotator DeltaRotation = (Other.Rotation - Rotation).GetNormalized();
	Rotation = Rotation + (OtherWeight * DeltaRotation);

	const FRotator DeltaControlRotation = (Other.ControlRotation - ControlRotation).GetNormalized();
	ControlRotation = ControlRotation + (OtherWeight * DeltaControlRotation);

	FieldOfView = FMath::Lerp(FieldOfView, Other.FieldOfView, OtherWeight);
}

// ============================================================================
// UGGYGOCameraMode
// ============================================================================

UGGYGOCameraMode::UGGYGOCameraMode()
{
}

UGGYGOCameraComponent* UGGYGOCameraMode::GetGGYGOCameraComponent() const
{
	// Outer 必然是相机组件：模式实例只由 UGGYGOCameraModeStack 创建，
	// 而栈的 Outer 是相机组件。
	return CastChecked<UGGYGOCameraComponent>(GetOuter()->GetOuter());
}

AActor* UGGYGOCameraMode::GetTargetActor() const
{
	const UGGYGOCameraComponent* CameraComponent = GetGGYGOCameraComponent();

	return CameraComponent ? CameraComponent->GetTargetActor() : nullptr;
}

FVector UGGYGOCameraMode::GetPivotLocation() const
{
	const AActor* TargetActor = GetTargetActor();
	if (!TargetActor)
	{
		return FVector::ZeroVector;
	}

	// 用 GetPawnViewLocation 而不是 Actor 原点：前者已经算进了眼高，
	// 用原点会让镜头从脚底出发。
	if (const APawn* TargetPawn = Cast<APawn>(TargetActor))
	{
		return TargetPawn->GetPawnViewLocation();
	}

	return TargetActor->GetActorLocation();
}

FRotator UGGYGOCameraMode::GetPivotRotation() const
{
	const AActor* TargetActor = GetTargetActor();
	if (!TargetActor)
	{
		return FRotator::ZeroRotator;
	}

	if (const APawn* TargetPawn = Cast<APawn>(TargetActor))
	{
		return TargetPawn->GetViewRotation();
	}

	return TargetActor->GetActorRotation();
}

FGGYGOCameraEvaluationResult UGGYGOCameraMode::UpdateView(float DeltaTime)
{
	const FVector PivotLocation = GetPivotLocation();
	FRotator PivotRotation = GetPivotRotation();

	// 钳制俯仰。不钳会让镜头翻过头顶，此后左右方向感反转。
	PivotRotation.Pitch = FMath::ClampAngle(PivotRotation.Pitch, ViewPitchMin, ViewPitchMax);

	View.Location = PivotLocation;
	View.Rotation = PivotRotation;
	View.ControlRotation = View.Rotation;
	View.FieldOfView = FieldOfView;
	return FGGYGOCameraEvaluationResult::Success();
}

FGGYGOCameraEvaluationResult UGGYGOCameraMode::ValidateConfiguration() const
{
	if (!FMath::IsFinite(BlendTime) || BlendTime < 0.0f)
	{
		return FGGYGOCameraEvaluationResult::Failure(this, TEXT("BlendTime"),
			FMath::IsFinite(BlendTime) ? TEXT("negative") : TEXT("non-finite"));
	}
	switch (BlendFunction)
	{
	case EGGYGOCameraModeBlendFunction::Linear:
		break; // Linear does not use an exponent.
	case EGGYGOCameraModeBlendFunction::EaseIn:
	case EGGYGOCameraModeBlendFunction::EaseOut:
	case EGGYGOCameraModeBlendFunction::EaseInOut:
		if (!FMath::IsFinite(BlendExponent) || BlendExponent <= 0.0f)
		{
			return FGGYGOCameraEvaluationResult::Failure(this, TEXT("BlendExponent"),
				FMath::IsFinite(BlendExponent) ? TEXT("not-positive") : TEXT("non-finite"));
		}
		if (!FMath::IsFinite(1.0f / BlendExponent))
		{
			return FGGYGOCameraEvaluationResult::Failure(this, TEXT("BlendExponent"), TEXT("non-finite-reciprocal"));
		}
		break;
	default:
		return FGGYGOCameraEvaluationResult::Failure(this, TEXT("BlendFunction"), TEXT("unknown-enum"));
	}
	if (!FMath::IsFinite(FieldOfView) || FieldOfView < 5.0f || FieldOfView > 170.0f)
	{
		return FGGYGOCameraEvaluationResult::Failure(this, TEXT("FieldOfView"),
			FMath::IsFinite(FieldOfView) ? TEXT("outside-[5,170]") : TEXT("non-finite"));
	}
	if (!FMath::IsFinite(ViewPitchMin) || ViewPitchMin < -89.9f || ViewPitchMin > 89.9f)
	{
		return FGGYGOCameraEvaluationResult::Failure(this, TEXT("ViewPitchMin"),
			FMath::IsFinite(ViewPitchMin) ? TEXT("outside-[-89.9,89.9]") : TEXT("non-finite"));
	}
	if (!FMath::IsFinite(ViewPitchMax) || ViewPitchMax < -89.9f || ViewPitchMax > 89.9f)
	{
		return FGGYGOCameraEvaluationResult::Failure(this, TEXT("ViewPitchMax"),
			FMath::IsFinite(ViewPitchMax) ? TEXT("outside-[-89.9,89.9]") : TEXT("non-finite"));
	}
	if (ViewPitchMin > ViewPitchMax)
	{
		return FGGYGOCameraEvaluationResult::Failure(this, TEXT("ViewPitchMin/ViewPitchMax"), TEXT("unordered-limits"));
	}
	return ValidateModeConfiguration();
}

FGGYGOCameraEvaluationResult UGGYGOCameraMode::ValidateModeConfiguration() const
{
	return FGGYGOCameraEvaluationResult::Success();
}

FGGYGOCameraEvaluationResult UGGYGOCameraMode::SetBlendWeight(float Weight)
{
	if (!FMath::IsFinite(Weight) || Weight < 0.0f || Weight > 1.0f)
	{
		return FGGYGOCameraEvaluationResult::Failure(this, TEXT("BlendWeight"),
			FMath::IsFinite(Weight) ? TEXT("outside-[0,1]") : TEXT("non-finite"));
	}
	FGGYGOCameraEvaluationResult Result = ValidateConfiguration();
	if (!Result.IsSuccess())
	{
		return Result;
	}

	float CandidateAlpha;
	switch (BlendFunction)
	{
	case EGGYGOCameraModeBlendFunction::Linear:
		CandidateAlpha = Weight;
		break;
	case EGGYGOCameraModeBlendFunction::EaseIn:
		CandidateAlpha = FMath::InterpEaseIn(0.0f, 1.0f, Weight, 1.0f / BlendExponent);
		break;
	case EGGYGOCameraModeBlendFunction::EaseOut:
		CandidateAlpha = FMath::InterpEaseOut(0.0f, 1.0f, Weight, 1.0f / BlendExponent);
		break;
	case EGGYGOCameraModeBlendFunction::EaseInOut:
		CandidateAlpha = FMath::InterpEaseInOut(0.0f, 1.0f, Weight, 1.0f / BlendExponent);
		break;
	default:
		return FGGYGOCameraEvaluationResult::Failure(this, TEXT("BlendFunction"), TEXT("unknown-enum"));
	}
	if (!FMath::IsFinite(CandidateAlpha) || CandidateAlpha < 0.0f || CandidateAlpha > 1.0f)
	{
		return FGGYGOCameraEvaluationResult::Failure(this, TEXT("BlendAlpha"), TEXT("invalid-inverse-blend-result"));
	}
	BlendWeight = Weight;
	BlendAlpha = CandidateAlpha;
	return FGGYGOCameraEvaluationResult::Success();
}

FGGYGOCameraEvaluationResult UGGYGOCameraMode::UpdateCameraMode(float DeltaTime)
{
	if (!FMath::IsFinite(DeltaTime) || DeltaTime < 0.0f)
	{
		return FGGYGOCameraEvaluationResult::Failure(this, TEXT("DeltaTime"),
			FMath::IsFinite(DeltaTime) ? TEXT("negative") : TEXT("non-finite"));
	}
	FGGYGOCameraEvaluationResult Result = ValidateConfiguration();
	if (!Result.IsSuccess())
	{
		return Result;
	}
	if (!FMath::IsFinite(BlendAlpha) || BlendAlpha < 0.0f || BlendAlpha > 1.0f)
	{
		return FGGYGOCameraEvaluationResult::Failure(this, TEXT("BlendAlpha"), TEXT("invalid-runtime-alpha"));
	}
	if (!FMath::IsFinite(BlendWeight) || BlendWeight < 0.0f || BlendWeight > 1.0f)
	{
		return FGGYGOCameraEvaluationResult::Failure(this, TEXT("BlendWeight"), TEXT("invalid-runtime-weight"));
	}

	CameraPenetrationRequest = FGGYGOCameraPenetrationRequest();
	Result = UpdateView(DeltaTime);
	if (!Result.IsSuccess())
	{
		return Result;
	}
	// A subclass callback cannot bypass common admission by changing configuration during UpdateView.
	Result = ValidateConfiguration();
	if (!Result.IsSuccess())
	{
		return Result;
	}
	Result = ValidateCameraView(this, View);
	if (!Result.IsSuccess())
	{
		return Result;
	}

	// Valid positive time advances and saturates at the endpoint; legal zero time is an instant cut.
	const float CandidateAlpha = BlendTime > 0.0f
		? FMath::Min(BlendAlpha + DeltaTime / BlendTime, 1.0f)
		: 1.0f;
	float CandidateWeight;
	switch (BlendFunction)
	{
	case EGGYGOCameraModeBlendFunction::Linear:
		CandidateWeight = CandidateAlpha;
		break;
	case EGGYGOCameraModeBlendFunction::EaseIn:
		CandidateWeight = FMath::InterpEaseIn(0.0f, 1.0f, CandidateAlpha, BlendExponent);
		break;
	case EGGYGOCameraModeBlendFunction::EaseOut:
		CandidateWeight = FMath::InterpEaseOut(0.0f, 1.0f, CandidateAlpha, BlendExponent);
		break;
	case EGGYGOCameraModeBlendFunction::EaseInOut:
		CandidateWeight = FMath::InterpEaseInOut(0.0f, 1.0f, CandidateAlpha, BlendExponent);
		break;
	default:
		return FGGYGOCameraEvaluationResult::Failure(this, TEXT("BlendFunction"), TEXT("unknown-enum"));
	}
	if (!FMath::IsFinite(CandidateWeight) || CandidateWeight < 0.0f || CandidateWeight > 1.0f)
	{
		return FGGYGOCameraEvaluationResult::Failure(this, TEXT("BlendWeight"), TEXT("invalid-forward-blend-result"));
	}
	BlendAlpha = CandidateAlpha;
	BlendWeight = CandidateWeight;
	return FGGYGOCameraEvaluationResult::Success();
}

// ============================================================================
// UGGYGOCameraModeStack
// ============================================================================

UGGYGOCameraModeStack::UGGYGOCameraModeStack()
{
}

void UGGYGOCameraModeStack::ClearStack()
{
	for (const TObjectPtr<UGGYGOCameraMode>& Mode : CameraModeStack)
	{
		if (Mode)
		{
			Mode->OnDeactivation();
		}
	}

	CameraModeStack.Reset();
}

UGGYGOCameraMode* UGGYGOCameraModeStack::GetCameraModeInstance(TSubclassOf<UGGYGOCameraMode> CameraModeClass)
{
	UClass* OriginalClass = CameraModeClass.GetGCPtr().Get();
	if (!IsValid(OriginalClass) || !OriginalClass->IsChildOf(UGGYGOCameraMode::StaticClass())
		|| OriginalClass->HasAnyClassFlags(CLASS_Abstract | CLASS_NewerVersionExists))
	{
		return nullptr;
	}
	for (const TObjectPtr<UGGYGOCameraMode>& Mode : CameraModeInstances)
	{
		if (Mode && Mode->GetClass() == OriginalClass)
		{
			return Mode;
		}
	}
	// Push admits the new candidate before adding it to the unique instance pool.
	return NewObject<UGGYGOCameraMode>(this, OriginalClass, NAME_None, RF_NoFlags);
}

FGGYGOCameraEvaluationResult UGGYGOCameraModeStack::PushCameraMode(TSubclassOf<UGGYGOCameraMode> CameraModeClass)
{
	UClass* OriginalClass = CameraModeClass.GetGCPtr().Get();
	const auto ClassFailure = [OriginalClass](const TCHAR* Reason)
	{
		FGGYGOCameraEvaluationResult Result = FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("CameraModeClass"), Reason);
		Result.ModeClassPath = GetPathNameSafe(OriginalClass);
		return Result;
	};
	if (!IsValid(OriginalClass))
	{
		return ClassFailure(TEXT("missing-or-invalid-class"));
	}
	if (!OriginalClass->IsChildOf(UGGYGOCameraMode::StaticClass()))
	{
		return ClassFailure(TEXT("wrong-base-class"));
	}
	if (OriginalClass->HasAnyClassFlags(CLASS_Abstract | CLASS_NewerVersionExists))
	{
		return ClassFailure(TEXT("abstract-or-obsolete-class"));
	}
	const UGGYGOCameraMode* Defaults = OriginalClass->GetDefaultObject<UGGYGOCameraMode>();
	if (!IsValid(Defaults))
	{
		return ClassFailure(TEXT("invalid-class-default-object"));
	}
	FGGYGOCameraEvaluationResult Result = Defaults->ValidateConfiguration();
	if (!Result.IsSuccess())
	{
		return Result;
	}

	const int32 StackSize = CameraModeStack.Num();
	int32 ExistingStackIndex = INDEX_NONE;
	float ExistingStackContribution = 1.0f;
	for (int32 StackIndex = 0; StackIndex < StackSize; ++StackIndex)
	{
		UGGYGOCameraMode* ExistingMode = CameraModeStack[StackIndex];
		if (!IsValid(ExistingMode))
		{
			return FGGYGOCameraEvaluationResult::Failure(ExistingMode, TEXT("Mode"), TEXT("invalid-mode"));
		}
		const float Weight = ExistingMode->GetBlendWeight();
		if (!FMath::IsFinite(Weight) || Weight < 0.0f || Weight > 1.0f)
		{
			return FGGYGOCameraEvaluationResult::Failure(ExistingMode, TEXT("BlendWeight"),
				FMath::IsFinite(Weight) ? TEXT("outside-[0,1]") : TEXT("non-finite"));
		}
		if (ExistingStackIndex == INDEX_NONE)
		{
			if (ExistingMode->GetClass() == OriginalClass)
			{
				ExistingStackIndex = StackIndex;
				ExistingStackContribution *= Weight;
			}
			else
			{
				ExistingStackContribution *= (1.0f - Weight);
			}
		}
	}
	if (!FMath::IsFinite(ExistingStackContribution)
		|| ExistingStackContribution < 0.0f || ExistingStackContribution > 1.0f)
	{
		return ClassFailure(TEXT("invalid-existing-contribution"));
	}

	UGGYGOCameraMode* CameraMode = GetCameraModeInstance(CameraModeClass);
	if (!IsValid(CameraMode))
	{
		return ClassFailure(TEXT("instance-creation-failed"));
	}
	// The candidate stays alive across virtual admission without becoming a pooled or active mode.
	TStrongObjectPtr<UGGYGOCameraMode> CandidateLifetime(CameraMode);
	Result = CameraMode->ValidateConfiguration();
	if (!Result.IsSuccess())
	{
		return Result;
	}
	if (StackSize > 0 && CameraModeStack[0] == CameraMode)
	{
		return FGGYGOCameraEvaluationResult::Success();
	}

	const bool bIsBottomMode = StackSize == 0;
	Result = CameraMode->SetBlendWeight(bIsBottomMode ? 1.0f
		: (ExistingStackIndex == INDEX_NONE ? 0.0f : ExistingStackContribution));
	if (!Result.IsSuccess())
	{
		return Result;
	}
	CameraModeInstances.AddUnique(CameraMode);
	if (ExistingStackIndex != INDEX_NONE)
	{
		CameraModeStack.RemoveAt(ExistingStackIndex);
	}
	CameraModeStack.Insert(CameraMode, 0);
	if (ExistingStackIndex == INDEX_NONE)
	{
		CameraMode->OnActivation();
	}
	return CameraMode->ValidateConfiguration();
}

FGGYGOCameraEvaluationResult UGGYGOCameraModeStack::UpdateStack(float DeltaTime)
{
	if (!FMath::IsFinite(DeltaTime) || DeltaTime < 0.0f)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("DeltaTime"),
			FMath::IsFinite(DeltaTime) ? TEXT("negative") : TEXT("non-finite"));
	}
	const int32 StackSize = CameraModeStack.Num();
	if (StackSize == 0)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("ModeStack"), TEXT("empty-stack"));
	}
	for (UGGYGOCameraMode* CameraMode : CameraModeStack)
	{
		if (!IsValid(CameraMode))
		{
			return FGGYGOCameraEvaluationResult::Failure(CameraMode, TEXT("Mode"), TEXT("invalid-mode"));
		}
	}

	int32 RemoveIndex = INDEX_NONE;
	int32 RemoveCount = 0;
	for (int32 StackIndex = 0; StackIndex < StackSize; ++StackIndex)
	{
		UGGYGOCameraMode* CameraMode = CameraModeStack[StackIndex];
		FGGYGOCameraEvaluationResult Result = CameraMode->UpdateCameraMode(DeltaTime);
		if (!Result.IsSuccess())
		{
			return Result;
		}
		if (CameraMode->GetBlendWeight() >= 1.0f)
		{
			RemoveIndex = StackIndex + 1;
			RemoveCount = StackSize - RemoveIndex;
			break;
		}
	}
	if (RemoveCount > 0)
	{
		for (int32 StackIndex = RemoveIndex; StackIndex < StackSize; ++StackIndex)
		{
			CameraModeStack[StackIndex]->OnDeactivation();
		}
		CameraModeStack.RemoveAt(RemoveIndex, RemoveCount);
	}
	return FGGYGOCameraEvaluationResult::Success();
}

FGGYGOCameraEvaluationResult UGGYGOCameraModeStack::BlendStack(FGGYGOCameraModeView& OutCameraModeView) const
{
	const int32 StackSize = CameraModeStack.Num();
	if (StackSize == 0)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("ModeStack"), TEXT("empty-stack"));
	}

	// Keep the established bottom-to-top blend and validate before committing its output.
	UGGYGOCameraMode* BottomMode = CameraModeStack[StackSize - 1];
	if (!IsValid(BottomMode))
	{
		return FGGYGOCameraEvaluationResult::Failure(BottomMode, TEXT("Mode"), TEXT("invalid-mode"));
	}
	FGGYGOCameraModeView CandidateView = BottomMode->GetCameraModeView();
	FGGYGOCameraEvaluationResult Result = ValidateCameraView(BottomMode, CandidateView);
	if (!Result.IsSuccess())
	{
		return Result;
	}

	for (int32 StackIndex = StackSize - 2; StackIndex >= 0; --StackIndex)
	{
		UGGYGOCameraMode* CameraMode = CameraModeStack[StackIndex];
		if (!IsValid(CameraMode))
		{
			return FGGYGOCameraEvaluationResult::Failure(CameraMode, TEXT("Mode"), TEXT("invalid-mode"));
		}
		const float Weight = CameraMode->GetBlendWeight();
		if (!FMath::IsFinite(Weight) || Weight < 0.0f || Weight > 1.0f)
		{
			return FGGYGOCameraEvaluationResult::Failure(CameraMode, TEXT("BlendWeight"),
				FMath::IsFinite(Weight) ? TEXT("outside-[0,1]") : TEXT("non-finite"));
		}
		if (Weight == 0.0f)
		{
			continue;
		}

		const FGGYGOCameraModeView& ModeView = CameraMode->GetCameraModeView();
		Result = ValidateCameraView(CameraMode, ModeView);
		if (!Result.IsSuccess())
		{
			return Result;
		}
		if (Weight < 1.0f)
		{
			Result = ValidateCameraBlendArithmetic(CameraMode, CandidateView, ModeView, Weight);
			if (!Result.IsSuccess())
			{
				return Result;
			}
		}
		CandidateView.Blend(ModeView, Weight);
		Result = ValidateCameraView(CameraMode, CandidateView);
		if (!Result.IsSuccess())
		{
			return Result;
		}
	}

	OutCameraModeView = CandidateView;
	return FGGYGOCameraEvaluationResult::Success();
}

FGGYGOCameraEvaluationResult UGGYGOCameraModeStack::BlendPenetrationRequests(
	FGGYGOCameraPenetrationRequest& OutPenetrationRequest) const
{
	const int32 StackSize = CameraModeStack.Num();
	if (StackSize == 0)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("ModeStack"), TEXT("empty-stack"));
	}

	FGGYGOCameraPenetrationRequest CandidateRequest;
	float RemainingContribution = 1.0f;
	float TotalRequestContribution = 0.0f;
	FVector WeightedPivot = FVector::ZeroVector;
	float MaxProbeRadius = 0.0f;
	float SlowestRecoverySpeed = 0.0f;
	bool bHasPositiveRecoverySpeed = false;

	for (int32 StackIndex = 0; StackIndex < StackSize; ++StackIndex)
	{
		UGGYGOCameraMode* CameraMode = CameraModeStack[StackIndex];
		if (!IsValid(CameraMode))
		{
			return FGGYGOCameraEvaluationResult::Failure(CameraMode, TEXT("Mode"), TEXT("invalid-mode"));
		}
		const float BlendWeight = CameraMode->GetBlendWeight();
		if (!FMath::IsFinite(BlendWeight) || BlendWeight < 0.0f || BlendWeight > 1.0f)
		{
			return FGGYGOCameraEvaluationResult::Failure(CameraMode, TEXT("BlendWeight"),
				FMath::IsFinite(BlendWeight) ? TEXT("outside-[0,1]") : TEXT("non-finite"));
		}
		const bool bIsBottomMode = (StackIndex == StackSize - 1);
		const float Contribution = bIsBottomMode ? RemainingContribution : RemainingContribution * BlendWeight;
		if (!FMath::IsFinite(Contribution) || Contribution < 0.0f)
		{
			return FGGYGOCameraEvaluationResult::Failure(CameraMode, TEXT("Contribution"), TEXT("invalid-blend-arithmetic"));
		}
		if (!bIsBottomMode)
		{
			RemainingContribution *= (1.0f - BlendWeight);
			if (!FMath::IsFinite(RemainingContribution) || RemainingContribution < 0.0f)
			{
				return FGGYGOCameraEvaluationResult::Failure(CameraMode, TEXT("RemainingContribution"), TEXT("invalid-blend-arithmetic"));
			}
		}

		// An invisible mode or an explicitly disabled request contributes no protection policy.
		const FGGYGOCameraPenetrationRequest& Request = CameraMode->GetCameraPenetrationRequest();
		if (Contribution == 0.0f || !Request.bEnabled)
		{
			continue;
		}
		if (!FMath::IsFinite(Request.PivotLocation.X)
			|| !FMath::IsFinite(Request.PivotLocation.Y) || !FMath::IsFinite(Request.PivotLocation.Z))
		{
			return FGGYGOCameraEvaluationResult::Failure(CameraMode, TEXT("PivotLocation"), TEXT("non-finite"));
		}
		if (!FMath::IsFinite(Request.ProbeRadius) || Request.ProbeRadius < 0.0f)
		{
			return FGGYGOCameraEvaluationResult::Failure(CameraMode, TEXT("ProbeRadius"),
				FMath::IsFinite(Request.ProbeRadius) ? TEXT("negative") : TEXT("non-finite"));
		}
		if (!FMath::IsFinite(Request.RecoverySpeed) || Request.RecoverySpeed < 0.0f)
		{
			return FGGYGOCameraEvaluationResult::Failure(CameraMode, TEXT("RecoverySpeed"),
				FMath::IsFinite(Request.RecoverySpeed) ? TEXT("negative") : TEXT("non-finite"));
		}

		const double WeightedX = WeightedPivot.X + Request.PivotLocation.X * Contribution;
		const double WeightedY = WeightedPivot.Y + Request.PivotLocation.Y * Contribution;
		const double WeightedZ = WeightedPivot.Z + Request.PivotLocation.Z * Contribution;
		const float NewTotalContribution = TotalRequestContribution + Contribution;
		if (!FMath::IsFinite(WeightedX) || !FMath::IsFinite(WeightedY) || !FMath::IsFinite(WeightedZ))
		{
			return FGGYGOCameraEvaluationResult::Failure(CameraMode, TEXT("WeightedPivot"), TEXT("non-finite-aggregate-arithmetic"));
		}
		if (!FMath::IsFinite(NewTotalContribution) || NewTotalContribution <= 0.0f)
		{
			return FGGYGOCameraEvaluationResult::Failure(CameraMode, TEXT("TotalRequestContribution"), TEXT("invalid-aggregate-arithmetic"));
		}
		WeightedPivot = FVector(WeightedX, WeightedY, WeightedZ);
		TotalRequestContribution = NewTotalContribution;
		CandidateRequest.bEnabled = true;
		MaxProbeRadius = FMath::Max(MaxProbeRadius, Request.ProbeRadius);
		if (Request.RecoverySpeed > 0.0f
			&& (!bHasPositiveRecoverySpeed || Request.RecoverySpeed < SlowestRecoverySpeed))
		{
			SlowestRecoverySpeed = Request.RecoverySpeed;
			bHasPositiveRecoverySpeed = true;
		}
	}

	if (CandidateRequest.bEnabled)
	{
		const double InverseContribution = 1.0 / static_cast<double>(TotalRequestContribution);
		const double PivotX = WeightedPivot.X * InverseContribution;
		const double PivotY = WeightedPivot.Y * InverseContribution;
		const double PivotZ = WeightedPivot.Z * InverseContribution;
		if (!FMath::IsFinite(InverseContribution)
			|| !FMath::IsFinite(PivotX) || !FMath::IsFinite(PivotY) || !FMath::IsFinite(PivotZ))
		{
			return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("PivotLocation"), TEXT("non-finite-aggregate-normalization"));
		}
		CandidateRequest.PivotLocation = FVector(PivotX, PivotY, PivotZ);
		CandidateRequest.ProbeRadius = MaxProbeRadius;
		// Zero is the selected legal policy only when every contributing speed is zero.
		CandidateRequest.RecoverySpeed = bHasPositiveRecoverySpeed ? SlowestRecoverySpeed : 0.0f;
	}
	OutPenetrationRequest = CandidateRequest;
	return FGGYGOCameraEvaluationResult::Success();
}

FGGYGOCameraEvaluationResult UGGYGOCameraModeStack::EvaluateStack(float DeltaTime,
	FGGYGOCameraModeView& OutCameraModeView, FGGYGOCameraPenetrationRequest& OutPenetrationRequest)
{
	if (CameraModeStack.IsEmpty())
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("ModeStack"), TEXT("empty-stack"));
	}
	for (UGGYGOCameraMode* CameraMode : CameraModeStack)
	{
		if (!IsValid(CameraMode))
		{
			return FGGYGOCameraEvaluationResult::Failure(CameraMode, TEXT("Mode"), TEXT("invalid-mode"));
		}
	}

	FGGYGOCameraEvaluationResult Result = UpdateStack(DeltaTime);
	if (!Result.IsSuccess())
	{
		return Result;
	}
	FGGYGOCameraModeView CandidateView;
	FGGYGOCameraPenetrationRequest CandidateRequest;
	Result = BlendPenetrationRequests(CandidateRequest);
	if (!Result.IsSuccess())
	{
		return Result;
	}
	Result = BlendStack(CandidateView);
	if (!Result.IsSuccess())
	{
		return Result;
	}
	OutCameraModeView = CandidateView;
	OutPenetrationRequest = CandidateRequest;
	return FGGYGOCameraEvaluationResult::Success();
}
