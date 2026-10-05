#include "AbilitySystem/Tests/GGYGOPlayerComboLifecycleTestTypes.h"

#include "AbilitySystem/Abilities/GGYGOComboCorrection.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/Tasks/GGYGOAbilityTask_PlayMontageAndWaitForEvent.h"
#include "AbilitySystem/Tasks/GGYGOAbilityTask_WaitComboInput.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimMontage.h"
#include "Animation/Skeleton.h"
#include "Combat/HitDetection/GGYGOMeleeTraceComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Character/Data/GGYGOMovementSet.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "System/GGYGOGameplayTags.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include <limits>

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOPlayerComboLifecycleTestTypes)

AGGYGOPlayerComboLifecycleTestCharacter::AGGYGOPlayerComboLifecycleTestCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UGGYGOCharacterMovementComponent>(ACharacter::CharacterMovementComponentName))
{
	CreateDefaultSubobject<UGGYGOPawnExtensionComponent>(TEXT("PawnExtension"));
}

AGGYGOPlayerComboLifecycleTestController::AGGYGOPlayerComboLifecycleTestController(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	OverridePlayerInputClass = UPlayerInput::StaticClass();
}

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
		Step.MotionSlotName = TEXT("DefaultSlot");
		Step.NextStepIndex = Index == 0 ? 1 : INDEX_NONE;
		Step.PlayRate = Index == 0 ? FirstPlayRate : 1.0f;
		Step.TraceShape.Mode = EGGYGOMeleeTraceShapeMode::SocketChains;
		Step.TraceShape.Chains.SetNum(1);
		Step.TraceShape.Chains[0].Points = { TraceStartBone, TraceEndBone };
		Step.TraceShape.Chains[0].WorldRadiusCm = 20.0f;
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

