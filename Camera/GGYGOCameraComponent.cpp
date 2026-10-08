/**
 * @file GGYGOCameraComponent.cpp
 * @brief 相机组件实现
 */
#include "Camera/GGYGOCameraComponent.h"

#include "Camera/GGYGOCameraMode.h"
#include "Engine/HitResult.h"
#include "Engine/Scene.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "Logging/LogMacros.h"
#include "Math/RotationMatrix.h"
#include "Misc/ScopeExit.h"
#include "UObject/Class.h"
#include "UObject/StrongObjectPtr.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOCameraComponent)

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOCamera, Log, All);

namespace
{
	FGGYGOCameraEvaluationResult ValidateCameraComponentView(const FGGYGOCameraModeView& View)
	{
		if (!FMath::IsFinite(View.Location.X) || !FMath::IsFinite(View.Location.Y) || !FMath::IsFinite(View.Location.Z))
		{
			return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("View.Location"), TEXT("non-finite"));
		}
		if (!FMath::IsFinite(View.Rotation.Pitch) || !FMath::IsFinite(View.Rotation.Yaw) || !FMath::IsFinite(View.Rotation.Roll))
		{
			return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("View.Rotation"), TEXT("non-finite"));
		}
		if (!FMath::IsFinite(View.ControlRotation.Pitch) || !FMath::IsFinite(View.ControlRotation.Yaw) || !FMath::IsFinite(View.ControlRotation.Roll))
		{
			return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("View.ControlRotation"), TEXT("non-finite"));
		}
		if (!FMath::IsFinite(View.FieldOfView))
		{
			return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("View.FieldOfView"), TEXT("non-finite"));
		}
		return FGGYGOCameraEvaluationResult::Success();
	}
}


// Opaque call-local provenance and value. The Component never stores this record.
struct FGGYGOPreparedCameraViewState
{
	TWeakObjectPtr<UGGYGOCameraComponent> Component;
	TWeakObjectPtr<AActor> Owner;
	TWeakObjectPtr<AActor> Target;
	TWeakObjectPtr<APawn> Pawn;
	TWeakObjectPtr<AController> Controller;
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<UGGYGOCameraModeStack> Stack;
	FDelegateHandle ArbiterHandle;
	uint64 Revision = 0;
	bool bHadController = false;
	FTransform ComponentTransform;
	FTransform TargetTransform;
	FRotator ControlRotation = FRotator::ZeroRotator;
	FMinimalViewInfo SourceLens;
	FGGYGOCameraOffset Offset;
	FGGYGOCameraOffsetHandle OffsetHandle;
	uint64 OffsetSequence = 0;
	float OffsetAlpha = 0.0f;
	float PenetrationRatio = 1.0f;
	bool bOffsetActive = false;
	FGGYGOCameraModeView View;
	FString SelectedClassPath;

	void MergePublishedFields(FMinimalViewInfo& Output) const
	{
		Output.Location = View.Location;
		Output.Rotation = View.Rotation;
		Output.FOV = View.FieldOfView;
		Output.OrthoWidth = SourceLens.OrthoWidth;
		Output.OrthoNearClipPlane = SourceLens.OrthoNearClipPlane;
		Output.OrthoFarClipPlane = SourceLens.OrthoFarClipPlane;
		Output.AspectRatio = SourceLens.AspectRatio;
		Output.bConstrainAspectRatio = SourceLens.bConstrainAspectRatio;
		Output.bUseFieldOfViewForLOD = SourceLens.bUseFieldOfViewForLOD;
		Output.ProjectionMode = SourceLens.ProjectionMode;
		Output.PostProcessBlendWeight = SourceLens.PostProcessBlendWeight;
		if (SourceLens.PostProcessBlendWeight > 0.0f)
		{
			Output.PostProcessSettings = SourceLens.PostProcessSettings;
		}
	}
};

FGGYGOPreparedCameraView::FGGYGOPreparedCameraView() = default;
FGGYGOPreparedCameraView::~FGGYGOPreparedCameraView() = default;
FGGYGOPreparedCameraView::FGGYGOPreparedCameraView(FGGYGOPreparedCameraView&& Other) noexcept = default;
FGGYGOPreparedCameraView& FGGYGOPreparedCameraView::operator=(FGGYGOPreparedCameraView&& Other) noexcept = default;

FGGYGOCameraEvaluationResult FGGYGOPreparedCameraView::TryGetCandidateView(FMinimalViewInfo& InOutCandidateView) const
{
	if (!State)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("PreparedCameraView"), TEXT("empty-or-consumed"));
	}
	const UGGYGOCameraComponent* Component = State->Component.Get();
	if (!IsValid(Component))
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("PreparedCameraView.Component"), TEXT("original-component-invalid"));
	}
	if (Component->bCameraEvaluationBusy)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("PreparedCameraView"), TEXT("synchronous-evaluation-busy"));
	}
	const FGGYGOCameraEvaluationResult Result = Component->ValidatePreparedCameraContext(*State);
	if (!Result.IsSuccess())
	{
		return Result;
	}
	State->MergePublishedFields(InOutCandidateView);
	return FGGYGOCameraEvaluationResult::Success();
}

UGGYGOCameraComponent::UGGYGOCameraComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	CameraModeStack = nullptr;

	// 关掉引擎的"用 Pawn 控制旋转"：视角朝向由相机模式给出。
	// 两者同时生效会让模式算出的朝向被 Pawn 的控制旋转覆盖。
	bUsePawnControlRotation = false;
}

