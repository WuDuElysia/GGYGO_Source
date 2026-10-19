/** @file GGYGOMontageTaskLifecycleTest.cpp @brief Montage task rate and ownership regression tests. */
#include "AbilitySystem/Tests/GGYGOMontageTaskTestTypes.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/Tasks/GGYGOAbilityTask_PlayMontageAndWaitForEvent.h"
#include "AbilitySystem/Tasks/GGYGORootMotionScaleLease.h"
#include "Animation/Notifies/GGYGOAnimNotifyState_GameplayEventWindow.h"
#include "System/GGYGOGameplayTags.h"

#include "Animation/ActiveMontageInstanceScope.h"
#include "Animation/AnimNotifyQueue.h"
#include "Animation/AnimComposite.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/Skeleton.h"
#include "AnimNotifies/AnimNotify_PlayMontageNotify.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#include <limits>

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOMontageTaskTestTypes)

UGGYGOMontageTaskTestAbility::UGGYGOMontageTaskTestAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;
	bHasBlueprintActivate = true;
}

void UGGYGOMontageTaskTestAbility::ActivateAbility(FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);
}

bool UGGYGOMontageTaskTestAbility::EndAndReactivateForTest(TFunction<void()> NewActivationAction)
{
	const FGameplayAbilitySpecHandle Handle = CurrentSpecHandle;
	const FGameplayAbilityActorInfo* ActorInfo = CurrentActorInfo;
	const FGameplayAbilityActivationInfo ActivationInfo = CurrentActivationInfo;
	UAbilitySystemComponent* ASC = ActorInfo ? ActorInfo->AbilitySystemComponent.Get() : nullptr;
	if (!Handle.IsValid() || !ActorInfo || !ASC)
	{
		return false;
	}

	SetK2ActivateActionForTest(MoveTemp(NewActivationAction));
	EndAbility(Handle, ActorInfo, ActivationInfo, false, false);
	return ASC->TryActivateAbility(Handle);
}

void UGGYGOMontageTaskTestAbility::FinishForTest()
{
	if (IsActive())
	{
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, false, false);
	}
}

void UGGYGOMontageTaskTestAbility::ProcessEvent(UFunction* Function, void* Parms)
{
	if (Function && Function->GetFName() == GetK2ActivateAbilityFunctionNameForTest()
		&& K2ActivateAction)
	{
		TFunction<void()> Action = MoveTemp(K2ActivateAction);
		Action();
	}
	Super::ProcessEvent(Function, Parms);
}

FName UGGYGOMontageTaskTestAbility::GetK2ActivateAbilityFunctionNameForTest()
{
	return GET_FUNCTION_NAME_CHECKED(UGameplayAbility, K2_ActivateAbility);
}

float UGGYGOMontageTaskTestAnimInstance::Montage_PlayInternal(UAnimMontage* MontageToPlay,
	const FMontageBlendSettings& BlendInSettings, float InPlayRate, EMontagePlayReturnType ReturnValueType,
	float InTimeToStartMontageAt, bool bStopAllMontages)
{
	const float Result = Super::Montage_PlayInternal(MontageToPlay, BlendInSettings, InPlayRate,
		ReturnValueType, InTimeToStartMontageAt, bStopAllMontages);
	if (Result > 0.0f)
	{
		++SuccessfulSuperMontagePlayCount;
	}
	if (AfterSuperMontagePlayAction)
	{
		TFunction<void()> Action = MoveTemp(AfterSuperMontagePlayAction);
		Action();
	}
	return Result;
}

void UGGYGOMontageStartedDiagnosticObserver::ArmForTest(UAnimInstance* AnimInstance,
	UAnimMontage* Montage, TFunction<void()> Action)
{
	DisarmForTest();
	ObservedAnimInstance = AnimInstance;
	ObservedMontage = Montage;
	StartedAction = MoveTemp(Action);
	if (AnimInstance)
	{
		AnimInstance->OnMontageStarted.AddDynamic(this,
			&UGGYGOMontageStartedDiagnosticObserver::HandleMontageStarted);
	}
}

void UGGYGOMontageStartedDiagnosticObserver::DisarmForTest()
{
	if (UAnimInstance* AnimInstance = ObservedAnimInstance.Get())
	{
		AnimInstance->OnMontageStarted.RemoveDynamic(this,
			&UGGYGOMontageStartedDiagnosticObserver::HandleMontageStarted);
	}
	ObservedAnimInstance.Reset();
	ObservedMontage.Reset();
	StartedAction = nullptr;
}

void UGGYGOMontageStartedDiagnosticObserver::HandleMontageStarted(UAnimMontage* Montage)
{
	if (Montage != ObservedMontage.Get()) { return; }
	TFunction<void()> Action = MoveTemp(StartedAction);
	DisarmForTest();
	if (Action) { Action(); }
}

