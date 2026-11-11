/** @file GGYGOCombatHitQuery.cpp @brief 不推进技能时钟的真实范围重叠与弹体段查询。 */
#include "Combat/HitDetection/GGYGOCombatHitQuery.h"

#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

namespace GGYGOCombatHitQueryPrivate
{
	bool IsValidSphere(const FVector& Position, float Radius, FString& OutError)
	{
		if (Position.ContainsNaN() || !FMath::IsFinite(Radius)
			|| Radius <= FCollisionShape::MinSphereRadius())
		{
			OutError = TEXT("球查询需要有限世界坐标及大于碰撞零形状阈值的半径。");
			return false;
		}
		return true;
	}
}

TSharedPtr<FGGYGOCombatHitQuery> FGGYGOCombatHitQuery::Open(AActor* SourceActor, FString& OutError,
	TConstArrayView<AActor*> IgnoredActors)
{
	check(IsInGameThread());
	OutError.Reset();
	if (!IsValid(SourceActor) || SourceActor->IsActorBeingDestroyed() || !SourceActor->HasAuthority()
		|| !IsValid(SourceActor->GetWorld()) || !SourceActor->GetWorld()->GetPhysicsScene())
	{
		OutError = TEXT("Combat 查询开启失败：需要有效权威来源及其物理世界。");
		return nullptr;
	}
	for (AActor* IgnoredActor : IgnoredActors)
	{
		if (!IsValid(IgnoredActor) || IgnoredActor->GetWorld() != SourceActor->GetWorld())
		{
			OutError = TEXT("Combat 查询开启失败：附加忽略 Actor 无效或不属于来源世界。");
			return nullptr;
		}
	}
	return MakeShareable(new FGGYGOCombatHitQuery(SourceActor, IgnoredActors));
}

FGGYGOCombatHitQuery::FGGYGOCombatHitQuery(AActor* SourceActor, TConstArrayView<AActor*> IgnoredActors)
	: Source(SourceActor), World(SourceActor->GetWorld())
{
	for (AActor* Actor : IgnoredActors) { Ignored.AddUnique(Actor); }
}

bool FGGYGOCombatHitQuery::PrepareQuery(ECollisionChannel Channel, FCollisionQueryParams& OutParams,
	FString& OutError) const
{
	check(IsInGameThread());
	AActor* SourceActor = Source.Get();
	if (!SourceActor || SourceActor->IsActorBeingDestroyed() || !SourceActor->HasAuthority()
		|| !World.IsValid() || SourceActor->GetWorld() != World.Get() || !World->GetPhysicsScene())
	{
		OutError = TEXT("Combat 查询拒绝：实例已关闭、原来源失效／失去权威或原物理世界已改变。");
		return false;
	}
	if (static_cast<uint8>(Channel) >= static_cast<uint8>(ECC_OverlapAll_Deprecated))
	{
		OutError = TEXT("Combat 查询拒绝：碰撞通道不是有效的物理查询通道。");
		return false;
	}
	OutParams.AddIgnoredActor(SourceActor);
	for (const TWeakObjectPtr<AActor>& Actor : Ignored)
	{
		if (Actor.IsValid()) { OutParams.AddIgnoredActor(Actor.Get()); }
	}
	OutParams.bFindInitialOverlaps = true;
	OutParams.bReturnPhysicalMaterial = true;
	return true;
}

bool FGGYGOCombatHitQuery::RecordActor(AActor* Actor)
{
	if (!IsValid(Actor)) { return false; }
	const TWeakObjectPtr<AActor> Identity(Actor);
	if (HitActors.Contains(Identity)) { return false; }
	HitActors.Add(Identity);
	return true;
}

bool FGGYGOCombatHitQuery::OverlapSphere(const FVector& Center, float Radius, ECollisionChannel Channel,
	TArray<FGGYGOCombatOverlapFact>& OutFacts, FString& OutError)
{
	OutFacts.Reset();
	OutError.Reset();
	if (!GGYGOCombatHitQueryPrivate::IsValidSphere(Center, Radius, OutError)) { return false; }
	return OverlapShape(Center, FQuat::Identity, FCollisionShape::MakeSphere(Radius), Channel, OutFacts, OutError);
}

