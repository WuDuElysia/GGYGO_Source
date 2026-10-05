/**
 * @file GGYGOMeleeTraceComponent.cpp
 * @brief 近战判定实现
 */
#include "Combat/HitDetection/GGYGOMeleeTraceComponent.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/HitResult.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "Templates/UnrealTemplate.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOMeleeTraceComponent)

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOMeleeTrace, Log, All);

namespace
{
	FGGYGOMeleeTraceShape MakeRootTipShape(FName Start, FName End, float Radius)
	{
		FGGYGOMeleeTraceShape Shape;
		FGGYGOMeleeTraceChain& Chain = Shape.Chains.AddDefaulted_GetRef();
		Chain.Points = {Start, End};
		Chain.WorldRadiusCm = Radius;
		return Shape;
	}
}

UGGYGOMeleeTraceComponent::UGGYGOMeleeTraceComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = true;

	// 默认不 Tick，只在判定窗口内开启。近战判定是短暂的，
	// 常开 Tick 会让场上每个角色每帧都做一次无用的扫掠。
	PrimaryComponentTick.bStartWithTickEnabled = false;
}

bool UGGYGOMeleeTraceComponent::ValidateTraceShapeDefinition(const FGGYGOMeleeTraceShape& Shape, FString& OutError)
{
	OutError.Reset();
	if (Shape.Mode != EGGYGOMeleeTraceShapeMode::RootTip && Shape.Mode != EGGYGOMeleeTraceShapeMode::SocketChains)
	{
		OutError = FString::Printf(TEXT("Mode=%d 未定义。"), static_cast<int32>(Shape.Mode));
		return false;
	}
	if (Shape.Chains.IsEmpty() || (Shape.Mode == EGGYGOMeleeTraceShapeMode::RootTip
		&& (Shape.Chains.Num() != 1 || Shape.Chains[0].Points.Num() != 2)))
	{
		OutError = FString::Printf(TEXT("Mode=%d 配置为空或拓扑无效：RootTip 必须一链两点，SocketChains 至少一链。"),
			static_cast<int32>(Shape.Mode));
		return false;
	}
	for (int32 ChainIndex = 0; ChainIndex < Shape.Chains.Num(); ++ChainIndex)
	{
		const FGGYGOMeleeTraceChain& Chain = Shape.Chains[ChainIndex];
		if (Chain.Points.Num() < 2 || !FMath::IsFinite(Chain.WorldRadiusCm) || Chain.WorldRadiusCm < 1.0f)
		{
			OutError = FString::Printf(TEXT("Mode=%d Chain=%d 点数 %d/固定世界半径 %.9g 无效，至少两点且半径为有限值 >=1cm。"),
				static_cast<int32>(Shape.Mode), ChainIndex, Chain.Points.Num(), Chain.WorldRadiusCm);
			return false;
		}
		for (int32 PointIndex = 0; PointIndex < Chain.Points.Num(); ++PointIndex)
		{
			if (Chain.Points[PointIndex].IsNone())
			{
				OutError = FString::Printf(TEXT("Mode=%d Chain=%d Point=%d 名称为空。"),
					static_cast<int32>(Shape.Mode), ChainIndex, PointIndex);
				return false;
			}
		}
	}
	return true;
}

bool UGGYGOMeleeTraceComponent::ValidateTraceShapeOnCurrentMesh(const FGGYGOMeleeTraceShape& Shape,
	const USkeletalMeshComponent* ExpectedMainMesh, FString& OutError) const
{
	check(IsInGameThread());
	TArray<FVector> Points;
	TArray<FTraceSegment> Segments;
	return ReadTraceShapeFrame(Shape, ExpectedMainMesh, nullptr, Points, Segments, OutError);
}

