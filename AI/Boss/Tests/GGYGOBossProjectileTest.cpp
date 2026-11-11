/** @file GGYGOBossProjectileTest.cpp @brief 同 Actor 的碰触去重不能吞掉弹体物理阻挡。 */
#include "AI/Boss/Abilities/GGYGOBossProjectile.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Combat/HitDetection/GGYGOCombatHitQuery.h"
#include "Components/SphereComponent.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "NiagaraSystem.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOBossProjectileBlockingTest, "GGYGO.BossAI.Projectile.TouchThenBlock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOBossProjectileBlockingTest::RunTest(const FString& Parameters)
{
	UWorld::InitializationValues Init;
	Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(true)
		.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	if (!TestNotNull(TEXT("真实碰撞世界"), World)) { return false; }
	struct FWorldCleanup
	{
		UWorld* World;
		~FWorldCleanup() { World->DestroyWorld(false); }
	} Cleanup{World};
	AActor* Source = World->SpawnActor<AActor>();
	AActor* Target = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("原来源"), Source) || !TestNotNull(TEXT("双组件目标"), Target)) { return false; }
	const auto AddBody = [Target](double X, ECollisionResponse Response)
	{
		USphereComponent* Body = NewObject<USphereComponent>(Target);
		Target->AddInstanceComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->InitSphereRadius(25.0f);
		Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Body->SetCollisionObjectType(ECC_Pawn);
		Body->SetCollisionResponseToAllChannels(Response);
		Body->SetGenerateOverlapEvents(false);
		if (!Target->GetRootComponent()) { Target->SetRootComponent(Body); }
		else { Body->SetupAttachment(Target->GetRootComponent()); }
		Body->RegisterComponent();
		Body->SetWorldLocation(FVector(X, 0.0, 0.0));
	};
	AddBody(300.0, ECR_Overlap);
	AddBody(600.0, ECR_Block);
	// 世界不开始播放：只经真实公开 Configure／移动段入口检查碰撞和销毁，不启动视觉或另一套 Tick。
	TStrongObjectPtr<UNiagaraSystem> Visual(NewObject<UNiagaraSystem>());
	for (bool bSplitSegment : {false, true})
	{
		AGGYGOBossProjectile* Projectile = World->SpawnActor<AGGYGOBossProjectile>();
		if (!TestNotNull(TEXT("生产弹体"), Projectile)) { return false; }
		int32 HitCount = 0;
		int32 FailureCount = 0;
		FString Error;
		const bool bConfigured = Projectile->Configure(Source, 10.0f, 100.0f, 3.0f, 0.0f, ECC_Pawn,
			Visual.Get(), FTransform::Identity,
			FGGYGOBossProjectileHit::CreateLambda([&](const FGGYGOCombatSweepFact& Fact)
			{
				++HitCount;
				TestTrue(TEXT("仅向原目标结算一次 Touch"), Fact.Hit.GetActor() == Target && !Fact.Hit.bBlockingHit);
			}), FGGYGOBossProjectileFailure::CreateLambda([&](const FString&) { ++FailureCount; }), Error);
		if (!TestTrue(TEXT("真实弹体配置成功"), bConfigured)) { return false; }
		if (bSplitSegment)
		{
			Projectile->QueryMovementSegment(FVector::ZeroVector, FVector(450.0, 0.0, 0.0));
			TestEqual(TEXT("前段 Touch 已结算一次"), HitCount, 1);
			TestFalse(TEXT("只有 Touch 时弹体继续存活"), Projectile->IsActorBeingDestroyed());
			Projectile->QueryMovementSegment(FVector(450.0, 0.0, 0.0), FVector(700.0, 0.0, 0.0));
		}
		else
		{
			Projectile->QueryMovementSegment(FVector::ZeroVector, FVector(700.0, 0.0, 0.0));
		}
		TestEqual(TEXT("同 Actor 去重仍只结算一次"), HitCount, 1);
		TestEqual(TEXT("合法阻挡不是查询失败"), FailureCount, 0);
		TestTrue(bSplitSegment ? TEXT("跨段 Touch→Block 销毁生产弹体") : TEXT("同段 Touch→Block 销毁生产弹体"),
			Projectile->IsActorBeingDestroyed());
	}
	return !HasAnyErrors();
}
#endif