#if WITH_DEV_AUTOMATION_TESTS
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace
{
	struct FScopedMontageTaskTestWorld
	{
		UEngine* Engine = nullptr;
		UWorld* World = nullptr;

		explicit FScopedMontageTaskTestWorld(UEngine* InEngine)
			: Engine(InEngine)
		{
			UWorld::InitializationValues Init;
			Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(false)
				.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
				ERHIFeatureLevel::Num, &Init);
			if (World && Engine)
			{
				Engine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			}
		}

		~FScopedMontageTaskTestWorld()
		{
			if (World)
			{
				// Keep the registered context alive while actors and components tear down.
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

	struct FScopedMontageInstances
	{
		UAnimInstance* AnimInstance = nullptr;
		TArray<FAnimMontageInstance*> Instances;

		explicit FScopedMontageInstances(UAnimInstance* InAnimInstance)
			: AnimInstance(InAnimInstance)
		{}

		FAnimMontageInstance* Add(UAnimMontage* Montage, float PlayRate = 1.0f)
		{
			if (!AnimInstance || !Montage)
			{
				return nullptr;
			}

			FAnimMontageInstance* Instance = new FAnimMontageInstance(AnimInstance);
			Instance->Initialize(Montage);
			Instance->Play(PlayRate);
			AnimInstance->MontageInstances.Add(Instance);
			Instances.Add(Instance);
			return Instance;
		}

		~FScopedMontageInstances()
		{
			for (FAnimMontageInstance* Instance : Instances)
			{
				if (AnimInstance)
				{
					AnimInstance->MontageInstances.Remove(Instance);
				}
				delete Instance;
			}
		}
	};

	struct FScopedActiveMontageTestAbility
	{
		UGGYGOMontageTaskTestAbility* Ability = nullptr;

		~FScopedActiveMontageTestAbility()
		{
			if (IsValid(Ability) && Ability->IsActive())
			{
				Ability->FinishForTest();
			}
		}
	};

	struct FScopedConsoleVariableValue
	{
		IConsoleVariable* Variable = nullptr;
		float OriginalValue = 0.0f;
		EConsoleVariableFlags OriginalSetByPriority = ECVF_SetByConstructor;

		FScopedConsoleVariableValue(IConsoleVariable* InVariable, float TemporaryValue)
			: Variable(InVariable)
		{
			if (Variable)
			{
				OriginalValue = Variable->GetFloat();
				OriginalSetByPriority = static_cast<EConsoleVariableFlags>(Variable->GetFlags() & ECVF_SetByMask);
				Variable->Set(TemporaryValue, OriginalSetByPriority);
			}
		}

		~FScopedConsoleVariableValue()
		{
			if (Variable)
			{
				Variable->Set(OriginalValue, OriginalSetByPriority);
			}
		}
	};

	struct FGGYGOMontageTaskTestFixture
	{
		UWorld* World = nullptr;
		ACharacter* OriginalAvatar = nullptr;
		ACharacter* NewAvatar = nullptr;
		USkeletalMeshComponent* OriginalMesh = nullptr;
		UGGYGOMontageGuardAnimInstance* AnimInstance = nullptr;
		UGGYGOAbilitySystemComponent* ASC = nullptr;
		UGGYGOMontageTaskTestAbility* Ability = nullptr;
		UAnimMontage* Montage = nullptr;
		FGameplayAbilitySpecHandle AbilityHandle;

		bool Initialize(UWorld* InWorld)
		{
			World = InWorld;
			OriginalAvatar = World ? World->SpawnActor<ACharacter>() : nullptr;
			NewAvatar = World ? World->SpawnActor<ACharacter>() : nullptr;
			if (!OriginalAvatar || !NewAvatar) { return false; }

			USkeletalMesh* MeshAsset = LoadObject<USkeletalMesh>(nullptr,
				TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
			if (!MeshAsset) { return false; }
			USkeleton* Skeleton = MeshAsset->GetSkeleton();
			FSkeletalMeshRenderData* MeshRenderData = MeshAsset->GetResourceForRendering();
			if (!Skeleton || !MeshRenderData || MeshRenderData->LODRenderData.IsEmpty()
				|| MeshAsset->GetRefSkeleton().GetRawBoneNum() == 0
				|| MeshAsset->GetRefSkeleton().GetNum() == 0
				|| !Skeleton->IsCompatibleMesh(MeshAsset))
			{
				return false;
			}

			Montage = NewObject<UAnimMontage>(OriginalAvatar);
			if (!Montage) { return false; }
			Montage->SetSkeleton(Skeleton);
			Montage->SetCompositeLength(1.0f);
			if (Montage->SlotAnimTracks.Num() != 1) { return false; }
			FSlotAnimationTrack& SlotTrack = Montage->SlotAnimTracks[0];
			SlotTrack.SlotName = FName(TEXT("DefaultSlot"));
			UAnimComposite* SegmentAsset = NewObject<UAnimComposite>(Montage);
			if (!SegmentAsset) { return false; }
			SegmentAsset->SetSkeleton(Skeleton);
			SegmentAsset->SetCompositeLength(1.0f);
			if (SegmentAsset->GetPlayLength() <= 0.0f) { return false; }
			FAnimSegment Segment;
			Segment.SetAnimReference(SegmentAsset, true);
			SlotTrack.AnimTrack.AnimSegments.Add(Segment);
#if WITH_EDITOR
			if (Montage->AddAnimCompositeSection(TEXT("Main"), 0.0f) == INDEX_NONE
				|| Montage->AddAnimCompositeSection(TEXT("End"), 0.5f) == INDEX_NONE) { return false; }
			Montage->CompositeSections[Montage->GetSectionIndex(TEXT("Main"))].NextSectionName = TEXT("End");
			Montage->CompositeSections[Montage->GetSectionIndex(TEXT("End"))].NextSectionName = NAME_None;
#else
			return false;
#endif
			const FAnimSegment* ValidatedSegment = SlotTrack.AnimTrack.AnimSegments.Num() == 1
				? &SlotTrack.AnimTrack.AnimSegments[0] : nullptr;
			const UAnimSequenceBase* SegmentReference = ValidatedSegment
				? ValidatedSegment->GetAnimReference().Get() : nullptr;
			if (Montage->GetPlayLength() <= 0.0f || Montage->GetSectionIndex(TEXT("Main")) == INDEX_NONE
				|| Montage->GetSectionIndex(TEXT("End")) == INDEX_NONE
				|| Montage->SlotAnimTracks.Num() != 1
				|| SlotTrack.SlotName != FName(TEXT("DefaultSlot"))
				|| !Montage->IsValidSlot(SlotTrack.SlotName)
				|| !ValidatedSegment || !ValidatedSegment->IsValid() || !SegmentReference
				|| SegmentReference->GetSkeleton() != Skeleton
				|| !FMath::IsFinite(ValidatedSegment->GetLength()) || ValidatedSegment->GetLength() <= 0.0f
				|| ValidatedSegment->AnimStartTime < 0.0f
				|| ValidatedSegment->AnimEndTime > SegmentReference->GetPlayLength())
			{
				return false;
			}

			OriginalMesh = OriginalAvatar->GetMesh();
			USkeletalMeshComponent* NewAvatarMesh = NewAvatar->GetMesh();
			if (!OriginalMesh || !NewAvatarMesh || !OriginalMesh->IsRegistered() || !NewAvatarMesh->IsRegistered())
			{
				return false;
			}
			for (USkeletalMeshComponent* Mesh : {OriginalMesh, NewAvatarMesh})
			{
				Mesh->SetComponentTickEnabled(false);
				Mesh->SetVisibility(false);
				Mesh->SetCastShadow(false);
				Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
				Mesh->bEnableUpdateRateOptimizations = true;
				// Use the built-in, prebuilt test mesh so animation initialization has real LOD data.
				Mesh->SetSkinnedAssetAndUpdate(MeshAsset);
				Mesh->SetAnimInstanceClass(UGGYGOGuardedMontageStartedTestAnimInstance::StaticClass());
			}
			AnimInstance = Cast<UGGYGOMontageGuardAnimInstance>(OriginalMesh->GetAnimInstance());
			UGGYGOMontageGuardAnimInstance* NewAvatarAnim = Cast<UGGYGOMontageGuardAnimInstance>(NewAvatarMesh->GetAnimInstance());
			if (!AnimInstance || !AnimInstance->IsInitialized() || !NewAvatarAnim || !NewAvatarAnim->IsInitialized()) { return false; }

			ASC = NewObject<UGGYGOAbilitySystemComponent>(OriginalAvatar);
			if (!ASC) { return false; }
			ASC->RegisterComponent();
			ASC->InitAbilityActorInfo(OriginalAvatar, OriginalAvatar);
			AbilityHandle = ASC->GiveAbility(FGameplayAbilitySpec(UGGYGOMontageTaskTestAbility::StaticClass(), 1));
			FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromHandle(AbilityHandle);
			Ability = Spec ? Cast<UGGYGOMontageTaskTestAbility>(Spec->GetPrimaryInstance()) : nullptr;
			return AbilityHandle.IsValid() && Ability != nullptr;
		}
	};

	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* CreateMontageTask(
		UGGYGOMontageTaskTestAbility* Ability, UAnimMontage* Montage, FName InstanceName,
		float Rate = 1.0f, bool bStopWhenAbilityEnds = true, float RootMotionScale = 1.0f)
	{
		return Ability
			? UGGYGOAbilityTask_PlayMontageAndWaitForEvent::PlayMontageAndWaitForEvent(
				Ability, InstanceName, Montage, FGameplayTagContainer(), Rate, NAME_None, bStopWhenAbilityEnds, RootMotionScale)
			: nullptr;
	}
}

namespace
{
	// Necessary section-source checks reuse this fixture and the existing guarded leaf.
	// The original native/synthetic lifecycle assertions and diagnostic matrix stay intact.
	bool RunGGYGOMontageTaskSectionContract(FAutomationTestBase& Test)
	{
		FScopedMontageTaskTestWorld TestWorld(GEngine);
		if (!Test.TestNotNull(TEXT("Section 测试世界"), TestWorld.World)) { return false; }
		FGGYGOMontageTaskTestFixture SectionFixture;
		if (!Test.TestTrue(TEXT("Section 原实例夹具初始化"), SectionFixture.Initialize(TestWorld.World))) { return false; }
		FScopedActiveMontageTestAbility SectionAbilityCleanup{SectionFixture.Ability};
		SectionFixture.OriginalMesh->SetAnimInstanceClass(UGGYGOGuardedMontageStartedTestAnimInstance::StaticClass());
		UGGYGOMontageGuardAnimInstance* SectionAnim = Cast<UGGYGOMontageGuardAnimInstance>(
			SectionFixture.OriginalMesh->GetAnimInstance());
		if (!Test.TestTrue(TEXT("Section 使用真实已初始化 Guard"), SectionAnim && SectionAnim->IsInitialized())) { return false; }
		UAnimMontage* SectionMontage = SectionFixture.Montage;
		SectionMontage->CompositeSections[SectionMontage->GetSectionIndex(TEXT("Main"))].NextSectionName = TEXT("End");
		SectionMontage->CompositeSections[SectionMontage->GetSectionIndex(TEXT("End"))].NextSectionName = NAME_None;
		SectionMontage->BlendOut.SetBlendTime(0.25f);
		SectionMontage->BlendOutTriggerTime = -1.0f;
		SectionMontage->bEnableAutoBlendOut = true;
		auto StartSectionTask = [&](FName InstanceName, float StartTime,
			UGGYGOAbilityTask_PlayMontageAndWaitForEvent::FNativeCallbacks Callbacks)
		{
			SectionFixture.Ability->FinishForTest();
			UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Started = nullptr;
			SectionFixture.Ability->SetK2ActivateActionForTest(
				[&, InstanceName, StartTime, Callbacks = MoveTemp(Callbacks)]() mutable
				{
					Started = CreateMontageTask(SectionFixture.Ability, SectionMontage, InstanceName);
					if (!Started) { return; }
					Test.TestTrue(TEXT("Section-only native 包可按原契约注册"),
						Started->RegisterNativeCallbacks(MoveTemp(Callbacks)).IsValid());
					Started->SetStartTimeSeconds(StartTime);
					Started->ReadyForActivation();
				});
			Test.TestTrue(TEXT("Section 原能力真实激活"), SectionFixture.ASC->TryActivateAbility(SectionFixture.AbilityHandle));
			Test.TestTrue(TEXT("Section Task 原 Ready 播放成功"), Started && Started->IsActive());
			return Started;
		};

		TArray<FName> SectionOrder;
		FGGYGOMontageSectionFact LastSectionFact;
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent::FNativeCallbacks SectionCallbacks;
		SectionCallbacks.OnBlendOut.BindLambda([&](FGameplayTag, FGameplayEventData) { SectionOrder.Add(TEXT("BlendOut")); });
		SectionCallbacks.SectionReceived.BindLambda([&](const FGGYGOMontageSectionFact& Fact)
		{
			LastSectionFact = Fact;
			SectionOrder.Add(Fact.SectionName);
		});
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent* SectionTask = StartSectionTask(
			TEXT("NaturalSectionFact"), 0.0f, MoveTemp(SectionCallbacks));
		if (!Test.TestNotNull(TEXT("自然转段 Task"), SectionTask)) { return false; }
		FGGYGOMontageSectionSnapshot SectionBefore;
		if (!Test.TestTrue(TEXT("Ready 后原 Main 快照可读"), SectionTask->TryGetOriginalSectionSnapshot(SectionBefore))) { return false; }
		Test.TestEqual(TEXT("初始原区段 Main"), SectionBefore.SectionName, FName(TEXT("Main")));
		Test.TestEqual(TEXT("初始原位置 0"), SectionBefore.PositionSeconds, 0.0f);
		// Real engine tick queues BlendOut before SectionChanged in this same-frame crossing.
		SectionAnim->TickMontageOnly(0.9f / SectionTask->GetEffectivePlayRate());
		Test.TestTrue(TEXT("自然 Main→End 原事实送达"), SectionOrder.Contains(TEXT("End")));
		Test.TestTrue(TEXT("同帧混出后仍接原 Section 事实"), SectionOrder.Contains(TEXT("BlendOut"))
			&& SectionOrder.IndexOfByKey(FName(TEXT("BlendOut"))) < SectionOrder.IndexOfByKey(FName(TEXT("End"))));
		Test.TestEqual(TEXT("Section 事实来自原 Montage"), LastSectionFact.Montage.Get(), SectionMontage);
		Test.TestEqual(TEXT("Section 事实来自原 instance ID"), LastSectionFact.MontageInstanceId, SectionBefore.MontageInstanceId);
		Test.TestFalse(TEXT("自然跨段不是 loop"), LastSectionFact.bLooped);
		FGGYGOMontageSectionSnapshot BlendOutSnapshot;
		Test.TestTrue(TEXT("原混出尾部快照仍可读"), SectionTask->TryGetOriginalSectionSnapshot(BlendOutSnapshot));
		Test.TestTrue(TEXT("尾部快照仍为原实例 End"), BlendOutSnapshot.MontageInstanceId == SectionBefore.MontageInstanceId
			&& BlendOutSnapshot.SectionName == FName(TEXT("End")));
		SectionFixture.Ability->FinishForTest();

		int32 InitialSectionEvents = 0;
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent::FNativeCallbacks InitialCallbacks;
		InitialCallbacks.SectionReceived.BindLambda([&](const FGGYGOMontageSectionFact&) { ++InitialSectionEvents; });
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent* InitialEndTask = StartSectionTask(
			TEXT("InitialEndSnapshot"), 0.75f, MoveTemp(InitialCallbacks));
		if (!Test.TestNotNull(TEXT("非零位置 Task"), InitialEndTask)) { return false; }
		FGGYGOMontageSectionSnapshot InitialEnd;
		if (!Test.TestTrue(TEXT("非零位置 Ready 原快照可读"), InitialEndTask->TryGetOriginalSectionSnapshot(InitialEnd))) { return false; }
		Test.TestEqual(TEXT("初始真实区段 End，不假报 Main"), InitialEnd.SectionName, FName(TEXT("End")));
		Test.TestEqual(TEXT("初始真实位置 0.75"), InitialEnd.PositionSeconds, 0.75f);
		Test.TestEqual(TEXT("原 instance rate 不含资产 RateScale"), InitialEnd.InstancePlayRate, InitialEndTask->GetEffectivePlayRate() / SectionMontage->RateScale);
		Test.TestEqual(TEXT("初始快照未伪造 section-change 事件"), InitialSectionEvents, 0);
		FAnimMontageInstance* InitialInstance = SectionAnim->GetMontageInstanceForID(InitialEnd.MontageInstanceId);
		if (!Test.TestNotNull(TEXT("非零位置精确原实例"), InitialInstance)) { return false; }
		InitialInstance->SetPlayRate(0.0f);
		Test.TestTrue(TEXT("原实例暂停仍可读事实"), InitialEndTask->TryGetOriginalSectionSnapshot(InitialEnd));
		Test.TestEqual(TEXT("零 instance rate 如实报告，不回落名义速率"), InitialEnd.InstancePlayRate, 0.0f);
		SectionFixture.Ability->FinishForTest();
		Test.TestFalse(TEXT("结束 Task 不重放初始快照"), InitialEndTask->TryGetOriginalSectionSnapshot(InitialEnd));
		Test.TestTrue(TEXT("失败快照清空原源"), InitialEnd.Montage == nullptr && InitialEnd.MontageInstanceId == INDEX_NONE);

		int32 RetiredSectionEvents = 0;
		int32 SuccessorSectionEvents = 0;
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent::FNativeCallbacks RetiredCallbacks;
		RetiredCallbacks.SectionReceived.BindLambda([&](const FGGYGOMontageSectionFact&) { ++RetiredSectionEvents; });
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent* RetiredTask = StartSectionTask(
			TEXT("QueuedOriginalSection"), 0.0f, MoveTemp(RetiredCallbacks));
		if (!Test.TestNotNull(TEXT("待退休原 Task"), RetiredTask)) { return false; }
		FGGYGOMontageSectionSnapshot RetiredSnapshot;
		if (!Test.TestTrue(TEXT("退休前原快照"), RetiredTask->TryGetOriginalSectionSnapshot(RetiredSnapshot))) { return false; }
		FAnimMontageInstance* RetiredInstance = SectionAnim->GetMontageInstanceForID(RetiredSnapshot.MontageInstanceId);
		if (!Test.TestNotNull(TEXT("退休前精确原实例"), RetiredInstance)) { return false; }
		// Retain exactly the delegate copy that an engine queued event retains, then dispatch
		// it through the engine queue entry after unregister/owner end and same-asset successor.
		const FQueuedMontageSectionChangedEvent RetiredQueuedFact(SectionMontage,
			RetiredSnapshot.MontageInstanceId, TEXT("End"), false, RetiredInstance->OnMontageSectionChanged);
		RetiredTask->TaskOwnerEnded();
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent::FNativeCallbacks SuccessorCallbacks;
		SuccessorCallbacks.SectionReceived.BindLambda([&](const FGGYGOMontageSectionFact&) { ++SuccessorSectionEvents; });
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent* SectionSuccessor = StartSectionTask(
			TEXT("SameAssetSectionSuccessor"), 0.75f, MoveTemp(SuccessorCallbacks));
		if (!Test.TestNotNull(TEXT("同资产后继 Task"), SectionSuccessor)) { return false; }
		FGGYGOMontageSectionSnapshot SuccessorBefore;
		if (!Test.TestTrue(TEXT("后继原快照"), SectionSuccessor->TryGetOriginalSectionSnapshot(SuccessorBefore))) { return false; }
		SectionAnim->QueueMontageSectionChangedEvent(RetiredQueuedFact);
		// Public native dispatch processes the retained queue without advancing the successor.
		SectionAnim->DispatchQueuedAnimEvents();
		FGGYGOMontageSectionSnapshot SuccessorAfter;
		Test.TestTrue(TEXT("旧队列派发后后继仍可读"), SectionSuccessor->TryGetOriginalSectionSnapshot(SuccessorAfter));
		Test.TestEqual(TEXT("退休原包未收到旧队列"), RetiredSectionEvents, 0);
		Test.TestEqual(TEXT("旧队列未冒认同资产后继"), SuccessorSectionEvents, 0);
		Test.TestTrue(TEXT("后继 instance/区段/位置/rate 保持"), SuccessorAfter.MontageInstanceId != RetiredSnapshot.MontageInstanceId
			&& SuccessorAfter.MontageInstanceId == SuccessorBefore.MontageInstanceId
			&& SuccessorAfter.SectionName == SuccessorBefore.SectionName
			&& SuccessorAfter.PositionSeconds == SuccessorBefore.PositionSeconds
			&& SuccessorAfter.InstancePlayRate == SuccessorBefore.InstancePlayRate);
		SectionFixture.Ability->FinishForTest();

		int32 ReentrantSectionEvents = 0;
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent* ReentrantSectionTask = nullptr;
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent* ReentrantSectionSuccessor = nullptr;
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent::FNativeCallbacks ReentrantCallbacks;
		ReentrantCallbacks.SectionReceived.BindLambda([&](const FGGYGOMontageSectionFact&)
		{
			++ReentrantSectionEvents;
			ReentrantSectionTask->TaskOwnerEnded();
			ReentrantSectionSuccessor = CreateMontageTask(SectionFixture.Ability, SectionMontage, TEXT("SectionCallbackSuccessor"));
			if (ReentrantSectionSuccessor)
			{
				ReentrantSectionSuccessor->SetStartTimeSeconds(0.75f);
				ReentrantSectionSuccessor->ReadyForActivation();
			}
		});
		ReentrantSectionTask = StartSectionTask(TEXT("SectionCallbackOwnerEnd"), 0.0f, MoveTemp(ReentrantCallbacks));
		if (!Test.TestNotNull(TEXT("区段回调重入 Task"), ReentrantSectionTask)) { return false; }
		FGGYGOMontageSectionSnapshot ReentrantBefore;
		if (!Test.TestTrue(TEXT("重入前原快照"), ReentrantSectionTask->TryGetOriginalSectionSnapshot(ReentrantBefore))) { return false; }
		FAnimMontageInstance* ReentrantInstance = SectionAnim->GetMontageInstanceForID(ReentrantBefore.MontageInstanceId);
		if (!Test.TestNotNull(TEXT("重入前原实例"), ReentrantInstance)) { return false; }
		const FQueuedMontageSectionChangedEvent ReentrantQueuedFact(SectionMontage,
			ReentrantBefore.MontageInstanceId, TEXT("End"), false, ReentrantInstance->OnMontageSectionChanged);
		SectionAnim->QueueMontageSectionChangedEvent(ReentrantQueuedFact);
		SectionAnim->DispatchQueuedAnimEvents();
		Test.TestEqual(TEXT("原区段回调仅一次"), ReentrantSectionEvents, 1);
		Test.TestTrue(TEXT("原回调结束原 Task"), ReentrantSectionTask->IsFinished());
		FGGYGOMontageSectionSnapshot ReentrantAfter;
		Test.TestTrue(TEXT("回调后后继保持独立原资源"), ReentrantSectionSuccessor
			&& ReentrantSectionSuccessor->TryGetOriginalSectionSnapshot(ReentrantAfter)
			&& ReentrantAfter.MontageInstanceId != ReentrantBefore.MontageInstanceId
			&& ReentrantAfter.SectionName == FName(TEXT("End")) && ReentrantAfter.PositionSeconds == 0.75f);
		Test.TestFalse(TEXT("重入后原快照资格关闭"), ReentrantSectionTask->TryGetOriginalSectionSnapshot(ReentrantBefore));
		SectionFixture.Ability->FinishForTest();

		return true;
	}
}

namespace
{
	bool RunGGYGOMontageWindowProducerContract(FAutomationTestBase& Test)
	{
		FScopedMontageTaskTestWorld TestWorld(GEngine);
		FGGYGOMontageTaskTestFixture Fixture;
		if (!Test.TestTrue(TEXT("窗口原实例夹具"), Fixture.Initialize(TestWorld.World))) { return false; }
		FScopedActiveMontageTestAbility AbilityCleanup{ Fixture.Ability };
		const FGameplayTag BeginTag = GGYGOGameplayTags::Event_Montage_HitWindowBegin;
		const FGameplayTag EndTag = GGYGOGameplayTags::Event_Montage_HitWindowEnd;
		FGameplayTagContainer Tags;
		Tags.AddTag(BeginTag);
		Tags.AddTag(EndTag);
		auto AddWindow = [&](float Begin, float Duration, bool bQueued)
		{
			auto* Notify = NewObject<UGGYGOAnimNotifyState_GameplayEventWindow>(Fixture.Montage);
			Notify->InitializeEventTags(BeginTag, EndTag);
			Notify->NotifyStateBehaviorFlags = 0;
			FAnimNotifyEvent& Event = Fixture.Montage->Notifies.AddDefaulted_GetRef();
			Event.NotifyStateClass = Notify;
			Event.MontageTickType = bQueued ? EMontageNotifyTickType::Queued : EMontageNotifyTickType::BranchingPoint;
			Event.Link(Fixture.Montage, Begin);
			Event.SetDuration(Duration);
		};
		AddWindow(0.1f, 0.2f, true);
		FGGYGOMontageWindowDeclaration Unique;
		FString Diagnostic;
		if (!Test.TestTrue(TEXT("通用唯一窗口允许原 NoMerge=0"),
			UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolveOriginalMontageWindowDeclaration(
				Fixture.Montage, BeginTag, EndTag, 0.0f, 1.0f, Unique, Diagnostic))) { return false; }
		Test.TestFalse(TEXT("声明保留实际 NoMerge=0"), Unique.bNoMergeOnConcurrentPlay);
		AddWindow(0.5f, 0.2f, false);
		TArray<FGGYGOMontageWindowDeclaration> Declarations;
		if (!Test.TestTrue(TEXT("同 Tag 对的顺序窗口返回完整集合"),
			UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolveOriginalMontageWindows(
				Fixture.Montage, Tags, 0.0f, 1.0f, Declarations, Diagnostic))
			|| !Test.TestEqual(TEXT("两个真实作者窗口"), Declarations.Num(), 2)) { return false; }
		Test.TestFalse(TEXT("顺序条目具有不同本地声明身份"), Declarations[0].HasSameWindow(Declarations[1]));
		Test.TestFalse(TEXT("通用声明保留 BranchingPoint"), Declarations[1].bQueued);
		Test.TestFalse(TEXT("必需唯一入口拒绝同 Tag 多窗"),
			UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolveOriginalMontageWindowDeclaration(
				Fixture.Montage, BeginTag, EndTag, 0.0f, 1.0f, Unique, Diagnostic));
		Test.TestNull(TEXT("唯一解析失败不发布部分声明"), Unique.Montage.Get());
		TArray<FGGYGOMontageWindowDeclaration> Empty;
		FGameplayTagContainer Unused(GGYGOGameplayTags::Event_Montage_ComboWindowBegin);
		Test.TestTrue(TEXT("没有匹配作者窗是明确空集"),
			UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolveOriginalMontageWindows(
				Fixture.Montage, Unused, 0.0f, 1.0f, Empty, Diagnostic));
		Test.TestTrue(TEXT("未猜造订阅词汇对应的窗口"), Empty.IsEmpty());
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = nullptr;
		TArray<FGGYGOMontageWindowFact> Facts;
		bool bNestedReplayCase = false;
		bool bEndTaskInNestedWindow = false;
		TFunction<void(const FGGYGOMontageWindowFact&)> NestedProducerAndReplay;
		bool bPublicPayloadUnchanged = true;
		TOptional<float> ExpectedPublicMagnitude;
		int32 PublicEventCount = 0;
		const FDelegateHandle PublicObserver = Fixture.ASC->AddGameplayEventTagContainerDelegate(Tags,
			FGameplayEventTagMulticastDelegate::FDelegate::CreateLambda([&](FGameplayTag, const FGameplayEventData* Payload)
			{
				++PublicEventCount;
				bPublicPayloadUnchanged &= Payload && Payload->TargetData.Num() == 0
					&& Payload->OptionalObject == Fixture.Montage && Payload->OptionalObject2 == Fixture.OriginalMesh
					&& (!ExpectedPublicMagnitude.IsSet() || Payload->EventMagnitude == ExpectedPublicMagnitude.GetValue());
			}));
		ON_SCOPE_EXIT
		{
			Fixture.ASC->RemoveGameplayEventTagContainerDelegate(Tags, PublicObserver);
			Fixture.Ability->SetK2ActivateActionForTest({});
		};
		Fixture.Ability->SetK2ActivateActionForTest([&]()
		{
			auto* EmptyTask = CreateMontageTask(Fixture.Ability, Fixture.Montage, TEXT("ExplicitEmptyWindows"));
			if (Test.TestNotNull(TEXT("显式空集的 Ready 前 Task"), EmptyTask))
			{
				Test.TestTrue(TEXT("Ready 前可明确配置无作者窗口"), EmptyTask->ConfigureOriginalMontageWindows(Empty, Diagnostic));
				Test.TestFalse(TEXT("显式空集也已消费单次配置"), EmptyTask->ConfigureOriginalMontageWindows(Declarations, Diagnostic));
				EmptyTask->EndTask();
			}
			Task = CreateMontageTask(Fixture.Ability, Fixture.Montage, TEXT("TypedWindowProducer"));
			if (!Task) { return; }
			Test.TestTrue(TEXT("Ready 前一次完整窗口配置"), Task->ConfigureOriginalMontageWindows(Declarations, Diagnostic));
			UGGYGOAbilityTask_PlayMontageAndWaitForEvent::FNativeCallbacks Callbacks;
			Callbacks.WindowReceived.BindLambda([&](const FGGYGOMontageWindowFact& Fact)
			{
				Test.TestTrue(TEXT("同步原窗口事实通过签发 Task 认证"), Task->IsOriginalMontageWindowFact(Fact));
				Facts.Add(Fact);
				if (bEndTaskInNestedWindow && Fact.Declaration.HasSameWindow(Declarations[1]))
				{
					Task->EndTask();
					return;
				}
				if (bNestedReplayCase && Fact.Declaration.HasSameWindow(Declarations[0]))
				{
					bNestedReplayCase = false;
					NestedProducerAndReplay(Fact);
				}
			});
			Test.TestTrue(TEXT("仅 WindowReceived 的 native 包可注册"), Task->RegisterNativeCallbacks(MoveTemp(Callbacks)).IsValid());
			Task->ReadyForActivation();
		});
		if (!Test.TestTrue(TEXT("真实原能力激活窗口 Task"), Fixture.ASC->TryActivateAbility(Fixture.AbilityHandle))
			|| !Test.TestNotNull(TEXT("真实窗口 Task"), Task)) { return false; }
		FGGYGOMontageSectionSnapshot Playback;
		if (!Test.TestTrue(TEXT("原固定实例快照"), Task->TryGetOriginalSectionSnapshot(Playback))) { return false; }
		FGameplayEventData Forged;
		Forged.EventTag = BeginTag;
		Forged.Instigator = Fixture.OriginalAvatar;
		Forged.Target = Fixture.OriginalAvatar;
		Forged.OptionalObject = Fixture.Montage;
		Forged.OptionalObject2 = Fixture.OriginalMesh;
		Forged.EventMagnitude = static_cast<float>(Playback.MontageInstanceId + 1);
		Fixture.ASC->HandleGameplayEvent(BeginTag, &Forged);
		Test.TestTrue(TEXT("普通 GameplayEvent 不能伪造 typed 作者事实"), Facts.IsEmpty());
		auto Emit = [&](int32 Index, bool bBegin, bool bCopyEnd, int32 InstanceId)
		{
			FAnimNotifyEvent Copy = Fixture.Montage->Notifies[Index];
			FAnimNotifyEvent* Event = bCopyEnd ? &Copy : &Fixture.Montage->Notifies[Index];
			auto* Notify = CastChecked<UGGYGOAnimNotifyState_GameplayEventWindow>(Event->NotifyStateClass.Get());
			const TOptional<float> PreviousMagnitude = ExpectedPublicMagnitude;
			if (Event->MontageTickType == EMontageNotifyTickType::BranchingPoint)
			{
				// 原生 BranchingPoint 的 EventReference 不含作者条目或整数实例上下文。
				// 真实身份由原 Payload 承接；公开事件保留旧空引用路径的 Magnitude=0。
				ExpectedPublicMagnitude = 0.0f;
				FBranchingPointNotifyPayload BranchingPayload(Fixture.OriginalMesh, Fixture.Montage,
					Event, InstanceId, !bBegin);
				if (bBegin) { Notify->BranchingPointNotifyBegin(BranchingPayload); }
				else { Notify->BranchingPointNotifyEnd(BranchingPayload); }
			}
			else
			{
				ExpectedPublicMagnitude = static_cast<float>(static_cast<int64>(InstanceId) + 1);
				FAnimNotifyEventReference Reference(Event, Fixture.Montage);
				Reference.AddContextData<UE::Anim::FAnimNotifyMontageInstanceContext>(InstanceId);
				if (bBegin) { Notify->NotifyBegin(Fixture.OriginalMesh, Fixture.Montage, Event->GetDuration(), Reference); }
				else { Notify->NotifyEnd(Fixture.OriginalMesh, Fixture.Montage, Reference); }
			}
			ExpectedPublicMagnitude = PreviousMagnitude;
		};
		Emit(0, true, false, Playback.MontageInstanceId == 0 ? 1 : 0);
		Test.TestTrue(TEXT("错误整数实例不能取得 typed 许可"), Facts.IsEmpty());
		Emit(1, true, false, Playback.MontageInstanceId == 0 ? 1 : 0);
		Test.TestTrue(TEXT("真实 BranchingPoint 错误 Payload 整数实例不能取得 typed 许可"), Facts.IsEmpty());
		Emit(0, true, false, Playback.MontageInstanceId);
		Emit(0, false, true, Playback.MontageInstanceId);
		if (!Test.TestEqual(TEXT("Queued 原引用单独产生 Begin/End"), Facts.Num(), 2)) { return false; }
		const int32 BeforeBranchingPublicEvents = PublicEventCount;
		Emit(1, true, false, Playback.MontageInstanceId);
		Emit(1, false, true, Playback.MontageInstanceId);
		if (!Test.TestEqual(TEXT("真实 BranchingPoint Payload 补齐第二组 Begin/End"), Facts.Num(), 4)) { return false; }
		Test.TestEqual(TEXT("BranchingPoint 每个入口仍只发送一次公开事件"), PublicEventCount, BeforeBranchingPublicEvents + 2);
		Test.TestTrue(TEXT("BranchingPoint 原 int32 实例与实际作者模式被保留"),
			Facts[2].MontageInstanceId == Playback.MontageInstanceId
			&& Facts[3].MontageInstanceId == Playback.MontageInstanceId
			&& !Facts[2].Declaration.bQueued && !Facts[3].Declaration.bQueued);
		Test.TestTrue(TEXT("End 的活动通知副本仍关联原声明"), Facts[0].Declaration.HasSameWindow(Facts[1].Declaration));
		Test.TestFalse(TEXT("下一作者窗不冒认上个声明"), Facts[1].Declaration.HasSameWindow(Facts[2].Declaration));
		Test.TestTrue(TEXT("两组 End phase 明确"), Facts[1].Phase == EGGYGOMontageWindowPhase::End
			&& Facts[3].Phase == EGGYGOMontageWindowPhase::End);
		Test.TestFalse(TEXT("派发后的历史副本不再授权工作"), Task->IsOriginalMontageWindowFact(Facts[0]));
		Test.TestTrue(TEXT("Queued/BranchingPoint 公开 Payload 的原对象、Magnitude 与空 TargetData 保持"), bPublicPayloadUnchanged);
		NestedProducerAndReplay = [&](const FGGYGOMontageWindowFact& OuterFact)
		{
			const int32 BeforeInner = Facts.Num();
			Emit(1, true, false, Playback.MontageInstanceId);
			if (!Test.TestEqual(TEXT("嵌套 B 交付一个新事实"), Facts.Num(), BeforeInner + 1)) { return; }
			const FGGYGOMontageWindowFact InnerFact = Facts.Last();
			Test.TestTrue(TEXT("B 返回后仍在 A 同步回调中，A 资格恢复"), Task->IsOriginalMontageWindowFact(OuterFact));
			Test.TestFalse(TEXT("B 返回后 B 历史副本已失效"), Task->IsOriginalMontageWindowFact(InnerFact));
			// B 已实际退出，A 的原发送 scope 仍存活；公开重发不得再次消费 A。
			Fixture.ASC->HandleGameplayEvent(BeginTag, &Forged);
			Test.TestEqual(TEXT("恢复 A 资格不重新消费其 producer token"), Facts.Num(), BeforeInner + 1);
			Test.TestTrue(TEXT("拒绝重发后 A 仍保有原同步资格"), Task->IsOriginalMontageWindowFact(OuterFact));
		};
		const int32 BeforeNested = Facts.Num();
		bNestedReplayCase = true;
		Emit(0, true, false, Playback.MontageInstanceId);
		if (!Test.TestEqual(TEXT("A→真实 B→公开重发 A 只产生 A/B 两个事实"), Facts.Num(), BeforeNested + 2)) { return false; }
		Test.TestFalse(TEXT("A 完整返回后 A 历史副本失效"), Task->IsOriginalMontageWindowFact(Facts[BeforeNested]));
		Test.TestFalse(TEXT("A 完整返回后 B 历史副本仍失效"), Task->IsOriginalMontageWindowFact(Facts[BeforeNested + 1]));
		Test.TestTrue(TEXT("嵌套 BranchingPoint 返回后仍保持各自公开 Payload"), bPublicPayloadUnchanged);
		FGGYGOMontageWindowDeclaration Ambiguous;
		FGGYGOMontageSectionSnapshot AmbiguousPlayback;
		Test.TestFalse(TEXT("多窗口快照不猜首个同 Tag 条目"),
			Task->TryGetOriginalMontageWindowSnapshot(BeginTag, EndTag, Ambiguous, AmbiguousPlayback));
		const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> OriginalTask(Task);
		NestedProducerAndReplay = [&](const FGGYGOMontageWindowFact& OuterFact)
		{
			Emit(1, true, false, Playback.MontageInstanceId);
			const auto* ClosingTask = OriginalTask.GetEvenIfUnreachable();
			Test.TestFalse(TEXT("B 内 EndTask 返回后不得恢复 A 资格"),
				ClosingTask && ClosingTask->IsOriginalMontageWindowFact(OuterFact));
		};
		const int32 BeforeNestedEnd = Facts.Num();
		bEndTaskInNestedWindow = true;
		bNestedReplayCase = true;
		Emit(0, true, false, Playback.MontageInstanceId);
		const auto* ClosingTask = OriginalTask.GetEvenIfUnreachable();
		if (!Test.TestEqual(TEXT("B 内结束仍只交付原 A/B 两个事实"), Facts.Num(), BeforeNestedEnd + 2)) { return false; }
		Test.TestFalse(TEXT("结束后的旧 A 栈不能重新安装 A 资格"),
			ClosingTask && ClosingTask->IsOriginalMontageWindowFact(Facts[BeforeNestedEnd]));
		Test.TestFalse(TEXT("结束后 B 资格也保持失效"),
			ClosingTask && ClosingTask->IsOriginalMontageWindowFact(Facts[BeforeNestedEnd + 1]));
		if (auto* LiveTask = OriginalTask.Get()) { LiveTask->EndTask(); }
		Fixture.Ability->FinishForTest();
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOMontageTaskLifecycleTest,
	"GGYGO.AbilitySystem.MontageTask.RateLeaseAndLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOMontageTaskLifecycleTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEngine"), GEngine)) { return false; }
	FScopedMontageTaskTestWorld TestWorld(GEngine);
	if (!TestNotNull(TEXT("临时测试世界"), TestWorld.World)) { return false; }

	FGGYGOMontageTaskTestFixture Fixture;
	if (!TestTrue(TEXT("真实 SkeletalMesh/Skeleton/Montage/ASC 夹具初始化"), Fixture.Initialize(TestWorld.World))) { return false; }
	ACharacter* OriginalAvatar = Fixture.OriginalAvatar;
	ACharacter* NewAvatar = Fixture.NewAvatar;
	UGGYGOMontageTaskTestAbility* Ability = Fixture.Ability;
	UGGYGOAbilitySystemComponent* ASC = Fixture.ASC;
	FScopedActiveMontageTestAbility AbilityCleanup{Ability};
	if (!TestNotNull(TEXT("真实 GGYGO ASC"), ASC) || !TestNotNull(TEXT("激活中的测试能力"), Ability)) { return false; }
	if (!TestNotNull(TEXT("注册原 Mesh"), Fixture.OriginalMesh)
		|| !TestNotNull(TEXT("由 Mesh 所有的原 AnimInstance"), Fixture.AnimInstance)
			|| !TestNotNull(TEXT("有效 SkeletalMesh/Skeleton Montage"), Fixture.Montage)) { return false; }
	UAnimMontage* Montage = Fixture.Montage;
	UGGYGOMontageGuardAnimInstance* AnimInstance = Fixture.AnimInstance;
	UGGYGOMontageStartedDiagnosticObserver* StartedObserver = NewObject<UGGYGOMontageStartedDiagnosticObserver>(OriginalAvatar);
	if (!TestNotNull(TEXT("原生创建回调观察器"), StartedObserver)) { return false; }
	struct FScopedLifecycleStartedObserver
	{
		UGGYGOMontageStartedDiagnosticObserver* Observer;
		~FScopedLifecycleStartedObserver() { if (IsValid(Observer)) { Observer->DisarmForTest(); } }
	} StartedObserverCleanup{StartedObserver};
	int32 NativeCreatedPlayCount = 0;

	// Use a non-one engine cvar and restore its value and priority through RAII.
	Montage->RateScale = 0.5f;
	IConsoleVariable* GlobalScaleCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("AbilitySystem.GlobalAbilityScale"));
	if (!TestNotNull(TEXT("AbilitySystem.GlobalAbilityScale cvar"), GlobalScaleCVar)) { return false; }
	const float PreviousGlobalScale = GlobalScaleCVar->GetFloat();
	const EConsoleVariableFlags PreviousGlobalScalePriority =
		static_cast<EConsoleVariableFlags>(GlobalScaleCVar->GetFlags() & ECVF_SetByMask);
	{
		FScopedConsoleVariableValue RestoreGlobalScale(GlobalScaleCVar, 2.0f);
		TestEqual(TEXT("全局速率缩放临时设为 2"), GlobalScaleCVar->GetFloat(), 2.0f);
		float ResolvedTaskRate = 0.0f;
		float ResolvedEffectiveRate = 0.0f;
		TestTrue(TEXT("有效速率可解析"), UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolvePlayRate(
			Montage, 1.5f, ResolvedTaskRate, ResolvedEffectiveRate));
		TestTrue(TEXT("Requested 1.5 乘 Global 2 得 TaskRate 3"), FMath::IsNearlyEqual(ResolvedTaskRate, 3.0f));
		TestTrue(TEXT("Montage RateScale 0.5 得 EffectiveRate 1.5"), FMath::IsNearlyEqual(ResolvedEffectiveRate, 1.5f));

		UGGYGOAbilityTask_PlayMontageAndWaitForEvent* RateTask = CreateMontageTask(Ability, Montage, TEXT("RateSnapshot"), 1.5f);
		if (!TestNotNull(TEXT("速率快照工厂返回 Task"), RateTask)) { return false; }
		TestTrue(TEXT("工厂没有重复施加全局缩放"), FMath::IsNearlyEqual(RateTask->Rate, 3.0f));
		TestTrue(TEXT("Combo 可读 EffectiveRate 快照"),
			FMath::IsNearlyEqual(RateTask->GetEffectivePlayRate(), 1.5f));
		RateTask->EndTask();
	}
	TestEqual(TEXT("全局缩放恢复原值"), GlobalScaleCVar->GetFloat(), PreviousGlobalScale);
	TestEqual(TEXT("全局缩放恢复原优先级"),
		static_cast<EConsoleVariableFlags>(GlobalScaleCVar->GetFlags() & ECVF_SetByMask), PreviousGlobalScalePriority);

	TestTrue(TEXT("实际测试能力可激活"), ASC->TryActivateAbility(Fixture.AbilityHandle));

	const float NaN = std::numeric_limits<float>::quiet_NaN();
	const float Infinity = std::numeric_limits<float>::infinity();
	const TArray<float> InvalidRequestedRates = {0.0f, -1.0f, NaN, Infinity};
	for (const float InvalidRate : InvalidRequestedRates)
	{
		float OutTaskRate = 9.0f;
		float OutEffectiveRate = 9.0f;
		TestFalse(TEXT("零、负数、NaN 与 Infinity 请求速率均拒绝"),
			UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolvePlayRate(
				Montage, InvalidRate, OutTaskRate, OutEffectiveRate));
		TestEqual(TEXT("无效速率清零 TaskRate 输出"), OutTaskRate, 0.0f);
		TestEqual(TEXT("无效速率清零 EffectiveRate 输出"), OutEffectiveRate, 0.0f);
	}
	float IgnoredTaskRate = 0.0f;
	float IgnoredEffectiveRate = 0.0f;
	TestFalse(TEXT("空 Montage 配置拒绝"), UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolvePlayRate(
		nullptr, 1.0f, IgnoredTaskRate, IgnoredEffectiveRate));
	const TArray<float> InvalidMontageScales = {0.0f, -1.0f, NaN, Infinity};
	for (const float InvalidScale : InvalidMontageScales)
	{
		Montage->RateScale = InvalidScale;
		TestFalse(TEXT("零、负数、NaN 与 Infinity Montage RateScale 均拒绝"),
			UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolvePlayRate(
				Montage, 1.0f, IgnoredTaskRate, IgnoredEffectiveRate));
	}
	Montage->RateScale = 0.5f;
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* InvalidTask = CreateMontageTask(
		Ability, Montage, TEXT("InvalidRate"), NaN);
	if (!TestNotNull(TEXT("无效配置仍返回 Task 兼容旧工厂调用"), InvalidTask)) { return false; }
	TestFalse(TEXT("工厂记录无效速率配置"), InvalidTask->bHasValidConfiguration);
	AddExpectedError(TEXT("配置无效"), EAutomationExpectedErrorFlags::Contains, 1);
	InvalidTask->Activate();
	TestTrue(TEXT("无效配置激活时取消并结束 Task"), InvalidTask->IsFinished());

	// Independent root-scale owners prove token transfer, same-value takeover, and baseline restoration.
	OriginalAvatar->SetAnimRootMotionTranslationScale(2.375f);
	NewAvatar->SetAnimRootMotionTranslationScale(0.75f);
	UGGYGOMontageScaleLeaseTestOwner* FirstOwner = NewObject<UGGYGOMontageScaleLeaseTestOwner>(OriginalAvatar);
	UGGYGOMontageScaleLeaseTestOwner* SecondOwner = NewObject<UGGYGOMontageScaleLeaseTestOwner>(OriginalAvatar);
	const uint64 FirstToken = FGGYGORootMotionScaleLease::Acquire(OriginalAvatar, FirstOwner, 0.5f);
	const uint64 SameValueSuccessorToken = FGGYGORootMotionScaleLease::Acquire(OriginalAvatar, SecondOwner, 0.5f);
	TestTrue(TEXT("首次 scale lease 成功"), FirstToken != 0);
	TestTrue(TEXT("同值接管得到不同 token"), SameValueSuccessorToken != 0 && SameValueSuccessorToken != FirstToken);
	TestEqual(TEXT("同值接管不重置当前缩放"), OriginalAvatar->GetAnimRootMotionTranslationScale(), 0.5f);
	TestFalse(TEXT("旧 owner 释放 token 失败"), FGGYGORootMotionScaleLease::Release(OriginalAvatar, FirstOwner, FirstToken));
	TestEqual(TEXT("旧 owner 无权恢复 scale"), OriginalAvatar->GetAnimRootMotionTranslationScale(), 0.5f);
	TestTrue(TEXT("最新 owner 释放成功"), FGGYGORootMotionScaleLease::Release(OriginalAvatar, SecondOwner, SameValueSuccessorToken));
	TestEqual(TEXT("恢复最初的非 1 基线"), OriginalAvatar->GetAnimRootMotionTranslationScale(), 2.375f);

	const uint64 NewAvatarToken = FGGYGORootMotionScaleLease::Acquire(NewAvatar, FirstOwner, 1.25f);
	TestTrue(TEXT("新 Avatar 独立 lease 成功"), NewAvatarToken != 0);
	TestEqual(TEXT("新 Avatar lease 只改自己的 scale"), NewAvatar->GetAnimRootMotionTranslationScale(), 1.25f);
	TestEqual(TEXT("新 Avatar lease 不改原角色"), OriginalAvatar->GetAnimRootMotionTranslationScale(), 2.375f);
	FGGYGORootMotionScaleLease::Release(NewAvatar, FirstOwner, NewAvatarToken);
	TestEqual(TEXT("新 Avatar 也恢复各自原值"), NewAvatar->GetAnimRootMotionTranslationScale(), 0.75f);

	// Real ASC -> ReadyForActivation playback acquires and releases a non-one scale lease.
	OriginalAvatar->SetAnimRootMotionTranslationScale(2.375f);
	Ability->FinishForTest();
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* LiveCancelTask = nullptr;
	Ability->SetK2ActivateActionForTest([&]
	{
		LiveCancelTask = CreateMontageTask(Ability, Montage, TEXT("LiveCancel"), 1.0f, true, 0.35f);
		if (LiveCancelTask) { LiveCancelTask->ReadyForActivation(); }
	});
	TestTrue(TEXT("ASC 激活并由真实 ReadyForActivation 播放 Montage"), ASC->TryActivateAbility(Fixture.AbilityHandle));
	TestNotNull(TEXT("真实播放建立 Task"), LiveCancelTask);
	if (LiveCancelTask)
	{
		TestTrue(TEXT("真实播放 Task 已激活"), LiveCancelTask->IsActive());
		TestEqual(TEXT("真实 Task 获得 0.35 root scale lease"),
			OriginalAvatar->GetAnimRootMotionTranslationScale(), 0.35f);
		LiveCancelTask->ExternalCancel();
		TestTrue(TEXT("真实 ExternalCancel 结束 Task"), LiveCancelTask->IsFinished());
		TestEqual(TEXT("真实取消恢复非 1 的原 root scale"),
			OriginalAvatar->GetAnimRootMotionTranslationScale(), 2.375f);
		TestFalse(TEXT("真实取消停止 Montage"), AnimInstance->Montage_IsActive(Montage));
	}
	Ability->FinishForTest();

	// Real engine Started runs inside Montage_PlayInternal after creation, before the guarded
	// native return. The legacy unguarded after-Super hook is not a legal Task play source.
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* SynchronousEndTask = nullptr;
	Ability->SetK2ActivateActionForTest([&]
	{
		SynchronousEndTask = CreateMontageTask(Ability, Montage, TEXT("SynchronousOwnerEnd"));
		if (SynchronousEndTask) { SynchronousEndTask->ReadyForActivation(); }
	});
	StartedObserver->ArmForTest(AnimInstance, Montage, [&]
	{
		++NativeCreatedPlayCount;
		Ability->FinishForTest();
	});
	const int32 PlayCountBeforeSynchronousEnd = NativeCreatedPlayCount;
	ASC->TryActivateAbility(Fixture.AbilityHandle);
	TestEqual(TEXT("同步结束用例进入真实 Super Montage_PlayInternal"),
		NativeCreatedPlayCount, PlayCountBeforeSynchronousEnd + 1);
	TestFalse(TEXT("真实回调中结束的 ability 保持结束"), Ability->IsActive());
	TestFalse(TEXT("无嵌套后继的同步 TaskOwnerEnded 清理刚创建的孤儿播放"),
		AnimInstance->Montage_IsActive(Montage));
	TestEqual(TEXT("同步结束没有泄漏 root scale"), OriginalAvatar->GetAnimRootMotionTranslationScale(), 2.375f);

	// End+reactivate inside the real montage-play callback. The successor's nested Task is
	// a second actual ReadyForActivation call on the same captured AnimInstance.
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* NestedSuccessorTask = nullptr;
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* OuterReentrantTask = nullptr;
	Ability->SetK2ActivateActionForTest([&]
	{
		OuterReentrantTask = CreateMontageTask(Ability, Montage, TEXT("OuterReentrant"));
		if (OuterReentrantTask)
		{
			AddExpectedError(FString::Printf(
				TEXT("MontageTask \\[%s\\].*ASC 未签发成功的原播放资源；Outcome=%d Reason=%d Guard=%d Stage=%d"),
				*OuterReentrantTask->GetName(), int32(EGGYGOAbilityMontagePlaybackOutcome::Stale),
				int32(EGGYGOAbilityMontagePlaybackReason::GuardSuperseded), int32(EGGYGOMontagePlayGuardOutcome::Superseded),
				int32(EGGYGOMontagePlayGuardNativeStage::Returned)), EAutomationExpectedErrorFlags::Contains, 1);
			OuterReentrantTask->ReadyForActivation();
		}
	});
	bool bReactivatedDuringOuterPlay = false;
	StartedObserver->ArmForTest(AnimInstance, Montage, [&]
	{
		++NativeCreatedPlayCount;
		StartedObserver->ArmForTest(AnimInstance, Montage, [&] { ++NativeCreatedPlayCount; });
		bReactivatedDuringOuterPlay = Ability->EndAndReactivateForTest([&]
		{
			NestedSuccessorTask = CreateMontageTask(Ability, Montage, TEXT("NestedSuccessor"), 1.0f, true, 0.55f);
			if (NestedSuccessorTask) { NestedSuccessorTask->ReadyForActivation(); }
		});
	});
	const int32 PlayCountBeforeNestedReactivation = NativeCreatedPlayCount;
	ASC->TryActivateAbility(Fixture.AbilityHandle);
	TestTrue(TEXT("Montage_PlayInternal 回调同步 End 并重激活成功"), bReactivatedDuringOuterPlay);
	TestTrue(TEXT("被合法后继取代的旧 Task 已明确结束"), OuterReentrantTask && OuterReentrantTask->IsFinished());
	TestEqual(TEXT("外层与嵌套任务均通过真实 Montage_PlayInternal"),
		NativeCreatedPlayCount, PlayCountBeforeNestedReactivation + 2);
	TestNotNull(TEXT("嵌套 successor task 建立"), NestedSuccessorTask);
	if (NestedSuccessorTask)
	{
		TestTrue(TEXT("嵌套 successor task 保持活动"), NestedSuccessorTask->IsActive());
		const FAnimMontageInstance* SuccessorInstance = AnimInstance->GetMontageInstanceForID(
			NestedSuccessorTask->MontageInstanceId);
		TestNotNull(TEXT("successor 的准确 Montage instance 仍存在"), SuccessorInstance);
		TestTrue(TEXT("旧外层结束清理没有停止同资产嵌套后继"),
			SuccessorInstance && SuccessorInstance->IsActive());
		TestEqual(TEXT("嵌套 successor 持有自己的 root scale"),
			OriginalAvatar->GetAnimRootMotionTranslationScale(), 0.55f);
	}
	TestTrue(TEXT("嵌套重入后新 ability 激活仍然有效"), Ability->IsActive());
	Ability->FinishForTest();
	TestEqual(TEXT("结束 successor 后恢复原 root scale"), OriginalAvatar->GetAnimRootMotionTranslationScale(), 2.375f);

	// Obtain each cleanup resource from a real Ready call before invalidating its ActorInfo.
	// A same-asset successor plays on the new Avatar's actual mesh, so both exact instances
	// can coexist legally without manufacturing an engine instance ID or guard identity.
	auto StartOriginalCleanupTask = [&](FName InstanceName, float RootScale)
	{
		Ability->FinishForTest();
		ASC->InitAbilityActorInfo(OriginalAvatar, OriginalAvatar);
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Started = nullptr;
		Ability->SetK2ActivateActionForTest([&, InstanceName, RootScale]
		{
			Started = CreateMontageTask(Ability, Montage, InstanceName, 1.0f, true, RootScale);
			if (Started) { Started->ReadyForActivation(); }
		});
		TestTrue(TEXT("清理夹具原能力真实激活"), ASC->TryActivateAbility(Fixture.AbilityHandle));
		TestTrue(TEXT("清理夹具原 Task 有原生签发身份"), Started && Started->IsActive()
			&& Started->OriginalGuardIdentity.CallId != 0
			&& AnimInstance->IsMontagePlayGuardIdentityCurrent(Started->OriginalGuardIdentity)
			&& Started->GetTaskMontageInstance());
		return Started;
	};
	auto StartNewAvatarSuccessor = [&](FName InstanceName)
	{
		ASC->InitAbilityActorInfo(OriginalAvatar, NewAvatar);
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Successor = CreateMontageTask(
			Ability, Montage, InstanceName, 1.0f, true, 0.75f);
		if (Successor) { Successor->ReadyForActivation(); }
		TestTrue(TEXT("同资产后继在新 Avatar 有真实播放身份"), Successor && Successor->IsActive()
			&& Successor->GetTaskMontageInstance()
			&& Successor->ActivatedAnimInstance.Get() == NewAvatar->GetMesh()->GetAnimInstance());
		return Successor;
	};

	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* EndTaskOwner = StartOriginalCleanupTask(
		TEXT("EndTaskKeepsMontage"), 0.25f);
	if (!TestNotNull(TEXT("EndTask 生命周期夹具"), EndTaskOwner)) { return false; }
	FAnimMontageInstance* OldInstance = EndTaskOwner->GetTaskMontageInstance();
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* SameAssetSuccessorTask = StartNewAvatarSuccessor(TEXT("EndTaskSameAssetSuccessor"));
	FAnimMontageInstance* SameAssetSuccessor = SameAssetSuccessorTask ? SameAssetSuccessorTask->GetTaskMontageInstance() : nullptr;
	if (!TestNotNull(TEXT("旧 Montage instance"), OldInstance)
		|| !TestNotNull(TEXT("同資產新 instance"), SameAssetSuccessor)) { return false; }
	TestTrue(TEXT("ASC 已绑定新 Avatar 与新 AnimInstance"),
		ASC->AbilityActorInfo.IsValid() && ASC->AbilityActorInfo->AvatarActor.Get() == NewAvatar
			&& ASC->AbilityActorInfo->GetAnimInstance() == NewAvatar->GetMesh()->GetAnimInstance());
	TestFalse(TEXT("ActorInfo 已切换到新 Avatar"), EndTaskOwner->IsActivatedActorInfoCurrent());
	EndTaskOwner->EndTask();
	TestTrue(TEXT("普通 EndTask 结束 Task"), EndTaskOwner->IsFinished());
	TestTrue(TEXT("普通 EndTask 不停止捕获的旧 Montage"), OldInstance->IsActive());
	TestTrue(TEXT("普通 EndTask 不停止同资产后继"), SameAssetSuccessor->IsActive());
	TestTrue(TEXT("EndTask 只解绑自己的旧实例委托"),
		OldInstance->OnMontageEnded.GetUObject() == nullptr
			&& OldInstance->OnMontageBlendingOutStarted.GetUObject() == nullptr);
	TestEqual(TEXT("EndTask 释放 scale lease 并恢复原 Avatar 基线"),
		OriginalAvatar->GetAnimRootMotionTranslationScale(), 2.375f);
	TestEqual(TEXT("EndTask 不改新 Avatar scale"), NewAvatar->GetAnimRootMotionTranslationScale(), 0.75f);
	TestTrue(TEXT("清理未覆盖 ASC 的新 Avatar ActorInfo"),
		ASC->AbilityActorInfo.IsValid() && ASC->AbilityActorInfo->AvatarActor.Get() == NewAvatar);

	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* CancelTask = StartOriginalCleanupTask(
		TEXT("CancelExactInstance"), 0.4f);
	if (!TestNotNull(TEXT("取消夹具"), CancelTask)) { return false; }
	OldInstance = CancelTask->GetTaskMontageInstance();
	SameAssetSuccessorTask = StartNewAvatarSuccessor(TEXT("CancelSameAssetSuccessor"));
	SameAssetSuccessor = SameAssetSuccessorTask ? SameAssetSuccessorTask->GetTaskMontageInstance() : nullptr;
	if (!TestNotNull(TEXT("取消前精确旧原实例"), OldInstance)
		|| !TestNotNull(TEXT("取消前新 Avatar 后继实例"), SameAssetSuccessor)) { return false; }
	CancelTask->ExternalCancel();
	TestTrue(TEXT("ExternalCancel 结束 Task"), CancelTask->IsFinished());
	TestTrue(TEXT("Avatar 切换后取消旧 instance"), OldInstance->IsStopped());
	TestTrue(TEXT("取消旧 instance 不停止同资产 successor"), SameAssetSuccessor->IsActive());
	TestEqual(TEXT("取消释放原角色 scale"), OriginalAvatar->GetAnimRootMotionTranslationScale(), 2.375f);
	TestEqual(TEXT("取消不改新 Avatar scale"), NewAvatar->GetAnimRootMotionTranslationScale(), 0.75f);

	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* AbilityEndedTask = StartOriginalCleanupTask(
		TEXT("AbilityEndedExactInstance"), 0.3f);
	if (!TestNotNull(TEXT("能力结束夹具"), AbilityEndedTask)) { return false; }
	SameAssetSuccessor = AbilityEndedTask->GetTaskMontageInstance();
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* NestedSameAssetSuccessorTask = StartNewAvatarSuccessor(TEXT("OwnerEndSameAssetSuccessor"));
	FAnimMontageInstance* NestedSameAssetSuccessor = NestedSameAssetSuccessorTask ? NestedSameAssetSuccessorTask->GetTaskMontageInstance() : nullptr;
	if (!TestNotNull(TEXT("能力结束前原实例"), SameAssetSuccessor)
		|| !TestNotNull(TEXT("嵌套同資產 successor"), NestedSameAssetSuccessor)) { return false; }
	AbilityEndedTask->TaskOwnerEnded();
	TestTrue(TEXT("能力结束清理 Task"), AbilityEndedTask->IsFinished());
	TestTrue(TEXT("AbilityEnded 停止本任务的准确旧 instance"), SameAssetSuccessor->IsStopped());
	TestTrue(TEXT("同资产嵌套后继仍继续播放"), NestedSameAssetSuccessor->IsActive());
	TestEqual(TEXT("AbilityEnded 释放原角色 scale"), OriginalAvatar->GetAnimRootMotionTranslationScale(), 2.375f);
	TestEqual(TEXT("AbilityEnded 不改新 Avatar scale"), NewAvatar->GetAnimRootMotionTranslationScale(), 0.75f);

	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* NaturalEndTask = StartOriginalCleanupTask(
		TEXT("NaturalBlendOut"), 0.6f);
	if (!TestNotNull(TEXT("自然混出夹具"), NaturalEndTask)) { return false; }
	FAnimMontageInstance* NaturalInstance = NaturalEndTask->GetTaskMontageInstance();
	if (!TestNotNull(TEXT("自然结束 Montage instance"), NaturalInstance)) { return false; }
	AnimInstance->TickMontageOnly(0.9f / NaturalEndTask->GetEffectivePlayRate());
	TestEqual(TEXT("自然混出立刻释放 root scale"), OriginalAvatar->GetAnimRootMotionTranslationScale(), 2.375f);
	AnimInstance->TickMontageOnly(0.5f / NaturalEndTask->GetEffectivePlayRate());
	TestTrue(TEXT("自然结束结束 Task"), NaturalEndTask->IsFinished());
	TestEqual(TEXT("自然结束重复释放安全"), OriginalAvatar->GetAnimRootMotionTranslationScale(), 2.375f);
	return RunGGYGOMontageWindowProducerContract(*this);
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOMontageTaskNamedNotifyTest,
	"GGYGO.AbilitySystem.MontageTask.NamedNotifyContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOMontageTaskNamedNotifyTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEngine"), GEngine)) { return false; }
	FScopedMontageTaskTestWorld TestWorld(GEngine);
	FGGYGOMontageTaskTestFixture Fixture;
	if (!TestTrue(TEXT("具名通知原生夹具初始化"), Fixture.Initialize(TestWorld.World))) { return false; }
	FScopedActiveMontageTestAbility Cleanup{Fixture.Ability};
	UAnimMontage* Montage = Fixture.Montage;
	FNameProperty* NameProperty = FindFProperty<FNameProperty>(UAnimNotify_PlayMontageNotify::StaticClass(), TEXT("NotifyName"));
	if (!TestNotNull(TEXT("stock NotifyName 属性"), NameProperty)) { return false; }
	const FName FirstName(TEXT("OriginalPointA"));
	const FName SecondName(TEXT("OriginalPointB"));
	const FName MissingName(TEXT("UnconfiguredPoint"));
	const float FirstPosition = 0.25f;
	const float SecondPosition = Montage->GetAnimCompositeSection(Montage->GetSectionIndex(TEXT("End"))).GetTime();
	const float SecondOffset = -UE_KINDA_SMALL_NUMBER;
	auto AddPoint = [&](FName Name, float Position, float Offset)
	{
		UAnimNotify_PlayMontageNotify* Notify = NewObject<UAnimNotify_PlayMontageNotify>(Montage);
		NameProperty->SetPropertyValue_InContainer(Notify, Name);
		FAnimNotifyEvent& Event = Montage->Notifies.AddDefaulted_GetRef();
		Event.Notify = Notify;
		Event.NotifyName = Name;
		Event.MontageTickType = EMontageNotifyTickType::BranchingPoint;
		Event.Link(Montage, Position);
		Event.TriggerTimeOffset = Offset;
	};
	AddPoint(FirstName, FirstPosition, 0.0f);
	AddPoint(SecondName, SecondPosition, SecondOffset);
	FGGYGOMontageNotifyDeclaration SecondDeclaration;
	FString Diagnostic;
	if (!TestTrue(TEXT("合法 OffsetBefore 原声明可解析"),
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolveOriginalMontageNotifyDeclaration(
			Montage, SecondName, SecondDeclaration, Diagnostic))) { return false; }
	TestEqual(TEXT("声明保留作者位置"), SecondDeclaration.AuthoredPositionSeconds, SecondPosition, 0.0f);
	TestEqual(TEXT("声明保留原 offset"), SecondDeclaration.TriggerTimeOffsetSeconds, SecondOffset, 0.0f);
	TestEqual(TEXT("声明保留实际触发时间"), SecondDeclaration.TriggerPositionSeconds, SecondPosition + SecondOffset, 0.0f);
	TestTrue(TEXT("提前触发时刻不是作者位置"), SecondDeclaration.TriggerPositionSeconds < SecondDeclaration.AuthoredPositionSeconds);
	float LegacyTime = 9.0f;
	TestTrue(TEXT("旧静态接口保持 trigger 语义"),
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolveOriginalMontageNotify(Montage, SecondName, LegacyTime, Diagnostic));
	TestEqual(TEXT("旧输出含 offset"), LegacyTime, SecondDeclaration.TriggerPositionSeconds, 0.0f);

	// 不依赖缓存或告警：真实 marker 碰撞即拒绝，包括未被请求的第三个名字。
	AddPoint(TEXT("OtherPoint"), SecondDeclaration.TriggerPositionSeconds, 0.0f);
	TestFalse(TEXT("不同名字的精确 trigger 碰撞拒绝"),
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolveOriginalMontageNotifyDeclaration(
			Montage, SecondName, SecondDeclaration, Diagnostic));
	TestTrue(TEXT("碰撞诊断定位所选名字"), Diagnostic.Contains(SecondName.ToString()));
	TestTrue(TEXT("声明失败清输出"), SecondDeclaration.NotifyName.IsNone());
	Montage->Notifies.Pop();
	Montage->Notifies[1].MontageTickType = EMontageNotifyTickType::Queued;
	// stock 点由 native 标志成为 BranchingPoint，不能仅凭 Queued 枚举判成普通点。
	TestTrue(TEXT("stock Queued 设置仍按原生 BranchingPoint 认证"),
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolveOriginalMontageNotifyDeclaration(
			Montage, SecondName, SecondDeclaration, Diagnostic));
	UAnimNotify* SecondNotify = Montage->Notifies[1].Notify.Get();
	const bool bOriginalNativeBranchingPoint = SecondNotify->bIsNativeBranchingPoint;
	SecondNotify->bIsNativeBranchingPoint = false;
	TestFalse(TEXT("真正不具 BranchingPoint 资格的点被拒绝"),
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolveOriginalMontageNotifyDeclaration(
			Montage, SecondName, SecondDeclaration, Diagnostic));
	SecondNotify->bIsNativeBranchingPoint = bOriginalNativeBranchingPoint;
	Montage->Notifies[1].MontageTickType = EMontageNotifyTickType::BranchingPoint;
	Montage->RefreshCacheData();
	Montage->BlendIn.SetBlendTime(0.0f);
	Montage->BlendOut.SetBlendTime(0.1f);
	if (!TestTrue(TEXT("配置夹具原能力激活"), Fixture.ASC->TryActivateAbility(Fixture.AbilityHandle))) { return false; }
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* ConfigurationTask = CreateMontageTask(Fixture.Ability, Montage, TEXT("NamedConfiguration"));
	if (!TestNotNull(TEXT("pre-Ready Task"), ConfigurationTask)) { return false; }
	TestFalse(TEXT("空集合拒绝"), ConfigurationTask->ConfigureOriginalMontageNotifies({}, Diagnostic));
	TestFalse(TEXT("None 名字拒绝"), ConfigurationTask->ConfigureOriginalMontageNotifies({FirstName, NAME_None}, Diagnostic));
	TestFalse(TEXT("重复名字拒绝"), ConfigurationTask->ConfigureOriginalMontageNotifies({FirstName, FirstName}, Diagnostic));
	TestFalse(TEXT("缺第二点不部分提交"), ConfigurationTask->ConfigureOriginalMontageNotifies({FirstName, MissingName}, Diagnostic));
	TestTrue(TEXT("失败后可一次提交完整两点"), ConfigurationTask->ConfigureOriginalMontageNotifies({FirstName, SecondName}, Diagnostic));
	TestFalse(TEXT("原集合不能被追加或替换"), ConfigurationTask->ConfigureOriginalMontageNotify(FirstName, Diagnostic));
	ConfigurationTask->EndTask();

	TArray<FGGYGOMontageNotifyFact> Received;
	auto StartTask = [&](FName TaskName, float StartTime, bool bSingle)
	{
		Fixture.Ability->FinishForTest();
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Started = nullptr;
		Fixture.Ability->SetK2ActivateActionForTest([&, TaskName, StartTime, bSingle]()
		{
			Started = CreateMontageTask(Fixture.Ability, Montage, TaskName);
			if (!Started) { return; }
			const bool bConfigured = bSingle ? Started->ConfigureOriginalMontageNotify(FirstName, Diagnostic)
				: Started->ConfigureOriginalMontageNotifies({FirstName, SecondName}, Diagnostic);
			TestTrue(TEXT("Ready 前配置原通知集"), bConfigured);
			if (!bConfigured) { Started->EndTask(); return; }
			UGGYGOAbilityTask_PlayMontageAndWaitForEvent::FNativeCallbacks Callbacks;
			Callbacks.NotifyReceived.BindLambda([&](const FGGYGOMontageNotifyFact& Fact) { Received.Add(Fact); });
			TestTrue(TEXT("一次 native Notify 包注册"), Started->RegisterNativeCallbacks(MoveTemp(Callbacks)).IsValid());
			Started->SetStartTimeSeconds(StartTime);
			Started->ReadyForActivation();
		});
		TestTrue(TEXT("原能力真实 Try 激活"), Fixture.ASC->TryActivateAbility(Fixture.AbilityHandle));
		return Started;
	};
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = StartTask(TEXT("NamedNativePoints"), 0.0f, false);
	if (!TestTrue(TEXT("两点 Task 真实 Ready 播放"), Task && Task->IsActive())) { return false; }
	FGGYGOMontageNotifyFact FirstFact, SecondFact;
	float Position = 9.0f;
	if (!TestTrue(TEXT("第一名字原快照可读"), Task->TryGetOriginalMontageNotifySnapshotByName(FirstName, FirstFact, Position))
		|| !TestTrue(TEXT("第二名字原快照可读"), Task->TryGetOriginalMontageNotifySnapshotByName(SecondName, SecondFact, Position))) { return false; }
	const int32 OriginalId = FirstFact.MontageInstanceId;
	TestTrue(TEXT("两点绑定同一原实例而非第二播放器"), OriginalId != INDEX_NONE && SecondFact.MontageInstanceId == OriginalId);
	TestEqual(TEXT("两点快照保留原 Mesh"), SecondFact.Mesh.Get(), Fixture.OriginalMesh);
	TestEqual(TEXT("第二点保留作者时间"), SecondFact.NotifyAuthoredPositionSeconds, SecondPosition, 0.0f);
	TestEqual(TEXT("第二点保留实际 trigger"), SecondFact.NotifyPositionSeconds, SecondPosition + SecondOffset, 0.0f);
	TestFalse(TEXT("多点旧无名快照不猜第一点"), Task->TryGetOriginalMontageNotifySnapshot(FirstFact, Position));
	TestTrue(TEXT("旧入口失败清 fact"), FirstFact.NotifyName.IsNone() && FirstFact.MontageInstanceId == INDEX_NONE);
	TestEqual(TEXT("旧入口失败清 position"), Position, 0.0f, 0.0f);
	TestFalse(TEXT("未配置名字不能借其它点快照"), Task->TryGetOriginalMontageNotifySnapshotByName(MissingName, FirstFact, Position));
	TestFalse(TEXT("Ready 后不能重新配置"), Task->ConfigureOriginalMontageNotifies({FirstName, SecondName}, Diagnostic));
	Fixture.AnimInstance->TickMontageOnly(0.3f / Task->GetEffectivePlayRate());
	if (!TestEqual(TEXT("真实 native 第一个点仅发一次"), Received.Num(), 1)) { return false; }
	TestEqual(TEXT("第一 native 点名字"), Received[0].NotifyName, FirstName);
	TestEqual(TEXT("第一 native 点精确原 ID"), Received[0].MontageInstanceId, OriginalId);
	if (!Task->TryGetOriginalMontageNotifySnapshotByName(SecondName, SecondFact, Position)) { return false; }
	// 原生步进越过 trigger、仍停在作者边界之前，避免恰落 float 端点掩盖契约。
	const float BeforeAuthoredPosition = (SecondFact.NotifyPositionSeconds + SecondFact.NotifyAuthoredPositionSeconds) * 0.5f;
	Fixture.AnimInstance->TickMontageOnly((BeforeAuthoredPosition - Position) / Task->GetEffectivePlayRate());
	if (!TestEqual(TEXT("真实 native 第二个独立点送达"), Received.Num(), 2)) { return false; }
	TestEqual(TEXT("第二 native 点名字"), Received[1].NotifyName, SecondName);
	TestTrue(TEXT("OffsetBefore 通知送达时仍未到作者边界"),
		Task->TryGetOriginalMontageNotifySnapshotByName(SecondName, SecondFact, Position) && Position < SecondPosition);
	Fixture.AnimInstance->TickMontageOnly(0.002f / Task->GetEffectivePlayRate());
	TestTrue(TEXT("同名字 live 快照随后见证真实边界"),
		Task->TryGetOriginalMontageNotifySnapshotByName(SecondName, SecondFact, Position) && Position >= SecondPosition);
	TestEqual(TEXT("跨作者边界不合成或重发 Notify"), Received.Num(), 2);

	Received.Reset();
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* NonzeroTask = StartTask(TEXT("NamedNonzeroStart"), 0.65f, false);
	if (!TestTrue(TEXT("非零起点 Task Ready"), NonzeroTask && NonzeroTask->IsActive())) { return false; }
	TestEqual(TEXT("跳过原生点不补发通知"), Received.Num(), 0);
	TestTrue(TEXT("非零起点各名字原实例快照可读"),
		NonzeroTask->TryGetOriginalMontageNotifySnapshotByName(FirstName, FirstFact, Position)
		&& NonzeroTask->TryGetOriginalMontageNotifySnapshotByName(SecondName, SecondFact, Position) && Position >= 0.65f);
	// 保持 trigger 不变仍不能偷换作者时间与 offset；另一名字的独立来源仍可认证。
	Montage->Notifies[0].Link(Montage, FirstPosition + 0.015625f);
	Montage->Notifies[0].TriggerTimeOffset = -0.015625f;
	TestEqual(TEXT("来源探针保持原 trigger"), Montage->Notifies[0].GetTriggerTime(), FirstPosition, 0.0f);
	TestFalse(TEXT("作者时间/offset 改变使该原声明失效"),
		NonzeroTask->TryGetOriginalMontageNotifySnapshotByName(FirstName, FirstFact, Position));
	TestTrue(TEXT("独立第二名字不借或依赖第一名字授权"),
		NonzeroTask->TryGetOriginalMontageNotifySnapshotByName(SecondName, SecondFact, Position));
	Montage->Notifies[0].Link(Montage, FirstPosition);
	Montage->Notifies[0].TriggerTimeOffset = 0.0f;
	const int32 NonzeroId = SecondFact.MontageInstanceId;
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Successor = StartTask(TEXT("NamedSameAssetSuccessor"), 0.0f, false);
	if (!TestTrue(TEXT("同资产后继 Ready"), Successor && Successor->IsActive())) { return false; }
	TestFalse(TEXT("旧 Task 不借后继读取任一名字"), NonzeroTask->TryGetOriginalMontageNotifySnapshotByName(SecondName, SecondFact, Position));
	TestTrue(TEXT("后继原名字及新 ID 可读"), Successor->TryGetOriginalMontageNotifySnapshotByName(FirstName, FirstFact, Position)
		&& FirstFact.MontageInstanceId != NonzeroId);
	FBranchingPointNotifyPayload OldPayload(Fixture.OriginalMesh, Montage, &Montage->Notifies[0], NonzeroId);
	Fixture.AnimInstance->OnPlayMontageNotifyBegin.Broadcast(FirstName, OldPayload);
	TestEqual(TEXT("晚到旧 ID 不交付给同资产后继"), Received.Num(), 0);
	Successor->EndTask();
	TestFalse(TEXT("退休 Task 的具名快照关闭"), Successor->TryGetOriginalMontageNotifySnapshotByName(FirstName, FirstFact, Position));
	Fixture.AnimInstance->OnPlayMontageNotifyBegin.Broadcast(FirstName, OldPayload);
	TestEqual(TEXT("清理后原订阅不再派发"), Received.Num(), 0);
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* SingleTask = StartTask(TEXT("NamedLegacySingle"), 0.0f, true);
	if (!TestTrue(TEXT("原单点 Ready"), SingleTask && SingleTask->IsActive())) { return false; }
	TestTrue(TEXT("旧单点无名快照仍可读"), SingleTask->TryGetOriginalMontageNotifySnapshot(FirstFact, Position));
	TestEqual(TEXT("旧单点快照原名字"), FirstFact.NotifyName, FirstName);
	Fixture.Ability->FinishForTest();
	return !HasAnyErrors();
}
#endif