bool UGGYGOMeleeTraceComponent::ReadTracePoint(const USkeletalMeshComponent* Mesh, FName Point,
	FVector& OutPosition, FString& OutError)
{
	OutError.Reset();
	if (!Mesh->DoesSocketExist(Point)) { OutError = TEXT("Socket/Bone 缺失。"); return false; }
	const USkeletalMeshSocket* Socket = Mesh->GetSocketByName(Point);
	if (Socket && (Socket->RelativeLocation.ContainsNaN() || Socket->RelativeRotation.ContainsNaN()
		|| Socket->RelativeScale.ContainsNaN()))
	{
		OutError = TEXT("Socket 局部位置/旋转/缩放非有限值。");
		return false;
	}
	// 与引擎公开 SocketOverride 解析一致；Bone名称点也通过同一接口取原父骨。
	const FName Bone = Mesh->GetSocketBoneName(Point);
	const int32 BoneIndex = Mesh->GetBoneIndex(Bone);
	// 无 Socket 时，引擎位置查询仍用原 Point 查骨；名称 Override 成功不能冒充位置解析成功。
	if (!Socket && (BoneIndex == INDEX_NONE || Mesh->GetBoneIndex(Point) != BoneIndex))
	{
		OutError = FString::Printf(TEXT("无 Socket 的名称 [%s] 与位置查询实际骨不一致 [ResolvedBone=%s]；禁止采用组件位置或另一骨的回落结果。"),
			*Point.ToString(), *Bone.ToString());
		return false;
	}
	const TArray<FTransform>& BoneTransforms = Mesh->GetComponentSpaceTransforms();
	if (!BoneTransforms.IsValidIndex(BoneIndex) || !BoneTransforms[BoneIndex].IsValid())
	{
		OutError = FString::Printf(TEXT("父骨 [%s] 不存在、动画变换缓存未就绪或变换无效 [BoneIndex=%d, Cached=%d]。"),
			*Bone.ToString(), BoneIndex, BoneTransforms.Num());
		return false;
	}
	OutPosition = Mesh->GetSocketLocation(Point);
	if (OutPosition.ContainsNaN()) { OutError = TEXT("世界坐标非有限值。"); return false; }
	return true;
}

