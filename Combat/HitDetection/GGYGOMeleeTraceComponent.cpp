/**
 * @file GGYGOMeleeTraceComponent.cpp
 * @brief 近战判定实现
 */
#include "Combat/HitDetection/GGYGOMeleeTraceComponent.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "Templates/UnrealTemplate.h"

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
	check(IsInGameThread());
	if (bIsTracing && WindowSource == EWindowSource::Owned)
	{
		UE_LOG(LogGGYGOMeleeTrace, Warning,
			TEXT("MeleeTrace [%s/%s] 拒绝 Legacy Begin 接管活动 Owned 窗口 [%s -> %s, R=%.2f]。"),
			*GetNameSafe(GetOwner()), *GetName(), *InStartSocket.ToString(), *InEndSocket.ToString(), InTraceRadius);
		return;
	}
	CloseCurrentTraceWindow();
	StartTraceWindow(InStartSocket, InEndSocket, InTraceRadius, EWindowSource::Legacy);
}

EGGYGOMeleeTraceWindowOpenResult UGGYGOMeleeTraceComponent::TryOpenOwnedTraceWindow(
	FName InStartSocket, FName InEndSocket, float InTraceRadius,
	const FGGYGOMeleeTraceWindowHandle& ExpectedWindow, FGGYGOMeleeTraceWindowHandle& OutWindow)
{
	check(IsInGameThread());
	// 调用者可原地替换自己的句柄；不能先清空 Out 而抹掉预期身份。
	const FGGYGOMeleeTraceWindowHandle ExpectedSnapshot = ExpectedWindow;
	OutWindow = FGGYGOMeleeTraceWindowHandle();
	if (ExpectedSnapshot.HasWindow())
	{
		const EGGYGOMeleeTraceWindowQueryResult ExpectedState = QueryOwnedTraceWindow(ExpectedSnapshot);
		if (ExpectedState != EGGYGOMeleeTraceWindowQueryResult::Active)
		{
			UE_LOG(LogGGYGOMeleeTrace, Warning,
				TEXT("MeleeTrace [%s/%s] 拒绝 Owned Begin：预期原窗口不是本组件活动 Owned 窗口 [%s -> %s, R=%.2f, Query=%d]。"),
				*GetNameSafe(GetOwner()), *GetName(), *InStartSocket.ToString(), *InEndSocket.ToString(), InTraceRadius,
				static_cast<int32>(ExpectedState));
			return ExpectedState == EGGYGOMeleeTraceWindowQueryResult::Inactive
				? EGGYGOMeleeTraceWindowOpenResult::WindowConflict
				: EGGYGOMeleeTraceWindowOpenResult::InvalidExpectedHandle;
		}
		CloseCurrentTraceWindow();
	}

	const EGGYGOMeleeTraceWindowOpenResult Result = StartTraceWindow(
		InStartSocket, InEndSocket, InTraceRadius, EWindowSource::Owned);
	if (Result == EGGYGOMeleeTraceWindowOpenResult::Opened)
	{
		OutWindow.Issuer = this;
		OutWindow.Serial = WindowSerial;
	}
	return Result;
}

