#include "AbilitySystem/Tests/GGYGOAbilityAdmissionTestTypes.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/Groups/GGYGOAbilityGroupTypes.h"
#include "Abilities/GameplayAbilityTargetTypes.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "System/GGYGOGameplayTags.h"
#include "UObject/Package.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOAbilityAdmissionTestTypes)

UGGYGOAbilityAdmissionTestAbility::UGGYGOAbilityAdmissionTestAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	GroupTag = GGYGOGameplayTags::AbilityGroup_Skill;
	ActivationPriority = 100;
	SelfPolicy = EGGYGOAbilitySelfPolicy::Coexist;
}

void UGGYGOAbilityAdmissionTestAbility::ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
	FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	Super::ActivateAbilityBody(Original, Handle, ActorInfo, ActivationInfo, TriggerEventData);
	if (IsActive())
	{
		++BusinessActivationCount;
	}
}

void UGGYGOAbilityAdmissionTestAbility::FinishForTest()
{
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, false, false);
}

bool UGGYGOAbilityAdmissionTestAbility::ActivateWithInvalidPredictionKeyForTest(
	FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo)
{
	if (!ActorInfo || !CanActivateAbility(Handle, ActorInfo, nullptr, nullptr, nullptr)) { return false; }
	// Use GAS's real PreActivate/Activate entry without TryActivate's generated server key.
	// The same invalid prediction key is deliberately reused by consecutive activations.
	const FGameplayAbilityActivationInfo UnpredictedActivation(ActorInfo->OwnerActor.Get());
	CallActivateAbility(Handle, ActorInfo, UnpredictedActivation);
	return true;
}

UGGYGOAbilityAdmissionHigherTestAbility::UGGYGOAbilityAdmissionHigherTestAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	ActivationPriority = 200;
}

UGGYGOAbilityAdmissionHighestTestAbility::UGGYGOAbilityAdmissionHighestTestAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	ActivationPriority = 300;
}

UGGYGOAbilityAdmissionPerExecutionTestAbility::UGGYGOAbilityAdmissionPerExecutionTestAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerExecution;
}

FGGYGOAbilityAdmissionLifecycleObserver UGGYGOAbilityAdmissionPerExecutionTestAbility::LifecycleObserver;

void UGGYGOAbilityAdmissionPerExecutionTestAbility::ObserveLifecycle(
	EGGYGOAbilityAdmissionLifecycleObservation Observation, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilitySpecHandle Handle, bool bReplicateEndAbility, bool bWasCancelled)
{
	FGGYGOAbilityAdmissionLifecycleSample Sample;
	Sample.Ability = this;
	Sample.Observation = Observation;
	Sample.bIsActive = IsActive();
	Sample.bReplicateEndAbility = bReplicateEndAbility;
	Sample.bWasCancelled = bWasCancelled;
	if (ActorInfo)
	{
		if (UGGYGOAbilitySystemComponent* ASC = Cast<UGGYGOAbilitySystemComponent>(ActorInfo->AbilitySystemComponent.Get()))
		{
			if (const FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromHandle(Handle))
			{
				Sample.SpecActiveCount = static_cast<int32>(Spec->ActiveCount);
			}
		}
	}

	LifecycleObserver.ExecuteIfBound(Sample);
}

#if WITH_DEV_AUTOMATION_TESTS
void UGGYGOAbilityAdmissionPerExecutionTestAbility::ObserveAbilityActivationEntryForTest(
	FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	ObserveLifecycle(EGGYGOAbilityAdmissionLifecycleObservation::ActivateBeforeSuper, ActorInfo, Handle);
}

void UGGYGOAbilityAdmissionPerExecutionTestAbility::ObserveAbilityActivationReturnForTest(
	FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	ObserveLifecycle(EGGYGOAbilityAdmissionLifecycleObservation::ActivateAfterSuper, ActorInfo, Handle);
}

void UGGYGOAbilityAdmissionPerExecutionTestAbility::ObserveAbilityEndEntryForTest(
	FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled)
{
	ObserveLifecycle(EGGYGOAbilityAdmissionLifecycleObservation::EndBeforeSuper, ActorInfo, Handle,
		bReplicateEndAbility, bWasCancelled);
}
#endif

UGGYGOAbilityAdmissionUncancelableTestAbility::UGGYGOAbilityAdmissionUncancelableTestAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SelfPolicy = EGGYGOAbilitySelfPolicy::Exclusive;
}

void UGGYGOAbilityAdmissionUncancelableTestAbility::InitializeAbilityActivation(
	const FGGYGOAbilityActivationHandle& Original)
{
	SetCanBeCanceled(false);
	Super::InitializeAbilityActivation(Original);
}

UGGYGOAbilityAdmissionQueuedTestAbility::UGGYGOAbilityAdmissionQueuedTestAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	GroupTag = GGYGOGameplayTags::AbilityGroup_Attack;
}

UGGYGOAbilityAdmissionCoexistTestAbility::UGGYGOAbilityAdmissionCoexistTestAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	GroupTag = GGYGOGameplayTags::AbilityGroup_Passive;
}

UGGYGOAbilityAdmissionCorrectionTestAbility::UGGYGOAbilityAdmissionCorrectionTestAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	GroupTag = FGameplayTag();
}

void UGGYGOAbilityAdmissionCorrectionTestAbility::ReceiveAbilityCorrection(const FGameplayAbilityTargetDataHandle& Correction)
{
	++CorrectionCount;
	CorrectionTargetCount = Correction.Num();
	bReceivedLocationCorrection = Correction.Num() > 0
		&& Correction.Get(0)->GetScriptStruct() == FGameplayAbilityTargetData_LocationInfo::StaticStruct();
}

UGGYGOAbilityAdmissionForeignTestAbility::UGGYGOAbilityAdmissionForeignTestAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;
}

void UGGYGOAbilityAdmissionForeignTestAbility::FinishForTest()
{
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, false, false);
}

