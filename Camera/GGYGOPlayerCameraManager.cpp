#include "Camera/GGYGOPlayerCameraManager.h"

#include "Camera/CameraActor.h"
#include "Camera/GGYGOCameraComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOPlayerCameraManager)

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOCameraPublication, Log, All);

namespace
{
	FGGYGOCameraEvaluationResult CameraPublicationFailure(FName Field, const TCHAR* Reason)
	{
		return FGGYGOCameraEvaluationResult::Failure(nullptr, Field, Reason);
	}

	bool IsFiniteCameraVector(const FVector& Value)
	{
		return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
	}

	bool IsFiniteCameraRotation(const FRotator& Value)
	{
		return FMath::IsFinite(Value.Pitch) && FMath::IsFinite(Value.Yaw) && FMath::IsFinite(Value.Roll);
	}

	FGGYGOCameraEvaluationResult ValidateNativeCameraPOV(const FMinimalViewInfo& View)
	{
		// Read scalars before any FVector/FRotator diagnostic can repair them in place.
		if (!IsFiniteCameraVector(View.Location) || !IsFiniteCameraRotation(View.Rotation))
		{
			return CameraPublicationFailure(TEXT("NativePOV.Transform"), TEXT("non-finite-raw-location-or-rotation"));
		}
		if (!FMath::IsFinite(View.FOV) || View.FOV <= 0.0f || View.FOV >= 180.0f
			|| !FMath::IsFinite(View.AspectRatio) || View.AspectRatio <= 0.0f
			|| !FMath::IsFinite(View.OrthoWidth) || !FMath::IsFinite(View.OrthoNearClipPlane)
			|| !FMath::IsFinite(View.OrthoFarClipPlane) || !FMath::IsFinite(View.PerspectiveNearClipPlane)
			|| !FMath::IsFinite(View.FirstPersonFOV) || !FMath::IsFinite(View.FirstPersonScale)
			|| !FMath::IsFinite(View.OffCenterProjectionOffset.X) || !FMath::IsFinite(View.OffCenterProjectionOffset.Y)
			|| !FMath::IsFinite(View.AutoPlaneShift))
		{
			return CameraPublicationFailure(TEXT("NativePOV.Lens"), TEXT("invalid-raw-lens-values"));
		}
		if (View.ProjectionMode != ECameraProjectionMode::Perspective && View.ProjectionMode != ECameraProjectionMode::Orthographic)
		{
			return CameraPublicationFailure(TEXT("NativePOV.ProjectionMode"), TEXT("invalid-projection-mode"));
		}
		if (View.ProjectionMode == ECameraProjectionMode::Orthographic
			&& (View.OrthoWidth <= 0.0f || (!View.bAutoCalculateOrthoPlanes && View.OrthoFarClipPlane <= View.OrthoNearClipPlane)))
		{
			return CameraPublicationFailure(TEXT("NativePOV.OrthoPlanes"), TEXT("invalid-active-orthographic-lens"));
		}
		if (View.bUseFirstPersonParameters
			&& (View.FirstPersonFOV <= 0.0f || View.FirstPersonFOV >= 180.0f || View.FirstPersonScale <= 0.0f))
		{
			return CameraPublicationFailure(TEXT("NativePOV.FirstPerson"), TEXT("invalid-active-first-person-lens"));
		}
		if (!FMath::IsFinite(View.PostProcessBlendWeight) || View.PostProcessBlendWeight < 0.0f || View.PostProcessBlendWeight > 1.0f)
		{
			return CameraPublicationFailure(TEXT("NativePOV.PostProcessBlendWeight"), TEXT("invalid-post-process-weight"));
		}
		return FGGYGOCameraEvaluationResult::Success();
	}

	FGGYGOCameraEvaluationResult ValidateNativeCameraTransition(const FViewTargetTransitionParams& Params)
	{
		if (!FMath::IsFinite(Params.BlendTime) || Params.BlendTime < 0.0f || !FMath::IsFinite(Params.BlendExp))
		{
			return CameraPublicationFailure(TEXT("ViewTargetTransition"), TEXT("non-finite-or-negative-transition-input"));
		}
		switch (Params.BlendFunction)
		{
		case VTBlend_Linear:
		case VTBlend_Cubic:
		case VTBlend_PreBlended:
			break;
		case VTBlend_EaseIn:
		case VTBlend_EaseOut:
		case VTBlend_EaseInOut:
			if (Params.BlendExp <= 0.0f || !FMath::IsFinite(1.0f / Params.BlendExp))
			{
				return CameraPublicationFailure(TEXT("ViewTargetTransition.BlendExp"), TEXT("invalid-active-blend-exponent"));
			}
			break;
		default:
			return CameraPublicationFailure(TEXT("ViewTargetTransition.BlendFunction"), TEXT("invalid-blend-function"));
		}
		return FGGYGOCameraEvaluationResult::Success();
	}

