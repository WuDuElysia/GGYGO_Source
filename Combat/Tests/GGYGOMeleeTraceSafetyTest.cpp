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

	AddExpectedError(TEXT("拒绝无效 Mesh/Socket/半径/采样上限"), EAutomationExpectedErrorFlags::Contains, 1);
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
	AddExpectedError(TEXT("超过上限"), EAutomationExpectedErrorFlags::Contains, 1);
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
	TestTrue(TEXT("Deactivate 清零采样端点"), Trace->PreviousStart.IsZero() && Trace->PreviousEnd.IsZero());
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
	TestTrue(TEXT("OnUnregister 清零采样端点"), Trace->PreviousStart.IsZero() && Trace->PreviousEnd.IsZero());
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
	AddExpectedError(TEXT("拒绝 Owned Begin：预期原窗口不是本组件活动 Owned 窗口"), EAutomationExpectedErrorFlags::Contains, 1);
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
	TestTrue(TEXT("C 的新采样端点与去重仍为空"), Trace->PreviousStart.IsZero()
		&& Trace->PreviousEnd.IsZero() && Trace->HitActorsThisWindow.IsEmpty());
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

	Trace->OnMeleeHit.RemoveDynamic(Receiver.Get(), &UGGYGOMeleeTraceTestReceiver::HandleHit);
	return true;
}

#endif