namespace
{
	/** Independent of the task fixture and its after-Super/manual-instance hooks. */
	struct FMontageStartedDiagnosticFixture
	{
		ACharacter* Avatar = nullptr;
		UAnimInstance* AnimInstance = nullptr;
		UGGYGOMontageStartedDiagnosticASC* ASC = nullptr;
		UGGYGOMontageTaskTestAbility* AbilityA = nullptr;
		UGGYGOMontageTaskTestAbility* AbilityB = nullptr;
		UGGYGOMontageStartedDiagnosticObserver* Observer = nullptr;
		UAnimMontage* MontageA = nullptr;
		UAnimMontage* MontageB = nullptr;
		FGameplayAbilitySpecHandle HandleA;
		FGameplayAbilitySpecHandle HandleB;

		~FMontageStartedDiagnosticFixture()
		{
			if (IsValid(Observer)) { Observer->DisarmForTest(); }
			if (IsValid(AbilityA)) { AbilityA->FinishForTest(); }
			if (IsValid(AbilityB)) { AbilityB->FinishForTest(); }
		}

		UAnimMontage* MakeMontage(USkeleton* Skeleton)
		{
			UAnimMontage* Montage = NewObject<UAnimMontage>(Avatar);
			Montage->SetSkeleton(Skeleton);
			Montage->SetCompositeLength(1.0f);
			if (Montage->SlotAnimTracks.Num() != 1) { return nullptr; }
			FSlotAnimationTrack& Slot = Montage->SlotAnimTracks[0];
			Slot.SlotName = TEXT("DefaultSlot");
			UAnimComposite* SegmentAsset = NewObject<UAnimComposite>(Montage);
			SegmentAsset->SetSkeleton(Skeleton);
			SegmentAsset->SetCompositeLength(1.0f);
			FAnimSegment Segment;
			Segment.SetAnimReference(SegmentAsset, true);
			Slot.AnimTrack.AnimSegments.Add(Segment);
#if WITH_EDITOR
			if (Montage->AddAnimCompositeSection(TEXT("Intro"), 0.0f) == INDEX_NONE
				|| Montage->AddAnimCompositeSection(TEXT("OuterStart"), 0.2f) == INDEX_NONE
				|| Montage->AddAnimCompositeSection(TEXT("SuccessorStart"), 0.65f) == INDEX_NONE)
			{
				return nullptr;
			}
#else
			return nullptr;
#endif
			const FAnimSegment& ValidatedSegment = Slot.AnimTrack.AnimSegments[0];
			const UAnimSequenceBase* Reference = ValidatedSegment.GetAnimReference().Get();
			return Montage->IsValidSlot(Slot.SlotName) && ValidatedSegment.IsValid() && Reference
				&& Reference->GetSkeleton() == Skeleton
				&& FMath::IsFinite(ValidatedSegment.GetLength()) && ValidatedSegment.GetLength() > 0.0f
				&& ValidatedSegment.AnimStartTime >= 0.0f
				&& ValidatedSegment.AnimEndTime <= Reference->GetPlayLength()
				&& FMath::IsNearlyEqual(Montage->GetPlayLength(), 1.0f) ? Montage : nullptr;
		}