EGGYGOMeleeTraceWindowOpenResult UGGYGOMeleeTraceComponent::StartTraceWindow(
	FName InStartSocket, FName InEndSocket, float InTraceRadius, EWindowSource Source)
{
	// 包括清理旧订阅期间发生重入的情况；不能覆盖刚取得窗口的新请求。
	if (bIsTracing)
	{
		UE_LOG(LogGGYGOMeleeTrace, Warning,
			TEXT("MeleeTrace [%s/%s] 拒绝开窗：已有活动窗口，必须提供匹配 Owned 身份或先结束 Legacy [%s -> %s, R=%.2f]。"),
			*GetNameSafe(GetOwner()), *GetName(), *InStartSocket.ToString(), *InEndSocket.ToString(), InTraceRadius);
		return EGGYGOMeleeTraceWindowOpenResult::WindowConflict;
	}
	const AActor* Owner = GetOwner();
	if (bClosingForLifecycle || !IsRegistered() || IsBeingDestroyed()
		|| !IsValid(Owner) || Owner->IsActorBeingDestroyed() || !GetWorld())
	{
		UE_LOG(LogGGYGOMeleeTrace, Warning,
			TEXT("MeleeTrace [%s/%s] 拒绝开窗：生命周期清理中、未注册、正在销毁或 Owner/World 不可用 [%s -> %s, R=%.2f]。"),
			*GetNameSafe(Owner), *GetName(), *InStartSocket.ToString(), *InEndSocket.ToString(), InTraceRadius);
		return EGGYGOMeleeTraceWindowOpenResult::Unavailable;
	}
	USkeletalMeshComponent* Mesh = GetTraceMesh();
	if (!Mesh || InStartSocket.IsNone() || InEndSocket.IsNone()
		|| !Mesh->DoesSocketExist(InStartSocket) || !Mesh->DoesSocketExist(InEndSocket)
		|| !FMath::IsFinite(InTraceRadius) || InTraceRadius < 1.0f || MaxTraceSegments < 1 || MaxTraceSegments > 512)
	{
		UE_LOG(LogGGYGOMeleeTrace, Warning, TEXT("MeleeTrace [%s/%s] 拒绝无效 Mesh/Socket/半径/采样上限 [%s -> %s, R=%.2f, MaxSegments=%d]。"),
			*GetNameSafe(Owner), *GetName(), *InStartSocket.ToString(), *InEndSocket.ToString(), InTraceRadius, MaxTraceSegments);
		return EGGYGOMeleeTraceWindowOpenResult::InvalidConfiguration;
	}
	if (WindowSerial == MAX_uint64)
	{
		UE_LOG(LogGGYGOMeleeTrace, Warning,
			TEXT("MeleeTrace [%s/%s] 拒绝开窗：WindowSerial 已耗尽，禁止回绕复用 [%s -> %s, R=%.2f]。"),
			*GetNameSafe(Owner), *GetName(), *InStartSocket.ToString(), *InEndSocket.ToString(), InTraceRadius);
		return EGGYGOMeleeTraceWindowOpenResult::SerialExhausted;
	}
	StartSocket = InStartSocket;
	EndSocket = InEndSocket;
	TraceRadius = InTraceRadius;
	++WindowSerial;
	WindowSource = Source;
	bIsTracing = true;
	SetComponentTickEnabled(true);
	return EGGYGOMeleeTraceWindowOpenResult::Opened;
}

void UGGYGOMeleeTraceComponent::EndTraceWindow()
{
	check(IsInGameThread());
	if (bIsTracing && WindowSource == EWindowSource::Owned)
	{
		UE_LOG(LogGGYGOMeleeTrace, Warning,
			TEXT("MeleeTrace [%s/%s] 拒绝 Legacy End 关闭活动 Owned 窗口 [%s -> %s, R=%.2f]。"),
			*GetNameSafe(GetOwner()), *GetName(), *StartSocket.ToString(), *EndSocket.ToString(), TraceRadius);
		return;
	}
	CloseCurrentTraceWindow();
}

EGGYGOMeleeTraceWindowCloseResult UGGYGOMeleeTraceComponent::CloseOwnedTraceWindow(
	const FGGYGOMeleeTraceWindowHandle& Window)
{
	check(IsInGameThread());
	switch (QueryOwnedTraceWindow(Window))
	{
	case EGGYGOMeleeTraceWindowQueryResult::Active:
		CloseCurrentTraceWindow();
		return EGGYGOMeleeTraceWindowCloseResult::Closed;
	case EGGYGOMeleeTraceWindowQueryResult::Inactive:
		return EGGYGOMeleeTraceWindowCloseResult::AlreadyInactive;
	case EGGYGOMeleeTraceWindowQueryResult::WrongComponent:
		return EGGYGOMeleeTraceWindowCloseResult::WrongComponent;
	case EGGYGOMeleeTraceWindowQueryResult::InvalidHandle:
	default:
		return EGGYGOMeleeTraceWindowCloseResult::InvalidHandle;
	}
}

EGGYGOMeleeTraceWindowQueryResult UGGYGOMeleeTraceComponent::QueryOwnedTraceWindow(
	const FGGYGOMeleeTraceWindowHandle& Window) const
{
	check(IsInGameThread());
	if (!Window.HasWindow() || Window.Issuer.IsExplicitlyNull())
	{
		return EGGYGOMeleeTraceWindowQueryResult::InvalidHandle;
	}
	if (!Window.Issuer.HasSameIndexAndSerialNumber(TWeakObjectPtr<UGGYGOMeleeTraceComponent>(
		const_cast<UGGYGOMeleeTraceComponent*>(this))))
	{
		return EGGYGOMeleeTraceWindowQueryResult::WrongComponent;
	}
	return WindowSource == EWindowSource::Owned && IsActiveWindow(Window.Serial)
		? EGGYGOMeleeTraceWindowQueryResult::Active : EGGYGOMeleeTraceWindowQueryResult::Inactive;
}

