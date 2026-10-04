#include "AbilitySystem/Tests/GGYGOPlayerComboLifecycleTestTypes.h"

#include "AbilitySystem/Abilities/GGYGOComboCorrection.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/Tasks/GGYGOAbilityTask_PlayMontageAndWaitForEvent.h"
#include "AbilitySystem/Tasks/GGYGOAbilityTask_WaitComboInput.h"
#include "Animation/AnimComposite.h"
#include "Animation/AnimMontage.h"
#include "Animation/Skeleton.h"
#include "Combat/HitDetection/GGYGOMeleeTraceComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "System/GGYGOGameplayTags.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include <limits>

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOPlayerComboLifecycleTestTypes)

UGGYGOPlayerComboLifecycleTestAbility::UGGYGOPlayerComboLifecycleTestAbility(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Drive the same native Super -> Blueprint event branch without a Blueprint asset.
	bHasBlueprintActivate = true;
}

void UGGYGOPlayerComboLifecycleTestAbility::ConfigureStepsForTest(
	UAnimMontage* Montage, FName TraceStartBone, FName TraceEndBone, float FirstPlayRate)
{
	ComboSteps.SetNum(2);
	for (int32 Index = 0; Index < ComboSteps.Num(); ++Index)
	{
		FGGYGOComboStep& Step = ComboSteps[Index];
		Step.Montage = Montage;
		Step.MainSection = TEXT("Main");
		Step.EndSection = TEXT("End");
		Step.NextStepIndex = Index == 0 ? 1 : INDEX_NONE;
		Step.PlayRate = Index == 0 ? FirstPlayRate : 1.0f;
		Step.TraceStartSocket = TraceStartBone;
		Step.TraceEndSocket = TraceEndBone;
		Step.TraceRadius = 20.0f;
	}
}

void UGGYGOPlayerComboLifecycleTestAbility::SetFirstPlayRateForTest(float PlayRate)
{
	if (ComboSteps.IsValidIndex(0)) { ComboSteps[0].PlayRate = PlayRate; }
}

FName UGGYGOPlayerComboLifecycleTestAbility::GetK2ActivateAbilityFunctionName()
{
	return GET_FUNCTION_NAME_CHECKED(UGameplayAbility, K2_ActivateAbility);
}

bool UGGYGOPlayerComboLifecycleTestAbility::EndAndReactivateForTest()
{
	const FGameplayAbilitySpecHandle Handle = CurrentSpecHandle;
	const FGameplayAbilityActorInfo* ActorInfo = CurrentActorInfo;
	const FGameplayAbilityActivationInfo ActivationInfo = CurrentActivationInfo;
	UGGYGOAbilitySystemComponent* const ASC = GetGGYGOAbilitySystemComponentFromActorInfo();
	if (!Handle.IsValid() || !ActorInfo || !ASC) { return false; }
	EndAbility(Handle, ActorInfo, ActivationInfo, false, false);
	return ASC->TryActivateAbility(Handle);
}

bool UGGYGOPlayerComboLifecycleTestAbility::EndAndReactivateAfterDeferredEndForTest(
	bool& bDeferredEndWasCoalesced)
{
	const FGameplayAbilitySpecHandle Handle = CurrentSpecHandle;
	const FGameplayAbilityActorInfo* ActorInfo = CurrentActorInfo;
	const FGameplayAbilityActivationInfo ActivationInfo = CurrentActivationInfo;
	UGGYGOAbilitySystemComponent* const ASC = GetGGYGOAbilitySystemComponentFromActorInfo();
	if (!Handle.IsValid() || !ActorInfo || !ASC) { return false; }

	bool bReactivated = false;
	IncrementListLock();
	// Run a different queued callback first. It ends the captured activation and re-activates
	// the same Spec; the following production End wrapper must reject its old generation.
	WaitingToExecute.Add(FPostLockDelegate::CreateLambda([this, ASC, Handle, ActorInfo, ActivationInfo, &bReactivated]
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, false, false);
		bReactivated = ASC->TryActivateAbility(Handle);
	}));
	EndAbility(Handle, ActorInfo, ActivationInfo, false, false);
	const int32 QueueCountAfterFirstEnd = WaitingToExecute.Num();
	EndAbility(Handle, ActorInfo, ActivationInfo, false, true);
	bDeferredEndWasCoalesced = WaitingToExecute.Num() == QueueCountAfterFirstEnd;
	DecrementListLock();
	return bReactivated && IsActive();
}