		bool Initialize(UWorld* World, bool bSameAsset)
		{
			Avatar = World ? World->SpawnActor<ACharacter>() : nullptr;
			if (!Avatar || !Avatar->HasAuthority()) { return false; }
			USkeletalMesh* MeshAsset = LoadObject<USkeletalMesh>(nullptr,
				TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
			USkeleton* Skeleton = MeshAsset ? MeshAsset->GetSkeleton() : nullptr;
			FSkeletalMeshRenderData* RenderData = MeshAsset ? MeshAsset->GetResourceForRendering() : nullptr;
			if (!Skeleton || !RenderData || RenderData->LODRenderData.IsEmpty()
				|| MeshAsset->GetRefSkeleton().GetRawBoneNum() == 0
				|| MeshAsset->GetRefSkeleton().GetNum() == 0
				|| !Skeleton->IsCompatibleMesh(MeshAsset)) { return false; }
			USkeletalMeshComponent* Mesh = Avatar->GetMesh();
			if (!Mesh || !Mesh->IsRegistered()) { return false; }
			Mesh->SetComponentTickEnabled(false);
			Mesh->SetVisibility(false);
			Mesh->SetCastShadow(false);
			Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
			Mesh->bEnableUpdateRateOptimizations = true;
			Mesh->SetSkinnedAssetAndUpdate(MeshAsset);
			Mesh->SetAnimInstanceClass(UAnimInstance::StaticClass());
			AnimInstance = Mesh->GetAnimInstance();
			MontageA = MakeMontage(Skeleton);
			MontageB = bSameAsset ? MontageA : MakeMontage(Skeleton);
			if (!AnimInstance || !MontageA || !MontageB) { return false; }
			ASC = NewObject<UGGYGOMontageStartedDiagnosticASC>(Avatar);
			ASC->RegisterComponent();
			ASC->InitAbilityActorInfo(Avatar, Avatar);
			HandleA = ASC->GiveAbility(FGameplayAbilitySpec(UGGYGOMontageTaskTestAbility::StaticClass(), 1));
			HandleB = ASC->GiveAbility(FGameplayAbilitySpec(UGGYGOMontageTaskTestAbility::StaticClass(), 1));
			const FGameplayAbilitySpec* SpecA = ASC->FindAbilitySpecFromHandle(HandleA);
			const FGameplayAbilitySpec* SpecB = ASC->FindAbilitySpecFromHandle(HandleB);
			AbilityA = SpecA ? Cast<UGGYGOMontageTaskTestAbility>(SpecA->GetPrimaryInstance()) : nullptr;
			AbilityB = SpecB ? Cast<UGGYGOMontageTaskTestAbility>(SpecB->GetPrimaryInstance()) : nullptr;
			Observer = NewObject<UGGYGOMontageStartedDiagnosticObserver>(Avatar);
			return HandleA.IsValid() && HandleB.IsValid() && HandleA != HandleB
				&& AbilityA && AbilityB && AbilityA != AbilityB && Observer;
		}
	};

