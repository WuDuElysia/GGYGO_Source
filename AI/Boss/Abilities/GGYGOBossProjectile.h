/** @file GGYGOBossProjectile.h @brief GA 独占的原生弹体，Combat 唯一查询碰撞 */
#pragma once

#include "GameFramework/Actor.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "GGYGOBossProjectile.generated.h"

class USphereComponent;
class UNiagaraComponent;
class UNiagaraSystem;
class FGGYGOCombatHitQuery;
struct FGGYGOCombatSweepFact;
DECLARE_DELEGATE_OneParam(FGGYGOBossProjectileHit, const FGGYGOCombatSweepFact&);
DECLARE_DELEGATE_OneParam(FGGYGOBossProjectileFailure, const FString&);

/** 复用原生 ProjectileMovement 的每个真实移动段，不新增帧调度器。 */
UCLASS()
class GGYGO_API UGGYGOBossProjectileMovement : public UProjectileMovementComponent
{
	GENERATED_BODY()
protected:
	virtual bool MoveUpdatedComponentImpl(const FVector& Delta, const FQuat& NewRotation,
		bool bSweep, FHitResult* OutHit, ETeleportType Teleport) override;
};

UCLASS(NotBlueprintable)
class GGYGO_API AGGYGOBossProjectile : public AActor
{
	GENERATED_BODY()
public:
	AGGYGOBossProjectile();
	bool Configure(AActor* Source, float Radius, float Speed, float Lifetime, float Gravity,
		ECollisionChannel Channel, UNiagaraSystem* System, const FTransform& VisualTransform, FGGYGOBossProjectileHit Hit,
		FGGYGOBossProjectileFailure Failure, FString& Error);
	void QueryMovementSegment(const FVector& From, const FVector& To);
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Props) const override;
protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
	UPROPERTY()
	TObjectPtr<USphereComponent> Sphere;
	UPROPERTY()
	TObjectPtr<UGGYGOBossProjectileMovement> Movement;
	UPROPERTY()
	TObjectPtr<UNiagaraComponent> Visual;
	UPROPERTY(ReplicatedUsing = OnRep_Visual)
	TObjectPtr<UNiagaraSystem> VisualSystem;
	UPROPERTY(ReplicatedUsing = OnRep_Visual)
	FTransform VisualLocalTransform;
	UFUNCTION()
	void OnRep_Visual();
	TSharedPtr<FGGYGOCombatHitQuery> Query;
	FGGYGOBossProjectileHit HitCallback;
	FGGYGOBossProjectileFailure FailureCallback;
	float QueryRadius = 0.0f;
	ECollisionChannel QueryChannel = ECC_Pawn;
};
