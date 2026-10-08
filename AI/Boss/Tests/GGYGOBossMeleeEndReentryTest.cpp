#include "AI/Boss/Tests/GGYGOBossMeleeLifecycleTestAbility.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
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

bool UGGYGOBossMeleeLifecycleTestAbility::BeginMotionForTest(
	const FGGYGOAbilityActivationHandle& Original, const UGGYGOActionMotionProfile* Profile)
{
	const ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	return BeginOriginalMeleeMotion(Original,
		Character ? Cast<UGGYGOCharacterMovementComponent>(Character->GetCharacterMovement()) : nullptr, Profile, 1.0f);
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Character/GGYGOCharacterBase.h"
#include "Character/Components/GGYGOActionCurveRootMotionSource.h"
#include "Character/Data/GGYGOActionMotionProfile.h"
#include "Curves/CurveVector.h"
#include "Misc/AutomationTest.h"

namespace
{
	bool VerifyOriginalMotionFailure(FAutomationTestBase& Test)
	{
		UWorld::InitializationValues Init;
		Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(true)
			.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
		if (!Test.TestNotNull(TEXT("位移失败真实测试 World"), World)) { return false; }
		struct FCleanup { UWorld* World; ~FCleanup() { World->DestroyWorld(false); } } Cleanup{World};
		AGGYGOCharacterBase* Owner = World->SpawnActor<AGGYGOCharacterBase>();
		UGGYGOCharacterMovementComponent* Movement = Owner
			? Cast<UGGYGOCharacterMovementComponent>(Owner->GetCharacterMovement()) : nullptr;
		if (!Test.TestNotNull(TEXT("位移失败原 Character"), Owner)
			|| !Test.TestNotNull(TEXT("位移失败原角色实际 CMC"), Movement)) { return false; }
		Movement->SetComponentTickEnabled(false);
		Movement->SetMovementMode(MOVE_Walking);
		USkeletalMeshComponent* Mesh = Owner->GetMesh();
		const EVisibilityBasedAnimTickOption OriginalTick = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
		Mesh->VisibilityBasedAnimTickOption = OriginalTick;
		Mesh->bEnableUpdateRateOptimizations = true;
		UGGYGOAbilitySystemComponent* ASC = NewObject<UGGYGOAbilitySystemComponent>(Owner);
		ASC->RegisterComponent();
		ASC->InitAbilityActorInfo(Owner, Owner);
		const FGameplayAbilitySpecHandle SpecHandle = ASC->GiveAbility(
			FGameplayAbilitySpec(UGGYGOBossMeleeLifecycleTestAbility::StaticClass(), 1));
		const FGGYGOAbilityActivationRequestResult First = ASC->TryActivateAbilityWithTerminationBoundary(SpecHandle);
		if (!Test.TestTrue(TEXT("位移失败原受控激活"), First.bNativeAccepted
			&& First.Outcome == EGGYGOAbilityActivationRequestOutcome::Accepted && First.OriginalActivation.HasActivation())) { return false; }
		const FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromHandle(SpecHandle);
		UGGYGOBossMeleeLifecycleTestAbility* Ability = Spec
			? Cast<UGGYGOBossMeleeLifecycleTestAbility>(Spec->GetPrimaryInstance()) : nullptr;
		if (!Test.TestNotNull(TEXT("位移失败原 Ability 实例"), Ability)) { return false; }
		UGGYGOActionMotionProfile* Profile = NewObject<UGGYGOActionMotionProfile>(Owner);
		Profile->Duration = 1.0f;
		Profile->TranslationCurve = NewObject<UCurveVector>(Profile);
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			FRichCurve& Curve = Profile->TranslationCurve->FloatCurves[Axis];
			Curve.SetKeyInterpMode(Curve.AddKey(0.0f, 0.0f), RCIM_Linear);
			Curve.SetKeyInterpMode(Curve.AddKey(1.0f, Axis == 0 ? 100.0f : 0.0f), RCIM_Linear);
		}
		if (!Test.TestTrue(TEXT("生产入口开始并观察原 Profile motion"),
			Ability->BeginMotionForTest(First.OriginalActivation, Profile))) { return false; }
		const TSharedPtr<FRootMotionSource> Source = Movement->GetRootMotionSource(TEXT("GGYGO.ActionCurve"));
		if (!Test.TestTrue(TEXT("原 motion 是实际已安装的 Profile RMS"), Source.IsValid()
			&& Source->GetScriptStruct() == FRootMotionSource_GGYGOActionCurve::StaticStruct())) { return false; }
		const auto OriginalSource = StaticCastSharedPtr<FRootMotionSource_GGYGOActionCurve>(Source);
		if (!Test.TestTrue(TEXT("RMS 固定原 CMC/资源/句柄"), OriginalSource->OriginalResource.IsValid()
			&& OriginalSource->OriginalResource->Owner.Get() == Movement
			&& OriginalSource->OriginalResource->Handle != INDEX_NONE)) { return false; }
		// Keep the native pre-failure snapshot and its original resource identity for late preparation.
		const TSharedPtr<FRootMotionSource> LateOriginalSource(OriginalSource->Clone());
		if (!Test.TestTrue(TEXT("原生 RMS 副本保留原资源身份"), LateOriginalSource.IsValid()
			&& LateOriginalSource->GetScriptStruct() == FRootMotionSource_GGYGOActionCurve::StaticStruct()
			&& static_cast<const FRootMotionSource_GGYGOActionCurve*>(LateOriginalSource.Get())->OriginalResource
				== OriginalSource->OriginalResource)) { return false; }
		TArray<bool> NativeCancelled;
		TArray<FGGYGOAbilityTerminationCompletedNotice> Completions;
		bool bRetiredAndRestoredAtNativeEnd = false;
		const FDelegateHandle NativeEnd = ASC->OnAbilityEnded.AddLambda([&](const FAbilityEndedData& Data)
		{
			NativeCancelled.Add(Data.bWasCancelled);
			if (NativeCancelled.Num() == 1)
			{
				bRetiredAndRestoredAtNativeEnd = !Movement->HasActiveActionMotion() && !Ability->HasActiveMeshForTest()
					&& Mesh->VisibilityBasedAnimTickOption == OriginalTick && Mesh->bEnableUpdateRateOptimizations;
			}
		});
		const FDelegateHandle Completed = ASC->OnAbilityTerminationCompleted().AddLambda(
			[&](const FGGYGOAbilityTerminationCompletedNotice& Notice) { Completions.Add(Notice); });
		// Invalidate the accepted original configuration, then run native RMS preparation.
		// The test never calls an Ability failure/End hook to supply this evidence.
		Profile->Duration = 0.0f;
		const FString FailureReason = TEXT("original Profile configuration, owner or playback mapping was retired or changed");
		Test.AddExpectedMessage(FString::Printf(
			TEXT("[Movement.ActionMotion] Owner='%s' Profile='%s' Curve='%s' Reason='%s'"),
			*Owner->GetPathName(), *Profile->GetPathName(), *Profile->TranslationCurve->GetPathName(), *FailureReason),
			ELogVerbosity::Error, EAutomationExpectedMessageFlags::Exact, 1, false);
		Test.AddExpectedMessage(FString::Printf(
			TEXT("BossMelee [%s] 原动作在 [Motion] 失败：Avatar [%s] ASC [%s] Montage [None] Trace [None]；%s"),
			*Ability->GetPathName(), *GetNameSafe(Owner), *GetNameSafe(ASC), *FailureReason),
			ELogVerbosity::Error, EAutomationExpectedMessageFlags::Exact, 1, false);
		Movement->CurrentRootMotion.PrepareRootMotion(0.01f, *Owner, *Movement, true);
		bool bPassed = Test.TestTrue(TEXT("非法原 Profile 同步取消原 GA 且恢复实际资源"),
			!Ability->IsActive() && bRetiredAndRestoredAtNativeEnd);
		bPassed &= Test.TestTrue(TEXT("原 RMS 显式退役且不留下零 Override"),
			OriginalSource->bExplicitlyCancelled
			&& OriginalSource->Status.HasFlag(ERootMotionSourceStatusFlags::MarkedForRemoval)
			&& OriginalSource->AccumulateMode == ERootMotionAccumulateMode::Additive
			&& !Movement->CurrentRootMotion.HasOverrideVelocity());
		bPassed &= Test.TestTrue(TEXT("原 native End 恰一次并保留取消事实"), NativeCancelled.Num() == 1 && NativeCancelled[0]);
		bPassed &= Test.TestTrue(TEXT("原取消 Completed 恰一次且属于保存的 Original"), Completions.Num() == 1
			&& Completions[0].GetOriginal().WasCancelled()
			&& Completions[0].GetOriginal().GetOriginalActivation().HasSameActivation(First.OriginalActivation));
		if (bPassed)
		{
			Profile->Duration = 1.0f;
			const FGGYGOAbilityActivationRequestResult Next = ASC->TryActivateAbilityWithTerminationBoundary(SpecHandle);
			const bool bNextStarted = Test.TestTrue(TEXT("完整原结束返回后受控后继及 motion 启动"), Next.bNativeAccepted
				&& Next.Outcome == EGGYGOAbilityActivationRequestOutcome::Accepted
				&& Next.OriginalActivation.HasActivation()
				&& !Next.OriginalActivation.HasSameActivation(First.OriginalActivation)
				&& Ability->BeginMotionForTest(Next.OriginalActivation, Profile));
			bPassed &= bNextStarted;
			if (bNextStarted)
			{
				Test.AddExpectedMessage(FString::Printf(
					TEXT("[Movement.ActionMotion] Owner='%s' Profile='%s' Curve='%s' Reason='Profile source no longer belongs to the original active action slot'"),
					*Owner->GetPathName(), *Profile->GetPathName(), *Profile->TranslationCurve->GetPathName()),
					ELogVerbosity::Error, EAutomationExpectedMessageFlags::Exact, 1, false);
				// The native source dispatches its own late failure; CMC rejects its original retired slot.
				LateOriginalSource->PrepareRootMotion(0.01f, 0.01f, *Owner, *Movement);
				Movement->EndActionMotion(OriginalSource->OriginalResource->Handle);
				bPassed &= Test.TestTrue(TEXT("原失败重报/旧句柄清理不得终止后继或清后继资源"),
					Ability->IsActive() && Ability->HasActiveMeshForTest() && Movement->HasActiveActionMotion()
					&& Ability->GetInitializedOriginalForTest().HasSameActivation(Next.OriginalActivation)
					&& NativeCancelled.Num() == 1 && Completions.Num() == 1);
				Ability->FinishForTest(Next.OriginalActivation);
				bPassed &= Test.TestTrue(TEXT("后继正常结束恰一次并清自身 motion/Mesh"),
					NativeCancelled.Num() == 2 && !NativeCancelled[1] && Completions.Num() == 2
					&& !Completions[1].GetOriginal().WasCancelled()
					&& Completions[1].GetOriginal().GetOriginalActivation().HasSameActivation(Next.OriginalActivation)
					&& !Ability->IsActive() && !Ability->HasActiveMeshForTest() && !Movement->HasActiveActionMotion()
					&& Mesh->VisibilityBasedAnimTickOption == OriginalTick && Mesh->bEnableUpdateRateOptimizations);
			}
		}
		ASC->OnAbilityEnded.Remove(NativeEnd);
		ASC->OnAbilityTerminationCompleted().Remove(Completed);
		Test.AddInfo(FString::Printf(TEXT("BossMelee MotionFailure behavior assertions: %s; intentional Error expectations use exact messages/counts."),
			bPassed ? TEXT("PASS") : TEXT("FAIL")));
		return bPassed;
	}
}

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
	if (bPassed)
	{
		AddInfo(TEXT("BossMelee original NormalLifecycle behavior assertions: PASS."));
		bPassed = VerifyOriginalMotionFailure(*this);
	}
	return bPassed;
}
#endif