	struct FMontageStartedDiagnosticSnapshot
	{
		FGameplayAbilityLocalAnimMontage Local;
		FGameplayAbilityRepAnimMontage Rep;
		UAnimMontage* AbilityAMontage = nullptr;
		UAnimMontage* AbilityBMontage = nullptr;
		int32 InstanceId = INDEX_NONE;
		int32 ActiveAssetInstanceId = INDEX_NONE;
		float Position = 0.0f;
		float PlayRate = 0.0f;
		FName Section;
		bool bExactInstanceFound = false;
		bool bInstanceActive = false;
		bool bAbilityAActive = false;
		bool bAbilityBActive = false;

		static FMontageStartedDiagnosticSnapshot Read(const FMontageStartedDiagnosticFixture& Fixture,
			int32 InstanceId)
		{
			FMontageStartedDiagnosticSnapshot Result;
			Result.Local = Fixture.ASC->ReadLocalMontageForTest();
			Result.Rep = Fixture.ASC->ReadRepMontageForTest();
			Result.AbilityAMontage = Fixture.AbilityA->GetCurrentMontage();
			Result.AbilityBMontage = Fixture.AbilityB->GetCurrentMontage();
			Result.bAbilityAActive = Fixture.AbilityA->IsActive();
			Result.bAbilityBActive = Fixture.AbilityB->IsActive();
			if (const FAnimMontageInstance* Active = Fixture.AnimInstance->GetActiveInstanceForMontage(Fixture.MontageB))
			{
				Result.ActiveAssetInstanceId = Active->GetInstanceID();
			}
			// Re-read by captured ID, never treat an asset lookup as the successor identity.
			if (const FAnimMontageInstance* Exact = Fixture.AnimInstance->GetMontageInstanceForID(InstanceId))
			{
				Result.bExactInstanceFound = Exact->Montage == Fixture.MontageB;
				Result.InstanceId = Exact->GetInstanceID();
				Result.bInstanceActive = Exact->IsActive();
				Result.Position = Exact->GetPosition();
				Result.PlayRate = Exact->GetPlayRate();
				Result.Section = Exact->GetCurrentSection();
			}
			return Result;
		}
	};

