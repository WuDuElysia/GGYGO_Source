/** @file GGYGOCharacterLandingTest.cpp @brief 真实 CMC 碰撞验证角色互阻、拒绝互踩及滑离后环境落地。 */
#include "Character/Tests/GGYGOCharacterLandingTestTypes.h"

#include "GameFramework/Character.h"

void UGGYGOCharacterLandingTestObserver::RecordLanded(const FHitResult& Hit)
{
	if (Cast<ACharacter>(Hit.GetActor())) { ++CharacterLandings; }
	else { ++EnvironmentLandings; }
}

void UGGYGOCharacterLandingTestObserver::RecordMovement(float DeltaSeconds, FVector OldLocation, FVector OldVelocity)
{
	CompletedLocations.Add(GetTypedOuter<ACharacter>()->GetActorLocation());
}

void UGGYGOCharacterLandingTestObserver::RecordHit(AActor* SelfActor, AActor* OtherActor, FVector NormalImpulse, const FHitResult& Hit)
{
	if (Cast<ACharacter>(OtherActor)) ++CharacterImpacts;
}

#if WITH_DEV_AUTOMATION_TESTS
#include "AI/Boss/GGYGOBossCharacter.h"
#include "Character/GGYGOHeroCharacter.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Character/Data/GGYGOMovementSet.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOCharacterLandingTest, "GGYGO.Character.Collision.NoCharacterPerch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOCharacterLandingTest::RunTest(const FString& Parameters)
{
	UWorld::InitializationValues Init;
	Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(true)
		.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	if (!TestNotNull(TEXT("原生碰撞世界"), World)) { return false; }
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	struct FWorldCleanup
	{
		UWorld* World;
		~FWorldCleanup()
		{
			World->DestroyWorld(false);
			GEngine->DestroyWorldContext(World);
			World->GetPackage()->SetDirtyFlag(false);
		}
	} Cleanup{World};
	AActor* Ground = World->SpawnActor<AActor>();
	UBoxComponent* Floor = NewObject<UBoxComponent>(Ground);
	Ground->AddInstanceComponent(Floor);
	Ground->SetRootComponent(Floor);
	Floor->SetBoxExtent(FVector(3000, 3000, 50));
	Floor->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Floor->SetCollisionObjectType(ECC_WorldStatic);
	Floor->SetCollisionResponseToAllChannels(ECR_Block);
	Floor->RegisterComponent();
	Floor->SetWorldLocation(FVector(0, 0, -50));

	// 两种实际原生角色分支，覆盖双向及同类角色；不注入伪造命中结果或手动 Landed。
	for (bool bUpperBoss : {false, true})
	for (bool bLowerBoss : {false, true})
	for (double Offset : {0.0, 25.0})
	{
		const FString Case = FString::Printf(TEXT("%s落向%s Offset=%.0f"),
			bUpperBoss ? TEXT("Boss") : TEXT("Hero"), bLowerBoss ? TEXT("Boss") : TEXT("Hero"), Offset);
		FActorSpawnParameters Spawn;
		Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		const auto SpawnCharacter = [&](bool bBoss, const FVector& Location)
		{
			UClass* Class = bBoss ? AGGYGOBossCharacter::StaticClass() : AGGYGOHeroCharacter::StaticClass();
			AGGYGOCharacterBase* Character = World->SpawnActor<AGGYGOCharacterBase>(Class, Location, FRotator::ZeroRotator, Spawn);
			Character->GetCapsuleComponent()->SetCapsuleSize(bBoss ? 60.0f : 35.0f, bBoss ? 125.0f : 88.0f);
			Character->GetGGYGOMovementComponent()->bRunPhysicsWithNoController = true;
			Character->GetGGYGOMovementComponent()->Activate(true);
			TestTrue(Case + TEXT(" 物理组件处于活动状态"), Character->GetGGYGOMovementComponent()->IsActive());
			UGGYGOMovementSet* MovementSet = NewObject<UGGYGOMovementSet>(Character);
			MovementSet->bUseCurveDrivenSpeed = false;
			TestTrue(Case + TEXT(" 绑定原生固定速度测试配置"), Character->GetGGYGOMovementComponent()->SetMovementSet(MovementSet));
			return Character;
		};
		const double LowerHalfHeight = bLowerBoss ? 125.0 : 88.0;
		AGGYGOCharacterBase* Lower = SpawnCharacter(bLowerBoss, FVector(0, 0, LowerHalfHeight + 2));
		AGGYGOCharacterBase* Upper = SpawnCharacter(bUpperBoss, FVector(Offset, 0, LowerHalfHeight + 500));
		UGGYGOCharacterMovementComponent* Move = Upper->GetGGYGOMovementComponent();
		Lower->GetGGYGOMovementComponent()->DisableMovement();
		TStrongObjectPtr<UGGYGOCharacterLandingTestObserver> Observer(NewObject<UGGYGOCharacterLandingTestObserver>(Upper));
		Upper->LandedDelegate.AddDynamic(Observer.Get(), &UGGYGOCharacterLandingTestObserver::RecordLanded);
		TestEqual(Case + TEXT(" 下方胶囊原生不可走配置"),
			Lower->GetCapsuleComponent()->GetWalkableSlopeOverride().GetWalkableSlopeBehavior(), WalkableSlope_Unwalkable);
		Move->SetMovementMode(MOVE_Falling);
		bool bUsedCharacterBase = false;
		const bool bCheckReplay = !bUpperBoss && !bLowerBoss && Offset == 0.0;
		bool bReplayedContact = false;
		const FVector InitialLocation = Upper->GetActorLocation();
		FNetworkPredictionData_Client_Character* ClientData = Move->GetPredictionData_Client_Character();
		if (bCheckReplay)
		{
			Upper->OnCharacterMovementUpdated.AddDynamic(Observer.Get(), &UGGYGOCharacterLandingTestObserver::RecordMovement);
			Upper->OnActorHit.AddDynamic(Observer.Get(), &UGGYGOCharacterLandingTestObserver::RecordHit);
		}
		// 完整 MoveAutonomous -> PerformMovement，包含原生力消费、物理和完成通知。
		for (int32 Frame = 0; Frame < 480; ++Frame)
		{
			constexpr float DeltaSeconds = 1.0f / 60.0f;
			FSavedMovePtr SavedMove;
			if (bCheckReplay && !bReplayedContact)
			{
				ClientData->CurrentTimeStamp = (Frame + 1) * DeltaSeconds;
				SavedMove = ClientData->AllocateNewMove();
				SavedMove->SetMoveFor(Upper, DeltaSeconds, FVector::ZeroVector, *ClientData);
				ClientData->SavedMoves.Add(SavedMove);
			}
			Move->MoveAutonomous((Frame + 1) * DeltaSeconds, DeltaSeconds, 0, FVector::ZeroVector);
			if (SavedMove.IsValid()) SavedMove->PostUpdate(Upper, FSavedMove_Character::PostUpdate_Record);
			bUsedCharacterBase |= Upper->GetMovementBaseObject() == Lower->GetCapsuleComponent();
			if (bCheckReplay && !bReplayedContact && Observer->CharacterImpacts > 0)
			{
				// 最新 move 刚碰撞、尚未执行下一 move：把校正后的胶囊/速度恢复到更早区间起点。
				// SavedMove 和 replay 循环用真实原生实现；这是本地校正边界夹具，不冒充网络传包。
				const TArray<FVector> PredictedLocations = Observer->CompletedLocations;
				Observer->CompletedLocations.Reset();
				Upper->SetActorLocation(InitialLocation, false, nullptr, ETeleportType::TeleportPhysics);
				Move->Velocity = FVector::ZeroVector;
				ClientData->bUpdatePosition = true;
				TestTrue(TEXT("碰撞后校正执行真实 SavedMove 重放"), Move->ClientUpdatePositionAfterServerUpdate());
				TestEqual(TEXT("完整物理区间逐帧重放数量"), Observer->CompletedLocations.Num(), PredictedLocations.Num());
				bool bSameTrajectory = Observer->CompletedLocations.Num() == PredictedLocations.Num();
				for (int32 Index = 0; Index < FMath::Min(Observer->CompletedLocations.Num(), PredictedLocations.Num()); ++Index)
					bSameTrajectory &= Observer->CompletedLocations[Index].Equals(PredictedLocations[Index], 0.0001);
				TestTrue(TEXT("未来碰撞不污染较早重放区间且重放碰撞轨迹一致"), bSameTrajectory);
				if (TestFalse(TEXT("预测和重放均有完成样本"), PredictedLocations.IsEmpty() || Observer->CompletedLocations.IsEmpty()))
					AddInfo(FString::Printf(TEXT("校正重放 %d 个真实 move，首帧预测=%s 重放=%s"), PredictedLocations.Num(),
						*PredictedLocations[0].ToString(), *Observer->CompletedLocations[0].ToString()));
				Upper->OnCharacterMovementUpdated.RemoveDynamic(Observer.Get(), &UGGYGOCharacterLandingTestObserver::RecordMovement);
				Upper->OnActorHit.RemoveDynamic(Observer.Get(), &UGGYGOCharacterLandingTestObserver::RecordHit);
				ClientData->SavedMoves.Reset();
				bReplayedContact = true;
			}
		}
		if (bCheckReplay) TestTrue(TEXT("实际到达同轴碰撞后的校正边界"), bReplayedContact);
		TestEqual(Case + TEXT(" 不产生角色 Landed"), Observer->CharacterLandings, 0);
		TestEqual(Case + TEXT(" 只在环境真实落地一次"), Observer->EnvironmentLandings, 1);
		TestFalse(Case + TEXT(" 不以角色为 MovementBase"), bUsedCharacterBase);
		TestTrue(Case + TEXT(" 最终稳定 Walking 且基底为环境"), Move->IsMovingOnGround() && Upper->GetMovementBaseObject() == Floor);
		AddInfo(Case + TEXT(" Final=") + Upper->GetActorLocation().ToString());

		// 在同一实际组件上水平扫掠，证明拒绝互踩没有关闭角色互阻。
		Move->StopMovementImmediately();
		Upper->SetActorLocation(FVector(-300, 0, (bUpperBoss ? 125.0 : 88.0) + 2), false, nullptr, ETeleportType::TeleportPhysics);
		FHitResult SideHit;
		Move->SafeMoveUpdatedComponent(FVector(600, 0, 0), FQuat::Identity, true, SideHit);
		TestTrue(Case + TEXT(" 侧向仍被原角色阻挡"), SideHit.IsValidBlockingHit() && SideHit.GetActor() == Lower);
		Upper->LandedDelegate.RemoveDynamic(Observer.Get(), &UGGYGOCharacterLandingTestObserver::RecordLanded);
		Upper->Destroy();
		Lower->Destroy();
	}
	return !HasAnyErrors();
}
#endif
