#include "AI/Boss/Tests/GGYGOBossMeleeLifecycleTestAbility.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOBossMeleeLifecycleTestAbility)

void UGGYGOBossMeleeLifecycleTestAbility::ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
	FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	UGGYGOCombatActionAbility::ActivateAbilityBody(Original, Handle, ActorInfo, ActivationInfo, TriggerEventData);
	if (!CaptureCurrentActivation().HasSameActivation(Original)) { return; }
	ACharacter* Character = ActorInfo ? Cast<ACharacter>(ActorInfo->AvatarActor.Get()) : nullptr;
	if (!PrepareOriginalMeleeMesh(Original, Character ? Character->GetMesh() : nullptr))
	{
		RequestAbilityEnd(Original, false, true);
	}
}

void UGGYGOBossMeleeLifecycleTestAbility::FinishForTest(const FGGYGOAbilityActivationHandle& Original)
{
	RequestAbilityEnd(Original, false, false);
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
	UGGYGOAbilitySystemComponent* ASC = NewObject<UGGYGOAbilitySystemComponent>(Owner);
	ASC->RegisterComponent();
	ASC->InitAbilityActorInfo(Owner, Owner);
	USkeletalMeshComponent* Mesh = Owner->GetMesh();
	Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
	Mesh->bEnableUpdateRateOptimizations = true;
	const FGameplayAbilitySpecHandle Handle = ASC->GiveAbility(FGameplayAbilitySpec(UGGYGOBossMeleeLifecycleTestAbility::StaticClass(), 1));
	const FGGYGOAbilityActivationRequestResult FirstRequest = ASC->TryActivateAbilityWithTerminationBoundary(Handle);
	if (!TestTrue(TEXT("首个测试动作激活"), FirstRequest.Outcome == EGGYGOAbilityActivationRequestOutcome::Accepted
		&& FirstRequest.bNativeAccepted && FirstRequest.OriginalActivation.HasActivation()))
	{
		World->DestroyWorld(false);
		return false;
	}
	UGGYGOBossMeleeLifecycleTestAbility* Ability = Cast<UGGYGOBossMeleeLifecycleTestAbility>(ASC->FindAbilitySpecFromHandle(Handle)->GetPrimaryInstance());
	bool bReactivated = false;
	bool bRestoredBeforeBroadcast = false;
	FGGYGOAbilityActivationHandle ReentryOriginal;
	const FDelegateHandle EndHandle = ASC->OnAbilityEnded.AddLambda([&](const FAbilityEndedData& Data)
	{
		bRestoredBeforeBroadcast = Mesh->VisibilityBasedAnimTickOption == EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered
			&& Mesh->bEnableUpdateRateOptimizations && !Ability->HasActiveMeshForTest();
		// Retain the strict native entry from the original reproduction, including its Busy boundary.
		bReactivated = ASC->TryActivateAbility(Handle);
		ReentryOriginal = Ability->GetInitializedOriginalForTest();
	});
	Ability->FinishForTest(FirstRequest.OriginalActivation);
	AddInfo(FString::Printf(TEXT("原生 End 回调内同 Spec 原生尝试：NativeAccepted=%d InitializedOriginal=%d；旧重入成功期待保留。"),
		bReactivated ? 1 : 0, ReentryOriginal.HasActivation() ? 1 : 0));
	TestTrue(TEXT("广播前恢复 Mesh 并清引用"), bRestoredBeforeBroadcast);
	// Preserve the original expectations below. The approved Busy policy rejects this native-End
	// reentry until original Completed; these expectations remain a visible policy mismatch.
	TestTrue(TEXT("结束回调内重新激活同一 Spec"), bReactivated);
	TestTrue(TEXT("旧结束不得清空新动作 Mesh"), Ability->HasActiveMeshForTest());
	TestTrue(TEXT("旧结束不得覆盖新动作 Tick 设置"), Mesh->VisibilityBasedAnimTickOption == EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones);
	TestFalse(TEXT("新动作仍关闭 URO"), Mesh->bEnableUpdateRateOptimizations);
	ASC->OnAbilityEnded.Remove(EndHandle);
	if (bReactivated) { Ability->FinishForTest(ReentryOriginal); }
	TestTrue(TEXT("新动作最终恢复最初设置"), Mesh->VisibilityBasedAnimTickOption == EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered && Mesh->bEnableUpdateRateOptimizations);
	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOBossMeleeNormalLifecycleTest, "GGYGO.BossAI.Melee.NormalLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOBossMeleeNormalLifecycleTest::RunTest(const FString& Parameters)
{
	UWorld::InitializationValues Init;
	Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(true)
		.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	if (!TestNotNull(TEXT("普通链创建生命周期测试世界"), World)) { return false; }
	ACharacter* Owner = World->SpawnActor<ACharacter>();
	USkeletalMeshComponent* Mesh = Owner ? Owner->GetMesh() : nullptr;
	if (!TestNotNull(TEXT("普通链创建原 Owner/Avatar"), Owner)
		|| !TestNotNull(TEXT("普通链原 Mesh"), Mesh))
	{
		World->DestroyWorld(false);
		return false;
	}
	UGGYGOAbilitySystemComponent* ASC = NewObject<UGGYGOAbilitySystemComponent>(Owner);
	ASC->RegisterComponent();
	ASC->InitAbilityActorInfo(Owner, Owner);
	const EVisibilityBasedAnimTickOption OriginalTick = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
	Mesh->VisibilityBasedAnimTickOption = OriginalTick;
	Mesh->bEnableUpdateRateOptimizations = true;
	const FGameplayAbilitySpecHandle Handle = ASC->GiveAbility(FGameplayAbilitySpec(UGGYGOBossMeleeLifecycleTestAbility::StaticClass(), 1));
	if (!TestTrue(TEXT("普通链已授予原 Spec"), Handle.IsValid()))
	{
		World->DestroyWorld(false);
		return false;
	}

	struct FCompletionObservation
	{
		FGGYGOAbilityTerminationCompletedNotice Notice;
		bool bMeshRestored = false;
	};
	UGGYGOBossMeleeLifecycleTestAbility* Ability = nullptr;
	TArray<FCompletionObservation> Completions;
	const FDelegateHandle CompletionHandle = ASC->OnAbilityTerminationCompleted().AddLambda(
		[&](const FGGYGOAbilityTerminationCompletedNotice& Notice)
		{
			FCompletionObservation& Observation = Completions.AddDefaulted_GetRef();
			Observation.Notice = Notice;
			Observation.bMeshRestored = Ability && !Ability->HasActiveMeshForTest()
				&& !Ability->GetInitializedOriginalForTest().HasActivation()
				&& Mesh->VisibilityBasedAnimTickOption == OriginalTick && Mesh->bEnableUpdateRateOptimizations;
			// Observe immutable history and real resources only; never restart from this callback.
		});
	if (!TestTrue(TEXT("普通链已登记原 Completed 观察"), CompletionHandle.IsValid()))
	{
		World->DestroyWorld(false);
		return false;
	}

	bool bPassed = true;
	FGGYGOAbilityActivationHandle PreviousOriginal;
	for (int32 Index = 0; Index < 2; ++Index)
	{
		const TCHAR* Label = Index == 0 ? TEXT("A") : TEXT("B");
		// On the second iteration, A's request and every completion listener have returned.
		const FGGYGOAbilityActivationRequestResult Request = ASC->TryActivateAbilityWithTerminationBoundary(Handle);
		const bool bAccepted = TestTrue(FString::Printf(TEXT("普通链 %s 受控原生激活并取得固定 Original"), Label),
			Request.Outcome == EGGYGOAbilityActivationRequestOutcome::Accepted
			&& Request.bNativeAccepted && Request.OriginalActivation.HasActivation());
		bPassed = bAccepted && bPassed;
		if (!bAccepted) { break; }
		const FGGYGOAbilityActivationHandle Original = Request.OriginalActivation;
		const FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromHandle(Handle);
		Ability = Spec ? Cast<UGGYGOBossMeleeLifecycleTestAbility>(Spec->GetPrimaryInstance()) : nullptr;
		if (!TestNotNull(FString::Printf(TEXT("普通链 %s 原实例"), Label), Ability))
		{
			bPassed = false;
			break;
		}
		if (Index == 1)
		{
			bPassed = TestFalse(TEXT("普通链 B 使用不同于 A 的 Original"),
				Original.HasSameActivation(PreviousOriginal)) && bPassed;
		}
		bPassed = TestFalse(FString::Printf(TEXT("普通链 %s 启动没有提前完成"), Label),
			Request.OriginalTerminationCompleted.HasCompletion()) && bPassed;
		const bool bPrepared = TestTrue(FString::Printf(TEXT("普通链 %s 实际建立原 Mesh 资源"), Label),
			Ability->IsActive() && Ability->HasActiveMeshForTest()
			&& Ability->GetInitializedOriginalForTest().HasSameActivation(Original)
			&& Mesh->VisibilityBasedAnimTickOption == EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones
			&& !Mesh->bEnableUpdateRateOptimizations);
		bPassed = bPrepared && bPassed;
		if (!bPrepared)
		{
			if (Ability->IsActive()) { Ability->FinishForTest(Original); }
			break;
		}
		bPassed = TestEqual(FString::Printf(TEXT("普通链 %s 正常结束前没有额外 Completed"), Label),
			Completions.Num(), Index) && bPassed;
		Ability->FinishForTest(Original);
		const bool bCompletedOnce = TestEqual(FString::Printf(TEXT("普通链 %s 结束返回后恰有一个新 Completed"), Label),
			Completions.Num(), Index + 1);
		bPassed = bCompletedOnce && bPassed;
		if (bCompletedOnce)
		{
			const FCompletionObservation& Observation = Completions[Index];
			bPassed = TestTrue(FString::Printf(TEXT("普通链 %s Completed 唯一归属原正常结束"), Label),
				Observation.Notice.HasCompletion()
				&& Observation.Notice.GetOriginal().GetOriginalActivation().HasSameActivation(Original)
				&& !Observation.Notice.GetOriginal().WasCancelled()) && bPassed;
			bPassed = TestTrue(FString::Printf(TEXT("普通链 %s 通知时已清原资源并恢复实际 Mesh"), Label),
				Observation.bMeshRestored) && bPassed;
		}
		bPassed = TestTrue(FString::Printf(TEXT("普通链 %s 结束返回后原实例及实际 Mesh 已恢复"), Label),
			!Ability->IsActive() && !Ability->HasActiveMeshForTest()
			&& !Ability->GetInitializedOriginalForTest().HasActivation()
			&& Mesh->VisibilityBasedAnimTickOption == OriginalTick && Mesh->bEnableUpdateRateOptimizations) && bPassed;
		PreviousOriginal = Original;
		if (!bPassed) { break; }
	}
	ASC->OnAbilityTerminationCompleted().Remove(CompletionHandle);
	World->DestroyWorld(false);
	return bPassed;
}
#endif