FGGYGOCameraModeObservation UGGYGOCameraComponent::QueryCameraMode(
	TSubclassOf<UGGYGOCameraMode> ModeClass) const
{
	FGGYGOCameraModeObservation Observation;
	if (!IsInGameThread())
	{
		Observation.Diagnostic = TEXT("[Camera.ModeObservation] query requires the game thread.");
		return Observation;
	}
	UClass* OriginalClass = ModeClass.Get();
	const auto Unavailable = [&Observation, this, OriginalClass](const TCHAR* Reason)
	{
		Observation.Diagnostic = FString::Printf(TEXT("[Camera.ModeObservation] Component='%s' Class='%s': %s"),
			*GetPathName(), *GetPathNameSafe(OriginalClass), Reason);
		return Observation;
	};
	AActor* OriginalOwner = GetOwner();
	UWorld* OriginalWorld = GetWorld();
	if (!IsValid(this) || IsBeingDestroyed() || !IsRegistered()
		|| !IsValid(OriginalOwner) || OriginalOwner->IsActorBeingDestroyed()
		|| !IsValid(OriginalWorld) || OriginalOwner->GetWorld() != OriginalWorld)
	{
		return Unavailable(TEXT("original registered Component/Owner/World is unavailable."));
	}
	if (!IsValid(OriginalClass) || !OriginalClass->IsChildOf(UGGYGOCameraMode::StaticClass())
		|| OriginalClass->HasAnyClassFlags(CLASS_Abstract | CLASS_NewerVersionExists))
	{
		return Unavailable(TEXT("requires an explicit valid concrete camera mode class."));
	}
	UGGYGOCameraModeStack* OriginalStack = CameraModeStack.Get();
	if (!IsValid(OriginalStack) || OriginalStack->GetOuter() != this)
	{
		return Unavailable(TEXT("current original camera mode stack is unavailable or reparented."));
	}
	Observation = OriginalStack->QueryCameraMode(OriginalClass);
	if (Observation.Status != EGGYGOCameraModeObservationStatus::Unavailable)
	{
		Observation.OriginalOwner = OriginalOwner;
		Observation.OriginalComponent = const_cast<UGGYGOCameraComponent*>(this);
		Observation.OriginalStack = OriginalStack;
	}
	return Observation;
}

void UGGYGOCameraComponent::OnRegister()
{
	InvalidatePreparedCameraViews();
	Super::OnRegister();

	if (!CameraModeStack)
	{
		// 栈的 Outer 是本组件。相机模式靠 GetOuter()->GetOuter() 反查组件，
		// 这层关系不能变。
		CameraModeStack = NewObject<UGGYGOCameraModeStack>(this);
	}
}

void UGGYGOCameraComponent::OnUnregister()
{
	InvalidatePreparedCameraViews();
	Super::OnUnregister();
}

void UGGYGOCameraComponent::InvalidatePreparedCameraViews()
{
	// Saturation rejects preparation/consumption instead of reusing an old revision.
	if (CameraEvaluationRevision != MAX_uint64)
	{
		++CameraEvaluationRevision;
	}
}

void UGGYGOCameraComponent::FinishCameraEvaluation()
{
	if (bDeferredCameraModeStackClear)
	{
		bDeferredCameraModeStackClear = false;
		if (IsValid(CameraModeStack))
		{
			CameraModeStack->ClearStack();
		}
		// A nested Clear/Reset during OnDeactivation is covered by this same clear.
		bDeferredCameraModeStackClear = false;
	}
	bCameraEvaluationBusy = false;
}

void UGGYGOCameraComponent::ClearCameraModeStack()
{
	InvalidatePreparedCameraViews();
	CurrentPenetrationRecoveryRatio = 1.0f;
	if (bCameraEvaluationBusy)
	{
		bDeferredCameraModeStackClear = true;
		return;
	}
	bCameraEvaluationBusy = true;
	bDeferredCameraModeStackClear = true;
	FinishCameraEvaluation();
}

void UGGYGOCameraComponent::ResetCameraRuntimeState()
{
	ClearCameraModeStack();
	CameraOffset = FGGYGOCameraOffset();
	CameraOffsetAlpha = 0.0f;
	bCameraOffsetActive = false;
	ActiveCameraOffsetHandle = FGGYGOCameraOffsetHandle();
	CurrentPenetrationRecoveryRatio = 1.0f;
}

FGGYGOCameraEvaluationResult UGGYGOCameraComponent::UpdateCameraModes()
{
	if (!IsValid(CameraModeStack) || CameraModeStack->GetOuter() != this)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("CameraModeStack"), TEXT("missing-or-invalid-stack"));
	}
	if (!DetermineCameraModeDelegate.IsBound())
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("DetermineCameraModeDelegate"), TEXT("unbound-required-arbiter"));
	}

	// 原始类交给 Stack 校验，不能用 TSubclassOf 的真假转换掩盖错误基类。
	TSubclassOf<UGGYGOCameraMode> CameraMode = DetermineCameraModeDelegate.Execute();
	FGGYGOCameraEvaluationResult Result = CameraModeStack->PushCameraMode(CameraMode);
	if (Result.IsSuccess())
	{
		Result.ModeClassPath = GetPathNameSafe(CameraMode.GetGCPtr().Get());
	}
	return Result;
}