bool UGGYGOMeleeTraceComponent::ReadTraceShapeFrame(const FGGYGOMeleeTraceShape& Shape,
	const USkeletalMeshComponent* ExpectedMainMesh, const TArray<FVector>* PriorPoints,
	TArray<FVector>& OutPoints, TArray<FTraceSegment>& OutSegments, FString& OutError) const
{
	OutPoints.Reset();
	OutSegments.Reset();
	if (!ValidateTraceShapeDefinition(Shape, OutError)) { return false; }
	if (!IsValid(ExpectedMainMesh) || ExpectedMainMesh != GetTraceMesh()
		|| !ExpectedMainMesh->IsRegistered() || ExpectedMainMesh->IsBeingDestroyed()
		|| !ExpectedMainMesh->GetComponentTransform().IsValid()
		|| !IsValid(ExpectedMainMesh->GetSkeletalMeshAsset()))
	{
		OutError = FString::Printf(TEXT("Mode=%d 原主 Mesh 不一致、未注册、正在销毁、世界变换或资产无效 [Expected=%s, Current=%s]。"),
			static_cast<int32>(Shape.Mode), *GetPathNameSafe(ExpectedMainMesh), *GetPathNameSafe(GetTraceMesh()));
		return false;
	}
	if (MaxTraceSegments < 1 || MaxTraceSegments > 512)
	{
		OutError = FString::Printf(TEXT("Mode=%d MaxTraceSegments=%d 无效，范围为1..512。"),
			static_cast<int32>(Shape.Mode), MaxTraceSegments);
		return false;
	}
	const int32 SweepBudget = MaxTraceSegments + 1;
	int64 MinimumSweeps = 0;
	for (const FGGYGOMeleeTraceChain& Chain : Shape.Chains)
	{
		MinimumSweeps += 2LL * (Chain.Points.Num() - 1);
		if (MinimumSweeps > SweepBudget)
		{
			OutError = FString::Printf(TEXT("Mode=%d 最少 Sweep %lld 超过上限 %d（整窗含端点）。"),
				static_cast<int32>(Shape.Mode), MinimumSweeps, SweepBudget);
			return false;
		}
	}
	// 在任何 Sweep 前读完所有点；后一链无效不能使前一链先产生业务命中。
	for (int32 ChainIndex = 0; ChainIndex < Shape.Chains.Num(); ++ChainIndex)
	{
		const FGGYGOMeleeTraceChain& Chain = Shape.Chains[ChainIndex];
		for (int32 PointIndex = 0; PointIndex < Chain.Points.Num(); ++PointIndex)
		{
			const FName Point = Chain.Points[PointIndex];
			FVector Position;
			FString PointError;
			if (!ReadTracePoint(ExpectedMainMesh, Point, Position, PointError))
			{
				OutError = FString::Printf(TEXT("Mode=%d Chain=%d Point=%d [%s, R=%.9g]：%s"),
					static_cast<int32>(Shape.Mode), ChainIndex, PointIndex, *Point.ToString(), Chain.WorldRadiusCm, *PointError);
				return false;
			}
			OutPoints.Add(Position);
		}
	}
	if (PriorPoints)
	{
		if (PriorPoints->Num() != OutPoints.Num())
		{
			OutError = TEXT("原窗口控制点基线拓扑不一致。");
			return false;
		}
		for (const FVector& Point : *PriorPoints)
		{
			if (Point.ContainsNaN()) { OutError = TEXT("原窗口控制点基线非有限值。"); return false; }
		}
		for (int32 Index = 0; Index < OutPoints.Num(); ++Index)
		{
			if ((OutPoints[Index] - (*PriorPoints)[Index]).ContainsNaN())
			{
				OutError = FString::Printf(TEXT("原窗口 Point=%d 帧间位移非有限值。"), Index);
				return false;
			}
		}
	}
	int32 Offset = 0;
	int32 TotalSweeps = 0;
	for (int32 ChainIndex = 0; ChainIndex < Shape.Chains.Num(); ++ChainIndex)
	{
		const FGGYGOMeleeTraceChain& Chain = Shape.Chains[ChainIndex];
		for (int32 PointIndex = 0; PointIndex + 1 < Chain.Points.Num(); ++PointIndex)
		{
			const int32 Start = Offset + PointIndex;
			const int32 End = Start + 1;
			const double Length = FMath::Max(FVector::Distance(OutPoints[Start], OutPoints[End]),
				PriorPoints ? FVector::Distance((*PriorPoints)[Start], (*PriorPoints)[End]) : 0.0);
			const double Required = FMath::Max(1.0, FMath::CeilToDouble(Length / Chain.WorldRadiusCm));
			// 先比较有限性及剩余预算，禁止把无穷或超大分段数转为整数。
			if (!FMath::IsFinite(Required) || Required + 1.0 > SweepBudget - TotalSweeps)
			{
				OutError = FString::Printf(TEXT("Mode=%d Chain=%d Segment=%d [%s -> %s, Length=%.9g, R=%.9g] Sweep %.0f + %d 超过上限 %d或非有限。"),
					static_cast<int32>(Shape.Mode), ChainIndex, PointIndex,
					*Chain.Points[PointIndex].ToString(), *Chain.Points[PointIndex + 1].ToString(),
					Length, Chain.WorldRadiusCm, Required + 1.0, TotalSweeps, SweepBudget);
				return false;
			}
			FTraceSegment& Segment = OutSegments.AddDefaulted_GetRef();
			Segment.StartPoint = Start;
			Segment.EndPoint = End;
			Segment.Subdivisions = static_cast<int32>(Required);
			Segment.Radius = Chain.WorldRadiusCm;
			TotalSweeps += Segment.Subdivisions + 1;
		}
		Offset += Chain.Points.Num();
	}
	return true;
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
	const FGGYGOMeleeTraceShape Shape = MakeRootTipShape(InStartSocket, InEndSocket, InTraceRadius);
	const TWeakObjectPtr<USkeletalMeshComponent> OriginalMesh(GetTraceMesh());
	const TWeakObjectPtr<USkeletalMesh> OriginalAsset(
		OriginalMesh.IsValid() ? OriginalMesh->GetSkeletalMeshAsset() : nullptr);
	CloseCurrentTraceWindow();
	StartTraceWindow(Shape, OriginalMesh.Get(), OriginalAsset.Get(), EWindowSource::Legacy);
}

