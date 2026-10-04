#include "AI/Boss/Tests/GGYGOBossMeleeLifecycleTestAbility.h"
#include "AbilitySystemComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOBossMeleeLifecycleTestAbility)

void UGGYGOBossMeleeLifecycleTestAbility::ActivateAbility(FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	bCleaningUp = false;
	bBlendingOut = false;
	ActiveMesh = CastChecked<ACharacter>(GetAvatarActorFromActorInfo())->GetMesh();
	SavedMeshTick = ActiveMesh->VisibilityBasedAnimTickOption;
	bSavedUpdateRateOptimizations = ActiveMesh->bEnableUpdateRateOptimizations;
	bChangedMeshTick = true;
	ActiveMesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	ActiveMesh->bEnableUpdateRateOptimizations = false;
}

void UGGYGOBossMeleeLifecycleTestAbility::FinishForTest()
{
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, false, false);
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOBossMeleeEndReentryTest, "GGYGO.BossAI.Melee.EndReentry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOBossMeleeEndReentryTest::RunTest(const FString& Parameters)
{
	UWorld::InitializationValues Init;
	Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(true)
		.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	if (!TestNotNull(TEXT("创建生命周期测试世界"), World)) { return false; }
	ACharacter* Owner = World->SpawnActor<ACharacter>();
	UAbilitySystemComponent* ASC = NewObject<UAbilitySystemComponent>(Owner);
	ASC->RegisterComponent();
	ASC->InitAbilityActorInfo(Owner, Owner);
	USkeletalMeshComponent* Mesh = Owner->GetMesh();
	Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
	Mesh->bEnableUpdateRateOptimizations = true;
	const FGameplayAbilitySpecHandle Handle = ASC->GiveAbility(FGameplayAbilitySpec(UGGYGOBossMeleeLifecycleTestAbility::StaticClass(), 1));
	if (!TestTrue(TEXT("首个测试动作激活"), ASC->TryActivateAbility(Handle))) { World->DestroyWorld(false); return false; }
	UGGYGOBossMeleeLifecycleTestAbility* Ability = Cast<UGGYGOBossMeleeLifecycleTestAbility>(ASC->FindAbilitySpecFromHandle(Handle)->GetPrimaryInstance());
	bool bReactivated = false;
	bool bRestoredBeforeBroadcast = false;
	const FDelegateHandle EndHandle = ASC->OnAbilityEnded.AddLambda([&](const FAbilityEndedData& Data)
	{
		bRestoredBeforeBroadcast = Mesh->VisibilityBasedAnimTickOption == EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered
			&& Mesh->bEnableUpdateRateOptimizations && !Ability->HasActiveMeshForTest();
		bReactivated = ASC->TryActivateAbility(Handle);
	});
	Ability->FinishForTest();
	TestTrue(TEXT("广播前恢复 Mesh 并清引用"), bRestoredBeforeBroadcast);
	TestTrue(TEXT("结束回调内重新激活同一 Spec"), bReactivated);
	TestTrue(TEXT("旧结束不得清空新动作 Mesh"), Ability->HasActiveMeshForTest());
	TestTrue(TEXT("旧结束不得覆盖新动作 Tick 设置"), Mesh->VisibilityBasedAnimTickOption == EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones);
	TestFalse(TEXT("新动作仍关闭 URO"), Mesh->bEnableUpdateRateOptimizations);
	ASC->OnAbilityEnded.Remove(EndHandle);
	Ability->FinishForTest();
	TestTrue(TEXT("新动作最终恢复最初设置"), Mesh->VisibilityBasedAnimTickOption == EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered && Mesh->bEnableUpdateRateOptimizations);
	World->DestroyWorld(false);
	return true;
}
#endif
