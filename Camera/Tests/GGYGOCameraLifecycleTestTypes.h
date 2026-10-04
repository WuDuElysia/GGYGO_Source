#pragma once

#include "AbilitySystem/Abilities/GGYGOGameplayAbility.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Camera/GGYGOCameraComponent.h"
#include "Camera/GGYGOCameraMode.h"
#include "Character/Components/GGYGOHeroComponent.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Combatants/GGYGOCombatantState.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Pawn.h"

#include "GGYGOCameraLifecycleTestTypes.generated.h"

/** Asset-free camera mode whose defaults are configured by automation tests. */
UCLASS(Transient)
class UGGYGOCameraLifecycleTestModeBase : public UGGYGOCameraMode
{
	GENERATED_BODY()

public:
	void ConfigureDefaultsForTest(const FVector& Location, const FRotator& Rotation, float FieldOfView,
		const FGGYGOCameraPenetrationRequest& PenetrationRequest, float InBlendTime = 0.0f);

protected:
	virtual void OnActivation() override;
	virtual void UpdateView(float DeltaTime) override;

private:
	UPROPERTY()
	FVector TestViewLocation = FVector::ZeroVector;

	UPROPERTY()
	FRotator TestViewRotation = FRotator::ZeroRotator;

	UPROPERTY()
	float TestFieldOfView = 80.0f;

	UPROPERTY()
	bool bTestPenetrationEnabled = false;

	UPROPERTY()
	FVector TestPenetrationPivot = FVector::ZeroVector;

	UPROPERTY()
	float TestPenetrationProbeRadius = 0.0f;

	UPROPERTY()
	float TestPenetrationRecoverySpeed = 0.0f;
};

UCLASS(Transient)
class UGGYGOCameraLifecycleTestModeA : public UGGYGOCameraLifecycleTestModeBase
{
	GENERATED_BODY()
};

UCLASS(Transient)
class UGGYGOCameraLifecycleTestModeB : public UGGYGOCameraLifecycleTestModeBase
{
	GENERATED_BODY()
};

UCLASS(Transient)
class UGGYGOCameraLifecycleTestModeC : public UGGYGOCameraLifecycleTestModeBase
{
	GENERATED_BODY()
};

/** Exposes only the protected production camera entry points needed by tests. */
UCLASS(Transient)
class UGGYGOCameraLifecycleTestComponent : public UGGYGOCameraComponent
{
	GENERATED_BODY()

public:
	FGGYGOCameraEvaluationResult PushModeForTest(TSubclassOf<UGGYGOCameraMode> ModeClass);
	FMinimalViewInfo EvaluateViewForTest(float DeltaTime);
	/** Copies the caller's original output for this call only; the native getter remains void. */
	FMinimalViewInfo EvaluateViewForTest(float DeltaTime, const FMinimalViewInfo& OriginalView);
	FGGYGOCameraEvaluationResult EvaluateStackForTest(float DeltaTime, FGGYGOCameraModeView& OutView,
		FGGYGOCameraPenetrationRequest& OutRequest);
	bool IsModeStackActiveForTest() const;
	FGGYGOCameraEvaluationResult AdvanceOffsetForTest(float DeltaTime) { return UpdateCameraOffsetAlpha(DeltaTime); }
	float GetOffsetAlphaForTest() const { return CameraOffsetAlpha; }
	// BEGIN 05-B6-T1 read-only Offset observation
	bool IsOffsetActiveForTest() const { return bCameraOffsetActive; }
	const FGGYGOCameraOffset& GetOffsetForTest() const { return CameraOffset; }
	// END 05-B6-T1 read-only Offset observation
};

/** Hero subtype used by the minimal Pawn fixtures. */
UCLASS(Transient)
class UGGYGOCameraLifecycleTestHeroComponent : public UGGYGOHeroComponent
{
	GENERATED_BODY()
};

/** Minimal Pawn with the real Hero/ASC components and a test camera component. */
UCLASS(Transient)
class AGGYGOCameraLifecycleTestPawn : public APawn
{
	GENERATED_BODY()

public:
	AGGYGOCameraLifecycleTestPawn(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	UGGYGOCameraLifecycleTestHeroComponent* GetHeroForTest() const { return HeroComponent; }
	UGGYGOCameraLifecycleTestComponent* GetCameraForTest() const { return CameraComponent; }
	UGGYGOAbilitySystemComponent* GetAbilitySystemForTest() const { return AbilitySystemComponent; }

private:
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneComponent> TestRootComponent;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UGGYGOCameraLifecycleTestHeroComponent> HeroComponent;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UGGYGOCameraLifecycleTestComponent> CameraComponent;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UGGYGOAbilitySystemComponent> AbilitySystemComponent;
};

/** Instanced GA fixture that uses the production camera lifecycle entry points. */
UCLASS(Transient)
class UGGYGOCameraLifecycleTestAbility : public UGGYGOGameplayAbility
{
	GENERATED_BODY()

public:
	UGGYGOCameraLifecycleTestAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	void ConfigureCameraForTest(TSubclassOf<UGGYGOCameraMode> ModeClass, const FGGYGOCameraOffset& Offset);
	void FinishForTest();
};

/** Distinct Spec class used to test that Hero generation matching also checks identity. */
UCLASS(Transient)
class UGGYGOCameraLifecycleTestSecondaryAbility : public UGGYGOGameplayAbility
{
	GENERATED_BODY()
};

// BEGIN 05-B6-T1 independent fixture types
/** Real PawnExtension/Hero lifecycle; the ASC belongs exclusively to the external host. */
UCLASS(Transient)
class AGGYGOCameraRealUninitializeTestPawn : public APawn
{
	GENERATED_BODY()

public:
	AGGYGOCameraRealUninitializeTestPawn(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	UGGYGOPawnExtensionComponent* GetPawnExtensionForTest() const { return PawnExtensionComponent; }
	UGGYGOHeroComponent* GetHeroForTest() const { return HeroComponent; }
	UGGYGOCameraLifecycleTestComponent* GetCameraForTest() const { return CameraComponent; }

private:
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneComponent> TestRootComponent;
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UGGYGOPawnExtensionComponent> PawnExtensionComponent;
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UGGYGOHeroComponent> HeroComponent;
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UGGYGOCameraLifecycleTestComponent> CameraComponent;
};

/** Concrete test host inherits the production ASC and public Attach/Detach implementation. */
UCLASS(Transient)
class AGGYGOCameraRealUninitializeTestHost : public AGGYGOCombatantState
{
	GENERATED_BODY()
};

/** The CDO asset tag, rather than a runtime instance tag, opts out of detach cancellation. */
UCLASS(Transient)
class UGGYGOCameraSurvivingLifecycleTestAbility : public UGGYGOCameraLifecycleTestAbility
{
	GENERATED_BODY()

public:
	UGGYGOCameraSurvivingLifecycleTestAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};
// END 05-B6-T1 independent fixture types