EGGYGOMeleeTraceWindowOpenResult UGGYGOMeleeTraceComponent::TryOpenOwnedTraceWindow(
	FName InStartSocket, FName InEndSocket, float InTraceRadius,
	const FGGYGOMeleeTraceWindowHandle& ExpectedWindow, FGGYGOMeleeTraceWindowHandle& OutWindow)
{
	check(IsInGameThread());
	return TryOpenOwnedTraceWindow(MakeRootTipShape(InStartSocket, InEndSocket, InTraceRadius),
		GetTraceMesh(), ExpectedWindow, OutWindow);
}

EGGYGOMeleeTraceWindowOpenResult UGGYGOMeleeTraceComponent::TryOpenOwnedTraceWindow(
	const FGGYGOMeleeTraceShape& Shape, const USkeletalMeshComponent* ExpectedMainMesh,
	const FGGYGOMeleeTraceWindowHandle& ExpectedWindow, FGGYGOMeleeTraceWindowHandle& OutWindow)
{
	check(IsInGameThread());
	// 在释放原订阅前快照输入；回调资源析构重入不能改变本请求或借用资源。
	const FGGYGOMeleeTraceShape ShapeSnapshot = Shape;
	const TWeakObjectPtr<USkeletalMeshComponent> OriginalMesh(const_cast<USkeletalMeshComponent*>(ExpectedMainMesh));
	const TWeakObjectPtr<USkeletalMesh> OriginalAsset(
		OriginalMesh.IsValid() ? OriginalMesh->GetSkeletalMeshAsset() : nullptr);
	const FGGYGOMeleeTraceWindowHandle ExpectedSnapshot = ExpectedWindow;
	OutWindow = FGGYGOMeleeTraceWindowHandle();
	if (ExpectedSnapshot.HasWindow())
	{
		const EGGYGOMeleeTraceWindowQueryResult ExpectedState = QueryOwnedTraceWindow(ExpectedSnapshot);
		if (ExpectedState != EGGYGOMeleeTraceWindowQueryResult::Active)
		{
			UE_LOG(LogGGYGOMeleeTrace, Warning,
				TEXT("MeleeTrace [%s/%s] 拒绝 Owned Begin：预期原窗口不是本组件活动 Owned 窗口 [Mode=%d, Query=%d]。"),
				*GetNameSafe(GetOwner()), *GetName(), static_cast<int32>(ShapeSnapshot.Mode),
				static_cast<int32>(ExpectedState));
			return ExpectedState == EGGYGOMeleeTraceWindowQueryResult::Inactive
				? EGGYGOMeleeTraceWindowOpenResult::WindowConflict
				: EGGYGOMeleeTraceWindowOpenResult::InvalidExpectedHandle;
		}
		CloseCurrentTraceWindow();
	}

	const EGGYGOMeleeTraceWindowOpenResult Result = StartTraceWindow(
		ShapeSnapshot, OriginalMesh.Get(), OriginalAsset.Get(), EWindowSource::Owned);
	if (Result == EGGYGOMeleeTraceWindowOpenResult::Opened)
	{
		OutWindow.Issuer = this;
		OutWindow.Serial = WindowSerial;
	}
	return Result;
}