	UCameraComponent* FindNativeSelectedCamera(AActor* Target, FName Style)
	{
		if (ACameraActor* CameraActor = Cast<ACameraActor>(Target))
		{
			return CameraActor->GetCameraComponent();
		}
		// These are explicitly selected native debug routes, not failed GG substitutes.
		if (!Target || !Target->bFindCameraComponentWhenViewTarget || Style == TEXT("Fixed")
			|| Style == TEXT("ThirdPerson") || Style == TEXT("FreeCam") || Style == TEXT("FreeCam_Default") || Style == TEXT("FirstPerson"))
		{
			return nullptr;
		}
		TInlineComponentArray<UCameraComponent*> Cameras;
		Target->GetComponents(Cameras);
		for (UCameraComponent* Camera : Cameras)
		{
			if (Camera && Camera->IsActive()) { return Camera; }
		}
		return nullptr;
	}

	template <typename T>
	bool IsFiniteNativeLerp(T A, T B, float Weight)
	{
		// Native BlendViewInfo uses Lerp. Guard its subtraction/product/addition as
		// scalars, rather than constructing a vector which can mask an overflow.
		const T Difference = B - A;
		const T Product = Weight * Difference;
		return FMath::IsFinite(Difference) && FMath::IsFinite(Product) && FMath::IsFinite(A + Product);
	}

	bool IsFiniteNativeAngleBlend(double A, double B, float Weight)
	{
		const double Difference = B - A;
		if (!FMath::IsFinite(Difference)) { return false; }
		const double Delta = FRotator::NormalizeAxis(Difference);
		return FMath::IsFinite(Delta) && FMath::IsFinite(Weight * Delta) && FMath::IsFinite(A + Weight * Delta);
	}
}

/** Only lives while Super::DoUpdateCamera runs; no persistent second POV/cache. */
struct FGGYGONativeCameraUpdateScope
{
	TWeakObjectPtr<APlayerController> Controller;
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<AActor> OriginalCurrentTarget;
	TWeakObjectPtr<AActor> OriginalPendingTarget;
	TWeakObjectPtr<AActor> EvaluatingTarget;
	TWeakObjectPtr<UGGYGOCameraComponent> EvaluatingComponent;
	FName OriginalStyle;
	FMinimalViewInfo OriginalCache;
	FMinimalViewInfo OriginalLastFrameCache;
	float OriginalCacheTime = 0.0f;
	float OriginalLastFrameTime = 0.0f;
	TArray<FPostProcessSettings> OriginalPPSettings;
	TArray<float> OriginalPPWeights;
	TArray<EViewTargetBlendOrder> OriginalPPOrders;
	uint32 CacheWriteCalls = 0;
	uint32 LastFrameWriteCalls = 0;
	bool bFailed = false;
	bool bCurrentComplete = false;
	bool bPendingComplete = false;
	bool bRawBlendChecked = false;
	bool bDeferLastFrameWrite = false;
	bool bCachePublished = false;
};