void UGGYGOAbilityAdmissionTestGroupConfig::SetRuleForTest(FGameplayTag GroupTag,
	EGGYGOAbilityGroupRule RuleType, bool bNewcomerWinsOnTie)
{
	FGGYGOAbilityGroupRule& Rule = GroupRules.FindOrAdd(GroupTag);
	Rule.Rule = RuleType;
	Rule.bNewcomerWinsOnTie = bNewcomerWinsOnTie;
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

namespace
{
	struct FGGYGOAbilityAdmissionTestWorld
	{
		UWorld* World = nullptr;
		UEngine* Engine = nullptr;

		explicit FGGYGOAbilityAdmissionTestWorld(UEngine* InEngine)
			: Engine(InEngine)
		{
			UWorld::InitializationValues Init;
			Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(false)
				.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
			if (World && Engine)
			{
				Engine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			}
		}

		~FGGYGOAbilityAdmissionTestWorld()
		{
			if (World)
			{
				// Keep the world context registered until component and actor teardown completes.
				World->DestroyWorld(false);
				if (Engine)
				{
					Engine->DestroyWorldContext(World);
				}
				if (UPackage* WorldPackage = World->GetPackage())
				{
					WorldPackage->SetDirtyFlag(false);
				}
			}
		}
	};

	struct FScopedAbilityEndedCallback
	{
		UGGYGOAbilitySystemComponent* ASC = nullptr;
		FDelegateHandle Handle;

		FScopedAbilityEndedCallback(UGGYGOAbilitySystemComponent* InASC, FDelegateHandle InHandle)
			: ASC(InASC), Handle(InHandle) {}

		void Reset()
		{
			if (ASC && Handle.IsValid())
			{
				ASC->OnAbilityEnded.Remove(Handle);
				Handle = FDelegateHandle();
			}
		}

		~FScopedAbilityEndedCallback() { Reset(); }
	};

	struct FScopedAbilityActivatedCallback
	{
		UGGYGOAbilitySystemComponent* ASC = nullptr;
		FDelegateHandle Handle;

		FScopedAbilityActivatedCallback(UGGYGOAbilitySystemComponent* InASC, FDelegateHandle InHandle)
			: ASC(InASC), Handle(InHandle) {}

		void Reset()
		{
			if (ASC && Handle.IsValid())
			{
				ASC->AbilityActivatedCallbacks.Remove(Handle);
				Handle = FDelegateHandle();
			}
		}

		~FScopedAbilityActivatedCallback() { Reset(); }
	};

	struct FScopedPerExecutionLifecycleObserver
	{
		void Reset()
		{
			UGGYGOAbilityAdmissionPerExecutionTestAbility::LifecycleObserver.Unbind();
		}

		~FScopedPerExecutionLifecycleObserver() { Reset(); }
	};

	struct FScopedAbilityGroupFreedCallback
	{
		UGGYGOAbilitySystemComponent* ASC = nullptr;
		FDelegateHandle Handle;

		FScopedAbilityGroupFreedCallback(UGGYGOAbilitySystemComponent* InASC, FDelegateHandle InHandle)
			: ASC(InASC), Handle(InHandle) {}

		void Reset()
		{
			if (ASC && Handle.IsValid())
			{
				ASC->OnAbilityGroupFreed.Remove(Handle);
				Handle = FDelegateHandle();
			}
		}

		~FScopedAbilityGroupFreedCallback() { Reset(); }
	};

	UGGYGOAbilitySystemComponent* MakeAdmissionTestASC(UWorld* World)
	{
		AActor* Owner = World ? World->SpawnActor<AActor>() : nullptr;
		if (!Owner)
		{
			return nullptr;
		}

		UGGYGOAbilitySystemComponent* ASC = NewObject<UGGYGOAbilitySystemComponent>(Owner);
		ASC->RegisterComponent();
		ASC->InitAbilityActorInfo(Owner, Owner);
		return ASC;
	}

	template <typename TAbility>
	FGameplayAbilitySpecHandle GrantAdmissionTestAbility(UGGYGOAbilitySystemComponent* ASC)
	{
		return ASC->GiveAbility(FGameplayAbilitySpec(TAbility::StaticClass(), 1));
	}

	UGGYGOAbilityAdmissionTestAbility* GetAdmissionTestAbility(UGGYGOAbilitySystemComponent* ASC,
		FGameplayAbilitySpecHandle Handle)
	{
		FGameplayAbilitySpec* Spec = ASC ? ASC->FindAbilitySpecFromHandle(Handle) : nullptr;
		return Spec ? Cast<UGGYGOAbilityAdmissionTestAbility>(Spec->GetPrimaryInstance()) : nullptr;
	}

	UGGYGOAbilityAdmissionCorrectionTestAbility* GetCorrectionTestAbility(UGGYGOAbilitySystemComponent* ASC,
		FGameplayAbilitySpecHandle Handle)
	{
		FGameplayAbilitySpec* Spec = ASC ? ASC->FindAbilitySpecFromHandle(Handle) : nullptr;
		return Spec ? Cast<UGGYGOAbilityAdmissionCorrectionTestAbility>(Spec->GetPrimaryInstance()) : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOAbilityAdmissionGroupLifecycleTest,
	"GGYGO.AbilitySystem.Admission.GroupLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOAbilityAdmissionGroupLifecycleTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEngine"), GEngine)) { return false; }
	FGGYGOAbilityAdmissionTestWorld TestWorld(GEngine);
	if (!TestNotNull(TEXT("临时测试世界"), TestWorld.World)) { return false; }
	UGGYGOAbilitySystemComponent* ASC = MakeAdmissionTestASC(TestWorld.World);
	if (!TestNotNull(TEXT("真实 GGYGO ASC"), ASC)) { return false; }

	UGGYGOAbilityAdmissionTestGroupConfig* GroupConfig = NewObject<UGGYGOAbilityAdmissionTestGroupConfig>(ASC);
	ASC->SetAbilityGroupConfig(GroupConfig);
	const FGameplayTag SkillGroup = GGYGOGameplayTags::AbilityGroup_Skill;

	// An un-cancelable single-instance occupant keeps the slot even against a higher priority request.
	const FGameplayAbilitySpecHandle UncancelableHandle = GrantAdmissionTestAbility<UGGYGOAbilityAdmissionUncancelableTestAbility>(ASC);
	if (!TestTrue(TEXT("不可取消旧实例激活"), ASC->TryActivateAbility(UncancelableHandle))) { return false; }
	UGGYGOAbilityAdmissionTestAbility* Uncancelable = GetAdmissionTestAbility(ASC, UncancelableHandle);
	if (!TestNotNull(TEXT("不可取消实例"), Uncancelable) || !TestFalse(TEXT("旧实例确实不可取消"), Uncancelable->CanBeCanceled())) { return false; }
	const FGameplayAbilitySpecHandle RejectedHigherHandle = GrantAdmissionTestAbility<UGGYGOAbilityAdmissionHigherTestAbility>(ASC);
	if (!TestFalse(TEXT("高优先级不能取代不可取消实例"), ASC->TryActivateAbility(RejectedHigherHandle))) { return false; }
	FGameplayAbilitySpec* RejectedHigherSpec = ASC->FindAbilitySpecFromHandle(RejectedHigherHandle);
	EGGYGOAbilityGroupBlockReason BlockReason = EGGYGOAbilityGroupBlockReason::NotBlocked;
	if (!TestNotNull(TEXT("高优先级请求 Spec"), RejectedHigherSpec)
		|| !TestTrue(TEXT("不可取消原因进入组仲裁"), ASC->IsActivationBlockedByGroup(
		Cast<UGGYGOGameplayAbility>(RejectedHigherSpec->Ability), BlockReason))
		|| !TestEqual(TEXT("明确报告不可取消原因"), BlockReason, EGGYGOAbilityGroupBlockReason::UncancelableActive)
		|| !TestEqual(TEXT("被拒高优先级能力未进入业务"), GetAdmissionTestAbility(ASC, RejectedHigherHandle)->GetBusinessActivationCountForTest(), 0)
		|| !TestTrue(TEXT("不可取消旧实例仍活跃"), Uncancelable->IsActive())) { return false; }
	Uncancelable->FinishForTest();

	// A higher-priority newcomer replaces an active, cancelable single-instance ability.
	const FGameplayAbilitySpecHandle LowerHandle = GrantAdmissionTestAbility<UGGYGOAbilityAdmissionTestAbility>(ASC);
	if (!TestTrue(TEXT("低优先级旧实例激活"), ASC->TryActivateAbility(LowerHandle))) { return false; }
	UGGYGOAbilityAdmissionTestAbility* Lower = GetAdmissionTestAbility(ASC, LowerHandle);
	const FGameplayAbilitySpecHandle HigherHandle = GrantAdmissionTestAbility<UGGYGOAbilityAdmissionHigherTestAbility>(ASC);
	if (!TestTrue(TEXT("高优先级新实例成功替换"), ASC->TryActivateAbility(HigherHandle))
		|| !TestFalse(TEXT("旧实例已被取消"), Lower->IsActive())
		|| !TestEqual(TEXT("新实例进入业务"), GetAdmissionTestAbility(ASC, HigherHandle)->GetBusinessActivationCountForTest(), 1)) { return false; }
	GetAdmissionTestAbility(ASC, HigherHandle)->FinishForTest();

	// The default tie policy lets the newcomer replace the equal-priority occupant.
	const FGameplayAbilitySpecHandle TieOldHandle = GrantAdmissionTestAbility<UGGYGOAbilityAdmissionTestAbility>(ASC);
	if (!TestTrue(TEXT("同级旧实例激活"), ASC->TryActivateAbility(TieOldHandle))) { return false; }
	UGGYGOAbilityAdmissionTestAbility* TieOld = GetAdmissionTestAbility(ASC, TieOldHandle);
	const FGameplayAbilitySpecHandle TieNewHandle = GrantAdmissionTestAbility<UGGYGOAbilityAdmissionTestAbility>(ASC);
	if (!TestTrue(TEXT("默认后来者胜平手"), ASC->TryActivateAbility(TieNewHandle))
		|| !TestFalse(TEXT("平手旧实例结束"), TieOld->IsActive())) { return false; }
	GetAdmissionTestAbility(ASC, TieNewHandle)->FinishForTest();

	// A configured first-wins tie keeps the old instance and rejects the same-priority newcomer.
	GroupConfig->SetRuleForTest(SkillGroup, EGGYGOAbilityGroupRule::SingleInstance, false);
	const FGameplayAbilitySpecHandle FirstWinsOldHandle = GrantAdmissionTestAbility<UGGYGOAbilityAdmissionTestAbility>(ASC);
	if (!TestTrue(TEXT("先到者组旧实例激活"), ASC->TryActivateAbility(FirstWinsOldHandle))) { return false; }
	UGGYGOAbilityAdmissionTestAbility* FirstWinsOld = GetAdmissionTestAbility(ASC, FirstWinsOldHandle);
	const FGameplayAbilitySpecHandle FirstWinsNewHandle = GrantAdmissionTestAbility<UGGYGOAbilityAdmissionTestAbility>(ASC);
	if (!TestFalse(TEXT("先到者胜平手"), ASC->TryActivateAbility(FirstWinsNewHandle))
		|| !TestTrue(TEXT("被拒平手不取消旧实例"), FirstWinsOld->IsActive())
		|| !TestEqual(TEXT("被拒同级未进入业务"), GetAdmissionTestAbility(ASC, FirstWinsNewHandle)->GetBusinessActivationCountForTest(), 0)) { return false; }
	FirstWinsOld->FinishForTest();

	// Queued never cancels the occupant, regardless of priority; Coexist admits both instances.
	const FGameplayTag AttackGroup = GGYGOGameplayTags::AbilityGroup_Attack;
	GroupConfig->SetRuleForTest(AttackGroup, EGGYGOAbilityGroupRule::SingleInstanceQueued);
	const FGameplayAbilitySpecHandle QueuedOldHandle = GrantAdmissionTestAbility<UGGYGOAbilityAdmissionQueuedTestAbility>(ASC);
	if (!TestTrue(TEXT("Queued 旧实例激活"), ASC->TryActivateAbility(QueuedOldHandle))) { return false; }
	UGGYGOAbilityAdmissionTestAbility* QueuedOld = GetAdmissionTestAbility(ASC, QueuedOldHandle);
	const FGameplayAbilitySpecHandle QueuedNewHandle = GrantAdmissionTestAbility<UGGYGOAbilityAdmissionQueuedTestAbility>(ASC);
	BlockReason = EGGYGOAbilityGroupBlockReason::NotBlocked;
	if (!TestFalse(TEXT("Queued 组拒绝第二实例"), ASC->TryActivateAbility(QueuedNewHandle))
		|| !TestTrue(TEXT("Queued 组不取消旧实例"), QueuedOld->IsActive())
		|| !TestTrue(TEXT("Queued 原因保留"), ASC->IsActivationBlockedByGroup(
			Cast<UGGYGOGameplayAbility>(ASC->FindAbilitySpecFromHandle(QueuedNewHandle)->Ability), BlockReason))
		|| !TestEqual(TEXT("Queued 拒绝可区分重试"), BlockReason, EGGYGOAbilityGroupBlockReason::GroupOccupiedQueued)) { return false; }
	QueuedOld->FinishForTest();

	const FGameplayTag PassiveGroup = GGYGOGameplayTags::AbilityGroup_Passive;
	GroupConfig->SetRuleForTest(PassiveGroup, EGGYGOAbilityGroupRule::Coexist);
	const FGameplayAbilitySpecHandle CoexistFirstHandle = GrantAdmissionTestAbility<UGGYGOAbilityAdmissionCoexistTestAbility>(ASC);
	const FGameplayAbilitySpecHandle CoexistSecondHandle = GrantAdmissionTestAbility<UGGYGOAbilityAdmissionCoexistTestAbility>(ASC);
	if (!TestTrue(TEXT("Coexist 首实例激活"), ASC->TryActivateAbility(CoexistFirstHandle))
		|| !TestTrue(TEXT("Coexist 次实例并行激活"), ASC->TryActivateAbility(CoexistSecondHandle))
		|| !TestEqual(TEXT("Coexist 保留两个实例"), ASC->GetActiveAbilityCountInGroup(PassiveGroup), 2)) { return false; }
	GetAdmissionTestAbility(ASC, CoexistFirstHandle)->FinishForTest();
	GetAdmissionTestAbility(ASC, CoexistSecondHandle)->FinishForTest();

	// Re-activation inside GAS's synchronous end callback gets a fresh group record.
	GroupConfig->SetRuleForTest(SkillGroup, EGGYGOAbilityGroupRule::SingleInstance, true);
	const FGameplayAbilitySpecHandle ReentryHandle = GrantAdmissionTestAbility<UGGYGOAbilityAdmissionTestAbility>(ASC);
	if (!TestTrue(TEXT("结束重入夹具激活"), ASC->TryActivateAbility(ReentryHandle))) { return false; }
	UGGYGOAbilityAdmissionTestAbility* Reentry = GetAdmissionTestAbility(ASC, ReentryHandle);
	int32 FreedCount = 0;
	bool bReenteredOnAbilityEnded = false;
	const FDelegateHandle EndReentryDelegate = ASC->OnAbilityEnded.AddLambda([&](const FAbilityEndedData& Data)
	{
		if (Data.AbilityThatEnded == Reentry)
		{
			bReenteredOnAbilityEnded = ASC->TryActivateAbility(ReentryHandle);
		}
	});
	FScopedAbilityEndedCallback EndReentryCallback(ASC, EndReentryDelegate);
	const FDelegateHandle FreedDuringReentryDelegate = ASC->OnAbilityGroupFreed.AddLambda([&](FGameplayTag GroupTag)
	{
		if (GroupTag == SkillGroup) { ++FreedCount; }
	});
	FScopedAbilityGroupFreedCallback FreedDuringReentryCallback(ASC, FreedDuringReentryDelegate);
	Reentry->FinishForTest();
	EndReentryCallback.Reset();
	TestTrue(TEXT("OnAbilityEnded 内重新激活同一 Spec"), bReenteredOnAbilityEnded);
	TestEqual(TEXT("结束回调的新登记仍保留"), ASC->GetActiveAbilityCountInGroup(SkillGroup), 1);
	TestEqual(TEXT("新登记存在时抑制旧 freed 广播"), FreedCount, 0);

	// GroupFreed retry observes ActiveCount after GAS decrements it and may re-activate the same Spec.
	bool bRetriedFromFreed = false;
	const FDelegateHandle FreedRetryDelegate = ASC->OnAbilityGroupFreed.AddLambda([&](FGameplayTag GroupTag)
	{
		if (GroupTag == SkillGroup && !bRetriedFromFreed)
		{
			bRetriedFromFreed = ASC->TryActivateAbility(ReentryHandle);
		}
	});
	FScopedAbilityGroupFreedCallback FreedRetryCallback(ASC, FreedRetryDelegate);
	Reentry->FinishForTest();
	TestTrue(TEXT("GroupFreed 回调在 Spec ActiveCount 递减后重试成功"), bRetriedFromFreed);
	TestEqual(TEXT("GroupFreed 重试的新登记保留"), ASC->GetActiveAbilityCountInGroup(SkillGroup), 1);
	FreedRetryCallback.Reset();
	FreedDuringReentryCallback.Reset();
	GetAdmissionTestAbility(ASC, ReentryHandle)->FinishForTest();

	// A higher-priority action started by a cancellation callback invalidates the still-pending newcomer.
	const FGameplayAbilitySpecHandle ReentrantOldHandle = GrantAdmissionTestAbility<UGGYGOAbilityAdmissionTestAbility>(ASC);
	if (!TestTrue(TEXT("取消回调重入旧实例激活"), ASC->TryActivateAbility(ReentrantOldHandle))) { return false; }
	UGGYGOAbilityAdmissionTestAbility* ReentrantOld = GetAdmissionTestAbility(ASC, ReentrantOldHandle);
	const FGameplayAbilitySpecHandle PendingHigherHandle = GrantAdmissionTestAbility<UGGYGOAbilityAdmissionHigherTestAbility>(ASC);
	const FGameplayAbilitySpecHandle ReentrantHighestHandle = GrantAdmissionTestAbility<UGGYGOAbilityAdmissionHighestTestAbility>(ASC);
	bool bActivatedFromCancelCallback = false;
	const FDelegateHandle CancelReentryDelegate = ASC->OnAbilityEnded.AddLambda([&](const FAbilityEndedData& Data)
	{
		if (Data.AbilityThatEnded == ReentrantOld)
		{
			bActivatedFromCancelCallback = ASC->TryActivateAbility(ReentrantHighestHandle);
		}
	});
	FScopedAbilityEndedCallback CancelReentryCallback(ASC, CancelReentryDelegate);
	const bool bPendingHigherTrySucceeded = ASC->TryActivateAbility(PendingHigherHandle);
	CancelReentryCallback.Reset();
	if (!TestTrue(TEXT("高优先级准入完成 PreActivate"), bPendingHigherTrySucceeded)
		|| !TestTrue(TEXT("取消回调内更高动作激活"), bActivatedFromCancelCallback)
		|| !TestFalse(TEXT("被重入取代的 pending 动作未保持活跃"), GetAdmissionTestAbility(ASC, PendingHigherHandle)->IsActive())
		|| !TestEqual(TEXT("被重入取代的 pending 动作未进入业务"), GetAdmissionTestAbility(ASC, PendingHigherHandle)->GetBusinessActivationCountForTest(), 0)
		|| !TestTrue(TEXT("回调中更高动作继续活跃"), GetAdmissionTestAbility(ASC, ReentrantHighestHandle)->IsActive())
		|| !TestEqual(TEXT("取消后组中只有胜出的更高动作"), ASC->GetActiveAbilityCountInGroup(SkillGroup), 1)) { return false; }
	GetAdmissionTestAbility(ASC, ReentrantHighestHandle)->FinishForTest();

	// A per-execution Spec can hold a live old instance and a new pending activation simultaneously.
	// Cancellation callbacks start a third instance of this same Spec; the nested admission must
	// reject only the pending second instance and leave the new winner active.
	const FGameplayAbilitySpecHandle PerExecutionHandle = GrantAdmissionTestAbility<UGGYGOAbilityAdmissionPerExecutionTestAbility>(ASC);
	if (!TestTrue(TEXT("PerExecution 首实例激活"), ASC->TryActivateAbility(PerExecutionHandle))) { return false; }
	FGameplayAbilitySpec* PerExecutionSpec = ASC->FindAbilitySpecFromHandle(PerExecutionHandle);
	if (!TestNotNull(TEXT("PerExecution Spec"), PerExecutionSpec)) { return false; }
	UGGYGOAbilityAdmissionTestAbility* PerExecutionOld = nullptr;
	for (UGameplayAbility* Instance : PerExecutionSpec->GetAbilityInstances())
	{
		UGGYGOAbilityAdmissionTestAbility* Candidate = Cast<UGGYGOAbilityAdmissionTestAbility>(Instance);
		if (Candidate && Candidate->IsActive())
		{
			PerExecutionOld = Candidate;
			break;
		}
	}
	if (!TestNotNull(TEXT("PerExecution 活跃旧实例"), PerExecutionOld)) { return false; }

	bool bPerExecutionReentrySucceeded = false;
	UGGYGOAbilityAdmissionTestAbility* PerExecutionPendingAttempt = nullptr;
	UGGYGOAbilityAdmissionTestAbility* PerExecutionNestedInstance = nullptr;
	int32 PerExecutionCountAtEndCallbackEntry = INDEX_NONE;
	int32 PerExecutionCountAfterNestedTry = INDEX_NONE;
	bool bNestedInstanceActiveAfterNestedTry = false;
	TArray<FGGYGOAbilityAdmissionLifecycleSample> PerExecutionLifecycleSamples;
	FScopedPerExecutionLifecycleObserver PerExecutionLifecycleObserverScope;
	UGGYGOAbilityAdmissionPerExecutionTestAbility::LifecycleObserver.BindLambda(
		[&](const FGGYGOAbilityAdmissionLifecycleSample& Sample)
		{
			// Capture only the two newly activated instances; the original A is not diagnostic here.
			if (Sample.Ability == PerExecutionPendingAttempt || Sample.Ability == PerExecutionNestedInstance)
			{
				PerExecutionLifecycleSamples.Add(Sample);
			}
		});
	const FDelegateHandle PerExecutionActivatedDelegate = ASC->AbilityActivatedCallbacks.AddLambda([&](UGameplayAbility* Ability)
	{
		UGGYGOAbilityAdmissionTestAbility* Candidate = Cast<UGGYGOAbilityAdmissionTestAbility>(Ability);
		if (!Candidate || Candidate == PerExecutionOld)
		{
			return;
		}

		if (!PerExecutionPendingAttempt)
		{
			PerExecutionPendingAttempt = Candidate;
		}
		else if (Candidate != PerExecutionPendingAttempt)
		{
			PerExecutionNestedInstance = Candidate;
		}
	});
	FScopedAbilityActivatedCallback PerExecutionActivatedCallback(ASC, PerExecutionActivatedDelegate);
	const FDelegateHandle PerExecutionReentryDelegate = ASC->OnAbilityEnded.AddLambda([&](const FAbilityEndedData& Data)
	{
		if (Data.AbilityThatEnded == PerExecutionOld)
		{
			if (FGameplayAbilitySpec* CallbackSpec = ASC->FindAbilitySpecFromHandle(PerExecutionHandle))
			{
				PerExecutionCountAtEndCallbackEntry = static_cast<int32>(CallbackSpec->ActiveCount);
			}
			bPerExecutionReentrySucceeded = ASC->TryActivateAbility(PerExecutionHandle);
			if (FGameplayAbilitySpec* CallbackSpec = ASC->FindAbilitySpecFromHandle(PerExecutionHandle))
			{
				PerExecutionCountAfterNestedTry = static_cast<int32>(CallbackSpec->ActiveCount);
			}
			bNestedInstanceActiveAfterNestedTry = PerExecutionNestedInstance && PerExecutionNestedInstance->IsActive();
		}
	});
	FScopedAbilityEndedCallback PerExecutionReentryCallback(ASC, PerExecutionReentryDelegate);
	const bool bPendingPerExecutionTrySucceeded = ASC->TryActivateAbility(PerExecutionHandle);
	PerExecutionReentryCallback.Reset();
	PerExecutionActivatedCallback.Reset();
	PerExecutionLifecycleObserverScope.Reset();
	PerExecutionSpec = ASC->FindAbilitySpecFromHandle(PerExecutionHandle);
	int32 ActivePerExecutionCount = 0;
	UGGYGOAbilityAdmissionTestAbility* PerExecutionWinner = nullptr;
	if (PerExecutionSpec)
	{
		for (UGameplayAbility* Instance : PerExecutionSpec->GetAbilityInstances())
		{
			UGGYGOAbilityAdmissionTestAbility* Candidate = Cast<UGGYGOAbilityAdmissionTestAbility>(Instance);
			if (Candidate && Candidate->IsActive())
			{
				++ActivePerExecutionCount;
				PerExecutionWinner = Candidate;
			}
		}
	}
	const bool bNestedInstanceValidAfterOuterTry = IsValid(PerExecutionNestedInstance);
	const bool bNestedInstanceActiveAfterOuterTry = bNestedInstanceValidAfterOuterTry && PerExecutionNestedInstance->IsActive();
	const bool bNestedInstanceStillInSpec = PerExecutionSpec
		&& PerExecutionSpec->GetAbilityInstances().Contains(PerExecutionNestedInstance);
	int32 PerExecutionLifecycleSampleCount = 0;
	for (const FGGYGOAbilityAdmissionLifecycleSample& Sample : PerExecutionLifecycleSamples)
	{
		++PerExecutionLifecycleSampleCount;
		const TCHAR* AttemptRole = Sample.Ability == PerExecutionPendingAttempt ? TEXT("B") : TEXT("C");
		const TCHAR* ObservationName = TEXT("Unknown");
		switch (Sample.Observation)
		{
		case EGGYGOAbilityAdmissionLifecycleObservation::ActivateBeforeSuper:
			ObservationName = TEXT("ActivateBeforeSuper");
			break;
		case EGGYGOAbilityAdmissionLifecycleObservation::ActivateAfterSuper:
			ObservationName = TEXT("ActivateAfterSuper");
			break;
		case EGGYGOAbilityAdmissionLifecycleObservation::EndBeforeSuper:
			ObservationName = TEXT("EndBeforeSuper");
			break;
		default:
			break;
		}
		AddInfo(FString::Printf(TEXT("Admission lifecycle %s instance=%s@%p ActiveCount=%d IsActive=%d ReplicateEnd=%d bWasCancelled=%d"),
			ObservationName, AttemptRole, static_cast<void*>(Sample.Ability), Sample.SpecActiveCount,
			Sample.bIsActive ? 1 : 0, Sample.bReplicateEndAbility ? 1 : 0, Sample.bWasCancelled ? 1 : 0));
	}
	bool bPerExecutionDiagnosticsPassed = true;
	bPerExecutionDiagnosticsPassed = TestTrue(TEXT("同 Spec 旧实例结束回调内重入成功"), bPerExecutionReentrySucceeded)
		&& bPerExecutionDiagnosticsPassed;
	bPerExecutionDiagnosticsPassed = TestEqual(TEXT("旧实例结束回调入口 ActiveCount 已减为 pending B"), PerExecutionCountAtEndCallbackEntry, 1)
		&& bPerExecutionDiagnosticsPassed;
	bPerExecutionDiagnosticsPassed = TestNotNull(TEXT("激活回调捕获 pending B"), PerExecutionPendingAttempt)
		&& bPerExecutionDiagnosticsPassed;
	bPerExecutionDiagnosticsPassed = TestNotNull(TEXT("激活回调捕获嵌套 C 实例"), PerExecutionNestedInstance)
		&& bPerExecutionDiagnosticsPassed;
	bPerExecutionDiagnosticsPassed = TestTrue(TEXT("嵌套 C 与 pending B 是不同实例"), PerExecutionNestedInstance
		&& PerExecutionNestedInstance != PerExecutionPendingAttempt)
		&& bPerExecutionDiagnosticsPassed;
	bPerExecutionDiagnosticsPassed = TestEqual(TEXT("C PreActivate 返回后 B/C 两实例仍计数"), PerExecutionCountAfterNestedTry, 2)
		&& bPerExecutionDiagnosticsPassed;
	bPerExecutionDiagnosticsPassed = TestTrue(TEXT("C 在旧实例结束回调返回前仍活跃"), bNestedInstanceActiveAfterNestedTry)
		&& bPerExecutionDiagnosticsPassed;
	bPerExecutionDiagnosticsPassed = TestTrue(TEXT("B/C 生命周期观察钩子收到记录"), PerExecutionLifecycleSampleCount >= 4)
		&& bPerExecutionDiagnosticsPassed;
	bPerExecutionDiagnosticsPassed = TestTrue(TEXT("外层 Try 返回后 C 仍是有效 UObject"), bNestedInstanceValidAfterOuterTry)
		&& bPerExecutionDiagnosticsPassed;
	bPerExecutionDiagnosticsPassed = TestTrue(TEXT("外层 Try 返回后 C 仍活跃"), bNestedInstanceActiveAfterOuterTry)
		&& bPerExecutionDiagnosticsPassed;
	bPerExecutionDiagnosticsPassed = TestTrue(TEXT("外层 Try 返回后 C 仍在 Spec 实例列表"), bNestedInstanceStillInSpec)
		&& bPerExecutionDiagnosticsPassed;
	const bool bExistingPerExecutionAssertionsPassed =
		TestTrue(TEXT("PerExecution 第二激活进入 PreActivate"), bPendingPerExecutionTrySucceeded)
		& TestTrue(TEXT("同 Spec 旧实例取消回调重入成功"), bPerExecutionReentrySucceeded)
		& TestEqual(TEXT("pending 的同 Spec 旧尝试安全退出后只留一活跃实例"), ActivePerExecutionCount, 1)
		& TestNotNull(TEXT("同 Spec 重入胜者仍活跃"), PerExecutionWinner)
		& TestEqual(TEXT("被拒 pending 同 Spec 实例没有进入业务"), PerExecutionWinner ? PerExecutionWinner->GetBusinessActivationCountForTest() : -1, 1)
		& TestEqual(TEXT("同 Spec ActiveCount 精确等于唯一胜者"),
			PerExecutionSpec ? static_cast<int32>(PerExecutionSpec->ActiveCount) : INDEX_NONE, 1)
		& TestEqual(TEXT("同 Spec 组登记只留胜者"), ASC->GetActiveAbilityCountInGroup(SkillGroup), 1);
	if (!bPerExecutionDiagnosticsPassed || !bExistingPerExecutionAssertionsPassed) { return false; }
	PerExecutionWinner->FinishForTest();
	PerExecutionSpec = ASC->FindAbilitySpecFromHandle(PerExecutionHandle);
	if (!TestNotNull(TEXT("PerExecution 完成后的 Spec"), PerExecutionSpec)) { return false; }
	TestEqual(TEXT("PerExecution 结束后 ActiveCount 回到零"), static_cast<int32>(PerExecutionSpec->ActiveCount), 0);
	TestFalse(TEXT("PerExecution 结束后 Spec 不活跃"), PerExecutionSpec->IsActive());
	TestEqual(TEXT("PerExecution 结束后组登记归零"), ASC->GetActiveAbilityCountInGroup(SkillGroup), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOAbilityAdmissionInvalidPredictionKeyTest,
	"GGYGO.AbilitySystem.Admission.InvalidPredictionKeyReentry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOAbilityAdmissionInvalidPredictionKeyTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEngine"), GEngine)) { return false; }
	FGGYGOAbilityAdmissionTestWorld TestWorld(GEngine);
	if (!TestNotNull(TEXT("临时测试世界"), TestWorld.World)) { return false; }
	UGGYGOAbilitySystemComponent* ASC = MakeAdmissionTestASC(TestWorld.World);
	if (!TestNotNull(TEXT("真实 GGYGO ASC"), ASC)) { return false; }
	const FGameplayAbilitySpecHandle Handle = GrantAdmissionTestAbility<UGGYGOAbilityAdmissionTestAbility>(ASC);
	UGGYGOAbilityAdmissionTestAbility* Ability = GetAdmissionTestAbility(ASC, Handle);
	if (!TestNotNull(TEXT("无预测键生命周期实例"), Ability)) { return false; }
	bool bRejectedWhileSpecInactive = false;
	{
		const FDelegateHandle ActivatedHandle = ASC->AbilityActivatedCallbacks.AddLambda([&](UGameplayAbility* Activated)
		{
			if (Activated != Ability) { return; }
			const FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromHandle(Handle);
			bRejectedWhileSpecInactive = Spec && Spec->ActiveCount == 0 && Ability->IsActive();
			ASC->CancelAbilitiesByFunc([Ability](const UGGYGOGameplayAbility* Candidate, FGameplayAbilitySpecHandle)
			{
				return Candidate == Ability;
			}, false);
			TestTrue(TEXT("PreActivate 取消只标拒绝，实例仍待正式入口收尾"), Ability->IsActive());
			TestTrue(TEXT("拒绝标记期间 Spec 计数未被误减"), Spec && Spec->ActiveCount == 0);
		});
		ON_SCOPE_EXIT { ASC->AbilityActivatedCallbacks.Remove(ActivatedHandle); };
		TestTrue(TEXT("无预测键尝试走真实 GAS PreActivate"),
			Ability->ActivateWithInvalidPredictionKeyForTest(Handle, ASC->AbilityActorInfo.Get()));
	}
	FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromHandle(Handle);
	if (!TestTrue(TEXT("拒绝发生在 ActiveCount 递增之前"), bRejectedWhileSpecInactive)
		|| !TestFalse(TEXT("本次激活使用无效预测键"), Ability->GetCurrentActivationInfo().GetActivationPredictionKey().IsValidKey())
		|| !TestFalse(TEXT("正式入口消费拒绝并安全结束"), Ability->IsActive())
		|| !TestEqual(TEXT("拒绝尝试未进入业务"), Ability->GetBusinessActivationCountForTest(), 0)
		|| !TestTrue(TEXT("拒绝后 Spec 计数为零"), Spec && Spec->ActiveCount == 0)) { return false; }
	if (!TestTrue(TEXT("同实例复用无效预测键可再次准入"),
		Ability->ActivateWithInvalidPredictionKeyForTest(Handle, ASC->AbilityActorInfo.Get()))
		|| !TestTrue(TEXT("旧拒绝不污染后续激活"), Ability->IsActive())
		|| !TestEqual(TEXT("新尝试进入一次业务"), Ability->GetBusinessActivationCountForTest(), 1)) { return false; }
	bool bReactivated = false;
	const FDelegateHandle EndHandle = ASC->OnAbilityEnded.AddLambda([&](const FAbilityEndedData& Data)
	{
		if (Data.AbilityThatEnded == Ability && !bReactivated)
		{
			bReactivated = Ability->ActivateWithInvalidPredictionKeyForTest(Handle, ASC->AbilityActorInfo.Get());
		}
	});
	FScopedAbilityEndedCallback EndCallback(ASC, EndHandle);
	Ability->FinishForTest();
	EndCallback.Reset();
	Spec = ASC->FindAbilitySpecFromHandle(Handle);
	TestTrue(TEXT("结束回调仍可复用同一个无效预测键"), bReactivated);
	TestTrue(TEXT("旧结束栈保留新实例计数"), Spec && Spec->ActiveCount == 1 && Ability->IsActive());
	TestEqual(TEXT("旧结束栈保留新组登记"), ASC->GetActiveAbilityCountInGroup(GGYGOGameplayTags::AbilityGroup_Skill), 1);
	TestEqual(TEXT("后继激活业务次数正确"), Ability->GetBusinessActivationCountForTest(), 2);
	Ability->FinishForTest();
	Spec = ASC->FindAbilitySpecFromHandle(Handle);
	TestTrue(TEXT("最终清理没有幽灵计数"), Spec && Spec->ActiveCount == 0 && !Spec->IsActive());
	TestEqual(TEXT("最终组清空"), ASC->GetActiveAbilityCountInGroup(GGYGOGameplayTags::AbilityGroup_Skill), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOAbilityAdmissionCorrectionRpcTest,
	"GGYGO.AbilitySystem.Admission.CorrectionRpc",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOAbilityAdmissionCorrectionRpcTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEngine"), GEngine)) { return false; }
	FGGYGOAbilityAdmissionTestWorld TestWorld(GEngine);
	if (!TestNotNull(TEXT("临时测试世界"), TestWorld.World)) { return false; }
	UGGYGOAbilitySystemComponent* ASC = MakeAdmissionTestASC(TestWorld.World);
	if (!TestNotNull(TEXT("真实 GGYGO ASC"), ASC)) { return false; }

	const FGameplayAbilitySpecHandle FirstHandle = GrantAdmissionTestAbility<UGGYGOAbilityAdmissionCorrectionTestAbility>(ASC);
	const FGameplayAbilitySpecHandle SecondHandle = GrantAdmissionTestAbility<UGGYGOAbilityAdmissionCorrectionTestAbility>(ASC);
	if (!TestTrue(TEXT("纠正目标一激活"), ASC->TryActivateAbility(FirstHandle))
		|| !TestTrue(TEXT("纠正目标二激活"), ASC->TryActivateAbility(SecondHandle))) { return false; }
	UGGYGOAbilityAdmissionCorrectionTestAbility* First = GetCorrectionTestAbility(ASC, FirstHandle);
	UGGYGOAbilityAdmissionCorrectionTestAbility* Second = GetCorrectionTestAbility(ASC, SecondHandle);
	if (!TestNotNull(TEXT("纠正目标一实例"), First) || !TestNotNull(TEXT("纠正目标二实例"), Second)) { return false; }
	const FPredictionKey FirstKey = First->GetCurrentActivationInfo().GetActivationPredictionKey();
	const FPredictionKey SecondKey = Second->GetCurrentActivationInfo().GetActivationPredictionKey();
	if (!TestTrue(TEXT("RPC 错 key 夹具拥有两个有效且不同的激活 key"),
		FirstKey.IsValidKey() && SecondKey.IsValidKey() && !(FirstKey == SecondKey))) { return false; }
	FGameplayAbilityTargetDataHandle Correction;
	Correction.Add(new FGameplayAbilityTargetData_LocationInfo());

	ASC->ClientCorrectAbilityState_Implementation(FGameplayAbilitySpecHandle(), FirstKey, Correction);
	ASC->ClientCorrectAbilityState_Implementation(FirstHandle, SecondKey, Correction);
	TestEqual(TEXT("错误 Spec 与错误激活键均不分发"), First->GetCorrectionCountForTest(), 0);
	TestEqual(TEXT("错误 Spec 不会改投其他实例"), Second->GetCorrectionCountForTest(), 0);

	ASC->ClientCorrectAbilityState_Implementation(FirstHandle, FirstKey, Correction);
	TestEqual(TEXT("正确 Spec/key 分发一次"), First->GetCorrectionCountForTest(), 1);
	TestEqual(TEXT("TargetData 按 GAS 原生句柄到达基类扩展点"), First->GetCorrectionTargetCountForTest(), 1);
	TestTrue(TEXT("通用层不要求 Combo 业务载荷类型"), First->DidReceiveLocationCorrectionForTest());

	// A plain project-base ability uses the default no-op safely, and a foreign GA is ignored.
	const FGameplayAbilitySpecHandle BaseTypeHandle = GrantAdmissionTestAbility<UGGYGOAbilityAdmissionTestAbility>(ASC);
	const FGameplayAbilitySpecHandle ForeignHandle = GrantAdmissionTestAbility<UGGYGOAbilityAdmissionForeignTestAbility>(ASC);
	if (!TestTrue(TEXT("基类扩展点夹具激活"), ASC->TryActivateAbility(BaseTypeHandle))
		|| !TestTrue(TEXT("非 GGYGO 能力夹具激活"), ASC->TryActivateAbility(ForeignHandle))) { return false; }
	UGGYGOAbilityAdmissionTestAbility* BaseType = GetAdmissionTestAbility(ASC, BaseTypeHandle);
	FGameplayAbilitySpec* ForeignSpec = ASC->FindAbilitySpecFromHandle(ForeignHandle);
	UGGYGOAbilityAdmissionForeignTestAbility* Foreign = ForeignSpec
		? Cast<UGGYGOAbilityAdmissionForeignTestAbility>(ForeignSpec->GetPrimaryInstance())
		: nullptr;
	if (!TestNotNull(TEXT("基类扩展点实例"), BaseType)) { return false; }
	if (!TestNotNull(TEXT("非 GGYGO 实例"), Foreign)) { return false; }
	const FPredictionKey BaseTypeKey = BaseType->GetCurrentActivationInfo().GetActivationPredictionKey();
	const FPredictionKey ForeignKey = Foreign->GetCurrentActivationInfo().GetActivationPredictionKey();
	ASC->ClientCorrectAbilityState_Implementation(BaseTypeHandle, BaseTypeKey, Correction);
	ASC->ClientCorrectAbilityState_Implementation(ForeignHandle, ForeignKey, Correction);
	TestTrue(TEXT("基类默认 no-op 与非项目 GA 安全分发"), BaseType->IsActive()
		&& Foreign->IsActive() && First->GetCorrectionCountForTest() == 1);

	First->FinishForTest();
	ASC->ClientCorrectAbilityState_Implementation(FirstHandle, FirstKey, Correction);
	TestEqual(TEXT("非活跃实例忽略迟到纠正"), First->GetCorrectionCountForTest(), 1);
	Second->FinishForTest();
	BaseType->FinishForTest();
	Foreign->FinishForTest();
	return true;
}
#endif