EGGYGOMeleeTraceWindowOpenResult UGGYGOMeleeTraceComponent::StartTraceWindow(
	const FGGYGOMeleeTraceShape& Shape, const USkeletalMeshComponent* ExpectedMainMesh,
	const USkeletalMesh* ExpectedMeshAsset, EWindowSource Source)
{
	// 包括清理旧订阅期间发生重入的情况；不能覆盖刚取得窗口的新请求。
	if (bIsTracing)
	{
		UE_LOG(LogGGYGOMeleeTrace, Warning,
			TEXT("MeleeTrace [%s/%s] 拒绝开窗：已有活动窗口，必须提供匹配 Owned 身份或先结束 Legacy [Mode=%d]。"),
			*GetNameSafe(GetOwner()), *GetName(), static_cast<int32>(Shape.Mode));
		return EGGYGOMeleeTraceWindowOpenResult::WindowConflict;
	}
	const AActor* Owner = GetOwner();
	if (bClosingForLifecycle || !IsRegistered() || IsBeingDestroyed()
		|| !IsValid(Owner) || Owner->IsActorBeingDestroyed() || !GetWorld())
	{
		UE_LOG(LogGGYGOMeleeTrace, Warning,
			TEXT("MeleeTrace [%s/%s] 拒绝开窗：生命周期清理中、未注册、正在销毁或 Owner/World 不可用 [Mode=%d, Mesh=%s, Asset=%s]。"),
			*GetNameSafe(Owner), *GetName(), static_cast<int32>(Shape.Mode),
			*GetPathNameSafe(ExpectedMainMesh), *GetPathNameSafe(ExpectedMeshAsset));
		return EGGYGOMeleeTraceWindowOpenResult::Unavailable;
	}
	FString Error;
	bool bConfigurationValid = false;
	if (!IsValid(ExpectedMainMesh) || !IsValid(ExpectedMeshAsset)
		|| ExpectedMainMesh->GetSkeletalMeshAsset() != ExpectedMeshAsset)
	{
		Error = TEXT("原 Mesh/Asset 不可用或已在开窗清理期间替换。");
	}
	else
	{
		bConfigurationValid = ValidateTraceShapeOnCurrentMesh(Shape, ExpectedMainMesh, Error);
	}
	if (!bConfigurationValid)
	{
		UE_LOG(LogGGYGOMeleeTrace, Warning, TEXT("MeleeTrace [%s/%s] 拒绝无效 Mesh/Socket/半径/采样上限 [Mode=%d, Mesh=%s, Asset=%s]：%s"),
			*GetNameSafe(Owner), *GetName(), static_cast<int32>(Shape.Mode),
			*GetPathNameSafe(ExpectedMainMesh), *GetPathNameSafe(ExpectedMeshAsset), *Error);
		return EGGYGOMeleeTraceWindowOpenResult::InvalidConfiguration;
	}
	if (WindowSerial == MAX_uint64)
	{
		UE_LOG(LogGGYGOMeleeTrace, Warning,
			TEXT("MeleeTrace [%s/%s] 拒绝开窗：WindowSerial 已耗尽，禁止回绕复用 [Mode=%d]。"),
			*GetNameSafe(Owner), *GetName(), static_cast<int32>(Shape.Mode));
		return EGGYGOMeleeTraceWindowOpenResult::SerialExhausted;
	}
	ActiveShape = Shape;
	WindowMesh = const_cast<USkeletalMeshComponent*>(ExpectedMainMesh);
	WindowMeshAsset = const_cast<USkeletalMesh*>(ExpectedMeshAsset);
	WindowWorld = GetWorld();
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
			TEXT("MeleeTrace [%s/%s] 拒绝 Legacy End 关闭活动 Owned 窗口 [Mode=%d]。"),
			*GetNameSafe(GetOwner()), *GetName(), static_cast<int32>(ActiveShape.Mode));
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
	ActiveShape = FGGYGOMeleeTraceShape();
	WindowMesh.Reset();
	WindowMeshAsset.Reset();
	WindowWorld.Reset();
	PreviousPoints.Reset();
	bHasPreviousTransform = false;
	HitActorsThisWindow.Reset();
	SetComponentTickEnabled(false);
}

