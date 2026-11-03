/** @file GGYGOBossMeleeEndReentryTest.cpp @brief 严格原生命周期、真实 Boss Host XYZ 及正式 Kevin Ice01 生产冒烟。 */
#include "AI/Boss/Tests/GGYGOBossMeleeLifecycleTestAbility.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/Tasks/GGYGOAbilityTask_PlayMontageAndWaitForEvent.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameplayEffect.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOBossMeleeLifecycleTestAbility)

void UGGYGOBossMeleeLifecycleTestAbility::ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
	FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	if (AttackMontage)
	{
		Super::ActivateAbilityBody(Original, Handle, ActorInfo, ActivationInfo, TriggerEventData);
		return;
	}
	UGGYGOCombatActionAbility::ActivateAbilityBody(Original, Handle, ActorInfo, ActivationInfo, TriggerEventData);
	if (!CaptureCurrentActivation().HasSameActivation(Original)) { return; }
	ACharacter* Character = ActorInfo ? Cast<ACharacter>(ActorInfo->AvatarActor.Get()) : nullptr;
	if (!PrepareOriginalMeleeMesh(Original, Character ? Character->GetMesh() : nullptr))
	{
		RequestAbilityEnd(Original, false, true);
	}
}

void UGGYGOBossMeleeLifecycleTestAbility::CleanupAbilityResourcesForTermination(const FGGYGOAbilityTerminationContext& Context)
{
	LastTermination = Context;
	Super::CleanupAbilityResourcesForTermination(Context);
}

namespace GGYGOBossMeleeTestLifecycle
{
	/** 原生 Try 的返回值与 Try 内原生 PreActivate 签发的激活身份（Try 内同步结束时仍保留）。 */
	struct FActivation
	{
		bool bNativeAccepted = false;
		FGGYGOAbilityActivationHandle OriginalActivation;
	};

	FActivation Activate(UGGYGOAbilitySystemComponent* ASC, FGameplayAbilitySpecHandle Handle)
	{
		FActivation Result;
		const FDelegateHandle Observer = ASC->AbilityActivatedCallbacks.AddLambda([&Result, Handle](UGameplayAbility* Ability)
		{
			const UGGYGOGameplayAbility* ProjectAbility = Cast<UGGYGOGameplayAbility>(Ability);
			if (ProjectAbility && ProjectAbility->GetCurrentAbilitySpecHandle() == Handle && !Result.OriginalActivation.HasActivation())
			{
				Result.OriginalActivation = ProjectAbility->CaptureCurrentActivation();
			}
		});
		Result.bNativeAccepted = ASC->TryActivateAbility(Handle);
		ASC->AbilityActivatedCallbacks.Remove(Observer);
		return Result;
	}

	/** 原生 OnAbilityEnded 时读取的那次结束清理参数。 */
	struct FEnded
	{
		FGGYGOAbilityTerminationContext Original;
		const FGGYGOAbilityTerminationContext& GetOriginal() const { return Original; }
		bool HasCompletion() const { return Original.GetOriginalActivation().HasActivation(); }
	};

	FDelegateHandle ObserveEnded(UGGYGOAbilitySystemComponent* ASC, TFunction<void(const FEnded&)> Sink)
	{
		return ASC->OnAbilityEnded.AddLambda([Sink = MoveTemp(Sink)](const FAbilityEndedData& Data)
		{
			const UGGYGOBossMeleeLifecycleTestAbility* Ability = Cast<UGGYGOBossMeleeLifecycleTestAbility>(Data.AbilityThatEnded.Get());
			FEnded Ended;
			if (Ability) { Ended.Original = Ability->GetLastTerminationForTest(); }
			Sink(Ended);
		});
	}
}

void UGGYGOBossMeleeLifecycleTestAbility::ConfigureMontageForTest(UAnimMontage* Montage, FName StartBone, FName EndBone)
{
	AttackMontage = Montage;
	bUseMontageSourceMotion = true;
	ActionMotionProfile = nullptr;
	TraceStartSocket = StartBone;
	TraceEndSocket = EndBone;
	DamageEffect = UGameplayEffect::StaticClass(); // No hit is fabricated by this playback/resource fixture.
	HitCueTag = FGameplayTag();
}

void UGGYGOBossMeleeLifecycleTestAbility::FinishForTest(const FGGYGOAbilityActivationHandle& Original)
{
	RequestAbilityEnd(Original, false, false);
}