EGGYGOMeleeTraceWindowSubscribeResult UGGYGOMeleeTraceComponent::SubscribeWindowHit(
	const FGGYGOMeleeTraceWindowHandle& Window, FGGYGOMeleeTraceWindowHitDelegate Callback,
	FDelegateHandle& OutSubscription)
{
	check(IsInGameThread());
	OutSubscription.Reset();
	const EGGYGOMeleeTraceWindowQueryResult State = QueryOwnedTraceWindow(Window);
	if (State != EGGYGOMeleeTraceWindowQueryResult::Active || !Callback.IsBound())
	{
		UE_LOG(LogGGYGOMeleeTrace, Warning,
			TEXT("MeleeTrace [%s/%s] 拒绝原窗口命中订阅：窗口身份无效/不活动或回调未绑定 [Query=%d, Bound=%d]。"),
			*GetNameSafe(GetOwner()), *GetName(), static_cast<int32>(State), Callback.IsBound());
		switch (State)
		{
		case EGGYGOMeleeTraceWindowQueryResult::InvalidHandle:
			return EGGYGOMeleeTraceWindowSubscribeResult::InvalidHandle;
		case EGGYGOMeleeTraceWindowQueryResult::WrongComponent:
			return EGGYGOMeleeTraceWindowSubscribeResult::WrongComponent;
		case EGGYGOMeleeTraceWindowQueryResult::Inactive:
			return EGGYGOMeleeTraceWindowSubscribeResult::WindowInactive;
		case EGGYGOMeleeTraceWindowQueryResult::Active:
		default:
			return EGGYGOMeleeTraceWindowSubscribeResult::InvalidCallback;
		}
	}
	FWindowHitSubscription Subscription;
	Subscription.Window = Window;
	// 每次注册签发独立资源令牌，不能沿用被复制回调自身的 DelegateHandle。
	Subscription.Handle = FDelegateHandle(FDelegateHandle::GenerateNewHandle);
	Subscription.Callback = MoveTemp(Callback);
	OutSubscription = Subscription.Handle;
	WindowHitSubscriptions.Add(MoveTemp(Subscription));
	return EGGYGOMeleeTraceWindowSubscribeResult::Subscribed;
}

bool UGGYGOMeleeTraceComponent::UnsubscribeWindowHit(FDelegateHandle Subscription)
{
	check(IsInGameThread());
	const int32 Index = WindowHitSubscriptions.IndexOfByPredicate(
		[Subscription](const FWindowHitSubscription& Entry) { return Entry.Handle == Subscription; });
	if (Index == INDEX_NONE) { return false; }
	// 先从成员容器移除，再析构回调持有资源；析构重入不能打断容器修改。
	FWindowHitSubscription RetiredSubscription = MoveTemp(WindowHitSubscriptions[Index]);
	WindowHitSubscriptions.RemoveAtSwap(Index, 1, EAllowShrinking::No);
	return true;
}

void UGGYGOMeleeTraceComponent::CloseCurrentTraceWindow()
{
	bIsTracing = false;
	WindowSource = EWindowSource::None;
	// 先脱离原窗口订阅，再完成清理，最后释放回调；不擦除重入新建的资源。
	TArray<FWindowHitSubscription> RetiredSubscriptions = MoveTemp(WindowHitSubscriptions);
	PreviousStart = FVector::ZeroVector;
	PreviousEnd = FVector::ZeroVector;
	bHasPreviousTransform = false;
	HitActorsThisWindow.Reset();
	SetComponentTickEnabled(false);
}

bool UGGYGOMeleeTraceComponent::IsActiveWindow(uint64 Serial) const
{
	return bIsTracing && WindowSerial == Serial;
}

void UGGYGOMeleeTraceComponent::DispatchOwnedWindowHit(
	const FGGYGOMeleeTraceWindowHandle& Window, AActor* HitActor, const FHitResult& Hit)
{
	const FGGYGOMeleeTraceWindowHandle OriginalWindow = Window;
	if (QueryOwnedTraceWindow(OriginalWindow) != EGGYGOMeleeTraceWindowQueryResult::Active) { return; }
	const TArray<FWindowHitSubscription> Snapshot = WindowHitSubscriptions;
	for (const FWindowHitSubscription& Entry : Snapshot)
	{
		if (QueryOwnedTraceWindow(OriginalWindow) != EGGYGOMeleeTraceWindowQueryResult::Active) { return; }
		const bool bStillSubscribed = WindowHitSubscriptions.ContainsByPredicate(
			[&Entry, &OriginalWindow](const FWindowHitSubscription& Current)
			{
				return Current.Handle == Entry.Handle && Current.Window == OriginalWindow;
			});
		if (!bStillSubscribed) { continue; }
		Entry.Callback.ExecuteIfBound(OriginalWindow, HitActor, Hit);
		if (QueryOwnedTraceWindow(OriginalWindow) != EGGYGOMeleeTraceWindowQueryResult::Active) { return; }
	}
}