bool UGGYGOMeleeTraceComponent::IsActiveWindow(uint64 Serial) const
{
	return bIsTracing && WindowSerial == Serial;
}

bool UGGYGOMeleeTraceComponent::EnsureActiveTraceResources(uint64 Serial)
{
	if (!IsActiveWindow(Serial)) { return false; }
	const USkeletalMeshComponent* Mesh = WindowMesh.Get();
	FString Error;
	if (!IsRegistered() || IsBeingDestroyed() || !IsValid(GetOwner()) || GetOwner()->IsActorBeingDestroyed()
		|| !WindowWorld.IsValid() || WindowWorld.Get() != GetWorld())
	{
		Error = TEXT("原 Owner/World/组件生命周期不可用。");
	}
	else if (!IsValid(Mesh) || Mesh != GetTraceMesh() || !Mesh->IsRegistered() || Mesh->IsBeingDestroyed()
		|| !Mesh->GetComponentTransform().IsValid()
		|| !WindowMeshAsset.IsValid() || Mesh->GetSkeletalMeshAsset() != WindowMeshAsset.Get())
	{
		Error = TEXT("活动窗口原主 Mesh/Asset 失效或替换。");
	}
	else
	{
		// 回调可使原资产的点失效；下一订阅/扫掠前仍须拒绝同原窗。
		for (int32 ChainIndex = 0; ChainIndex < ActiveShape.Chains.Num() && Error.IsEmpty(); ++ChainIndex)
		{
			const FGGYGOMeleeTraceChain& Chain = ActiveShape.Chains[ChainIndex];
			for (int32 PointIndex = 0; PointIndex < Chain.Points.Num(); ++PointIndex)
			{
				const FName Point = Chain.Points[PointIndex];
				FVector Position;
				FString PointError;
				if (!ReadTracePoint(Mesh, Point, Position, PointError))
				{
					Error = FString::Printf(TEXT("Chain=%d Point=%d [%s, R=%.9g]：%s"),
						ChainIndex, PointIndex, *Point.ToString(), Chain.WorldRadiusCm, *PointError);
					break;
				}
			}
		}
	}
	if (!Error.IsEmpty())
	{
		UE_LOG(LogGGYGOMeleeTrace, Warning, TEXT("MeleeTrace [%s/%s] 关闭原窗口 Serial=%llu [Mode=%d, Mesh=%s, Asset=%s]：%s"),
			*GetNameSafe(GetOwner()), *GetName(), Serial, static_cast<int32>(ActiveShape.Mode),
			*GetPathNameSafe(Mesh), *GetPathNameSafe(WindowMeshAsset.Get()), *Error);
		CloseCurrentTraceWindow();
		return false;
	}
	return true;
}