FGGYGOAbilityActivationHandle UGGYGOBossMeleeLifecycleTestAbility::DeferredEndAfterReactivationForTest(
	const FGGYGOAbilityActivationHandle& Original, bool& bAcceptedWhileLocked)
{
	UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo();
	const FGameplayAbilitySpecHandle Handle = CurrentSpecHandle;
	FGGYGOAbilityActivationHandle Successor;
	bAcceptedWhileLocked = false;
	if (!ASC || !Handle.IsValid()) { return Successor; }
	IncrementListLock();
	// 先排队的回调在解锁时结束原激活并立即重激活同一 Spec；其后才执行的原延后请求不得结束后继。
	WaitingToExecute.Add(FPostLockDelegate::CreateLambda([this, ASC, Handle, Original, &Successor]()
	{
		RequestAbilityEnd(Original, false, false);
		if (ASC->TryActivateAbility(Handle)) { Successor = CaptureCurrentActivation(); }
	}));
	bAcceptedWhileLocked = RequestAbilityEnd(Original, false, false).IsAccepted() && IsActive();
	DecrementListLock();
	return Successor;
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
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/Notifies/GGYGOAnimNotifyState_GameplayEventWindow.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "AI/Boss/Tests/GGYGOBossEncounterLifecycleTestTypes.h"
#include "AI/Boss/GGYGOBossAIController.h"
#include "AI/Boss/GGYGOBossState.h"
#include "Combat/HitDetection/GGYGOMeleeTraceComponent.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Character/Data/GGYGOMovementSet.h"
#include "Character/Data/GGYGOPawnData.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "System/GGYGOGameplayTags.h"
#include "Misc/AutomationTest.h"
#if WITH_EDITOR
#include "AbilitySystem/GGYGOAbilitySet.h"
#include "AbilitySystem/Attributes/GGYGOHealthSet.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Components/CapsuleComponent.h"
#include "Components/SphereComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerStart.h"
#include "EngineUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PlayInEditorDataTypes.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "UObject/Package.h"
#endif

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
		const GGYGOBossMeleeTestLifecycle::FActivation First = GGYGOBossMeleeTestLifecycle::Activate(ASC, SpecHandle);
		if (!Test.TestTrue(TEXT("位移失败原受控激活"), First.bNativeAccepted
			&& First.OriginalActivation.HasActivation())) { return false; }
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
		TArray<GGYGOBossMeleeTestLifecycle::FEnded> Completions;
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
		const FDelegateHandle Completed = GGYGOBossMeleeTestLifecycle::ObserveEnded(ASC, [&](const GGYGOBossMeleeTestLifecycle::FEnded& Notice) { Completions.Add(Notice); });
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
			const GGYGOBossMeleeTestLifecycle::FActivation Next = GGYGOBossMeleeTestLifecycle::Activate(ASC, SpecHandle);
			const bool bNextStarted = Test.TestTrue(TEXT("完整原结束返回后受控后继及 motion 启动"), Next.bNativeAccepted
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
		ASC->OnAbilityEnded.Remove(Completed);
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
	const GGYGOBossMeleeTestLifecycle::FActivation FirstRequest = GGYGOBossMeleeTestLifecycle::Activate(ASC, Handle);
	if (!TestTrue(TEXT("首个测试动作激活"), FirstRequest.bNativeAccepted && FirstRequest.OriginalActivation.HasActivation()))
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
		// 保留真实 OnAbilityEnded 内的原生重入入口，核旧 End 栈不会破坏后继资源。
		bReactivated = ASC->TryActivateAbility(Handle);
		ReentryOriginal = Ability->GetInitializedOriginalForTest();
	});
	Ability->FinishForTest(FirstRequest.OriginalActivation);
	AddInfo(FString::Printf(TEXT("原生 End 回调内同 Spec 原生尝试：NativeAccepted=%d InitializedOriginal=%d；旧重入成功期待保留。"),
		bReactivated ? 1 : 0, ReentryOriginal.HasActivation() ? 1 : 0));
	TestTrue(TEXT("广播前恢复 Mesh 并清引用"), bRestoredBeforeBroadcast);
	// 旧 proof 的完整返回义务不阻止原生退役后新 proof 准入；旧结束只能收口自身资源。
	TestTrue(TEXT("结束回调内重新激活同一 Spec"), bReactivated);
	TestTrue(TEXT("旧结束不得清空新动作 Mesh"), Ability->HasActiveMeshForTest());
	TestTrue(TEXT("旧结束不得覆盖新动作 Tick 设置"), Mesh->VisibilityBasedAnimTickOption == EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones);
	TestFalse(TEXT("新动作仍关闭 URO"), Mesh->bEnableUpdateRateOptimizations);
	ASC->OnAbilityEnded.Remove(EndHandle);
	if (bReactivated)
	{
		int32 SuccessorEnds = 0;
		const FDelegateHandle SuccessorEnded = ASC->OnAbilityEnded.AddLambda(
			[&SuccessorEnds, Ability](const FAbilityEndedData& Data)
			{
				if (Data.AbilityThatEnded.Get() == Ability) { ++SuccessorEnds; }
			});
		// 结束广播内重新激活的后继拥有自己的激活身份；用它结束只结束后继一次。
		TestTrue(TEXT("后继拥有新的激活身份"), ReentryOriginal.HasActivation()
			&& !ReentryOriginal.HasSameActivation(FirstRequest.OriginalActivation));
		Ability->FinishForTest(ReentryOriginal);
		ASC->OnAbilityEnded.Remove(SuccessorEnded);
		TestEqual(TEXT("后继正常结束恰一次"), SuccessorEnds, 1);
	}
	{
		const GGYGOBossMeleeTestLifecycle::FActivation Deferred = GGYGOBossMeleeTestLifecycle::Activate(ASC, Handle);
		bool bAcceptedWhileLocked = false;
		const FGGYGOAbilityActivationHandle Successor = Deferred.bNativeAccepted
			? Ability->DeferredEndAfterReactivationForTest(Deferred.OriginalActivation, bAcceptedWhileLocked)
			: FGGYGOAbilityActivationHandle{};
		TestTrue(TEXT("ScopeLock 内对原激活的结束请求被接受并延后"), bAcceptedWhileLocked);
		TestTrue(TEXT("解锁后先结束原激活并重激活出新身份"), Successor.HasActivation()
			&& !Successor.HasSameActivation(Deferred.OriginalActivation));
		TestTrue(TEXT("其后执行的原延后结束请求不结束后继"), Ability->IsActive()
			&& Ability->CaptureCurrentActivation().HasSameActivation(Successor));
		Ability->FinishForTest(Successor);
	}
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
		GGYGOBossMeleeTestLifecycle::FEnded Notice;
		bool bMeshRestored = false;
	};
	UGGYGOBossMeleeLifecycleTestAbility* Ability = nullptr;
	TArray<FCompletionObservation> Completions;
	const FDelegateHandle CompletionHandle = GGYGOBossMeleeTestLifecycle::ObserveEnded(ASC, [&](const GGYGOBossMeleeTestLifecycle::FEnded& Notice)
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
		const GGYGOBossMeleeTestLifecycle::FActivation Request = GGYGOBossMeleeTestLifecycle::Activate(ASC, Handle);
		const bool bAccepted = TestTrue(FString::Printf(TEXT("普通链 %s 受控原生激活并取得固定 Original"), Label),
			Request.bNativeAccepted && Request.OriginalActivation.HasActivation());
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
			Completions.Num() > Index) && bPassed;
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
	ASC->OnAbilityEnded.Remove(CompletionHandle);
	World->DestroyWorld(false);
	if (bPassed)
	{
		AddInfo(TEXT("BossMelee original NormalLifecycle behavior assertions: PASS."));
		bPassed = VerifyOriginalMotionFailure(*this);
	}
	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOBossMeleeOriginalMontageTest, "GGYGO.BossAI.Melee.OriginalMontageXYZ",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOBossMeleeOriginalMontageTest::RunTest(const FString& Parameters)
{
#if WITH_EDITOR
	UWorld::InitializationValues Init;
	Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(true)
		.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	if (!TestNotNull(TEXT("XYZ consumer real World"), World)) { return false; }
	if (!TestNotNull(TEXT("XYZ native Engine"), GEngine)) { World->DestroyWorld(false); return false; }
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	struct FBossXYZWorldCleanup
	{
		UWorld* World;
		AGGYGOEncounterLifecycleTestEncounter* Encounter = nullptr;
		~FBossXYZWorldCleanup()
		{
			if (IsValid(Encounter)) { Encounter->CleanupForTest(); }
			World->DestroyWorld(false);
			GEngine->DestroyWorldContext(World);
		}
	} Cleanup{World};
	World->InitializeActorsForPlay(FURL());
	UGGYGOMovementSet* MovementSet = NewObject<UGGYGOMovementSet>(World);
	MovementSet->bUseCurveDrivenSpeed = false; // Explicit isolated locomotion fixture; action must still supply XYZ.
	UGGYGOPawnData* PawnData = NewObject<UGGYGOPawnData>(World);
	PawnData->PawnClass = AGGYGOBossCharacter::StaticClass();
	PawnData->MovementSet = MovementSet;
	UGGYGOBossDefinition* Definition = NewObject<UGGYGOBossDefinition>(World);
	Definition->InitialFormTag = GGYGOGameplayTags::State_Boss_Form_Default;
	Definition->InitialPhaseTag = GGYGOGameplayTags::State_Boss_Phase_One;
	FGGYGOBossFormDefinition& Form = Definition->Forms.AddDefaulted_GetRef();
	Form.FormTag = Definition->InitialFormTag;
	Form.AvatarPawnData = PawnData;
	FGGYGOBossPhaseDefinition& Phase = Definition->Phases.AddDefaulted_GetRef();
	Phase.PhaseTag = Definition->InitialPhaseTag;
	AGGYGOEncounterLifecycleTestEncounter* Encounter = World->SpawnActor<AGGYGOEncounterLifecycleTestEncounter>();
	if (!TestNotNull(TEXT("XYZ actual Encounter"), Encounter)) { return false; }
	Cleanup.Encounter = Encounter;
	Encounter->ConfigureForTest(Definition);
	if (!TestTrue(TEXT("XYZ production assembly-only SpawnBoss"), Encounter->SpawnBoss())) { return false; }
	AGGYGOBossCharacter* Character = Encounter->GetBossAvatar();
	AGGYGOBossState* State = Encounter->GetBossState();
	UGGYGOAbilitySystemComponent* ASC = State ? State->GetGGYGOAbilitySystemComponent() : nullptr;
	UGGYGOCharacterMovementComponent* Movement = Character ? Character->GetGGYGOMovementComponent() : nullptr;
	USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
	if (!TestNotNull(TEXT("XYZ original Character"), Character) || !TestNotNull(TEXT("XYZ original CMC"), Movement)
		|| !TestNotNull(TEXT("XYZ original Mesh"), Mesh)) { return false; }
	AGGYGOBossAIController* Controller = Encounter->GetBossController();
	UGGYGOPawnExtensionComponent* Extension = Character->GetPawnExtensionComponent();
	if (!TestNotNull(TEXT("XYZ original external Host"), State)
		|| !TestNotNull(TEXT("XYZ original Host ASC"), ASC)
		|| !TestNotNull(TEXT("XYZ original Controller"), Controller)
		|| !TestNotNull(TEXT("XYZ original PawnExtension"), Extension)) { return false; }
	const FGGYGOPawnASCResourceHandle InitialResource = Extension->GetCurrentLocalAbilitySystemResource();
	if (!TestTrue(TEXT("XYZ initial production assembly has real published Host Ready"),
		Character->GetGGYGOAbilitySystemComponent() == ASC && State->GetAvatarPawn() == Character
		&& State->GetGGYGOAbilitySystemComponent() == ASC && ASC->AbilityActorInfo.IsValid()
		&& ASC->AbilityActorInfo->OwnerActor.Get() == State && ASC->AbilityActorInfo->AvatarActor.Get() == Character
		&& InitialResource.HasResource() && InitialResource.GetIdentity().ASC.Get() == ASC
		&& Extension->IsLocalAbilitySystemResourceReady(InitialResource)
		&& Character->GetController() == Controller && Controller->GetPawn() == Character
		&& !Character->FindComponentByClass<UGGYGOAbilitySystemComponent>())) { return false; }
	// Mesh/AnimInstance are part of the committed native binding source. Release its
	// real Host resource before configuring the fixture, then publish a new binding.
	State->DetachAvatar(Character);
	if (!TestTrue(TEXT("XYZ real Host Release retires the original resource before source configuration"),
		State->GetGGYGOAbilitySystemComponent() == ASC && !State->GetAvatarPawn()
		&& ASC->GetOwnerActor() == State && !ASC->GetAvatarActor()
		&& !Character->GetGGYGOAbilitySystemComponent()
		&& !Extension->GetCurrentLocalAbilitySystemResource().HasResource()
		&& !Extension->IsLocalAbilitySystemResourceReady(InitialResource)
		&& Character->GetController() == Controller && Controller->GetPawn() == Character)) { return false; }
	USkeletalMesh* Cube = LoadObject<USkeletalMesh>(nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
	USkeleton* Skeleton = Cube && Cube->GetSkeleton() ? DuplicateObject<USkeleton>(Cube->GetSkeleton(), Character) : nullptr;
	if (!TestNotNull(TEXT("XYZ real engine Mesh"), Cube) || !TestNotNull(TEXT("XYZ owned Skeleton"), Skeleton)
		|| !TestTrue(TEXT("XYZ real Mesh compatible bones"), Skeleton->IsCompatibleMesh(Cube) && Cube->GetRefSkeleton().GetNum() >= 2)) { return false; }
	Skeleton->RegisterSlotNode(TEXT("DefaultSlot"));
	Mesh->USkinnedMeshComponent::SetSkinnedAssetAndUpdate(Cube);
	Mesh->SetAnimInstanceClass(UGGYGOBossMeleeLifecycleTestAnimInstance::StaticClass());
	Mesh->SetRelativeRotation(FRotator::ZeroRotator);
	Mesh->SetRelativeScale3D(FVector::OneVector);
	Mesh->SetComponentTickEnabled(false);
	Mesh->SetVisibility(false);
	Mesh->SetCastShadow(false);
	Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
	Mesh->bEnableUpdateRateOptimizations = true;
	UAnimInstance* Anim = Mesh->GetAnimInstance();
	if (!TestNotNull(TEXT("XYZ concrete Guard instance"), Anim)) { return false; }
	UGGYGOBossMeleeLifecycleTestAnimInstance* Guard = Cast<UGGYGOBossMeleeLifecycleTestAnimInstance>(Anim);
	if (!TestNotNull(TEXT("XYZ original concrete Guard receiver"), Guard)) { return false; }
	State->AttachAvatar(Character);
	const FGGYGOPawnASCResourceHandle ReboundResource = Extension->GetCurrentLocalAbilitySystemResource();
	if (!TestTrue(TEXT("XYZ same Host publishes a new real binding to the configured native source"),
		State->GetGGYGOAbilitySystemComponent() == ASC && State->GetAvatarPawn() == Character
		&& Character->GetGGYGOAbilitySystemComponent() == ASC
		&& ReboundResource.HasResource() && !ReboundResource.HasSameResource(InitialResource)
		&& Extension->IsLocalAbilitySystemResourceReady(ReboundResource)
		&& !Extension->IsLocalAbilitySystemResourceReady(InitialResource)
		&& ASC->AbilityActorInfo.IsValid() && ASC->AbilityActorInfo->OwnerActor.Get() == State
		&& ASC->AbilityActorInfo->AvatarActor.Get() == Character
		&& ASC->AbilityActorInfo->SkeletalMeshComponent.Get() == Mesh && ASC->AbilityActorInfo->GetAnimInstance() == Guard
		&& Character->GetController() == Controller && Controller->GetPawn() == Character)) { return false; }
	Movement->SetComponentTickEnabled(false);
	Movement->bRunPhysicsWithNoController = true;
	if (!TestTrue(TEXT("XYZ fixture valid MovementSet"), Movement->SetMovementSet(MovementSet))) { return false; }
	Character->SetActorLocation(FVector(0.0, 0.0, 1000.0));
	Movement->SetMovementMode(MOVE_Falling);
	UGGYGOMeleeTraceComponent* Trace = Character->FindComponentByClass<UGGYGOMeleeTraceComponent>();
	if (!TestNotNull(TEXT("XYZ Boss original Trace component"), Trace)) { return false; }
	Trace->SetComponentTickEnabled(false);
	if (!TestTrue(TEXT("XYZ real external Host owns the ready original Avatar ASC"), ASC && State
		&& Character->GetGGYGOAbilitySystemComponent() == ASC && State->GetAvatarPawn() == Character
		&& ASC->AbilityActorInfo.IsValid() && ASC->AbilityActorInfo->OwnerActor.Get() == State
		&& ASC->AbilityActorInfo->AvatarActor.Get() == Character)) { return false; }
	UAnimSequence* Sequence = NewObject<UAnimSequence>(Character);
	Sequence->SetSkeleton(Skeleton);
	IAnimationDataController& Data = Sequence->GetController();
	Data.InitializeModel();
	{
		IAnimationDataController::FScopedBracket Bracket(Data, FText::FromString(TEXT("Boss original XYZ fixture")), false);
		Data.SetFrameRate(FFrameRate(32, 1), false);
		Data.SetNumberOfFrames(FFrameNumber(32), false);
		const FName Names[] = {TEXT("RootMotion_PosX"), TEXT("RootMotion_PosY"), TEXT("RootMotion_PosZ")};
		const float Ends[] = {100.0f, 30.0f, 50.0f};
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const FAnimationCurveIdentifier Id(Names[Axis], ERawCurveTrackTypes::RCT_Float);
			FRichCurveKey First(0.0f, 0.0f), Last(1.0f, Ends[Axis]);
			First.InterpMode = RCIM_Linear;
			if (!TestTrue(TEXT("XYZ authored cumulative axis installed"), Data.AddCurve(Id, 4, false)
				&& Data.SetCurveKeys(Id, {First, Last}, false))) { return false; }
		}
		Data.NotifyPopulated();
	}
	UAnimMontage* Montage = NewObject<UAnimMontage>(Character);
	Montage->SetSkeleton(Skeleton);
	Montage->SetCompositeLength(1.0f);
	Montage->BlendIn.SetBlendTime(0.0f);
	Montage->BlendOut.SetBlendTime(0.125f);
	Montage->BlendOutTriggerTime = 0.125f;
	Montage->SlotAnimTracks.SetNum(1);
	Montage->SlotAnimTracks[0].SlotName = TEXT("DefaultSlot");
	FAnimSegment Segment;
	Segment.SetAnimReference(Sequence, true);
	Montage->SlotAnimTracks[0].AnimTrack.AnimSegments.Add(Segment);
	if (!TestTrue(TEXT("XYZ one complete Main section"), Montage->AddAnimCompositeSection(TEXT("Main"), 0.0f) != INDEX_NONE)) { return false; }
	UGGYGOAnimNotifyState_GameplayEventWindow* HitWindow = NewObject<UGGYGOAnimNotifyState_GameplayEventWindow>(Montage);
	HitWindow->NotifyStateBehaviorFlags |= static_cast<uint8>(EAnimNotifyStateBehaviorFlags::NoMergeOnConcurrentPlay);
	HitWindow->InitializeEventTags(GGYGOGameplayTags::Event_Montage_HitWindowBegin,
		GGYGOGameplayTags::Event_Montage_HitWindowEnd);
	FAnimNotifyEvent& WindowEvent = Montage->Notifies.AddDefaulted_GetRef();
	WindowEvent.NotifyStateClass = HitWindow;
	WindowEvent.MontageTickType = EMontageNotifyTickType::Queued;
	WindowEvent.Link(Montage, 0.125f);
	WindowEvent.SetDuration(0.5f);
	WindowEvent.EndLink.Link(Montage, 0.625f);
	const FGameplayAbilitySpecHandle SpecHandle = ASC->GiveAbility(FGameplayAbilitySpec(UGGYGOBossMeleeLifecycleTestAbility::StaticClass(), 1));
	const FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromHandle(SpecHandle);
	UGGYGOBossMeleeLifecycleTestAbility* Ability = Spec ? Cast<UGGYGOBossMeleeLifecycleTestAbility>(Spec->GetPrimaryInstance()) : nullptr;
	if (!TestNotNull(TEXT("XYZ actual GA instance"), Ability)) { return false; }
	Ability->ConfigureMontageForTest(Montage, Cube->GetRefSkeleton().GetBoneName(0), Cube->GetRefSkeleton().GetBoneName(1));
	FString Error;
	Ability->SetProfileForTest(NewObject<UGGYGOActionMotionProfile>(Character));
	bool bPassed = TestFalse(TEXT("XYZ and Profile are mutually exclusive"), Ability->ValidateMeleeConfiguration(Error));
	bPassed &= TestTrue(TEXT("Mutual exclusion has explicit diagnostic"), Error.Contains(TEXT("互斥")));
	Ability->SetProfileForTest(nullptr);
	if (!TestTrue(TEXT("XYZ complete configuration valid"), Ability->ValidateMeleeConfiguration(Error))) { return false; }
	TArray<GGYGOBossMeleeTestLifecycle::FEnded> Completed;
	const FDelegateHandle Completion = GGYGOBossMeleeTestLifecycle::ObserveEnded(ASC, [&](const GGYGOBossMeleeTestLifecycle::FEnded& Notice) { Completed.Add(Notice); });
	const FVector Entry = Character->GetActorLocation();
	const GGYGOBossMeleeTestLifecycle::FActivation Request = GGYGOBossMeleeTestLifecycle::Activate(ASC, SpecHandle);
	if (!TestTrue(TEXT("XYZ production GA starts original native Task and CMC"), Request.bNativeAccepted
		&& Request.OriginalActivation.HasActivation() && Ability->IsActive() && Movement->HasActiveActionMotion()))
	{
		ASC->OnAbilityEnded.Remove(Completion);
		return false;
	}
	FAnimMontageInstance* Instance = Anim->GetActiveInstanceForMontage(Montage);
	if (!TestNotNull(TEXT("XYZ original native instance"), Instance)) { ASC->OnAbilityEnded.Remove(Completion); return false; }
	const int32 OriginalInstanceId = Instance->GetInstanceID();
	const TSharedPtr<FRootMotionSource> OriginalSource = Movement->GetRootMotionSource(TEXT("GGYGO.ActionCurve"));
	if (!TestTrue(TEXT("XYZ activation owns its original native curve source"), OriginalSource.IsValid()
		&& OriginalSource->GetScriptStruct() == FRootMotionSource_GGYGOActionCurve::StaticStruct()))
	{
		ASC->OnAbilityEnded.Remove(Completion);
		return false;
	}
	const auto& OriginalCurveSource = static_cast<const FRootMotionSource_GGYGOActionCurve&>(*OriginalSource);
	bool bSawPlaybackEndBeforeFinalMove = false;
	for (int32 Step = 0; Step < 8; ++Step)
	{
		if (FAnimMontageInstance* OriginalInstance = Anim->GetMontageInstanceForID(OriginalInstanceId))
		{
			OriginalInstance->UpdateWeight(0.125f);
			OriginalInstance->Advance(0.125f, nullptr, false);
		}
		Anim->DispatchQueuedAnimEvents();
		// Task Completed retires the active handle; the same finite native RMS retains
		// its unconsumed tail. Observe that original source rather than active admission.
		if (Ability->GetActiveTaskCountForTest() == 0 && OriginalCurveSource.GetTime() < OriginalCurveSource.Duration)
		{
			bSawPlaybackEndBeforeFinalMove = TestTrue(TEXT("Native playback end retains original GA until CMC final contribution"),
				Ability->IsActive() && Completed.IsEmpty() && !Movement->HasActiveActionMotion()
				&& Movement->GetRootMotionSourceByID(OriginalSource->LocalID) == OriginalSource
				&& OriginalCurveSource.bCompletionRequested && !OriginalCurveSource.bExplicitlyCancelled
				&& OriginalCurveSource.SourceMode == EGGYGOActionCurveSourceMode::OriginalMontage
				&& OriginalCurveSource.OriginalBinding.IsValid() && OriginalCurveSource.OriginalBinding->Montage.Get() == Montage
				&& (Character->GetActorLocation() - Entry).Equals(FVector(100.0, 30.0, 50.0) * OriginalCurveSource.GetTime(), 0.1));
			bPassed &= bSawPlaybackEndBeforeFinalMove;
		}
		Movement->MoveAutonomous(0.0f, 0.125f, 0, FVector::ZeroVector);
	}
	bPassed &= TestTrue(TEXT("Real capsule consumes full authored XYZ once"), (Character->GetActorLocation() - Entry).Equals(FVector(100.0, 30.0, 50.0), 0.1));
	bPassed &= TestTrue(TEXT("Original Task ended before its final native move was consumed"), bSawPlaybackEndBeforeFinalMove);
	bPassed &= TestTrue(TEXT("The same original RMS final interval was actually consumed by native movement"),
		OriginalCurveSource.bPreparedNaturalEnd && OriginalCurveSource.bPreparedContributionConsumed
		&& OriginalCurveSource.GetTime() >= OriginalCurveSource.Duration && !OriginalCurveSource.bExplicitlyCancelled);
	bPassed &= TestTrue(TEXT("XYZ normal End occurs once after both original facts"), Completed.Num() == 1
		&& !Completed[0].GetOriginal().WasCancelled() && !Ability->IsActive() && !Movement->HasActiveActionMotion()
		&& !Ability->HasActiveMeshForTest() && Mesh->bEnableUpdateRateOptimizations
		&& Mesh->VisibilityBasedAnimTickOption == EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered);
	if (bPassed)
	{
		Movement->SetMovementMode(MOVE_Falling);
		const GGYGOBossMeleeTestLifecycle::FActivation Cancelled = GGYGOBossMeleeTestLifecycle::Activate(ASC, SpecHandle);
		bPassed &= TestTrue(TEXT("XYZ next real activation starts"), Cancelled.bNativeAccepted && Ability->IsActive() && Movement->HasActiveActionMotion());
		Ability->RequestAbilityCancel(Cancelled.OriginalActivation, true);
		bPassed &= TestTrue(TEXT("XYZ cancellation releases only its original resources"), Completed.Num() == 2
			&& Completed[1].GetOriginal().WasCancelled() && !Movement->HasActiveActionMotion() && !Ability->HasActiveMeshForTest());
	}
	if (bPassed)
	{
		// Required-role mode without its compiled producer fails in the actual pre-Ready
		// Task. Its N0 failure arrives after EndTask, before a CMC resource can be issued.
		Guard->RequiredPoseCorrectionSlot = TEXT("DefaultSlot");
		AddExpectedMessage(TEXT("[AbilitySystem.MontageTask.Pose]"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, 1, false);
		AddExpectedMessage(TEXT("原动作在 [Montage Task] 失败"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, 1, false);
		const GGYGOBossMeleeTestLifecycle::FActivation StartupFailed = GGYGOBossMeleeTestLifecycle::Activate(ASC, SpecHandle);
		bPassed &= TestTrue(TEXT("Original required Task startup failure ends once without issuing CMC motion"),
			StartupFailed.bNativeAccepted && StartupFailed.OriginalActivation.HasActivation() && Completed.Num() == 3
			&& Completed[2].GetOriginal().WasCancelled()
			&& Completed[2].GetOriginal().GetOriginalActivation().HasSameActivation(StartupFailed.OriginalActivation)
			&& !Ability->IsActive() && Ability->GetActiveTaskCountForTest() == 0
			&& !Movement->HasActiveActionMotion() && !Ability->HasActiveMeshForTest()
			&& Mesh->bEnableUpdateRateOptimizations
			&& Mesh->VisibilityBasedAnimTickOption == EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered);
		Guard->RequiredPoseCorrectionSlot = NAME_None;
	}
	if (bPassed)
	{
		Movement->SetMovementMode(MOVE_Falling);
		const GGYGOBossMeleeTestLifecycle::FActivation Failed = GGYGOBossMeleeTestLifecycle::Activate(ASC, SpecHandle);
		bPassed &= TestTrue(TEXT("XYZ failure probe starts through real GA"), Failed.bNativeAccepted && Ability->IsActive());
		const FAnimationCurveIdentifier ZId(TEXT("RootMotion_PosZ"), ERawCurveTrackTypes::RCT_Float);
		bPassed &= TestTrue(TEXT("Invalidate accepted original required Z source"), Data.RemoveCurve(ZId, false));
		AddExpectedMessage(TEXT("RootMotion_PosZ"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, 2, false);
		Movement->CurrentRootMotion.PrepareRootMotion(0.01f, *Character, *Movement, true);
		bPassed &= TestTrue(TEXT("CMC source failure ends original GA as cancelled and restores Mesh"), Completed.Num() == 4
			&& Completed[3].GetOriginal().WasCancelled()
			&& Completed[3].GetOriginal().GetOriginalActivation().HasSameActivation(Failed.OriginalActivation)
			&& !Ability->IsActive() && !Movement->HasActiveActionMotion() && !Ability->HasActiveMeshForTest()
			&& Mesh->bEnableUpdateRateOptimizations);
		bPassed &= TestFalse(TEXT("Missing Z rejects configuration rather than no-motion success"), Ability->ValidateMeleeConfiguration(Error));
	}
	ASC->OnAbilityEnded.Remove(Completion);
	return bPassed;
#else
	AddError(TEXT("Original Montage fixture requires editor animation data model."));
	return false;
#endif
}

#if WITH_EDITOR
namespace
{
	/** 正式资产的有限观察叶；自有原生 PIE 的动画／CMC／查询帧均由 UE 驱动。 */
	class FKevinIce01ProductionCommand : public IAutomationLatentCommand
	{
	public:
		explicit FKevinIce01ProductionCommand(FAutomationTestBase& InTest) : Test(InTest) {}
		virtual ~FKevinIce01ProductionCommand() override
		{
			Cleanup();
			FEditorDelegates::PostPIEStarted.Remove(PIEStartedObserver);
			// 自动化被中止时也只能收尾本次请求，不能接管原用户或替换会话。
			if (OwnsPIE() && CountPIEWorlds() == 1) GEditor->EndPlayMap();
			CancelOwnQueuedPIE();
		}

		virtual bool Update() override
		{
			if (bEnding) return UpdateEnding();
			if (!bStarted)
			{
				bStarted = true;
				if (!RequestHost()) { Cleanup(); return true; }
				return false;
			}
			if (bFailed) return BeginEnding();
			if (!bAssetsStarted)
			{
				if (!bPIEStarted)
				{
					if (FPlatformTime::Seconds() < HostDeadline) return false;
					Check(false, TEXT("自有 PIE 启动超时")); return BeginEnding();
				}
				bAssetsStarted = true;
				if (!Start()) return BeginEnding();
			}
			const FWorldContext* Context = GEditor && World.IsValid() ? GEditor->GetWorldContextFromWorld(World.Get()) : nullptr;
			if (!Check(OwnsPIE() && CountPIEWorlds() == 1 && Context && GEditor->PlayWorld == World.Get() && Context->ContextHandle == WorldContext
				&& !GEditor->ShouldEndPlayMap() && Source.IsValid() && Target.IsValid() && ASC.IsValid()
				&& TargetASC.IsValid() && Movement.IsValid() && Mesh.IsValid() && Anim.IsValid() && Trace.IsValid() && Ability.IsValid()
				&& CurrentResource(Source.Get()).HasSameResource(SourceBinding) && CurrentResource(Target.Get()).HasSameResource(TargetBinding)
				&& ASC->GetAvatarActor() == Source.Get() && TargetASC->GetAvatarActor() == Target.Get()
				&& Mesh->GetAnimInstance() == Anim.Get(),
				TEXT("原 PIE 或正式动作接收者已失效／被替换"))) return BeginEnding();
			if (!Check(FPlatformTime::Seconds() < Deadline, TEXT("正式 Ice01 未在有限期限内结束"))) return BeginEnding();
			if (Trace->IsTracing())
			{
				++OpenSamples;
				if (Probe.IsValid() && FMath::PointDistToSegment(Probe->GetComponentLocation(), Mesh->GetSocketLocation(TraceStart),
					Mesh->GetSocketLocation(TraceEnd)) <= Probe->GetScaledSphereRadius()) ++ContactSamples;
			}
			bSawCapsuleMove |= (Source->GetActorLocation() - Entry).SizeSquared() > 1.0;
			if (CompletionCount == 0) { return false; }
			// 结束通知捕获最后胶囊提交；再观察原生 RMS 从实际组件退役，避免尾部惯性冒充曲线位移。
			if (Movement->GetRootMotionSource(TEXT("GGYGO.ActionCurve")).IsValid()) { return false; }
			bool bPassed = Check(CompletionCount == 1 && !bCancelled && !Ability->IsActive()
				&& !Ability->CaptureCurrentActivation().HasActivation(), TEXT("原 GA 未自然且唯一结束"));
			bPassed &= Check(CurveSource.IsValid() && CurveSource->bPreparedNaturalEnd
				&& CurveSource->bPreparedContributionConsumed && !CurveSource->bExplicitlyCancelled
				&& CurveSource->GetTime() >= CurveSource->Duration, TEXT("原生 RMS 完整末区间未实际消费"));
			bPassed &= Check(bSawCapsuleMove && (FinalLocation - Entry).Equals(ExpectedDelta, 1.0),
				FString::Printf(TEXT("正式胶囊 XYZ 不符：实际 %s，资产期望 %s"),
					*(FinalLocation - Entry).ToString(), *ExpectedDelta.ToString()));
			bPassed &= Check(BeginCount == 1 && EndCount == 1 && OpenSamples >= 2 && ContactSamples >= 2 && EffectCount == 1,
				FString::Printf(TEXT("实际窗口／重复接触去重不符：Begin=%d End=%d OpenSamples=%d ContactSamples=%d GE=%d"),
					BeginCount, EndCount, OpenSamples, ContactSamples, EffectCount));
			bPassed &= Check(FMath::IsNearlyEqual(InitialHealth - TargetASC->GetNumericAttribute(UGGYGOHealthSet::GetHealthAttribute()), Damage, 0.01f)
				&& FMath::IsNearlyEqual(InitialPoise - TargetASC->GetNumericAttribute(UGGYGOHealthSet::GetPoiseAttribute()), PoiseDamage, 0.01f),
				TEXT("正式 GE／Execution／AttributeSet 的生命或韧性结算不符"));
			bPassed &= Check(!Movement->HasActiveActionMotion() && !Trace->IsTracing() && !Trace->IsComponentTickEnabled()
				&& !Anim->Montage_IsPlaying(Montage.Get()) && ASC->GetTickingTasksDescription().IsEmpty()
				&& Mesh->VisibilityBasedAnimTickOption == OriginalTick && Mesh->bEnableUpdateRateOptimizations == bOriginalURO,
				TEXT("自然结束后的 CMC／Trace／Montage／Mesh 资源未恢复"));
			for (const TWeakObjectPtr<UPackage>& Package : InputPackages)
				bPassed &= Check(Package.IsValid() && !Package->IsDirty(), TEXT("正式输入资产变脏，拒绝保存／通过"));
			bBehaviorVerified = bPassed;
			return BeginEnding();
		}

	private:
		bool Check(bool bCondition, const FString& Reason)
		{
			if (!bCondition) Test.AddError(TEXT("[BossAI.KevinIce01.ProductionSmoke] ") + Reason);
			bFailed |= !bCondition;
			return bCondition;
		}

		int32 CountPIEWorlds() const
		{
			int32 Count = 0;
			if (GEditor) for (const FWorldContext& Context : GEditor->GetWorldContexts())
				if (Context.WorldType == EWorldType::PIE) ++Count;
			return Count;
		}

		bool OwnsPIE() const
		{
			if (!GEditor || !RequestSettings.IsValid()) return false;
			const TOptional<FPlayInEditorSessionInfo> Info = GEditor->GetPlayInEditorSessionInfo();
			return Info.IsSet() && Info->OriginalRequestParams.EditorPlaySettings == RequestSettings.Get()
				&& Info->OriginalRequestParams.SessionDestination == EPlaySessionDestinationType::InProcess
				&& !Info->OriginalRequestParams.GameModeOverride;
		}

		bool RequestHost()
		{
			if (!Check(GEditor && !GEditor->IsPlaySessionInProgress() && !GEditor->IsSettingUpPlayWorld()
				&& !GEditor->PlayWorld && !GEditor->ShouldEndPlayMap() && !GEditor->GetPlaySessionRequest().IsSet()
				&& CountPIEWorlds() == 0, TEXT("需要空闲 Native Editor；拒绝接管已有或排队中的用户 PIE"))) return false;
			EditorWorld = GEditor->GetEditorWorldContext().World();
			if (!Check(EditorWorld.IsValid() && EditorWorld->GetPackage()->GetName() == TEXT("/Game/Map/L_Movement_Test"),
				TEXT("请以 L_Movement_Test 启动 Native Editor；本叶不换地图"))) return false;
			bEditorMapDirty = EditorWorld->GetPackage()->IsDirty();
			OuterWorld = GWorld; OuterViewport = GEngine->GameViewport.Get();
			RequestSettings.Reset(NewObject<ULevelEditorPlaySettings>(GetTransientPackage()));
			RequestSettings->SetPlayNetMode(EPlayNetMode::PIE_Standalone);
			RequestSettings->SetRunUnderOneProcess(true); RequestSettings->SetPlayNumberOfClients(1);
			RequestSettings->bLaunchSeparateServer = false; RequestSettings->GameGetsMouseControl = false;
			RequestSettings->bShouldMinimizeEditorOnNonVRPIE = false;
			FRequestPlaySessionParams Params;
			Params.EditorPlaySettings = RequestSettings.Get(); Params.bAllowOnlineSubsystem = false;
			PIEStartedObserver = FEditorDelegates::PostPIEStarted.AddLambda([this](bool bSimulating)
			{
				bPIEStarted = Check(!bSimulating && OwnsPIE() && CountPIEWorlds() == 1,
					TEXT("PostPIEStarted 未确认本叶唯一原生请求"));
			});
			HostDeadline = FPlatformTime::Seconds() + 30.0;
			GEditor->RequestPlaySession(Params);
			const TOptional<FRequestPlaySessionParams> Queued = GEditor->GetPlaySessionRequest();
			if (!Check(Queued.IsSet() && Queued->EditorPlaySettings
				&& Queued->SessionDestination == EPlaySessionDestinationType::InProcess && !Queued->GameModeOverride
				&& !Queued->bAllowOnlineSubsystem, TEXT("原生 PIE 未保留本叶请求"))) return false;
			// RequestPlaySession 同步复制设置；持有实际排队副本，后续会话归属仅认这个原生身份。
			RequestSettings.Reset(Queued->EditorPlaySettings.Get());
			return true;
		}

		void CancelOwnQueuedPIE()
		{
			if (!GEditor || !RequestSettings.IsValid()) return;
			const TOptional<FRequestPlaySessionParams> Queued = GEditor->GetPlaySessionRequest();
			if (Queued.IsSet() && Queued->EditorPlaySettings == RequestSettings.Get()) GEditor->CancelRequestPlaySession();
		}

		bool BeginEnding()
		{
			Cleanup();
			FEditorDelegates::PostPIEStarted.Remove(PIEStartedObserver);
			bEnding = true; EndDeadline = FPlatformTime::Seconds() + 10.0;
			if (OwnsPIE())
			{
				if (Check(CountPIEWorlds() == 1, TEXT("出现外来 PIE Context，拒绝批量结束"))) GEditor->RequestEndPlayMap();
			}
			else CancelOwnQueuedPIE();
			return false;
		}

		bool UpdateEnding()
		{
			if (OwnsPIE())
			{
				if (FPlatformTime::Seconds() < EndDeadline) return false;
				Check(false, TEXT("自有 PIE 收尾超时"));
				if (CountPIEWorlds() == 1) GEditor->EndPlayMap();
			}
			Check(!OwnsPIE() && CountPIEWorlds() == 0, TEXT("自有 PIE Context 未释放或被外来会话替换"));
			Check(EditorWorld.IsValid() && GWorld == OuterWorld && GEngine->GameViewport.Get() == OuterViewport
				&& EditorWorld->GetPackage()->IsDirty() == bEditorMapDirty, TEXT("原 Editor World／Viewport／地图脏状态未恢复"));
			if (!bFailed && bBehaviorVerified)
				Test.AddInfo(TEXT("[BossAI.KevinIce01.ProductionSmoke] PASS：正式配置授予／实际 XYZ 胶囊／原生窗口 GE 去重结算／资源、自建 Actor 及自有 PIE 清理。仅 L_Movement_Test 受控 Falling 查询夹具，无地面实战／P3／原作命中框／联机声明。"));
			return true;
		}

		bool IsReady(AGGYGOEncounterLifecycleTestEncounter* Encounter)
		{
			AGGYGOBossCharacter* Avatar = Encounter ? Encounter->GetBossAvatar() : nullptr;
			AGGYGOBossState* State = Encounter ? Encounter->GetBossState() : nullptr;
			UGGYGOAbilitySystemComponent* HostASC = State ? State->GetGGYGOAbilitySystemComponent() : nullptr;
			UGGYGOPawnExtensionComponent* Extension = Avatar ? Avatar->GetPawnExtensionComponent() : nullptr;
			if (!Avatar || !HostASC || !Extension) return false;
			const FGGYGOPawnASCResourceHandle Resource = Extension->GetCurrentLocalAbilitySystemResource();
			return Avatar->HasAuthority() && Avatar->GetGGYGOAbilitySystemComponent() == HostASC && State->GetAvatarPawn() == Avatar
				&& HostASC->AbilityActorInfo.IsValid() && HostASC->GetOwnerActor() == State && HostASC->GetAvatarActor() == Avatar
				&& Resource.HasResource() && Resource.GetIdentity().ASC.Get() == HostASC
				&& Extension->IsLocalAbilitySystemResourceReady(Resource)
				&& Encounter->GetBossController() && Avatar->GetController() == Encounter->GetBossController()
				&& Encounter->GetBossController()->GetPawn() == Avatar && !Avatar->FindComponentByClass<UGGYGOAbilitySystemComponent>();
		}

		AGGYGOEncounterLifecycleTestEncounter* SpawnEncounter(const FTransform& Transform)
		{
			FActorSpawnParameters Params;
			Params.ObjectFlags |= RF_Transient;
			Params.bDeferConstruction = true;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			AGGYGOEncounterLifecycleTestEncounter* Encounter = World->SpawnActor<AGGYGOEncounterLifecycleTestEncounter>(
				AGGYGOEncounterLifecycleTestEncounter::StaticClass(), Transform, Params);
			if (!Encounter) return nullptr;
			Encounters.Add(Encounter);
			Encounter->ConfigureForTest(Definition.Get());
			Encounter->FinishSpawning(Transform);
			if (!Encounter->SpawnBoss() || !IsReady(Encounter)) return nullptr;
			CreatedActors.Add(Encounter->GetBossState()); CreatedActors.Add(Encounter->GetBossController());
			CreatedActors.Add(Encounter->GetBossAvatar()); CreatedActors.Add(Encounter);
			return Encounter;
		}

		bool Start()
		{
			World = GEditor ? GEditor->PlayWorld : nullptr;
			if (!Check(World.IsValid() && World->HasBegunPlay() && World->GetNetMode() == NM_Standalone
				&& World->GetGameInstance() && UWorld::RemovePIEPrefix(World->GetPackage()->GetName()) == TEXT("/Game/Map/L_Movement_Test"),
				TEXT("本叶原生 Standalone PIE 未就绪或改变了 L_Movement_Test 宿主"))) return false;
			const FWorldContext* Context = GEditor->GetWorldContextFromWorld(World.Get());
			if (!Check(Context && Context->WorldType == EWorldType::PIE, TEXT("缺少真实 PIE Context"))) return false;
			WorldContext = Context->ContextHandle;
			Deadline = FPlatformTime::Seconds() + 15.0;
			FString ConfigText;
			TSharedPtr<FJsonObject> Config;
			const FString ConfigPath = FPaths::ProjectDir() / TEXT("AAADocs/Assets/BH3/KevinDemonBattle/BH3_Kevin_Combat_Wiring_Config.json");
			if (!Check(FFileHelper::LoadFileToString(ConfigText, *ConfigPath)
				&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ConfigText), Config) && Config.IsValid(),
				TEXT("正式 Wiring_Config 无效：") + ConfigPath)) return false;
			const TSharedPtr<FJsonObject>* Assets = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Actions = nullptr;
			FString Trigger, DefinitionPath, AbilityPath, MontagePath, EffectPath, StartSocket, EndSocket, MeshPath;
			if (!Check(Config->TryGetStringField(TEXT("attack_trigger"), Trigger) && Trigger == TEXT("automation_only")
				&& Config->TryGetObjectField(TEXT("assets"), Assets) && (*Assets)->TryGetStringField(TEXT("definition"), DefinitionPath)
				&& Config->TryGetArrayField(TEXT("actions"), Actions) && Actions->Num() == 1
				&& Config->TryGetStringField(TEXT("damage_effect"), EffectPath)
				&& Config->TryGetStringField(TEXT("mesh"), MeshPath)
				&& Config->TryGetStringField(TEXT("trace_start_socket"), StartSocket)
				&& Config->TryGetStringField(TEXT("trace_end_socket"), EndSocket), TEXT("正式配置必需字段缺失"))) return false;
			const TSharedPtr<FJsonObject> Action = (*Actions)[0]->AsObject();
			FString ActionId;
			double ConfigDamage = 0.0, ConfigPoise = 0.0, ConfigRadius = 0.0;
			if (!Check(Action.IsValid() && Action->TryGetStringField(TEXT("id"), ActionId) && ActionId == TEXT("Ice01")
				&& Action->TryGetStringField(TEXT("ability"), AbilityPath) && Action->TryGetStringField(TEXT("montage"), MontagePath)
				&& Action->TryGetNumberField(TEXT("damage"), ConfigDamage) && Action->TryGetNumberField(TEXT("poise_damage"), ConfigPoise)
				&& Action->TryGetNumberField(TEXT("trace_radius"), ConfigRadius)
				&& FMath::IsFinite(ConfigDamage) && ConfigDamage > 0 && FMath::IsFinite(ConfigPoise) && ConfigPoise > 0
				&& FMath::IsFinite(ConfigRadius) && ConfigRadius >= 1, TEXT("正式 Ice01 路径或测试数值无效"))) return false;
			Damage = static_cast<float>(ConfigDamage); PoiseDamage = static_cast<float>(ConfigPoise);
			const auto ObjectPath = [](const FString& Package) { return Package + TEXT(".") + FPackageName::GetLongPackageAssetName(Package); };
			Definition.Reset(LoadObject<UGGYGOBossDefinition>(nullptr, *ObjectPath(DefinitionPath)));
			Montage.Reset(LoadObject<UAnimMontage>(nullptr, *ObjectPath(MontagePath)));
			AbilityClass.Reset(LoadClass<UGGYGOBossMeleeAbility>(nullptr, *(ObjectPath(AbilityPath) + TEXT("_C"))));
			EffectClass.Reset(LoadClass<UGameplayEffect>(nullptr, *EffectPath));
			MeshAsset.Reset(LoadObject<USkeletalMesh>(nullptr, *ObjectPath(MeshPath)));
			if (!Check(Definition.IsValid() && Montage.IsValid() && AbilityClass.IsValid() && EffectClass.IsValid() && MeshAsset.IsValid(),
				TEXT("正式落盘 Definition／GA／Montage／GE／Mesh 缺失，拒绝替换资产"))) return false;
			const FGGYGOBossFormDefinition* Form = Definition->FindForm(Definition->InitialFormTag);
			if (!Check(Form && Form->AvatarPawnData && Form->AvatarPawnData->AbilitySets.Num() == 1
			&& Form->AvatarPawnData->MovementSet && Form->AvatarPawnData->MovementSet->bUseCurveDrivenSpeed
				&& !Definition->BehaviorTree && Montage->GetNumSections() == 1 && Montage->GetSectionName(0) == TEXT("Main")
				&& Montage->GetAnimCompositeSection(0).GetTime() == 0 && Montage->GetAnimCompositeSection(0).NextSectionName.IsNone()
				&& Montage->SlotAnimTracks.Num() == 1 && Montage->SlotAnimTracks[0].AnimTrack.AnimSegments.Num() == 1,
				TEXT("正式挂接／动画单段或正常曲线模式不符"))) return false;
			Sequence.Reset(Cast<UAnimSequence>(Montage->SlotAnimTracks[0].AnimTrack.AnimSegments[0].GetAnimReference()));
			if (!Check(Sequence.IsValid() && Sequence->GetDataModel(), TEXT("正式派生 Sequence 无原生曲线模型"))) return false;
			const FName CurveNames[] = {TEXT("RootMotion_PosX"), TEXT("RootMotion_PosY"), TEXT("RootMotion_PosZ")};
			FVector CurveDelta;
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				const FFloatCurve* Curve = Sequence->GetDataModel()->FindFloatCurve(FAnimationCurveIdentifier(CurveNames[Axis], ERawCurveTrackTypes::RCT_Float));
				if (!Check(Curve && !Curve->FloatCurve.GetConstRefOfKeys().IsEmpty(), TEXT("正式完整 XYZ 曲线缺失"))) return false;
				CurveDelta[Axis] = Curve->FloatCurve.Eval(Sequence->GetPlayLength()) - Curve->FloatCurve.Eval(0.0f);
			}
			if (!Check(!CurveDelta.ContainsNaN(), TEXT("正式 XYZ 末值非法"))) return false;
			for (const UObject* Input : {static_cast<const UObject*>(Definition.Get()), static_cast<const UObject*>(Montage.Get()),
				static_cast<const UObject*>(Sequence.Get()), static_cast<const UObject*>(AbilityClass.Get()), static_cast<const UObject*>(EffectClass.Get()),
				static_cast<const UObject*>(MeshAsset.Get()), static_cast<const UObject*>(MeshAsset->GetSkeleton()), static_cast<const UObject*>(Form->AvatarPawnData.Get()),
				static_cast<const UObject*>(Form->AvatarPawnData->MovementSet.Get()), static_cast<const UObject*>(Form->AvatarPawnData->AbilitySets[0].Get())})
			{
				if (!Check(Input && !Input->GetPackage()->IsDirty(), TEXT("正式输入资产须由原作者保存冻结"))) return false;
				InputPackages.AddUnique(Input->GetPackage());
			}
			APlayerStart* Anchor = nullptr;
			for (TActorIterator<APlayerStart> It(World.Get()); It; ++It) { Anchor = *It; break; }
			if (!Check(Anchor != nullptr, TEXT("L_Movement_Test 无真实 PlayerStart，拒绝猜测测试锚点"))) return false;
			if (!Check(Form->AvatarPawnData->PawnClass && Form->AvatarPawnData->PawnClass->IsChildOf(AGGYGOBossCharacter::StaticClass()),
				TEXT("正式 PawnClass 缺失或不属于 Boss Avatar"))) return false;
			const ACharacter* PawnCDO = Form->AvatarPawnData->PawnClass->GetDefaultObject<ACharacter>();
			if (!Check(PawnCDO && PawnCDO->GetCapsuleComponent(), TEXT("正式 PawnClass 胶囊无效"))) return false;
			// 使用 CMC 原生准入的 Falling 夹具；仅临时 Source 实例关闭重力以核对三轴贡献，不代表地面实战。
			FTransform FixtureTransform(FRotator::ZeroRotator, Anchor->GetActorLocation()
				+ FVector(0, 0, PawnCDO->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() * 4.0));
			AGGYGOEncounterLifecycleTestEncounter* SourceEncounter = SpawnEncounter(FixtureTransform);
			if (!Check(SourceEncounter != nullptr, TEXT("正式 Source Encounter 装配／Ready 失败"))) return false;
			Source = SourceEncounter->GetBossAvatar(); ASC = SourceEncounter->GetBossState()->GetGGYGOAbilitySystemComponent();
			Mesh = Source->GetMesh(); Movement = Source->GetGGYGOMovementComponent(); Trace = Source->FindComponentByClass<UGGYGOMeleeTraceComponent>();
			if (!Check(Mesh.IsValid() && Movement.IsValid() && Trace.IsValid() && Mesh->GetSkeletalMeshAsset() == MeshAsset.Get()
				&& Mesh->DoesSocketExist(FName(*StartSocket)) && Mesh->DoesSocketExist(FName(*EndSocket)), TEXT("正式 Mesh／CMC／Socket 接收者不符"))) return false;
			Anim = Mesh->GetAnimInstance(); SourceBinding = CurrentResource(Source.Get());
			if (!Check(Anim.IsValid(), TEXT("正式动画实例缺失"))) return false;
			if (!Check(!Anim->GetClass()->GetPackage()->IsDirty(), TEXT("正式 ABP 输入变脏"))) return false;
			InputPackages.AddUnique(Anim->GetClass()->GetPackage());
			TraceStart = FName(*StartSocket); TraceEnd = FName(*EndSocket);
			Movement->GravityScale = 0.0f;
			Movement->SetMovementMode(MOVE_Falling);
			if (!Check(Movement->IsFalling() && FMath::IsNearlyZero(Movement->GetGravityZ()),
				TEXT("受控 Falling 夹具未进入合法模式或实例重力未关闭"))) return false;
			Source->GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
			FixtureTransform.AddToTranslation(FVector(0, 0, PawnCDO->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() * 8.0));
			AGGYGOEncounterLifecycleTestEncounter* TargetEncounter = SpawnEncounter(FixtureTransform);
			if (!Check(TargetEncounter != nullptr, TEXT("正式 Target Encounter 装配／Ready 失败"))) return false;
			Target = TargetEncounter->GetBossAvatar(); TargetASC = TargetEncounter->GetBossState()->GetGGYGOAbilitySystemComponent();
			TargetBinding = CurrentResource(Target.Get());
			Target->GetCharacterMovement()->SetComponentTickEnabled(false);
			Target->GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Target->GetMesh()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Probe = NewObject<USphereComponent>(Target.Get(), NAME_None, RF_Transient);
			Probe->SetupAttachment(Target->GetRootComponent());
			Probe->SetCollisionObjectType(ECC_Pawn); Probe->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Probe->SetCollisionResponseToAllChannels(ECR_Ignore); Probe->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
			Probe->RegisterComponent();
			InitialHealth = TargetASC->GetNumericAttribute(UGGYGOHealthSet::GetHealthAttribute());
			InitialPoise = TargetASC->GetNumericAttribute(UGGYGOHealthSet::GetPoiseAttribute());
			if (!Check(InitialHealth > Damage && InitialPoise > PoiseDamage, TEXT("真实目标属性未初始化或不足以验去重"))) return false;
			const FGameplayAbilitySpec* Selected = nullptr;
			int32 SpecCount = 0;
			for (const FGameplayAbilitySpec& Granted : ASC->GetActivatableAbilities())
				if (Granted.Ability && Granted.Ability->GetClass() == AbilityClass.Get()) { Selected = &Granted; ++SpecCount; }
			if (!Check(SpecCount == 1 && Selected && Selected->SourceObject.Get() == Definition.Get(),
				TEXT("正式 State 配置授予的唯一 Spec／来源不符；不手工补授予"))) return false;
			Ability = Cast<UGGYGOBossMeleeAbility>(Selected->GetPrimaryInstance());
			FString AbilityError;
			if (!Check(Ability.IsValid() && Ability->ValidateMeleeConfiguration(AbilityError), TEXT("正式 GA 配置失败：") + AbilityError)) return false;
			OriginalTick = Mesh->VisibilityBasedAnimTickOption; bOriginalURO = Mesh->bEnableUpdateRateOptimizations;
			Entry = Source->GetActorLocation(); ExpectedDelta = Source->GetActorQuat().RotateVector(CurveDelta * Mesh->GetComponentScale());
			WindowEventTags.AddTag(GGYGOGameplayTags::Event_Montage_HitWindowBegin);
			WindowEventTags.AddTag(GGYGOGameplayTags::Event_Montage_HitWindowEnd);
			WindowObserver = ASC->AddGameplayEventTagContainerDelegate(WindowEventTags,
				FGameplayEventTagMulticastDelegate::FDelegate::CreateLambda(
				[this, ConfigRadius](FGameplayTag Tag, const FGameplayEventData* Payload)
				{
					if (bCleaned) return;
					FGGYGOMontageWindowSourceData WindowSource;
					// 只观察真实 producer 的同次来源；窗口准入仍由正式 Task 及下游 Trace／GE 证明。
					if (!Check(UGGYGOAnimNotifyState_GameplayEventWindow::TryReadWindowDispatch(Payload, WindowSource)
						&& WindowSource.EventTag == Tag && WindowSource.Animation.Get() == Montage.Get()
						&& WindowSource.Mesh.Get() == Mesh.Get() && WindowSource.AnimInstance.Get() == Anim.Get()
						&& WindowSource.Notify.IsValid() && WindowSource.bHasMontageInstanceContext
						&& WindowSource.MontageInstanceId != INDEX_NONE, TEXT("正式窗口不是原 Notify 的真实发送来源"))) return;
					if (WindowSource.Phase == EGGYGOMontageWindowPhase::End)
					{
						if (Tag == GGYGOGameplayTags::Event_Montage_HitWindowEnd) { ++EndCount; }
						return;
					}
					if (Tag != GGYGOGameplayTags::Event_Montage_HitWindowBegin) return;
					++BeginCount;
					if (!Check(BeginCount == 1 && Mesh.IsValid() && Target.IsValid() && Probe.IsValid(), TEXT("窗口或目标夹具身份失效"))) return;
					const FVector A = Mesh->GetSocketLocation(TraceStart), B = Mesh->GetSocketLocation(TraceEnd);
					if (!Check(!A.ContainsNaN() && !B.ContainsNaN() && (B - A).Size() > 1, TEXT("真实武器 Socket 帧无效"))) return;
					Probe->SetSphereRadius(static_cast<float>((B - A).Size() * 0.5 + ConfigRadius));
					Target->SetActorLocation((A + B) * 0.5, false, nullptr, ETeleportType::TeleportPhysics);
				}));
			if (!Check(WindowObserver.IsValid(), TEXT("真实窗口 producer 观察登记失败"))) return false;
			EffectObserver = ASC->OnGameplayEffectAppliedDelegateToTarget.AddLambda(
				[this](UAbilitySystemComponent* Receiver, const FGameplayEffectSpec& Spec, FActiveGameplayEffectHandle)
				{
					++EffectCount;
					if (!Check(ASC.IsValid() && TargetASC.IsValid() && Ability.IsValid() && Trace.IsValid(), TEXT("GE 回执时原接收者已失效"))) return;
					const FGameplayEffectContextHandle& HitContext = Spec.GetContext();
					const FHitResult* Hit = HitContext.GetHitResult();
					Check(Receiver == TargetASC.Get() && Spec.Def && Spec.Def->GetClass() == EffectClass.Get()
						&& HitContext.GetAbilityInstance_NotReplicated() == Ability.Get() && HitContext.GetInstigator() == ASC->GetOwnerActor()
						&& Hit && Hit->GetActor() == Target.Get() && Trace->IsTracing() && BeginCount == 1 && EndCount == 0
						&& FMath::IsNearlyEqual(Spec.GetSetByCallerMagnitude(GGYGOGameplayTags::SetByCaller_Damage, false), Damage)
						&& FMath::IsNearlyEqual(Spec.GetSetByCallerMagnitude(GGYGOGameplayTags::SetByCaller_PoiseDamage, false), PoiseDamage),
						TEXT("原生 GE 回执的真实目标／正式 GA／GE／命中上下文／窗口／数值不符"));
				});
			CompletionObserver = ASC->OnAbilityEnded.AddLambda([this](const FAbilityEndedData& Data)
			{
				if (!Original.HasActivation() || Data.AbilityThatEnded.Get() != Ability.Get()) return;
				++CompletionCount; bCancelled = Data.bWasCancelled;
				if (Source.IsValid()) FinalLocation = Source->GetActorLocation();
			});
			const GGYGOBossMeleeTestLifecycle::FActivation Request = GGYGOBossMeleeTestLifecycle::Activate(ASC.Get(), Selected->Handle);
			Original = Request.OriginalActivation;
			if (!Check(Request.bNativeAccepted && Original.HasActivation() && Ability->IsActive() && Movement->HasActiveActionMotion(),
				TEXT("正式已授予 GA 未建立原始播放／CMC"))) return false;
			const TSharedPtr<FRootMotionSource> NativeSource = Movement->GetRootMotionSource(TEXT("GGYGO.ActionCurve"));
			if (!Check(NativeSource.IsValid() && NativeSource->GetScriptStruct() == FRootMotionSource_GGYGOActionCurve::StaticStruct(),
				TEXT("实际 CMC 未持有正式原生 Action RMS"))) return false;
			CurveSource = StaticCastSharedPtr<FRootMotionSource_GGYGOActionCurve>(NativeSource);
			if (!Check(CurveSource->SourceMode == EGGYGOActionCurveSourceMode::OriginalMontage && CurveSource->OriginalBinding.IsValid()
				&& CurveSource->OriginalBinding->Montage.Get() == Montage.Get() && CurveSource->OriginalBinding->Segments.Num() == 1
				&& CurveSource->OriginalBinding->Segments[0].Sequence.Get() == Sequence.Get(), TEXT("实际 RMS 没有消费正式 paired XYZ"))) return false;
			Test.AddInfo(TEXT("[BossAI.KevinIce01.ProductionSmoke] 已由真实 Encounter／State 配置授予并激活正式 GA；等待 UE 自然帧，不推进 Task／CMC 私有时钟。"));
			return true;
		}

		bool Cleanup()
		{
			if (bCleaned) return !bFailed;
			bCleaned = true;
			if (ASC.IsValid())
			{
				ASC->RemoveGameplayEventTagContainerDelegate(WindowEventTags, WindowObserver);
				ASC->OnGameplayEffectAppliedDelegateToTarget.Remove(EffectObserver);
				ASC->OnAbilityEnded.Remove(CompletionObserver);
			}
			WindowObserver.Reset();
			if (Ability.IsValid() && Ability->IsActive() && Original.HasActivation()) Ability->RequestAbilityCancel(Original, true);
			for (const TWeakObjectPtr<AGGYGOEncounterLifecycleTestEncounter>& Encounter : Encounters)
				if (Encounter.IsValid()) { Encounter->CleanupForTest(); Encounter->Destroy(); }
			for (const TWeakObjectPtr<AActor>& Created : CreatedActors)
				Check(!Created.IsValid() || Created->IsActorBeingDestroyed(), TEXT("Encounter 自建 Actor 未回收：") + GetPathNameSafe(Created.Get()));
			CurveSource.Reset();
			return !bFailed;
		}

		FAutomationTestBase& Test;
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<UWorld> EditorWorld;
		TStrongObjectPtr<ULevelEditorPlaySettings> RequestSettings;
		FDelegateHandle PIEStartedObserver;
		UWorld* OuterWorld = nullptr;
		UGameViewportClient* OuterViewport = nullptr;
		FName WorldContext;
		TStrongObjectPtr<UGGYGOBossDefinition> Definition;
		TStrongObjectPtr<UAnimMontage> Montage;
		TStrongObjectPtr<UAnimSequence> Sequence;
		TStrongObjectPtr<UClass> AbilityClass, EffectClass;
		TStrongObjectPtr<USkeletalMesh> MeshAsset;
		TArray<TWeakObjectPtr<UPackage>> InputPackages;
		TArray<TWeakObjectPtr<AGGYGOEncounterLifecycleTestEncounter>> Encounters;
		TArray<TWeakObjectPtr<AActor>> CreatedActors;
		TWeakObjectPtr<AGGYGOBossCharacter> Source, Target;
		TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC, TargetASC;
		TWeakObjectPtr<UGGYGOCharacterMovementComponent> Movement;
		TWeakObjectPtr<USkeletalMeshComponent> Mesh;
		TWeakObjectPtr<UAnimInstance> Anim;
		TWeakObjectPtr<UGGYGOMeleeTraceComponent> Trace;
		TWeakObjectPtr<UGGYGOBossMeleeAbility> Ability;
		TWeakObjectPtr<USphereComponent> Probe;
		TSharedPtr<FRootMotionSource_GGYGOActionCurve> CurveSource;
		FGGYGOAbilityActivationHandle Original;
		/** 绑定身份：同一次绑定的 PawnExtension 资源句柄。 */
		FGGYGOPawnASCResourceHandle SourceBinding, TargetBinding;
		static FGGYGOPawnASCResourceHandle CurrentResource(const APawn* Pawn)
		{
			const UGGYGOPawnExtensionComponent* Extension = UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(Pawn);
			return Extension ? Extension->GetCurrentLocalAbilitySystemResource() : FGGYGOPawnASCResourceHandle{};
		}
		FGameplayTagContainer WindowEventTags;
		FDelegateHandle WindowObserver, EffectObserver, CompletionObserver;
		FName TraceStart, TraceEnd;
		FVector Entry, FinalLocation, ExpectedDelta;
		EVisibilityBasedAnimTickOption OriginalTick = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
		float Damage = 0.0f, PoiseDamage = 0.0f, InitialHealth = 0.0f, InitialPoise = 0.0f;
		double Deadline = 0.0, HostDeadline = 0.0, EndDeadline = 0.0;
		int32 BeginCount = 0, EndCount = 0, OpenSamples = 0, ContactSamples = 0, EffectCount = 0, CompletionCount = 0;
		bool bStarted = false, bFailed = false, bCleaned = false, bCancelled = false, bSawCapsuleMove = false, bOriginalURO = false;
		bool bAssetsStarted = false, bPIEStarted = false, bEnding = false, bBehaviorVerified = false, bEditorMapDirty = false;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOKevinIce01ProductionTest, "GGYGO.BossAI.Melee.KevinIce01ProductionSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGGYGOKevinIce01ProductionTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(FKevinIce01ProductionCommand(*this));
	return true;
}
#endif
#endif