	bool RunMontageStartedDiagnosticScenario(FAutomationTestBase& Test, bool bSameAsset)
	{
		const FString Label = bSameAsset ? TEXT("SameAssetDifferentSection") : TEXT("DifferentAssets");
		auto Name = [&Label](const TCHAR* Field) { return Label + TEXT(".") + Field; };
		FScopedMontageTaskTestWorld TestWorld(GEngine);
		FMontageStartedDiagnosticFixture Fixture;
		if (!Test.TestTrue(Name(TEXT("Prerequisite.RealAuthorityFixture")), Fixture.Initialize(TestWorld.World, bSameAsset)))
		{
			return false;
		}
		bool bPrerequisites = true;
		bPrerequisites &= Test.TestTrue(Name(TEXT("Prerequisite.RecordReplication")), Fixture.ASC->ShouldRecordMontageReplication());
		bPrerequisites &= Test.TestTrue(Name(TEXT("Prerequisite.NativeAnimInstance")), Fixture.AnimInstance->GetClass() == UAnimInstance::StaticClass());
		bPrerequisites &= Test.TestEqual(Name(TEXT("Prerequisite.AssetRelation")), Fixture.MontageA == Fixture.MontageB, bSameAsset);
		bPrerequisites &= Test.TestTrue(Name(TEXT("Prerequisite.DifferentSections")),
			Fixture.MontageA->GetSectionIndex(TEXT("OuterStart")) != Fixture.MontageB->GetSectionIndex(TEXT("SuccessorStart")));
		if (!bPrerequisites) { return false; }

		int32 StartedCount = 0;
		int32 InstanceAId = INDEX_NONE;
		bool bAActiveAtStarted = false;
		bool bAInstanceActiveAtStarted = false;
		bool bAEndedBeforeB = false;
		bool bBActivated = false;
		bool bBPlayReturned = false;
		bool bBaselineCaptured = false;
		float BDuration = -1.0f;
		float ADuration = -1.0f;
		FMontageStartedDiagnosticSnapshot Baseline;
		Fixture.AbilityB->SetK2ActivateActionForTest([&]()
		{
			BDuration = Fixture.ASC->PlayMontage(Fixture.AbilityB, Fixture.AbilityB->GetCurrentActivationInfo(),
				Fixture.MontageB, 1.25f, TEXT("SuccessorStart"));
			bBPlayReturned = true;
			if (const FAnimMontageInstance* InstanceB = Fixture.AnimInstance->GetActiveInstanceForMontage(Fixture.MontageB))
			{
				Baseline = FMontageStartedDiagnosticSnapshot::Read(Fixture, InstanceB->GetInstanceID());
				bBaselineCaptured = true;
			}
		});
		Fixture.Observer->ArmForTest(Fixture.AnimInstance, Fixture.MontageA, [&]()
		{
			++StartedCount;
			bAActiveAtStarted = Fixture.AbilityA->IsActive();
			if (const FAnimMontageInstance* InstanceA = Fixture.AnimInstance->GetActiveInstanceForMontage(Fixture.MontageA))
			{
				InstanceAId = InstanceA->GetInstanceID();
				bAInstanceActiveAtStarted = InstanceA->IsActive();
			}
			Fixture.AbilityA->FinishForTest();
			bAEndedBeforeB = !Fixture.AbilityA->IsActive();
			bBActivated = Fixture.ASC->TryActivateAbility(Fixture.HandleB);
		});
		Fixture.AbilityA->SetK2ActivateActionForTest([&]()
		{
			ADuration = Fixture.ASC->PlayMontage(Fixture.AbilityA, Fixture.AbilityA->GetCurrentActivationInfo(),
				Fixture.MontageA, 0.75f, TEXT("OuterStart"));
		});
		const bool bAActivated = Fixture.ASC->TryActivateAbility(Fixture.HandleA);
		// No Tick, animation advance, deferred action or state restoration between these observations.
		const FMontageStartedDiagnosticSnapshot After = FMontageStartedDiagnosticSnapshot::Read(Fixture, Baseline.InstanceId);
		Test.AddInfo(FString::Printf(TEXT("%s: Started=%d AInstance=%d BInstance=%d ADuration=%.3f BDuration=%.3f"),
			*Label, StartedCount, InstanceAId, Baseline.InstanceId, ADuration, BDuration));

		bPrerequisites &= Test.TestTrue(Name(TEXT("Prerequisite.AActivationAccepted")), bAActivated);
		bPrerequisites &= Test.TestEqual(Name(TEXT("Prerequisite.RealStartedOnce")), StartedCount, 1);
		bPrerequisites &= Test.TestTrue(Name(TEXT("Prerequisite.AActiveWithCreatedInstance")), bAActiveAtStarted
			&& bAInstanceActiveAtStarted && InstanceAId != INDEX_NONE);
		bPrerequisites &= Test.TestTrue(Name(TEXT("Prerequisite.AEndedBeforeB")), bAEndedBeforeB);
		bPrerequisites &= Test.TestTrue(Name(TEXT("Prerequisite.BActivationAccepted")), bBActivated);
		bPrerequisites &= Test.TestTrue(Name(TEXT("Prerequisite.BSuccessfulPlayReturn")), bBPlayReturned && FMath::IsFinite(BDuration) && BDuration > 0.0f);
		bPrerequisites &= Test.TestTrue(Name(TEXT("Prerequisite.BExactNewInstance")), bBaselineCaptured && Baseline.bExactInstanceFound
			&& Baseline.bInstanceActive && Baseline.InstanceId != InstanceAId && Baseline.ActiveAssetInstanceId == Baseline.InstanceId);
		bPrerequisites &= Test.TestTrue(Name(TEXT("Prerequisite.BLocalAssetAndOwner")), Baseline.Local.AnimMontage.Get() == Fixture.MontageB
			&& Baseline.Local.AnimatingAbility.Get() == Fixture.AbilityB && Baseline.Local.PlayInstanceId != 0);
		bPrerequisites &= Test.TestTrue(Name(TEXT("Prerequisite.BGAMontageAndLifecycle")), Baseline.AbilityAMontage == nullptr
			&& Baseline.AbilityBMontage == Fixture.MontageB && !Baseline.bAbilityAActive && Baseline.bAbilityBActive);
		bPrerequisites &= Test.TestTrue(Name(TEXT("Prerequisite.BSectionPositionAndRate")), Baseline.Section == FName(TEXT("SuccessorStart"))
			&& FMath::IsNearlyEqual(Baseline.Position, 0.65f) && FMath::IsNearlyEqual(Baseline.PlayRate, 1.25f));
		bPrerequisites &= Test.TestTrue(Name(TEXT("Prerequisite.BReplication")), Baseline.Rep.Animation.Get() == Fixture.MontageB
			&& Baseline.Rep.PlayInstanceId != 0
			&& Baseline.Rep.SectionIdToPlay == Fixture.MontageB->GetSectionIndex(TEXT("SuccessorStart")) + 1
			&& FMath::IsNearlyEqual(Baseline.Rep.Position, Baseline.Position)
			&& FMath::IsNearlyEqual(Baseline.Rep.PlayRate, Baseline.PlayRate) && !Baseline.Rep.IsStopped);
		if (!bPrerequisites)
		{
			Test.AddError(Name(TEXT("Prerequisite failed; this result is not evidence of the B4 stale write.")));
			return false;
		}

		// Deliberately strict successor contract: report every mismatch, then run the other scenario.
		Test.AddInfo(FString::Printf(TEXT("%s: B-return -> A-return Local=%s -> %s Owner=%s -> %s Rep=%s -> %s LocalID=%u -> %u RepID=%u -> %u RepSection=%u -> %u BPosition=%.3f -> %.3f BSection=%s -> %s"),
			*Label, *GetNameSafe(Baseline.Local.AnimMontage.Get()), *GetNameSafe(After.Local.AnimMontage.Get()),
			*GetNameSafe(Baseline.Local.AnimatingAbility.Get()), *GetNameSafe(After.Local.AnimatingAbility.Get()),
			*GetNameSafe(Baseline.Rep.Animation.Get()), *GetNameSafe(After.Rep.Animation.Get()),
			uint32(Baseline.Local.PlayInstanceId), uint32(After.Local.PlayInstanceId),
			uint32(Baseline.Rep.PlayInstanceId), uint32(After.Rep.PlayInstanceId),
			uint32(Baseline.Rep.SectionIdToPlay), uint32(After.Rep.SectionIdToPlay),
			Baseline.Position, After.Position, *Baseline.Section.ToString(), *After.Section.ToString()));
		bool bPreserved = true;
		bPreserved &= Test.TestEqual(Name(TEXT("Local.AnimMontage")), After.Local.AnimMontage.Get(), Baseline.Local.AnimMontage.Get());
		bPreserved &= Test.TestEqual(Name(TEXT("Local.AnimatingAbility")), After.Local.AnimatingAbility.Get(), Baseline.Local.AnimatingAbility.Get());
		bPreserved &= Test.TestEqual(Name(TEXT("Local.PlayInstanceId")), After.Local.PlayInstanceId, Baseline.Local.PlayInstanceId);
		bPreserved &= Test.TestEqual(Name(TEXT("GA.A.CurrentMontage")), After.AbilityAMontage, Baseline.AbilityAMontage);
		bPreserved &= Test.TestEqual(Name(TEXT("GA.B.CurrentMontage")), After.AbilityBMontage, Baseline.AbilityBMontage);
		bPreserved &= Test.TestEqual(Name(TEXT("GA.A.Active")), After.bAbilityAActive, Baseline.bAbilityAActive);
		bPreserved &= Test.TestEqual(Name(TEXT("GA.B.Active")), After.bAbilityBActive, Baseline.bAbilityBActive);
		bPreserved &= Test.TestEqual(Name(TEXT("Rep.Animation")), After.Rep.Animation.Get(), Baseline.Rep.Animation.Get());
		bPreserved &= Test.TestEqual(Name(TEXT("Rep.PlayInstanceId")), After.Rep.PlayInstanceId, Baseline.Rep.PlayInstanceId);
		bPreserved &= Test.TestEqual(Name(TEXT("Rep.SectionIdToPlay")), After.Rep.SectionIdToPlay, Baseline.Rep.SectionIdToPlay);
		bPreserved &= Test.TestEqual(Name(TEXT("Rep.Position")), After.Rep.Position, Baseline.Rep.Position);
		bPreserved &= Test.TestEqual(Name(TEXT("Rep.PlayRate")), After.Rep.PlayRate, Baseline.Rep.PlayRate);
		bPreserved &= Test.TestEqual(Name(TEXT("Rep.IsStopped")), bool(After.Rep.IsStopped), bool(Baseline.Rep.IsStopped));
		bPreserved &= Test.TestEqual(Name(TEXT("Rep.NextSectionID")), After.Rep.NextSectionID, Baseline.Rep.NextSectionID);
		bPreserved &= Test.TestEqual(Name(TEXT("Rep.SlotName")), After.Rep.SlotName, Baseline.Rep.SlotName);
		bPreserved &= Test.TestEqual(Name(TEXT("Rep.BlendTime")), After.Rep.BlendTime, Baseline.Rep.BlendTime);
		bPreserved &= Test.TestEqual(Name(TEXT("Rep.BlendOutTime")), After.Rep.BlendOutTime, Baseline.Rep.BlendOutTime);
		bPreserved &= Test.TestEqual(Name(TEXT("Rep.PlayCount")), After.Rep.PlayCount, Baseline.Rep.PlayCount);
		bPreserved &= Test.TestEqual(Name(TEXT("Rep.bRepPosition")), bool(After.Rep.bRepPosition), bool(Baseline.Rep.bRepPosition));
		bPreserved &= Test.TestEqual(Name(TEXT("Rep.SkipPositionCorrection")), bool(After.Rep.SkipPositionCorrection), bool(Baseline.Rep.SkipPositionCorrection));
		bPreserved &= Test.TestEqual(Name(TEXT("Rep.bSkipPlayRate")), bool(After.Rep.bSkipPlayRate), bool(Baseline.Rep.bSkipPlayRate));
		bPreserved &= Test.TestTrue(Name(TEXT("Instance.ExactIdStillFound")), After.bExactInstanceFound);
		bPreserved &= Test.TestEqual(Name(TEXT("Instance.ID")), After.InstanceId, Baseline.InstanceId);
		bPreserved &= Test.TestEqual(Name(TEXT("Instance.ActiveAssetID")), After.ActiveAssetInstanceId, Baseline.ActiveAssetInstanceId);
		bPreserved &= Test.TestEqual(Name(TEXT("Instance.Active")), After.bInstanceActive, Baseline.bInstanceActive);
		bPreserved &= Test.TestEqual(Name(TEXT("Instance.Position")), After.Position, Baseline.Position);
		bPreserved &= Test.TestEqual(Name(TEXT("Instance.Section")), After.Section, Baseline.Section);
		bPreserved &= Test.TestEqual(Name(TEXT("Instance.PlayRate")), After.PlayRate, Baseline.PlayRate);
		return bPreserved;
	}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FMontageStartedReentryDiagnostic,
	"ProjectDiagnostics.B4.MontageStartedReentry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FMontageStartedReentryDiagnostic::GetTests(TArray<FString>& OutBeautifiedNames,
	TArray<FString>& OutTestCommands) const
{
	if (FParse::Param(FCommandLine::Get(), TEXT("GGYGOB4ReentryDiagnostic")))
	{
		OutBeautifiedNames.Add(TEXT("LocalRepGaSectionInstance"));
		OutTestCommands.Add(TEXT("LocalRepGaSectionInstance"));
	}
}

bool FMontageStartedReentryDiagnostic::RunTest(const FString& Parameters)
{
	if (!TestTrue(TEXT("Explicit diagnostic launch flag"), FParse::Param(FCommandLine::Get(), TEXT("GGYGOB4ReentryDiagnostic")))
		|| !TestEqual(TEXT("Explicit diagnostic case"), Parameters, FString(TEXT("LocalRepGaSectionInstance")))
		|| !TestNotNull(TEXT("GEngine"), GEngine)) { return false; }
	bool bPreserved = RunMontageStartedDiagnosticScenario(*this, false);
	bPreserved &= RunMontageStartedDiagnosticScenario(*this, true);
	return bPreserved;
}

namespace
{
	/** Releases only this case's observed instances, after all successor observations. */
	struct FScopedGuardedStartedCleanup
	{
		FMontageStartedDiagnosticFixture& Fixture;
		TWeakObjectPtr<UGGYGOMontageGuardAnimInstance> OriginalAnimInstance;
		const FGGYGOMontagePlayGuardResult& ResultA;
		const FGGYGOMontagePlayGuardResult& ResultB;
		const int32& StartedAId;
		const int32& StartedBId;