FGGYGOCameraOffsetHandle UGGYGOCameraComponent::SetCameraOffset(const FGGYGOCameraOffset& InOffset)
{
	// 完整校验先于发号和成员写入，失败不能替换或撤销入口时的旧请求。
	const TCHAR* InvalidField = nullptr;
	const TCHAR* InvalidReason = TEXT("non-finite");
	if (!FMath::IsFinite(InOffset.LocationOffset.X))
	{
		InvalidField = TEXT("LocationOffset.X");
	}
	else if (!FMath::IsFinite(InOffset.LocationOffset.Y))
	{
		InvalidField = TEXT("LocationOffset.Y");
	}
	else if (!FMath::IsFinite(InOffset.LocationOffset.Z))
	{
		InvalidField = TEXT("LocationOffset.Z");
	}
	else if (!FMath::IsFinite(InOffset.FieldOfViewDelta))
	{
		InvalidField = TEXT("FieldOfViewDelta");
	}
	else if (!FMath::IsFinite(InOffset.BlendInTime) || InOffset.BlendInTime < 0.0f)
	{
		InvalidField = TEXT("BlendInTime");
		InvalidReason = FMath::IsFinite(InOffset.BlendInTime) ? TEXT("negative") : TEXT("non-finite");
	}
	else if (!FMath::IsFinite(InOffset.BlendOutTime) || InOffset.BlendOutTime < 0.0f)
	{
		InvalidField = TEXT("BlendOutTime");
		InvalidReason = FMath::IsFinite(InOffset.BlendOutTime) ? TEXT("negative") : TEXT("non-finite");
	}

	if (InvalidField)
	{
		UE_LOG(LogGGYGOCamera, Error,
			TEXT("Camera SetCameraOffset rejected: Component=[%s] Owner=[%s] Field=[%s] Reason=[%s]."),
			*GetPathNameSafe(this), *GetPathNameSafe(GetOwner()), InvalidField, InvalidReason);
		return FGGYGOCameraOffsetHandle();
	}

	// 序号耗尽后拒绝新请求，避免旧能力持有的 token 在 Reset 或 Avatar 切换后复用。
	if (LastIssuedCameraOffsetHandle == MAX_uint64)
	{
		UE_LOG(LogGGYGOCamera, Error,
			TEXT("Camera SetCameraOffset rejected: Component=[%s] Owner=[%s] Field=[HandleSequence] Reason=[token-exhausted]."),
			*GetPathNameSafe(this), *GetPathNameSafe(GetOwner()));
		return FGGYGOCameraOffsetHandle();
	}

	InvalidatePreparedCameraViews();
	FGGYGOCameraOffsetHandle NewHandle;
	NewHandle.Value = ++LastIssuedCameraOffsetHandle;

	CameraOffset = InOffset;
	bCameraOffsetActive = true;
	ActiveCameraOffsetHandle = NewHandle;

	// 不重置 alpha：上一份微调可能还没回落完就来了新的一份，
	// 从当前强度继续过渡比归零重来更平滑。
	return NewHandle;
}

bool UGGYGOCameraComponent::ClearCameraOffset(FGGYGOCameraOffsetHandle Handle)
{
	if (!Handle.IsValid() || Handle != ActiveCameraOffsetHandle)
	{
		return false;
	}

	InvalidatePreparedCameraViews();
	// 保留 CameraOffset 本身，回落期间还要用它的数值和 BlendOutTime。
	bCameraOffsetActive = false;
	ActiveCameraOffsetHandle = FGGYGOCameraOffsetHandle();
	return true;
}

FGGYGOCameraEvaluationResult UGGYGOCameraComponent::UpdateCameraOffsetAlpha(float DeltaTime)
{
	if (!FMath::IsFinite(DeltaTime) || DeltaTime < 0.0f)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("DeltaTime"),
			FMath::IsFinite(DeltaTime) ? TEXT("negative") : TEXT("non-finite"));
	}
	if (!FMath::IsFinite(CameraOffsetAlpha) || CameraOffsetAlpha < 0.0f || CameraOffsetAlpha > 1.0f)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("CameraOffsetAlpha"), TEXT("non-finite-or-outside-[0,1]"));
	}

	const float BlendTime = bCameraOffsetActive ? CameraOffset.BlendInTime : CameraOffset.BlendOutTime;
	if (!FMath::IsFinite(BlendTime) || BlendTime < 0.0f)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr,
			bCameraOffsetActive ? TEXT("CameraOffset.BlendInTime") : TEXT("CameraOffset.BlendOutTime"),
			FMath::IsFinite(BlendTime) ? TEXT("negative") : TEXT("non-finite"));
	}
	if (BlendTime == 0.0f)
	{
		CameraOffsetAlpha = bCameraOffsetActive ? 1.0f : 0.0f;
		return FGGYGOCameraEvaluationResult::Success();
	}

	// 按时间线性推进；有限结果到达端点时仍执行正常钳制。
	const float Step = DeltaTime / BlendTime;
	const float AdvancedAlpha = CameraOffsetAlpha + (bCameraOffsetActive ? Step : -Step);
	if (!FMath::IsFinite(Step) || !FMath::IsFinite(AdvancedAlpha))
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("CameraOffsetAlpha.Step"), TEXT("non-finite-arithmetic"));
	}
	CameraOffsetAlpha = FMath::Clamp(AdvancedAlpha, 0.0f, 1.0f);
	return FGGYGOCameraEvaluationResult::Success();
}

