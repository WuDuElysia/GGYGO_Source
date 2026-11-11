/** @file GGYGOCombatHitQueryTest.cpp @brief 原生范围／弹体碰撞事实、查询寿命去重及权威拒绝契约。 */
#include "Combat/HitDetection/GGYGOCombatHitQuery.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Components/SphereComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "UObject/StrongObjectPtr.h"

#include <limits>

namespace GGYGOCombatHitQueryTestPrivate
{
	USphereComponent* AddBody(AActor* Actor, const FVector& Location, UPhysicalMaterial* Material)
	{
		USphereComponent* Body = NewObject<USphereComponent>(Actor);
		Actor->AddInstanceComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->InitSphereRadius(25.0f);
		Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Body->SetCollisionObjectType(ECC_Pawn);
		Body->SetCollisionResponseToAllChannels(ECR_Overlap);
		Body->SetGenerateOverlapEvents(false);
		Body->SetPhysMaterialOverride(Material);
		if (!Actor->GetRootComponent()) { Actor->SetRootComponent(Body); }
		else { Body->SetupAttachment(Actor->GetRootComponent()); }
		Body->RegisterComponent();
		Body->SetWorldLocation(Location);
		return Body;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOCombatHitQueryTest, "GGYGO.Combat.HitQuery.AreaAndProjectile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOCombatHitQueryTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UPhysicalMaterial> Material(NewObject<UPhysicalMaterial>());
	// Solver 初始化前建立材质 handle，使查询回填来自真实 PhysicsMaterial。
	Material->GetPhysicsMaterial();
	UWorld::InitializationValues Init;
	Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(true)
		.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	if (!TestNotNull(TEXT("建立原生碰撞世界"), World)) { return false; }
	struct FWorldCleanup
	{
		UWorld* World;
		~FWorldCleanup() { World->DestroyWorld(false); }
	} Cleanup{World};

	AActor* Source = World->SpawnActor<AActor>();
	AActor* TargetA = World->SpawnActor<AActor>();
	AActor* TargetB = World->SpawnActor<AActor>();
	AActor* SideTarget = World->SpawnActor<AActor>();
	AActor* Projectile = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("来源 Actor"), Source) || !TestNotNull(TEXT("目标 A"), TargetA)
		|| !TestNotNull(TEXT("目标 B"), TargetB) || !TestNotNull(TEXT("侧向目标"), SideTarget)
		|| !TestNotNull(TEXT("弹体忽略 Actor"), Projectile)) { return false; }
	using namespace GGYGOCombatHitQueryTestPrivate;
	AddBody(Source, FVector::ZeroVector, Material.Get());
	AddBody(TargetA, FVector(300.0, 0.0, 0.0), Material.Get());
	AddBody(TargetA, FVector(300.0, 25.0, 0.0), Material.Get());
	USphereComponent* BlockingBody = AddBody(TargetB, FVector(600.0, 0.0, 0.0), Material.Get());
	AddBody(SideTarget, FVector(450.0, 180.0, 0.0), Material.Get());
	USphereComponent* ProjectileBody = AddBody(Projectile, FVector(150.0, 0.0, 0.0), Material.Get());

	FString Error;
	TestFalse(TEXT("空来源拒绝开启并给原因"), FGGYGOCombatHitQuery::Open(nullptr, Error).IsValid());
	TestFalse(TEXT("开启拒绝原因不为空"), Error.IsEmpty());
	TSharedPtr<FGGYGOCombatHitQuery> Area = FGGYGOCombatHitQuery::Open(Source, Error);
	if (!TestTrue(TEXT("权威来源开启一次范围寿命"), Area.IsValid())) { return false; }
	TArray<FGGYGOCombatOverlapFact> Overlaps;
	if (!TestTrue(TEXT("无目标是成功的空查询"), Area->OverlapSphere(FVector::ZeroVector, 80.0f, ECC_Pawn, Overlaps, Error))) { return false; }
	TestEqual(TEXT("忽略来源自身碰撞体"), Overlaps.Num(), 0);
	const FVector AreaCenter(300.0, 0.0, 0.0);
	if (!TestTrue(TEXT("原生球范围查询成功"), Area->OverlapSphere(AreaCenter, 90.0f, ECC_Pawn, Overlaps, Error))
		|| !TestEqual(TEXT("同 Actor 两碰撞体只返回一次"), Overlaps.Num(), 1)) { return false; }
	TestTrue(TEXT("重叠保留真实原组件及 Actor"), Overlaps[0].Overlap.GetActor() == TargetA
		&& Overlaps[0].Overlap.GetComponent() && Overlaps[0].Overlap.GetComponent()->GetOwner() == TargetA);
	TestTrue(TEXT("重叠保留原来源和实际中心"), Overlaps[0].SourceActor.Get() == Source && Overlaps[0].Origin == AreaCenter);
	TestFalse(TEXT("Touch 结果没有被虚构为阻挡"), Overlaps[0].Overlap.bBlockingHit);
	if (!TestTrue(TEXT("同寿命重复球查询仍成功"), Area->OverlapSphere(AreaCenter, 90.0f, ECC_Pawn, Overlaps, Error))) { return false; }
	TestEqual(TEXT("同寿命不重复命中"), Overlaps.Num(), 0);
	if (!TestTrue(TEXT("盒范围消费同一寿命"), Area->OverlapBox(FVector(450.0, 0.0, 0.0), FQuat::Identity,
		FVector(210.0, 40.0, 40.0), ECC_Pawn, Overlaps, Error))
		|| !TestEqual(TEXT("不同形状共用去重，只新增 B"), Overlaps.Num(), 1)) { return false; }
	TestTrue(TEXT("盒范围真实新增 B"), Overlaps[0].Overlap.GetActor() == TargetB);
	if (!TestTrue(TEXT("盒范围使用作者旋转"), Area->OverlapBox(FVector(450.0, 0.0, 0.0),
		FRotator(0.0, 90.0, 0.0).Quaternion(), FVector(210.0, 40.0, 40.0), ECC_Pawn, Overlaps, Error))
		|| !TestEqual(TEXT("旋转盒沿侧向覆盖目标"), Overlaps.Num(), 1)) { return false; }
	TestTrue(TEXT("旋转盒没有退回无旋转形状"), Overlaps[0].Overlap.GetActor() == SideTarget);

	TSharedPtr<FGGYGOCombatHitQuery> NextWindow = FGGYGOCombatHitQuery::Open(Source, Error);
	if (!TestTrue(TEXT("下一作者窗口持独立实例"), NextWindow.IsValid())) { return false; }
	Area->Close();
	Area->Close();
	Overlaps.AddDefaulted();
	TestFalse(TEXT("关闭实例拒绝再查询"), Area->OverlapSphere(AreaCenter, 90.0f, ECC_Pawn, Overlaps, Error));
	TestTrue(TEXT("关闭失败输出为空且有原因"), Overlaps.IsEmpty() && !Error.IsEmpty());
	TestFalse(TEXT("负半径明确拒绝"), NextWindow->OverlapSphere(AreaCenter, -1.0f, ECC_Pawn, Overlaps, Error));
	TestFalse(TEXT("非法盒尺寸明确拒绝"), NextWindow->OverlapBox(AreaCenter, FQuat::Identity,
		FVector(100.0, -1.0, 100.0), ECC_Pawn, Overlaps, Error));
	TestFalse(TEXT("非单位盒旋转明确拒绝"), NextWindow->OverlapBox(AreaCenter, FQuat(0.0, 0.0, 0.0, 0.0),
		FVector(100.0), ECC_Pawn, Overlaps, Error));
	TestFalse(TEXT("非法碰撞通道明确拒绝"), NextWindow->OverlapSphere(AreaCenter, 90.0f, ECC_MAX, Overlaps, Error));
	if (!TestTrue(TEXT("非法查询未消费新窗去重"), NextWindow->OverlapSphere(AreaCenter, 90.0f, ECC_Pawn, Overlaps, Error))
		|| !TestEqual(TEXT("旧实例关闭不影响新窗再次命中 A"), Overlaps.Num(), 1)) { return false; }

	// 弹体借用原生移动段，本测试不引入弹体 Tick 或另一套位移积分。
	BlockingBody->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	ProjectileBody->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	TArray<AActor*> IgnoredActors{Projectile};
	TSharedPtr<FGGYGOCombatHitQuery> Sweep = FGGYGOCombatHitQuery::Open(Source, Error, IgnoredActors);
	if (!TestTrue(TEXT("弹体开启并显式忽略自身"), Sweep.IsValid())) { return false; }
	TArray<FGGYGOCombatSweepFact> Hits;
	FHitResult BlockingHit;
	const FVector From = FVector::ZeroVector;
	const FVector To(700.0, 0.0, 0.0);
	if (!TestTrue(TEXT("弹体段执行原生连续球扫"), Sweep->SweepSphere(From, To, 10.0f, ECC_Pawn, Hits, BlockingHit, Error))
		|| !TestEqual(TEXT("扫掠只返回 A 与 B，不含来源或弹体自身"), Hits.Num(), 2)) { return false; }
	bool bFoundBlockingB = false;
	for (const FGGYGOCombatSweepFact& Fact : Hits)
	{
		TestTrue(TEXT("球扫结果保留真实来源、段端点与组件"), Fact.SourceActor.Get() == Source
			&& Fact.Origin == From && Fact.Hit.TraceStart == From && Fact.Hit.TraceEnd == To
			&& Fact.Hit.GetComponent() && (Fact.Hit.GetActor() == TargetA || Fact.Hit.GetActor() == TargetB));
		if (Fact.Hit.GetActor() == TargetB)
		{
			bFoundBlockingB = true;
			TestTrue(TEXT("真实阻挡接触而非伪造命中"), Fact.Hit.bBlockingHit && !Fact.Hit.bStartPenetrating
				&& Fact.Hit.Time > 0.0f && Fact.Hit.Time < 1.0f && !Fact.Hit.ImpactPoint.ContainsNaN());
			TestTrue(TEXT("球扫真实返回物理材质"), Fact.Hit.PhysMaterial.Get() == Material.Get());
		}
	}
	TestTrue(TEXT("扫到真实阻挡 B"), bFoundBlockingB);
	TestTrue(TEXT("独立阻挡返回原组件及完整接触事实"), BlockingHit.bBlockingHit
		&& BlockingHit.GetComponent() == BlockingBody && BlockingHit.PhysMaterial.Get() == Material.Get());
	if (!TestTrue(TEXT("同弹体重复段仍是有效查询"), Sweep->SweepSphere(From, To, 10.0f, ECC_Pawn, Hits, BlockingHit, Error))) { return false; }
	TestEqual(TEXT("同弹体寿命 Actor 去重"), Hits.Num(), 0);
	TestTrue(TEXT("Actor 去重不丢失原物理阻挡"), BlockingHit.bBlockingHit && BlockingHit.GetActor() == TargetB);
	TestFalse(TEXT("非有限弹体终点拒绝"), Sweep->SweepSphere(From,
		FVector(std::numeric_limits<double>::infinity(), 0.0, 0.0), 10.0f, ECC_Pawn, Hits, BlockingHit, Error));
	TestTrue(TEXT("非法弹体段输出为空且有原因"), Hits.IsEmpty() && !Error.IsEmpty());
	TestFalse(TEXT("非法段清除旧阻挡输出"), BlockingHit.bBlockingHit);
	Sweep->Close();
	TestFalse(TEXT("弹体关闭后不再查询"), Sweep->SweepSphere(From, To, 10.0f, ECC_Pawn, Hits, BlockingHit, Error));

	Source->SetRole(ROLE_SimulatedProxy);
	TestFalse(TEXT("非权威来源不能开新查询"), FGGYGOCombatHitQuery::Open(Source, Error).IsValid());
	TestFalse(TEXT("原查询失去权威明确拒绝"), NextWindow->OverlapSphere(AreaCenter, 90.0f, ECC_Pawn, Overlaps, Error));
	Source->SetRole(ROLE_Authority);
	if (!TestTrue(TEXT("销毁来源 Actor"), Source->Destroy())) { return false; }
	TestFalse(TEXT("来源销毁后原查询拒绝"), NextWindow->OverlapSphere(AreaCenter, 90.0f, ECC_Pawn, Overlaps, Error));
	TestTrue(TEXT("来源失效输出为空且有原因"), Overlaps.IsEmpty() && !Error.IsEmpty());
	NextWindow->Close();
	return !HasAnyErrors();
}
#endif
