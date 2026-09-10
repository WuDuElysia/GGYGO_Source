/**
 * @file GGYGOMeleeTraceComponent.cpp
 * @brief 近战判定实现
 */
#include "Combat/HitDetection/GGYGOMeleeTraceComponent.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOMeleeTraceComponent)

UGGYGOMeleeTraceComponent::UGGYGOMeleeTraceComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = true;

	// 默认不 Tick，只在判定窗口内开启。近战判定是短暂的，
	// 常开 Tick 会让场上每个角色每帧都做一次无用的扫掠。
	PrimaryComponentTick.bStartWithTickEnabled = false;
}

void UGGYGOMeleeTraceComponent::BeginTraceWindow(FName InStartSocket, FName InEndSocket, float InTraceRadius)
{
	StartSocket = InStartSocket;
	EndSocket = InEndSocket;
	TraceRadius = FMath::Max(InTraceRadius, 1.0f);

	bIsTracing = true;

	// 清空已命中记录：连招的每一段都是独立的一次攻击，
	// 同一个敌人应当能被每段各命中一次。
	HitActorsThisWindow.Reset();

	// 丢弃上一帧端点。窗口刚开启时武器可能已经移动了一段距离，
	// 沿用旧端点会扫出一条不属于本次攻击的长路径，命中身后的敌人。
	bHasPreviousTransform = false;

	SetComponentTickEnabled(true);
}

void UGGYGOMeleeTraceComponent::EndTraceWindow()
{
	bIsTracing = false;
	bHasPreviousTransform = false;

	SetComponentTickEnabled(false);
}

void UGGYGOMeleeTraceComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (bIsTracing)
	{
		PerformTrace();
	}
}

FVector UGGYGOMeleeTraceComponent::GetSocketLocation(const USkeletalMeshComponent* Mesh, FName SocketName) const
{
	if (!Mesh)
	{
		return FVector::ZeroVector;
	}

	// DoesSocketExist 同时覆盖 socket 与骨骼名，所以武器挂点用哪种都行。
	if (SocketName.IsNone() || !Mesh->DoesSocketExist(SocketName))
	{
		// 回退到组件原点而不是返回零向量：零向量会让扫掠从世界原点开始，
		// 扫过整张地图。
		return Mesh->GetComponentLocation();
	}

	return Mesh->GetSocketLocation(SocketName);
}

void UGGYGOMeleeTraceComponent::PerformTrace()
{
	const AActor* Owner = GetOwner();
	const UWorld* World = GetWorld();
	if (!Owner || !World)
	{
		return;
	}

	const ACharacter* OwnerCharacter = Cast<ACharacter>(Owner);
	const USkeletalMeshComponent* Mesh = OwnerCharacter ? OwnerCharacter->GetMesh() : nullptr;
	if (!Mesh)
	{
		return;
	}

	const FVector CurrentStart = GetSocketLocation(Mesh, StartSocket);
	const FVector CurrentEnd = GetSocketLocation(Mesh, EndSocket);

	if (!bHasPreviousTransform)
	{
		// 第一帧只建立基线，不判定。此时没有路径可扫。
		PreviousStart = CurrentStart;
		PreviousEnd = CurrentEnd;
		bHasPreviousTransform = true;
		return;
	}

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(GGYGOMeleeTrace), /*bTraceComplex=*/false);
	QueryParams.AddIgnoredActor(Owner);

	// 沿武器长度分段扫掠。
	//
	// 只扫一条"武器根到武器尖"的胶囊是不够的：那样只能检测垂直于武器的接触，
	// 而挥砍时是武器的**侧面**扫过敌人。分段后每段各自做一次从上一帧位置到
	// 本帧位置的扫掠，合起来覆盖了武器扫过的整个面。
	constexpr int32 SegmentCount = 4;

	TArray<FHitResult> Hits;

	for (int32 SegmentIndex = 0; SegmentIndex <= SegmentCount; ++SegmentIndex)
	{
		const float Ratio = static_cast<float>(SegmentIndex) / static_cast<float>(SegmentCount);

		const FVector From = FMath::Lerp(PreviousStart, PreviousEnd, Ratio);
		const FVector To = FMath::Lerp(CurrentStart, CurrentEnd, Ratio);

		Hits.Reset();

		// SweepMulti 而不是 Single：一次挥砍要能同时打到多个敌人。
		World->SweepMultiByChannel(
			Hits,
			From,
			To,
			FQuat::Identity,
			TraceChannel,
			FCollisionShape::MakeSphere(TraceRadius),
			QueryParams);

		for (const FHitResult& Hit : Hits)
		{
			AActor* HitActor = Hit.GetActor();
			if (!HitActor)
			{
				continue;
			}

			// 本次窗口内已命中过就跳过。不去重会让一刀造成多次伤害，
			// 且伤害量随帧率变化。
			const bool bAlreadyHit = HitActorsThisWindow.ContainsByPredicate(
				[HitActor](const TWeakObjectPtr<AActor>& Weak) { return Weak.Get() == HitActor; });

			if (bAlreadyHit)
			{
				continue;
			}

			HitActorsThisWindow.Add(HitActor);

			OnMeleeHit.Broadcast(HitActor, Hit);
		}
	}

	PreviousStart = CurrentStart;
	PreviousEnd = CurrentEnd;
}