FGGYGOCameraEvaluationResult UGGYGOCameraComponent::ApplyCameraOffset(FGGYGOCameraModeView& View) const
{
	const FGGYGOCameraEvaluationResult ViewResult = ValidateCameraComponentView(View);
	if (!ViewResult.IsSuccess())
	{
		return ViewResult;
	}
	if (!FMath::IsFinite(CameraOffsetAlpha) || CameraOffsetAlpha < 0.0f || CameraOffsetAlpha > 1.0f)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("CameraOffsetAlpha"), TEXT("non-finite-or-outside-[0,1]"));
	}
	if (CameraOffsetAlpha == 0.0f)
	{
		return FGGYGOCameraEvaluationResult::Success();
	}
	if (!FMath::IsFinite(CameraOffset.LocationOffset.X) || !FMath::IsFinite(CameraOffset.LocationOffset.Y)
		|| !FMath::IsFinite(CameraOffset.LocationOffset.Z))
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("CameraOffset.LocationOffset"), TEXT("non-finite"));
	}
	if (!FMath::IsFinite(CameraOffset.FieldOfViewDelta))
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("CameraOffset.FieldOfViewDelta"), TEXT("non-finite"));
	}
	if (CameraOffset.IsNearlyZero())
	{
		return FGGYGOCameraEvaluationResult::Success();
	}

	// 在构造 FVector 前检查原始分量，避免引擎 NaN 诊断把溢出替换成零向量。
	const double LocalX = CameraOffset.LocationOffset.X * CameraOffsetAlpha;
	const double LocalY = CameraOffset.LocationOffset.Y * CameraOffsetAlpha;
	const double LocalZ = CameraOffset.LocationOffset.Z * CameraOffsetAlpha;
	if (!FMath::IsFinite(LocalX) || !FMath::IsFinite(LocalY) || !FMath::IsFinite(LocalZ))
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("CameraOffset.LocalLocation"), TEXT("non-finite-arithmetic"));
	}
	const FRotationMatrix RotationMatrix(View.Rotation);
	const double WorldX = LocalX * RotationMatrix.M[0][0] + LocalY * RotationMatrix.M[1][0] + LocalZ * RotationMatrix.M[2][0];
	const double WorldY = LocalX * RotationMatrix.M[0][1] + LocalY * RotationMatrix.M[1][1] + LocalZ * RotationMatrix.M[2][1];
	const double WorldZ = LocalX * RotationMatrix.M[0][2] + LocalY * RotationMatrix.M[1][2] + LocalZ * RotationMatrix.M[2][2];
	const double LocationX = View.Location.X + WorldX;
	const double LocationY = View.Location.Y + WorldY;
	const double LocationZ = View.Location.Z + WorldZ;
	if (!FMath::IsFinite(WorldX) || !FMath::IsFinite(WorldY) || !FMath::IsFinite(WorldZ)
		|| !FMath::IsFinite(LocationX) || !FMath::IsFinite(LocationY) || !FMath::IsFinite(LocationZ))
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("CameraOffset.WorldLocation"), TEXT("non-finite-arithmetic"));
	}
	const float OffsetFOV = CameraOffset.FieldOfViewDelta * CameraOffsetAlpha;
	const float FieldOfViewWithOffset = View.FieldOfView + OffsetFOV;
	if (!FMath::IsFinite(OffsetFOV) || !FMath::IsFinite(FieldOfViewWithOffset))
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("CameraOffset.FieldOfView"), TEXT("non-finite-arithmetic"));
	}

	View.Location = FVector(LocationX, LocationY, LocationZ);
	View.FieldOfView = FMath::Clamp(FieldOfViewWithOffset, 5.0f, 170.0f);
	return FGGYGOCameraEvaluationResult::Success();
}

FGGYGOCameraEvaluationResult UGGYGOCameraComponent::ResolveCameraPenetration(
	float DeltaTime,
	const FGGYGOCameraPenetrationRequest& Request,
	FGGYGOCameraModeView& View)
{
	if (!FMath::IsFinite(DeltaTime) || DeltaTime < 0.0f)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("DeltaTime"),
			FMath::IsFinite(DeltaTime) ? TEXT("negative") : TEXT("non-finite"));
	}
	const FGGYGOCameraEvaluationResult ViewResult = ValidateCameraComponentView(View);
	if (!ViewResult.IsSuccess())
	{
		return ViewResult;
	}
	if (!FMath::IsFinite(CurrentPenetrationRecoveryRatio)
		|| CurrentPenetrationRecoveryRatio < 0.0f || CurrentPenetrationRecoveryRatio > 1.0f)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("CurrentPenetrationRecoveryRatio"), TEXT("non-finite-or-outside-[0,1]"));
	}
	if (!Request.bEnabled)
	{
		CurrentPenetrationRecoveryRatio = 1.0f;
		return FGGYGOCameraEvaluationResult::Success();
	}

	UWorld* World = GetWorld();
	AActor* TargetActor = GetTargetActor();
	if (!IsValid(World) || !IsValid(TargetActor))
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("Penetration.Context"), TEXT("missing-or-invalid-world-or-target"));
	}
	if (!FMath::IsFinite(Request.PivotLocation.X) || !FMath::IsFinite(Request.PivotLocation.Y) || !FMath::IsFinite(Request.PivotLocation.Z))
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("Penetration.PivotLocation"), TEXT("non-finite"));
	}
	if (!FMath::IsFinite(Request.ProbeRadius) || Request.ProbeRadius < 0.0f)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("Penetration.ProbeRadius"), TEXT("non-finite-or-negative"));
	}
	if (!FMath::IsFinite(Request.RecoverySpeed) || Request.RecoverySpeed < 0.0f)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("Penetration.RecoverySpeed"), TEXT("non-finite-or-negative"));
	}

	const FVector Start = Request.PivotLocation;
	const FVector End = View.Location;
	const double TravelX = End.X - Start.X;
	const double TravelY = End.Y - Start.Y;
	const double TravelZ = End.Z - Start.Z;
	const double TravelSizeSquared = TravelX * TravelX + TravelY * TravelY + TravelZ * TravelZ;
	if (!FMath::IsFinite(TravelX) || !FMath::IsFinite(TravelY) || !FMath::IsFinite(TravelZ) || !FMath::IsFinite(TravelSizeSquared))
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("Penetration.Travel"), TEXT("non-finite-arithmetic"));
	}
	if (TravelSizeSquared <= FMath::Square(KINDA_SMALL_NUMBER))
	{
		CurrentPenetrationRecoveryRatio = 1.0f;
		return FGGYGOCameraEvaluationResult::Success();
	}

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(GGYGOCameraPenetration), /*bTraceComplex=*/false);
	QueryParams.AddIgnoredActor(TargetActor);
	FHitResult Hit;
	const bool bBlocked = Request.ProbeRadius > 0.0f
		? World->SweepSingleByChannel(Hit, Start, End, FQuat::Identity, ECC_Camera,
			FCollisionShape::MakeSphere(Request.ProbeRadius), QueryParams)
		: World->LineTraceSingleByChannel(Hit, Start, End, ECC_Camera, QueryParams);
	if (bBlocked && !FMath::IsFinite(Hit.Time))
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("Penetration.Hit.Time"), TEXT("non-finite-native-result"));
	}
	const float TargetRatio = bBlocked ? FMath::Clamp(Hit.Time, 0.0f, 1.0f) : 1.0f;

	float RecoveryRatio = TargetRatio;
	if (TargetRatio >= CurrentPenetrationRecoveryRatio && Request.RecoverySpeed > 0.0f)
	{
		// FInterpTo 的正常近端点分支不消费步长；仅检查实际会参与插值的乘积。
		const float RecoveryDistance = TargetRatio - CurrentPenetrationRecoveryRatio;
		if (FMath::Square(RecoveryDistance) >= UE_SMALL_NUMBER
			&& !FMath::IsFinite(DeltaTime * Request.RecoverySpeed))
		{
			return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("Penetration.RecoveryStep"), TEXT("non-finite-arithmetic"));
		}
		RecoveryRatio = FMath::FInterpTo(CurrentPenetrationRecoveryRatio, TargetRatio, DeltaTime, Request.RecoverySpeed);
	}
	if (!FMath::IsFinite(RecoveryRatio))
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("Penetration.RecoveryRatio"), TEXT("non-finite-arithmetic"));
	}
	RecoveryRatio = FMath::Clamp(RecoveryRatio, 0.0f, 1.0f);
	const double LocationX = Start.X + TravelX * RecoveryRatio;
	const double LocationY = Start.Y + TravelY * RecoveryRatio;
	const double LocationZ = Start.Z + TravelZ * RecoveryRatio;
	if (!FMath::IsFinite(LocationX) || !FMath::IsFinite(LocationY) || !FMath::IsFinite(LocationZ))
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("Penetration.Location"), TEXT("non-finite-arithmetic"));
	}

	CurrentPenetrationRecoveryRatio = RecoveryRatio;
	View.Location = FVector(LocationX, LocationY, LocationZ);
	return FGGYGOCameraEvaluationResult::Success();
}