void UGGYGOPlayerComboLifecycleTestAbility::ApplyAbilityTagsToGameplayEffectSpec(
	FGameplayEffectSpec& Spec, FGameplayAbilitySpec* AbilitySpec) const
{
	Super::ApplyAbilityTagsToGameplayEffectSpec(Spec, AbilitySpec);
	++SpecExtensionCount;
	if (bInvalidateNextRequiredSpec && IsValid(Spec.Def.Get()) && Spec.GetContext().IsValid())
	{
		bInvalidateNextRequiredSpec = false;
		++InvalidatedSpecCount;
		// The production Builder must reject the actual outgoing Spec after this native extension.
		Spec.Def = nullptr;
	}
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
#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemGlobals.h"
#include "Components/SphereComponent.h"
#include "GameplayCueManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "NativeGameplayTags.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "TimerManager.h"
#include "UObject/UnrealType.h"

/** Owns the synthetic components and is the single narrow friend used to inspect private GA resources. */
struct FGGYGOPlayerComboLifecycleFixture
{
	UWorld* World = nullptr;
	ACharacter* Character = nullptr;
	APlayerController* Controller = nullptr;
	ULocalPlayer* LocalPlayer = nullptr;
	UGGYGOCharacterMovementComponent* Movement = nullptr;
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
		Character = World ? World->SpawnActor<AGGYGOPlayerComboLifecycleTestCharacter>() : nullptr;
		if (!Character) { return false; }
		Movement = Cast<UGGYGOCharacterMovementComponent>(Character->GetCharacterMovement());
		Controller = World->SpawnActor<AGGYGOPlayerComboLifecycleTestController>();
		if (!Movement || !Controller) { return false; }
		// UE 5.8 requires a real local player for a PC with no NetDriver to be local.
		// SetPlayer initializes native input/ownership; do not override GAS locality or policy.
		LocalPlayer = NewObject<ULocalPlayer>(GEngine);
		if (!LocalPlayer) { return false; }
		Controller->SetPlayer(LocalPlayer);
		Controller->Possess(Character);
		Movement->SetMovementMode(MOVE_Walking);
		UGGYGOMovementSet* MovementSet = NewObject<UGGYGOMovementSet>(Character);
		MovementSet->bUseCurveDrivenSpeed = false; // Explicit fixed locomotion fixture; action still reads real original curves.
		FString MovementError;
		if (!Movement->SetMovementSet(MovementSet, &MovementError)) { return false; }
		// Enter the real component lifecycle for its native owner scope; no private identity is forged.
		Movement->BeginPlay();
		FGGYGOMovementOwnerSyncScopeId OwnerScope;
		if (!Movement->GetMovementOwnerSyncScope(OwnerScope, MovementError)) { return false; }

		USkeletalMesh* MeshAsset = LoadObject<USkeletalMesh>(nullptr,
			TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
		USkeleton* Skeleton = MeshAsset && MeshAsset->GetSkeleton()
			? DuplicateObject<USkeleton>(MeshAsset->GetSkeleton(), Character) : nullptr;
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
		UAnimSequence* SegmentAsset = NewObject<UAnimSequence>(Montage);
		if (!SegmentAsset) { return false; }
		SegmentAsset->SetSkeleton(Skeleton);
#if WITH_EDITOR
		IAnimationDataController& DataController = SegmentAsset->GetController();
		DataController.InitializeModel();
		DataController.SetFrameRate(FFrameRate(30, 1), false);
		DataController.SetNumberOfFrames(FFrameNumber(30), false);
		for (const FName CurveName : {FName(TEXT("RootMotion_PosX")), FName(TEXT("RootMotion_PosY")), FName(TEXT("RootMotion_PosZ"))})
		{
			const FAnimationCurveIdentifier CurveId(CurveName, ERawCurveTrackTypes::RCT_Float);
			FRichCurveKey First(0.0f, 0.0f);
			FRichCurveKey Last(1.0f, CurveName == FName(TEXT("RootMotion_PosX")) ? 100.0f : 0.0f);
			First.InterpMode = RCIM_Linear;
			if (!DataController.AddCurve(CurveId, 4, false) || !DataController.SetCurveKeys(CurveId, {First, Last}, false)) { return false; }
		}
		DataController.NotifyPopulated();
#else
		return false;
#endif
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

	bool HasOriginalLocalActorInfo() const
	{
		return LocalPlayer && Controller && Character && ASC && ASC->AbilityActorInfo.IsValid()
			&& Controller->GetLocalPlayer() == LocalPlayer && LocalPlayer->PlayerController == Controller
			&& Controller->GetPawn() == Character && Character->GetController() == Controller
			&& Controller->PlayerInput && Controller->PlayerInput->GetClass() == UPlayerInput::StaticClass()
			&& Controller->IsLocalController() && Character->IsLocallyControlled()
			&& ASC->AbilityActorInfo->PlayerController.Get() == Controller
			&& ASC->AbilityActorInfo->OwnerActor.Get() == Character
			&& ASC->AbilityActorInfo->AvatarActor.Get() == Character
			&& ASC->AbilityActorInfo->IsLocallyControlled();
	}

	bool HasActiveMesh() const { return Ability && Ability->ActiveMesh != nullptr; }
	bool HasStepMotionResources() const { return Ability && Ability->StepMotionResources.IsValid(); }
	FGGYGOComboStep& GetFirstStepForTest() const
	{
		check(Ability && Ability->ComboSteps.IsValidIndex(0));
		return Ability->ComboSteps[0];
	}
	bool WereAllResourcesCleanedBeforeBroadcast(
		const UGGYGOAbilityTask_PlayMontageAndWaitForEvent* EndingMontageTask,
		const UGGYGOAbilityTask_WaitComboInput* EndingInputTask) const
	{
		return Ability && !Ability->StepMotionResources.IsValid() && !Ability->ActiveMesh && !Ability->MontageTask && !Ability->InputTask
			&& Movement && !Movement->HasActiveActionMotion()
			&& Ability->GetCurrentComboStep() == INDEX_NONE
			&& Ability->GetActiveTaskCountForTest() == 0
			&& Trace && !Trace->IsTracing()
			&& EndingMontageTask && !EndingMontageTask->IsActive()
			&& !EndingMontageTask->OnCompleted.IsBound()
			&& !EndingMontageTask->OnInterrupted.IsBound()
			&& !EndingMontageTask->OnCancelled.IsBound()
			&& !EndingMontageTask->OnBlendOut.IsBound()
			&& !EndingMontageTask->EventReceived.IsBound()
			&& !EndingMontageTask->SectionReceived.IsBound()
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
	FTimerHandle GetWatchdogHandle() const
	{
		return Ability ? Ability->WatchdogHandle : FTimerHandle{};
	}
	bool HasResourcesForActivation(const FGGYGOAbilityActivationHandle& Original) const
	{
		return Ability && Ability->ResourceActivation.HasSameActivation(Original)
			&& Ability->ActiveMesh == Mesh && Ability->TraceComponent == Trace
			&& Ability->OriginalWorld.Get() == World;
	}
	bool HasMeshPrerequisite() const
	{
		return Trace && Mesh && Trace->PrimaryComponentTick.GetPrerequisites().ContainsByPredicate(
			[this](const FTickPrerequisite& Prerequisite)
			{
				return Prerequisite.PrerequisiteObject.Get() == Mesh
					&& Prerequisite.Get() == &Mesh->PrimaryComponentTick;
			});
	}
	FGGYGOMeleeTraceWindowHandle GetTraceWindow() const
	{
		return Ability ? Ability->TraceWindow : FGGYGOMeleeTraceWindowHandle{};
	}
	void ConfigureRuntimeHitDamage(TSubclassOf<UGameplayEffect> EffectClass, FGameplayTag CueTag)
	{
		Ability->DamageEffect = EffectClass;
		Ability->bUseSharedDamageEffectWhenUnset = false;
		Ability->HitCueTag = CueTag;
	}
	bool CaptureNativeTaskLedgerForRuntimeTest(TArray<UGameplayTask*>& OutTasks) const
	{
		OutTasks.Reset();
		if (!IsValid(Ability)) { return false; }
		for (UGameplayTask* Task : Ability->ActiveTasks) { OutTasks.Add(Task); }
		return true;
	}
	bool AreRuntimeResourceMembersDetachedForTest(FString& OutDiagnostic) const
	{
		if (!IsValid(Ability))
		{
			OutDiagnostic = TEXT("Ability unavailable");
			return false;
		}
		const bool bMeshCleared = !Ability->ActiveMesh;
		const bool bMontageTaskCleared = !Ability->MontageTask;
		const bool bStepMotionCleared = !Ability->StepMotionResources.IsValid();
		const bool bInputTaskCleared = !Ability->InputTask;
		const bool bTraceCleared = !Ability->TraceComponent;
		const bool bActivationCleared = !Ability->ResourceActivation.HasActivation();
		const bool bWorldCleared = Ability->OriginalWorld.IsExplicitlyNull();
		const bool bMontageCallbackCleared = !Ability->MontageCallbackRegistration.IsValid();
		const bool bInputCallbackCleared = !Ability->InputCallbackRegistration.IsValid();
		const bool bHitSubscriptionCleared = !Ability->TraceHitSubscription.IsValid();
		const bool bMeshRestoreFlagCleared = !Ability->bChangedMeshTick;
		const bool bPrerequisiteFlagCleared = !Ability->bAddedMeshPrerequisite;
		OutDiagnostic = FString::Printf(TEXT("MeshCleared=%d MontageTaskCleared=%d InputTaskCleared=%d TraceCleared=%d ActivationCleared=%d WorldCleared=%d MontageCallbackCleared=%d InputCallbackCleared=%d HitSubscriptionCleared=%d MeshRestoreFlagCleared=%d PrerequisiteFlagCleared=%d StepToken=%llu"),
			bMeshCleared, bMontageTaskCleared, bInputTaskCleared, bTraceCleared, bActivationCleared, bWorldCleared,
			bMontageCallbackCleared, bInputCallbackCleared, bHitSubscriptionCleared, bMeshRestoreFlagCleared,
			bPrerequisiteFlagCleared, static_cast<unsigned long long>(Ability->CurrentStepToken));
		OutDiagnostic += FString::Printf(TEXT(" StepMotionCleared=%d ActionActive=%d"), bStepMotionCleared,
			Movement ? Movement->HasActiveActionMotion() : false);
		return bStepMotionCleared && Movement && !Movement->HasActiveActionMotion()
			&& bMeshCleared && bMontageTaskCleared && bInputTaskCleared && bTraceCleared && bActivationCleared
			&& bWorldCleared && bMontageCallbackCleared && bInputCallbackCleared && bHitSubscriptionCleared
			&& bMeshRestoreFlagCleared && bPrerequisiteFlagCleared && Ability->CurrentStepToken == 0;
	}
	float GetExpectedWatchdogRemainingFromCurrentTask() const
	{
		const UGGYGOAbilityTask_PlayMontageAndWaitForEvent* const Task = GetMontageTask();
		const float EffectiveRate = Task ? Task->GetEffectivePlayRate() : 0.0f;
		return Montage && FMath::IsFinite(EffectiveRate) && EffectiveRate > 0.0f
			? Montage->GetPlayLength() / EffectiveRate + 2.0f
			: -1.0f;
	}
	float GetExpectedWatchdogRemainingFromStartPosition(float StartPosition) const
	{
		const UGGYGOAbilityTask_PlayMontageAndWaitForEvent* const Task = GetMontageTask();
		if (!Montage || !Task || !FMath::IsFinite(StartPosition)
			|| StartPosition < 0.0f || StartPosition > Montage->GetPlayLength()) { return -1.0f; }
		const float EffectiveRate = Task->GetEffectivePlayRate();
		return FMath::IsFinite(EffectiveRate) && EffectiveRate > 0.0f
			? FMath::Max(0.0f, Montage->GetPlayLength() - StartPosition) / EffectiveRate + 2.0f
			: -1.0f;
	}
};

namespace
{
	struct FGGYGOComboLifecycleTestWorld
	{
		UEngine* Engine = nullptr;
		UWorld* World = nullptr;

		explicit FGGYGOComboLifecycleTestWorld(UEngine* InEngine, bool bCreatePhysicsScene = false)
			: Engine(InEngine)
		{
			UWorld::InitializationValues Init;
			Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(bCreatePhysicsScene)
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
			Fixture.Initialize(TestWorld.World))
		&& Test.TestTrue(TEXT("native LocalPlayer/Possess and original GAS ActorInfo are locally controlled"),
			Fixture.HasOriginalLocalActorInfo());
	}

	FGameplayAbilityTargetDataHandle MakeComboCorrection(int32 Revision, int32 RequestId, int32 ServerStep,
		float Position, bool bWindowOpen = false, bool bWindowClosed = false, bool bAccepted = true)
	{
		return FGameplayAbilityTargetDataHandle(new FGGYGOComboCorrectionData(
			Revision, RequestId, ServerStep, Position, bWindowOpen, bWindowClosed, bAccepted));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOPlayerComboTraceShapeCompatibilityTest,
	"GGYGO.AbilitySystem.PlayerCombo.TraceShape.Compatibility",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOPlayerComboTraceShapeCompatibilityTest::RunTest(const FString& Parameters)
{
	UScriptStruct* StepStruct = FGGYGOComboStep::StaticStruct();
	const FProperty* LegacyStart = StepStruct->FindPropertyByName(TEXT("TraceStartSocket"));
	const FProperty* LegacyEnd = StepStruct->FindPropertyByName(TEXT("TraceEndSocket"));
	const FProperty* LegacyRadius = StepStruct->FindPropertyByName(TEXT("TraceRadius"));
	if (!TestNotNull(TEXT("历史起点成员仍在"), CastField<FNameProperty>(LegacyStart))
		|| !TestNotNull(TEXT("历史终点成员仍在"), CastField<FNameProperty>(LegacyEnd))
		|| !TestNotNull(TEXT("历史半径成员类型仍为 float"), CastField<FFloatProperty>(LegacyRadius))) { return false; }
	for (const FProperty* Property : { LegacyStart, LegacyEnd, LegacyRadius })
	{
		TestFalse(FString::Printf(TEXT("历史成员 %s 没有丢序列化值的标志"), *Property->GetName()),
			Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated));
	}
	FGGYGOComboStep Original;
	TestTrue(TEXT("历史起终点默认仍为空"), Original.TraceStartSocket.IsNone() && Original.TraceEndSocket.IsNone());
	TestEqual(TEXT("历史半径默认仍为 20"), Original.TraceRadius, 20.0f);
	Original.TraceStartSocket = TEXT("StoredLegacyStart");
	Original.TraceEndSocket = TEXT("StoredLegacyEnd");
	Original.TraceRadius = 37.25f;
	Original.MotionSlotName = TEXT("StoredMotionSlot");
	Original.MotionTranslationScale = 0.75f;
	Original.TraceShape.Mode = EGGYGOMeleeTraceShapeMode::SocketChains;
	FGGYGOMeleeTraceChain& StoredChain = Original.TraceShape.Chains.AddDefaulted_GetRef();
	StoredChain.Points = { FName(TEXT("StoredPoint0")), FName(TEXT("StoredPoint1")), FName(TEXT("StoredPoint2")) };
	StoredChain.WorldRadiusCm = 13.5f;
	TArray<uint8> SerializedBytes;
	{
		FMemoryWriter InnerWriter(SerializedBytes, true);
		FObjectAndNameAsStringProxyArchive Writer(InnerWriter, false);
		StepStruct->SerializeItem(Writer, &Original, nullptr);
		if (!TestFalse(TEXT("Step 属性写入没有归档错误"), Writer.IsError())) { return false; }
	}
	FGGYGOComboStep RoundTrip;
	{
		FMemoryReader InnerReader(SerializedBytes, true);
		FObjectAndNameAsStringProxyArchive Reader(InnerReader, false);
		StepStruct->SerializeItem(Reader, &RoundTrip, nullptr);
		if (!TestFalse(TEXT("Step 属性读回没有归档错误"), Reader.IsError())) { return false; }
	}
	TestEqual(TEXT("历史起点非默认值留存"), RoundTrip.TraceStartSocket, Original.TraceStartSocket);
	TestEqual(TEXT("历史终点非默认值留存"), RoundTrip.TraceEndSocket, Original.TraceEndSocket);
	TestEqual(TEXT("历史半径非默认值留存"), RoundTrip.TraceRadius, Original.TraceRadius);
	TestEqual(TEXT("显式动作Slot留存"), RoundTrip.MotionSlotName, Original.MotionSlotName);
	TestEqual(TEXT("显式动作位移倍率留存"), RoundTrip.MotionTranslationScale, Original.MotionTranslationScale);
	TestTrue(TEXT("新 Shape 模式留存"), RoundTrip.TraceShape.Mode == Original.TraceShape.Mode);
	if (!TestEqual(TEXT("新 Shape 链数量留存"), RoundTrip.TraceShape.Chains.Num(), 1)) { return false; }
	TestTrue(TEXT("新 Shape 有序点留存"), RoundTrip.TraceShape.Chains[0].Points == StoredChain.Points);
	TestEqual(TEXT("新 Shape 世界半径留存"), RoundTrip.TraceShape.Chains[0].WorldRadiusCm, StoredChain.WorldRadiusCm);

	FGGYGOComboLifecycleTestWorld TestWorld(GEngine);
	FGGYGOPlayerComboLifecycleFixture Fixture;
	if (!InitializeFixture(*this, TestWorld, Fixture)) { return false; }
	FGGYGOComboStep& Step = Fixture.GetFirstStepForTest();
	Step.TraceStartSocket = NAME_None;
	Step.TraceEndSocket = NAME_None;
	Step.TraceRadius = std::numeric_limits<float>::quiet_NaN();
	FString Error(TEXT("stale diagnostic"));
	const FName ConfiguredMotionSlot = Step.MotionSlotName;
	Step.MotionSlotName = NAME_None;
	TestFalse(TEXT("缺失动作Slot不从第一Slot或默认Slot补齐"), Fixture.Ability->ValidateComboConfiguration(Error));
	TestTrue(TEXT("缺失动作Slot诊断可定位必需配置"), Error.Contains(TEXT("MotionSlotName")));
	Step.MotionSlotName = ConfiguredMotionSlot;
	Step.MotionTranslationScale = 0.0f;
	TestFalse(TEXT("非法动作位移倍率不换成固定倍率"), Fixture.Ability->ValidateComboConfiguration(Error));
	Step.MotionTranslationScale = 1.0f;
	TestTrue(TEXT("合法新 Shape 不读取非法历史值"), Fixture.Ability->ValidateComboConfiguration(Error));
	TestTrue(TEXT("成功验证清空诊断"), Error.IsEmpty());
	int32 CommitCount = 0;
	const FDelegateHandle CommitHandle = Fixture.ASC->AbilityCommittedCallbacks.AddLambda([&](UGameplayAbility* Ability)
	{
		if (Ability == Fixture.Ability) { ++CommitCount; }
	});
	ON_SCOPE_EXIT { Fixture.ASC->AbilityCommittedCallbacks.Remove(CommitHandle); };
	const FGGYGOAbilityActivationRequestResult ShapeActivation =
		Fixture.ASC->TryActivateAbilityWithTerminationBoundary(Fixture.AbilityHandle);
	if (!TestTrue(TEXT("新 Shape 正常激活且忽略非法历史值"), ShapeActivation.bNativeAccepted)) { return false; }
	if (!TestTrue(TEXT("新 Shape 受控请求保留确切本地 Original"),
		ShapeActivation.Outcome == EGGYGOAbilityActivationRequestOutcome::Accepted
		&& ShapeActivation.Reason == EGGYGOAbilityActivationRequestReason::None
		&& ShapeActivation.OriginalActivation.HasActivation()
		&& Fixture.Ability->CaptureCurrentActivation().HasSameActivation(ShapeActivation.OriginalActivation))) { return false; }
	TestEqual(TEXT("正常路径提交一次"), CommitCount, 1);
	TestEqual(TEXT("正常路径实际 Montage 播放一次"), Fixture.AnimInstance->GetSuccessfulSuperMontagePlayCountForTest(), 1);
	TestEqual(TEXT("正常路径保留原 Montage 与 Input 两任务"), Fixture.Ability->GetActiveTaskCountForTest(), 2);
	TestTrue(TEXT("正常Main已取得真实CMC原动作资源"), Fixture.Movement->HasActiveActionMotion());
	TestEqual(TEXT("Main动作接管时普通Walk/Run速度上限为0"), Fixture.Movement->GetMaxSpeed(), 0.0f);
	const FGGYGOAbilityTerminationResult ShapeEnd =
		Fixture.Ability->RequestAbilityEnd(ShapeActivation.OriginalActivation, false, false);
	TestTrue(TEXT("新 Shape 普通结束取得真实 Completed"), ShapeEnd.Outcome == EGGYGOAbilityTerminationOutcome::Completed
		&& ShapeEnd.Reason == EGGYGOAbilityTerminationReason::None);
	TestTrue(TEXT("新 Shape 结束保留固定 Original 和普通 End 参数"),
		ShapeEnd.Original.GetOriginalActivation().HasSameActivation(ShapeActivation.OriginalActivation)
		&& ShapeEnd.Original.GetRequestKind() == EGGYGOAbilityTerminationRequestKind::End
		&& !ShapeEnd.Original.GetReplicateEndAbility() && !ShapeEnd.Original.WasCancelled());
	TestFalse(TEXT("正常结束关闭 Trace"), Fixture.Trace->IsTracing());
	TestEqual(TEXT("正常结束清空任务"), Fixture.Ability->GetActiveTaskCountForTest(), 0);
	TestFalse(TEXT("正常结束释放GA原段动作资源"), Fixture.HasStepMotionResources());
	TestFalse(TEXT("正常结束精确退出CMC原动作资源"), Fixture.Movement->HasActiveActionMotion());
	AddInfo(TEXT("成员往返仅证明序列化留存，不代表未知旧 Blueprint Pin/地图消费者已迁移。"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOPlayerComboInvalidTraceShapeTest,
	"GGYGO.AbilitySystem.PlayerCombo.TraceShape.InvalidConfiguration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOPlayerComboInvalidTraceShapeTest::RunTest(const FString& Parameters)
{
	FGGYGOComboLifecycleTestWorld TestWorld(GEngine);
	FGGYGOPlayerComboLifecycleFixture Fixture;
	if (!InitializeFixture(*this, TestWorld, Fixture)) { return false; }
	// 通过公开基类虚接口调用；动态目标仍是项目 final 准入校验。
	const UGameplayAbility* const PublicAdmissionAbility = Fixture.Ability;
	FGGYGOComboStep& Step = Fixture.GetFirstStepForTest();
	const FGGYGOMeleeTraceShape ValidShape = Step.TraceShape;
	Step.TraceStartSocket = Fixture.TraceStartBone;
	Step.TraceEndSocket = Fixture.TraceEndBone;
	Step.TraceRadius = 20.0f;
	Step.TraceShape = FGGYGOMeleeTraceShape();
	FString Error;
	bool bBehaviorPassed = TestFalse(TEXT("有效旧值不能补齐空 Shape"), Fixture.Ability->ValidateComboConfiguration(Error));
	bBehaviorPassed &= TestTrue(TEXT("空 Shape 诊断指明新配置与历史值无运行效力"),
		Error.Contains(TEXT("TraceShape")) && Error.Contains(TEXT("历史存值")));
	bBehaviorPassed &= TestFalse(TEXT("第一次纯准入拒绝空 Shape"),
		PublicAdmissionAbility->CanActivateAbility(Fixture.AbilityHandle, Fixture.ASC->AbilityActorInfo.Get(), nullptr, nullptr, nullptr));
	bBehaviorPassed &= TestFalse(TEXT("重复纯准入仍拒绝且不产生动作"),
		PublicAdmissionAbility->CanActivateAbility(Fixture.AbilityHandle, Fixture.ASC->AbilityActorInfo.Get(), nullptr, nullptr, nullptr));
	Step.TraceShape = ValidShape;
	Step.TraceShape.Chains[0].WorldRadiusCm = 0.0f;
	bBehaviorPassed &= TestFalse(TEXT("有效旧半径不能补齐非法新半径"), Fixture.Ability->ValidateComboConfiguration(Error));
	bBehaviorPassed &= TestFalse(TEXT("纯准入拒绝非法新半径"),
		PublicAdmissionAbility->CanActivateAbility(Fixture.AbilityHandle, Fixture.ASC->AbilityActorInfo.Get(), nullptr, nullptr, nullptr));
	Step.TraceShape = FGGYGOMeleeTraceShape();
	int32 CommitCount = 0;
	int32 EndCount = 0;
	bool bEndWasCanceled = false;
	bool bResourcesCleanAtEnd = false;
	const FDelegateHandle CommitHandle = Fixture.ASC->AbilityCommittedCallbacks.AddLambda([&](UGameplayAbility* Ability)
	{
		if (Ability == Fixture.Ability) { ++CommitCount; }
	});
	const FDelegateHandle EndHandle = Fixture.ASC->OnAbilityEnded.AddLambda([&](const FAbilityEndedData& Data)
	{
		if (Data.AbilityThatEnded == Fixture.Ability)
		{
			++EndCount;
			bEndWasCanceled = Data.bWasCancelled;
			FString Diagnostic;
			bResourcesCleanAtEnd = Fixture.AreRuntimeResourceMembersDetachedForTest(Diagnostic);
		}
	});
	ON_SCOPE_EXIT
	{
		Fixture.ASC->AbilityCommittedCallbacks.Remove(CommitHandle);
		Fixture.ASC->OnAbilityEnded.Remove(EndHandle);
	};
	// Keep both actual production Error diagnostics visible, as with the required-GE fault leaves.
	const FGGYGOAbilityActivationRequestResult EmptyShapeRequest =
		Fixture.ASC->TryActivateAbilityWithTerminationBoundary(Fixture.AbilityHandle);
	bBehaviorPassed &= TestFalse(TEXT("实际请求明确拒绝空 Shape"), EmptyShapeRequest.bNativeAccepted);
	bBehaviorPassed &= TestTrue(TEXT("受控准入拒绝没有激活或完成历史"),
		EmptyShapeRequest.Outcome == EGGYGOAbilityActivationRequestOutcome::Rejected
		&& EmptyShapeRequest.Reason == EGGYGOAbilityActivationRequestReason::NativeActivationRejected
		&& !EmptyShapeRequest.OriginalActivation.HasActivation() && !EmptyShapeRequest.OriginalTerminationCompleted.HasCompletion());
	bBehaviorPassed &= TestEqual(TEXT("准入拒绝未创建激活结束通知"), EndCount, 0);
	Step.TraceShape = ValidShape;
	bool bBlueprintCallbackRan = false;
	Fixture.Ability->SetK2ActivateActionForTest([&]
	{
		bBlueprintCallbackRan = true;
		Fixture.GetFirstStepForTest().TraceShape = FGGYGOMeleeTraceShape();
	});
	const FGGYGOAbilityActivationRequestResult ClearedShapeRequest =
		Fixture.ASC->TryActivateAbilityWithTerminationBoundary(Fixture.AbilityHandle);
	bBehaviorPassed &= TestTrue(TEXT("受控请求保留提交前结束的 Original 历史"),
		ClearedShapeRequest.bNativeAccepted && ClearedShapeRequest.Outcome == EGGYGOAbilityActivationRequestOutcome::Accepted
		&& ClearedShapeRequest.Reason == EGGYGOAbilityActivationRequestReason::None
		&& ClearedShapeRequest.OriginalActivation.HasActivation());
	const FGGYGOAbilityTerminationCompletedNotice& ClearedShapeCompletion = ClearedShapeRequest.OriginalTerminationCompleted;
	bBehaviorPassed &= TestTrue(TEXT("真实 outer Try 返回后取得同原激活的 Completed"),
		ClearedShapeCompletion.HasCompletion() && ClearedShapeCompletion.GetReason() == EGGYGOAbilityTerminationReason::None
		&& ClearedShapeCompletion.GetOriginal().GetOriginalActivation().HasSameActivation(ClearedShapeRequest.OriginalActivation)
		&& ClearedShapeCompletion.GetOriginal().GetRequestKind() == EGGYGOAbilityTerminationRequestKind::End
		&& ClearedShapeCompletion.GetOriginal().GetReplicateEndAbility() && ClearedShapeCompletion.GetOriginal().WasCancelled());
	bBehaviorPassed &= TestTrue(TEXT("实际 Super/BP 回调已运行并撤掉 Shape"), bBlueprintCallbackRan);
	bBehaviorPassed &= TestEqual(TEXT("提交前拒绝没有 Commit 通知"), CommitCount, 0);
	bBehaviorPassed &= TestEqual(TEXT("提交前拒绝仅结束原激活一次"), EndCount, 1);
	bBehaviorPassed &= TestTrue(TEXT("提交前拒绝按故障取消原激活"), bEndWasCanceled);
	bBehaviorPassed &= TestTrue(TEXT("结束广播前原资源已清理"), bResourcesCleanAtEnd);
	bBehaviorPassed &= TestFalse(TEXT("无活动 GA"), Fixture.Ability->IsActive());
	bBehaviorPassed &= TestEqual(TEXT("从未播放 Montage"), Fixture.AnimInstance->GetSuccessfulSuperMontagePlayCountForTest(), 0);
	bBehaviorPassed &= TestEqual(TEXT("没有残留 Task"), Fixture.Ability->GetActiveTaskCountForTest(), 0);
	bBehaviorPassed &= TestFalse(TEXT("没有 Trace 执行"), Fixture.Trace->IsTracing());
	bBehaviorPassed &= TestFalse(TEXT("没有 Owned 窗口"), Fixture.GetTraceWindow().HasWindow());
	bBehaviorPassed &= TestTrue(TEXT("没有 Watchdog"), Fixture.GetWatchdogRemaining() < 0.0f);
	bBehaviorPassed &= TestFalse(TEXT("未接管 Mesh Tick 依赖"), Fixture.HasMeshPrerequisite());
	bBehaviorPassed &= TestEqual(TEXT("未改变原 Mesh Tick 策略"), Fixture.Mesh->VisibilityBasedAnimTickOption,
		EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered);
	bBehaviorPassed &= TestTrue(TEXT("未改变原 URO"), Fixture.Mesh->bEnableUpdateRateOptimizations);
	AddInfo(FString::Printf(TEXT("TraceShape invalid configuration behavior assertions %s; production diagnostics retained."),
		bBehaviorPassed ? TEXT("PASS") : TEXT("FAIL")));
	return bBehaviorPassed;
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
	const bool bHadMeshPrerequisiteBeforeActivation = Fixture.HasMeshPrerequisite();
	const auto CheckActiveResources = [&](const FGGYGOAbilityActivationHandle& OriginalActivation,
		const TCHAR* Label, float ExpectedStartPosition)
	{
		const auto Check = [&](const TCHAR* Detail, bool bCondition)
		{
			return TestTrue(FString::Printf(TEXT("%s %s"), Label, Detail), bCondition);
		};
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent* const CurrentMontageTask = Fixture.GetMontageTask();
		UGGYGOAbilityTask_WaitComboInput* const CurrentInputTask = Fixture.GetInputTask();
		const float WatchdogRemaining = Fixture.GetWatchdogRemaining();
		bool bPassed = Check(TEXT("持有非空原激活身份"), OriginalActivation.HasActivation());
		bPassed &= Check(TEXT("本地原激活仍活跃"), Fixture.Ability->IsActive()
			&& Fixture.Ability->CaptureCurrentActivation().HasSameActivation(OriginalActivation));
		bPassed &= Check(TEXT("实际 Mesh/Trace/World 资源属于原激活"), Fixture.HasResourcesForActivation(OriginalActivation));
		bPassed &= Check(TEXT("实际 Montage Task 活跃"), IsValid(CurrentMontageTask) && CurrentMontageTask->IsActive());
		bPassed &= Check(TEXT("实际 Input Task 活跃"), IsValid(CurrentInputTask) && CurrentInputTask->IsActive());
		bPassed &= Check(TEXT("仅持有两项实际 Task"), Fixture.Ability->GetActiveTaskCountForTest() == 2);
		bPassed &= Check(TEXT("真实 Montage 播放已建立"), Fixture.AnimInstance->Montage_IsActive(Fixture.Montage));
		bPassed &= Check(TEXT("实际 Montage cursor 匹配明确起播位置"), FMath::IsNearlyEqual(
			Fixture.AnimInstance->Montage_GetPosition(Fixture.Montage), ExpectedStartPosition, 0.02f));
		bPassed &= Check(TEXT("实际 Montage section 为 Main"),
			Fixture.AnimInstance->Montage_GetCurrentSection(Fixture.Montage) == FName(TEXT("Main")));
		bPassed &= Check(TEXT("明确起播位置对应真实原CMC动作资源"), Fixture.Movement && Fixture.Movement->HasActiveActionMotion());
		bPassed &= Check(TEXT("Mesh 刷新和 URO 接管已建立"), Fixture.HasActiveMesh()
			&& Fixture.Mesh->VisibilityBasedAnimTickOption == EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones
			&& !Fixture.Mesh->bEnableUpdateRateOptimizations);
		bPassed &= Check(TEXT("实际 Mesh tick prerequisite 已建立"), Fixture.HasMeshPrerequisite());
		bPassed &= Check(TEXT("原 watchdog 已建立且使用实际 Task 速率"), FMath::IsFinite(WatchdogRemaining)
			&& WatchdogRemaining > 0.0f && TestWorld.World->GetTimerManager().IsTimerActive(Fixture.GetWatchdogHandle())
			&& FMath::IsNearlyEqual(WatchdogRemaining,
				Fixture.GetExpectedWatchdogRemainingFromStartPosition(ExpectedStartPosition), 0.02f));
		return bPassed;
	};
	const auto EndNormalActivation = [&](const FGGYGOAbilityActivationHandle& OriginalActivation, const TCHAR* Label)
	{
		// Observe fixed original resources; neither notification callback requests a successor.
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent* const EndingMontageTask = Fixture.GetMontageTask();
		UGGYGOAbilityTask_WaitComboInput* const EndingInputTask = Fixture.GetInputTask();
		const FTimerHandle EndingWatchdog = Fixture.GetWatchdogHandle();
		const FGGYGOMeleeTraceWindowHandle EndingWindow = Fixture.GetTraceWindow();
		const auto ResourcesAreRestored = [&]
		{
			return Fixture.WereAllResourcesCleanedBeforeBroadcast(EndingMontageTask, EndingInputTask)
				&& !TestWorld.World->GetTimerManager().TimerExists(EndingWatchdog)
				&& Fixture.HasMeshPrerequisite() == bHadMeshPrerequisiteBeforeActivation
				&& !Fixture.GetTraceWindow().HasWindow()
				&& (!EndingWindow.HasWindow()
					|| Fixture.Trace->QueryOwnedTraceWindow(EndingWindow) == EGGYGOMeleeTraceWindowQueryResult::Inactive);
		};
		int32 NativeEndCount = 0;
		int32 CompletedCount = 0;
		bool bInsideNativeEndCallback = false;
		bool bInsideCompletedCallback = false;
		bool bNativeEndParametersMatch = false;
		bool bResourcesRestoredBeforeNativeBroadcast = false;
		bool bResourcesRestoredAtCompletion = false;
		bool bCompletionFollowedNativeCallbackExit = false;
		FGGYGOAbilityTerminationCompletedNotice CompletedNotice;
		const FDelegateHandle NativeEndHandle = Fixture.ASC->OnAbilityEnded.AddLambda(
			[&](const FAbilityEndedData& Data)
			{
				if (Data.AbilityThatEnded != Fixture.Ability) { return; }
				bInsideNativeEndCallback = true;
				++NativeEndCount;
				bNativeEndParametersMatch = Data.AbilitySpecHandle == Fixture.AbilityHandle
					&& !Data.bReplicateEndAbility && !Data.bWasCancelled;
				bResourcesRestoredBeforeNativeBroadcast = ResourcesAreRestored();
				bInsideNativeEndCallback = false;
			});
		const FDelegateHandle CompletedHandle = Fixture.ASC->OnAbilityTerminationCompleted().AddLambda(
			[&](const FGGYGOAbilityTerminationCompletedNotice& Notice)
			{
				bInsideCompletedCallback = true;
				++CompletedCount;
				CompletedNotice = Notice;
				bCompletionFollowedNativeCallbackExit = NativeEndCount == 1 && !bInsideNativeEndCallback;
				bResourcesRestoredAtCompletion = ResourcesAreRestored();
				bInsideCompletedCallback = false;
			});
		const FGGYGOAbilityTerminationResult EndResult = Fixture.Ability->RequestAbilityEnd(OriginalActivation, false, false);
		Fixture.ASC->OnAbilityEnded.Remove(NativeEndHandle);
		Fixture.ASC->OnAbilityTerminationCompleted().Remove(CompletedHandle);

		const auto Check = [&](const TCHAR* Detail, bool bCondition)
		{
			return TestTrue(FString::Printf(TEXT("%s %s"), Label, Detail), bCondition);
		};
		bool bPassed = Check(TEXT("正常结束请求返回 Completed/None"), EndResult.Outcome == EGGYGOAbilityTerminationOutcome::Completed
			&& EndResult.Reason == EGGYGOAbilityTerminationReason::None);
		bPassed &= Check(TEXT("结束结果匹配固定原激活"), EndResult.Original.GetOriginalActivation().HasSameActivation(OriginalActivation));
		bPassed &= Check(TEXT("结束结果保留正常 End 参数"), EndResult.Original.GetRequestKind() == EGGYGOAbilityTerminationRequestKind::End
			&& !EndResult.Original.GetReplicateEndAbility() && !EndResult.Original.WasCancelled());
		bPassed &= Check(TEXT("原生结束广播恰好一次且参数匹配"), NativeEndCount == 1 && bNativeEndParametersMatch);
		bPassed &= Check(TEXT("原生结束广播前实际已持资源恢复"), bResourcesRestoredBeforeNativeBroadcast);
		bPassed &= Check(TEXT("原完成通知恰好一次且有精确完成事实"), CompletedCount == 1 && CompletedNotice.HasCompletion()
			&& CompletedNotice.GetReason() == EGGYGOAbilityTerminationReason::None);
		bPassed &= Check(TEXT("完成通知匹配固定原激活及同一次终止"),
			CompletedNotice.GetOriginal().GetOriginalActivation().HasSameActivation(OriginalActivation)
			&& CompletedNotice.GetOriginal().GetOriginalTermination().HasSameTermination(EndResult.Original.GetOriginalTermination()));
		bPassed &= Check(TEXT("完成通知发生在原生结束回调退出后"), bCompletionFollowedNativeCallbackExit);
		bPassed &= Check(TEXT("完成通知时实际资源保持恢复"), bResourcesRestoredAtCompletion);
		bPassed &= Check(TEXT("请求返回后两项通知回调均已退出"), !bInsideNativeEndCallback && !bInsideCompletedCallback);
		bPassed &= Check(TEXT("请求返回后原激活已结束且实际资源仍恢复"), !Fixture.Ability->IsActive()
			&& !Fixture.Ability->CaptureCurrentActivation().HasActivation() && ResourcesAreRestored());
		return bPassed;
	};

	FGGYGOAbilityActivationHandle OriginalForScopeCleanup;
	ON_SCOPE_EXIT
	{
		// Return through the formal original End before the fixture unregisters Mesh/ASC.
		// Normal A/B termination leaves the ability inactive, so this sends no duplicate request.
		if (!IsValid(Fixture.Ability) || !Fixture.Ability->IsActive()) { return; }
		if (!TestTrue(TEXT("正常叶退出清理持有受控 Try 返回的固定原身份"), OriginalForScopeCleanup.HasActivation())) { return; }
		EndNormalActivation(OriginalForScopeCleanup, TEXT("正常叶退出清理"));
	};
	const FGGYGOAbilityActivationRequestResult ActivationA = Fixture.ASC->TryActivateAbilityWithTerminationBoundary(Fixture.AbilityHandle);
	OriginalForScopeCleanup = ActivationA.OriginalActivation;
	if (!TestTrue(TEXT("纠正接收路径初始激活"), ActivationA.bNativeAccepted)) { return false; }
	if (!TestTrue(TEXT("A 受控激活请求 Accepted/None"), ActivationA.Outcome == EGGYGOAbilityActivationRequestOutcome::Accepted
		&& ActivationA.Reason == EGGYGOAbilityActivationRequestReason::None)
		|| !CheckActiveResources(ActivationA.OriginalActivation, TEXT("A"), 0.0f)) { return false; }
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
	if (!CheckActiveResources(ActivationA.OriginalActivation, TEXT("纠正后的 A"), 0.1f)) { return false; }
	if (!TestFalse(TEXT("A 纠正阶段未取得 Owned Trace 窗口"), Fixture.GetTraceWindow().HasWindow())) { return false; }
	if (!EndNormalActivation(ActivationA.OriginalActivation, TEXT("A"))) { return false; }

	// Caller continuation: A's request and both notifications have returned before controlled B starts.
	const FGGYGOAbilityActivationRequestResult ActivationB = Fixture.ASC->TryActivateAbilityWithTerminationBoundary(Fixture.AbilityHandle);
	OriginalForScopeCleanup = ActivationB.OriginalActivation;
	if (!TestTrue(TEXT("A 完成后受控 B 请求 Accepted/None"), ActivationB.bNativeAccepted
		&& ActivationB.Outcome == EGGYGOAbilityActivationRequestOutcome::Accepted
		&& ActivationB.Reason == EGGYGOAbilityActivationRequestReason::None)
		|| !TestTrue(TEXT("同一实例/Spec 的 B 获得不同于 A 的原激活身份"), ActivationB.OriginalActivation.HasActivation()
			&& !ActivationB.OriginalActivation.HasSameActivation(ActivationA.OriginalActivation))
		|| !CheckActiveResources(ActivationB.OriginalActivation, TEXT("B"), 0.0f)) { return false; }
	TestEqual(TEXT("B 正常重新从首段开始"), Fixture.Ability->GetCurrentComboStep(), 0);
	if (!TestFalse(TEXT("B 开窗事件前没有 Owned Trace 窗口"), Fixture.GetTraceWindow().HasWindow())) { return false; }
	FAnimMontageInstance* const PlayingInstance = Fixture.AnimInstance->GetActiveInstanceForMontage(Fixture.Montage);
	if (!TestNotNull(TEXT("B 开窗事件使用当前真实 Montage 实例"), PlayingInstance)) { return false; }
	FGameplayEventData HitWindowEvent;
	HitWindowEvent.EventTag = GGYGOGameplayTags::Event_Montage_HitWindowBegin;
	HitWindowEvent.Instigator = Fixture.Character;
	HitWindowEvent.Target = Fixture.Character;
	HitWindowEvent.OptionalObject = Fixture.Montage;
	HitWindowEvent.OptionalObject2 = Fixture.Mesh;
	HitWindowEvent.EventMagnitude = static_cast<float>(PlayingInstance->GetInstanceID() + 1);
	Fixture.ASC->HandleGameplayEvent(HitWindowEvent.EventTag, &HitWindowEvent);
	const FGGYGOMeleeTraceWindowHandle WindowB = Fixture.GetTraceWindow();
	if (!TestTrue(TEXT("ASC 事件经真实 Montage Task/生产 Combo 建立 B 的 Owned Trace 窗口"),
		WindowB.HasWindow() && Fixture.Trace->QueryOwnedTraceWindow(WindowB) == EGGYGOMeleeTraceWindowQueryResult::Active
		&& Fixture.Trace->IsTracing())
		|| !CheckActiveResources(ActivationB.OriginalActivation, TEXT("开窗后的 B"), 0.0f)) { return false; }
	if (!EndNormalActivation(ActivationB.OriginalActivation, TEXT("B"))) { return false; }
	TestTrue(TEXT("B 正常结束后确切原 Owned Trace 窗口为 Inactive"),
		Fixture.Trace->QueryOwnedTraceWindow(WindowB) == EGGYGOMeleeTraceWindowQueryResult::Inactive);
	return true;
}

namespace
{
	UE_DEFINE_GAMEPLAY_TAG_STATIC(TAG_PlayerComboRuntimeHitCue, "GameplayCue.GGYGO.Tests.PlayerCombo.RuntimeHit");

	enum class EComboRuntimeHitCase : uint8 { NoGE, ValidGE, InvalidRequiredGE, InvalidRequiredSpec };

	struct FComboRuntimeResources
	{
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent* MontageTask;
		UGGYGOAbilityTask_WaitComboInput* InputTask;
		FTimerHandle Watchdog;
		FGGYGOMeleeTraceWindowHandle Window;

		explicit FComboRuntimeResources(const FGGYGOPlayerComboLifecycleFixture& Fixture)
			: MontageTask(Fixture.GetMontageTask()), InputTask(Fixture.GetInputTask()),
			Watchdog(Fixture.GetWatchdogHandle()), Window(Fixture.GetTraceWindow()) {}
	};

	bool AreRuntimeResourcesRestored(const FGGYGOPlayerComboLifecycleFixture& Fixture,
		const FComboRuntimeResources& Resources, bool bHadPrerequisite)
	{
		return Fixture.WereAllResourcesCleanedBeforeBroadcast(Resources.MontageTask, Resources.InputTask)
			&& !Fixture.World->GetTimerManager().TimerExists(Resources.Watchdog)
			&& Fixture.HasMeshPrerequisite() == bHadPrerequisite
			&& !Fixture.GetTraceWindow().HasWindow() && !Fixture.Trace->IsComponentTickEnabled()
			&& (!Resources.Window.HasWindow()
				|| Fixture.Trace->QueryOwnedTraceWindow(Resources.Window) == EGGYGOMeleeTraceWindowQueryResult::Inactive);
	}

	/** Read-only snapshots taken inside the same synchronous ending call, before any fixture/world teardown. */
	struct FComboRuntimeResourceObservation
	{
		bool bSourcesPresent = false;
		bool bOriginalTasksPresent = false;
		bool bNativeLedgerObserved = false;
		bool bMontageTaskValid = false;
		bool bInputTaskValid = false;
		bool bMontageTaskActive = false;
		bool bInputTaskActive = false;
		bool bMontageOwnerFinished = false;
		bool bInputOwnerFinished = false;
		TArray<UGameplayTask*> NativeTasks;
		TArray<TPair<FString, bool>> RestoredChecks;
		FString Diagnostic;

		FComboRuntimeResourceObservation() = default;
		FComboRuntimeResourceObservation(const FGGYGOPlayerComboLifecycleFixture& Fixture,
			const FComboRuntimeResources& Resources, bool bHadPrerequisite)
		{
			bSourcesPresent = IsValid(Fixture.Ability) && IsValid(Fixture.World) && IsValid(Fixture.Trace)
				&& IsValid(Fixture.Mesh) && IsValid(Fixture.AnimInstance) && IsValid(Fixture.Montage);
			bOriginalTasksPresent = Resources.MontageTask && Resources.InputTask
				&& static_cast<UGameplayTask*>(Resources.MontageTask) != static_cast<UGameplayTask*>(Resources.InputTask);
			bNativeLedgerObserved = Fixture.CaptureNativeTaskLedgerForRuntimeTest(NativeTasks);
			Diagnostic = FString::Printf(TEXT("SourcesPresent=%d AbilityValid=%d WorldValid=%d TraceValid=%d MeshValid=%d AnimInstanceValid=%d MontageValid=%d OriginalTasksPresent=%d NativeLedgerObserved=%d NativeCount=%d NativeTasks=["),
				bSourcesPresent, IsValid(Fixture.Ability), IsValid(Fixture.World), IsValid(Fixture.Trace), IsValid(Fixture.Mesh),
				IsValid(Fixture.AnimInstance), IsValid(Fixture.Montage), bOriginalTasksPresent, bNativeLedgerObserved, NativeTasks.Num());
			for (UGameplayTask* Task : NativeTasks)
			{
				Diagnostic.Appendf(TEXT("{%s@%p State=%d OwnerFinished=%d}"), *GetPathNameSafe(Task), static_cast<void*>(Task),
					Task ? static_cast<int32>(Task->GetState()) : INDEX_NONE, Task && Task->HasOwnerFinished());
			}
			Diagnostic += TEXT("]; ");
			if (!bSourcesPresent)
			{
				Diagnostic += TEXT("required fixture source unavailable; resource checks rejected");
				return;
			}
			// TaskOwnerEnded marks the saved tasks Finished/garbage before native End retires their owner ledger.
			// Observe their actual state here; IsValid=false must never skip a missing or unfinished task check.
			bMontageTaskValid = IsValid(Resources.MontageTask);
			bInputTaskValid = IsValid(Resources.InputTask);
			bMontageTaskActive = Resources.MontageTask && Resources.MontageTask->IsActive();
			bInputTaskActive = Resources.InputTask && Resources.InputTask->IsActive();
			bMontageOwnerFinished = Resources.MontageTask && Resources.MontageTask->HasOwnerFinished();
			bInputOwnerFinished = Resources.InputTask && Resources.InputTask->HasOwnerFinished();
			Diagnostic.Appendf(TEXT("OriginalMontageTask=%s@%p Valid=%d State=%d Active=%d OwnerFinished=%d; OriginalInputTask=%s@%p Valid=%d State=%d Active=%d OwnerFinished=%d; "),
				*GetPathNameSafe(Resources.MontageTask), static_cast<void*>(Resources.MontageTask),
				bMontageTaskValid, Resources.MontageTask ? static_cast<int32>(Resources.MontageTask->GetState()) : INDEX_NONE, bMontageTaskActive, bMontageOwnerFinished,
				*GetPathNameSafe(Resources.InputTask), static_cast<void*>(Resources.InputTask),
				bInputTaskValid, Resources.InputTask ? static_cast<int32>(Resources.InputTask->GetState()) : INDEX_NONE, bInputTaskActive, bInputOwnerFinished);
			const auto Record = [this](const TCHAR* Name, bool bValue) { RestoredChecks.Emplace(Name, bValue); };
			FString MemberDiagnostic;
			Record(TEXT("原成员及订阅句柄脱开"), Fixture.AreRuntimeResourceMembersDetachedForTest(MemberDiagnostic));
			Diagnostic += MemberDiagnostic + TEXT("; ");
			Record(TEXT("Combo step 复位"), Fixture.Ability->GetCurrentComboStep() == INDEX_NONE);
			Record(TEXT("Trace 已停止"), !Fixture.Trace->IsTracing());
			Record(TEXT("Trace Tick 已关闭"), !Fixture.Trace->IsComponentTickEnabled());
			Record(TEXT("当前 Owned Window 成员清空"), !Fixture.GetTraceWindow().HasWindow());
			Record(TEXT("确切原 Owned Window 已捕获"), Resources.Window.HasWindow());
			const EGGYGOMeleeTraceWindowQueryResult WindowQuery = Fixture.Trace->QueryOwnedTraceWindow(Resources.Window);
			Record(TEXT("确切原 Owned Window 为 Inactive"), WindowQuery == EGGYGOMeleeTraceWindowQueryResult::Inactive);
			Record(TEXT("Montage Task 已 Finished"), Resources.MontageTask && Resources.MontageTask->IsFinished());
			Record(TEXT("Montage Task OwnerFinished"), bMontageOwnerFinished);
			Record(TEXT("Montage Task OnCompleted 脱开"), Resources.MontageTask && !Resources.MontageTask->OnCompleted.IsBound());
			Record(TEXT("Montage Task OnInterrupted 脱开"), Resources.MontageTask && !Resources.MontageTask->OnInterrupted.IsBound());
			Record(TEXT("Montage Task OnCancelled 脱开"), Resources.MontageTask && !Resources.MontageTask->OnCancelled.IsBound());
			Record(TEXT("Montage Task OnBlendOut 脱开"), Resources.MontageTask && !Resources.MontageTask->OnBlendOut.IsBound());
			Record(TEXT("Montage Task EventReceived 脱开"), Resources.MontageTask && !Resources.MontageTask->EventReceived.IsBound());
			Record(TEXT("Input Task 已 Finished"), Resources.InputTask && Resources.InputTask->IsFinished());
			Record(TEXT("Input Task OwnerFinished"), bInputOwnerFinished);
			Record(TEXT("Input Task OnPress 脱开"), Resources.InputTask && !Resources.InputTask->OnPress.IsBound());
			Record(TEXT("Mesh Tick 策略恢复"), Fixture.Mesh->VisibilityBasedAnimTickOption == EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered);
			Record(TEXT("Mesh UpdateRate 策略恢复"), Fixture.Mesh->bEnableUpdateRateOptimizations);
			Record(TEXT("Mesh prerequisite 恢复"), Fixture.HasMeshPrerequisite() == bHadPrerequisite);
			Record(TEXT("原 watchdog handle 已捕获"), Resources.Watchdog.IsValid());
			Record(TEXT("当前 watchdog handle 清空"), !Fixture.GetWatchdogHandle().IsValid());
			Record(TEXT("当前 watchdog 无剩余时间"), Fixture.GetWatchdogRemaining() < 0.0f);
			Record(TEXT("确切原 watchdog 不存在"), !Fixture.World->GetTimerManager().TimerExists(Resources.Watchdog));
			Record(TEXT("原 Montage 停止"), !Fixture.AnimInstance->Montage_IsActive(Fixture.Montage));
			Diagnostic.Appendf(TEXT("ComboStep=%d WindowQuery=%d MeshTick=%d UpdateRate=%d Prerequisite=%d ExpectedPrerequisite=%d WatchdogRemaining=%.6f CurrentWatchdogValid=%d OriginalWatchdogValid=%d OriginalWatchdogExists=%d MontageActive=%d;"),
				Fixture.Ability->GetCurrentComboStep(), static_cast<int32>(WindowQuery), static_cast<int32>(Fixture.Mesh->VisibilityBasedAnimTickOption),
				Fixture.Mesh->bEnableUpdateRateOptimizations, Fixture.HasMeshPrerequisite(), bHadPrerequisite, Fixture.GetWatchdogRemaining(),
				Fixture.GetWatchdogHandle().IsValid(), Resources.Watchdog.IsValid(), Fixture.World->GetTimerManager().TimerExists(Resources.Watchdog),
				Fixture.AnimInstance->Montage_IsActive(Fixture.Montage));
			for (const TPair<FString, bool>& Check : RestoredChecks) { Diagnostic.Appendf(TEXT(" %s=%d;"), *Check.Key, Check.Value); }
		}

		bool WereProjectResourcesRestored() const
		{
			if (!bSourcesPresent || !bOriginalTasksPresent || RestoredChecks.IsEmpty()) { return false; }
			for (const TPair<FString, bool>& Check : RestoredChecks) { if (!Check.Value) { return false; } }
			return true;
		}
		bool CheckNativeLedger(FAutomationTestBase& Test, const TCHAR* Stage,
			const FComboRuntimeResources& Resources, bool bExpectOriginalTasks) const
		{
			bool bPassed = Test.TestTrue(FString::Printf(TEXT("真实 hit %s 必需观察来源有效"), Stage), bSourcesPresent);
			bPassed &= Test.TestTrue(FString::Printf(TEXT("真实 hit %s 两个不同原 Task 已捕获"), Stage), bOriginalTasksPresent);
			bPassed &= Test.TestTrue(FString::Printf(TEXT("真实 hit %s 原生 Task 账本实际已读取"), Stage), bNativeLedgerObserved);
			bPassed &= Test.TestEqual(FString::Printf(TEXT("真实 hit %s 原生 Task 账本数量精确"), Stage), NativeTasks.Num(), bExpectOriginalTasks ? 2 : 0);
			if (bExpectOriginalTasks)
			{
				bPassed &= Test.TestTrue(FString::Printf(TEXT("真实 hit %s 账本含确切原 Montage Task"), Stage), NativeTasks.Contains(Resources.MontageTask));
				bPassed &= Test.TestTrue(FString::Printf(TEXT("真实 hit %s 账本含确切原 Input Task"), Stage), NativeTasks.Contains(Resources.InputTask));
			}
			return bPassed;
		}
		bool CheckBeforeTrigger(FAutomationTestBase& Test, const FComboRuntimeResources& Resources) const
		{
			Test.AddInfo(FString::Printf(TEXT("E14-C resources Stage=BeforeTrigger; %s"), *Diagnostic));
			bool bPassed = CheckNativeLedger(Test, TEXT("触发前"), Resources, true);
			bPassed &= Test.TestTrue(TEXT("真实 hit 触发前原 Montage Task 有效"), bMontageTaskValid);
			bPassed &= Test.TestTrue(TEXT("真实 hit 触发前原 Input Task 有效"), bInputTaskValid);
			bPassed &= Test.TestTrue(TEXT("真实 hit 触发前原 Montage Task 正在执行"), bMontageTaskActive);
			bPassed &= Test.TestTrue(TEXT("真实 hit 触发前原 Input Task 正在执行"), bInputTaskActive);
			bPassed &= Test.TestFalse(TEXT("真实 hit 触发前原 Montage Task Owner 未结束"), bMontageOwnerFinished);
			bPassed &= Test.TestFalse(TEXT("真实 hit 触发前原 Input Task Owner 未结束"), bInputOwnerFinished);
			return bPassed;
		}
		bool CheckAfterCleanup(FAutomationTestBase& Test, const TCHAR* Stage,
			const FComboRuntimeResources& Resources, bool bExpectOriginalTasks) const
		{
			Test.AddInfo(FString::Printf(TEXT("E14-C resources Stage=%s; %s"), Stage, *Diagnostic));
			bool bPassed = CheckNativeLedger(Test, Stage, Resources, bExpectOriginalTasks);
			bPassed &= Test.TestTrue(FString::Printf(TEXT("真实 hit %s 资源分项实际已捕获"), Stage), !RestoredChecks.IsEmpty());
			for (const TPair<FString, bool>& Check : RestoredChecks)
			{
				bPassed &= Test.TestTrue(FString::Printf(TEXT("真实 hit %s %s"), Stage, *Check.Key), Check.Value);
			}
			return bPassed;
		}
	};

	/** Owns only public observation subscriptions and the query target, never the Combo's resources. */
	struct FComboRuntimeHitProbe
	{
		AActor* Target = nullptr;
		UGGYGOAbilitySystemComponent* TargetASC = nullptr;
		UGameplayCueManager* CueManager = nullptr;
		FDelegateHandle ApplyHandle;
		FDelegateHandle CueHandle;
		int32 AppliedCount = 0;
		int32 CueCount = 0;
		bool bObservedSourcesMatch = true;

		~FComboRuntimeHitProbe()
		{
			if (IsValid(TargetASC)) { TargetASC->OnGameplayEffectAppliedDelegateToSelf.Remove(ApplyHandle); }
			if (IsValid(CueManager)) { CueManager->OnGameplayCueRouted().Remove(CueHandle); }
		}

		bool Initialize(FAutomationTestBase& Test, const FGGYGOPlayerComboLifecycleFixture& Fixture, FGameplayTag CueTag)
		{
			if (!Test.TestNotNull(TEXT("真实 hit 独立 PhysicsScene"), Fixture.World->GetPhysicsScene())) { return false; }
			Target = Fixture.World->SpawnActor<AActor>();
			if (!Test.TestNotNull(TEXT("真实 hit 查询目标"), Target)) { return false; }
			USphereComponent* Sphere = NewObject<USphereComponent>(Target);
			if (!Test.TestNotNull(TEXT("真实 hit 查询碰撞体"), Sphere)
				|| !Test.TestTrue(TEXT("真实 hit 目标 Root 设置成功"), Target->SetRootComponent(Sphere))) { return false; }
			Sphere->SetSphereRadius(8.0f);
			Sphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Sphere->SetCollisionResponseToAllChannels(ECR_Ignore);
			Sphere->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
			Sphere->RegisterComponent();
			TargetASC = NewObject<UGGYGOAbilitySystemComponent>(Target);
			if (!Test.TestNotNull(TEXT("真实 hit TargetASC"), TargetASC)) { return false; }
			TargetASC->RegisterComponent();
			TargetASC->InitAbilityActorInfo(Target, Target);
			CueManager = UAbilitySystemGlobals::Get().GetGameplayCueManager();
			if (!Test.TestNotNull(TEXT("真实 Cue 路由 Manager"), CueManager)
				|| !Test.TestTrue(TEXT("真实 hit 目标组件和原生 ASC 发现路径有效"), Sphere->IsRegistered()
					&& TargetASC->IsRegistered() && TargetASC->IsOwnerActorAuthoritative()
					&& UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Target) == TargetASC)
				|| !Test.TestTrue(TEXT("真实 hit Cue Tag 已注册且目标未被抑制"), CueTag.IsValid()
					&& !CueManager->ShouldSuppressGameplayCues(Target))) { return false; }
			UGGYGOAbilitySystemComponent* const SourceASC = Fixture.ASC;
			AActor* const SourceAvatar = Fixture.Character;
			ApplyHandle = TargetASC->OnGameplayEffectAppliedDelegateToSelf.AddLambda(
				[this, SourceASC](UAbilitySystemComponent* Source, const FGameplayEffectSpec& Spec, FActiveGameplayEffectHandle)
				{
					++AppliedCount;
					bObservedSourcesMatch &= Source == SourceASC && IsValid(Spec.Def.Get()) && Spec.GetContext().IsValid();
				});
			CueHandle = CueManager->OnGameplayCueRouted().AddLambda(
				[this, SourceAvatar, CueTag](AActor* Actor, FGameplayTag Tag, EGameplayCueEvent::Type Event,
					const FGameplayCueParameters& Parameters, EGameplayCueExecutionOptions)
				{
					if (Actor == Target && Tag == CueTag && Event == EGameplayCueEvent::Executed)
					{
						++CueCount;
						bObservedSourcesMatch &= Parameters.Instigator.Get() == SourceAvatar;
					}
				});
			return Test.TestTrue(TEXT("真实 Apply/Cue 观察订阅已建立"), ApplyHandle.IsValid() && CueHandle.IsValid());
		}

		bool SweepOneHit(FAutomationTestBase& Test, const FGGYGOPlayerComboLifecycleFixture& Fixture)
		{
			const FVector Motion(0.0, 0.0, 80.0);
			const FVector Start = Fixture.Mesh->GetSocketLocation(Fixture.TraceStartBone);
			if (!Test.TestTrue(TEXT("真实 Sweep 起点有限"), !Start.ContainsNaN())
				|| !Test.TestTrue(TEXT("真实 Sweep 目标移到路径中段"), Target->SetActorLocation(Start + Motion * 0.5))) { return false; }
			const int32 InitialApplied = AppliedCount;
			const int32 InitialCues = CueCount;
			Fixture.Trace->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
			if (!Test.TestTrue(TEXT("真实 Owned Sweep 首帧仅建立基线"), AppliedCount == InitialApplied && CueCount == InitialCues)) { return false; }
			const FVector NewMeshLocation = Fixture.Mesh->GetComponentLocation() + Motion;
			Fixture.Mesh->SetWorldLocation(NewMeshLocation);
			if (!Test.TestTrue(TEXT("真实 Sweep Mesh 已完成非零位移"), Fixture.Mesh->GetComponentLocation().Equals(NewMeshLocation))) { return false; }
			Fixture.Trace->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
			CueManager->FlushPendingCues();
			return true;
		}
	};

	/** Observes one saved Original and the resources it actually held before the triggering hit. */
	struct FComboRuntimeEndObservation
	{
		FGGYGOPlayerComboLifecycleFixture& Fixture;
		FGGYGOAbilityActivationHandle Original;
		FComboRuntimeResources Resources;
		bool bHadPrerequisite;
		FComboRuntimeResourceObservation BeforeResources;
		FComboRuntimeResourceObservation AbilityEndResources;
		FComboRuntimeResourceObservation EndResources;
		FComboRuntimeResourceObservation CompletedResources;
		FDelegateHandle AbilityEndHandle;
		FDelegateHandle EndHandle;
		FDelegateHandle CompletedHandle;
		FAbilityEndedData AbilityEndedData;
		FAbilityEndedData EndedData;
		FGGYGOAbilityTerminationCompletedNotice Notice;
		int32 AbilityEndCount = 0;
		int32 EndCount = 0;
		int32 CompletedCount = 0;
		int32 ObservationOrder = 0;
		int32 AbilityEndOrder = 0;
		int32 EndOrder = 0;
		int32 CompletedOrder = 0;
		bool bInsideAbilityEnd = false;
		bool bInsideEnd = false;
		bool bAbilityEndResourcesRestored = false;
		bool bEndResourcesRestored = false;
		bool bCompletedResourcesRestored = false;
		bool bEndAfterAbilityEndExit = false;
		bool bCompletionAfterEndExit = false;

		FComboRuntimeEndObservation(FGGYGOPlayerComboLifecycleFixture& InFixture,
			const FGGYGOAbilityActivationHandle& InOriginal, bool bInHadPrerequisite)
			: Fixture(InFixture), Original(InOriginal), Resources(InFixture), bHadPrerequisite(bInHadPrerequisite),
			BeforeResources(InFixture, Resources, bInHadPrerequisite)
		{
			AbilityEndHandle = Fixture.Ability->OnGameplayAbilityEndedWithData.AddLambda([this](const FAbilityEndedData& Data)
			{
				bInsideAbilityEnd = true;
				++AbilityEndCount;
				AbilityEndOrder = ++ObservationOrder;
				AbilityEndedData = Data;
				AbilityEndResources = FComboRuntimeResourceObservation(Fixture, Resources, bHadPrerequisite);
				bAbilityEndResourcesRestored = AbilityEndResources.WereProjectResourcesRestored();
				bInsideAbilityEnd = false;
			});
			EndHandle = Fixture.ASC->OnAbilityEnded.AddLambda([this](const FAbilityEndedData& Data)
			{
				if (Data.AbilityThatEnded != Fixture.Ability && Data.AbilitySpecHandle != Fixture.AbilityHandle) { return; }
				bInsideEnd = true;
				++EndCount;
				EndOrder = ++ObservationOrder;
				EndedData = Data;
				bEndAfterAbilityEndExit = AbilityEndCount == 1 && !bInsideAbilityEnd;
				EndResources = FComboRuntimeResourceObservation(Fixture, Resources, bHadPrerequisite);
				bEndResourcesRestored = AreRuntimeResourcesRestored(Fixture, Resources, bHadPrerequisite);
				bInsideEnd = false;
			});
			CompletedHandle = Fixture.ASC->OnAbilityTerminationCompleted().AddLambda(
				[this](const FGGYGOAbilityTerminationCompletedNotice& InNotice)
				{
					++CompletedCount;
					CompletedOrder = ++ObservationOrder;
					Notice = InNotice;
					bCompletionAfterEndExit = AbilityEndCount == 1 && EndCount == 1 && !bInsideAbilityEnd && !bInsideEnd;
					CompletedResources = FComboRuntimeResourceObservation(Fixture, Resources, bHadPrerequisite);
					bCompletedResourcesRestored = AreRuntimeResourcesRestored(Fixture, Resources, bHadPrerequisite);
				});
		}
		~FComboRuntimeEndObservation()
		{
			Fixture.Ability->OnGameplayAbilityEndedWithData.Remove(AbilityEndHandle);
			Fixture.ASC->OnAbilityEnded.Remove(EndHandle);
			Fixture.ASC->OnAbilityTerminationCompleted().Remove(CompletedHandle);
		}
		bool CheckBeforeTrigger(FAutomationTestBase& Test) const
		{
			return BeforeResources.CheckBeforeTrigger(Test, Resources);
		}
		bool Check(FAutomationTestBase& Test, bool bFaultEnd) const
		{
			const FComboRuntimeResourceObservation ReturnedResources(Fixture, Resources, bHadPrerequisite);
			Test.AddInfo(FString::Printf(TEXT("E14-C native end observation: expected Ability=%s Spec=%s GAReplicate=%d Cancel=%d; GA Count=%d Ability=%s Spec=%s Replicate=%d Cancel=%d Order=%d ProjectResourcesRestored=%d; ASC Count=%d Ability=%s Spec=%s Replicate=%d Cancel=%d Order=%d ResourcesRestored=%d GAExited=%d; Completed Count=%d Order=%d ResourcesRestored=%d NativeCallbacksExited=%d."),
				*GetPathNameSafe(Fixture.Ability), *Fixture.AbilityHandle.ToString(), bFaultEnd, bFaultEnd,
				AbilityEndCount, *GetPathNameSafe(AbilityEndedData.AbilityThatEnded.Get()), *AbilityEndedData.AbilitySpecHandle.ToString(),
				AbilityEndedData.bReplicateEndAbility, AbilityEndedData.bWasCancelled, AbilityEndOrder, bAbilityEndResourcesRestored,
				EndCount, *GetPathNameSafe(EndedData.AbilityThatEnded.Get()), *EndedData.AbilitySpecHandle.ToString(),
				EndedData.bReplicateEndAbility, EndedData.bWasCancelled, EndOrder, bEndResourcesRestored, bEndAfterAbilityEndExit,
				CompletedCount, CompletedOrder, bCompletedResourcesRestored, bCompletionAfterEndExit));
			bool bPassed = Test.TestTrue(TEXT("真实 hit GA 原生结束观察订阅有效"), AbilityEndHandle.IsValid());
			bPassed &= Test.TestTrue(TEXT("真实 hit ASC 原生结束观察订阅有效"), EndHandle.IsValid());
			bPassed &= Test.TestTrue(TEXT("真实 hit Completed 观察订阅有效"), CompletedHandle.IsValid());
			bPassed &= Test.TestEqual(TEXT("真实 hit GA 原生结束恰好一次"), AbilityEndCount, 1);
			bPassed &= Test.TestTrue(TEXT("真实 hit GA 原生结束 Ability 身份精确"), AbilityEndedData.AbilityThatEnded == Fixture.Ability);
			bPassed &= Test.TestTrue(TEXT("真实 hit GA 原生结束 Spec 身份精确"), AbilityEndedData.AbilitySpecHandle == Fixture.AbilityHandle);
			bPassed &= Test.TestEqual(TEXT("真实 hit GA 原生结束取消参数精确"), AbilityEndedData.bWasCancelled, bFaultEnd);
			// GA broadcasts the actual Super End arguments; ASC's local NotifyAbilityEnded broadcasts replicate=false.
			bPassed &= Test.TestEqual(TEXT("真实 hit GA 原生结束复制参数精确"), AbilityEndedData.bReplicateEndAbility, bFaultEnd);
			bPassed &= Test.TestEqual(TEXT("真实 hit ASC 原生结束恰好一次"), EndCount, 1);
			bPassed &= Test.TestTrue(TEXT("真实 hit ASC 原生结束 Ability 身份精确"), EndedData.AbilityThatEnded == Fixture.Ability);
			bPassed &= Test.TestTrue(TEXT("真实 hit ASC 原生结束 Spec 身份精确"), EndedData.AbilitySpecHandle == Fixture.AbilityHandle);
			bPassed &= Test.TestEqual(TEXT("真实 hit ASC 原生结束取消参数精确"), EndedData.bWasCancelled, bFaultEnd);
			bPassed &= Test.TestFalse(TEXT("真实 hit ASC 本地结束通知复制字段固定为 false"), EndedData.bReplicateEndAbility);
			bPassed &= Test.TestTrue(TEXT("真实 hit 固定 Original 的 End 已精确 Completed"), CompletedCount == 1 && Notice.HasCompletion()
				&& Notice.GetReason() == EGGYGOAbilityTerminationReason::None
				&& Notice.GetOriginal().GetOriginalActivation().HasSameActivation(Original)
				&& Notice.GetOriginal().GetRequestKind() == EGGYGOAbilityTerminationRequestKind::End
				&& Notice.GetOriginal().WasCancelled() == bFaultEnd
				&& Notice.GetOriginal().GetReplicateEndAbility() == bFaultEnd);
			bPassed &= Test.TestEqual(TEXT("真实 hit GA 原生结束先广播"), AbilityEndOrder, 1);
			bPassed &= Test.TestEqual(TEXT("真实 hit ASC 原生结束随后广播"), EndOrder, 2);
			bPassed &= Test.TestEqual(TEXT("真实 hit Completed 最后通知"), CompletedOrder, 3);
			bPassed &= Test.TestTrue(TEXT("真实 hit ASC 广播时 GA 回调已退出"), bEndAfterAbilityEndExit);
			bPassed &= Test.TestTrue(TEXT("真实 hit Completed 时两原生回调已退出"), bCompletionAfterEndExit);
			// UE broadcasts GA data before retiring ActiveTasks; finished original tasks remain registered until native Reset.
			bPassed &= AbilityEndResources.CheckAfterCleanup(Test, TEXT("GA"), Resources, true);
			bPassed &= EndResources.CheckAfterCleanup(Test, TEXT("ASC"), Resources, false);
			bPassed &= CompletedResources.CheckAfterCleanup(Test, TEXT("Completed"), Resources, false);
			bPassed &= ReturnedResources.CheckAfterCleanup(Test, TEXT("Returned"), Resources, false);
			bPassed &= Test.TestTrue(TEXT("真实 hit GA 广播时项目原资源已恢复且原 Task 已结束"), bAbilityEndResourcesRestored);
			bPassed &= Test.TestTrue(TEXT("真实 hit ASC 广播时原资源已恢复"), bEndResourcesRestored);
			bPassed &= Test.TestTrue(TEXT("真实 hit Completed 时原资源已恢复"), bCompletedResourcesRestored);
			bPassed &= Test.TestTrue(TEXT("真实 hit 回调返回后原能力及实际已持资源释放"), !Fixture.Ability->IsActive()
				&& !Fixture.Ability->CaptureCurrentActivation().HasActivation()
				&& AreRuntimeResourcesRestored(Fixture, Resources, bHadPrerequisite));
			return bPassed;
		}
	};

	bool SendRuntimeWindowEvent(FAutomationTestBase& Test, FGGYGOPlayerComboLifecycleFixture& Fixture, bool bOpen)
	{
		FAnimMontageInstance* const Instance = Fixture.AnimInstance->GetActiveInstanceForMontage(Fixture.Montage);
		if (!Test.TestNotNull(TEXT("真实 hit 窗口事件匹配当前 Montage 实例"), Instance)) { return false; }
		FGameplayEventData Event;
		Event.EventTag = bOpen ? GGYGOGameplayTags::Event_Montage_HitWindowBegin : GGYGOGameplayTags::Event_Montage_HitWindowEnd;
		Event.Instigator = Fixture.Character;
		Event.Target = Fixture.Character;
		Event.OptionalObject = Fixture.Montage;
		Event.OptionalObject2 = Fixture.Mesh;
		Event.EventMagnitude = static_cast<float>(Instance->GetInstanceID() + 1);
		Fixture.ASC->HandleGameplayEvent(Event.EventTag, &Event);
		const FGGYGOMeleeTraceWindowHandle Window = Fixture.GetTraceWindow();
		return Test.TestTrue(TEXT("正式窗口事件完成 Owned 资源接线"), bOpen
			? Window.HasWindow() && Fixture.Trace->QueryOwnedTraceWindow(Window) == EGGYGOMeleeTraceWindowQueryResult::Active
				&& Fixture.Trace->IsTracing() && Fixture.Trace->IsComponentTickEnabled()
			: !Window.HasWindow() && !Fixture.Trace->IsTracing());
	}

	bool RunComboRuntimeHitCase(FAutomationTestBase& Test, EComboRuntimeHitCase Case)
	{
		FGGYGOComboLifecycleTestWorld TestWorld(GEngine, true);
		FGGYGOPlayerComboLifecycleFixture Fixture;
		if (!InitializeFixture(Test, TestWorld, Fixture)) { return false; }
		const bool bHadPrerequisite = Fixture.HasMeshPrerequisite();
		FGGYGOAbilityActivationHandle CleanupOriginal;
		FComboRuntimeHitProbe Probe;
		ON_SCOPE_EXIT
		{
			if (!IsValid(Fixture.Ability) || !Fixture.Ability->IsActive()) { return; }
			if (!Test.TestTrue(TEXT("真实 hit 退出清理使用受控 Try 保存身份"), CleanupOriginal.HasActivation())) { return; }
			const FComboRuntimeResources Resources(Fixture);
			const FGGYGOAbilityTerminationResult Result = Fixture.Ability->RequestAbilityEnd(CleanupOriginal, false, false);
			Test.TestTrue(TEXT("真实 hit 退出清理正式原 End Completed"), Result.Outcome == EGGYGOAbilityTerminationOutcome::Completed
				&& Result.Original.GetOriginalActivation().HasSameActivation(CleanupOriginal));
			Test.TestTrue(TEXT("真实 hit 退出清理恢复实际已持资源"), AreRuntimeResourcesRestored(Fixture, Resources, bHadPrerequisite));
		};
		const FGameplayTag CueTag = TAG_PlayerComboRuntimeHitCue.GetTag();
		if (!Probe.Initialize(Test, Fixture, CueTag)) { return false; }
		const bool bNoGE = Case == EComboRuntimeHitCase::NoGE;
		const TSubclassOf<UGameplayEffect> DamageClass = bNoGE
			? TSubclassOf<UGameplayEffect>() : TSubclassOf<UGameplayEffect>(UGameplayEffect::StaticClass());
		Fixture.ConfigureRuntimeHitDamage(DamageClass, CueTag);
		if (Case == EComboRuntimeHitCase::InvalidRequiredSpec)
		{
			// Freeze this scene's explicit policy before the real group admission and activation.
			Fixture.Ability->ConfigureExclusiveSelfPolicyForTest();
			if (!Test.TestTrue(TEXT("不可取消 Builder 场景在受控启动前明确配置 Exclusive"),
				Fixture.Ability->GetSelfPolicy() == EGGYGOAbilitySelfPolicy::Exclusive)) { return false; }
		}
		const FGGYGOAbilityActivationRequestResult Activation = Fixture.ASC->TryActivateAbilityWithTerminationBoundary(Fixture.AbilityHandle);
		CleanupOriginal = Activation.OriginalActivation;
		if (!Test.TestTrue(TEXT("真实 hit 受控本地 Original Accepted"), Activation.bNativeAccepted
			&& Activation.Outcome == EGGYGOAbilityActivationRequestOutcome::Accepted
			&& Activation.Reason == EGGYGOAbilityActivationRequestReason::None
			&& CleanupOriginal.HasActivation() && Fixture.Ability->IsActive()
			&& Fixture.Ability->CaptureCurrentActivation().HasSameActivation(CleanupOriginal))
			|| !SendRuntimeWindowEvent(Test, Fixture, true)) { return false; }
		const auto CheckHeldResources = [&]
		{
			return Test.TestTrue(TEXT("真实 hit 前实际 Task/Montage/Mesh/prerequisite/watchdog 已持有"),
				Fixture.HasResourcesForActivation(CleanupOriginal) && Fixture.GetMontageTask() && Fixture.GetMontageTask()->IsActive()
				&& Fixture.GetInputTask() && Fixture.GetInputTask()->IsActive() && Fixture.Ability->GetActiveTaskCountForTest() == 2
				&& Fixture.AnimInstance->Montage_IsActive(Fixture.Montage)
				&& Fixture.Mesh->VisibilityBasedAnimTickOption == EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones
				&& !Fixture.Mesh->bEnableUpdateRateOptimizations && Fixture.HasMeshPrerequisite()
				&& Fixture.World->GetTimerManager().IsTimerActive(Fixture.GetWatchdogHandle())
				&& FMath::IsFinite(Fixture.GetWatchdogRemaining()) && Fixture.GetWatchdogRemaining() > 0.0f);
		};
		if (!CheckHeldResources() || !Probe.SweepOneHit(Test, Fixture)) { return false; }
		bool bPassed = Test.TestEqual(TEXT("真实 hit 正向对照公开 Apply 次数"), Probe.AppliedCount, bNoGE ? 0 : 1);
		bPassed &= Test.TestEqual(TEXT("真实 hit 正向对照 Cue 路由一次"), Probe.CueCount, 1);
		bPassed &= Test.TestTrue(TEXT("真实 hit 正向对照来源正确且原能力继续"), Probe.bObservedSourcesMatch
			&& Fixture.Ability->IsActive() && Fixture.Ability->CaptureCurrentActivation().HasSameActivation(CleanupOriginal));
		bPassed &= Test.TestEqual(TEXT("真实 hit 正向对照使用实际必需 Spec 扩展"),
			Fixture.Ability->GetSpecExtensionCountForTest(), bNoGE ? 0 : 1);
		if (!bPassed) { return false; }
		const bool bFault = Case == EComboRuntimeHitCase::InvalidRequiredGE || Case == EComboRuntimeHitCase::InvalidRequiredSpec;
		if (bFault)
		{
			const FGGYGOMeleeTraceWindowHandle ControlWindow = Fixture.GetTraceWindow();
			if (!SendRuntimeWindowEvent(Test, Fixture, false)
				|| !Test.TestTrue(TEXT("故障前正向对照的原窗口已关闭"),
					Fixture.Trace->QueryOwnedTraceWindow(ControlWindow) == EGGYGOMeleeTraceWindowQueryResult::Inactive)
				|| !SendRuntimeWindowEvent(Test, Fixture, true)
				|| !Test.TestTrue(TEXT("故障命中取得同一 Original 的新窗口"), Fixture.GetTraceWindow() != ControlWindow)
				|| !CheckHeldResources()) { return false; }
			if (Case == EComboRuntimeHitCase::InvalidRequiredGE)
			{
				Fixture.ConfigureRuntimeHitDamage(UGGYGOPlayerComboLifecycleInvalidDamageEffect::StaticClass(), CueTag);
			}
			else
			{
				Fixture.Ability->SetCanBeCanceledForTest(false);
				if (!Test.TestFalse(TEXT("Builder 故障原能力明确不可取消"), Fixture.Ability->CanBeCanceled())) { return false; }
				Fixture.Ability->InvalidateNextRequiredSpecForTest();
			}
		}
		FComboRuntimeEndObservation EndObservation(Fixture, CleanupOriginal, bHadPrerequisite);
		if (!EndObservation.CheckBeforeTrigger(Test)) { return false; }
		if (bFault)
		{
			const int32 AppliedBeforeFault = Probe.AppliedCount;
			const int32 CuesBeforeFault = Probe.CueCount;
			const int32 SpecsBeforeFault = Fixture.Ability->GetSpecExtensionCountForTest();
			if (!Probe.SweepOneHit(Test, Fixture)) { return false; }
			bPassed &= Test.TestEqual(TEXT("故障 hit 没有继续 Apply"), Probe.AppliedCount, AppliedBeforeFault);
			bPassed &= Test.TestEqual(TEXT("故障 hit 没有继续 Cue"), Probe.CueCount, CuesBeforeFault);
			const bool bBuilderFault = Case == EComboRuntimeHitCase::InvalidRequiredSpec;
			bPassed &= Test.TestEqual(TEXT("故障 hit 实际进入所选 GE/Builder 分支"),
				Fixture.Ability->GetSpecExtensionCountForTest() - SpecsBeforeFault, bBuilderFault ? 1 : 0);
			bPassed &= Test.TestEqual(TEXT("Builder 故障来自一次实际已构造 Spec 的扩展失效"),
				Fixture.Ability->GetInvalidatedSpecCountForTest(), bBuilderFault ? 1 : 0);
		}
		else
		{
			const FGGYGOAbilityTerminationResult EndResult = Fixture.Ability->RequestAbilityEnd(CleanupOriginal, false, false);
			bPassed &= Test.TestTrue(TEXT("真实 hit 正常对照原 End 请求 Completed"), EndResult.Outcome == EGGYGOAbilityTerminationOutcome::Completed
				&& EndResult.Original.GetOriginalActivation().HasSameActivation(CleanupOriginal)
				&& EndObservation.Notice.GetOriginal().GetOriginalTermination().HasSameTermination(EndResult.Original.GetOriginalTermination()));
		}
		bPassed &= EndObservation.Check(Test, bFault);
		Test.AddInfo(FString::Printf(TEXT("E14-C Case=%d behavior assertions=%s; Apply=%d Cue=%d SpecExtension=%d InvalidSpec=%d GANativeEnd=%d NativeEnd=%d Completed=%d. Production Error events remain unfiltered."),
			static_cast<int32>(Case), bPassed ? TEXT("PASS") : TEXT("FAIL"), Probe.AppliedCount, Probe.CueCount,
			Fixture.Ability->GetSpecExtensionCountForTest(), Fixture.Ability->GetInvalidatedSpecCountForTest(),
			EndObservation.AbilityEndCount, EndObservation.EndCount, EndObservation.CompletedCount));
		return bPassed;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOPlayerComboRuntimeHitNormalModesTest,
	"GGYGO.AbilitySystem.PlayerCombo.RuntimeHit.NormalModes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOPlayerComboRuntimeHitNormalModesTest::RunTest(const FString& Parameters)
{
	bool bPassed = RunComboRuntimeHitCase(*this, EComboRuntimeHitCase::NoGE);
	bPassed &= RunComboRuntimeHitCase(*this, EComboRuntimeHitCase::ValidGE);
	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOPlayerComboRuntimeHitRequiredGEFailureTest,
	"GGYGO.AbilitySystem.PlayerCombo.RuntimeHit.RequiredGEFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOPlayerComboRuntimeHitRequiredGEFailureTest::RunTest(const FString& Parameters)
{
	return RunComboRuntimeHitCase(*this, EComboRuntimeHitCase::InvalidRequiredGE);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOPlayerComboRuntimeHitBuilderFailureTest,
	"GGYGO.AbilitySystem.PlayerCombo.RuntimeHit.BuilderFailureUncancelable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOPlayerComboRuntimeHitBuilderFailureTest::RunTest(const FString& Parameters)
{
	return RunComboRuntimeHitCase(*this, EComboRuntimeHitCase::InvalidRequiredSpec);
}
#endif
