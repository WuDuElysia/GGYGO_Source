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

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOMeleeTrace, Log, All);

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
	EndTraceWindow();
	// EndTraceWindow 已统一使上一窗口失效并清空命中去重与采样基线。
	USkeletalMeshComponent* Mesh = GetTraceMesh();
	if (!Mesh || InStartSocket.IsNone() || InEndSocket.IsNone()
		|| !Mesh->DoesSocketExist(InStartSocket) || !Mesh->DoesSocketExist(InEndSocket)
		|| !FMath::IsFinite(InTraceRadius) || InTraceRadius < 1.0f || MaxTraceSegments < 1 || MaxTraceSegments > 512)
	{
		UE_LOG(LogGGYGOMeleeTrace, Warning, TEXT("MeleeTrace [%s] 拒绝无效 Mesh/Socket/半径/采样上限 [%s -> %s, R=%.2f]。"),
			*GetNameSafe(GetOwner()), *InStartSocket.ToString(), *InEndSocket.ToString(), InTraceRadius);
		return;
	}
	StartSocket = InStartSocket;
	EndSocket = InEndSocket;
	TraceRadius = FMath::Max(InTraceRadius, 1.0f);

	bIsTracing = true;

	SetComponentTickEnabled(true);
}

void UGGYGOMeleeTraceComponent::EndTraceWindow()
{
	// 只有活动窗口需要使当前扫掠的快照失效；重复清理保持幂等。
	if (bIsTracing)
	{
		++WindowSerial;
	}
	bIsTracing = false;
	PreviousStart = FVector::ZeroVector;
	PreviousEnd = FVector::ZeroVector;
	bHasPreviousTransform = false;
	HitActorsThisWindow.Reset();

	SetComponentTickEnabled(false);
}

void UGGYGOMeleeTraceComponent::OnUnregister()
{
	EndTraceWindow();
	Super::OnUnregister();
}

void UGGYGOMeleeTraceComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	EndTraceWindow();
	Super::EndPlay(EndPlayReason);
}

void UGGYGOMeleeTraceComponent::Deactivate()
{
	EndTraceWindow();
	Super::Deactivate();
}

void UGGYGOMeleeTraceComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (bIsTracing)
	{
		PerformTrace();
	}
}

USkeletalMeshComponent* UGGYGOMeleeTraceComponent::GetTraceMesh() const
{
	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	return Character ? Character->GetMesh() : nullptr;
}

void UGGYGOMeleeTraceComponent::PerformTrace()
{
	const AActor* Owner = GetOwner();
	const UWorld* World = GetWorld();
	if (!Owner || !World || !bIsTracing)
	{
		return;
	}

	const USkeletalMeshComponent* Mesh = GetTraceMesh();
	if (!Mesh || !Mesh->DoesSocketExist(StartSocket) || !Mesh->DoesSocketExist(EndSocket))
	{
		EndTraceWindow();
		return;
	}

	const uint32 TraceSerial = WindowSerial;
	const FVector CurrentStart = Mesh->GetSocketLocation(StartSocket);
	const FVector CurrentEnd = Mesh->GetSocketLocation(EndSocket);
	if (CurrentStart.ContainsNaN() || CurrentEnd.ContainsNaN()) { EndTraceWindow(); return; }
	const double MaxLength = FMath::Max(FVector::Distance(CurrentStart, CurrentEnd),
		bHasPreviousTransform ? FVector::Distance(PreviousStart, PreviousEnd) : 0.0);
	// 采样间距不超过半径，长武器不会因固定等分数在长度方向留下空洞。
	const double RequiredSegments = FMath::Max(1.0, FMath::CeilToDouble(MaxLength / TraceRadius));
	if (RequiredSegments > MaxTraceSegments)
	{
		UE_LOG(LogGGYGOMeleeTrace, Warning,
			TEXT("MeleeTrace [%s] 所需分段 %.0f 超过上限 %d；长度 %.1f、半径 %.1f，关闭窗口。"),
			*GetNameSafe(GetOwner()), RequiredSegments, MaxTraceSegments, MaxLength, TraceRadius);
		EndTraceWindow();
		return;
	}
	const int32 SegmentCount = static_cast<int32>(RequiredSegments);

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
	QueryParams.bReturnPhysicalMaterial = true;

	// 沿武器长度分段扫掠。
	//
	// 每个采样点从上帧位置扫到本帧位置；离散帧之间按直线近似，
	// 大角度旋转时仍不能声称还原了武器的真实弧形运动。
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
			if (!bIsTracing || WindowSerial != TraceSerial) { return; }
		}
	}

	PreviousStart = CurrentStart;
	PreviousEnd = CurrentEnd;
}