void UGGYGOCameraComponent::ReportCameraEvaluationFailure(
	const FGGYGOCameraEvaluationResult& Result,
	const AActor* OriginalTarget,
	const FString& SelectedClassPath)
{
	const FString Diagnostic = FString::Printf(
		TEXT("Module=[Camera] Component=[%s] Target=[%s] Mode=[%s] ModeClass=[%s] SelectedClass=[%s] Field=[%s] Reason=[%s]"),
		*GetPathNameSafe(this), *GetPathNameSafe(OriginalTarget), *Result.ModePath, *Result.ModeClassPath,
		*SelectedClassPath, *Result.Field.ToString(), *Result.Reason);
	if (Diagnostic != LastCameraEvaluationFailure)
	{
		LastCameraEvaluationFailure = Diagnostic;
		UE_LOG(LogGGYGOCamera, Error, TEXT("Camera evaluation rejected: %s."), *Diagnostic);
	}
}

FGGYGOCameraEvaluationResult UGGYGOCameraComponent::ValidatePreparedCameraContext(
	const FGGYGOPreparedCameraViewState& State, bool bCheckPublishedInputs) const
{
	if (!IsValid(this) || !IsRegistered() || IsTemplate() || State.Component.Get() != this)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("PreparedCameraView.Component"), TEXT("original-component-unavailable-or-different"));
	}
	if (CameraEvaluationRevision == MAX_uint64 || State.Revision != CameraEvaluationRevision)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("PreparedCameraView.Revision"), TEXT("invalidated-or-exhausted"));
	}
	AActor* OwnerActor = State.Owner.Get();
	AActor* TargetActor = State.Target.Get();
	UWorld* World = State.World.Get();
	UGGYGOCameraModeStack* Stack = State.Stack.Get();
	if (!IsValid(OwnerActor) || !IsValid(TargetActor) || !IsValid(World) || !IsValid(Stack)
		|| GetOwner() != OwnerActor || GetTargetActor() != TargetActor || GetWorld() != World
		|| OwnerActor->GetWorld() != World || TargetActor->GetWorld() != World
		|| CameraModeStack != Stack || Stack->GetOuter() != this)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("PreparedCameraView.Context"), TEXT("original-owner-target-world-or-stack-changed"));
	}
	APawn* TargetPawn = Cast<APawn>(TargetActor);
	AController* Controller = TargetPawn ? TargetPawn->GetController() : nullptr;
	if (State.Pawn.Get() != TargetPawn || (State.bHadController && !State.Controller.IsValid())
		|| Controller != State.Controller.Get())
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("PreparedCameraView.Controller"), TEXT("original-controller-association-changed"));
	}
	if (!DetermineCameraModeDelegate.IsBound() || DetermineCameraModeDelegate.GetHandle() != State.ArbiterHandle)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("DetermineCameraModeDelegate"), TEXT("original-arbiter-binding-changed"));
	}
	if (CameraOffset.LocationOffset != State.Offset.LocationOffset
		|| CameraOffset.FieldOfViewDelta != State.Offset.FieldOfViewDelta
		|| CameraOffset.BlendInTime != State.Offset.BlendInTime || CameraOffset.BlendOutTime != State.Offset.BlendOutTime
		|| ActiveCameraOffsetHandle != State.OffsetHandle || LastIssuedCameraOffsetHandle != State.OffsetSequence
		|| bCameraOffsetActive != State.bOffsetActive || CameraOffsetAlpha != State.OffsetAlpha
		|| CurrentPenetrationRecoveryRatio != State.PenetrationRatio)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("PreparedCameraView.Offset"), TEXT("original-offset-ownership-or-runtime-changed"));
	}
	if (OrthoWidth != State.SourceLens.OrthoWidth || OrthoNearClipPlane != State.SourceLens.OrthoNearClipPlane
		|| OrthoFarClipPlane != State.SourceLens.OrthoFarClipPlane || AspectRatio != State.SourceLens.AspectRatio
		|| bConstrainAspectRatio != State.SourceLens.bConstrainAspectRatio
		|| bUseFieldOfViewForLOD != State.SourceLens.bUseFieldOfViewForLOD || ProjectionMode != State.SourceLens.ProjectionMode
		|| PostProcessBlendWeight != State.SourceLens.PostProcessBlendWeight
		|| (PostProcessBlendWeight > 0.0f && !FPostProcessSettings::StaticStruct()->CompareScriptStruct(
			&PostProcessSettings, &State.SourceLens.PostProcessSettings, 0)))
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("PreparedCameraView.Configuration"), TEXT("original-lens-or-postprocess-changed"));
	}
	if (bCheckPublishedInputs
		&& (FieldOfView != State.SourceLens.FOV || !GetComponentTransform().Equals(State.ComponentTransform, 0.0)
			|| !TargetActor->GetActorTransform().Equals(State.TargetTransform, 0.0)
			|| (Controller && !Controller->GetControlRotation().Equals(State.ControlRotation, 0.0))))
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("PreparedCameraView.SourceView"), TEXT("original-transform-control-or-fov-changed"));
	}
	// Virtual context getters may synchronously invalidate this same candidate.
	if (State.Revision != CameraEvaluationRevision || CameraEvaluationRevision == MAX_uint64)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("PreparedCameraView.Revision"), TEXT("invalidated-during-context-check"));
	}
	return FGGYGOCameraEvaluationResult::Success();
}