void UGGYGOMeleeTraceComponent::OnUnregister()
{
	TGuardValue<bool> LifecycleGuard(bClosingForLifecycle, true);
	CloseCurrentTraceWindow();
	Super::OnUnregister();
}

void UGGYGOMeleeTraceComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	TGuardValue<bool> LifecycleGuard(bClosingForLifecycle, true);
	CloseCurrentTraceWindow();
	Super::EndPlay(EndPlayReason);
}

void UGGYGOMeleeTraceComponent::Deactivate()
{
	TGuardValue<bool> LifecycleGuard(bClosingForLifecycle, true);
	CloseCurrentTraceWindow();
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
	if (!bIsTracing) { return; }
	const uint64 TraceSerial = WindowSerial;
	const bool bOwnedWindow = WindowSource == EWindowSource::Owned;
	FGGYGOMeleeTraceWindowHandle TraceWindow;
	if (bOwnedWindow)
	{
		TraceWindow.Issuer = this;
		TraceWindow.Serial = TraceSerial;
	}
	const AActor* Owner = GetOwner();
	const UWorld* World = GetWorld();
	if (!IsValid(Owner) || !World)
	{
		UE_LOG(LogGGYGOMeleeTrace, Warning,
			TEXT("MeleeTrace [%s/%s] Owner/World 不可用，关闭窗口 [%s -> %s, R=%.2f]。"),
			*GetNameSafe(Owner), *GetName(), *StartSocket.ToString(), *EndSocket.ToString(), TraceRadius);
		CloseCurrentTraceWindow();
		return;
	}

	const USkeletalMeshComponent* Mesh = GetTraceMesh();
	if (!Mesh || !Mesh->DoesSocketExist(StartSocket) || !Mesh->DoesSocketExist(EndSocket))
	{
		UE_LOG(LogGGYGOMeleeTrace, Warning,
			TEXT("MeleeTrace [%s/%s] 活动窗口 Mesh/Socket 缺失，关闭窗口 [%s -> %s, R=%.2f]。"),
			*GetNameSafe(Owner), *GetName(), *StartSocket.ToString(), *EndSocket.ToString(), TraceRadius);
		CloseCurrentTraceWindow();
		return;
	}

	const FVector CurrentStart = Mesh->GetSocketLocation(StartSocket);
	const FVector CurrentEnd = Mesh->GetSocketLocation(EndSocket);
	if (CurrentStart.ContainsNaN() || CurrentEnd.ContainsNaN())
	{
		UE_LOG(LogGGYGOMeleeTrace, Warning,
			TEXT("MeleeTrace [%s/%s] 活动窗口 Socket 坐标非有限值，关闭窗口 [%s -> %s, R=%.2f]。"),
			*GetNameSafe(Owner), *GetName(), *StartSocket.ToString(), *EndSocket.ToString(), TraceRadius);
		CloseCurrentTraceWindow();
		return;
	}
	const double MaxLength = FMath::Max(FVector::Distance(CurrentStart, CurrentEnd),
		bHasPreviousTransform ? FVector::Distance(PreviousStart, PreviousEnd) : 0.0);
	// 采样间距不超过半径，长武器不会因固定等分数在长度方向留下空洞。
	const double RequiredSegments = FMath::Max(1.0, FMath::CeilToDouble(MaxLength / TraceRadius));
	if (RequiredSegments > MaxTraceSegments)
	{
		UE_LOG(LogGGYGOMeleeTrace, Warning,
			TEXT("MeleeTrace [%s/%s] 所需分段 %.0f 超过上限 %d；长度 %.1f、半径 %.1f [%s -> %s]，关闭窗口。"),
			*GetNameSafe(Owner), *GetName(), RequiredSegments, MaxTraceSegments, MaxLength, TraceRadius,
			*StartSocket.ToString(), *EndSocket.ToString());
		CloseCurrentTraceWindow();
		return;
	}
	const int32 SegmentCount = static_cast<int32>(RequiredSegments);

	if (!bHasPreviousTransform)
	{
		// 第一帧只建立基线，不判定。此时没有路径可扫。
		if (!IsActiveWindow(TraceSerial)) { return; }
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

			if (bOwnedWindow)
			{
				DispatchOwnedWindowHit(TraceWindow, HitActor, Hit);
			}
			else
			{
				OnMeleeHit.Broadcast(HitActor, Hit);
			}
			if (!IsActiveWindow(TraceSerial)) { return; }
		}
	}

	if (!IsActiveWindow(TraceSerial)) { return; }
	PreviousStart = CurrentStart;
	PreviousEnd = CurrentEnd;
}
