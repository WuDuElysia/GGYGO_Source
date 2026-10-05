/** @file GGYGOMeleeTraceSafetyTest.cpp @brief 真实碰撞查询下的去重、采样和回调取消边界 */
#include "Combat/Tests/GGYGOMeleeTraceTestReceiver.h"
#include "Combat/HitDetection/GGYGOMeleeTraceComponent.h"
#include "Engine/HitResult.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Character.h"
#include "PhysicalMaterials/PhysicalMaterial.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOMeleeTraceTestReceiver)

void UGGYGOMeleeTraceTestReceiver::HandleHit(AActor* Actor, const FHitResult& Hit)
{
	++HitCount;
	LastPhysicalMaterial = Hit.PhysMaterial.Get();
	if (bCloseOnHit && Trace) { Trace->EndTraceWindow(); }
	if (bReopenOnHit && Trace) { Trace->BeginTraceWindow(TEXT("TraceBase"), TEXT("TraceTip"), 20.0f); }
}

#if WITH_DEV_AUTOMATION_TESTS
#include "ReferenceSkeleton.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOMeleeTraceSafetyTest, "GGYGO.Combat.MeleeTrace.SafetyAndCoverage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOMeleeTraceSafetyTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UPhysicalMaterial> PhysicalMaterial(NewObject<UPhysicalMaterial>());
	// 必须在 Solver 创建前建立 handle，确保 Solver 初始化时复制到 QueryMaterials_External。
	PhysicalMaterial->GetPhysicsMaterial();

	UWorld::InitializationValues Init;
	Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(true)
		.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	if (!TestNotNull(TEXT("创建独立测试世界"), World)) { return false; }
	struct FWorldCleanup
	{
		UWorld* World;
		~FWorldCleanup() { World->DestroyWorld(false); }
	} Cleanup{World};

	ACharacter* Owner = World->SpawnActor<ACharacter>();
	if (!TestNotNull(TEXT("测试角色"), Owner)) { return false; }
	// 合成单骨骼与 500cm 武器端点，不依赖 Kevin/Pyrios 或其他磁盘资产。
	USkeletalMesh* MeshAsset = NewObject<USkeletalMesh>(Owner);
	USkeleton* Skeleton = NewObject<USkeleton>(Owner);
	FReferenceSkeleton RefSkeleton;
	{
		// 析构时才从 Raw 重建 Final 数据；完成后才能复制给 Mesh。
		FReferenceSkeletonModifier Modifier(RefSkeleton, Skeleton);
		Modifier.Add(FMeshBoneInfo(TEXT("Root"), TEXT("Root"), INDEX_NONE), FTransform::Identity);
	}
	if (!TestEqual(TEXT("Mesh Raw 单骨骼"), RefSkeleton.GetRawBoneNum(), 1)
		|| !TestEqual(TEXT("Mesh Final 单骨骼"), RefSkeleton.GetNum(), 1)) { return false; }
	MeshAsset->SetRefSkeleton(RefSkeleton);
	MeshAsset->SetSkeleton(Skeleton);
	if (!TestTrue(TEXT("初始化 Skeleton 参考骨架及 BoneTree"), Skeleton->MergeAllBonesToBoneTree(MeshAsset, false))
		|| !TestEqual(TEXT("Skeleton Raw 单骨骼"), Skeleton->GetReferenceSkeleton().GetRawBoneNum(), 1)
		|| !TestEqual(TEXT("Skeleton Final 单骨骼"), Skeleton->GetReferenceSkeleton().GetNum(), 1)
		|| !TestEqual(TEXT("Skeleton Root 索引"), Skeleton->GetReferenceSkeleton().FindBoneIndex(TEXT("Root")), 0)
		|| !TestTrue(TEXT("Skeleton 与 Mesh 兼容"), Skeleton->IsCompatibleMesh(MeshAsset))) { return false; }
	for (int32 Index = 0; Index < 2; ++Index)
	{
		USkeletalMeshSocket* Socket = NewObject<USkeletalMeshSocket>(MeshAsset);
		Socket->SocketName = Index == 0 ? TEXT("TraceBase") : TEXT("TraceTip");
		Socket->BoneName = TEXT("Root");
		Socket->RelativeLocation = FVector(Index * 500.0, 0.0, 0.0);
		MeshAsset->GetMeshOnlySocketList().Add(Socket);
	}
	USkeletalMeshComponent* Mesh = Owner->GetMesh();
	if (!TestNotNull(TEXT("主 Mesh"), Mesh)
		|| !TestTrue(TEXT("查询夹具主 Mesh 保持注册"), Mesh->IsRegistered())) { return false; }
	// 本夹具验证 Socket 与真实物理查询，不运行动画求值或创建渲染 LOD。
	// 限定基类入口保留资产装配/变换分配，避免 SetSkeletalMesh 的 InitAnim。
	Mesh->SetComponentTickEnabled(false);
	Mesh->SetVisibility(false);
	Mesh->SetCastShadow(false);
	Mesh->USkinnedMeshComponent::SetSkinnedAssetAndUpdate(MeshAsset);
	Mesh->SetComponentSpaceTransformsDoubleBuffering(false);
	Mesh->GetEditableComponentSpaceTransforms() = MeshAsset->GetRefSkeleton().GetRefBonePose();
	const FTransform FixtureTransform(FRotator(0.0, 35.0, 0.0), FVector(1000.0, 2000.0, 300.0));
	Mesh->SetWorldTransform(FixtureTransform);
	if (!TestEqual(TEXT("单 Root 组件空间缓存"), Mesh->GetNumComponentSpaceTransforms(), 1)
		|| !TestTrue(TEXT("起点 Socket 使用世界变换"), Mesh->GetSocketLocation(TEXT("TraceBase")).Equals(
			FixtureTransform.TransformPosition(FVector::ZeroVector), 0.01))
		|| !TestTrue(TEXT("终点 Socket 使用骨骼缓存及世界变换"), Mesh->GetSocketLocation(TEXT("TraceTip")).Equals(
			FixtureTransform.TransformPosition(FVector(500.0, 0.0, 0.0)), 0.01))) { return false; }
	UGGYGOMeleeTraceComponent* Trace = NewObject<UGGYGOMeleeTraceComponent>(Owner);
	Trace->RegisterComponent();
	TStrongObjectPtr<UGGYGOMeleeTraceTestReceiver> Receiver(NewObject<UGGYGOMeleeTraceTestReceiver>());
	Receiver->Trace = Trace;
	Trace->OnMeleeHit.AddDynamic(Receiver.Get(), &UGGYGOMeleeTraceTestReceiver::HandleHit);
	auto TickTrace = [Trace]() { Trace->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr); };

	AddExpectedError(TEXT("Mode=0 Chain=0 Point=0 [Missing, R=20]：Socket/Bone 缺失。"), EAutomationExpectedErrorFlags::Contains, 1, false);
	Trace->BeginTraceWindow(TEXT("Missing"), TEXT("TraceTip"), 20.0f);
	TestFalse(TEXT("缺失骨骼拒绝开窗而非回退原点"), Trace->IsTracing());
	TestFalse(TEXT("无效窗口不启动 Tick"), Trace->IsComponentTickEnabled());

	const FVector Base = Mesh->GetSocketLocation(TEXT("TraceBase"));
	const FVector Tip = Mesh->GetSocketLocation(TEXT("TraceTip"));
	if (!TestTrue(TEXT("合成武器长度有效"), FVector::Distance(Base, Tip) > 490.0)) { return false; }

	// 本段使用非零 Block Sweep，隔离验证查询材质回填。
	// 自定义材质句柄须在 Solver 创建前注册，这是合成夹具的时序要求。
	AActor* MaterialTarget = World->SpawnActor<AActor>();
	USphereComponent* MaterialSphere = NewObject<USphereComponent>(MaterialTarget);
	MaterialTarget->SetRootComponent(MaterialSphere);
	MaterialSphere->SetSphereRadius(8.0f);
	MaterialSphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	MaterialSphere->SetCollisionResponseToAllChannels(ECR_Ignore);
	MaterialSphere->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	MaterialSphere->RegisterComponent();
	MaterialSphere->SetPhysMaterialOverride(PhysicalMaterial.Get());
	if (!TestTrue(TEXT("材质目标碰撞体解析到目标物理材质"),
		MaterialSphere->GetBodyInstance()->GetSimplePhysicalMaterial() == PhysicalMaterial.Get()))
	{
		return false;
	}
	MaterialTarget->SetActorLocation(FMath::Lerp(Base, Tip, 0.25) + FVector(0.0, 0.0, 40.0));

	Trace->BeginTraceWindow(TEXT("TraceBase"), TEXT("TraceTip"), 20.0f);
	TickTrace();
	Mesh->SetWorldTransform(FTransform(FixtureTransform.GetRotation(),
		FixtureTransform.GetLocation() + FVector(0.0, 0.0, 80.0)));
	TickTrace();
	TestEqual(TEXT("非初始阻挡 Sweep 命中材质目标一次"), Receiver->HitCount, 1);
	TestTrue(TEXT("Trace 查询返回目标物理材质"), Receiver->LastPhysicalMaterial == PhysicalMaterial.Get());
	Trace->EndTraceWindow();
	MaterialSphere->UnregisterComponent();
	Mesh->SetWorldTransform(FixtureTransform);
	Receiver->HitCount = 0;
	Receiver->LastPhysicalMaterial = nullptr;

	// 两个目标都位于旧固定四等分之间，重叠响应使一次查询可发现多个 Actor。
	for (double Ratio : {0.125, 0.375})
	{
		AActor* Target = World->SpawnActor<AActor>();
		USphereComponent* Sphere = NewObject<USphereComponent>(Target);
		Target->SetRootComponent(Sphere);
		Sphere->SetSphereRadius(8.0f);
		Sphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Sphere->SetCollisionResponseToAllChannels(ECR_Ignore);
		Sphere->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
		Sphere->RegisterComponent();
		// 注册后再覆盖材质，使 SetPhysMaterialOverride 更新已经创建的 Chaos shape。
		// 注册前只写 BodyInstance 在本夹具的初始重叠 Sweep 中没有稳定返回材质。
		Sphere->SetPhysMaterialOverride(PhysicalMaterial.Get());
		if (!TestTrue(TEXT("测试碰撞体解析到目标物理材质"),
			Sphere->GetBodyInstance()->GetSimplePhysicalMaterial() == PhysicalMaterial.Get()))
		{
			return false;
		}
		Target->SetActorLocation(FMath::Lerp(Base, Tip, Ratio));
	}
	Trace->BeginTraceWindow(TEXT("TraceBase"), TEXT("TraceTip"), 20.0f);
	TickTrace();
	TestEqual(TEXT("首帧只建基线"), Receiver->HitCount, 0);
	TickTrace();
	TestEqual(TEXT("半径推导密度覆盖长武器采样间隙"), Receiver->HitCount, 2);
	TickTrace();
	TestEqual(TEXT("同窗口每个目标只命中一次"), Receiver->HitCount, 2);
	Trace->EndTraceWindow();
	TickTrace();
	TestEqual(TEXT("结束后不再产生伤害事件"), Receiver->HitCount, 2);

	Trace->BeginTraceWindow(TEXT("TraceBase"), TEXT("TraceTip"), 20.0f);
	TickTrace(); TickTrace();
	TestEqual(TEXT("新窗口允许每个目标再次命中"), Receiver->HitCount, 4);
	Trace->EndTraceWindow();
	Receiver->HitCount = 0;
	Receiver->bCloseOnHit = true;
	Trace->BeginTraceWindow(TEXT("TraceBase"), TEXT("TraceTip"), 20.0f);
	TickTrace(); TickTrace();
	TestEqual(TEXT("首个回调取消后旧扫掠不得继续命中第二个目标"), Receiver->HitCount, 1);
	TestFalse(TEXT("回调取消立即关闭窗口"), Trace->IsTracing());

	Receiver->HitCount = 0;
	Receiver->bReopenOnHit = true;
	Trace->BeginTraceWindow(TEXT("TraceBase"), TEXT("TraceTip"), 20.0f);
	TickTrace(); TickTrace();
	TestEqual(TEXT("回调重开窗口也必须中止旧扫掠"), Receiver->HitCount, 1);
	TickTrace();
	TestEqual(TEXT("回调重开的窗口重新建立基线"), Receiver->HitCount, 1);
	Trace->EndTraceWindow();
	Trace->MaxTraceSegments = 4;
	AddExpectedError(TEXT("超过上限 5或非有限。"), EAutomationExpectedErrorFlags::Contains, 1, false);
	Trace->BeginTraceWindow(TEXT("TraceBase"), TEXT("TraceTip"), 20.0f);
	TickTrace();
	TestFalse(TEXT("超出采样预算时关闭并诊断，不静默制造空洞"), Trace->IsTracing());

	Receiver->bCloseOnHit = false;
	Receiver->bReopenOnHit = false;
	Trace->MaxTraceSegments = 64;
	Trace->BeginTraceWindow(TEXT("TraceBase"), TEXT("TraceTip"), 20.0f);
	TickTrace();
	TickTrace();
	TestTrue(TEXT("停用前已建立采样基线"), Trace->bHasPreviousTransform);
	TestTrue(TEXT("停用前已有窗口去重记录"), Trace->HitActorsThisWindow.Num() > 0);
	Trace->Deactivate();
	TestFalse(TEXT("Deactivate 关闭 Trace 窗口"), Trace->IsTracing());
	TestFalse(TEXT("Deactivate 禁用组件 Tick"), Trace->IsComponentTickEnabled());
	TestFalse(TEXT("Deactivate 清除采样基线"), Trace->bHasPreviousTransform);
	TestTrue(TEXT("Deactivate 清除本窗口去重记录"), Trace->HitActorsThisWindow.IsEmpty());
	TestTrue(TEXT("Deactivate 清除全部采样端点"), Trace->PreviousPoints.IsEmpty());
	Trace->Deactivate();
	TestFalse(TEXT("重复 Deactivate 保持关闭"), Trace->IsTracing());
	TestTrue(TEXT("重复 Deactivate 保持状态已清理"), Trace->HitActorsThisWindow.IsEmpty());

	Trace->Activate();
	Trace->BeginTraceWindow(TEXT("TraceBase"), TEXT("TraceTip"), 20.0f);
	TickTrace();
	TickTrace();
	TestTrue(TEXT("注销前已重新建立采样基线"), Trace->bHasPreviousTransform);
	TestTrue(TEXT("注销前已有窗口去重记录"), Trace->HitActorsThisWindow.Num() > 0);
	Trace->UnregisterComponent();
	TestFalse(TEXT("OnUnregister 关闭 Trace 窗口"), Trace->IsTracing());
	TestFalse(TEXT("OnUnregister 清除采样基线"), Trace->bHasPreviousTransform);
	TestTrue(TEXT("OnUnregister 清除本窗口去重记录"), Trace->HitActorsThisWindow.IsEmpty());
	TestTrue(TEXT("OnUnregister 清除全部采样端点"), Trace->PreviousPoints.IsEmpty());
	Trace->UnregisterComponent();
	TestFalse(TEXT("重复注销保持关闭"), Trace->IsTracing());
	Trace->RegisterComponent();
	TestTrue(TEXT("清理后组件可以重新注册"), Trace->IsRegistered());

	// Owned 冒烟复用同一真实查询夹具；旧动态接收器继续绑定以验证事件隔离。
	const int32 LegacyHitCountBeforeOwned = Receiver->HitCount;
	FGGYGOMeleeTraceWindowHandle OwnedWindow;
	if (!TestTrue(TEXT("正常取得 Owned 窗口 A"), Trace->TryOpenOwnedTraceWindow(
		TEXT("TraceBase"), TEXT("TraceTip"), 20.0f, FGGYGOMeleeTraceWindowHandle(), OwnedWindow)
		== EGGYGOMeleeTraceWindowOpenResult::Opened)) { return false; }
	const FGGYGOMeleeTraceWindowHandle OwnedWindowA = OwnedWindow;
	TestTrue(TEXT("A 已签发且活动"), OwnedWindowA.HasWindow()
		&& Trace->QueryOwnedTraceWindow(OwnedWindowA) == EGGYGOMeleeTraceWindowQueryResult::Active);
	if (!TestTrue(TEXT("Expected/Out 同变量允许 A 替换为 B"), Trace->TryOpenOwnedTraceWindow(
		TEXT("TraceBase"), TEXT("TraceTip"), 20.0f, OwnedWindow, OwnedWindow)
		== EGGYGOMeleeTraceWindowOpenResult::Opened)) { return false; }
	const FGGYGOMeleeTraceWindowHandle OwnedWindowB = OwnedWindow;
	TestTrue(TEXT("B 已签发且身份不同于 A"), OwnedWindowB.HasWindow() && OwnedWindowB != OwnedWindowA);
	TestTrue(TEXT("替换后 A 保留历史句柄但不再活动"), OwnedWindowA.HasWindow()
		&& Trace->QueryOwnedTraceWindow(OwnedWindowA) == EGGYGOMeleeTraceWindowQueryResult::Inactive);
	TestTrue(TEXT("晚到 A 关闭保持幂等失效"), Trace->CloseOwnedTraceWindow(OwnedWindowA)
		== EGGYGOMeleeTraceWindowCloseResult::AlreadyInactive);
	FGGYGOMeleeTraceWindowHandle RejectedWindow;
	AddExpectedError(TEXT("拒绝 Owned Begin：预期原窗口不是本组件活动 Owned 窗口"), EAutomationExpectedErrorFlags::Contains, 1, false);
	TestTrue(TEXT("晚到 A 开窗明确拒绝"), Trace->TryOpenOwnedTraceWindow(
		TEXT("TraceBase"), TEXT("TraceTip"), 20.0f, OwnedWindowA, RejectedWindow)
		== EGGYGOMeleeTraceWindowOpenResult::WindowConflict);
	TestFalse(TEXT("拒绝开窗不签发资源"), RejectedWindow.HasWindow());
	if (!TestTrue(TEXT("晚到 A 开关均未影响 B"), Trace->IsTracing()
		&& Trace->QueryOwnedTraceWindow(OwnedWindowB) == EGGYGOMeleeTraceWindowQueryResult::Active)) { return false; }

	int32 OwnedBHitCount = 0;
	int32 OwnedCHitCount = 0;
	FGGYGOMeleeTraceWindowHandle OwnedWindowC;
	FDelegateHandle OwnedBSubscription;
	FDelegateHandle OwnedCSubscription;
	const FGGYGOMeleeTraceWindowHitDelegate OwnedBCallback = FGGYGOMeleeTraceWindowHitDelegate::CreateLambda(
		[this, Trace, OwnedWindowB, &OwnedBHitCount, &OwnedCHitCount, &OwnedWindowC, &OwnedCSubscription]
		(const FGGYGOMeleeTraceWindowHandle& HitWindow, AActor*, const FHitResult&)
		{
			++OwnedBHitCount;
			TestTrue(TEXT("B 命中携带注册时的原窗口身份"), HitWindow == OwnedWindowB);
			if (!TestTrue(TEXT("原命中回调关闭 B"), Trace->CloseOwnedTraceWindow(OwnedWindowB)
				== EGGYGOMeleeTraceWindowCloseResult::Closed)) { return; }
			if (!TestTrue(TEXT("原命中回调取得新窗口 C"), Trace->TryOpenOwnedTraceWindow(
				TEXT("TraceBase"), TEXT("TraceTip"), 20.0f, FGGYGOMeleeTraceWindowHandle(), OwnedWindowC)
				== EGGYGOMeleeTraceWindowOpenResult::Opened)) { return; }
			TestTrue(TEXT("C 注册自己的原窗口订阅"), Trace->SubscribeWindowHit(OwnedWindowC,
				FGGYGOMeleeTraceWindowHitDelegate::CreateLambda(
					[this, OriginalC = OwnedWindowC, &OwnedCHitCount]
					(const FGGYGOMeleeTraceWindowHandle& CWindow, AActor*, const FHitResult&)
					{
						TestTrue(TEXT("C 命中携带注册时的原窗口身份"), CWindow == OriginalC);
						++OwnedCHitCount;
					}), OwnedCSubscription) == EGGYGOMeleeTraceWindowSubscribeResult::Subscribed);
		});
	if (!TestTrue(TEXT("B 注册原窗口订阅"), Trace->SubscribeWindowHit(OwnedWindowB, OwnedBCallback, OwnedBSubscription)
		== EGGYGOMeleeTraceWindowSubscribeResult::Subscribed)) { return false; }
	TickTrace(); TickTrace();
	TestEqual(TEXT("关 B 开 C 后旧扫掠只产生一个 B 回调"), OwnedBHitCount, 1);
	TestEqual(TEXT("旧扫掠不能把剩余命中交给 C"), OwnedCHitCount, 0);
	TestFalse(TEXT("旧扫掠不能写回 C 的采样基线"), Trace->bHasPreviousTransform);
	TestTrue(TEXT("C 的新采样端点与去重仍为空"), Trace->PreviousPoints.IsEmpty() && Trace->HitActorsThisWindow.IsEmpty());
	TestFalse(TEXT("B 关闭已移除原订阅令牌"), Trace->UnsubscribeWindowHit(OwnedBSubscription));
	TestEqual(TEXT("B 原生命中未广播 Legacy 全局事件"), Receiver->HitCount, LegacyHitCountBeforeOwned);
	if (!TestTrue(TEXT("回调重开后 B 失效且 C 活动"),
		Trace->QueryOwnedTraceWindow(OwnedWindowB) == EGGYGOMeleeTraceWindowQueryResult::Inactive
		&& Trace->QueryOwnedTraceWindow(OwnedWindowC) == EGGYGOMeleeTraceWindowQueryResult::Active)) { return false; }
	TickTrace();
	TestTrue(TEXT("C 首次 Tick 重新建立基线"), Trace->bHasPreviousTransform);
	TestEqual(TEXT("C 首次 Tick 不命中"), OwnedCHitCount, 0);
	TickTrace();
	TestEqual(TEXT("C 原生扫掠命中现有两个目标"), OwnedCHitCount, 2);
	TestEqual(TEXT("C 原生命中未广播 Legacy 全局事件"), Receiver->HitCount, LegacyHitCountBeforeOwned);
	Trace->Deactivate();
	TestTrue(TEXT("Deactivate 使 C 原窗口失效"), Trace->QueryOwnedTraceWindow(OwnedWindowC)
		== EGGYGOMeleeTraceWindowQueryResult::Inactive);
	TestFalse(TEXT("Deactivate 关闭 Owned 窗口"), Trace->IsTracing());
	TestFalse(TEXT("Deactivate 停止 Owned Tick"), Trace->IsComponentTickEnabled());
	TestFalse(TEXT("Deactivate 已清理 C 原订阅令牌"), Trace->UnsubscribeWindowHit(OwnedCSubscription));

	// 分段形状复用同一物理世界；合成折角与第二链，不依赖角色/动画资产。
	Trace->Activate();
	Trace->EndTraceWindow();
	auto AddSocket = [MeshAsset](FName Name, FVector Position)
	{
		USkeletalMeshSocket* Socket = NewObject<USkeletalMeshSocket>(MeshAsset);
		Socket->SocketName = Name;
		Socket->BoneName = TEXT("Root");
		Socket->RelativeLocation = Position;
		MeshAsset->GetMeshOnlySocketList().Add(Socket);
		return Socket;
	};
	AddSocket(TEXT("CurveStart"), FVector(0.0, 100.0, 0.0));
	AddSocket(TEXT("CurveBend"), FVector(100.0, 100.0, 0.0));
	AddSocket(TEXT("CurveTip"), FVector(100.0, 200.0, 0.0));
	USkeletalMeshSocket* OtherTip = AddSocket(TEXT("OtherTip"), FVector(0.0, 200.0, 0.0));
	auto AddTarget = [World, &FixtureTransform](FVector Position)
	{
		AActor* Target = World->SpawnActor<AActor>();
		USphereComponent* Sphere = NewObject<USphereComponent>(Target);
		Target->SetRootComponent(Sphere);
		Sphere->SetSphereRadius(8.0f);
		Sphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Sphere->SetCollisionResponseToAllChannels(ECR_Ignore);
		Sphere->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
		Sphere->RegisterComponent();
		Target->SetActorLocation(FixtureTransform.TransformPosition(Position));
		return Target;
	};
	AActor* CurveTarget = AddTarget(FVector(100.0, 100.0, 0.0));
	AActor* OtherTarget = AddTarget(FVector(0.0, 150.0, 0.0));
	AActor* SharedTarget = AddTarget(FVector(0.0, 100.0, 0.0));
	AActor* EmptyTarget = AddTarget(FVector(50.0, 150.0, 0.0));
	FGGYGOMeleeTraceShape Chord;
	FGGYGOMeleeTraceChain& ChordChain = Chord.Chains.AddDefaulted_GetRef();
	ChordChain.Points = {TEXT("CurveStart"), TEXT("CurveTip")};
	ChordChain.WorldRadiusCm = 10.0f;
	FGGYGOMeleeTraceShape Chains;
	Chains.Mode = EGGYGOMeleeTraceShapeMode::SocketChains;
	FGGYGOMeleeTraceChain& CurveChain = Chains.Chains.AddDefaulted_GetRef();
	CurveChain.Points = {TEXT("CurveStart"), TEXT("CurveBend"), TEXT("CurveTip")};
	CurveChain.WorldRadiusCm = 10.0f;
	FGGYGOMeleeTraceChain& OtherChain = Chains.Chains.AddDefaulted_GetRef();
	OtherChain.Points = {TEXT("CurveStart"), TEXT("OtherTip")};
	OtherChain.WorldRadiusCm = 10.0f;
	FString ShapeError;
	TestFalse(TEXT("空默认 Shape 不能变成默认 RootTip"), Trace->ValidateTraceShapeDefinition(FGGYGOMeleeTraceShape(), ShapeError));
	TestFalse(TEXT("空 Shape 给出明确原因"), ShapeError.IsEmpty());
	FGGYGOMeleeTraceShape InvalidShape = Chains;
	InvalidShape.Mode = static_cast<EGGYGOMeleeTraceShapeMode>(255);
	TestFalse(TEXT("未知 Mode 明确拒绝"), Trace->ValidateTraceShapeDefinition(InvalidShape, ShapeError));
	InvalidShape = Chains;
	InvalidShape.Chains[1].WorldRadiusCm = std::numeric_limits<float>::quiet_NaN();
	TestFalse(TEXT("后一链非有限半径拒绝整配置"), Trace->ValidateTraceShapeDefinition(InvalidShape, ShapeError));
	TestTrue(TEXT("有效链结构检查成功"), Trace->ValidateTraceShapeDefinition(Chains, ShapeError));
	TestTrue(TEXT("成功配置检查清空旧错误"), ShapeError.IsEmpty());
	// 引擎名称 Override 到 Bone 可被元数据接受，但位置分支仍查原名并返回组件位置。
	const FName AliasPoint(TEXT("TraceAliasPoint"));
	FGGYGOMeleeTraceShape AliasShape = Chord;
	AliasShape.Chains[0].Points[0] = AliasPoint;
	Mesh->AddSocketOverride(AliasPoint, TEXT("Root"));
	TestTrue(TEXT("Override到Bone的名称元数据确实成功"), Mesh->DoesSocketExist(AliasPoint)
		&& Mesh->GetSocketBoneName(AliasPoint) == TEXT("Root") && Mesh->GetSocketByName(AliasPoint) == nullptr);
	TestTrue(TEXT("原名称不是实际Bone"), Mesh->GetBoneIndex(AliasPoint) == INDEX_NONE);
	TestTrue(TEXT("原生位置分支反例确实返回有限组件位置"), Mesh->GetSocketLocation(AliasPoint).Equals(Mesh->GetComponentLocation(), 0.01));
	TestFalse(TEXT("拒绝名称成功但位置回落的伪有效点"), Trace->ValidateTraceShapeOnCurrentMesh(AliasShape, Mesh, ShapeError));
	TestTrue(TEXT("名称位置不一致给出可定位诊断"), ShapeError.Contains(TEXT("位置查询实际骨不一致")));
	Mesh->RemoveSocketOverrides(AliasPoint);
	Mesh->AddSocketOverride(AliasPoint, TEXT("CurveStart"));
	TestTrue(TEXT("Override到真实Socket仍是合法路径"), Trace->ValidateTraceShapeOnCurrentMesh(AliasShape, Mesh, ShapeError));
	Mesh->RemoveSocketOverrides(AliasPoint);
	AliasShape.Chains[0].Points[0] = TEXT("Root");
	TestTrue(TEXT("普通实际Bone名称仍是合法路径"), Trace->ValidateTraceShapeOnCurrentMesh(AliasShape, Mesh, ShapeError));

	FGGYGOMeleeTraceWindowHandle ShapeWindow;
	FDelegateHandle ShapeSubscription;
	TArray<AActor*> ShapeHits;
	auto OpenAndSubscribe = [this, Trace, Mesh, &ShapeWindow, &ShapeSubscription, &ShapeHits](const FGGYGOMeleeTraceShape& Shape)
	{
		ShapeHits.Reset();
		if (!TestTrue(TEXT("合成形状取得 Owned 原窗"), Trace->TryOpenOwnedTraceWindow(Shape, Mesh, {}, ShapeWindow)
			== EGGYGOMeleeTraceWindowOpenResult::Opened)) { return false; }
		return TestTrue(TEXT("合成形状订阅原窗"), Trace->SubscribeWindowHit(ShapeWindow,
			FGGYGOMeleeTraceWindowHitDelegate::CreateLambda(
				[this, Original = ShapeWindow, &ShapeHits](const FGGYGOMeleeTraceWindowHandle& Reported, AActor* Actor, const FHitResult&)
				{
					TestTrue(TEXT("分段命中保留原窗身份"), Reported == Original);
					ShapeHits.Add(Actor);
				}), ShapeSubscription) == EGGYGOMeleeTraceWindowSubscribeResult::Subscribed);
	};
	// 合成复现直线近似的机理：折角中段漏出窄直线，直线空白区域仍被命中。
	// 此夹具不代替实际角色第三段、蒙皮/Notify/帧间运动的后继验收。
	if (!OpenAndSubscribe(Chord)) { return false; }
	TickTrace(); TickTrace();
	TestFalse(TEXT("合成 RootTip 机理复现：漏折角中段"), ShapeHits.Contains(CurveTarget));
	TestTrue(TEXT("合成 RootTip 机理复现：命中刃间空白"), ShapeHits.Contains(EmptyTarget));
	Trace->CloseOwnedTraceWindow(ShapeWindow);

	FGGYGOMeleeTraceShape CallerShape = Chains;
	if (!OpenAndSubscribe(CallerShape)) { return false; }
	CallerShape.Chains.Reset();
	USkeletalMeshComponent* WrongMesh = NewObject<USkeletalMeshComponent>(Owner);
	TestFalse(TEXT("预检拒绝替代 Mesh 入口"), Trace->ValidateTraceShapeOnCurrentMesh(Chains, WrongMesh, ShapeError));
	TestTrue(TEXT("失败预检不改变活动原窗或首次基线"), Trace->QueryOwnedTraceWindow(ShapeWindow)
		== EGGYGOMeleeTraceWindowQueryResult::Active && !Trace->bHasPreviousTransform);
	TickTrace();
	TestEqual(TEXT("全链共同首帧只建基线"), ShapeHits.Num(), 0);
	TestEqual(TEXT("全链基线包含两链全部控制点"), Trace->PreviousPoints.Num(), 5);
	TickTrace();
	TestEqual(TEXT("双链只有三个实际刃目标且共享目标一次"), ShapeHits.Num(), 3);
	TestTrue(TEXT("折角中段与第二链独占目标均覆盖"), ShapeHits.Contains(CurveTarget) && ShapeHits.Contains(OtherTarget));
	TestTrue(TEXT("两链共同目标得到命中"), ShapeHits.Contains(SharedTarget));
	TestFalse(TEXT("分段形状不命中刃间空白"), ShapeHits.Contains(EmptyTarget));
	TickTrace();
	TestEqual(TEXT("跨链跨帧统一 Actor 去重"), ShapeHits.Num(), 3);
	Trace->CloseOwnedTraceWindow(ShapeWindow);

	if (!OpenAndSubscribe(Chains)) { return false; }
	TickTrace();
	// 每链单独都可容纳，但整窗超过25次；不能先命中前一链。
	Trace->MaxTraceSegments = 24;
	AddExpectedError(TEXT("超过上限 25或非有限。"), EAutomationExpectedErrorFlags::Contains, 1, false);
	TickTrace();
	TestEqual(TEXT("整窗预算失败前没有部分链命中"), ShapeHits.Num(), 0);
	TestTrue(TEXT("预算故障关闭同原窗"), Trace->QueryOwnedTraceWindow(ShapeWindow) == EGGYGOMeleeTraceWindowQueryResult::Inactive);
	TestTrue(TEXT("预算故障清理全部基线和去重"), Trace->PreviousPoints.IsEmpty() && !Trace->bHasPreviousTransform
		&& Trace->HitActorsThisWindow.IsEmpty());
	TestFalse(TEXT("预算故障释放原订阅"), Trace->UnsubscribeWindowHit(ShapeSubscription));
	Trace->MaxTraceSegments = 64;

	if (!OpenAndSubscribe(Chains)) { return false; }
	TickTrace();
	OtherTip->BoneName = TEXT("MissingParentBone");
	TestTrue(TEXT("坏父骨夹具仍有同名Socket"), Mesh->DoesSocketExist(TEXT("OtherTip")));
	TestFalse(TEXT("Socket存在但父骨缺失不能回落组件原点"), Trace->ValidateTraceShapeOnCurrentMesh(Chains, Mesh, ShapeError));
	TestTrue(TEXT("父骨预检失败不改原窗"), Trace->QueryOwnedTraceWindow(ShapeWindow) == EGGYGOMeleeTraceWindowQueryResult::Active);
	OtherTip->BoneName = TEXT("Root");
	OtherTip->SocketName = TEXT("RemovedOtherTip");
	if (!TestFalse(TEXT("故障夹具确实移除后一链点"), Mesh->DoesSocketExist(TEXT("OtherTip")))) { return false; }
	AddExpectedError(TEXT("Chain=1 Point=1 [OtherTip, R=10]：Socket/Bone 缺失。"), EAutomationExpectedErrorFlags::Contains, 1, false);
	TickTrace();
	TestEqual(TEXT("后一链缺点在本批任何 Sweep 前拒绝"), ShapeHits.Num(), 0);
	TestTrue(TEXT("缺点关闭同原窗并清全部资源"), Trace->QueryOwnedTraceWindow(ShapeWindow) == EGGYGOMeleeTraceWindowQueryResult::Inactive
		&& Trace->PreviousPoints.IsEmpty() && Trace->ActiveShape.Chains.IsEmpty() && Trace->WindowMesh.IsExplicitlyNull()
		&& Trace->WindowMeshAsset.IsExplicitlyNull() && Trace->WindowWorld.IsExplicitlyNull() && !Trace->IsComponentTickEnabled());
	TestFalse(TEXT("缺点释放原订阅"), Trace->UnsubscribeWindowHit(ShapeSubscription));
	OtherTip->SocketName = TEXT("OtherTip");
	if (!OpenAndSubscribe(Chains)) { return false; }
	TickTrace();
	const FVector ValidOtherTipLocation = OtherTip->RelativeLocation;
	OtherTip->RelativeLocation.X = std::numeric_limits<double>::quiet_NaN();
	AddExpectedError(TEXT("Chain=1 Point=1 [OtherTip, R=10]：Socket 局部位置/旋转/缩放非有限值。"), EAutomationExpectedErrorFlags::Contains, 1, false);
	TickTrace();
	TestEqual(TEXT("后一链非有限点在任何Sweep前拒绝"), ShapeHits.Num(), 0);
	TestTrue(TEXT("非有限点关闭同原窗并停Tick"), Trace->QueryOwnedTraceWindow(ShapeWindow)
		== EGGYGOMeleeTraceWindowQueryResult::Inactive && !Trace->IsComponentTickEnabled());
	OtherTip->RelativeLocation = ValidOtherTipLocation;

	if (!OpenAndSubscribe(Chains)) { return false; }
	const FGGYGOMeleeTraceWindowHandle ValidShapeWindow = ShapeWindow;
	const uint64 SerialBeforeInvalid = Trace->WindowSerial;
	AddExpectedError(TEXT("无效，至少两点且半径为有限值 >=1cm。"), EAutomationExpectedErrorFlags::Contains, 1, false);
	TestTrue(TEXT("获准替换时非法新 Shape 失败而非回落"), Trace->TryOpenOwnedTraceWindow(InvalidShape, Mesh, ShapeWindow, ShapeWindow)
		== EGGYGOMeleeTraceWindowOpenResult::InvalidConfiguration);
	TestFalse(TEXT("非法 Shape 不签发句柄"), ShapeWindow.HasWindow());
	TestTrue(TEXT("非法替换不继续旧窗或推进序号"), Trace->QueryOwnedTraceWindow(ValidShapeWindow)
		== EGGYGOMeleeTraceWindowQueryResult::Inactive && Trace->WindowSerial == SerialBeforeInvalid && !Trace->IsTracing());

	if (!OpenAndSubscribe(Chains)) { return false; }
	TickTrace();
	Mesh->USkinnedMeshComponent::SetSkinnedAssetAndUpdate(nullptr);
	AddExpectedError(TEXT("活动窗口原主 Mesh/Asset 失效或替换。"), EAutomationExpectedErrorFlags::Contains, 1, false);
	TickTrace();
	TestEqual(TEXT("原 Mesh 资产失效前无部分命中"), ShapeHits.Num(), 0);
	TestTrue(TEXT("同组件换资产也使原窗失效"), Trace->QueryOwnedTraceWindow(ShapeWindow) == EGGYGOMeleeTraceWindowQueryResult::Inactive);
	Mesh->USkinnedMeshComponent::SetSkinnedAssetAndUpdate(MeshAsset);
	Mesh->GetEditableComponentSpaceTransforms() = MeshAsset->GetRefSkeleton().GetRefBonePose();

	// 回调在第一链关闭B、取得C，第二链不得借C身份继续，C必须重建完整基线。
	int32 ChainBHits = 0;
	int32 ChainCHits = 0;
	FGGYGOMeleeTraceWindowHandle ChainBWindow;
	FGGYGOMeleeTraceWindowHandle ChainCWindow;
	FDelegateHandle ChainBSubscription;
	FDelegateHandle ChainCSubscription;
	if (!TestTrue(TEXT("跨链重入取得B"), Trace->TryOpenOwnedTraceWindow(Chains, Mesh, {}, ChainBWindow)
		== EGGYGOMeleeTraceWindowOpenResult::Opened)) { return false; }
	if (!TestTrue(TEXT("跨链重入订阅B"), Trace->SubscribeWindowHit(ChainBWindow,
		FGGYGOMeleeTraceWindowHitDelegate::CreateLambda(
			[this, Trace, Mesh, &Chains, ChainBWindow, &ChainBHits, &ChainCHits, &ChainCWindow, &ChainCSubscription]
			(const FGGYGOMeleeTraceWindowHandle& Reported, AActor*, const FHitResult&)
			{
				++ChainBHits;
				TestTrue(TEXT("跨链B命中原身份"), Reported == ChainBWindow);
				Trace->CloseOwnedTraceWindow(ChainBWindow);
				if (!TestTrue(TEXT("跨链回调打开C"), Trace->TryOpenOwnedTraceWindow(Chains, Mesh, {}, ChainCWindow)
					== EGGYGOMeleeTraceWindowOpenResult::Opened)) { return; }
				TestTrue(TEXT("跨链C独立订阅"), Trace->SubscribeWindowHit(ChainCWindow,
					FGGYGOMeleeTraceWindowHitDelegate::CreateLambda(
						[this, OriginalC = ChainCWindow, &ChainCHits](const FGGYGOMeleeTraceWindowHandle& CWindow, AActor*, const FHitResult&)
						{
							TestTrue(TEXT("跨链C命中原身份"), CWindow == OriginalC);
							++ChainCHits;
						}), ChainCSubscription) == EGGYGOMeleeTraceWindowSubscribeResult::Subscribed);
			}), ChainBSubscription) == EGGYGOMeleeTraceWindowSubscribeResult::Subscribed)) { return false; }
	TickTrace(); TickTrace();
	TestEqual(TEXT("B回调后旧批跨链立即停止"), ChainBHits, 1);
	TestEqual(TEXT("旧批不能用C发布后续链命中"), ChainCHits, 0);
	TestTrue(TEXT("旧批不污染C基线与去重"), !Trace->bHasPreviousTransform && Trace->PreviousPoints.IsEmpty()
		&& Trace->HitActorsThisWindow.IsEmpty());
	TickTrace();
	TestEqual(TEXT("C全链首帧仍不命中"), ChainCHits, 0);
	TickTrace();
	TestEqual(TEXT("C重新采样全部实际目标"), ChainCHits, 3);
	Trace->Deactivate();
	TestTrue(TEXT("多链Deactivate清配置基线资源及原订阅"), Trace->ActiveShape.Chains.IsEmpty()
		&& Trace->PreviousPoints.IsEmpty() && Trace->WindowHitSubscriptions.IsEmpty() && Trace->WindowMesh.IsExplicitlyNull()
		&& Trace->WindowMeshAsset.IsExplicitlyNull() && Trace->WindowWorld.IsExplicitlyNull() && !Trace->IsComponentTickEnabled());
	TestFalse(TEXT("多链Deactivate原令牌已移除"), Trace->UnsubscribeWindowHit(ChainCSubscription));
	TestEqual(TEXT("全部分段Owned命中仍隔离Legacy事件"), Receiver->HitCount, LegacyHitCountBeforeOwned);

	Trace->OnMeleeHit.RemoveDynamic(Receiver.Get(), &UGGYGOMeleeTraceTestReceiver::HandleHit);
	return true;
}

#endif