FGGYGOCameraEvaluationResult UGGYGOCameraComponent::PrepareCameraView(
	float DeltaTime, FGGYGOPreparedCameraView& OutPrepared)
{
	FString SelectedClassPath;
	return PrepareCameraViewInternal(DeltaTime, OutPrepared, SelectedClassPath);
}

FGGYGOCameraEvaluationResult UGGYGOCameraComponent::PrepareCameraViewInternal(
	float DeltaTime, FGGYGOPreparedCameraView& OutPrepared, FString& OutSelectedClassPath)
{
	OutSelectedClassPath.Reset();
	if (OutPrepared.State)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("PreparedCameraView"), TEXT("output-already-holds-candidate"));
	}
	if (!FMath::IsFinite(DeltaTime) || DeltaTime < 0.0f)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("DeltaTime"),
			FMath::IsFinite(DeltaTime) ? TEXT("negative") : TEXT("non-finite"));
	}
	if (bCameraEvaluationBusy)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("PreparedCameraView"), TEXT("synchronous-evaluation-busy"));
	}
	AActor* TargetActor = GetTargetActor();
	AActor* OwnerActor = GetOwner();
	UWorld* World = GetWorld();
	if (!IsValid(this) || !IsRegistered() || IsTemplate() || !IsValid(TargetActor) || !IsValid(OwnerActor) || !IsValid(World))
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("Camera.Context"), TEXT("missing-or-invalid-world-owner-target-or-registration"));
	}
	if (!IsValid(CameraModeStack) || CameraModeStack->GetOuter() != this)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("CameraModeStack"), TEXT("missing-or-invalid-stack"));
	}
	if (!DetermineCameraModeDelegate.IsBound())
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("DetermineCameraModeDelegate"), TEXT("unbound-required-arbiter"));
	}
	if (CameraEvaluationRevision >= MAX_uint64 - 1)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("PreparedCameraView.Revision"), TEXT("revision-exhausted"));
	}

	TStrongObjectPtr<UGGYGOCameraComponent> KeepComponentAlive(this);
	TStrongObjectPtr<UGGYGOCameraModeStack> KeepStackAlive(CameraModeStack.Get());
	bCameraEvaluationBusy = true;
	ON_SCOPE_EXIT { FinishCameraEvaluation(); };
	InvalidatePreparedCameraViews();

	TUniquePtr<FGGYGOPreparedCameraViewState> Candidate = MakeUnique<FGGYGOPreparedCameraViewState>();
	Candidate->Component = this;
	Candidate->Owner = OwnerActor;
	Candidate->Target = TargetActor;
	Candidate->World = World;
	Candidate->Stack = CameraModeStack.Get();
	Candidate->ArbiterHandle = DetermineCameraModeDelegate.GetHandle();
	Candidate->Revision = CameraEvaluationRevision;
	Candidate->Pawn = Cast<APawn>(TargetActor);
	AController* Controller = Candidate->Pawn.IsValid() ? Candidate->Pawn->GetController() : nullptr;
	if (Controller && !IsValid(Controller))
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("Camera.Controller"), TEXT("invalid-controller"));
	}
	Candidate->Controller = Controller;
	Candidate->bHadController = Controller != nullptr;
	Candidate->ControlRotation = Controller ? Controller->GetControlRotation() : FRotator::ZeroRotator;
	Candidate->ComponentTransform = GetComponentTransform();
	Candidate->TargetTransform = TargetActor->GetActorTransform();
	if (!Candidate->ComponentTransform.IsValid() || !Candidate->TargetTransform.IsValid()
		|| !FMath::IsFinite(Candidate->ControlRotation.Pitch) || !FMath::IsFinite(Candidate->ControlRotation.Yaw)
		|| !FMath::IsFinite(Candidate->ControlRotation.Roll))
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("Camera.SourceView"), TEXT("non-finite-or-invalid-transform-or-control"));
	}
	Candidate->SourceLens.FOV = FieldOfView;
	Candidate->SourceLens.OrthoWidth = OrthoWidth;
	Candidate->SourceLens.OrthoNearClipPlane = OrthoNearClipPlane;
	Candidate->SourceLens.OrthoFarClipPlane = OrthoFarClipPlane;
	Candidate->SourceLens.AspectRatio = AspectRatio;
	Candidate->SourceLens.bConstrainAspectRatio = bConstrainAspectRatio;
	Candidate->SourceLens.bUseFieldOfViewForLOD = bUseFieldOfViewForLOD;
	Candidate->SourceLens.ProjectionMode = ProjectionMode;
	Candidate->SourceLens.PostProcessBlendWeight = PostProcessBlendWeight;
	Candidate->SourceLens.PostProcessSettings = PostProcessSettings;
	Candidate->Offset = CameraOffset;
	Candidate->OffsetHandle = ActiveCameraOffsetHandle;
	Candidate->OffsetSequence = LastIssuedCameraOffsetHandle;
	Candidate->bOffsetActive = bCameraOffsetActive;
	Candidate->OffsetAlpha = CameraOffsetAlpha;
	Candidate->PenetrationRatio = CurrentPenetrationRecoveryRatio;

	FGGYGOCameraEvaluationResult Result = ValidatePreparedCameraContext(*Candidate);
	if (!Result.IsSuccess())
	{
		return Result;
	}
	Result = UpdateCameraModes();
	OutSelectedClassPath = Result.ModeClassPath;
	Candidate->SelectedClassPath = OutSelectedClassPath;
	if (!Result.IsSuccess())
	{
		return Result;
	}
	Result = ValidatePreparedCameraContext(*Candidate);
	if (!Result.IsSuccess())
	{
		return Result;
	}
	FGGYGOCameraPenetrationRequest PenetrationRequest;
	Result = Candidate->Stack->EvaluateStack(DeltaTime, Candidate->View, PenetrationRequest);
	if (!Result.IsSuccess())
	{
		return Result;
	}
	Result = ValidatePreparedCameraContext(*Candidate);
	if (!Result.IsSuccess())
	{
		return Result;
	}
	Result = UpdateCameraOffsetAlpha(DeltaTime);
	if (!Result.IsSuccess())
	{
		return Result;
	}
	Candidate->OffsetAlpha = CameraOffsetAlpha;
	Result = ApplyCameraOffset(Candidate->View);
	if (!Result.IsSuccess())
	{
		return Result;
	}
	Result = ValidatePreparedCameraContext(*Candidate);
	if (!Result.IsSuccess())
	{
		return Result;
	}
	Result = ResolveCameraPenetration(DeltaTime, PenetrationRequest, Candidate->View);
	if (!Result.IsSuccess())
	{
		return Result;
	}
	Candidate->PenetrationRatio = CurrentPenetrationRecoveryRatio;
	Result = ValidateCameraComponentView(Candidate->View);
	if (!Result.IsSuccess())
	{
		return Result;
	}
	// 这些组件字段将被原样发布，非法配置不能借几何求值成功通过。
	const TCHAR* InvalidLensField = nullptr;
	if (ProjectionMode != ECameraProjectionMode::Perspective && ProjectionMode != ECameraProjectionMode::Orthographic)
	{
		InvalidLensField = TEXT("ProjectionMode");
	}
	else if (!FMath::IsFinite(AspectRatio) || AspectRatio <= 0.0f)
	{
		InvalidLensField = TEXT("AspectRatio");
	}
	else if (!FMath::IsFinite(OrthoWidth))
	{
		InvalidLensField = TEXT("OrthoWidth");
	}
	else if (!FMath::IsFinite(OrthoNearClipPlane))
	{
		InvalidLensField = TEXT("OrthoNearClipPlane");
	}
	else if (!FMath::IsFinite(OrthoFarClipPlane))
	{
		InvalidLensField = TEXT("OrthoFarClipPlane");
	}
	else if (!FMath::IsFinite(PostProcessBlendWeight))
	{
		InvalidLensField = TEXT("PostProcessBlendWeight");
	}
	else if (ProjectionMode == ECameraProjectionMode::Orthographic
		&& (OrthoWidth <= 0.0f || OrthoFarClipPlane <= OrthoNearClipPlane))
	{
		InvalidLensField = TEXT("OrthographicVolume");
	}
	if (InvalidLensField)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, InvalidLensField, TEXT("non-finite-or-invalid-projection-config"));
	}

	Result = ValidatePreparedCameraContext(*Candidate);
	if (!Result.IsSuccess())
	{
		return Result;
	}
	OutPrepared.State = MoveTemp(Candidate);
	return FGGYGOCameraEvaluationResult::Success();
}

