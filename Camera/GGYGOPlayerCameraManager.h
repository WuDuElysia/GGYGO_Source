#pragma once

#include "Camera/PlayerCameraManager.h"
#include "Camera/GGYGOCameraMode.h"

#include "GGYGOPlayerCameraManager.generated.h"

class UGGYGOCameraComponent;
struct FGGYGONativeCameraUpdateScope;

/** Publication admission only. The native manager remains the sole camera updater/cache. */
enum class EGGYGOCameraPublicationState : uint8
{
	NotActivated,
	Running,
	Stopped
};

UCLASS()
class GGYGO_API AGGYGOPlayerCameraManager : public APlayerCameraManager
{
	GENERATED_BODY()

public:
	AGGYGOPlayerCameraManager(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/**
	 * One explicit first Ready from the provider's real initialization lifecycle.
	 * ExpectedTarget must own the exact selected, registered GG camera. Before Ready,
	 * native UpdateCamera calls do nothing; no provider polling is performed here.
	 * A repeated Ready for the admitted identity is idempotent, never a Restart.
	 */
	FGGYGOCameraEvaluationResult ActivateCameraEvaluation(
		AActor* ExpectedTarget, UGGYGOCameraComponent* ExpectedComponent);

	/**
	 * Explicit retry of the exact original failed target/component, controller and world.
	 * A GG source is rebuilt through its existing ReinitializeCameraEvaluation first.
	 * For an explicitly selected native camera failure, ExpectedComponent is null.
	 * No new target/source is substituted if that original identity has gone away.
	 */
	FGGYGOCameraEvaluationResult RestartCameraEvaluation(
		AActor* ExpectedOriginalTarget, UGGYGOCameraComponent* ExpectedOriginalComponent);

	/** LP's later render gate may allow a stopped manager only when this is true. */
	bool HasConfirmedNativeCameraView() const;
	EGGYGOCameraPublicationState GetCameraPublicationState() const { return PublicationState; }
	const FGGYGOCameraEvaluationResult& GetCameraPublicationResult() const { return PublicationResult; }

	virtual void InitializeFor(APlayerController* PC) override;
	virtual void UpdateCamera(float DeltaTime) override;
	/** Pause-only native entry obeys the same admission state as UpdateCamera. */
	virtual void UpdateCameraPhotographyOnly() override;
	virtual void SetViewTarget(AActor* NewViewTarget,
		FViewTargetTransitionParams TransitionParams = FViewTargetTransitionParams()) override;
	virtual void ApplyCameraModifiers(float DeltaTime, FMinimalViewInfo& InOutPOV) override;
	virtual void SetCameraCachePOV(const FMinimalViewInfo& InPOV) override;
	virtual void SetLastFrameCameraCachePOV(const FMinimalViewInfo& InPOV) override;

protected:
	virtual void DoUpdateCamera(float DeltaTime) override;
	virtual void UpdateViewTarget(FTViewTarget& OutVT, float DeltaTime) override;
	virtual bool UpdatePhotographyCamera(FMinimalViewInfo& NewPOV) override;

private:
	FGGYGOCameraEvaluationResult ValidateOriginalSource(
		AActor* Target, UGGYGOCameraComponent* Component) const;
	FGGYGOCameraEvaluationResult RunExplicitAttempt();
	void RejectCameraPublication(const FGGYGOCameraEvaluationResult& Failure,
		AActor* OriginalTarget, UGGYGOCameraComponent* OriginalComponent);
	void RejectCurrentUpdate(const FGGYGOCameraEvaluationResult& Failure);
	void ValidateNativeBlendBeforeFill();
	void ReportExternalCacheWrite();

	EGGYGOCameraPublicationState PublicationState = EGGYGOCameraPublicationState::NotActivated;
	FGGYGOCameraEvaluationResult PublicationResult;
	bool bHasConfirmedNativeView = false;
	bool bNativeInitialization = false;
	bool bExplicitRequestBusy = false;
	bool bReportedExternalCacheWrite = false;

	// Identity/provenance only; neither a copied POV nor another provider readiness state.
	TWeakObjectPtr<APlayerController> AdmissionController;
	TWeakObjectPtr<UWorld> AdmissionWorld;
	TWeakObjectPtr<AActor> AdmissionTarget;
	TWeakObjectPtr<UGGYGOCameraComponent> AdmissionComponent;
	TWeakObjectPtr<AActor> FailureTarget;
	TWeakObjectPtr<UGGYGOCameraComponent> FailureComponent;
	bool bFailureHadComponent = false;

	// Nonowning pointer to one synchronous native call's stack-local scratch/rollback data.
	FGGYGONativeCameraUpdateScope* ActiveNativeUpdate = nullptr;
};