		FScopedGuardedStartedCleanup(FMontageStartedDiagnosticFixture& InFixture,
			UGGYGOMontageGuardAnimInstance* InAnimInstance,
			const FGGYGOMontagePlayGuardResult& InResultA, const FGGYGOMontagePlayGuardResult& InResultB,
			const int32& InStartedAId, const int32& InStartedBId)
			: Fixture(InFixture), OriginalAnimInstance(InAnimInstance), ResultA(InResultA), ResultB(InResultB),
			  StartedAId(InStartedAId), StartedBId(InStartedBId)
		{}

		~FScopedGuardedStartedCleanup()
		{
			if (IsValid(Fixture.Observer)) { Fixture.Observer->DisarmForTest(); }
			if (IsValid(Fixture.AbilityA)) { Fixture.AbilityA->SetK2ActivateActionForTest(nullptr); }
			if (IsValid(Fixture.AbilityB)) { Fixture.AbilityB->SetK2ActivateActionForTest(nullptr); }
			UGGYGOMontageGuardAnimInstance* Anim = OriginalAnimInstance.Get();
			USkeletalMeshComponent* Mesh = IsValid(Fixture.Avatar) ? Fixture.Avatar->GetMesh() : nullptr;
			if (Anim && Anim->IsInitialized() && Mesh && Mesh->GetAnimInstance() == Anim
				&& Anim->GetOwningActor() == Fixture.Avatar && Anim->GetSkelMeshComponent() == Mesh)
			{
				// Resolve the exact successor first; asset equality alone cannot authorize a GAS stop.
				const bool bASCStillOwnsB = [&]()
				{
					if (!IsValid(Fixture.ASC) || !Fixture.ASC->AbilityActorInfo.IsValid()
						|| Fixture.ASC->AbilityActorInfo->OwnerActor.Get() != Fixture.Avatar
						|| Fixture.ASC->AbilityActorInfo->AvatarActor.Get() != Fixture.Avatar
						|| Fixture.ASC->AbilityActorInfo->SkeletalMeshComponent.Get() != Mesh
						|| Fixture.ASC->AbilityActorInfo->GetAnimInstance() != Anim
						|| Fixture.ASC->GetCurrentMontage() != Fixture.MontageB
						|| Fixture.ASC->GetAnimatingAbility() != Fixture.AbilityB
						|| ResultB.Identity.CreatedInstanceId == INDEX_NONE
						|| !Anim->IsMontagePlayGuardIdentityCurrent(ResultB.Identity)) { return false; }
					const FAnimMontageInstance* Active = Anim->GetActiveInstanceForMontage(Fixture.MontageB);
					return Active && Active->Montage == Fixture.MontageB && Active->IsActive()
						&& Active->GetInstanceID() == ResultB.Identity.CreatedInstanceId;
				}();
				if (bASCStillOwnsB) { Fixture.ASC->CurrentMontageStop(0.0f); }
				StopExactInstance(ResultB.Identity.CreatedInstanceId, Fixture.MontageB);
				StopExactInstance(StartedBId, Fixture.MontageB);
				StopExactInstance(ResultA.Identity.CreatedInstanceId, Fixture.MontageA);
				StopExactInstance(StartedAId, Fixture.MontageA);
			}
			if (IsValid(Fixture.AbilityA)) { Fixture.AbilityA->FinishForTest(); }
			if (IsValid(Fixture.AbilityB)) { Fixture.AbilityB->FinishForTest(); }
		}

		void StopExactInstance(int32 InstanceId, UAnimMontage* Montage) const
		{
			UGGYGOMontageGuardAnimInstance* Anim = OriginalAnimInstance.Get();
			USkeletalMeshComponent* Mesh = IsValid(Fixture.Avatar) ? Fixture.Avatar->GetMesh() : nullptr;
			if (!Anim || !Anim->IsInitialized() || !Mesh || Mesh->GetAnimInstance() != Anim
				|| Anim->GetOwningActor() != Fixture.Avatar || Anim->GetSkelMeshComponent() != Mesh
				|| InstanceId == INDEX_NONE || !IsValid(Montage)) { return; }
			FMontageBlendSettings BlendOut(Montage->GetBlendOutArgs());
			BlendOut.Blend.BlendTime = 0.0f;
			BlendOut.BlendMode = Montage->BlendModeOut;
			BlendOut.BlendProfile = Montage->BlendProfileOut;
			if (FAnimMontageInstance* Exact = Anim->GetMontageInstanceForID(InstanceId))
			{
				if (Exact->Montage == Montage && Exact->IsActive()) { Exact->Stop(BlendOut); }
				// Stop can call out; never retain or dereference this pointer afterwards.
			}
		}
	};