FGGYGOCameraEvaluationResult UGGYGOCameraComponent::CommitPreparedCameraView(
	FGGYGOPreparedCameraView&& Prepared, FMinimalViewInfo& DesiredView)
{
	// Consume at entry even on rejection; no attempt can reuse or redirect this packet.
	TUniquePtr<FGGYGOPreparedCameraViewState> Candidate = MoveTemp(Prepared.State);
	if (!Candidate)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("PreparedCameraView"), TEXT("empty-or-consumed"));
	}
	if (Candidate->Component.Get() != this)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("PreparedCameraView.Component"), TEXT("cross-component-commit"));
	}
	if (bCameraEvaluationBusy)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("PreparedCameraView"), TEXT("synchronous-evaluation-busy"));
	}
	TStrongObjectPtr<UGGYGOCameraComponent> KeepComponentAlive(this);
	TStrongObjectPtr<UGGYGOCameraModeStack> KeepStackAlive(Candidate->Stack.Get());
	bCameraEvaluationBusy = true;
	ON_SCOPE_EXIT { FinishCameraEvaluation(); };
	FGGYGOCameraEvaluationResult Result = ValidatePreparedCameraContext(*Candidate);
	if (!Result.IsSuccess())
	{
		return Result;
	}
	if (AController* Controller = Candidate->Controller.Get())
	{
		Controller->SetControlRotation(Candidate->View.ControlRotation);
		Result = ValidatePreparedCameraContext(*Candidate, false);
		if (!Result.IsSuccess())
		{
			return Result;
		}
	}
	SetWorldLocationAndRotation(Candidate->View.Location, Candidate->View.Rotation);
	Result = ValidatePreparedCameraContext(*Candidate, false);
	if (!Result.IsSuccess())
	{
		return Result;
	}
	FieldOfView = Candidate->View.FieldOfView;
	Candidate->MergePublishedFields(DesiredView);
	LastCameraEvaluationFailure.Reset();
	return FGGYGOCameraEvaluationResult::Success();
}