void UGGYGOMeleeTraceComponent::DispatchOwnedWindowHit(
	const FGGYGOMeleeTraceWindowHandle& Window, AActor* HitActor, const FHitResult& Hit)
{
	const FGGYGOMeleeTraceWindowHandle OriginalWindow = Window;
	if (QueryOwnedTraceWindow(OriginalWindow) != EGGYGOMeleeTraceWindowQueryResult::Active) { return; }
	const TArray<FWindowHitSubscription> Snapshot = WindowHitSubscriptions;
	for (const FWindowHitSubscription& Entry : Snapshot)
	{
		if (!EnsureActiveTraceResources(OriginalWindow.Serial)) { return; }
		const bool bStillSubscribed = WindowHitSubscriptions.ContainsByPredicate(
			[&Entry, &OriginalWindow](const FWindowHitSubscription& Current)
			{
				return Current.Handle == Entry.Handle && Current.Window == OriginalWindow;
			});
		if (!bStillSubscribed) { continue; }
		Entry.Callback.ExecuteIfBound(OriginalWindow, HitActor, Hit);
		if (!EnsureActiveTraceResources(OriginalWindow.Serial)) { return; }
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
	if (!EnsureActiveTraceResources(TraceSerial)) { return; }
	const bool bOwnedWindow = WindowSource == EWindowSource::Owned;
	FGGYGOMeleeTraceWindowHandle TraceWindow;
	if (bOwnedWindow)
	{
		TraceWindow.Issuer = this;
		TraceWindow.Serial = TraceSerial;
	}
	const FGGYGOMeleeTraceShape Shape = ActiveShape;
	const TArray<FVector> PriorPoints = PreviousPoints;
	TArray<FVector> CurrentPoints;
	TArray<FTraceSegment> Segments;
	FString Error;
	if (!ReadTraceShapeFrame(Shape, WindowMesh.Get(), bHasPreviousTransform ? &PriorPoints : nullptr,
		CurrentPoints, Segments, Error))
	{
		if (!IsActiveWindow(TraceSerial)) { return; }
		UE_LOG(LogGGYGOMeleeTrace, Warning,
			TEXT("MeleeTrace [%s/%s] 全链预检失败，关闭原窗口 Serial=%llu [Mode=%d, Mesh=%s, Asset=%s]：%s"),
			*GetNameSafe(GetOwner()), *GetName(), TraceSerial, static_cast<int32>(Shape.Mode),
			*GetPathNameSafe(WindowMesh.Get()), *GetPathNameSafe(WindowMeshAsset.Get()), *Error);
		CloseCurrentTraceWindow();
		return;
	}
	if (!EnsureActiveTraceResources(TraceSerial)) { return; }

	if (!bHasPreviousTransform)
	{
		// 全链共同建立首次基线，不判定；不能让后一链借用前一链的首帧状态。
		PreviousPoints = MoveTemp(CurrentPoints);
		bHasPreviousTransform = true;
		return;
	}

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(GGYGOMeleeTrace), /*bTraceComplex=*/false);
	QueryParams.AddIgnoredActor(GetOwner());
	QueryParams.bReturnPhysicalMaterial = true;

	// 沿武器长度分段扫掠。
	//
	// 每个采样点从上帧位置扫到本帧位置；离散帧之间按直线近似，
	// 大角度旋转时仍不能声称还原了武器的真实弧形运动。
	TArray<FHitResult> Hits;

	for (const FTraceSegment& Segment : Segments)
	{
		for (int32 SampleIndex = 0; SampleIndex <= Segment.Subdivisions; ++SampleIndex)
		{
			const double Ratio = static_cast<double>(SampleIndex) / Segment.Subdivisions;
			const FVector From = FMath::Lerp(PriorPoints[Segment.StartPoint], PriorPoints[Segment.EndPoint], Ratio);
			const FVector To = FMath::Lerp(CurrentPoints[Segment.StartPoint], CurrentPoints[Segment.EndPoint], Ratio);
			Hits.Reset();
			// 唯一查询入口。所有链共用预算、材质回填、Actor去重和原窗口订阅。
			WindowWorld->SweepMultiByChannel(Hits, From, To, FQuat::Identity, TraceChannel,
				FCollisionShape::MakeSphere(Segment.Radius), QueryParams);
			for (const FHitResult& Hit : Hits)
			{
				AActor* HitActor = Hit.GetActor();
				if (!IsValid(HitActor)) { continue; }
				const bool bAlreadyHit = HitActorsThisWindow.ContainsByPredicate(
					[HitActor](const TWeakObjectPtr<AActor>& Weak) { return Weak.Get() == HitActor; });
				if (bAlreadyHit) { continue; }
				HitActorsThisWindow.Add(HitActor);
				if (bOwnedWindow) { DispatchOwnedWindowHit(TraceWindow, HitActor, Hit); }
				else { OnMeleeHit.Broadcast(HitActor, Hit); }
				if (!EnsureActiveTraceResources(TraceSerial)) { return; }
			}
		}
	}

	if (!EnsureActiveTraceResources(TraceSerial)) { return; }
	PreviousPoints = MoveTemp(CurrentPoints);
}