	bool RunGuardedMontageStartedScenario(FAutomationTestBase& Test, bool bSameAsset)
	{
		const FString Label = bSameAsset ? TEXT("SameAssetDifferentSection") : TEXT("DifferentAssets");
		auto Name = [&Label](const TCHAR* Field) { return Label + TEXT(".") + Field; };
		FScopedMontageTaskTestWorld TestWorld(GEngine);
		FMontageStartedDiagnosticFixture Fixture;
		if (!Test.TestTrue(Name(TEXT("Prerequisite.RealAuthorityFixture")), Fixture.Initialize(TestWorld.World, bSameAsset)))
		{
			return false;
		}
		USkeletalMeshComponent* Mesh = Fixture.Avatar->GetMesh();
		// This case alone changes its transient mesh. The original native diagnostic initializer stays intact.
		Mesh->SetAnimInstanceClass(UGGYGOGuardedMontageStartedTestAnimInstance::StaticClass());
		Fixture.AnimInstance = Mesh->GetAnimInstance();
		UGGYGOMontageGuardAnimInstance* Guard = Cast<UGGYGOMontageGuardAnimInstance>(Fixture.AnimInstance);
		bool bPrerequisites = true;
		bPrerequisites &= Test.TestTrue(Name(TEXT("Prerequisite.RegisteredGuardInitialized")), Mesh->IsRegistered()
			&& Guard && Guard->IsInitialized()
			&& Guard->GetClass() == UGGYGOGuardedMontageStartedTestAnimInstance::StaticClass());
		bPrerequisites &= Test.TestTrue(Name(TEXT("Prerequisite.OriginalActorInfoAndAnim")), Guard
			&& Fixture.ASC->AbilityActorInfo.IsValid()
			&& Fixture.ASC->AbilityActorInfo->OwnerActor.Get() == Fixture.Avatar
			&& Fixture.ASC->AbilityActorInfo->AvatarActor.Get() == Fixture.Avatar
			&& Fixture.ASC->AbilityActorInfo->SkeletalMeshComponent.Get() == Mesh
			&& Fixture.ASC->AbilityActorInfo->GetAnimInstance() == Guard
			&& Guard->GetOwningActor() == Fixture.Avatar && Guard->GetSkelMeshComponent() == Mesh);
		bPrerequisites &= Test.TestTrue(Name(TEXT("Prerequisite.AuthorityAndRecordReplication")),
			Fixture.Avatar->HasAuthority() && Fixture.ASC->ShouldRecordMontageReplication());
		bPrerequisites &= Test.TestEqual(Name(TEXT("Prerequisite.AssetRelation")), Fixture.MontageA == Fixture.MontageB, bSameAsset);
		bPrerequisites &= Test.TestTrue(Name(TEXT("Prerequisite.SameGroupDifferentSections")),
			!Fixture.MontageA->GetGroupName().IsNone()
			&& Fixture.MontageA->GetGroupName() == Fixture.MontageB->GetGroupName()
			&& Fixture.MontageA->GetSectionIndex(TEXT("OuterStart")) != INDEX_NONE
			&& Fixture.MontageB->GetSectionIndex(TEXT("SuccessorStart")) != INDEX_NONE
			&& Fixture.MontageA->GetSectionIndex(TEXT("OuterStart")) != Fixture.MontageB->GetSectionIndex(TEXT("SuccessorStart")));
		if (!bPrerequisites) { return false; }

		FGGYGOMontagePlayGuardResult ResultA;
		FGGYGOMontagePlayGuardResult ResultB;
		int32 StartedAId = INDEX_NONE;
		int32 StartedBId = INDEX_NONE;
		FScopedGuardedStartedCleanup Cleanup(Fixture, Guard, ResultA, ResultB, StartedAId, StartedBId);
		int32 AStartedCount = 0;
		int32 BStartedCount = 0;
		bool bAActiveAtStarted = false;
		bool bAInstancePlayingAtStarted = false;
		bool bBActiveAtStarted = false;
		bool bBInstancePlayingAtStarted = false;
		bool bAEndedBeforeB = false;
		bool bBActivated = false;
		bool bBPlayReturned = false;
		bool bBaselineCaptured = false;
		bool bBaselineExactBPlaying = false;
		float ADuration = -1.0f;
		float BDuration = -1.0f;
		FMontageStartedDiagnosticSnapshot Baseline;
		Fixture.AbilityB->SetK2ActivateActionForTest([&]()
		{
			BDuration = Fixture.ASC->PlayMontageWithGuard(Fixture.AbilityB, Fixture.AbilityB->GetCurrentActivationInfo(),
				Fixture.MontageB, 1.25f, TEXT("SuccessorStart"), 0.0f, ResultB, nullptr);
			bBPlayReturned = true;
			if (ResultB.Identity.CreatedInstanceId != INDEX_NONE)
			{
				Baseline = FMontageStartedDiagnosticSnapshot::Read(Fixture, ResultB.Identity.CreatedInstanceId);
				bBaselineCaptured = true;
				const FAnimMontageInstance* Exact = Guard->GetMontageInstanceForID(ResultB.Identity.CreatedInstanceId);
				bBaselineExactBPlaying = Exact && Exact->Montage == Fixture.MontageB && Exact->IsActive() && Exact->IsPlaying();
			}
		});
		Fixture.Observer->ArmForTest(Guard, Fixture.MontageA, [&]()
		{
			++AStartedCount;
			bAActiveAtStarted = Fixture.AbilityA->IsActive();
			if (const FAnimMontageInstance* InstanceA = Guard->GetActiveInstanceForMontage(Fixture.MontageA))
			{
				StartedAId = InstanceA->GetInstanceID();
				bAInstancePlayingAtStarted = InstanceA->IsActive() && InstanceA->IsPlaying();
			}
			Fixture.AbilityA->FinishForTest();
			bAEndedBeforeB = !Fixture.AbilityA->IsActive();
			// The one-shot A observer has already unbound. Rearm only after A ends and before B activates.
			Fixture.Observer->ArmForTest(Guard, Fixture.MontageB, [&]()
			{
				++BStartedCount;
				bBActiveAtStarted = Fixture.AbilityB->IsActive();
				if (const FAnimMontageInstance* InstanceB = Guard->GetActiveInstanceForMontage(Fixture.MontageB))
				{
					StartedBId = InstanceB->GetInstanceID();
					bBInstancePlayingAtStarted = InstanceB->IsActive() && InstanceB->IsPlaying();
				}
			});
			bBActivated = Fixture.ASC->TryActivateAbility(Fixture.HandleB);
		});
		Fixture.AbilityA->SetK2ActivateActionForTest([&]()
		{
			ADuration = Fixture.ASC->PlayMontageWithGuard(Fixture.AbilityA, Fixture.AbilityA->GetCurrentActivationInfo(),
				Fixture.MontageA, 0.75f, TEXT("OuterStart"), 0.0f, ResultA, nullptr);
		});
		const bool bAActivated = Fixture.ASC->TryActivateAbility(Fixture.HandleA);
		// Both snapshots use B's typed exact identity; no animation advance or state restoration.
		const FMontageStartedDiagnosticSnapshot After = FMontageStartedDiagnosticSnapshot::Read(Fixture, ResultB.Identity.CreatedInstanceId);
		bool bAfterExactBPlaying = false;
		if (const FAnimMontageInstance* Exact = Guard->GetMontageInstanceForID(ResultB.Identity.CreatedInstanceId))
		{
			bAfterExactBPlaying = Exact->Montage == Fixture.MontageB && Exact->IsActive() && Exact->IsPlaying();
		}
		Test.AddInfo(FString::Printf(TEXT("%s: Started A/B=%d/%d ObservedID A/B=%d/%d ResultID A/B=%d/%d Call A/B=%llu/%llu Generation A/B=%llu/%llu Outcome A/B=%u/%u Stage A/B=%u/%u Superseding A/B=%llu/%llu PublicReturn A/B=%.3f/%.3f NativeReturn A/B=%.3f/%.3f CallerReturn A/B=%.3f/%.3f"),
			*Label, AStartedCount, BStartedCount, StartedAId, StartedBId,
			ResultA.Identity.CreatedInstanceId, ResultB.Identity.CreatedInstanceId,
			ResultA.Identity.CallId, ResultB.Identity.CallId, ResultA.Identity.LifecycleGeneration, ResultB.Identity.LifecycleGeneration,
			uint32(ResultA.Outcome), uint32(ResultB.Outcome), uint32(ResultA.NativeStage), uint32(ResultB.NativeStage),
			ResultA.SupersedingCallId, ResultB.SupersedingCallId, ADuration, BDuration,
			ResultA.GuardedNativeReturnValue, ResultB.GuardedNativeReturnValue, ResultA.CallerReturnValue, ResultB.CallerReturnValue));

		bPrerequisites &= Test.TestTrue(Name(TEXT("Prerequisite.AActivationAccepted")), bAActivated);
		bPrerequisites &= Test.TestEqual(Name(TEXT("Prerequisite.RealAStartedOnce")), AStartedCount, 1);
		bPrerequisites &= Test.TestEqual(Name(TEXT("Prerequisite.RealBStartedOnce")), BStartedCount, 1);
		bPrerequisites &= Test.TestTrue(Name(TEXT("Prerequisite.AActiveWithCreatedInstance")), bAActiveAtStarted
			&& bAInstancePlayingAtStarted && StartedAId != INDEX_NONE);
		bPrerequisites &= Test.TestTrue(Name(TEXT("Prerequisite.AEndedBeforeB")), bAEndedBeforeB);
		bPrerequisites &= Test.TestTrue(Name(TEXT("Prerequisite.BActivationAccepted")), bBActivated);
		bPrerequisites &= Test.TestTrue(Name(TEXT("Prerequisite.BActiveWithCreatedInstance")), bBActiveAtStarted
			&& bBInstancePlayingAtStarted && StartedBId != INDEX_NONE && StartedBId != StartedAId);
		bPrerequisites &= Test.TestTrue(Name(TEXT("Prerequisite.BSuccessfulAcceptedPlay")), bBPlayReturned
			&& FMath::IsFinite(BDuration) && BDuration > 0.0f && ResultB.Outcome == EGGYGOMontagePlayGuardOutcome::Accepted);
		bPrerequisites &= Test.TestTrue(Name(TEXT("Prerequisite.BExactNewInstance")), bBaselineCaptured && bBaselineExactBPlaying
			&& Baseline.bExactInstanceFound && Baseline.bInstanceActive && Baseline.InstanceId == StartedBId
			&& Baseline.InstanceId == ResultB.Identity.CreatedInstanceId && Baseline.ActiveAssetInstanceId == Baseline.InstanceId);
		bPrerequisites &= Test.TestTrue(Name(TEXT("Prerequisite.BLocalAssetAndOwner")), Baseline.Local.AnimMontage.Get() == Fixture.MontageB
			&& Baseline.Local.AnimatingAbility.Get() == Fixture.AbilityB && Baseline.Local.PlayInstanceId != 0);
		bPrerequisites &= Test.TestTrue(Name(TEXT("Prerequisite.BGAMontageAndLifecycle")), Baseline.AbilityAMontage == nullptr
			&& Baseline.AbilityBMontage == Fixture.MontageB && !Baseline.bAbilityAActive && Baseline.bAbilityBActive);
		bPrerequisites &= Test.TestTrue(Name(TEXT("Prerequisite.BSectionPositionAndRate")), Baseline.Section == FName(TEXT("SuccessorStart"))
			&& FMath::IsNearlyEqual(Baseline.Position, 0.65f) && FMath::IsNearlyEqual(Baseline.PlayRate, 1.25f));
		bPrerequisites &= Test.TestTrue(Name(TEXT("Prerequisite.BReplication")), Baseline.Rep.Animation.Get() == Fixture.MontageB
			&& Baseline.Rep.PlayInstanceId != 0
			&& Baseline.Rep.SectionIdToPlay == Fixture.MontageB->GetSectionIndex(TEXT("SuccessorStart")) + 1
			&& FMath::IsNearlyEqual(Baseline.Rep.Position, Baseline.Position)
			&& FMath::IsNearlyEqual(Baseline.Rep.PlayRate, Baseline.PlayRate) && !Baseline.Rep.IsStopped);
		if (!bPrerequisites)
		{
			Test.AddError(Name(TEXT("Prerequisite failed; guarded successor preservation is unproven.")));
			return false;
		}

		bool bPreserved = true;
		bPreserved &= Test.TestEqual(Name(TEXT("Guard.A.Outcome")), uint8(ResultA.Outcome), uint8(EGGYGOMontagePlayGuardOutcome::Superseded));
		bPreserved &= Test.TestEqual(Name(TEXT("Guard.B.Outcome")), uint8(ResultB.Outcome), uint8(EGGYGOMontagePlayGuardOutcome::Accepted));
		bPreserved &= Test.TestEqual(Name(TEXT("Guard.A.NativeStage")), uint8(ResultA.NativeStage), uint8(EGGYGOMontagePlayGuardNativeStage::Returned));
		bPreserved &= Test.TestEqual(Name(TEXT("Guard.B.NativeStage")), uint8(ResultB.NativeStage), uint8(EGGYGOMontagePlayGuardNativeStage::Returned));
		bPreserved &= Test.TestEqual(Name(TEXT("Guard.A.PublicReturn")), ADuration, 0.0f);
		bPreserved &= Test.TestEqual(Name(TEXT("Guard.A.NativeReturn")), ResultA.GuardedNativeReturnValue, 0.0f);
		bPreserved &= Test.TestEqual(Name(TEXT("Guard.A.CallerReturn")), ResultA.CallerReturnValue, 0.0f);
		bPreserved &= Test.TestTrue(Name(TEXT("Guard.B.PositiveFiniteNativeAndCallerReturn")),
			FMath::IsFinite(ResultB.GuardedNativeReturnValue) && ResultB.GuardedNativeReturnValue > 0.0f
			&& FMath::IsFinite(ResultB.CallerReturnValue) && ResultB.CallerReturnValue > 0.0f);
		bPreserved &= Test.TestEqual(Name(TEXT("Guard.B.NativeMatchesPublicReturn")), ResultB.GuardedNativeReturnValue, BDuration);
		bPreserved &= Test.TestEqual(Name(TEXT("Guard.B.CallerMatchesPublicReturn")), ResultB.CallerReturnValue, BDuration);
		bPreserved &= Test.TestEqual(Name(TEXT("Guard.A.SupersedingCallId")), ResultA.SupersedingCallId, ResultB.Identity.CallId);
		bPreserved &= Test.TestEqual(Name(TEXT("Guard.B.SupersedingCallId")), ResultB.SupersedingCallId, uint64(0));
		bPreserved &= Test.TestTrue(Name(TEXT("Guard.IssuedCallsIncreasing")), ResultA.Identity.CallId != 0
			&& ResultB.Identity.CallId > ResultA.Identity.CallId);
		bPreserved &= Test.TestTrue(Name(TEXT("Guard.SameNonzeroGeneration")), ResultA.Identity.LifecycleGeneration != 0
			&& ResultB.Identity.LifecycleGeneration == ResultA.Identity.LifecycleGeneration);
		bPreserved &= Test.TestTrue(Name(TEXT("Guard.OriginalAnimIdentities")), ResultA.Identity.OriginalAnimInstance.Get() == Guard
			&& ResultB.Identity.OriginalAnimInstance.Get() == Guard);
		bPreserved &= Test.TestEqual(Name(TEXT("Guard.A.CreatedIdMatchesStarted")), ResultA.Identity.CreatedInstanceId, StartedAId);
		bPreserved &= Test.TestEqual(Name(TEXT("Guard.B.CreatedIdMatchesStarted")), ResultB.Identity.CreatedInstanceId, StartedBId);
		bPreserved &= Test.TestTrue(Name(TEXT("Guard.DistinctCreatedIds")), ResultA.Identity.CreatedInstanceId != INDEX_NONE
			&& ResultB.Identity.CreatedInstanceId != INDEX_NONE && ResultA.Identity.CreatedInstanceId != ResultB.Identity.CreatedInstanceId);
		// A4 proves lifecycle/issued identity only. The independent exact-instance checks remain mandatory.
		bPreserved &= Test.TestTrue(Name(TEXT("Guard.A.IdentityCurrent")), Guard->IsMontagePlayGuardIdentityCurrent(ResultA.Identity));
		bPreserved &= Test.TestTrue(Name(TEXT("Guard.B.IdentityCurrent")), Guard->IsMontagePlayGuardIdentityCurrent(ResultB.Identity));
		bPreserved &= Test.TestTrue(Name(TEXT("Instance.ExactBStillPlaying")), bAfterExactBPlaying);
		bPreserved &= Test.TestTrue(Name(TEXT("Instance.SuccessorSectionAndPosition")), After.Section == FName(TEXT("SuccessorStart"))
			&& FMath::IsNearlyEqual(After.Position, 0.65f));
		Test.AddInfo(FString::Printf(TEXT("%s: B-return -> A-return Local=%s -> %s Owner=%s -> %s Rep=%s -> %s LocalID=%u -> %u RepID=%u -> %u RepSection=%u -> %u BPosition=%.3f -> %.3f BSection=%s -> %s"),
			*Label, *GetNameSafe(Baseline.Local.AnimMontage.Get()), *GetNameSafe(After.Local.AnimMontage.Get()),
			*GetNameSafe(Baseline.Local.AnimatingAbility.Get()), *GetNameSafe(After.Local.AnimatingAbility.Get()),
			*GetNameSafe(Baseline.Rep.Animation.Get()), *GetNameSafe(After.Rep.Animation.Get()),
			uint32(Baseline.Local.PlayInstanceId), uint32(After.Local.PlayInstanceId),
			uint32(Baseline.Rep.PlayInstanceId), uint32(After.Rep.PlayInstanceId),
			uint32(Baseline.Rep.SectionIdToPlay), uint32(After.Rep.SectionIdToPlay),
			Baseline.Position, After.Position, *Baseline.Section.ToString(), *After.Section.ToString()));
		bPreserved &= Test.TestEqual(Name(TEXT("Local.AnimMontage")), After.Local.AnimMontage.Get(), Baseline.Local.AnimMontage.Get());
		bPreserved &= Test.TestEqual(Name(TEXT("Local.AnimatingAbility")), After.Local.AnimatingAbility.Get(), Baseline.Local.AnimatingAbility.Get());
		bPreserved &= Test.TestEqual(Name(TEXT("Local.PlayInstanceId")), After.Local.PlayInstanceId, Baseline.Local.PlayInstanceId);
		bPreserved &= Test.TestEqual(Name(TEXT("GA.A.CurrentMontage")), After.AbilityAMontage, Baseline.AbilityAMontage);
		bPreserved &= Test.TestEqual(Name(TEXT("GA.B.CurrentMontage")), After.AbilityBMontage, Baseline.AbilityBMontage);
		bPreserved &= Test.TestEqual(Name(TEXT("GA.A.Active")), After.bAbilityAActive, Baseline.bAbilityAActive);
		bPreserved &= Test.TestEqual(Name(TEXT("GA.B.Active")), After.bAbilityBActive, Baseline.bAbilityBActive);
		bPreserved &= Test.TestEqual(Name(TEXT("Rep.Animation")), After.Rep.Animation.Get(), Baseline.Rep.Animation.Get());
		bPreserved &= Test.TestEqual(Name(TEXT("Rep.PlayInstanceId")), After.Rep.PlayInstanceId, Baseline.Rep.PlayInstanceId);
		bPreserved &= Test.TestEqual(Name(TEXT("Rep.SectionIdToPlay")), After.Rep.SectionIdToPlay, Baseline.Rep.SectionIdToPlay);
		bPreserved &= Test.TestEqual(Name(TEXT("Rep.Position")), After.Rep.Position, Baseline.Rep.Position);
		bPreserved &= Test.TestEqual(Name(TEXT("Rep.PlayRate")), After.Rep.PlayRate, Baseline.Rep.PlayRate);
		bPreserved &= Test.TestEqual(Name(TEXT("Rep.IsStopped")), bool(After.Rep.IsStopped), bool(Baseline.Rep.IsStopped));
		bPreserved &= Test.TestEqual(Name(TEXT("Rep.NextSectionID")), After.Rep.NextSectionID, Baseline.Rep.NextSectionID);
		bPreserved &= Test.TestEqual(Name(TEXT("Rep.SlotName")), After.Rep.SlotName, Baseline.Rep.SlotName);
		bPreserved &= Test.TestEqual(Name(TEXT("Rep.BlendTime")), After.Rep.BlendTime, Baseline.Rep.BlendTime);
		bPreserved &= Test.TestEqual(Name(TEXT("Rep.BlendOutTime")), After.Rep.BlendOutTime, Baseline.Rep.BlendOutTime);
		bPreserved &= Test.TestEqual(Name(TEXT("Rep.PlayCount")), After.Rep.PlayCount, Baseline.Rep.PlayCount);
		bPreserved &= Test.TestEqual(Name(TEXT("Rep.bRepPosition")), bool(After.Rep.bRepPosition), bool(Baseline.Rep.bRepPosition));
		bPreserved &= Test.TestEqual(Name(TEXT("Rep.SkipPositionCorrection")), bool(After.Rep.SkipPositionCorrection), bool(Baseline.Rep.SkipPositionCorrection));
		bPreserved &= Test.TestEqual(Name(TEXT("Rep.bSkipPlayRate")), bool(After.Rep.bSkipPlayRate), bool(Baseline.Rep.bSkipPlayRate));
		bPreserved &= Test.TestTrue(Name(TEXT("Instance.ExactIdStillFound")), After.bExactInstanceFound);
		bPreserved &= Test.TestEqual(Name(TEXT("Instance.ID")), After.InstanceId, Baseline.InstanceId);
		bPreserved &= Test.TestEqual(Name(TEXT("Instance.ActiveAssetID")), After.ActiveAssetInstanceId, Baseline.ActiveAssetInstanceId);
		bPreserved &= Test.TestEqual(Name(TEXT("Instance.Active")), After.bInstanceActive, Baseline.bInstanceActive);
		bPreserved &= Test.TestEqual(Name(TEXT("Instance.Position")), After.Position, Baseline.Position);
		bPreserved &= Test.TestEqual(Name(TEXT("Instance.Section")), After.Section, Baseline.Section);
		bPreserved &= Test.TestEqual(Name(TEXT("Instance.PlayRate")), After.PlayRate, Baseline.PlayRate);
		return bPreserved;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGuardedMontageStartedSuccessorPreservationTest,
	"GGYGO.AbilitySystem.MontageGuard.StartedSuccessorPreservation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGuardedMontageStartedSuccessorPreservationTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("GEngine"), GEngine)) { return false; }
	bool bPreserved = RunGuardedMontageStartedScenario(*this, false);
	// &= evaluates the isolated same-asset case even when the different-asset case failed.
	bPreserved &= RunGuardedMontageStartedScenario(*this, true);
	bPreserved &= RunGGYGOMontageTaskSectionContract(*this);
	return bPreserved;
}
#endif