FGGYGOCameraEvaluationResult UGGYGOCameraComponent::ReinitializeCameraEvaluation()
{
	if (bCameraEvaluationBusy)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("Camera.Reinitialize"), TEXT("synchronous-evaluation-busy"));
	}
	if (!IsValid(this) || !IsRegistered() || IsTemplate())
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("Camera.Reinitialize.Component"), TEXT("original-component-unavailable"));
	}
	if (!IsValid(CameraModeStack) || CameraModeStack->GetOuter() != this)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("CameraModeStack"), TEXT("missing-or-invalid-original-stack"));
	}
	if (!DetermineCameraModeDelegate.IsBound())
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("DetermineCameraModeDelegate"), TEXT("unbound-required-arbiter"));
	}
	if (CameraEvaluationRevision >= MAX_uint64 - 1)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("Camera.Reinitialize.Revision"), TEXT("revision-exhausted"));
	}

	TStrongObjectPtr<UGGYGOCameraComponent> KeepComponentAlive(this);
	TStrongObjectPtr<UGGYGOCameraModeStack> OriginalStack(CameraModeStack.Get());
	bCameraEvaluationBusy = true;
	ON_SCOPE_EXIT { FinishCameraEvaluation(); };
	InvalidatePreparedCameraViews();

	// Reuse the P3b context validator. This local snapshot is never a prepared view or cache.
	FGGYGOPreparedCameraViewState OriginalContext;
	OriginalContext.Component = this;
	OriginalContext.Owner = GetOwner();
	OriginalContext.Target = GetTargetActor();
	OriginalContext.World = GetWorld();
	OriginalContext.Stack = OriginalStack.Get();
	OriginalContext.ArbiterHandle = DetermineCameraModeDelegate.GetHandle();
	OriginalContext.Revision = CameraEvaluationRevision;
	AActor* TargetActor = OriginalContext.Target.Get();
	if (!IsValid(OriginalContext.Owner.Get()) || !IsValid(TargetActor) || !IsValid(OriginalContext.World.Get()))
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("Camera.Reinitialize.Context"), TEXT("missing-or-invalid-original-owner-target-or-world"));
	}
	OriginalContext.Pawn = Cast<APawn>(TargetActor);
	AController* Controller = OriginalContext.Pawn.IsValid() ? OriginalContext.Pawn->GetController() : nullptr;
	if (Controller && !IsValid(Controller))
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("Camera.Reinitialize.Controller"), TEXT("invalid-original-controller"));
	}
	OriginalContext.Controller = Controller;
	OriginalContext.bHadController = Controller != nullptr;
	OriginalContext.ControlRotation = Controller ? Controller->GetControlRotation() : FRotator::ZeroRotator;
	OriginalContext.ComponentTransform = GetComponentTransform();
	OriginalContext.TargetTransform = TargetActor->GetActorTransform();
	OriginalContext.SourceLens.FOV = FieldOfView;
	OriginalContext.SourceLens.OrthoWidth = OrthoWidth;
	OriginalContext.SourceLens.OrthoNearClipPlane = OrthoNearClipPlane;
	OriginalContext.SourceLens.OrthoFarClipPlane = OrthoFarClipPlane;
	OriginalContext.SourceLens.AspectRatio = AspectRatio;
	OriginalContext.SourceLens.bConstrainAspectRatio = bConstrainAspectRatio;
	OriginalContext.SourceLens.bUseFieldOfViewForLOD = bUseFieldOfViewForLOD;
	OriginalContext.SourceLens.ProjectionMode = ProjectionMode;
	OriginalContext.SourceLens.PostProcessBlendWeight = PostProcessBlendWeight;
	if (PostProcessBlendWeight > 0.0f)
	{
		OriginalContext.SourceLens.PostProcessSettings = PostProcessSettings;
	}
	OriginalContext.Offset = CameraOffset;
	OriginalContext.OffsetHandle = ActiveCameraOffsetHandle;
	OriginalContext.OffsetSequence = LastIssuedCameraOffsetHandle;
	OriginalContext.bOffsetActive = bCameraOffsetActive;
	OriginalContext.OffsetAlpha = CameraOffsetAlpha;
	OriginalContext.PenetrationRatio = CurrentPenetrationRecoveryRatio;

	FGGYGOCameraEvaluationResult Result = ValidatePreparedCameraContext(OriginalContext);
	if (!Result.IsSuccess())
	{
		return Result;
	}
	TStrongObjectPtr<UGGYGOCameraModeStack> FreshStack(NewObject<UGGYGOCameraModeStack>(this));
	if (!IsValid(FreshStack.Get()) || FreshStack->GetOuter() != this)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, TEXT("Camera.Reinitialize.Stack"), TEXT("fresh-stack-creation-failed"));
	}
	Result = ValidatePreparedCameraContext(OriginalContext);
	if (!Result.IsSuccess())
	{
		return Result;
	}

	OriginalStack->ClearStack();
	// Nested Clear/Reset requested by old OnDeactivation is satisfied by this old-stack clear.
	// Its revision/Offset changes remain real and are rejected by the validator below.
	bDeferredCameraModeStackClear = false;
	Result = ValidatePreparedCameraContext(OriginalContext);
	if (!Result.IsSuccess())
	{
		return Result;
	}

	CameraModeStack = FreshStack.Get();
	CurrentPenetrationRecoveryRatio = 1.0f;
	// The next explicit Prepare admits current configuration; rebuild alone is not a valid frame.
	return FGGYGOCameraEvaluationResult::Success();
}

void UGGYGOCameraComponent::GetCameraView(float DeltaTime, FMinimalViewInfo& DesiredView)
{
	AActor* const OriginalTarget = GetTargetActor();
	FString SelectedClassPath;
	FGGYGOPreparedCameraView Prepared;
	FGGYGOCameraEvaluationResult Result = PrepareCameraViewInternal(DeltaTime, Prepared, SelectedClassPath);
	if (Result.IsSuccess())
	{
		Result = CommitPreparedCameraView(MoveTemp(Prepared), DesiredView);
	}
	if (!Result.IsSuccess())
	{
		ReportCameraEvaluationFailure(Result, OriginalTarget, SelectedClassPath);
	}
}