void UGGYGOPlayerComboLifecycleTestAbility::FinishForTest()
{
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, false, false);
}

void UGGYGOPlayerComboLifecycleTestAbility::UsePredictingActivationModeForTest()
{
	GetCurrentActivationInfoRef().ActivationMode = EGameplayAbilityActivationMode::Predicting;
}

void UGGYGOPlayerComboLifecycleTestAbility::ProcessEvent(UFunction* Function, void* Parms)
{
	if (Function && Function->GetFName() == GetK2ActivateAbilityFunctionName()
		&& K2ActivateAction)
	{
		TFunction<void()> Action = MoveTemp(K2ActivateAction);
		Action();
	}
	Super::ProcessEvent(Function, Parms);
}

float UGGYGOPlayerComboLifecycleTestAnimInstance::Montage_PlayInternal(UAnimMontage* MontageToPlay,
	const FMontageBlendSettings& BlendInSettings, float InPlayRate, EMontagePlayReturnType ReturnValueType,
	float InTimeToStartMontageAt, bool bStopAllMontages)
{
	const float Result = Super::Montage_PlayInternal(MontageToPlay, BlendInSettings, InPlayRate,
		ReturnValueType, InTimeToStartMontageAt, bStopAllMontages);
	if (Result > 0.0f) { ++SuccessfulSuperMontagePlayCount; }
	if (AfterSuperMontagePlayAction)
	{
		TFunction<void()> Action = MoveTemp(AfterSuperMontagePlayAction);
		Action();
	}
	return Result;
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Abilities/GameplayAbilityTargetTypes.h"
#include "Misc/AutomationTest.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "TimerManager.h"

/** Owns the synthetic components and is the single narrow friend used to inspect private GA resources. */
struct FGGYGOPlayerComboLifecycleFixture
{
	UWorld* World = nullptr;
	ACharacter* Character = nullptr;
	UGGYGOAbilitySystemComponent* ASC = nullptr;
	UGGYGOMeleeTraceComponent* Trace = nullptr;
	USkeletalMeshComponent* Mesh = nullptr;
	UGGYGOPlayerComboLifecycleTestAnimInstance* AnimInstance = nullptr;
	UAnimMontage* Montage = nullptr;
	FName TraceStartBone = NAME_None;
	FName TraceEndBone = NAME_None;
	FGameplayAbilitySpecHandle AbilityHandle;
	UGGYGOPlayerComboLifecycleTestAbility* Ability = nullptr;

	bool Initialize(UWorld* InWorld)
	{
		World = InWorld;
		Character = World ? World->SpawnActor<ACharacter>() : nullptr;
		if (!Character) { return false; }

		USkeletalMesh* MeshAsset = LoadObject<USkeletalMesh>(nullptr,
			TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
		USkeleton* Skeleton = MeshAsset ? MeshAsset->GetSkeleton() : nullptr;
		FSkeletalMeshRenderData* MeshRenderData = MeshAsset ? MeshAsset->GetResourceForRendering() : nullptr;
		if (!MeshAsset || !Skeleton || !MeshRenderData || MeshRenderData->LODRenderData.Num() == 0
			|| !Skeleton->IsCompatibleMesh(MeshAsset)) { return false; }
		const FReferenceSkeleton& ReferenceSkeleton = MeshAsset->GetRefSkeleton();
		if (ReferenceSkeleton.GetRawBoneNum() < 2 || ReferenceSkeleton.GetNum() < 2) { return false; }
		TraceStartBone = ReferenceSkeleton.GetBoneName(0);
		TraceEndBone = ReferenceSkeleton.GetBoneName(1);
		if (TraceStartBone.IsNone() || TraceEndBone.IsNone() || TraceStartBone == TraceEndBone) { return false; }

		Montage = NewObject<UAnimMontage>(Character);
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
#else
		return false;
#endif
		const int32 MainSectionIndex = Montage->GetSectionIndex(TEXT("Main"));
		const int32 EndSectionIndex = Montage->GetSectionIndex(TEXT("End"));
		const FAnimSegment* ValidatedSegment = SlotTrack.AnimTrack.AnimSegments.Num() == 1
			? &SlotTrack.AnimTrack.AnimSegments[0] : nullptr;
		const UAnimSequenceBase* SegmentReference = ValidatedSegment
			? ValidatedSegment->GetAnimReference().Get() : nullptr;
		if (Montage->GetPlayLength() <= 0.0f || MainSectionIndex == INDEX_NONE || EndSectionIndex == INDEX_NONE
			|| Montage->SlotAnimTracks.Num() != 1
			|| SlotTrack.SlotName != FName(TEXT("DefaultSlot"))
			|| !Montage->IsValidSlot(SlotTrack.SlotName)
			|| !ValidatedSegment || !ValidatedSegment->IsValid() || !SegmentReference
			|| SegmentReference->GetSkeleton() != Skeleton
			|| !FMath::IsFinite(ValidatedSegment->GetLength()) || ValidatedSegment->GetLength() <= 0.0f
			|| ValidatedSegment->AnimStartTime < 0.0f
			|| ValidatedSegment->AnimEndTime > SegmentReference->GetPlayLength()
			|| Montage->GetAnimCompositeSection(MainSectionIndex).GetTime()
				>= Montage->GetAnimCompositeSection(EndSectionIndex).GetTime()
			|| Montage->GetAnimCompositeSection(EndSectionIndex).GetTime() >= Montage->GetPlayLength())
		{
			return false;
		}

		Mesh = Character->GetMesh();
		if (!Mesh || !Mesh->IsRegistered()) { return false; }
		Mesh->SetComponentTickEnabled(false);
		Mesh->SetVisibility(false);
		Mesh->SetCastShadow(false);
		Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
		Mesh->bEnableUpdateRateOptimizations = true;
		// Use the base assignment path and reuse the Engine asset's existing render data.
		Mesh->USkinnedMeshComponent::SetSkinnedAssetAndUpdate(MeshAsset);
		Mesh->SetAnimInstanceClass(UGGYGOPlayerComboLifecycleTestAnimInstance::StaticClass());
		AnimInstance = Cast<UGGYGOPlayerComboLifecycleTestAnimInstance>(Mesh->GetAnimInstance());
		if (!AnimInstance) { return false; }

		ASC = NewObject<UGGYGOAbilitySystemComponent>(Character);
		ASC->RegisterComponent();
		ASC->InitAbilityActorInfo(Character, Character);
		Trace = NewObject<UGGYGOMeleeTraceComponent>(Character);
		Trace->RegisterComponent();
		Trace->SetComponentTickEnabled(false);

		AbilityHandle = ASC->GiveAbility(FGameplayAbilitySpec(
			UGGYGOPlayerComboLifecycleTestAbility::StaticClass(), 1));
		FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromHandle(AbilityHandle);
		Ability = Spec ? Cast<UGGYGOPlayerComboLifecycleTestAbility>(Spec->GetPrimaryInstance()) : nullptr;
		if (!Ability) { return false; }
		Ability->ConfigureStepsForTest(Montage, TraceStartBone, TraceEndBone);
		return true;
	}

	bool HasActiveMesh() const { return Ability && Ability->ActiveMesh != nullptr; }
	bool WereAllResourcesCleanedBeforeBroadcast(
		const UGGYGOAbilityTask_PlayMontageAndWaitForEvent* EndingMontageTask,
		const UGGYGOAbilityTask_WaitComboInput* EndingInputTask) const
	{
		return Ability && !Ability->ActiveMesh && !Ability->MontageTask && !Ability->InputTask
			&& Ability->GetCurrentComboStep() == INDEX_NONE
			&& Ability->GetActiveTaskCountForTest() == 0
			&& Trace && !Trace->IsTracing()
			&& EndingMontageTask && !EndingMontageTask->IsActive()
			&& !EndingMontageTask->OnCompleted.IsBound()
			&& !EndingMontageTask->OnInterrupted.IsBound()
			&& !EndingMontageTask->OnCancelled.IsBound()
			&& !EndingMontageTask->OnBlendOut.IsBound()
			&& !EndingMontageTask->EventReceived.IsBound()
			&& EndingInputTask && !EndingInputTask->IsActive()
			&& !EndingInputTask->OnPress.IsBound()
			&& Mesh && Mesh->VisibilityBasedAnimTickOption == EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered
			&& Mesh->bEnableUpdateRateOptimizations
			&& GetWatchdogRemaining() < 0.0f
			&& AnimInstance && !AnimInstance->Montage_IsActive(Montage);
	}
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* GetMontageTask() const
	{
		return Ability ? Ability->MontageTask.Get() : nullptr;
	}
	UGGYGOAbilityTask_WaitComboInput* GetInputTask() const
	{
		return Ability ? Ability->InputTask.Get() : nullptr;
	}
	float GetWatchdogRemaining() const
	{
		return World && Ability ? World->GetTimerManager().GetTimerRemaining(Ability->WatchdogHandle) : -1.0f;
	}
	float GetExpectedWatchdogRemainingFromCurrentTask() const
	{
		const UGGYGOAbilityTask_PlayMontageAndWaitForEvent* const Task = GetMontageTask();
		const float EffectiveRate = Task ? Task->GetEffectivePlayRate() : 0.0f;
		return Montage && FMath::IsFinite(EffectiveRate) && EffectiveRate > 0.0f
			? Montage->GetPlayLength() / EffectiveRate + 2.0f
			: -1.0f;
	}
};

namespace
{
	struct FGGYGOComboLifecycleTestWorld
	{
		UEngine* Engine = nullptr;
		UWorld* World = nullptr;

		explicit FGGYGOComboLifecycleTestWorld(UEngine* InEngine)
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

		~FGGYGOComboLifecycleTestWorld()
		{
			if (World)
			{
				// Keep the registered context alive until actors and components are torn down.
				World->DestroyWorld(false);
				if (Engine) { Engine->DestroyWorldContext(World); }
				if (UPackage* Package = World->GetPackage()) { Package->SetDirtyFlag(false); }
			}
		}
	};

	bool InitializeFixture(FAutomationTestBase& Test, FGGYGOComboLifecycleTestWorld& TestWorld,
		FGGYGOPlayerComboLifecycleFixture& Fixture)
	{
	return Test.TestNotNull(TEXT("GEngine"), GEngine)
		&& Test.TestNotNull(TEXT("registered transient test World"), TestWorld.World)
		&& Test.TestTrue(TEXT("valid Character/Engine SkeletalMesh/Montage/ASC/Trace fixture"),
			Fixture.Initialize(TestWorld.World));
	}

	FGameplayAbilityTargetDataHandle MakeComboCorrection(int32 Revision, int32 RequestId, int32 ServerStep,
		float Position, bool bWindowOpen = false, bool bWindowClosed = false, bool bAccepted = true)
	{
		return FGameplayAbilityTargetDataHandle(new FGGYGOComboCorrectionData(
			Revision, RequestId, ServerStep, Position, bWindowOpen, bWindowClosed, bAccepted));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOPlayerComboActivationReentryTest,
	"GGYGO.AbilitySystem.PlayerCombo.ActivationCommitAndEndReentry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOPlayerComboActivationReentryTest::RunTest(const FString& Parameters)
{
	{
		FGGYGOComboLifecycleTestWorld TestWorld(GEngine);
		FGGYGOPlayerComboLifecycleFixture Fixture;
		if (!InitializeFixture(*this, TestWorld, Fixture)) { return false; }

		bool bReactivatedFromBlueprintDispatch = false;
		Fixture.Ability->SetK2ActivateActionForTest([&]
		{
			bReactivatedFromBlueprintDispatch = Fixture.Ability->EndAndReactivateForTest();
		});
		TestTrue(TEXT("真实 Super Activate 分发期间 End 后可立即重激活同一 Spec"),
			Fixture.ASC->TryActivateAbility(Fixture.AbilityHandle));
		TestTrue(TEXT("K2_ActivateAbility 回调成功建立新激活"), bReactivatedFromBlueprintDispatch);
		TestTrue(TEXT("旧 Super Activate 栈返回后新激活仍活跃"), Fixture.Ability->IsActive());
		TestEqual(TEXT("旧 Super Activate 栈没有重建任务或覆盖新段"), Fixture.Ability->GetActiveTaskCountForTest(), 2);
		TestEqual(TEXT("新激活仍在首段"), Fixture.Ability->GetCurrentComboStep(), 0);
		Fixture.Ability->FinishForTest();
	}

	{
		FGGYGOComboLifecycleTestWorld TestWorld(GEngine);
		FGGYGOPlayerComboLifecycleFixture Fixture;
		if (!InitializeFixture(*this, TestWorld, Fixture)) { return false; }

		bool bCommitCallbackReentered = false;
		bool bDidReenter = false;
		const FDelegateHandle CommitHandle = Fixture.ASC->AbilityCommittedCallbacks.AddLambda(
			[&](UGameplayAbility* CommittedAbility)
			{
				if (CommittedAbility == Fixture.Ability && !bDidReenter)
				{
					bDidReenter = true;
					bCommitCallbackReentered = Fixture.Ability->EndAndReactivateForTest();
				}
			});
		Fixture.ASC->TryActivateAbility(Fixture.AbilityHandle);
		Fixture.ASC->AbilityCommittedCallbacks.Remove(CommitHandle);
		TestTrue(TEXT("CommitAbility 的 AbilityCommittedCallbacks 内 End 后重激活成功"), bCommitCallbackReentered);
		TestTrue(TEXT("旧 CommitAbility 栈退出后新激活仍活跃"), Fixture.Ability->IsActive());
		TestEqual(TEXT("旧 CommitAbility 栈没有覆盖新任务"), Fixture.Ability->GetActiveTaskCountForTest(), 2);
		TestEqual(TEXT("新激活仍在首段"), Fixture.Ability->GetCurrentComboStep(), 0);
		Fixture.Ability->FinishForTest();
	}

	{
		FGGYGOComboLifecycleTestWorld TestWorld(GEngine);
		FGGYGOPlayerComboLifecycleFixture Fixture;
		if (!InitializeFixture(*this, TestWorld, Fixture)) { return false; }

		bool bReadyCallbackReentered = false;
		float OldEffectivePlayRate = 0.0f;
		Fixture.AnimInstance->SetAfterSuperMontagePlayActionForTest([&]
		{
			if (const UGGYGOAbilityTask_PlayMontageAndWaitForEvent* OldTask = Fixture.GetMontageTask())
			{
				OldEffectivePlayRate = OldTask->GetEffectivePlayRate();
			}
			Fixture.Ability->SetFirstPlayRateForTest(4.0f);
			bReadyCallbackReentered = Fixture.Ability->EndAndReactivateForTest();
		});
		if (!TestTrue(TEXT("Ready 重入初始激活"), Fixture.ASC->TryActivateAbility(Fixture.AbilityHandle))) { return false; }
		TestTrue(TEXT("Montage_PlayInternal 的真实 Super 返回后同步 End/重激活"), bReadyCallbackReentered);
		TestEqual(TEXT("回调先成功执行真实 Montage 播放"), Fixture.AnimInstance->GetSuccessfulSuperMontagePlayCountForTest(), 2);
		TestTrue(TEXT("旧 Montage ReadyForActivation 栈返回后新激活仍活跃"), Fixture.Ability->IsActive());
		TestEqual(TEXT("旧 ReadyForActivation 栈没有重建任务"), Fixture.Ability->GetActiveTaskCountForTest(), 2);
		TestEqual(TEXT("旧 ReadyForActivation 栈没有覆盖新段"), Fixture.Ability->GetCurrentComboStep(), 0);
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent* const NewMontageTask = Fixture.GetMontageTask();
		if (!TestNotNull(TEXT("重激活保留其 Montage Task 速率快照"), NewMontageTask)
			|| !TestTrue(TEXT("旧 Montage Task 存在有限有效速率"),
				FMath::IsFinite(OldEffectivePlayRate) && OldEffectivePlayRate > 0.0f)) { return false; }
		const float NewExpectedWatchdog = Fixture.GetExpectedWatchdogRemainingFromCurrentTask();
		const float OldRateWatchdog = Fixture.Montage->GetPlayLength() / OldEffectivePlayRate + 2.0f;
		TestTrue(TEXT("重激活新段使用更快的有效播放速率"), NewMontageTask->GetEffectivePlayRate() > OldEffectivePlayRate);
		TestTrue(TEXT("新激活 watchdog 保留其 Task 有效速率快照"),
			FMath::IsNearlyEqual(Fixture.GetWatchdogRemaining(), NewExpectedWatchdog, 0.02f));
		TestFalse(TEXT("旧 Ready 栈没有用旧有效速率覆盖 watchdog"),
			FMath::IsNearlyEqual(Fixture.GetWatchdogRemaining(), OldRateWatchdog, 0.02f));
		TestTrue(TEXT("检查计时器快照时新激活仍在运行"), Fixture.Ability->IsActive());
		Fixture.Ability->FinishForTest();
	}

	{
		FGGYGOComboLifecycleTestWorld TestWorld(GEngine);
		FGGYGOPlayerComboLifecycleFixture Fixture;
		if (!InitializeFixture(*this, TestWorld, Fixture)) { return false; }
		if (!TestTrue(TEXT("结束重入初始激活"), Fixture.ASC->TryActivateAbility(Fixture.AbilityHandle))) { return false; }
		Fixture.Trace->BeginTraceWindow(Fixture.TraceStartBone, Fixture.TraceEndBone, 20.0f);
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent* const EndingMontageTask = Fixture.GetMontageTask();
		UGGYGOAbilityTask_WaitComboInput* const EndingInputTask = Fixture.GetInputTask();
		bool bResourcesWereCleanBeforeBroadcast = false;
		bool bReactivatedFromEndBroadcast = false;
		const FDelegateHandle EndHandle = Fixture.ASC->OnAbilityEnded.AddLambda([&](const FAbilityEndedData& Data)
		{
			if (Data.AbilityThatEnded == Fixture.Ability && !bReactivatedFromEndBroadcast)
			{
				bResourcesWereCleanBeforeBroadcast =
					Fixture.WereAllResourcesCleanedBeforeBroadcast(EndingMontageTask, EndingInputTask);
				bReactivatedFromEndBroadcast = Fixture.ASC->TryActivateAbility(Fixture.AbilityHandle);
			}
		});
		Fixture.Ability->FinishForTest();
		Fixture.ASC->OnAbilityEnded.Remove(EndHandle);
		TestTrue(TEXT("Super EndAbility 广播前旧资源、Montage、输入任务、Trace 与 Mesh 全部清理"),
			bResourcesWereCleanBeforeBroadcast);
		TestTrue(TEXT("Super EndAbility 同步广播内重激活成功"), bReactivatedFromEndBroadcast);
		TestTrue(TEXT("旧 EndAbility Super 返回后新激活仍活跃"), Fixture.Ability->IsActive());
		TestEqual(TEXT("旧 EndAbility Super 返回后保留新任务"), Fixture.Ability->GetActiveTaskCountForTest(), 2);
		TestEqual(TEXT("旧 EndAbility Super 返回后保留新 Mesh 接管"),
			Fixture.Mesh->VisibilityBasedAnimTickOption, EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones);
		TestFalse(TEXT("新激活仍关闭 URO"), Fixture.Mesh->bEnableUpdateRateOptimizations);
		Fixture.Ability->FinishForTest();
	}

	{
		FGGYGOComboLifecycleTestWorld TestWorld(GEngine);
		FGGYGOPlayerComboLifecycleFixture Fixture;
		if (!InitializeFixture(*this, TestWorld, Fixture)) { return false; }
		if (!TestTrue(TEXT("ScopeLock 延期 End 初始激活"), Fixture.ASC->TryActivateAbility(Fixture.AbilityHandle))) { return false; }
		bool bDeferredEndWasCoalesced = false;
		const bool bNewActivationSurvivedOldDeferredEnd =
			Fixture.Ability->EndAndReactivateAfterDeferredEndForTest(bDeferredEndWasCoalesced);
		TestTrue(TEXT("同一代次的重复延期 End 被 coalesce"), bDeferredEndWasCoalesced);
		TestTrue(TEXT("先执行的结束回调重激活同一 Spec"), bNewActivationSurvivedOldDeferredEnd);
		TestTrue(TEXT("捕获旧代次的后续延期 End 不结束新激活"), Fixture.Ability->IsActive());
		TestEqual(TEXT("延期旧栈没有覆盖新激活任务"), Fixture.Ability->GetActiveTaskCountForTest(), 2);
		Fixture.Ability->FinishForTest();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOPlayerComboCorrectionPayloadTest,
	"GGYGO.AbilitySystem.PlayerCombo.TypedCorrectionPayload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOPlayerComboCorrectionPayloadTest::RunTest(const FString& Parameters)
{
	FGGYGOComboCorrectionData Original(7, 19, 1, 0.625f, true, false, true);
	TestEqual(TEXT("精确 ScriptStruct 类型"), Original.GetScriptStruct(), FGGYGOComboCorrectionData::StaticStruct());
	TestTrue(TEXT("合法纠正字段通过验证"), Original.HasValidFields());
	TArray<uint8> SerializedBytes;
	FMemoryWriter Writer(SerializedBytes, true);
	bool bWriteSuccess = false;
	TestTrue(TEXT("纠正字段可序列化"), Original.NetSerialize(Writer, nullptr, bWriteSuccess));
	TestTrue(TEXT("写入成功位为真"), bWriteSuccess);
	FMemoryReader Reader(SerializedBytes, true);
	FGGYGOComboCorrectionData RoundTrip;
	bool bReadSuccess = false;
	TestTrue(TEXT("纠正字段可反序列化"), RoundTrip.NetSerialize(Reader, nullptr, bReadSuccess));
	TestTrue(TEXT("读取成功位为真"), bReadSuccess);
	TestEqual(TEXT("Revision round-trips"), RoundTrip.Revision, Original.Revision);
	TestEqual(TEXT("RequestId round-trips"), RoundTrip.RequestId, Original.RequestId);
	TestEqual(TEXT("ServerStep round-trips"), RoundTrip.ServerStep, Original.ServerStep);
	TestEqual(TEXT("Position round-trips"), RoundTrip.Position, Original.Position);
	TestTrue(TEXT("窗口标志 round-trip"), RoundTrip.bWindowOpen && !RoundTrip.bWindowClosed && RoundTrip.bAccepted);

	FGGYGOComboCorrectionData Invalid = Original;
	Invalid.Revision = 0;
	TestFalse(TEXT("零 Revision 拒绝"), Invalid.HasValidFields());
	Invalid = Original;
	Invalid.RequestId = 65536;
	TestFalse(TEXT("超范围 RequestId 拒绝"), Invalid.HasValidFields());
	Invalid = Original;
	Invalid.ServerStep = INDEX_NONE;
	TestFalse(TEXT("负段序拒绝"), Invalid.HasValidFields());
	Invalid = Original;
	Invalid.Position = std::numeric_limits<float>::quiet_NaN();
	TestFalse(TEXT("非有限动画位置拒绝"), Invalid.HasValidFields());
	Invalid = Original;
	Invalid.bWindowClosed = true;
	TestFalse(TEXT("互斥窗口标志同时为真拒绝"), Invalid.HasValidFields());

	FGGYGOComboLifecycleTestWorld TestWorld(GEngine);
	FGGYGOPlayerComboLifecycleFixture Fixture;
	if (!InitializeFixture(*this, TestWorld, Fixture)) { return false; }
	if (!TestTrue(TEXT("纠正接收路径初始激活"), Fixture.ASC->TryActivateAbility(Fixture.AbilityHandle))) { return false; }
	Fixture.Ability->UsePredictingActivationModeForTest();
	FGameplayAbilityTargetDataHandle CorrectStep = MakeComboCorrection(7, 19, 1, 0.25f);
	TestEqual(TEXT("通过 GAS TargetDataHandle 发送精确载荷类型"), CorrectStep.Num(), 1);
	TestEqual(TEXT("TargetDataHandle 保存精确 USTRUCT 类型"),
		CorrectStep.Get(0)->GetScriptStruct(), FGGYGOComboCorrectionData::StaticStruct());
	Fixture.Ability->ReceiveAbilityCorrection(CorrectStep);
	TestEqual(TEXT("有效载荷复用生产纠正路径并换到服务器段"), Fixture.Ability->GetCurrentComboStep(), 1);

	const FGameplayAbilityTargetDataHandle WrongType(new FGameplayAbilityTargetData_LocationInfo());
	Fixture.Ability->ReceiveAbilityCorrection(WrongType);
	TestEqual(TEXT("错误 TargetData 类型被精确拒绝"), Fixture.Ability->GetCurrentComboStep(), 1);
	FGameplayAbilityTargetDataHandle MultipleEntries = MakeComboCorrection(8, 20, 0, 0.1f);
	MultipleEntries.Add(new FGameplayAbilityTargetData_LocationInfo());
	Fixture.Ability->ReceiveAbilityCorrection(MultipleEntries);
	TestEqual(TEXT("多项载荷被拒绝"), Fixture.Ability->GetCurrentComboStep(), 1);
	Fixture.Ability->ReceiveAbilityCorrection(MakeComboCorrection(0, 20, 0, 0.1f));
	Fixture.Ability->ReceiveAbilityCorrection(MakeComboCorrection(8, 0, 0, 0.1f));
	Fixture.Ability->ReceiveAbilityCorrection(MakeComboCorrection(8, 20, 99, 0.1f));
	Fixture.Ability->ReceiveAbilityCorrection(MakeComboCorrection(8, 20, 0, 1.01f));
	Fixture.Ability->ReceiveAbilityCorrection(MakeComboCorrection(8, 20, 0,
		std::numeric_limits<float>::infinity()));
	Fixture.Ability->ReceiveAbilityCorrection(MakeComboCorrection(8, 20, 0, 0.1f, true, true));
	TestEqual(TEXT("无效字段与越界段不能回写 Combo 状态"), Fixture.Ability->GetCurrentComboStep(), 1);
	Fixture.Ability->ReceiveAbilityCorrection(MakeComboCorrection(8, 20, 0, 0.1f, false, false, false));
	TestEqual(TEXT("较新拒绝载荷回滚到服务器段"), Fixture.Ability->GetCurrentComboStep(), 0);
	Fixture.Ability->ReceiveAbilityCorrection(MakeComboCorrection(7, 20, 1, 0.1f, false, false, false));
	TestEqual(TEXT("旧修订不能回滚较新服务器段"), Fixture.Ability->GetCurrentComboStep(), 0);
	Fixture.Ability->FinishForTest();
	return true;
}
#endif