AGGYGOPlayerCameraManager::AGGYGOPlayerCameraManager(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

bool AGGYGOPlayerCameraManager::HasConfirmedNativeCameraView() const
{
	return bHasConfirmedNativeView && IsValid(PCOwner) && PCOwner == AdmissionController.Get()
		&& IsValid(GetWorld()) && GetWorld() == AdmissionWorld.Get() && PCOwner->GetWorld() == GetWorld();
}

void AGGYGOPlayerCameraManager::InitializeFor(APlayerController* PC)
{
	if (ActiveNativeUpdate || bNativeInitialization || bExplicitRequestBusy)
	{
		RejectCurrentUpdate(CameraPublicationFailure(TEXT("CameraInitialization"), TEXT("reentrant-manager-initialization")));
		return;
	}
	if (PublicationState == EGGYGOCameraPublicationState::Stopped || !AdmissionController.IsExplicitlyNull())
	{
		RejectCurrentUpdate(CameraPublicationFailure(TEXT("CameraInitialization"), TEXT("existing-manager-admission-cannot-be-reset-by-initialize")));
		return;
	}
	PublicationState = EGGYGOCameraPublicationState::NotActivated;
	bHasConfirmedNativeView = false;
	AdmissionController = PC;
	AdmissionWorld = GetWorld();
	AdmissionTarget.Reset();
	AdmissionComponent.Reset();
	FailureTarget.Reset();
	FailureComponent.Reset();
	bFailureHadComponent = false;
	PublicationResult = CameraPublicationFailure(TEXT("CameraActivation"), TEXT("not-activated-by-original-provider-ready"));
	if (!IsValid(PC) || !IsValid(GetWorld()) || PC->GetWorld() != GetWorld())
	{
		RejectCameraPublication(CameraPublicationFailure(TEXT("CameraInitialization"), TEXT("invalid-original-controller-or-world")), PC, nullptr);
		return;
	}
	bNativeInitialization = true;
	ON_SCOPE_EXIT { bNativeInitialization = false; };
	// Super seeds FOV and calls UpdateCamera(0). The seed is never confirmed, and
	// that early native Update is held by NotActivated, not treated as bad config.
	Super::InitializeFor(PC);
}

FGGYGOCameraEvaluationResult AGGYGOPlayerCameraManager::ValidateOriginalSource(
	AActor* Target, UGGYGOCameraComponent* Component) const
{
	if (!IsValid(PCOwner) || PCOwner != AdmissionController.Get() || !IsValid(GetWorld())
		|| GetWorld() != AdmissionWorld.Get() || PCOwner->GetWorld() != GetWorld()
		|| !IsValid(Target) || Target->GetWorld() != GetWorld())
	{
		return CameraPublicationFailure(TEXT("CameraActivation.Context"), TEXT("original-controller-world-or-target-invalid"));
	}
	if (!IsValid(Component) || !Component->IsRegistered() || !Component->IsActive() || Component->IsTemplate()
		|| Component->GetOwner() != Target || Component->GetTargetActor() != Target || Component->GetWorld() != GetWorld()
		|| FindNativeSelectedCamera(Target, CameraStyle) != Component)
	{
		return CameraPublicationFailure(TEXT("CameraActivation.Component"), TEXT("exact-original-component-is-not-the-selected-live-source"));
	}
	return FGGYGOCameraEvaluationResult::Success();
}

FGGYGOCameraEvaluationResult AGGYGOPlayerCameraManager::ActivateCameraEvaluation(
	AActor* ExpectedTarget, UGGYGOCameraComponent* ExpectedComponent)
{
	if (PublicationState == EGGYGOCameraPublicationState::Stopped) { return PublicationResult; }
	if (ActiveNativeUpdate || bNativeInitialization || bExplicitRequestBusy)
	{
		return CameraPublicationFailure(TEXT("CameraActivation"), TEXT("native-camera-update-or-initialization-busy"));
	}
	if (PublicationState == EGGYGOCameraPublicationState::Running)
	{
		return IsValid(ExpectedTarget) && IsValid(ExpectedComponent)
			&& AdmissionTarget.Get() == ExpectedTarget && AdmissionComponent.Get() == ExpectedComponent
			? FGGYGOCameraEvaluationResult::Success()
			: CameraPublicationFailure(TEXT("CameraActivation"), TEXT("already-activated-for-another-original-source"));
	}
	AdmissionTarget = ExpectedTarget;
	AdmissionComponent = ExpectedComponent;
	TGuardValue<bool> ExplicitRequestGuard(bExplicitRequestBusy, true);
	TStrongObjectPtr<UGGYGOCameraComponent> KeepSourceAlive(ExpectedComponent);
	const FGGYGOCameraEvaluationResult Ready = ValidateOriginalSource(ExpectedTarget, ExpectedComponent);
	if (!Ready.IsSuccess())
	{
		RejectCameraPublication(Ready, ExpectedTarget, ExpectedComponent);
		return PublicationResult;
	}
	Super::SetViewTarget(ExpectedTarget);
	if (PublicationState == EGGYGOCameraPublicationState::Stopped) { return PublicationResult; }
	const FGGYGOCameraEvaluationResult Selected = ValidateOriginalSource(ExpectedTarget, ExpectedComponent);
	if (!Selected.IsSuccess() || ViewTarget.Target != ExpectedTarget || PendingViewTarget.Target)
	{
		RejectCameraPublication(Selected.IsSuccess()
			? CameraPublicationFailure(TEXT("CameraActivation.Context"), TEXT("original-view-target-changed-during-activation")) : Selected,
			ExpectedTarget, ExpectedComponent);
		return PublicationResult;
	}
	PublicationState = EGGYGOCameraPublicationState::Running;
	return RunExplicitAttempt();
}

FGGYGOCameraEvaluationResult AGGYGOPlayerCameraManager::RestartCameraEvaluation(
	AActor* ExpectedOriginalTarget, UGGYGOCameraComponent* ExpectedOriginalComponent)
{
	if (PublicationState != EGGYGOCameraPublicationState::Stopped || ActiveNativeUpdate || bNativeInitialization || bExplicitRequestBusy)
	{
		return CameraPublicationFailure(TEXT("CameraRestart"), TEXT("not-stopped-or-native-call-busy"));
	}
	if (!IsValid(ExpectedOriginalTarget) || FailureTarget.Get() != ExpectedOriginalTarget
		|| FailureComponent.Get() != ExpectedOriginalComponent || (bFailureHadComponent && !IsValid(ExpectedOriginalComponent))
		|| !IsValid(PCOwner) || PCOwner != AdmissionController.Get() || !IsValid(GetWorld())
		|| GetWorld() != AdmissionWorld.Get() || ExpectedOriginalTarget->GetWorld() != GetWorld())
	{
		return CameraPublicationFailure(TEXT("CameraRestart.Context"), TEXT("exact-original-failed-identity-no-longer-matches"));
	}
	TGuardValue<bool> ExplicitRequestGuard(bExplicitRequestBusy, true);
	TStrongObjectPtr<AActor> KeepTargetAlive(ExpectedOriginalTarget);
	TStrongObjectPtr<UGGYGOCameraComponent> KeepSourceAlive(ExpectedOriginalComponent);
	if (bFailureHadComponent)
	{
		const FGGYGOCameraEvaluationResult Context = ValidateOriginalSource(ExpectedOriginalTarget, ExpectedOriginalComponent);
		if (!Context.IsSuccess()) { return Context; }
		const FGGYGOCameraEvaluationResult Rebuilt = ExpectedOriginalComponent->ReinitializeCameraEvaluation();
		if (!Rebuilt.IsSuccess()) { return Rebuilt; }
		const FGGYGOCameraEvaluationResult Rechecked = ValidateOriginalSource(ExpectedOriginalTarget, ExpectedOriginalComponent);
		if (!Rechecked.IsSuccess()) { return Rechecked; }
	}
	Super::SetViewTarget(ExpectedOriginalTarget);
	if (!IsValid(ExpectedOriginalTarget) || !IsValid(PCOwner) || PCOwner != AdmissionController.Get()
		|| GetWorld() != AdmissionWorld.Get() || ViewTarget.Target != ExpectedOriginalTarget || PendingViewTarget.Target)
	{
		return CameraPublicationFailure(TEXT("CameraRestart.Context"), TEXT("original-context-changed-during-native-target-selection"));
	}
	if (bFailureHadComponent)
	{
		const FGGYGOCameraEvaluationResult Selected = ValidateOriginalSource(ExpectedOriginalTarget, ExpectedOriginalComponent);
		if (!Selected.IsSuccess()) { return Selected; }
	}
	PublicationState = EGGYGOCameraPublicationState::Running;
	return RunExplicitAttempt();
}

FGGYGOCameraEvaluationResult AGGYGOPlayerCameraManager::RunExplicitAttempt()
{
	DoUpdateCamera(0.0f);
	return PublicationResult;
}

void AGGYGOPlayerCameraManager::RejectCameraPublication(const FGGYGOCameraEvaluationResult& Failure,
	AActor* OriginalTarget, UGGYGOCameraComponent* OriginalComponent)
{
	if (ActiveNativeUpdate) { ActiveNativeUpdate->bFailed = true; }
	if (PublicationState == EGGYGOCameraPublicationState::Stopped) { return; }
	PublicationState = EGGYGOCameraPublicationState::Stopped;
	PublicationResult = Failure;
	FailureTarget = ActiveNativeUpdate ? ActiveNativeUpdate->EvaluatingTarget : TWeakObjectPtr<AActor>(OriginalTarget);
	FailureComponent = ActiveNativeUpdate ? ActiveNativeUpdate->EvaluatingComponent : TWeakObjectPtr<UGGYGOCameraComponent>(OriginalComponent);
	bFailureHadComponent = !FailureComponent.IsExplicitlyNull();
	UE_LOG(LogGGYGOCameraPublication, Error,
		TEXT("Module=[Camera] Manager=[%s] Controller=[%s] World=[%s] Target=[%s] Component=[%s] Mode=[%s] ModeClass=[%s] Field=[%s] Reason=[%s] ConfirmedNativeView=[%d]; explicit original Restart required."),
		*GetPathNameSafe(this), *GetPathNameSafe(AdmissionController.Get()), *GetPathNameSafe(AdmissionWorld.Get()),
		*GetPathNameSafe(OriginalTarget), *GetPathNameSafe(OriginalComponent), *Failure.ModePath, *Failure.ModeClassPath,
		*Failure.Field.ToString(), *Failure.Reason, bHasConfirmedNativeView ? 1 : 0);
}

void AGGYGOPlayerCameraManager::RejectCurrentUpdate(const FGGYGOCameraEvaluationResult& Failure)
{
	RejectCameraPublication(Failure,
		ActiveNativeUpdate ? ActiveNativeUpdate->EvaluatingTarget.Get() : ViewTarget.Target.Get(),
		ActiveNativeUpdate ? ActiveNativeUpdate->EvaluatingComponent.Get()
			: Cast<UGGYGOCameraComponent>(FindNativeSelectedCamera(ViewTarget.Target, CameraStyle)));
}

void AGGYGOPlayerCameraManager::UpdateCamera(float DeltaTime)
{
	if (PublicationState != EGGYGOCameraPublicationState::Running) { return; }
	if (ActiveNativeUpdate)
	{
		RejectCurrentUpdate(CameraPublicationFailure(TEXT("NativeUpdate"), TEXT("reentrant-native-camera-update")));
		return;
	}
	if (!FMath::IsFinite(DeltaTime) || DeltaTime < 0.0f || !IsValid(PCOwner) || PCOwner != AdmissionController.Get()
		|| !IsValid(GetWorld()) || GetWorld() != AdmissionWorld.Get())
	{
		RejectCurrentUpdate(CameraPublicationFailure(TEXT("NativeUpdate.Context"), TEXT("invalid-delta-time-original-controller-or-world")));
		return;
	}
	Super::UpdateCamera(DeltaTime);
}

void AGGYGOPlayerCameraManager::UpdateCameraPhotographyOnly()
{
	// LevelTick dispatches this virtual entry while paused without UpdateCamera.
	// Reject before native Fill can write timestamps after our POV setters return.
	if (PublicationState != EGGYGOCameraPublicationState::Running) { return; }
	Super::UpdateCameraPhotographyOnly();
}

void AGGYGOPlayerCameraManager::SetViewTarget(AActor* NewViewTarget, FViewTargetTransitionParams TransitionParams)
{
	if (ActiveNativeUpdate)
	{
		RejectCurrentUpdate(CameraPublicationFailure(TEXT("NativeViewTarget"), TEXT("view-target-replaced-during-native-update")));
		return;
	}
	if (PublicationState == EGGYGOCameraPublicationState::Stopped) { return; }
	const FGGYGOCameraEvaluationResult Transition = ValidateNativeCameraTransition(TransitionParams);
	if (!Transition.IsSuccess())
	{
		RejectCameraPublication(Transition, NewViewTarget, Cast<UGGYGOCameraComponent>(FindNativeSelectedCamera(NewViewTarget, CameraStyle)));
		return;
	}
	// Native bootstrap/null selection remains legal. It cannot activate or confirm.
	Super::SetViewTarget(NewViewTarget, TransitionParams);
}

void AGGYGOPlayerCameraManager::DoUpdateCamera(float DeltaTime)
{
	if (PublicationState != EGGYGOCameraPublicationState::Running) { return; }
	if (ActiveNativeUpdate)
	{
		RejectCurrentUpdate(CameraPublicationFailure(TEXT("NativeUpdate"), TEXT("reentrant-native-camera-evaluation")));
		return;
	}
	TStrongObjectPtr<AGGYGOPlayerCameraManager> KeepAlive(this);
	FGGYGONativeCameraUpdateScope Scope;
	Scope.Controller = PCOwner;
	Scope.World = GetWorld();
	Scope.OriginalCurrentTarget = ViewTarget.Target;
	Scope.OriginalPendingTarget = PendingViewTarget.Target;
	Scope.EvaluatingTarget = PendingViewTarget.Target ? PendingViewTarget.Target : ViewTarget.Target;
	Scope.EvaluatingComponent = Cast<UGGYGOCameraComponent>(FindNativeSelectedCamera(Scope.EvaluatingTarget.Get(), CameraStyle));
	Scope.OriginalStyle = CameraStyle;
	Scope.OriginalCache = GetCameraCacheView();
	Scope.OriginalLastFrameCache = GetLastFrameCameraCacheView();
	Scope.OriginalCacheTime = GetCameraCacheTime();
	Scope.OriginalLastFrameTime = GetLastFrameCameraCacheTime();
	Scope.bCurrentComplete = PendingViewTarget.Target && BlendParams.bLockOutgoing;
	const TArray<FPostProcessSettings>* Settings;
	const TArray<float>* Weights;
	const TArray<EViewTargetBlendOrder>* Orders;
	GetCachedPostProcessBlends(Settings, Weights, Orders);
	Scope.OriginalPPSettings = *Settings;
	Scope.OriginalPPWeights = *Weights;
	Scope.OriginalPPOrders = *Orders;
	ActiveNativeUpdate = &Scope;
	ON_SCOPE_EXIT { ActiveNativeUpdate = nullptr; };
	const FGGYGOCameraEvaluationResult Transition = ValidateNativeCameraTransition(BlendParams);
	if (!FMath::IsFinite(DeltaTime) || DeltaTime < 0.0f || !FMath::IsFinite(BlendTimeToGo) || BlendTimeToGo < 0.0f
		|| !IsValid(PCOwner) || PCOwner != AdmissionController.Get() || !IsValid(GetWorld()) || GetWorld() != AdmissionWorld.Get())
	{
		RejectCurrentUpdate(CameraPublicationFailure(TEXT("NativeUpdate.Context"), TEXT("invalid-original-native-update-context")));
	}
	else if (!Transition.IsSuccess()) { RejectCurrentUpdate(Transition); }
	else { Super::DoUpdateCamera(DeltaTime); }
	if (!Scope.bFailed && (!Scope.bCachePublished || Scope.CacheWriteCalls != 1))
	{
		RejectCurrentUpdate(CameraPublicationFailure(TEXT("NativeCache"), TEXT("native-update-did-not-publish-exactly-once")));
	}
	if (Scope.bFailed)
	{
		// Roll back only native view publication data. Mode progress and arbitrary
		// modifier/UE setter side effects are not advertised as a global transaction.
		Super::SetCameraCachePOV(Scope.OriginalCache);
		Super::SetLastFrameCameraCachePOV(Scope.OriginalLastFrameCache);
		SetCameraCacheTime(Scope.OriginalCacheTime);
		SetLastFrameCameraCacheTime(Scope.OriginalLastFrameTime);
		ClearCachedPPBlends();
		for (int32 Index = 0; Index < Scope.OriginalPPSettings.Num(); ++Index)
		{
			AddCachedPPBlend(Scope.OriginalPPSettings[Index], Scope.OriginalPPWeights[Index], Scope.OriginalPPOrders[Index]);
		}
	}
	else
	{
		bHasConfirmedNativeView = true;
		bReportedExternalCacheWrite = false;
		PublicationResult = FGGYGOCameraEvaluationResult::Success();
	}
}

void AGGYGOPlayerCameraManager::UpdateViewTarget(FTViewTarget& OutVT, float DeltaTime)
{
	if (!ActiveNativeUpdate || ActiveNativeUpdate->bFailed) { return; }
	if (PendingViewTarget.Target && BlendParams.bLockOutgoing && OutVT.Equal(ViewTarget)) { return; }
	ActiveNativeUpdate->EvaluatingTarget = OutVT.Target;
	UGGYGOCameraComponent* Component = Cast<UGGYGOCameraComponent>(FindNativeSelectedCamera(OutVT.Target, CameraStyle));
	ActiveNativeUpdate->EvaluatingComponent = Component;
	const TWeakObjectPtr<AActor> OriginalTarget = &OutVT == &PendingViewTarget
		? ActiveNativeUpdate->OriginalPendingTarget : ActiveNativeUpdate->OriginalCurrentTarget;
	if (!OriginalTarget.IsValid() || OriginalTarget.Get() != OutVT.Target)
	{
		ActiveNativeUpdate->EvaluatingTarget = OriginalTarget;
		RejectCurrentUpdate(CameraPublicationFailure(TEXT("NativeViewTarget"), TEXT("original-target-lost-or-replaced-by-native-check")));
		return;
	}
	const bool bUsesComponentRoute = Cast<ACameraActor>(OutVT.Target) || (CameraStyle != TEXT("Fixed")
		&& CameraStyle != TEXT("ThirdPerson") && CameraStyle != TEXT("FreeCam") && CameraStyle != TEXT("FreeCam_Default")
		&& CameraStyle != TEXT("FirstPerson"));
	if (bUsesComponentRoute && AdmissionTarget.Get() == OutVT.Target && Component != AdmissionComponent.Get())
	{
		ActiveNativeUpdate->EvaluatingComponent = AdmissionComponent;
		RejectCurrentUpdate(CameraPublicationFailure(TEXT("NativeViewTarget.Component"), TEXT("admitted-gg-source-missing-or-replaced")));
		return;
	}
	if (!IsValid(OutVT.Target) || OutVT.Target->GetWorld() != GetWorld())
	{
		RejectCurrentUpdate(CameraPublicationFailure(TEXT("NativeViewTarget"), TEXT("missing-or-invalid-original-target")));
		return;
	}
	if (Component)
	{
		TStrongObjectPtr<UGGYGOCameraComponent> KeepSourceAlive(Component);
		FGGYGOPreparedCameraView Prepared;
		FGGYGOCameraEvaluationResult Result = Component->PrepareCameraView(DeltaTime, Prepared);
		if (!Result.IsSuccess()) { RejectCurrentUpdate(Result); return; }
		// Match the native target's initial defaults for fields not owned by GG.
		OutVT.POV = FMinimalViewInfo();
		OutVT.POV.FOV = DefaultFOV;
		OutVT.POV.OrthoWidth = DefaultOrthoWidth;
		OutVT.POV.AspectRatio = DefaultAspectRatio;
		OutVT.POV.bConstrainAspectRatio = bDefaultConstrainAspectRatio;
		OutVT.POV.ProjectionMode = bIsOrthographic ? ECameraProjectionMode::Orthographic : ECameraProjectionMode::Perspective;
		OutVT.POV.PostProcessBlendWeight = 1.0f;
		OutVT.POV.bAutoCalculateOrthoPlanes = bAutoCalculateOrthoPlanes;
		OutVT.POV.AutoPlaneShift = AutoPlaneShift;
		OutVT.POV.bUpdateOrthoPlanes = bUpdateOrthoPlanes;
		OutVT.POV.bUseCameraHeightAsViewTarget = bUseCameraHeightAsViewTarget;
		Result = Prepared.TryGetCandidateView(OutVT.POV);
		if (!Result.IsSuccess()) { RejectCurrentUpdate(Result); return; }
		ApplyCameraModifiers(DeltaTime, OutVT.POV);
		if (ActiveNativeUpdate->bFailed) { return; }
		// Commit in native current->pending order, once. Its output is local scratch:
		// do not overwrite the POV already processed by native modifiers.
		FMinimalViewInfo ComponentOutput;
		Result = Component->CommitPreparedCameraView(MoveTemp(Prepared), ComponentOutput);
		if (!Result.IsSuccess()) { RejectCurrentUpdate(Result); return; }
		SetActorLocationAndRotation(OutVT.POV.Location, OutVT.POV.Rotation, false);
		if (bAutoCalculateOrthoPlanes && OutVT.Target)
		{
			const FVector TargetLocation = OutVT.Target->GetActorLocation();
			if (!IsFiniteCameraVector(TargetLocation)
				|| !IsFiniteNativeLerp(OutVT.POV.Location.X, TargetLocation.X, 1.0f)
				|| !IsFiniteNativeLerp(OutVT.POV.Location.Y, TargetLocation.Y, 1.0f)
				|| !IsFiniteNativeLerp(OutVT.POV.Location.Z, TargetLocation.Z, 1.0f))
			{
				RejectCurrentUpdate(CameraPublicationFailure(TEXT("NativePOV.CameraToViewTarget"), TEXT("invalid-raw-target-location-or-difference")));
				return;
			}
			OutVT.POV.SetCameraToViewTarget(TargetLocation);
		}
		UpdateCameraLensEffects(OutVT);
	}
	else
	{
		Super::UpdateViewTarget(OutVT, DeltaTime);
	}
	if (ActiveNativeUpdate->bFailed) { return; }
	const FGGYGOCameraEvaluationResult View = ValidateNativeCameraPOV(OutVT.POV);
	if (!View.IsSuccess()) { RejectCurrentUpdate(View); return; }
	if (&OutVT == &ViewTarget) { ActiveNativeUpdate->bCurrentComplete = true; }
	if (&OutVT == &PendingViewTarget) { ActiveNativeUpdate->bPendingComplete = true; }
	if (ActiveNativeUpdate->bCurrentComplete && (!PendingViewTarget.Target || ActiveNativeUpdate->bPendingComplete))
	{
		ValidateNativeBlendBeforeFill();
	}
}

void AGGYGOPlayerCameraManager::ApplyCameraModifiers(float DeltaTime, FMinimalViewInfo& InOutPOV)
{
	if (!ActiveNativeUpdate || ActiveNativeUpdate->bFailed) { return; }
	FGGYGOCameraEvaluationResult Result = ValidateNativeCameraPOV(InOutPOV);
	if (!Result.IsSuccess()) { RejectCurrentUpdate(Result); return; }
	Super::ApplyCameraModifiers(DeltaTime, InOutPOV);
	Result = ValidateNativeCameraPOV(InOutPOV);
	if (!Result.IsSuccess()) { RejectCurrentUpdate(Result); }
	const TArray<FPostProcessSettings>* Settings;
	const TArray<float>* Weights;
	const TArray<EViewTargetBlendOrder>* Orders;
	GetCachedPostProcessBlends(Settings, Weights, Orders);
	for (float Weight : *Weights)
	{
		if (!FMath::IsFinite(Weight) || Weight < 0.0f)
		{
			RejectCurrentUpdate(CameraPublicationFailure(TEXT("NativePOV.CachedPostProcessWeight"), TEXT("invalid-modifier-post-process-weight")));
			return;
		}
	}
}

void AGGYGOPlayerCameraManager::ValidateNativeBlendBeforeFill()
{
	if (ActiveNativeUpdate->bFailed) { return; }
	FGGYGOCameraEvaluationResult Result = ValidateNativeCameraPOV(ViewTarget.POV);
	if (!Result.IsSuccess()) { RejectCurrentUpdate(Result); return; }
	if (!FMath::IsFinite(BlendTimeToGo))
	{
		RejectCurrentUpdate(CameraPublicationFailure(TEXT("NativeBlend.Time"), TEXT("non-finite-raw-remaining-time")));
		return;
	}
	if (!PendingViewTarget.Target || BlendTimeToGo <= 0.0f)
	{
		ActiveNativeUpdate->bRawBlendChecked = true;
		return;
	}
	Result = ValidateNativeCameraPOV(PendingViewTarget.POV);
	if (!Result.IsSuccess()) { RejectCurrentUpdate(Result); return; }
	Result = ValidateNativeCameraTransition(BlendParams);
	if (!Result.IsSuccess()) { RejectCurrentUpdate(Result); return; }
	if (BlendParams.BlendTime <= 0.0f || !FMath::IsFinite(BlendTimeToGo))
	{
		RejectCurrentUpdate(CameraPublicationFailure(TEXT("NativeBlend.Time"), TEXT("invalid-active-blend-time")));
		return;
	}
	const float Duration = (BlendParams.BlendTime - BlendTimeToGo) / BlendParams.BlendTime;
	float Weight = 0.0f;
	switch (BlendParams.BlendFunction)
	{
	case VTBlend_Linear: Weight = FMath::Lerp(0.0f, 1.0f, Duration); break;
	case VTBlend_Cubic: Weight = FMath::CubicInterp(0.0f, 0.0f, 1.0f, 0.0f, Duration); break;
	case VTBlend_EaseIn: Weight = FMath::Pow(Duration, BlendParams.BlendExp); break;
	case VTBlend_EaseOut: Weight = FMath::Pow(Duration, 1.0f / BlendParams.BlendExp); break;
	case VTBlend_EaseInOut: Weight = FMath::InterpEaseInOut(0.0f, 1.0f, Duration, BlendParams.BlendExp); break;
	case VTBlend_PreBlended: Weight = 1.0f; break;
	default: RejectCurrentUpdate(CameraPublicationFailure(TEXT("NativeBlend.Function"), TEXT("invalid-active-blend-function"))); return;
	}
	const FMinimalViewInfo& A = ViewTarget.POV;
	const FMinimalViewInfo& B = PendingViewTarget.POV;
	if (!FMath::IsFinite(Duration) || Duration < 0.0f || Duration > 1.0f || !FMath::IsFinite(Weight) || Weight < 0.0f || Weight > 1.0f
		|| !IsFiniteNativeLerp(A.Location.X, B.Location.X, Weight) || !IsFiniteNativeLerp(A.Location.Y, B.Location.Y, Weight)
		|| !IsFiniteNativeLerp(A.Location.Z, B.Location.Z, Weight)
		|| !IsFiniteNativeAngleBlend(A.Rotation.Pitch, B.Rotation.Pitch, Weight)
		|| !IsFiniteNativeAngleBlend(A.Rotation.Yaw, B.Rotation.Yaw, Weight)
		|| !IsFiniteNativeAngleBlend(A.Rotation.Roll, B.Rotation.Roll, Weight)
		|| !IsFiniteNativeLerp(A.FOV, B.FOV, Weight) || !IsFiniteNativeLerp(A.FirstPersonFOV, B.FirstPersonFOV, Weight)
		|| !IsFiniteNativeLerp(A.FirstPersonScale, B.FirstPersonScale, Weight) || !IsFiniteNativeLerp(A.OrthoWidth, B.OrthoWidth, Weight)
		|| !IsFiniteNativeLerp(A.OrthoNearClipPlane, B.OrthoNearClipPlane, Weight) || !IsFiniteNativeLerp(A.OrthoFarClipPlane, B.OrthoFarClipPlane, Weight)
		|| !IsFiniteNativeLerp(A.PerspectiveNearClipPlane, B.PerspectiveNearClipPlane, Weight)
		|| !IsFiniteNativeLerp(A.AspectRatio, B.AspectRatio, Weight)
		|| !IsFiniteNativeLerp(A.OffCenterProjectionOffset.X, B.OffCenterProjectionOffset.X, Weight)
		|| !IsFiniteNativeLerp(A.OffCenterProjectionOffset.Y, B.OffCenterProjectionOffset.Y, Weight)
		|| !FMath::IsFinite(B.PostProcessBlendWeight * Weight))
	{
		RejectCurrentUpdate(CameraPublicationFailure(TEXT("NativeBlend.RawArithmetic"), TEXT("invalid-unrepaired-native-blend-arithmetic")));
		return;
	}
	// This read-only arithmetic guard runs before Super performs its blend/Fill,
	// including AllowPhotographyMode=false. It creates no second blended view.
	ActiveNativeUpdate->bRawBlendChecked = true;
}

bool AGGYGOPlayerCameraManager::UpdatePhotographyCamera(FMinimalViewInfo& NewPOV)
{
	if (!ActiveNativeUpdate || ActiveNativeUpdate->bFailed) { return false; }
	FGGYGOCameraEvaluationResult Result = ValidateNativeCameraPOV(NewPOV);
	if (!Result.IsSuccess()) { RejectCurrentUpdate(Result); return false; }
	const bool bCameraCut = Super::UpdatePhotographyCamera(NewPOV);
	Result = ValidateNativeCameraPOV(NewPOV);
	if (!Result.IsSuccess()) { RejectCurrentUpdate(Result); return false; }
	return bCameraCut;
}

void AGGYGOPlayerCameraManager::ReportExternalCacheWrite()
{
	if (bReportedExternalCacheWrite) { return; }
	bReportedExternalCacheWrite = true;
	UE_LOG(LogGGYGOCameraPublication, Warning,
		TEXT("Module=[Camera] Manager=[%s] Field=[NativeCache] Reason=[external-cache-seed-is-not-a-confirmed-native-update]; write rejected."),
		*GetPathNameSafe(this));
}

void AGGYGOPlayerCameraManager::SetLastFrameCameraCachePOV(const FMinimalViewInfo& InPOV)
{
	if (!ActiveNativeUpdate) { ReportExternalCacheWrite(); return; }
	++ActiveNativeUpdate->LastFrameWriteCalls;
	if (ActiveNativeUpdate->LastFrameWriteCalls > 1)
	{
		RejectCurrentUpdate(CameraPublicationFailure(TEXT("NativeCache.LastFrame"), TEXT("repeated-backup-write-during-native-update")));
		return;
	}
	// Native Fill calls this with the OLD cache. Delay until a valid current write.
	if (!ActiveNativeUpdate->bFailed) { ActiveNativeUpdate->bDeferLastFrameWrite = true; }
}

void AGGYGOPlayerCameraManager::SetCameraCachePOV(const FMinimalViewInfo& InPOV)
{
	if (bNativeInitialization)
	{
		Super::SetCameraCachePOV(InPOV);
		return;
	}
	if (!ActiveNativeUpdate) { ReportExternalCacheWrite(); return; }
	++ActiveNativeUpdate->CacheWriteCalls;
	if (ActiveNativeUpdate->bFailed) { return; }
	if (!ActiveNativeUpdate->bRawBlendChecked || ActiveNativeUpdate->CacheWriteCalls != 1
		|| !IsValid(PCOwner) || PCOwner != ActiveNativeUpdate->Controller.Get() || PCOwner != AdmissionController.Get()
		|| !IsValid(GetWorld()) || GetWorld() != ActiveNativeUpdate->World.Get() || GetWorld() != AdmissionWorld.Get()
		|| CameraStyle != ActiveNativeUpdate->OriginalStyle)
	{
		RejectCurrentUpdate(CameraPublicationFailure(TEXT("NativeCache.Context"), TEXT("unchecked-repeated-or-context-changed-native-publication")));
		return;
	}
	const FGGYGOCameraEvaluationResult View = ValidateNativeCameraPOV(InPOV);
	if (!View.IsSuccess()) { RejectCurrentUpdate(View); return; }
	if (ActiveNativeUpdate->bDeferLastFrameWrite) { Super::SetLastFrameCameraCachePOV(GetCameraCacheView()); }
	Super::SetCameraCachePOV(InPOV);
	ActiveNativeUpdate->bCachePublished = true;
}