bool FGGYGOCombatHitQuery::OverlapBox(const FVector& Center, const FQuat& Rotation, const FVector& HalfExtents,
	ECollisionChannel Channel, TArray<FGGYGOCombatOverlapFact>& OutFacts, FString& OutError)
{
	OutFacts.Reset();
	OutError.Reset();
	if (Center.ContainsNaN() || Rotation.ContainsNaN() || !Rotation.IsNormalized()
		|| HalfExtents.ContainsNaN() || HalfExtents.GetMin() <= 0.0
		|| !FMath::IsFinite(static_cast<float>(HalfExtents.GetMax())))
	{
		OutError = TEXT("盒查询需要有限世界坐标、单位旋转及可表示的正半尺寸。");
		return false;
	}
	const FCollisionShape Shape = FCollisionShape::MakeBox(HalfExtents);
	if (Shape.IsNearlyZero())
	{
		OutError = TEXT("盒尺寸低于有效碰撞形状阈值。");
		return false;
	}
	return OverlapShape(Center, Rotation, Shape, Channel, OutFacts, OutError);
}

bool FGGYGOCombatHitQuery::OverlapShape(const FVector& Center, const FQuat& Rotation, const FCollisionShape& Shape,
	ECollisionChannel Channel, TArray<FGGYGOCombatOverlapFact>& OutFacts, FString& OutError)
{
	FCollisionQueryParams Params(SCENE_QUERY_STAT(GGYGOCombatHitQuery), false);
	if (!PrepareQuery(Channel, Params, OutError)) { return false; }
	TArray<FOverlapResult> Overlaps;
	// 引擎 bool 只代表是否含阻挡；只有 Touch 的重叠数组同样是真实有效结果。
	World->OverlapMultiByChannel(Overlaps, Center, Rotation, Channel, Shape, Params);
	for (const FOverlapResult& Overlap : Overlaps)
	{
		if (!RecordActor(Overlap.GetActor())) { continue; }
		FGGYGOCombatOverlapFact& Fact = OutFacts.AddDefaulted_GetRef();
		Fact.SourceActor = Source;
		Fact.Origin = Center;
		Fact.Overlap = Overlap;
	}
	return true;
}

bool FGGYGOCombatHitQuery::SweepSphere(const FVector& From, const FVector& To, float Radius,
	ECollisionChannel Channel, TArray<FGGYGOCombatSweepFact>& OutFacts, FHitResult& OutBlockingHit, FString& OutError)
{
	OutFacts.Reset();
	OutBlockingHit = FHitResult();
	OutError.Reset();
	if (!GGYGOCombatHitQueryPrivate::IsValidSphere(From, Radius, OutError)) { return false; }
	if (To.ContainsNaN() || (To - From).ContainsNaN())
	{
		OutError = TEXT("弹体扫掠终点或段位移非有限值。");
		return false;
	}
	FCollisionQueryParams Params(SCENE_QUERY_STAT(GGYGOCombatHitQuery), false);
	if (!PrepareQuery(Channel, Params, OutError)) { return false; }
	TArray<FHitResult> Hits;
	World->SweepMultiByChannel(Hits, From, To, FQuat::Identity, Channel, FCollisionShape::MakeSphere(Radius), Params);
	for (const FHitResult& Hit : Hits)
	{
		if (Hit.bBlockingHit && (!OutBlockingHit.bBlockingHit || Hit.Time < OutBlockingHit.Time))
		{
			OutBlockingHit = Hit;
		}
		if (!RecordActor(Hit.GetActor())) { continue; }
		FGGYGOCombatSweepFact& Fact = OutFacts.AddDefaulted_GetRef();
		Fact.SourceActor = Source;
		Fact.Origin = From;
		Fact.Hit = Hit;
	}
	return true;
}

void FGGYGOCombatHitQuery::Close()
{
	check(IsInGameThread());
	Source.Reset();
	World.Reset();
	Ignored.Reset();
	HitActors.Reset();
}
