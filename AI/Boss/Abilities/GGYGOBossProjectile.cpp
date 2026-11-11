#include "AI/Boss/Abilities/GGYGOBossProjectile.h"
#include "Combat/HitDetection/GGYGOCombatHitQuery.h"
#include "Components/SphereComponent.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOBossProjectile)

bool UGGYGOBossProjectileMovement::MoveUpdatedComponentImpl(const FVector& Delta, const FQuat& Rotation,
	bool bSweep, FHitResult* OutHit, ETeleportType Teleport)
{
	const FVector From = UpdatedComponent ? UpdatedComponent->GetComponentLocation() : FVector::ZeroVector;
	const bool bMoved = Super::MoveUpdatedComponentImpl(Delta, Rotation, bSweep, OutHit, Teleport);
	if (bMoved && UpdatedComponent)
	{
		if (AGGYGOBossProjectile* Projectile = Cast<AGGYGOBossProjectile>(GetOwner()))
		{ Projectile->QueryMovementSegment(From, UpdatedComponent->GetComponentLocation()); }
	}
	return bMoved;
}

AGGYGOBossProjectile::AGGYGOBossProjectile()
{
	bReplicates = true;
	SetReplicateMovement(true);
	PrimaryActorTick.bCanEverTick = false;
	Sphere = CreateDefaultSubobject<USphereComponent>(TEXT("ProjectileRoot"));
	SetRootComponent(Sphere);
	Sphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Movement = CreateDefaultSubobject<UGGYGOBossProjectileMovement>(TEXT("ProjectileMovement"));
	Movement->SetUpdatedComponent(Sphere);
	Movement->bRotationFollowsVelocity = true;
	Movement->bAutoActivate = false;
	Visual = CreateDefaultSubobject<UNiagaraComponent>(TEXT("ProjectileVisual"));
	Visual->SetupAttachment(Sphere);
	Visual->bAutoActivate = false;
	Visual->SetAutoDestroy(false);
}

bool AGGYGOBossProjectile::Configure(AActor* Source, float Radius, float Speed, float Lifetime,
	float Gravity, ECollisionChannel Channel, UNiagaraSystem* System, const FTransform& VisualTransform, FGGYGOBossProjectileHit Hit,
	FGGYGOBossProjectileFailure Failure, FString& Error)
{
	if (!HasAuthority() || !IsValid(Source) || !FMath::IsFinite(Radius) || Radius <= 0.0f
		|| !FMath::IsFinite(Speed) || Speed <= 0.0f || !FMath::IsFinite(Lifetime) || Lifetime <= 0.0f
		|| !FMath::IsFinite(Gravity) || Gravity < 0.0f || !System || !VisualTransform.IsValid()
		|| !Hit.IsBound() || !Failure.IsBound())
	{ Error = TEXT("弹体需要权威来源、正半径/速度/寿命、非负重力与原接收者。"); return false; }
	AActor* Ignored[] = {this};
	Query = FGGYGOCombatHitQuery::Open(Source, Error, MakeArrayView(Ignored));
	if (!Query) { return false; }
	SetOwner(Source);
	VisualSystem = System;
	VisualLocalTransform = VisualTransform;
	QueryRadius = Radius;
	QueryChannel = Channel;
	Sphere->SetSphereRadius(Radius);
	HitCallback = MoveTemp(Hit);
	FailureCallback = MoveTemp(Failure);
	Movement->InitialSpeed = Speed;
	Movement->MaxSpeed = Speed;
	Movement->ProjectileGravityScale = Gravity;
	Movement->Velocity = GetActorForwardVector() * Speed;
	SetLifeSpan(Lifetime);
	Movement->Activate();
	return true;
}

void AGGYGOBossProjectile::QueryMovementSegment(const FVector& From, const FVector& To)
{
	if (!HasAuthority() || !Query) { return; }
	TArray<FGGYGOCombatSweepFact> Hits;
	FHitResult BlockingHit;
	FString Error;
	if (!Query->SweepSphere(From, To, QueryRadius, QueryChannel, Hits, BlockingHit, Error))
	{
		const auto Failure = FailureCallback;
		Failure.ExecuteIfBound(Error);
		if (!IsActorBeingDestroyed()) { Destroy(); }
		return;
	}
	for (const FGGYGOCombatSweepFact& Fact : Hits)
	{
		const auto Callback = HitCallback;
		Callback.ExecuteIfBound(Fact);
		if (IsActorBeingDestroyed()) { return; }
	}
	if (BlockingHit.bBlockingHit) { Destroy(); }
}

void AGGYGOBossProjectile::EndPlay(const EEndPlayReason::Type Reason)
{
	HitCallback.Unbind();
	FailureCallback.Unbind();
	TSharedPtr<FGGYGOCombatHitQuery> Original = MoveTemp(Query);
	if (Original) { Original->Close(); }
	Movement->Deactivate();
	Visual->DeactivateImmediate();
	Super::EndPlay(Reason);
}

void AGGYGOBossProjectile::BeginPlay()
{
	Super::BeginPlay();
	OnRep_Visual();
}

void AGGYGOBossProjectile::OnRep_Visual()
{
	if (!HasActorBegunPlay() || !VisualSystem || GetNetMode() == NM_DedicatedServer) { return; }
	Visual->SetRelativeTransform(VisualLocalTransform);
	if (Visual->GetAsset() != VisualSystem) { Visual->SetAsset(VisualSystem); }
	if (!Visual->IsActive()) { Visual->Activate(); }
}

void AGGYGOBossProjectile::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION(AGGYGOBossProjectile, VisualSystem, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AGGYGOBossProjectile, VisualLocalTransform, COND_InitialOnly);
}
