#include "AbilitySystem/Tests/GGYGOPlayerComboLifecycleTestTypes.h"

#include "AbilitySystem/Abilities/GGYGOComboCorrection.h"
#include "AbilitySystem/Tests/GGYGOMontageTaskTestTypes.h"
#include "AbilitySystem/Attributes/GGYGOCombatSet.h"
#include "AbilitySystem/Attributes/GGYGOHealthSet.h"
#include "AbilitySystem/Executions/GGYGODamageExecution.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/Tasks/GGYGOAbilityTask_PlayMontageAndWaitForEvent.h"
#include "AbilitySystem/Tasks/GGYGOAbilityTask_WaitComboInput.h"
#include "Animation/Data/GGYGOActionMotionSourceBinding.h"
#include "Animation/Notifies/GGYGOAnimNotifyState_GameplayEventWindow.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Character/Data/GGYGOMovementSet.h"
#include "Combat/HitDetection/GGYGOMeleeTraceComponent.h"
#include "System/GGYGOGameplayTags.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimMontage.h"
#include "Animation/ActiveMontageInstanceScope.h"
#include "Animation/AnimNotifyQueue.h"
#include "Animation/Skeleton.h"
#include "AnimNotifies/AnimNotify_PlayMontageNotify.h"
#include "Components/SkeletalMeshComponent.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "GameFramework/GameplayMessageSubsystem.h"
#include "HAL/CriticalSection.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeLock.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/LocalPlayer.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "NiagaraComponent.h"
#include "NiagaraSimCache.h"
#include "NiagaraSimCacheFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemInstanceController.h"

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

UGGYGOPlayerComboLifecycleMultiplierEffect::UGGYGOPlayerComboLifecycleMultiplierEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Instant;
	FGameplayEffectExecutionDefinition& Execution = Executions.AddDefaulted_GetRef();
	Execution.CalculationClass = UGGYGODamageExecution::StaticClass();
	Execution.PassedInTags.AddTag(GGYGOGameplayTags::DamageCalculation_AttackPowerMultiplier);
}

void UGGYGOPlayerComboLifecycleTestAbility::ConfigureDamageMultiplierForTest(float Multiplier)
{
	for (FGGYGOComboStep& Step : ComboSteps) { Step.DamageMultiplier = Multiplier; }
}

void UGGYGOPlayerComboLifecycleTestAbility::SetFirstPlayRateForTest(float PlayRate)
{
	if (ComboSteps.IsValidIndex(0)) { ComboSteps[0].PlayRate = PlayRate; }
}

void UGGYGOPlayerComboLifecycleTestAbility::CleanupAbilityResourcesForTermination(
	const FGGYGOAbilityTerminationContext& Context)
{
	TFunction<void()> OriginalAction = MoveTemp(BeforeCleanupAction);
	if (OriginalAction) { OriginalAction(); }
	Super::CleanupAbilityResourcesForTermination(Context);
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
	if (Result > 0.0f) { ++SuccessfulGuardReturnCount; }
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
#if WITH_EDITOR
#include "Animation/Runtime/GGYGOMontageGuardAnimInstance.h"
#include "Character/Components/GGYGOHeroComponent.h"
#include "Character/Data/GGYGOPawnData.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "EnhancedInputSubsystems.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/GameModeBase.h"
#include "GenericPlatform/GenericWindow.h"
#include "Input/GGYGOInputConfig.h"
#include "Input/GGYGOMovementInputOriginResource.h"
#include "Input/GGYGOPlayerInput.h"
#include "Player/GGYGOLocalPlayer.h"
#include "Player/GGYGOPlayerController.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"
#endif

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

#if WITH_EDITOR
	// Inspect the production ability without adopting its resources or changing its
	// native callbacks. The explicit Main-point probe below swaps only a PIE instance's
	// step configuration for an unsaved transient copy and restores it after teardown.
	static UGGYGOAbilityTask_PlayMontageAndWaitForEvent* GetProductionMontageTask(
		const UGGYGOPlayerComboAbility* Production)
	{
		return Production ? Production->GetMontageTask() : nullptr;
	}
	static UGGYGOAbilityTask_WaitComboInput* GetProductionInputTask(const UGGYGOPlayerComboAbility* Production)
	{
		return Production ? Production->InputTask.Get() : nullptr;
	}
	static UGGYGOAbilityTask_PlayNiagaraEffect* GetProductionFXTask(const UGGYGOPlayerComboAbility* Production)
	{
		return Production ? Production->GetStepFXTask() : nullptr;
	}
	static bool ReadProductionStep(const UGGYGOPlayerComboAbility* Production, FGGYGOComboStep& OutStep)
	{
		if (!Production || !Production->ComboSteps.IsValidIndex(Production->CurrentStep)) { return false; }
		OutStep = Production->ComboSteps[Production->CurrentStep];
		return true;
	}
	static bool ReadProductionFirstStep(const UGGYGOPlayerComboAbility* Production, FGGYGOComboStep& OutStep)
	{
		if (!Production || !Production->ComboSteps.IsValidIndex(0)) { return false; }
		OutStep = Production->ComboSteps[0];
		return true;
	}
	static int32 ReadProductionLastInputRequest(const UGGYGOPlayerComboAbility* Production)
	{
		return Production ? Production->LastRequestId : 0;
	}
	static bool ReplayCompletedProductionPointForTest(UGGYGOPlayerComboAbility* Production,
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent* OriginalTask, const FGGYGOMontageNotifyFact& OriginalPoint,
		UGGYGOCharacterMovementComponent* OriginalMovement, FString& Error)
	{
		Error.Reset();
		if (!Production || !Production->IsActive() || !OriginalTask
			|| OriginalTask->GetState() != EGameplayTaskState::Finished
			|| Production->GetMontageTask() != OriginalTask || !Production->StepMotionResources.IsValid())
		{
			Error = TEXT("晚到点探针未持有仍属当前能力的原 Finished Task／资源。"); return false;
		}
		const auto OriginalResource = Production->StepMotionResources;
		const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> HistoricalTask(OriginalTask);
		const ACharacter* OriginalCharacter = Production->ActiveMesh ? Cast<ACharacter>(Production->ActiveMesh->GetOwner()) : nullptr;
		if (HistoricalTask.IsValid() || HistoricalTask.Get(true) != OriginalTask
			|| OriginalTask->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			|| !Production->IsMotionResourceCurrent(OriginalResource) || !IsValid(OriginalMovement)
			|| !OriginalCharacter || OriginalCharacter->GetCharacterMovement() != OriginalMovement
			|| !OriginalMovement->HasActiveActionMotion())
		{
			Error = TEXT("原 Finished Task 未完成原生退役，或原完成资源／原 CMC 等待前提不成立。");
			return false;
		}
		// 原生 EndTask 已使弱 Task 无效，此时生产 IsMotionResourceCurrent 只接受已认证 Completed。
		// 保留该生产校验和原 CMC 活动查询，不为测试暴露／写入资源内部完成事实。
		const auto OriginalInput = Production->InputTask;
		const auto OriginalActivation = Production->CaptureCurrentActivation();
		const uint64 OriginalToken = Production->CurrentStepToken;
		const TWeakObjectPtr<UGGYGOCharacterMovementComponent> CapturedMovement(OriginalMovement);
		Production->HandleMontageNotify(OriginalResource, OriginalPoint);
		Production->OpenAttackContinuation(OriginalResource);
		Production->TryOpenMovementInterruption(OriginalResource);
		const bool bPreserved = Production->IsActive() && Production->StepMotionResources == OriginalResource
			&& Production->GetMontageTask() == OriginalTask && Production->InputTask == OriginalInput
			&& Production->CurrentStepToken == OriginalToken
			&& Production->CaptureCurrentActivation().HasSameActivation(OriginalActivation)
			&& Production->IsMotionResourceCurrent(OriginalResource)
			&& CapturedMovement.IsValid() && CapturedMovement->HasActiveActionMotion()
			&& !Production->IsAttackContinuationOpen() && !Production->IsMovementInterruptionOpen();
		if (!bPreserved) { Error = TEXT("历史点重放改变原能力／Task／输入／CMC 资源或重新开放权限。"); }
		return bPreserved;
	}
	static bool InstallProductionMainPointProbe(UGGYGOPlayerComboAbility* Production, int32 StepIndex,
		FGGYGOComboStep& OutOriginal, TStrongObjectPtr<UAnimMontage>& OutProbe, FString& Error)
	{
		if (!Production || Production->IsActive() || !Production->ComboSteps.IsValidIndex(StepIndex)) { return false; }
		OutOriginal = Production->ComboSteps[StepIndex];
		float OriginalPoint = 0.0f;
		if (!UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolveOriginalMontageNotify(
			OutOriginal.Montage, OutOriginal.InterruptionNotifyName, OriginalPoint, Error)) { return false; }
		UAnimMontage* Probe = DuplicateObject<UAnimMontage>(OutOriginal.Montage, GetTransientPackage(),
			MakeUniqueObjectName(GetTransientPackage(), UAnimMontage::StaticClass(), TEXT("PlayerComboSignalProbe")));
		if (!Probe) { Error = TEXT("could not duplicate the formal Montage for the finite unsaved point probe"); return false; }
		OutProbe.Reset(Probe);
		const int32 Main = Probe->GetSectionIndex(OutOriginal.MainSection);
		const int32 End = Probe->GetSectionIndex(OutOriginal.EndSection);
		if (Main == INDEX_NONE || End == INDEX_NONE) { Error = TEXT("formal probe lost its Main/End"); return false; }
		const float MainStart = Probe->GetAnimCompositeSection(Main).GetTime();
		const float EndStart = Probe->GetAnimCompositeSection(End).GetTime();
		const float PointPosition = MainStart + (EndStart - MainStart) * 0.5f;
		for (FAnimNotifyEvent& Event : Probe->Notifies)
		{
			const auto* Point = Cast<UAnimNotify_PlayMontageNotify>(Event.Notify.Get());
			if (Point && FName(*Point->GetNotifyName_Implementation()) == OutOriginal.InterruptionNotifyName)
			{
				Event.Link(Probe, PointPosition);
				Event.RefreshTriggerOffset(Probe->CalculateOffsetForNotify(PointPosition));
			}
		}
		static_cast<UAnimSequenceBase*>(Probe)->RefreshCacheData();
		float ResolvedPoint = 0.0f;
		if (!UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolveOriginalMontageNotify(
			Probe, OutOriginal.InterruptionNotifyName, ResolvedPoint, Error)
			|| ResolvedPoint <= MainStart || ResolvedPoint >= EndStart) { return false; }
		Production->ComboSteps[StepIndex].Montage = Probe;
		return true;
	}
	static void RestoreProductionMainPointProbe(UGGYGOPlayerComboAbility* Production, int32 StepIndex,
		const FGGYGOComboStep& Original, const UAnimMontage* Probe)
	{
		if (Production && Production->ComboSteps.IsValidIndex(StepIndex)
			&& Production->ComboSteps[StepIndex].Montage == Probe) { Production->ComboSteps[StepIndex] = Original; }
	}
	static bool ProductionResourcesReleased(const UGGYGOPlayerComboAbility* Production, UWorld* ProductionWorld)
	{
		return Production && ProductionWorld && !Production->StepMotionResources.IsValid()
			&& !Production->ActiveMesh && !Production->GetMontageTask() && !Production->GetStepFXTask() && !Production->InputTask
			&& !Production->TraceComponent && !Production->TraceWindow.HasWindow()
			&& !Production->ResourceActivation.HasActivation() && Production->OriginalWorld.IsExplicitlyNull()
			&& !Production->HasMontageCallbackRegistration() && !Production->InputCallbackRegistration.IsValid()
			&& !Production->TraceHitSubscription.IsValid() && !Production->bChangedMeshTick
			&& !Production->bAddedMeshPrerequisite && Production->CurrentStepToken == 0
			&& Production->CurrentStep == INDEX_NONE && Production->CurrentStepRequestId == 0
			&& !ProductionWorld->GetTimerManager().TimerExists(Production->GetWatchdogHandle());
	}
#endif

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
		UAnimNotify_PlayMontageNotify* Signal = NewObject<UAnimNotify_PlayMontageNotify>(Montage);
		FNameProperty* SignalNameProperty = FindFProperty<FNameProperty>(Signal->GetClass(), TEXT("NotifyName"));
		if (!SignalNameProperty) { return false; }
		SignalNameProperty->SetPropertyValue_InContainer(Signal, FName(TEXT("Event.Montage.CancelPoint")));
		FAnimNotifyEvent SignalEvent;
		SignalEvent.Notify = Signal;
		SignalEvent.NotifyName = FName(TEXT("Event.Montage.CancelPoint"));
		SignalEvent.Link(Montage, 0.5f);
		Montage->Notifies.Add(SignalEvent);
		static_cast<UAnimSequenceBase*>(Montage)->RefreshCacheData();
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
	/** 仅修改启动前的作者配置，不创建另一条运行状态写入链。 */
	FGGYGOComboStep& UseSingleStepForTest() const
	{
		check(Ability && !Ability->IsActive() && Ability->ComboSteps.IsValidIndex(0));
		Ability->ComboSteps.SetNum(1);
		return Ability->ComboSteps[0];
	}
	FAnimNotifyEvent* FindAuthoredPointForTest(FName Name) const
	{
		if (!Montage) { return nullptr; }
		for (FAnimNotifyEvent& Event : Montage->Notifies)
		{
			const auto* Point = Cast<UAnimNotify_PlayMontageNotify>(Event.Notify.Get());
			if (Point && FName(*Point->GetNotifyName_Implementation()) == Name) { return &Event; }
		}
		return nullptr;
	}
	/** 只在未激活的临时 Montage 中声明第二个原生点，偏移使用引擎原生计算。 */
	bool AddMovementPointForTest(float AuthoredPosition) const
	{
		if (!Ability || Ability->IsActive() || !Montage) { return false; }
		const FName Name(TEXT("GGYGO.Test.MovementPoint"));
		if (FindAuthoredPointForTest(Name)) { return false; }
		UAnimNotify_PlayMontageNotify* Point = NewObject<UAnimNotify_PlayMontageNotify>(Montage);
		FNameProperty* NameProperty = Point ? FindFProperty<FNameProperty>(Point->GetClass(), TEXT("NotifyName")) : nullptr;
		if (!NameProperty) { return false; }
		NameProperty->SetPropertyValue_InContainer(Point, Name);
		FAnimNotifyEvent Event;
		Event.Notify = Point;
		Event.NotifyName = Name;
		Event.Link(Montage, AuthoredPosition);
		Event.RefreshTriggerOffset(Montage->CalculateOffsetForNotify(AuthoredPosition));
		Montage->Notifies.Add(Event);
		static_cast<UAnimSequenceBase*>(Montage)->RefreshCacheData();
		FGGYGOComboStep& Step = GetFirstStepForTest();
		Step.MovementInterruptionMode = EGGYGOActionInterruptionMode::AuthoredNotify;
		Step.MovementInterruptionNotifyName = Name;
		return true;
	}
	/** 经真实生产 StartStep 创建新的原 Task/实例，从给定原生位置起播。 */
	bool RestartFirstStepForTest(float Position) const
	{
		return Ability && Ability->IsActive()
			&& Ability->StartStep(Ability->CaptureCurrentActivation(), 0, Position);
	}
	/** 在未启动的临时资产声明真实状态窗；不为无命中动作补窗口。 */
	bool AddHitWindowForTest() const
	{
		if (!Ability || Ability->IsActive() || !Montage) { return false; }
		UGGYGOAnimNotifyState_GameplayEventWindow* Window = NewObject<UGGYGOAnimNotifyState_GameplayEventWindow>(Montage);
		Window->InitializeEventTags(GGYGOGameplayTags::Event_Montage_HitWindowBegin,
			GGYGOGameplayTags::Event_Montage_HitWindowEnd);
		Window->NotifyStateBehaviorFlags = static_cast<uint8>(EAnimNotifyStateBehaviorFlags::NoMergeOnConcurrentPlay);
		FAnimNotifyEvent Event;
		Event.NotifyStateClass = Window;
		Event.MontageTickType = EMontageNotifyTickType::Queued;
		Event.Link(Montage, 0.0f);
		Event.SetDuration(Montage->GetPlayLength());
		Montage->Notifies.Add(Event);
		static_cast<UAnimSequenceBase*>(Montage)->RefreshCacheData();
		return true;
	}
	/** 使用原资产事件和真实原实例上下文进入生产 Notify 发送端，不手拼 ASC 认证负载。 */
	bool SendHitWindowForTest(bool bOpen, int32 HistoricalInstanceId = INDEX_NONE, int32 WindowOrdinal = 0) const
	{
		if (WindowOrdinal < 0) { return false; }
		FGGYGOMontageSectionSnapshot Snapshot;
		const UGGYGOAbilityTask_PlayMontageAndWaitForEvent* OriginalTask = GetMontageTask();
		if (!OriginalTask || !Mesh || !Montage || !OriginalTask->TryGetOriginalSectionSnapshot(Snapshot)
			|| Snapshot.Montage != Montage || Snapshot.MontageInstanceId == INDEX_NONE) { return false; }
		FAnimNotifyEvent* WindowEvent = nullptr;
		int32 HitWindowIndex = 0;
		for (FAnimNotifyEvent& Event : Montage->Notifies)
		{
			const auto* Window = Cast<UGGYGOAnimNotifyState_GameplayEventWindow>(Event.NotifyStateClass.Get());
			if (Window && Window->GetBeginEventTag() == GGYGOGameplayTags::Event_Montage_HitWindowBegin
				&& Window->GetEndEventTag() == GGYGOGameplayTags::Event_Montage_HitWindowEnd)
			{
				if (HitWindowIndex++ == WindowOrdinal) { WindowEvent = &Event; break; }
			}
		}
		if (!WindowEvent) { return false; }
		auto* Window = CastChecked<UGGYGOAnimNotifyState_GameplayEventWindow>(WindowEvent->NotifyStateClass.Get());
		FAnimNotifyEventReference Reference(WindowEvent, Montage);
		Reference.AddContextData<UE::Anim::FAnimNotifyMontageInstanceContext>(
			HistoricalInstanceId != INDEX_NONE ? HistoricalInstanceId : Snapshot.MontageInstanceId);
		if (bOpen) { Window->NotifyBegin(Mesh, Montage, WindowEvent->GetDuration(), Reference); }
		else { Window->NotifyEnd(Mesh, Montage, Reference); }
		return true;
	}
	bool WereAllResourcesCleanedBeforeBroadcast(
		const UGGYGOAbilityTask_PlayMontageAndWaitForEvent* EndingMontageTask,
		const UGGYGOAbilityTask_WaitComboInput* EndingInputTask) const
	{
		return Ability && !Ability->StepMotionResources.IsValid() && !Ability->ActiveMesh && !Ability->GetMontageTask() && !Ability->InputTask
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
			&& !EndingMontageTask->NotifyReceived.IsBound()
			&& AnimInstance && !AnimInstance->OnPlayMontageNotifyBegin.Contains(
				EndingMontageTask, FName(TEXT("OnOriginalMontageNotifyBegin")))
			&& EndingInputTask && !EndingInputTask->IsActive()
			&& !EndingInputTask->OnPress.IsBound()
			&& Mesh && Mesh->VisibilityBasedAnimTickOption == EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered
			&& Mesh->bEnableUpdateRateOptimizations
			&& GetWatchdogRemaining() < 0.0f
			&& AnimInstance && !AnimInstance->Montage_IsActive(Montage);
	}
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* GetMontageTask() const
	{
		return Ability ? Ability->GetMontageTask() : nullptr;
	}
	UGGYGOAbilityTask_WaitComboInput* GetInputTask() const
	{
		return Ability ? Ability->InputTask.Get() : nullptr;
	}
	float GetWatchdogRemaining() const
	{
		return World && Ability ? World->GetTimerManager().GetTimerRemaining(Ability->GetWatchdogHandle()) : -1.0f;
	}
	FTimerHandle GetWatchdogHandle() const
	{
		return Ability ? Ability->GetWatchdogHandle() : FTimerHandle{};
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
		const bool bMontageTaskCleared = !Ability->GetMontageTask();
		const bool bStepMotionCleared = !Ability->StepMotionResources.IsValid();
		const bool bInputTaskCleared = !Ability->InputTask;
		const bool bTraceCleared = !Ability->TraceComponent;
		const bool bActivationCleared = !Ability->ResourceActivation.HasActivation();
		const bool bWorldCleared = Ability->OriginalWorld.IsExplicitlyNull();
		const bool bMontageCallbackCleared = !Ability->HasMontageCallbackRegistration();
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
	void SetCompletionGraceForTest(float Seconds) const { Ability->MontageCompletionGraceSeconds = Seconds; }
	float GetExpectedCompletionTailBudget() const
	{
		const UGGYGOAbilityTask_PlayMontageAndWaitForEvent* const Task = GetMontageTask();
		FGGYGOMontageSectionSnapshot Snapshot;
		if (!Task || !Task->TryGetOriginalSectionSnapshot(Snapshot) || !AnimInstance || !Montage || !Ability) { return -1.0f; }
		const FAnimMontageInstance* const Playback = AnimInstance->GetMontageInstanceForID(Snapshot.MontageInstanceId);
		return Playback && Playback->Montage == Montage
			? Montage->GetDefaultBlendOutTime() * Playback->DefaultBlendTimeMultiplier + Ability->MontageCompletionGraceSeconds
			: -1.0f;
	}
	float GetExpectedWatchdogRemainingFromCurrentTask() const
	{
		const UGGYGOAbilityTask_PlayMontageAndWaitForEvent* const Task = GetMontageTask();
		const float EffectiveRate = Task ? Task->GetEffectivePlayRate() : 0.0f;
		const float TailBudget = GetExpectedCompletionTailBudget();
		return Montage && FMath::IsFinite(EffectiveRate) && EffectiveRate > 0.0f && FMath::IsFinite(TailBudget) && TailBudget >= 0.0f
			? Montage->GetPlayLength() / EffectiveRate + TailBudget
			: -1.0f;
	}
	float GetExpectedWatchdogRemainingFromStartPosition(float StartPosition) const
	{
		const UGGYGOAbilityTask_PlayMontageAndWaitForEvent* const Task = GetMontageTask();
		if (!Montage || !Task || !FMath::IsFinite(StartPosition)
			|| StartPosition < 0.0f || StartPosition > Montage->GetPlayLength()) { return -1.0f; }
		const float EffectiveRate = Task->GetEffectivePlayRate();
		const float TailBudget = GetExpectedCompletionTailBudget();
		return FMath::IsFinite(EffectiveRate) && EffectiveRate > 0.0f && FMath::IsFinite(TailBudget) && TailBudget >= 0.0f
			? FMath::Max(0.0f, Montage->GetPlayLength() - StartPosition) / EffectiveRate + TailBudget
			: -1.0f;
	}
};

namespace
{
	struct FGGYGOComboLifecycleTestWorld
	{
		UEngine* Engine = nullptr;
		UWorld* World = nullptr;

		TStrongObjectPtr<UGameInstance> GameInstance;

		explicit FGGYGOComboLifecycleTestWorld(UEngine* InEngine, bool bCreatePhysicsScene = false, bool bWithMessageRouter = false)
			: Engine(InEngine)
		{
			if (bWithMessageRouter)
			{
				GameInstance.Reset(NewObject<UGameInstance>(Engine));
				if (GameInstance.IsValid())
				{
					GameInstance->InitializeStandalone(FName(TEXT("GGYGOComboRuntimeDamageWorld")));
					World = GameInstance->GetWorld();
				}
				return;
			}
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
				// 原Router随GI保留到演员和组件退出，最后再退休WorldContext。
				World->DestroyWorld(false);
				if (GameInstance.IsValid()) { GameInstance->Shutdown(); }
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOPlayerComboObservationSourceTest,
	"GGYGO.AbilitySystem.PlayerCombo.Observation.OriginalSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOPlayerComboObservationSourceTest::RunTest(const FString& Parameters)
{
	FGGYGOComboLifecycleTestWorld TestWorld(GEngine);
	FGGYGOPlayerComboLifecycleFixture Fixture;
	if (!InitializeFixture(*this, TestWorld, Fixture)) { return false; }
	FGGYGOComboStep& Step = Fixture.UseSingleStepForTest();
	Step.InterruptionMode = EGGYGOActionInterruptionMode::Disabled;
	Step.InterruptionNotifyName = NAME_None;
	Step.NextStepIndex = INDEX_NONE;
	FGGYGOPlayerComboObservation Observation;
	FGGYGOMontageSectionSnapshot Snapshot;
	FString Error;
	TestFalse(TEXT("未激活不能捕获原段"), Fixture.Ability->CaptureOriginalStepObservation(Observation, Snapshot, Error));
	TestFalse(TEXT("CDO不能签发原源"), GetMutableDefault<UGGYGOPlayerComboLifecycleTestAbility>()
		->CaptureOriginalStepObservation(Observation, Snapshot, Error));
	const auto Activation = Fixture.ASC->TryActivateAbilityWithTerminationBoundary(Fixture.AbilityHandle);
	if (!TestTrue(TEXT("真实受控 GAS 启动"), Activation.bNativeAccepted && Activation.OriginalActivation.HasActivation())
		|| !TestTrue(TEXT("捕获实际原Task快照"), Fixture.Ability->CaptureOriginalStepObservation(Observation, Snapshot, Error)))
	{
		Fixture.Ability->FinishForTest();
		return false;
	}
	FGGYGOPlayerComboObservationFacts Facts;
	TestTrue(TEXT("原历史来源可读"), Observation.ReadHistory(Facts));
	TestTrue(TEXT("来源精确关联原proof/Step/Task/Mesh/World/CMC"),
		Facts.Activation.HasSameActivation(Activation.OriginalActivation) && Facts.StepToken != 0 && Facts.StepIndex == 0
		&& Facts.Ability.Get() == Fixture.Ability && Facts.ASC.Get() == Fixture.ASC && Facts.Pawn.Get() == Fixture.Character
		&& Facts.Mesh.Get() == Fixture.Character->GetMesh() && Facts.World.Get() == TestWorld.World
		&& Facts.Task.Get() == Fixture.GetMontageTask() && Facts.Montage.Get() == Fixture.Montage
		&& Facts.Movement.Get() == Fixture.Movement && Facts.MontageInstanceId == Snapshot.MontageInstanceId
		&& Facts.MotionHandle != INDEX_NONE);
	bool bResourceSourceCurrent = false;
	TestTrue(TEXT("活原源实时快照及GA自有源分别认证"),
		Fixture.Ability->ReadOriginalStepObservation(Observation, Snapshot, Error, &bResourceSourceCurrent)
		&& bResourceSourceCurrent);
	bool bBeforeCleanupHistory = false;
	Fixture.Ability->SetBeforeCleanupActionForTest([&]
	{
		FGGYGOPlayerComboObservationFacts Before;
		bBeforeCleanupHistory = Observation.ReadHistory(Before) && !Before.bCleanupObserved
			&& Before.Activation.HasSameActivation(Activation.OriginalActivation);
	});
	bool bMatchedCompleted = false;
	const auto Completion = Fixture.ASC->OnAbilityTerminationCompleted().AddLambda(
		[&](const FGGYGOAbilityTerminationCompletedNotice& Notice)
		{
			FGGYGOPlayerComboObservationFacts Ended;
			bMatchedCompleted = Notice.HasCompletion() && Observation.ReadHistory(Ended) && Ended.bCleanupObserved
				&& Notice.GetOriginal().GetOriginalActivation().HasSameActivation(Ended.Activation);
		});
	const auto End = Fixture.Ability->RequestAbilityEnd(Activation.OriginalActivation, false, false);
	Fixture.ASC->OnAbilityTerminationCompleted().Remove(Completion);
	TestTrue(TEXT("cleanup入口之前不提前签发cleanup历史"), bBeforeCleanupHistory);
	TestTrue(TEXT("原完整退出在cleanup之后且匹配固定proof"), bMatchedCompleted
		&& End.Outcome == EGGYGOAbilityTerminationOutcome::Completed);
	TestTrue(TEXT("已退役原历史仍读回"), Observation.ReadHistory(Facts) && Facts.bCleanupObserved
		&& !Facts.bTaskNaturalEnded && !Facts.bConsumerAccepted && !Facts.bDrainAccepted);
	Snapshot.Montage = Fixture.Montage;
	TestFalse(TEXT("退出后live快照拒绝"),
		Fixture.Ability->ReadOriginalStepObservation(Observation, Snapshot, Error, &bResourceSourceCurrent));
	TestTrue(TEXT("失败live输出及GA源事实完整清空"),
		!Snapshot.Montage && Snapshot.MontageInstanceId == INDEX_NONE && !bResourceSourceCurrent);
	const auto Successor = Fixture.ASC->TryActivateAbilityWithTerminationBoundary(Fixture.AbilityHandle);
	if (TestTrue(TEXT("同资产同实例后继真实启动"), Successor.bNativeAccepted && Successor.OriginalActivation.HasActivation()))
	{
		TestFalse(TEXT("旧视图不改绑同资产后继"), Fixture.Ability->ReadOriginalStepObservation(Observation, Snapshot, Error));
		FGGYGOPlayerComboObservation NewObservation;
		FGGYGOPlayerComboObservationFacts NewFacts;
		TestTrue(TEXT("后继显式新捕获具有不同proof及StepToken"),
			Fixture.Ability->CaptureOriginalStepObservation(NewObservation, Snapshot, Error) && NewObservation.ReadHistory(NewFacts)
			&& !NewFacts.Activation.HasSameActivation(Facts.Activation) && NewFacts.StepToken != Facts.StepToken);
		Fixture.Ability->FinishForTest();
	}
	Observation.Reset();
	TestFalse(TEXT("显式清理原视图，清空历史输出"), Observation.ReadHistory(Facts));
	TestFalse(TEXT("清理不保留原proof副本"), Facts.Activation.HasActivation());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOPlayerComboObservationRejectedDrainTest,
	"GGYGO.AbilitySystem.PlayerCombo.Observation.RejectedDrain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOPlayerComboObservationRejectedDrainTest::RunTest(const FString& Parameters)
{
	FGGYGOComboLifecycleTestWorld TestWorld(GEngine);
	FGGYGOPlayerComboLifecycleFixture Fixture;
	if (!InitializeFixture(*this, TestWorld, Fixture)) { return false; }
	FGGYGOComboStep& Step = Fixture.UseSingleStepForTest();
	Step.InterruptionMode = EGGYGOActionInterruptionMode::Disabled;
	Step.InterruptionNotifyName = NAME_None;
	Step.NextStepIndex = INDEX_NONE;
	const auto Activation = Fixture.ASC->TryActivateAbilityWithTerminationBoundary(Fixture.AbilityHandle);
	FGGYGOPlayerComboObservation Observation;
	FGGYGOMontageSectionSnapshot Snapshot;
	FString Error;
	if (!TestTrue(TEXT("实际启动并捕获原源"), Activation.bNativeAccepted
		&& Fixture.Ability->CaptureOriginalStepObservation(Observation, Snapshot, Error)))
	{
		Fixture.Ability->FinishForTest();
		return false;
	}
	FGGYGOPlayerComboObservationFacts Original;
	if (!TestTrue(TEXT("原来源可读"), Observation.ReadHistory(Original))) { Fixture.Ability->FinishForTest(); return false; }
	FGGYGOActionMotionSourceBindingPtr Source;
	if (!TestTrue(TEXT("只读构造完整原Montage source"), GGYGOActionMotionSource::BuildSourceBindingRange(
		Fixture.Montage, Step.MotionSlotName, Step.MainSection, Step.EndSection, Source, Error)))
	{
		Fixture.Ability->FinishForTest(); return false;
	}
	// 真实公开CMC入口退役两个令牌，使原GA的已发令牌确实过期；不写CMC内部状态或伪造返回。
	if (!TestTrue(TEXT("公开取消原CMC资源"), Fixture.Movement->ReleaseMontageActionMotion(
		Original.MotionHandle, EGGYGOActionMotionReleaseReason::Cancelled, Error)))
	{ Fixture.Ability->FinishForTest(); return false; }
	int32 OtherHandle = INDEX_NONE;
	if (!TestTrue(TEXT("公开入口创建另一真实CMC令牌"), Fixture.Movement->BeginMontageActionMotion(Source,
		Snapshot.MontageInstanceId, Snapshot.PositionSeconds, Snapshot.InstancePlayRate * Source->MontageRateScale,
		Step.MotionTranslationScale, OtherHandle, Error) && OtherHandle != Original.MotionHandle)
		|| !TestTrue(TEXT("公开退役另一令牌，使原令牌不再是末次幂等令牌"), Fixture.Movement->ReleaseMontageActionMotion(
			OtherHandle, EGGYGOActionMotionReleaseReason::Cancelled, Error)))
	{ Fixture.Ability->FinishForTest(); return false; }
	FGGYGOMontageSectionSnapshot Current;
	bool bResourceSourceCurrent = false;
	if (!TestTrue(TEXT("仅CMC令牌过期，原GA/Task/Mesh/实例仍认证"),
		Fixture.Ability->ReadOriginalStepObservation(Observation, Current, Error, &bResourceSourceCurrent)
		&& bResourceSourceCurrent))
	{ Fixture.Ability->FinishForTest(); return false; }
	FAnimMontageInstance* Instance = Fixture.AnimInstance->GetMontageInstanceForID(Original.MontageInstanceId);
	if (!TestTrue(TEXT("原实例native End delegate确属原Task"), Instance
		&& Instance->OnMontageEnded.GetUObject() == Fixture.GetMontageTask()))
	{ Fixture.Ability->FinishForTest(); return false; }
	// 已绑定原生回调探针，不推进动画时间，不是生产自然结尾／PIE验收。
	const FOnMontageEnded OriginalEnd = Instance->OnMontageEnded;
	bool bRejectedBeforeCleanup = false;
	Fixture.Ability->SetBeforeCleanupActionForTest([&]
	{
		FGGYGOPlayerComboObservationFacts During;
		bRejectedBeforeCleanup = Observation.ReadHistory(During) && During.bTaskNaturalEnded
			&& During.bConsumerAccepted && During.bDrainRequestReturned && !During.bDrainAccepted && !During.bCleanupObserved;
	});
	bool bCancelledCompletion = false;
	const auto Completion = Fixture.ASC->OnAbilityTerminationCompleted().AddLambda(
		[&](const FGGYGOAbilityTerminationCompletedNotice& Notice)
		{
			bCancelledCompletion = Notice.HasCompletion()
				&& Notice.GetOriginal().GetOriginalActivation().HasSameActivation(Original.Activation)
				&& Notice.GetOriginal().WasCancelled();
		});
	const FString ExpectedTokenDiagnostic = FString::Printf(
		TEXT("[Movement.ActionMotion] token=%d is not the original active/completed resource"), Original.MotionHandle);
	const FString ExpectedCleanupDiagnostic = FString::Printf(
		TEXT("MontageAction [%s] original handle %d cleanup failed: %s"),
		*Fixture.Ability->GetPathName(), Original.MotionHandle, *ExpectedTokenDiagnostic);
	AddExpectedError(TEXT("Original Task completed but its CMC natural drain was rejected"), EAutomationExpectedErrorFlags::Contains, 1);
	// 夹具刻意使原令牌过期；故障 cleanup 也须拒绝它，按原实例全文精确校验一次诊断。
	AddExpectedErrorPlain(ExpectedCleanupDiagnostic, EAutomationExpectedErrorFlags::Exact, 1);
	OriginalEnd.ExecuteIfBound(Fixture.Montage, false);
	Fixture.ASC->OnAbilityTerminationCompleted().Remove(Completion);
	FGGYGOPlayerComboObservationFacts Failed;
	TestTrue(TEXT("真实拒绝返回在cleanup之前记录"), bRejectedBeforeCleanup);
	TestTrue(TEXT("原GA因真实CMC拒绝完整故障退出"), bCancelledCompletion && !Fixture.Ability->IsActive());
	TestTrue(TEXT("Task/bMontageCompleted不能补成自然成功"), Observation.ReadHistory(Failed)
		&& Failed.bTaskNaturalEnded && Failed.bConsumerAccepted && Failed.bDrainRequestReturned
		&& !Failed.bDrainAccepted && !Failed.bMotionCompleted && Failed.bCleanupObserved && Failed.bCleanupCancelled
		&& !Failed.DrainDiagnostic.IsEmpty());
	TestEqual(TEXT("真实drain拒绝诊断精确关联原令牌"), Failed.DrainDiagnostic, ExpectedTokenDiagnostic);
	Fixture.Ability->FinishForTest();
	return true;
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
	TestTrue(TEXT("既有动作默认仍要求真实攻击接续作者点"),
		Original.InterruptionMode == EGGYGOActionInterruptionMode::AuthoredNotify
		&& Original.InterruptionNotifyName == FName(TEXT("Event.Montage.CancelPoint")));
	TestTrue(TEXT("移动打断默认 Disabled/None，旧序列化数据不自动获得移动门"),
		Original.MovementInterruptionMode == EGGYGOActionInterruptionMode::Disabled
		&& Original.MovementInterruptionNotifyName.IsNone());
	TestTrue(TEXT("历史起终点默认仍为空"), Original.TraceStartSocket.IsNone() && Original.TraceEndSocket.IsNone());
	TestEqual(TEXT("历史半径默认仍为 20"), Original.TraceRadius, 20.0f);
	Original.TraceStartSocket = TEXT("StoredLegacyStart");
	Original.TraceEndSocket = TEXT("StoredLegacyEnd");
	Original.TraceRadius = 37.25f;
	Original.MotionSlotName = TEXT("StoredMotionSlot");
	Original.MotionTranslationScale = 0.75f;
	Original.InterruptionMode = EGGYGOActionInterruptionMode::Disabled;
	Original.InterruptionNotifyName = NAME_None;
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
	TestTrue(TEXT("显式关闭打断模式留存"), RoundTrip.InterruptionMode == Original.InterruptionMode);
	TestEqual(TEXT("显式关闭通知名留存"), RoundTrip.InterruptionNotifyName, Original.InterruptionNotifyName);
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
	TestEqual(TEXT("正常路径实际 Montage 播放一次"), Fixture.AnimInstance->GetSuccessfulGuardReturnCountForTest(), 1);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOPlayerSingleActionInterruptionTest,
	"GGYGO.AbilitySystem.PlayerCombo.Interruption.ExplicitDisabled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOPlayerSingleActionInterruptionTest::RunTest(const FString& Parameters)
{
	FGGYGOComboLifecycleTestWorld TestWorld(GEngine);
	FGGYGOPlayerComboLifecycleFixture Fixture;
	if (!InitializeFixture(*this, TestWorld, Fixture)) { return false; }
	FString Error;
	if (!TestTrue(TEXT("既有作者点配置可用"), Fixture.Ability->ValidateComboConfiguration(Error))) { return false; }
	FGGYGOComboStep& Step = Fixture.UseSingleStepForTest();
	Step.NextStepIndex = INDEX_NONE;
	TestTrue(TEXT("攻击作者点＋默认移动 Disabled/None 可用"), Fixture.Ability->ValidateComboConfiguration(Error));
	Step.MovementInterruptionNotifyName = TEXT("GGYGO.Test.MovementPoint");
	TestFalse(TEXT("移动 Disabled 不能保留未使用通知名"), Fixture.Ability->ValidateComboConfiguration(Error));
	Step.MovementInterruptionMode = EGGYGOActionInterruptionMode::AuthoredNotify;
	Step.MovementInterruptionNotifyName = NAME_None;
	TestFalse(TEXT("AuthoredNotify 移动门必须配置名字"), Fixture.Ability->ValidateComboConfiguration(Error));
	Step.MovementInterruptionNotifyName = Step.InterruptionNotifyName;
	TestFalse(TEXT("移动点不能与攻击点同名而跨权限授权"), Fixture.Ability->ValidateComboConfiguration(Error));
	Step.MovementInterruptionNotifyName = TEXT("GGYGO.Test.MissingMovementPoint");
	TestFalse(TEXT("缺失的移动作者点明确拒绝"), Fixture.Ability->ValidateComboConfiguration(Error));
	Step.MovementInterruptionMode = static_cast<EGGYGOActionInterruptionMode>(99);
	TestFalse(TEXT("未知移动打断模式拒绝"), Fixture.Ability->ValidateComboConfiguration(Error));
	Step.MovementInterruptionMode = EGGYGOActionInterruptionMode::Disabled;
	Step.MovementInterruptionNotifyName = NAME_None;
	Fixture.Montage->Notifies.Reset();
	TestFalse(TEXT("要求作者点的动作不能因点缺失自动关闭打断"), Fixture.Ability->ValidateComboConfiguration(Error));

	Step.InterruptionMode = EGGYGOActionInterruptionMode::Disabled;
	TestFalse(TEXT("关闭模式不能仍声明未使用通知"), Fixture.Ability->ValidateComboConfiguration(Error));
	Step.InterruptionNotifyName = NAME_None;
	TestTrue(TEXT("单段显式关闭打断无需伪造作者点"), Fixture.Ability->ValidateComboConfiguration(Error));
	Step.NextStepIndex = 0;
	TestFalse(TEXT("关闭打断时不得配置攻击接续"), Fixture.Ability->ValidateComboConfiguration(Error));
	Step.NextStepIndex = INDEX_NONE;
	Step.InterruptionMode = static_cast<EGGYGOActionInterruptionMode>(99);
	TestFalse(TEXT("未知打断模式拒绝"), Fixture.Ability->ValidateComboConfiguration(Error));
	Step.InterruptionMode = EGGYGOActionInterruptionMode::AuthoredNotify;
	TestFalse(TEXT("恢复作者模式后 None 名称仍为必需配置错误"), Fixture.Ability->ValidateComboConfiguration(Error));

	Step.InterruptionMode = EGGYGOActionInterruptionMode::Disabled;
	const FGGYGOAbilityActivationRequestResult Activation =
		Fixture.ASC->TryActivateAbilityWithTerminationBoundary(Fixture.AbilityHandle);
	if (!TestTrue(TEXT("无作者点的显式关闭动作通过真实 GAS 启动"), Activation.bNativeAccepted
		&& Activation.Outcome == EGGYGOAbilityActivationRequestOutcome::Accepted
		&& Activation.OriginalActivation.HasActivation())) { return false; }
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* const OriginalMontageTask = Fixture.GetMontageTask();
	TestTrue(TEXT("启动仍持有原 Montage Task"), IsValid(OriginalMontageTask) && OriginalMontageTask->IsActive());
	TestTrue(TEXT("启动仍签发原 CMC 动作资源"), Fixture.Movement->HasActiveActionMotion());
	TestTrue(TEXT("启动仍持有 GA 原动作资源"), Fixture.HasStepMotionResources());
	TestFalse(TEXT("攻击与移动双 Disabled 不打开任一打断权限"), Fixture.Ability->IsInterruptionOpen()
		|| Fixture.Ability->IsAttackContinuationOpen() || Fixture.Ability->IsMovementInterruptionOpen());
	TestEqual(TEXT("未完成的动作保持当前唯一段"), Fixture.Ability->GetCurrentComboStep(), 0);
	const FGGYGOAbilityTerminationResult Cleanup =
		Fixture.Ability->RequestAbilityEnd(Activation.OriginalActivation, false, false);
	TestTrue(TEXT("按原激活正常清理测试动作"), Cleanup.Outcome == EGGYGOAbilityTerminationOutcome::Completed
		&& Cleanup.Original.GetOriginalActivation().HasSameActivation(Activation.OriginalActivation));
	TestFalse(TEXT("清理释放原 GA 动作资源"), Fixture.HasStepMotionResources());
	TestFalse(TEXT("清理释放原 CMC 动作资源"), Fixture.Movement->HasActiveActionMotion());
	TestEqual(TEXT("清理退役所有原任务"), Fixture.Ability->GetActiveTaskCountForTest(), 0);
	AddInfo(TEXT("本项验证真实启动和原资源清理；完整自然结束仍需原生动作冒烟。"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOPlayerComboAttackPointOnlyOpensAttackTest,
	"GGYGO.AbilitySystem.PlayerCombo.Interruption.AttackPointOnlyOpensAttack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOPlayerComboAttackPointOnlyOpensAttackTest::RunTest(const FString& Parameters)
{
	FGGYGOComboLifecycleTestWorld TestWorld(GEngine);
	FGGYGOPlayerComboLifecycleFixture Fixture;
	if (!InitializeFixture(*this, TestWorld, Fixture)) { return false; }
	FString Error;
	const FGGYGOComboStep& Step = Fixture.GetFirstStepForTest();
	if (!TestTrue(TEXT("攻击作者点＋默认移动 Disabled/None 的既有配置可用"),
		Step.MovementInterruptionMode == EGGYGOActionInterruptionMode::Disabled
		&& Step.MovementInterruptionNotifyName.IsNone() && Fixture.Ability->ValidateComboConfiguration(Error))) { return false; }
	const FGGYGOAbilityActivationRequestResult Activation =
		Fixture.ASC->TryActivateAbilityWithTerminationBoundary(Fixture.AbilityHandle);
	if (!TestTrue(TEXT("通过真实 GAS 启动"), Activation.bNativeAccepted
		&& Activation.Outcome == EGGYGOAbilityActivationRequestOutcome::Accepted
		&& Activation.OriginalActivation.HasActivation())) { return false; }
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* const OriginalTask = Fixture.GetMontageTask();
	FGGYGOMontageSectionSnapshot Snapshot;
	if (!TestTrue(TEXT("原 Task 原实例可读"), IsValid(OriginalTask) && OriginalTask->TryGetOriginalSectionSnapshot(Snapshot)
		&& Snapshot.Montage == Fixture.Montage && Snapshot.MontageInstanceId != INDEX_NONE)) { return false; }
	TestFalse(TEXT("起播未越过攻击点时两门均关闭"),
		Fixture.Ability->IsAttackContinuationOpen() || Fixture.Ability->IsMovementInterruptionOpen());
	FAnimNotifyEvent* PointEvent = nullptr;
	for (FAnimNotifyEvent& Event : Fixture.Montage->Notifies)
	{
		if (Cast<UAnimNotify_PlayMontageNotify>(Event.Notify.Get())) { PointEvent = &Event; break; }
	}
	if (!TestNotNull(TEXT("夹具有唯一原生攻击点"), PointEvent)) { return false; }
	// 原生入口：AnimInstance 广播 → 原 Task 认证原实例 → GA 消费；不直接写 GA 门状态。
	const FBranchingPointNotifyPayload Payload(Fixture.Mesh, Fixture.Montage, PointEvent, Snapshot.MontageInstanceId);
	Fixture.AnimInstance->OnPlayMontageNotifyBegin.Broadcast(Step.InterruptionNotifyName, Payload);
	TestTrue(TEXT("原实例攻击点打开攻击接续门，保留的 IsInterruptionOpen 表示同一攻击事实"),
		Fixture.Ability->IsAttackContinuationOpen() && Fixture.Ability->IsInterruptionOpen());
	TestFalse(TEXT("攻击点不打开移动打断门"), Fixture.Ability->IsMovementInterruptionOpen());
	TestTrue(TEXT("攻击点不取消原 CMC 轨迹，也不结束原能力"), Fixture.Movement->HasActiveActionMotion()
		&& Fixture.Ability->IsActive() && Fixture.GetMontageTask() == OriginalTask && Fixture.HasStepMotionResources()
		&& Fixture.Ability->CaptureCurrentActivation().HasSameActivation(Activation.OriginalActivation));
	const FGGYGOAbilityTerminationResult Cleanup =
		Fixture.Ability->RequestAbilityEnd(Activation.OriginalActivation, false, false);
	TestTrue(TEXT("按原激活正常清理"), Cleanup.Outcome == EGGYGOAbilityTerminationOutcome::Completed);
	TestFalse(TEXT("清理后两门不残留"),
		Fixture.Ability->IsAttackContinuationOpen() || Fixture.Ability->IsMovementInterruptionOpen());
	TestFalse(TEXT("清理释放原 GA 动作资源"), Fixture.HasStepMotionResources());
	TestFalse(TEXT("清理释放原 CMC 动作资源"), Fixture.Movement->HasActiveActionMotion());
	AddInfo(TEXT("本项经原 Task 的原生委托认证入口核对攻击点不授予移动；未自然 Advance，不替代双点与生产 Held 冒烟。"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOPlayerComboMovementPointConfigurationTest,
	"GGYGO.AbilitySystem.PlayerCombo.Interruption.MovementPointConfiguration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOPlayerComboMovementPointConfigurationTest::RunTest(const FString& Parameters)
{
	FGGYGOComboLifecycleTestWorld TestWorld(GEngine);
	FGGYGOPlayerComboLifecycleFixture Fixture;
	if (!InitializeFixture(*this, TestWorld, Fixture)) { return false; }
	FGGYGOComboStep& Step = Fixture.UseSingleStepForTest();
	Step.NextStepIndex = INDEX_NONE;
	const float MainEnd = Fixture.Montage->GetAnimCompositeSection(Fixture.Montage->GetSectionIndex(Step.EndSection)).GetTime();
	if (!TestTrue(TEXT("以当前 Section 时间声明独立移动点"), Fixture.AddMovementPointForTest(MainEnd))) { return false; }
	FAnimNotifyEvent* MovementPoint = Fixture.FindAuthoredPointForTest(Step.MovementInterruptionNotifyName);
	FAnimNotifyEvent* AttackPoint = Fixture.FindAuthoredPointForTest(Step.InterruptionNotifyName);
	if (!TestNotNull(TEXT("原攻击点保持"), AttackPoint) || !TestNotNull(TEXT("独立移动点存在"), MovementPoint)) { return false; }
	TestEqual(TEXT("移动作者时间精确等于 Section Main 末端"), MovementPoint->GetTime(), MainEnd, 0.0f);
	TestTrue(TEXT("引擎段界 OffsetBefore 保持且真实 trigger 早于 Main 末端"), MovementPoint->GetTriggerTime() < MainEnd);
	FString Error;
	TestTrue(TEXT("两个不同名/不同真实 trigger 的合法点通过配置核验"), Fixture.Ability->ValidateComboConfiguration(Error));

	MovementPoint->Link(Fixture.Montage, MainEnd * 0.75f);
	MovementPoint->RefreshTriggerOffset(Fixture.Montage->CalculateOffsetForNotify(MovementPoint->GetTime()));
	static_cast<UAnimSequenceBase*>(Fixture.Montage)->RefreshCacheData();
	TestFalse(TEXT("移动作者点提前于真实 Main 末端必须拒绝"), Fixture.Ability->ValidateComboConfiguration(Error));
	// RefreshCacheData 可排序 Notifies；重新按名字取元素，不能沿排序前的数组地址改另一个点。
	MovementPoint = Fixture.FindAuthoredPointForTest(Step.MovementInterruptionNotifyName);
	if (!TestNotNull(TEXT("排序后仍按名字定位移动点"), MovementPoint)) { return false; }
	MovementPoint->Link(Fixture.Montage, MainEnd);
	MovementPoint->RefreshTriggerOffset(Fixture.Montage->CalculateOffsetForNotify(MainEnd));
	static_cast<UAnimSequenceBase*>(Fixture.Montage)->RefreshCacheData();
	TestTrue(TEXT("恢复真实 Section 边界后配置重新有效"), Fixture.Ability->ValidateComboConfiguration(Error));
	MovementPoint = Fixture.FindAuthoredPointForTest(Step.MovementInterruptionNotifyName);
	AttackPoint = Fixture.FindAuthoredPointForTest(Step.InterruptionNotifyName);
	if (!TestNotNull(TEXT("恢复后的移动点"), MovementPoint) || !TestNotNull(TEXT("恢复后的原攻击点"), AttackPoint)) { return false; }

	// 构造实际 trigger 相等的非法声明；这是负例，不把同名或时间碰撞当合法播放。
	const float NativeOffset = MovementPoint->TriggerTimeOffset;
	MovementPoint->TriggerTimeOffset = AttackPoint->TriggerTimeOffset;
	TestEqual(TEXT("负例的两个实际 trigger 精确重合"), MovementPoint->GetTriggerTime(), AttackPoint->GetTriggerTime(), 0.0f);
	TestFalse(TEXT("真实 BranchingPoint trigger 碰撞必须拒绝"), Fixture.Ability->ValidateComboConfiguration(Error));
	MovementPoint->TriggerTimeOffset = NativeOffset;
	TestTrue(TEXT("恢复原生 offset 后配置有效"), Fixture.Ability->ValidateComboConfiguration(Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOPlayerComboAuthoredPointIndependenceTest,
	"GGYGO.AbilitySystem.PlayerCombo.Interruption.AuthoredPointIndependence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOPlayerComboAuthoredPointIndependenceTest::RunTest(const FString& Parameters)
{
	FGGYGOComboLifecycleTestWorld TestWorld(GEngine);
	FGGYGOPlayerComboLifecycleFixture Fixture;
	if (!InitializeFixture(*this, TestWorld, Fixture)) { return false; }
	FGGYGOComboStep& Step = Fixture.UseSingleStepForTest();
	Step.NextStepIndex = INDEX_NONE;
	const float MainEnd = Fixture.Montage->GetAnimCompositeSection(Fixture.Montage->GetSectionIndex(Step.EndSection)).GetTime();
	if (!TestTrue(TEXT("配置独立移动点"), Fixture.AddMovementPointForTest(MainEnd))) { return false; }
	const FName AttackName = Step.InterruptionNotifyName;
	const FName MovementName = Step.MovementInterruptionNotifyName;
	const FGGYGOAbilityActivationRequestResult Activation = Fixture.ASC->TryActivateAbilityWithTerminationBoundary(Fixture.AbilityHandle);
	if (!TestTrue(TEXT("真实 GAS 激活双点动作"), Activation.bNativeAccepted
		&& Activation.Outcome == EGGYGOAbilityActivationRequestOutcome::Accepted)) { return false; }
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* const Task = Fixture.GetMontageTask();
	FGGYGOMontageSectionSnapshot Snapshot;
	if (!TestTrue(TEXT("原 Task 原实例快照"), Task && Task->TryGetOriginalSectionSnapshot(Snapshot))) { return false; }
	FAnimNotifyEvent* const MovementPoint = Fixture.FindAuthoredPointForTest(MovementName);
	if (!TestNotNull(TEXT("Ready 后按名字取移动原生点"), MovementPoint)) { return false; }
	FAnimMontageInstance* const Instance = Fixture.AnimInstance->GetMontageInstanceForID(Snapshot.MontageInstanceId);
	if (!TestNotNull(TEXT("原生原 Montage 实例"), Instance)) { return false; }
	Instance->SetPosition(MovementPoint->GetTriggerTime());
	Fixture.AnimInstance->OnPlayMontageNotifyBegin.Broadcast(MovementName,
		FBranchingPointNotifyPayload(Fixture.Mesh, Fixture.Montage, MovementPoint, Snapshot.MontageInstanceId));
	if (!TestTrue(TEXT("早到移动 Fact 只 Pending：原能力与原 CMC 继续"), Fixture.Ability->IsActive()
		&& Fixture.Movement->HasActiveActionMotion() && Fixture.GetMontageTask() == Task)) { return false; }
	TestFalse(TEXT("OffsetBefore 在 Main 内不能打开移动门"), Fixture.Ability->IsMovementInterruptionOpen());
	TestFalse(TEXT("移动 Fact 不能代开攻击门"), Fixture.Ability->IsAttackContinuationOpen());

	// 原生委托可重入结束资源；回调后按固定原 ID 重新查询，不继续使用旧裸指针。
	FAnimMontageInstance* const OriginalAfterCallback = Fixture.AnimInstance->GetMontageInstanceForID(Snapshot.MontageInstanceId);
	FAnimNotifyEvent* const AttackPoint = Fixture.FindAuthoredPointForTest(AttackName);
	if (!TestNotNull(TEXT("回调后原实例仍在"), OriginalAfterCallback)
		|| !TestNotNull(TEXT("回调后攻击原生点"), AttackPoint)) { return false; }
	OriginalAfterCallback->SetPosition(MainEnd);
	Fixture.AnimInstance->OnPlayMontageNotifyBegin.Broadcast(AttackName,
		FBranchingPointNotifyPayload(Fixture.Mesh, Fixture.Montage, AttackPoint, Snapshot.MontageInstanceId));
	TestTrue(TEXT("原攻击 Fact 只打开攻击门"), Fixture.Ability->IsAttackContinuationOpen());
	TestFalse(TEXT("即使 livePosition 已达 Main 末端，攻击 Fact 也不能消费 Pending 移动门"), Fixture.Ability->IsMovementInterruptionOpen());
	TestTrue(TEXT("攻击门不查询/取消 CMC，原动作仍在"), Fixture.Ability->IsActive() && Fixture.Movement->HasActiveActionMotion());
	const FGGYGOAbilityTerminationResult Cleanup = Fixture.Ability->RequestAbilityEnd(Activation.OriginalActivation, false, false);
	TestTrue(TEXT("原激活幂等清理"), Cleanup.Outcome == EGGYGOAbilityTerminationOutcome::Completed);
	TestFalse(TEXT("清理后两门与 Pending 不残留"), Fixture.Ability->IsAttackContinuationOpen() || Fixture.Ability->IsMovementInterruptionOpen());
	TestFalse(TEXT("原 CMC 动作资源释放"), Fixture.Movement->HasActiveActionMotion());
	AddInfo(TEXT("本项驱动真实原实例位置与原 Task 的原生委托认证入口；未自然 Advance，不替代 QualifiedHeld 生产冒烟。"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOPlayerComboNonzeroNamedPointSnapshotTest,
	"GGYGO.AbilitySystem.PlayerCombo.Interruption.NonzeroNamedPointSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOPlayerComboNonzeroNamedPointSnapshotTest::RunTest(const FString& Parameters)
{
	FGGYGOComboLifecycleTestWorld TestWorld(GEngine);
	FGGYGOPlayerComboLifecycleFixture Fixture;
	if (!InitializeFixture(*this, TestWorld, Fixture)) { return false; }
	FGGYGOComboStep& Step = Fixture.UseSingleStepForTest();
	Step.NextStepIndex = INDEX_NONE;
	const int32 EndIndex = Fixture.Montage->GetSectionIndex(Step.EndSection);
	Fixture.Montage->GetAnimCompositeSection(EndIndex).Link(Fixture.Montage, 0.9f);
	static_cast<UAnimSequenceBase*>(Fixture.Montage)->RefreshCacheData();
	const float MainEnd = Fixture.Montage->GetAnimCompositeSection(EndIndex).GetTime();
	if (!TestTrue(TEXT("按当前 Section 时间配置移动点"), Fixture.AddMovementPointForTest(MainEnd))) { return false; }
	const FGGYGOAbilityActivationRequestResult Activation = Fixture.ASC->TryActivateAbilityWithTerminationBoundary(Fixture.AbilityHandle);
	if (!TestTrue(TEXT("真实 GAS 启动"), Activation.bNativeAccepted)) { return false; }
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* const FirstTask = Fixture.GetMontageTask();
	FGGYGOMontageSectionSnapshot FirstSnapshot;
	if (!TestTrue(TEXT("初始原实例"), FirstTask && FirstTask->TryGetOriginalSectionSnapshot(FirstSnapshot))) { return false; }
	if (!TestTrue(TEXT("生产 StartStep 从非零原生位置创建新播放"), Fixture.RestartFirstStepForTest(0.6f))) { return false; }
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* const RestartedTask = Fixture.GetMontageTask();
	FGGYGOMontageSectionSnapshot Restarted;
	TestTrue(TEXT("持有新固定原 ID，而非复用历史实例"), RestartedTask && RestartedTask != FirstTask
		&& RestartedTask->TryGetOriginalSectionSnapshot(Restarted) && Restarted.MontageInstanceId != FirstSnapshot.MontageInstanceId);
	TestTrue(TEXT("具名 liveSnapshot 恢复已过攻击点"), Fixture.Ability->IsAttackContinuationOpen());
	TestFalse(TEXT("尚未达到独立 MainEnd 的移动门保持关闭"), Fixture.Ability->IsMovementInterruptionOpen());
	TestTrue(TEXT("原能力与新 CMC 动作资源有效"), Fixture.Ability->IsActive() && Fixture.Movement->HasActiveActionMotion());
	const FGGYGOAbilityTerminationResult Cleanup = Fixture.Ability->RequestAbilityEnd(Activation.OriginalActivation, false, false);
	TestTrue(TEXT("原激活清理完成"), Cleanup.Outcome == EGGYGOAbilityTerminationOutcome::Completed);
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
	bBehaviorPassed &= TestEqual(TEXT("从未播放 Montage"), Fixture.AnimInstance->GetSuccessfulGuardReturnCountForTest(), 0);
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
		int32 NativeStartedCount = 0;
		int32 OriginalCreatedInstanceId = INDEX_NONE;
		int32 SuccessorCreatedInstanceId = INDEX_NONE;
		TStrongObjectPtr<UGGYGOMontageStartedDiagnosticObserver> StartedObserver(
			NewObject<UGGYGOMontageStartedDiagnosticObserver>(Fixture.Character));
		if (!TestNotNull(TEXT("真实 Started 观察者"), StartedObserver.Get())) { return false; }
		ON_SCOPE_EXIT
		{
			StartedObserver->DisarmForTest();
		};
		// 真实引擎创建回调仍在 Guard Executing 内；不把 Guard Returned 后的自定义钩子当成创建接缝。
		StartedObserver->ArmForTest(Fixture.AnimInstance, Fixture.Montage, [&]
		{
			++NativeStartedCount;
			if (const FAnimMontageInstance* Instance = Fixture.AnimInstance->GetActiveInstanceForMontage(Fixture.Montage))
			{
				OriginalCreatedInstanceId = Instance->GetInstanceID();
			}
			if (const UGGYGOAbilityTask_PlayMontageAndWaitForEvent* OldTask = Fixture.GetMontageTask())
			{
				OldEffectivePlayRate = OldTask->GetEffectivePlayRate();
			}
			// 原观察者在调用动作前已解绑，可复用它观察后继的真实创建，不递归执行原重入动作。
			StartedObserver->ArmForTest(Fixture.AnimInstance, Fixture.Montage, [&]
			{
				++NativeStartedCount;
				if (const FAnimMontageInstance* Instance = Fixture.AnimInstance->GetActiveInstanceForMontage(Fixture.Montage))
				{
					SuccessorCreatedInstanceId = Instance->GetInstanceID();
				}
			});
			Fixture.Ability->SetFirstPlayRateForTest(4.0f);
			bReadyCallbackReentered = Fixture.Ability->EndAndReactivateForTest();
		});
		if (!TestTrue(TEXT("Ready 重入初始激活"), Fixture.ASC->TryActivateAbility(Fixture.AbilityHandle))) { return false; }
		StartedObserver->DisarmForTest();
		TestTrue(TEXT("真实 OnMontageStarted 创建回调内同步 End/重激活"), bReadyCallbackReentered);
		TestEqual(TEXT("原生 Started 证明原播放与后继各创建一次"), NativeStartedCount, 2);
		TestTrue(TEXT("两次真实创建具有不同原生实例身份"), OriginalCreatedInstanceId != INDEX_NONE
			&& SuccessorCreatedInstanceId != INDEX_NONE && OriginalCreatedInstanceId != SuccessorCreatedInstanceId);
		TestTrue(TEXT("旧 Montage ReadyForActivation 栈返回后新激活仍活跃"), Fixture.Ability->IsActive());
		TestEqual(TEXT("旧 ReadyForActivation 栈没有重建任务"), Fixture.Ability->GetActiveTaskCountForTest(), 2);
		TestEqual(TEXT("旧 ReadyForActivation 栈没有覆盖新段"), Fixture.Ability->GetCurrentComboStep(), 0);
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent* const NewMontageTask = Fixture.GetMontageTask();
		if (!TestNotNull(TEXT("重激活保留其 Montage Task 速率快照"), NewMontageTask)
			|| !TestTrue(TEXT("旧 Montage Task 存在有限有效速率"),
				FMath::IsFinite(OldEffectivePlayRate) && OldEffectivePlayRate > 0.0f)) { return false; }
		const float NewExpectedWatchdog = Fixture.GetExpectedWatchdogRemainingFromCurrentTask();
		const float OldRateWatchdog = Fixture.Montage->GetPlayLength() / OldEffectivePlayRate + Fixture.GetExpectedCompletionTailBudget();
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
		if (!TestTrue(TEXT("结束重入场景启动前声明真实作者命中窗"), Fixture.AddHitWindowForTest())) { return false; }
		if (!TestTrue(TEXT("结束重入初始激活"), Fixture.ASC->TryActivateAbility(Fixture.AbilityHandle))) { return false; }
		if (!TestTrue(TEXT("结束重入场景由真实作者 Notify 开启 Owned Trace"), Fixture.SendHitWindowForTest(true))) { return false; }
		const FGGYGOMeleeTraceWindowHandle EndingTraceWindow = Fixture.GetTraceWindow();
		if (!TestTrue(TEXT("原激活在结束前持有准确的活动 Owned Trace"), EndingTraceWindow.HasWindow()
			&& Fixture.Trace->QueryOwnedTraceWindow(EndingTraceWindow) == EGGYGOMeleeTraceWindowQueryResult::Active
			&& Fixture.Trace->IsTracing())) { return false; }
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent* const EndingMontageTask = Fixture.GetMontageTask();
		UGGYGOAbilityTask_WaitComboInput* const EndingInputTask = Fixture.GetInputTask();
		bool bResourcesWereCleanBeforeBroadcast = false;
		bool bOriginalTraceWasClosedBeforeBroadcast = false;
		bool bReactivatedFromEndBroadcast = false;
		const FDelegateHandle EndHandle = Fixture.ASC->OnAbilityEnded.AddLambda([&](const FAbilityEndedData& Data)
		{
			if (Data.AbilityThatEnded == Fixture.Ability && !bReactivatedFromEndBroadcast)
			{
				bOriginalTraceWasClosedBeforeBroadcast = Fixture.Trace->QueryOwnedTraceWindow(EndingTraceWindow)
					== EGGYGOMeleeTraceWindowQueryResult::Inactive;
				bResourcesWereCleanBeforeBroadcast =
					Fixture.WereAllResourcesCleanedBeforeBroadcast(EndingMontageTask, EndingInputTask);
				bReactivatedFromEndBroadcast = Fixture.ASC->TryActivateAbility(Fixture.AbilityHandle);
			}
		});
		Fixture.Ability->FinishForTest();
		Fixture.ASC->OnAbilityEnded.Remove(EndHandle);
		TestTrue(TEXT("Super EndAbility 广播前准确原 Owned Trace 句柄已为 Inactive"),
			bOriginalTraceWasClosedBeforeBroadcast);
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
	if (!TestTrue(TEXT("命中叶在启动前声明两个独立作者窗口"), Fixture.AddHitWindowForTest() && Fixture.AddHitWindowForTest())) { return false; }
	const bool bHadMeshPrerequisiteBeforeActivation = Fixture.HasMeshPrerequisite();
	// Explicit transient long-mix case: its native mix may outlast the completion grace.
	// Production trigger/frame policy remains configured in its own Montage assets.
	Fixture.Montage->BlendOutTriggerTime = 0.0f;
	Fixture.Montage->BlendOut.SetBlendTime(3.0f);
	Fixture.SetCompletionGraceForTest(0.1f);
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
		bPassed &= Check(TEXT("可配原生混出超过宽限时，watchdog仍覆盖完整混出与宽限"),
			WatchdogRemaining > Fixture.Montage->GetDefaultBlendOutTime()
			&& Fixture.GetExpectedCompletionTailBudget() > Fixture.Montage->GetDefaultBlendOutTime());
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
	FGGYGOMontageSectionSnapshot BeforeCorrectionSnapshot;
	if (!TestTrue(TEXT("保留纠正前同资产的精确旧实例"), Fixture.GetMontageTask()
		&& Fixture.GetMontageTask()->TryGetOriginalSectionSnapshot(BeforeCorrectionSnapshot))) { return false; }
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
	// The configured author-point target can return to a smaller step; confirmations are ordered
	// by their original input requests, never by the numerical step index.
	Fixture.Ability->ReceiveAbilityCorrection(MakeComboCorrection(9, 21, 1, 0.2f));
	TestEqual(TEXT("较新确认进入前向段"), Fixture.Ability->GetCurrentComboStep(), 1);
	Fixture.Ability->ReceiveAbilityCorrection(MakeComboCorrection(10, 22, 0, 0.1f));
	TestEqual(TEXT("较新原请求确认可回接首段"), Fixture.Ability->GetCurrentComboStep(), 0);
	Fixture.Ability->ReceiveAbilityCorrection(MakeComboCorrection(11, 21, 1, 0.2f));
	TestEqual(TEXT("旧请求确认不回写已回接的首段"), Fixture.Ability->GetCurrentComboStep(), 0);
	FAnimNotifyEvent* OriginalSignalEvent = nullptr;
	for (FAnimNotifyEvent& Event : Fixture.Montage->Notifies)
	{
		if (Cast<UAnimNotify_PlayMontageNotify>(Event.Notify.Get())) { OriginalSignalEvent = &Event; break; }
	}
	if (!TestNotNull(TEXT("实际同资产有唯一原生作者点"), OriginalSignalEvent)) { return false; }
	FBranchingPointNotifyPayload RetiredSignal(Fixture.Mesh, Fixture.Montage, OriginalSignalEvent,
		BeforeCorrectionSnapshot.MontageInstanceId);
	Fixture.AnimInstance->OnPlayMontageNotifyBegin.Broadcast(FName(TEXT("Event.Montage.CancelPoint")), RetiredSignal);
	TestFalse(TEXT("同资产旧播放的原生通知不能打开新资源门"), Fixture.Ability->IsInterruptionOpen());
	FGameplayEventData WeakSignal;
	WeakSignal.EventTag = GGYGOGameplayTags::Event_Montage_CancelPoint;
	WeakSignal.OptionalObject = Fixture.Montage;
	WeakSignal.OptionalObject2 = Fixture.Mesh;
	Fixture.ASC->HandleGameplayEvent(WeakSignal.EventTag, &WeakSignal);
	TestFalse(TEXT("ASC普通GameplayEvent不能伪造原作者单点开门"), Fixture.Ability->IsInterruptionOpen());
	if (!TestTrue(TEXT("回放真实旧实例的迟到作者 Begin"),
		Fixture.SendHitWindowForTest(true, BeforeCorrectionSnapshot.MontageInstanceId))
		|| !TestFalse(TEXT("同资产旧实例窗口不能给纠正后段开 Trace"), Fixture.GetTraceWindow().HasWindow())) { return false; }
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
	if (!TestFalse(TEXT("裸 ASC 窗口负载不能伪造已认证原作者窗口"), Fixture.GetTraceWindow().HasWindow())
		|| !TestTrue(TEXT("生产 Notify 桥接受原作者窗和原实例上下文"), Fixture.SendHitWindowForTest(true))) { return false; }
	const FGGYGOMeleeTraceWindowHandle WindowB = Fixture.GetTraceWindow();
	if (!TestTrue(TEXT("作者 Notify 事件经真实 Montage Task/生产 Combo 建立 B 的 Owned Trace 窗口"),
		WindowB.HasWindow() && Fixture.Trace->QueryOwnedTraceWindow(WindowB) == EGGYGOMeleeTraceWindowQueryResult::Active
		&& Fixture.Trace->IsTracing())
		|| !CheckActiveResources(ActivationB.OriginalActivation, TEXT("开窗后的 B"), 0.0f)) { return false; }
	HitWindowEvent.EventTag = GGYGOGameplayTags::Event_Montage_HitWindowEnd;
	Fixture.ASC->HandleGameplayEvent(HitWindowEvent.EventTag, &HitWindowEvent);
	if (!TestTrue(TEXT("裸 ASC End 不能关闭 B 的真实窗口"), Fixture.GetTraceWindow() == WindowB
		&& Fixture.Trace->QueryOwnedTraceWindow(WindowB) == EGGYGOMeleeTraceWindowQueryResult::Active)
		|| !TestTrue(TEXT("回放旧实例迟到的作者 End"), Fixture.SendHitWindowForTest(false, BeforeCorrectionSnapshot.MontageInstanceId))
		|| !TestTrue(TEXT("旧窗口 End 不关闭后继的 Owned Trace"), Fixture.GetTraceWindow() == WindowB
			&& Fixture.Trace->QueryOwnedTraceWindow(WindowB) == EGGYGOMeleeTraceWindowQueryResult::Active)) { return false; }
	if (!TestTrue(TEXT("同一播放的第二个作者 Begin 接续原 Trace"), Fixture.SendHitWindowForTest(true, INDEX_NONE, 1))) { return false; }
	const FGGYGOMeleeTraceWindowHandle SecondWindowB = Fixture.GetTraceWindow();
	if (!TestTrue(TEXT("同值但独立的作者条目获得新的 Owned Trace"), SecondWindowB.HasWindow() && SecondWindowB != WindowB
		&& Fixture.Trace->QueryOwnedTraceWindow(WindowB) == EGGYGOMeleeTraceWindowQueryResult::Inactive
		&& Fixture.Trace->QueryOwnedTraceWindow(SecondWindowB) == EGGYGOMeleeTraceWindowQueryResult::Active)
		|| !TestTrue(TEXT("发送同播放第一条目的迟到 End"), Fixture.SendHitWindowForTest(false))
		|| !TestTrue(TEXT("第一条目 End 不关闭第二条目的 Trace"), Fixture.GetTraceWindow() == SecondWindowB
			&& Fixture.Trace->QueryOwnedTraceWindow(SecondWindowB) == EGGYGOMeleeTraceWindowQueryResult::Active)) { return false; }
	if (!EndNormalActivation(ActivationB.OriginalActivation, TEXT("B"))) { return false; }
	TestTrue(TEXT("B 正常结束后确切原 Owned Trace 窗口为 Inactive"),
		Fixture.Trace->QueryOwnedTraceWindow(WindowB) == EGGYGOMeleeTraceWindowQueryResult::Inactive);
	TestTrue(TEXT("B 正常结束回收第二条目仍持有的 Trace"),
		Fixture.Trace->QueryOwnedTraceWindow(SecondWindowB) == EGGYGOMeleeTraceWindowQueryResult::Inactive);
	return true;
}

namespace
{
	UE_DEFINE_GAMEPLAY_TAG_STATIC(TAG_PlayerComboRuntimeHitCue, "GameplayCue.GGYGO.Tests.PlayerCombo.RuntimeHit");

	enum class EComboRuntimeHitCase : uint8 { NoGE, ValidGE, InvalidRequiredGE, InvalidRequiredSpec,
		Multiplier, ConflictingDamageKey, MissingMultiplier };

	struct FComboRuntimeHitLogMessage
	{
		FString Text;
		ELogVerbosity::Type Verbosity;
		FName Category;
	};

	/** 只观察原日志，不改变类别、级别或其它输出设备。 */
	class FComboRuntimeHitLogObservation final : public FOutputDevice
	{
	public:
		FComboRuntimeHitLogObservation() { GLog->AddOutputDevice(this); }
		virtual ~FComboRuntimeHitLogObservation() override { GLog->RemoveOutputDevice(this); }
		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
		virtual void Serialize(const TCHAR* Text, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			FScopeLock Lock(&Mutex);
			if (bObserving && Category == FName(TEXT("LogGGYGOAbilitySystem"))
				&& (Verbosity == ELogVerbosity::Error || Verbosity == ELogVerbosity::Warning))
			{
				Messages.Add({FString(Text), Verbosity, Category});
			}
		}
		void Begin() { FScopeLock Lock(&Mutex); bObserving = true; }
		TArray<FComboRuntimeHitLogMessage> End()
		{
			FScopeLock Lock(&Mutex);
			bObserving = false;
			return Messages;
		}
	private:
		FCriticalSection Mutex;
		bool bObserving = false;
		TArray<FComboRuntimeHitLogMessage> Messages;
	};

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
		bool bExpectMultiplier = false;
		bool bObservedMultiplier = true;

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
					if (bExpectMultiplier)
					{
						const float* Value = Spec.SetByCallerTagMagnitudes.Find(GGYGOGameplayTags::SetByCaller_DamageMultiplier);
						bObservedMultiplier &= Value && *Value == 2.0f
							&& !Spec.SetByCallerTagMagnitudes.Contains(GGYGOGameplayTags::SetByCaller_Damage);
					}
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
		if (!Test.TestTrue(TEXT("正式 Notify 发送端消费真实原窗口上下文"), Fixture.SendHitWindowForTest(bOpen))) { return false; }
		const FGGYGOMeleeTraceWindowHandle Window = Fixture.GetTraceWindow();
		return Test.TestTrue(TEXT("正式窗口事件完成 Owned 资源接线"), bOpen
			? Window.HasWindow() && Fixture.Trace->QueryOwnedTraceWindow(Window) == EGGYGOMeleeTraceWindowQueryResult::Active
				&& Fixture.Trace->IsTracing() && Fixture.Trace->IsComponentTickEnabled()
			: !Window.HasWindow() && !Fixture.Trace->IsTracing());
	}

	bool RunComboRuntimeHitCase(FAutomationTestBase& Test, EComboRuntimeHitCase Case)
	{
		const bool bMultiplier = Case == EComboRuntimeHitCase::Multiplier
			|| Case == EComboRuntimeHitCase::ConflictingDamageKey || Case == EComboRuntimeHitCase::MissingMultiplier;
		const bool bMultiplierFault = Case == EComboRuntimeHitCase::ConflictingDamageKey || Case == EComboRuntimeHitCase::MissingMultiplier;
		const bool bUncancelableFault = Case == EComboRuntimeHitCase::InvalidRequiredSpec || bMultiplierFault;
		FGGYGOComboLifecycleTestWorld TestWorld(GEngine, true, bMultiplier);
		FGGYGOPlayerComboLifecycleFixture Fixture;
		if (!InitializeFixture(Test, TestWorld, Fixture)) { return false; }
		if (!Test.TestTrue(TEXT("真实 hit 启动前显式声明作者窗口"), Fixture.AddHitWindowForTest())) { return false; }
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
		TStrongObjectPtr<UGGYGOCombatSet> SourceCombat;
		TStrongObjectPtr<UGGYGOHealthSet> TargetHealth;
		if (bMultiplier)
		{
			if (!Test.TestNotNull(TEXT("倍率结算原生GameInstance"), TestWorld.GameInstance.Get())
				|| !Test.TestTrue(TEXT("倍率结算World持有原GameInstance"), Fixture.World->GetGameInstance() == TestWorld.GameInstance.Get())
				|| !Test.TestNotNull(TEXT("倍率结算原生MessageRouter"),
					UGameInstance::GetSubsystem<UGameplayMessageSubsystem>(TestWorld.GameInstance.Get()))) { return false; }
			SourceCombat.Reset(NewObject<UGGYGOCombatSet>(Fixture.Character, NAME_None, RF_Transient));
			if (!Test.TestNotNull(TEXT("倍率专项Source CombatSet"), SourceCombat.Get())) { return false; }
			SourceCombat->InitBaseDamage(10.0f);
			Fixture.ASC->AddAttributeSetSubobject(SourceCombat.Get());
			TargetHealth.Reset(NewObject<UGGYGOHealthSet>(Probe.Target, NAME_None, RF_Transient));
			if (!Test.TestNotNull(TEXT("倍率专项Target HealthSet"), TargetHealth.Get())) { return false; }
			TargetHealth->InitMaxHealth(100.0f); TargetHealth->InitHealth(100.0f);
			TargetHealth->InitMaxPoise(50.0f); TargetHealth->InitPoise(50.0f);
			Probe.TargetASC->AddAttributeSetSubobject(TargetHealth.Get());
			Fixture.Ability->ConfigureDamageMultiplierForTest(2.0f);
			Probe.bExpectMultiplier = true;
		}
		ON_SCOPE_EXIT
		{
			if (SourceCombat.IsValid()) { Fixture.ASC->RemoveSpawnedAttribute(SourceCombat.Get()); }
			if (TargetHealth.IsValid()) { Probe.TargetASC->RemoveSpawnedAttribute(TargetHealth.Get()); }
		};
		const TSubclassOf<UGameplayEffect> DamageClass = bNoGE ? TSubclassOf<UGameplayEffect>()
			: bMultiplier ? TSubclassOf<UGameplayEffect>(UGGYGOPlayerComboLifecycleMultiplierEffect::StaticClass())
			: TSubclassOf<UGameplayEffect>(UGameplayEffect::StaticClass());
		Fixture.ConfigureRuntimeHitDamage(DamageClass, CueTag);
		if (bNoGE)
		{
			const UGameplayAbility* const AdmissionAbility = Fixture.Ability;
			FGGYGOComboStep& Step = Fixture.GetFirstStepForTest();
			const float OriginalMultiplier = Step.DamageMultiplier;
			const float OriginalDamage = Step.Damage;
			const float OriginalPoise = Step.PoiseDamage;
			Step.DamageMultiplier = -2.0f;
			Test.TestFalse(TEXT("显式无GE的纯准入仍拒绝非法倍率存值"),
				AdmissionAbility->CanActivateAbility(Fixture.AbilityHandle, Fixture.ASC->AbilityActorInfo.Get(), nullptr, nullptr, nullptr));
			Step.DamageMultiplier = -1.0f;
			Step.Damage = std::numeric_limits<float>::quiet_NaN();
			Test.TestFalse(TEXT("显式无GE的纯准入仍拒绝非法旧绝对值"),
				AdmissionAbility->CanActivateAbility(Fixture.AbilityHandle, Fixture.ASC->AbilityActorInfo.Get(), nullptr, nullptr, nullptr));
			Step.Damage = OriginalDamage;
			Step.DamageMultiplier = OriginalMultiplier;
			Step.PoiseDamage = -1.0f;
			Test.TestFalse(TEXT("显式无GE的纯准入仍拒绝非法削韧值"),
				AdmissionAbility->CanActivateAbility(Fixture.AbilityHandle, Fixture.ASC->AbilityActorInfo.Get(), nullptr, nullptr, nullptr));
			Step.PoiseDamage = OriginalPoise;
			Test.TestTrue(TEXT("恢复原数值后显式无GE的纯准入通过"),
				AdmissionAbility->CanActivateAbility(Fixture.AbilityHandle, Fixture.ASC->AbilityActorInfo.Get(), nullptr, nullptr, nullptr));
		}
		else if (Case == EComboRuntimeHitCase::Multiplier)
		{
			const UGameplayAbility* const AdmissionAbility = Fixture.Ability;
			Fixture.Ability->ConfigureDamageMultiplierForTest(-1.0f);
			Test.TestFalse(TEXT("合法未配置哨兵不能通过倍率GE的纯准入"),
				AdmissionAbility->CanActivateAbility(Fixture.AbilityHandle, Fixture.ASC->AbilityActorInfo.Get(), nullptr, nullptr, nullptr));
			Fixture.Ability->ConfigureDamageMultiplierForTest(2.0f);
			Test.TestTrue(TEXT("显式段倍率与倍率GE匹配后纯准入通过"),
				AdmissionAbility->CanActivateAbility(Fixture.AbilityHandle, Fixture.ASC->AbilityActorInfo.Get(), nullptr, nullptr, nullptr));
		}
		if (bUncancelableFault)
		{
			// 不可取消故障场景先明确占用策略，再进入真实受控准入。
			Fixture.Ability->ConfigureExclusiveSelfPolicyForTest();
			if (!Test.TestTrue(TEXT("不可取消故障场景在受控启动前明确配置 Exclusive"),
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
		if (bMultiplier)
		{
			bPassed &= Test.TestTrue(TEXT("倍率实际Spec只含新键且真实HealthSet消费10×2伤害与独立削韧"),
				Probe.bObservedMultiplier && SourceCombat->GetBaseDamage() == 10.0f
				&& TargetHealth->GetHealth() == 80.0f && TargetHealth->GetPoise() == 40.0f);
		}
		if (!bPassed) { return false; }
		TArray<FString> ExpectedFaultDiagnostics;
		const auto RuntimeDiagnostic = [&](const FString& Reason)
		{
			return FString::Printf(TEXT("PlayerCombo runtime hit Ability [%s] Source [%s] Target [%s] Step %d Montage [%s] failed: %s."),
				*Fixture.Ability->GetPathName(), *Fixture.Character->GetPathName(), *Probe.Target->GetPathName(),
				Fixture.Ability->GetCurrentComboStep(), *Fixture.Montage->GetPathName(), *Reason);
		};
		const bool bFault = Case == EComboRuntimeHitCase::InvalidRequiredGE || Case == EComboRuntimeHitCase::InvalidRequiredSpec || bMultiplierFault;
		if (bFault)
		{
			const FGGYGOMeleeTraceWindowHandle ControlWindow = Fixture.GetTraceWindow();
			if (!SendRuntimeWindowEvent(Test, Fixture, false)
				|| !Test.TestTrue(TEXT("故障前正向对照的原窗口已关闭"),
					Fixture.Trace->QueryOwnedTraceWindow(ControlWindow) == EGGYGOMeleeTraceWindowQueryResult::Inactive)
				|| !SendRuntimeWindowEvent(Test, Fixture, true)
				|| !Test.TestTrue(TEXT("故障命中取得同一 Original 的新窗口"), Fixture.GetTraceWindow() != ControlWindow)
				|| !CheckHeldResources()) { return false; }
			if (bMultiplierFault)
			{
				Fixture.Ability->SetCanBeCanceledForTest(false);
				if (!Test.TestFalse(TEXT("倍率故障原能力实际不可取消"), Fixture.Ability->CanBeCanceled())) { return false; }
				if (Case == EComboRuntimeHitCase::MissingMultiplier)
				{
					Fixture.Ability->ConfigureDamageMultiplierForTest(-1.0f);
					ExpectedFaultDiagnostics.Add(RuntimeDiagnostic(TEXT("ComboDamage: required-multiplier-missing-or-invalid")));
				}
				else
				{
					FGameplayAbilitySpec* AbilitySpec = Fixture.ASC->FindAbilitySpecFromHandle(Fixture.AbilityHandle);
					if (!Test.TestNotNull(TEXT("冲突来自真实原AbilitySpec的SetByCaller"), AbilitySpec)) { return false; }
					AbilitySpec->SetByCallerTagMagnitudes.Add(GGYGOGameplayTags::SetByCaller_Damage, 7.0f);
					ExpectedFaultDiagnostics.Add(RuntimeDiagnostic(TEXT("ComboDamage: conflicting-damage-keys-or-multiplier")));
				}
			}
			else if (Case == EComboRuntimeHitCase::InvalidRequiredGE)
			{
				Fixture.ConfigureRuntimeHitDamage(UGGYGOPlayerComboLifecycleInvalidDamageEffect::StaticClass(), CueTag);
				ExpectedFaultDiagnostics.Add(RuntimeDiagnostic(FString::Printf(
					TEXT("模式=Override，配置=DamageEffect [%s]：已选GE类为抽象、已弃用或已被新版本替换。"),
					*UGGYGOPlayerComboLifecycleInvalidDamageEffect::StaticClass()->GetPathName())));
			}
			else
			{
				Fixture.Ability->SetCanBeCanceledForTest(false);
				if (!Test.TestFalse(TEXT("Builder 故障原能力明确不可取消"), Fixture.Ability->CanBeCanceled())) { return false; }
				Fixture.Ability->InvalidateNextRequiredSpecForTest();
				ExpectedFaultDiagnostics.Add(FString::Printf(
					TEXT("AbilitySystem BuildHitEffectPayload Ability [%s] Source [%s] Target [%s] GE [%s] failed: RequiredSpecInvalidAfterApplyAbilityTags."),
					*Fixture.Ability->GetPathName(), *Fixture.ASC->GetPathName(), *Probe.TargetASC->GetPathName(), *DamageClass->GetPathName()));
				ExpectedFaultDiagnostics.Add(RuntimeDiagnostic(FString::Printf(
					TEXT("BuildHitEffectPayload returned false for GE [%s]; see the original Builder diagnostic."), *DamageClass->GetPathName())));
			}
		}
		FComboRuntimeEndObservation EndObservation(Fixture, CleanupOriginal, bHadPrerequisite);
		if (!EndObservation.CheckBeforeTrigger(Test)) { return false; }
		if (bFault)
		{
			const int32 AppliedBeforeFault = Probe.AppliedCount;
			const int32 CuesBeforeFault = Probe.CueCount;
			const int32 SpecsBeforeFault = Fixture.Ability->GetSpecExtensionCountForTest();
			for (const FString& Message : ExpectedFaultDiagnostics)
			{
				Test.AddExpectedMessagePlain(TEXT("LogGGYGOAbilitySystem: ") + Message,
					ELogVerbosity::Error, EAutomationExpectedMessageFlags::Exact, 1);
			}
			if (!Test.TestNotNull(TEXT("故障原始日志输出"), GLog)) { return false; }
			FComboRuntimeHitLogObservation Diagnostics;
			Diagnostics.Begin();
			const bool bSwept = Probe.SweepOneHit(Test, Fixture);
			const TArray<FComboRuntimeHitLogMessage> Messages = Diagnostics.End();
			if (!bSwept) { return false; }
			bPassed &= Test.TestEqual(TEXT("故障原日志准确条数"), Messages.Num(), ExpectedFaultDiagnostics.Num());
			for (const FString& ExpectedMessage : ExpectedFaultDiagnostics)
			{
				const TArray<FComboRuntimeHitLogMessage> Matching = Messages.FilterByPredicate(
					[&](const FComboRuntimeHitLogMessage& Message) { return Message.Text == ExpectedMessage; });
				bPassed &= Test.TestEqual(TEXT("原对象完整诊断恰好一次"), Matching.Num(), 1);
				if (Matching.Num() == 1)
				{
					bPassed &= Test.TestTrue(TEXT("原诊断保留类别与Error级别"),
						Matching[0].Category == FName(TEXT("LogGGYGOAbilitySystem")) && Matching[0].Verbosity == ELogVerbosity::Error);
				}
			}
			bPassed &= Test.TestEqual(TEXT("故障 hit 没有继续 Apply"), Probe.AppliedCount, AppliedBeforeFault);
			bPassed &= Test.TestEqual(TEXT("故障 hit 没有继续 Cue"), Probe.CueCount, CuesBeforeFault);
			const bool bBuilderFault = Case == EComboRuntimeHitCase::InvalidRequiredSpec;
			bPassed &= Test.TestEqual(TEXT("故障 hit 实际进入所选 GE/Builder 分支"),
				Fixture.Ability->GetSpecExtensionCountForTest() - SpecsBeforeFault, (bBuilderFault || bMultiplierFault) ? 1 : 0);
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
		Test.AddInfo(FString::Printf(TEXT("RuntimeHit Case=%d behavior assertions=%s; Apply=%d Cue=%d SpecExtension=%d InvalidSpec=%d GANativeEnd=%d NativeEnd=%d Completed=%d. 原生产日志保留，预期故障逐条核完整文本/类别/Error级别/次数。"),
			static_cast<int32>(Case), bPassed ? TEXT("PASS") : TEXT("FAIL"), Probe.AppliedCount, Probe.CueCount,
			Fixture.Ability->GetSpecExtensionCountForTest(), Fixture.Ability->GetInvalidatedSpecCountForTest(),
			EndObservation.AbilityEndCount, EndObservation.EndCount, EndObservation.CompletedCount));
		return bPassed;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOPlayerComboRuntimeHitMultiplierTest,
	"GGYGO.AbilitySystem.PlayerCombo.RuntimeHit.DamageMultiplier",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOPlayerComboRuntimeHitMultiplierTest::RunTest(const FString& Parameters)
{
	return RunComboRuntimeHitCase(*this, EComboRuntimeHitCase::Multiplier)
		&& RunComboRuntimeHitCase(*this, EComboRuntimeHitCase::ConflictingDamageKey)
		&& RunComboRuntimeHitCase(*this, EComboRuntimeHitCase::MissingMultiplier);
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

#if WITH_EDITOR
namespace
{
	enum class EComboPoseFailureProbe : uint8 { None, StartupMissingDeclaration, RuntimeInvalidated };
	enum class EComboEndAttackProbe : uint8 { None, Restart, Natural };
	enum class EComboLocomotionObservation : uint8 { Immediate, RunHoldAndRelease };
	enum class EComboFXProbe : uint8 { None, NaturalTail, WindowCancel, EarlyCancelRestart };
	/** A finite production PIE smoke. Automation observes; UE alone runs input, animation and movement frames. */
	class FComboEndProductionPIECommand : public IAutomationLatentCommand
	{
	public:
		FComboEndProductionPIECommand(FAutomationTestBase& InTest, bool bInHeldDuringMain,
			EComboPoseFailureProbe InPoseProbe = EComboPoseFailureProbe::None, int32 InLastMovementStep = 0,
			EComboEndAttackProbe InEndAttackProbe = EComboEndAttackProbe::None,
			EComboLocomotionObservation InLocomotionObservation = EComboLocomotionObservation::Immediate,
			EComboFXProbe InFXProbe = EComboFXProbe::None)
			: Test(InTest), bHeldDuringMain(bInHeldDuringMain), PoseProbe(InPoseProbe), LastMovementStep(InLastMovementStep),
			EndAttackProbe(InEndAttackProbe), LocomotionObservation(InLocomotionObservation),
			FXProbe(InFXProbe),
			TargetStep(InEndAttackProbe == EComboEndAttackProbe::None ? 0 : 2), OuterWorld(GWorld),
			OuterViewport(GEngine ? GEngine->GameViewport.Get() : nullptr) {}
		virtual ~FComboEndProductionPIECommand() override
		{
			ReleaseFXInputObservation();
			ReleaseFXFrameObservation();
			if (Stage != EStage::Done)
			{
				Fail(TEXT("automation aborted before owned PIE cleanup"));
				Stage = EStage::Ending;
				ReleaseGameplayObservations();
				CloseOwnedPIE(true);
				ReleaseMainPointProbeAfterPIETeardown();
				RestoreFocusAfterPIETeardown();
			}
			FEditorDelegates::PreBeginPIE.Remove(PreHandle);
			FEditorDelegates::PostPIEStarted.Remove(PostHandle);
		}

		virtual bool Update() override
		{
			if (Stage == EStage::Done) { return true; }
			if (Stage == EStage::Start) { StartPIE(); return false; }
			if (Stage == EStage::Ending) { return UpdateEnding(); }
			if (bFailed) { BeginEnding(); return false; }
			if (FPlatformTime::Seconds() >= Deadline)
			{
				if (Stage == EStage::Locomotion || Stage == EStage::RunHold || Stage == EStage::RunRelease)
				{
					LogLocomotionSnapshot(TEXT("LocomotionTimeout"));
				}
				Fail(TEXT("bounded stage timeout; original input/section/cancellation/recovery condition not observed"));
				BeginEnding();
				return false;
			}
			if (Stage == EStage::Player)
			{
				if (bPIEStarted && !IsOwnedWorldCurrent()) { Fail(TEXT("original PIE context changed during startup")); }
				else if (bPIEStarted) { TryStartAttack(); }
				return false;
			}
			if (!IsProductionChainCurrent()) { Fail(TEXT("original world/player/pawn/input/ASC/mesh/scope or viewport focus changed")); return false; }
			// FX 阶段只由有限原生 Tick 观察；latent 保留启动、截止时间和原生 PIE 清理。
			if (FXProbe != EComboFXProbe::None) { return false; }
			if (Stage == EStage::PoseWaitFailure) { VerifyPoseFailure(); return false; }
			if (Stage == EStage::PoseSuccessorPress) { StartPoseSuccessor(); return false; }
			if (Stage == EStage::PoseSuccessorMain) { ObservePoseSuccessorMain(); return false; }
			if (Stage == EStage::PoseSuccessorEnd) { ObservePoseSuccessorEnd(); return false; }
			if (Stage == EStage::Main)
			{
				TryCaptureMain();
				return false;
			}
			if (Stage == EStage::AdvanceEnd) { AdvanceAtEnd(); return false; }
			if (Stage == EStage::MainAttackPress) { PressAttackDuringTerminalMain(); return false; }
			if (Stage == EStage::MainAttackRejected) { ObserveRejectedMainAttack(); return false; }
			if (Stage == EStage::EndAttack) { ObserveEndAttack(); return false; }
			if (Stage == EStage::Natural) { ObserveFullNaturalEnd(); return false; }
			if (Stage == EStage::NaturalIdle) { ObserveNaturalIdle(); return false; }
			if (Stage == EStage::ResetRelease) { RestartAfterMovement(); return false; }
			if (Stage == EStage::RunHold) { ObserveRunHold(); return false; }
			if (Stage == EStage::RunRelease) { ObserveRunRelease(); return false; }
			if (Stage == EStage::End) { ObserveNaturalEnd(); return false; }
			if (Stage == EStage::Cancel)
			{
				if (EndCount != 0 || CompletedCount != 0) { VerifyCancellation(); }
				return false;
			}
			if (Stage == EStage::Locomotion) { ObserveLocomotion(); }
			return false;
		}

	private:
		/** 原 Controller 之后、Pawn／CMC／Mesh 之前的有限观察，不执行输入或姿态 Tick。 */
		struct FFXInputObservationTick final : FTickFunction
		{
			FComboEndProductionPIECommand* Command = nullptr;
			FFXInputObservationTick()
			{
				bCanEverTick = true;
				bStartWithTickEnabled = true;
				bRunOnAnyThread = false;
				TickGroup = TG_PrePhysics;
				EndTickGroup = TG_PrePhysics;
			}
			virtual void ExecuteTick(float, ELevelTick, ENamedThreads::Type, const FGraphEventRef&) override
			{
				FComboEndProductionPIECommand* const OriginalCommand = Command;
				if (OriginalCommand) { OriginalCommand->ObserveFXAfterNativeInput(); }
			}
			virtual FString DiagnosticMessage() override { return TEXT("PlayerCombo FX original input observation"); }
		};
		/** 在每个真实 UE 帧的原 Mesh 更新后观察短 FX 窗口，不依赖 Automation latent 的轮询频率。 */
		struct FFXFrameObservationTick final : FTickFunction
		{
			FComboEndProductionPIECommand* Command = nullptr;
			FFXFrameObservationTick()
			{
				bCanEverTick = true;
				bStartWithTickEnabled = true;
				bRunOnAnyThread = false;
				TickGroup = TG_PostUpdateWork;
				EndTickGroup = TG_PostUpdateWork;
			}
			virtual void ExecuteTick(float, ELevelTick, ENamedThreads::Type, const FGraphEventRef&) override
			{
				if (Command) { Command->ObserveFXNativeFrame(); }
			}
			virtual FString DiagnosticMessage() override { return TEXT("PlayerCombo FX native frame observation"); }
		};

		enum class EStage : uint8 { Start, Player, Main, AdvanceEnd, MainAttackPress, MainAttackRejected, EndAttack, Natural, NaturalIdle,
			End, Cancel, Locomotion, ResetRelease, Ending, Done, PoseWaitFailure, PoseSuccessorMain, PoseSuccessorEnd, PoseSuccessorPress,
			RunHold, RunRelease, FXCancelWait, FXReady };
		void Fail(const FString& Reason)
		{
			if (bFailed) { return; }
			bFailed = true;
			if (PoseProbe != EComboPoseFailureProbe::None)
			{
				Test.AddError(FString::Printf(TEXT("[Combat.PlayerCombo.PoseProductionSmoke] Case=%s Stage=%u World=%s GA=%s Instance=%d: %s"),
					PoseCaseName(), static_cast<uint32>(Stage), *GetPathNameSafe(World.Get()), *GetPathNameSafe(Ability.Get()), InstanceId, *Reason));
				return;
			}
			Test.AddError(FString::Printf(TEXT("[Combat.PlayerCombo.EndProductionSmoke] Case=%s AttackProbe=%u TargetStep=%d ExpectedStep=%d Stage=%u World=%s Pawn=%s GA=%s Instance=%d: %s"),
				bHeldDuringMain ? TEXT("HeldDuringMain") : TEXT("NewPressDuringEnd"), static_cast<uint32>(EndAttackProbe), TargetStep, ExpectedStep, static_cast<uint32>(Stage),
				*GetPathNameSafe(World.Get()), *GetPathNameSafe(Character.Get()), *GetPathNameSafe(Ability.Get()), InstanceId, *Reason));
		}
		int32 CountPIEWorlds() const
		{
			int32 Count = 0;
			if (Editor.IsValid())
			{
				for (const FWorldContext& Context : Editor->GetWorldContexts())
				{
					if (Context.WorldType == EWorldType::PIE) { ++Count; }
				}
			}
			return Count;
		}
		bool OwnsPIESession() const
		{
			if (!Editor.IsValid() || !RequestSettings.IsValid()) { return false; }
			const TOptional<FPlayInEditorSessionInfo> Info = Editor->GetPlayInEditorSessionInfo();
			return Info.IsSet() && Info->OriginalRequestParams.EditorPlaySettings == RequestSettings.Get()
				&& Info->OriginalRequestParams.SessionDestination == EPlaySessionDestinationType::InProcess
				&& !Info->OriginalRequestParams.GameModeOverride;
		}
		bool IsOwnedWorldCurrent() const
		{
			if (!OwnsPIESession() || CountPIEWorlds() != 1 || !World.IsValid() || !GameInstance.IsValid()
				|| Editor->PlayWorld != World.Get() || Editor->ShouldEndPlayMap()) { return false; }
			const FWorldContext* Context = Editor->GetWorldContextFromWorld(World.Get());
			return Context && Context->ContextHandle == ContextHandle && Context->OwningGameInstance == GameInstance.Get();
		}
		void StartPIE()
		{
			Stage = EStage::Player;
			if (LocomotionObservation == EComboLocomotionObservation::RunHoldAndRelease
				&& (!bHeldDuringMain || PoseProbe != EComboPoseFailureProbe::None
					|| EndAttackProbe != EComboEndAttackProbe::None || LastMovementStep != 0))
			{
				Fail(TEXT("Run hold/release observation requires the normal single-step Held production case")); return;
			}
			Editor = Cast<UEditorEngine>(GEngine);
			if (!Editor.IsValid() || Editor.Get() != GEditor || !FSlateApplication::IsInitialized())
			{
				Fail(TEXT("requires an interactive Editor and initialized native Slate")); return;
			}
			if (Editor->IsPlaySessionInProgress() || Editor->IsSettingUpPlayWorld() || Editor->PlayWorld
				|| Editor->ShouldEndPlayMap() || CountPIEWorlds() != 0 || Editor->GetPlaySessionRequest().IsSet())
			{
				Fail(TEXT("existing or queued PIE is not owned by this test")); return;
			}
			UWorld* EditorWorld = Editor->GetEditorWorldContext().World();
			if (!EditorWorld) { Fail(TEXT("no current production editor map")); return; }
			EditorMap = EditorWorld->GetPackage()->GetName();
			OuterWorld = GWorld;
			OuterViewport = GEngine->GameViewport.Get();
			PreviousFocus = FSlateApplication::Get().GetKeyboardFocusedWidget();
			TStrongObjectPtr<ULevelEditorPlaySettings> Settings(NewObject<ULevelEditorPlaySettings>(GetTransientPackage()));
			Settings->SetPlayNetMode(EPlayNetMode::PIE_Standalone);
			Settings->SetRunUnderOneProcess(true);
			Settings->SetPlayNumberOfClients(1);
			Settings->bLaunchSeparateServer = false;
			Settings->GameGetsMouseControl = true;
			Settings->bShouldMinimizeEditorOnNonVRPIE = false;
			FRequestPlaySessionParams Params;
			Params.EditorPlaySettings = Settings.Get();
			Params.bAllowOnlineSubsystem = false;
			// Preserve the current production map, GameMode, Experience and spawn path.
			PreHandle = FEditorDelegates::PreBeginPIE.AddLambda([this](bool bSimulating)
			{
				const TOptional<FRequestPlaySessionParams> Queued = Editor->GetPlaySessionRequest();
				bSawPreBegin = Stage == EStage::Player && !bSimulating && Queued.IsSet()
					&& Queued->EditorPlaySettings == RequestSettings.Get();
				if (!bSawPreBegin) { Fail(TEXT("PreBeginPIE did not identify our original request")); }
			});
			PostHandle = FEditorDelegates::PostPIEStarted.AddLambda([this](bool bSimulating)
			{
				if (Stage != EStage::Player || bSimulating || !bSawPreBegin || !OwnsPIESession() || CountPIEWorlds() != 1)
				{
					Fail(TEXT("PostPIEStarted did not identify our sole native production session")); return;
				}
				const FWorldContext* Context = Editor->GetWorldContextFromWorld(Editor->PlayWorld);
				if (!Context || !Context->World() || !Context->OwningGameInstance)
				{
					Fail(TEXT("original PIE world/GI unavailable")); return;
				}
				ContextHandle = Context->ContextHandle;
				World = Context->World();
				GameInstance = Context->OwningGameInstance.Get();
				bPIEStarted = true;
				if (UWorld::RemovePIEPrefix(World->GetPackage()->GetName()) != EditorMap)
				{
					Fail(TEXT("PIE changed the original production map"));
				}
			});
			Deadline = FPlatformTime::Seconds() + 30.0;
			Editor->RequestPlaySession(Params);
			const TOptional<FRequestPlaySessionParams> Queued = Editor->GetPlaySessionRequest();
			if (!Queued.IsSet() || !Queued->EditorPlaySettings)
			{
				Fail(TEXT("native PIE request did not retain its original settings")); return;
			}
			RequestSettings = Queued->EditorPlaySettings.Get();
		}
		void TryStartAttack()
		{
			if (!World->HasBegunPlay() || !World->GetAuthGameMode() || GameInstance->GetLocalPlayers().Num() != 1) { return; }
			UGGYGOLocalPlayer* Player = Cast<UGGYGOLocalPlayer>(GameInstance->GetLocalPlayers()[0]);
			AGGYGOPlayerController* PC = Player ? Cast<AGGYGOPlayerController>(Player->GetPlayerController(World.Get())) : nullptr;
			ACharacter* Pawn = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
			UGGYGOPlayerInput* Source = PC ? Cast<UGGYGOPlayerInput>(PC->PlayerInput) : nullptr;
			UGGYGOPawnExtensionComponent* Extension = UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(Pawn);
			UGGYGOAbilitySystemComponent* ProductionASC = Extension ? Extension->GetGGYGOAbilitySystemComponent() : nullptr;
			UGGYGOCharacterMovementComponent* CMC = Pawn ? Cast<UGGYGOCharacterMovementComponent>(Pawn->GetCharacterMovement()) : nullptr;
			if (!Player || !PC || !Pawn || !Source || !ProductionASC || !CMC || !UGGYGOHeroComponent::FindHeroComponent(Pawn)) { return; }
			if (!PC->IsLocalController() || PC->GetLocalPlayer() != Player || !Pawn->IsLocallyControlled()
				|| !ProductionASC->AbilityActorInfo.IsValid() || ProductionASC->AbilityActorInfo->AvatarActor.Get() != Pawn
				|| ProductionASC->AbilityActorInfo->PlayerController.Get() != PC || !ProductionASC->AbilityActorInfo->IsLocallyControlled())
			{
				Fail(TEXT("production ASC retained different original local ActorInfo")); return;
			}
			FString Error;
			FGGYGOMovementOwnerSyncScopeId CandidateScope;
			if (!CMC->GetMovementOwnerSyncScope(CandidateScope, Error))
			{
				if (bStartupCaptured) { Fail(TEXT("original startup owner scope unavailable: ") + Error); }
				return;
			}
			UEnhancedInputLocalPlayerSubsystem* Subsystem = Player->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
			const UGGYGOPawnData* PawnData = Extension->GetPawnData<UGGYGOPawnData>();
			const UGGYGOInputConfig* Config = PawnData ? PawnData->InputConfig.Get() : nullptr;
			if (!Subsystem || !Config) { return; }
			const UInputAction* MoveAction = Config->FindNativeInputActionForTag(GGYGOGameplayTags::InputTag_Move, false);
			const UInputAction* AttackAction = Config->FindAbilityInputActionForTag(GGYGOGameplayTags::InputTag_Attack_Light, false);
			if (!MoveAction || !AttackAction) { Fail(TEXT("production Move/Attack.Light action configuration missing")); return; }
			const TArray<FKey> AttackKeys = Subsystem->QueryKeysMappedToAction(AttackAction);
			if (!Subsystem->QueryKeysMappedToAction(MoveAction).Contains(EKeys::W) || AttackKeys.IsEmpty()) { return; }
			TArray<FKey> NativeAttackKeys;
			for (const FKey& Key : AttackKeys)
			{
				if (Key.IsValid() && !Key.IsGamepadKey() && !Key.IsAnalog()) { NativeAttackKeys.AddUnique(Key); }
			}
			if (NativeAttackKeys.IsEmpty() || AttackKeys.Contains(EKeys::W))
			{
				Fail(TEXT("smoke requires an actual digital keyboard/mouse Attack.Light mapping distinct from W")); return;
			}
			UGGYGOPlayerComboAbility* Combo = nullptr;
			for (const FGameplayAbilitySpec& Spec : ProductionASC->GetActivatableAbilities())
			{
				if (Spec.GetDynamicSpecSourceTags().HasTagExact(GGYGOGameplayTags::InputTag_Attack_Light))
				{
					if (UGGYGOPlayerComboAbility* Candidate = Cast<UGGYGOPlayerComboAbility>(Spec.GetPrimaryInstance()))
					{
						if (Combo) { Fail(TEXT("multiple production combo abilities share Attack.Light")); return; }
						Combo = Candidate;
						SpecHandle = Spec.Handle;
					}
				}
			}
			if (!Combo) { Fail(TEXT("production Attack.Light has no granted instantiated player combo GA")); return; }
			UGGYGOMovementInputOriginResource* Origin = Player->GetMovementInputOriginResource();
			if (bStartupCaptured && (LocalPlayer.Get() != Player || Controller.Get() != PC || Character.Get() != Pawn
				|| PlayerInput.Get() != Source || ASC.Get() != ProductionASC || Movement.Get() != CMC || Ability.Get() != Combo
				|| OriginalOrigin.Get() != Origin || CandidateScope != Scope || Mesh.Get() != Pawn->GetMesh()
				|| !Mesh.IsValid() || Mesh->GetAnimInstance() != AnimInstance.Get()
				|| Trace.Get() != Pawn->FindComponentByClass<UGGYGOMeleeTraceComponent>() || !Viewport.IsValid()
				|| World->GetGameViewport() != Viewport.Get() || Viewport->GetGameViewportWidget() != ViewportWidget
				|| Viewport->GetGameViewport() != SceneViewport))
			{
				Fail(TEXT("captured startup production identity changed while waiting for native Ready")); return;
			}
			LocalPlayer = Player; Controller = PC; Character = Pawn; PlayerInput = Source;
			ASC = ProductionASC; Movement = CMC; Ability = Combo;
			Scope = CandidateScope;
			OriginalOrigin = Origin;
			Mesh = Pawn->GetMesh();
			AnimInstance = Mesh.IsValid() ? Mesh->GetAnimInstance() : nullptr;
			Trace = Pawn->FindComponentByClass<UGGYGOMeleeTraceComponent>();
			Viewport = World->GetGameViewport();
			ViewportWidget = Viewport.IsValid() ? Viewport->GetGameViewportWidget() : nullptr;
			SceneViewport = Viewport.IsValid() ? Viewport->GetGameViewport() : nullptr;
			if (ViewportWidget.IsValid() && (ViewportWidget->GetCachedGeometry().GetLocalSize().X <= 0
				|| ViewportWidget->GetCachedGeometry().GetLocalSize().Y <= 0)) { return; } // Bounded native layout readiness.
			FGGYGOQualifiedMovementIntent BeforePress;
			const EGGYGOMovementInputOriginQualification Qualification = IsValid(Origin)
				? Origin->GetQualification(Source) : EGGYGOMovementInputOriginQualification::Unavailable;
			const EGGYGOQualifiedMovementIntentQueryResult BeforePressResult = CMC->QueryQualifiedMovementIntent(Scope, BeforePress, Error);
			// Cold is unused original permission, not observed Neutral. Untouched input
			// legitimately awaits its FIRST real press; KnownGap/flush retires Cold.
			const bool bFirstPressStart = Qualification == EGGYGOMovementInputOriginQualification::Cold
				&& (BeforePressResult == EGGYGOQualifiedMovementIntentQueryResult::AwaitingPhysicalProof
					|| BeforePressResult == EGGYGOQualifiedMovementIntentQueryResult::NotHeld);
			if (!Mesh.IsValid() || !AnimInstance.IsValid() || !Trace.IsValid() || !ViewportWidget.IsValid() || !SceneViewport
				|| !bFirstPressStart
				|| Combo->IsActive() || CMC->HasActiveActionMotion() || CMC->IsMovementBlockedByTag()
				|| ProductionASC->HasMatchingGameplayTag(GGYGOGameplayTags::State_Attacking))
			{
				LogPreflight(TEXT("RejectedAssembly"), Qualification, BeforePressResult, Error);
				Fail(TEXT("original Cold first-observation/assembly unavailable; no requalification or replacement")); return;
			}
			bStartupCaptured = true;
			if (!OwnerSyncObserver.IsSet())
			{
				if (!CMC->SubscribeMovementOwnerSync(Scope, FGGYGOMovementOwnerSyncDelegate::CreateLambda(
					[this](const FGGYGOMovementOwnerSyncObserverId& Observer, const FGGYGOMovementOwnerSyncNotice& Notice)
					{
						if (Observer.GetObserverSerial() != OwnerSyncObserver.GetObserverSerial()
							|| Observer.GetScope() != Scope || !Notice.IsSet() || Notice.GetScope() != Scope)
						{
							Fail(TEXT("native owner notice did not identify the installed original observer/scope")); return;
						}
						OwnerNotice = Notice; // Read-only issuer snapshot, invalidated by its next original notice/scope loss.
						Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.EndProductionSmoke] NativeOwnerNotice Scope=%llu Observer=%llu State=%u Notice=%llu Generation=%llu Nonce=%llu InitialEligible=%d Reason=%s."),
							static_cast<unsigned long long>(Scope.GetScopeSerial()), static_cast<unsigned long long>(Observer.GetObserverSerial()),
							static_cast<uint32>(Notice.GetState()), static_cast<unsigned long long>(Notice.GetNoticeSerial()),
							static_cast<unsigned long long>(Notice.GetServerOwnerGeneration()), static_cast<unsigned long long>(Notice.GetNativeResponseNonce()),
							Notice.IsInitialSynchronizationEligible(), *Notice.GetReason().ToString()));
						if (Notice.GetState() == EGGYGOMovementOwnerSyncState::Invalidated)
						{
							Fail(TEXT("original owner scope invalidated: ") + Notice.GetReason().ToString());
						}
					}), OwnerSyncObserver, Error))
				{
					Fail(TEXT("original owner observation installation rejected: ") + Error); return;
				}
			}
			if (bFailed) { return; }
			if (!OwnerNotice.IsSet() || OwnerNotice.GetState() != EGGYGOMovementOwnerSyncState::Ready)
			{
				if (!bLoggedOwnerWait)
				{
					LogPreflight(TEXT("WaitingForNativeOwnerReady"), Qualification, BeforePressResult, Error);
					bLoggedOwnerWait = true;
				}
				return; // Existing 30-second deadline; observing Waiting neither grants Ready nor manufactures input.
			}
			AttackKey = NativeAttackKeys[0]; // An existing mapping of this same action; selected key is reported below.
			for (const FKey& Key : { EKeys::W, AttackKey })
			{
				if (Key.IsMouseButton())
				{
					if (Key != EKeys::LeftMouseButton && Key != EKeys::RightMouseButton && Key != EKeys::MiddleMouseButton)
					{
						Fail(TEXT("unsupported configured mouse attack key: ") + Key.ToString()); return;
					}
					continue;
				}
				uint32 KeyCode = 0, CharacterCode = 0;
				if (!ResolveNativeKeyboardCodes(Key, KeyCode, CharacterCode)) { return; }
				Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.EndProductionSmoke] NativeKeyboardEncoding Key=%s KeyCode=%u CharacterCode=%u OriginalKeyRoundTrip=1."),
					*Key.ToString(), KeyCode, CharacterCode));
			}
			for (const FKey& Key : { EKeys::W, EKeys::A, EKeys::S, EKeys::D, AttackKey })
			{
				if (Source->IsPressed(Key)) { Fail(TEXT("pre-existing pressed key is not owned by this smoke")); return; }
			}
			SavedTick = Mesh->VisibilityBasedAnimTickOption;
			bSavedURO = Mesh->bEnableUpdateRateOptimizations;
			SavedRootScale = Pawn->GetAnimRootMotionTranslationScale();
			bSavedPrerequisite = HasMeshPrerequisite();
			LogPreflight(TEXT("BeforeNativeFocus"), Qualification, BeforePressResult, Error);
			NativeWindow = FSlateApplication::Get().FindWidgetWindow(ViewportWidget.ToSharedRef());
			if (!NativeWindow.IsValid() || !NativeWindow->GetNativeWindow().IsValid())
			{
				Fail(TEXT("native production viewport/window unavailable")); return;
			}
			FSlateApplication& Slate = FSlateApplication::Get();
			if (Slate.GetKeyboardFocusedWidget() != ViewportWidget)
			{
				Slate.SetKeyboardFocus(ViewportWidget, EFocusCause::SetDirectly);
			}
			// SetKeyboardFocus reports a change; false also means already focused.
			if (Slate.GetKeyboardFocusedWidget() != ViewportWidget)
			{
				Fail(TEXT("native production viewport did not acquire the actual keyboard focus")); return;
			}
			NativeWindow->GetNativeWindow()->SetWindowFocus();
			if (FXProbe != EComboFXProbe::None)
			{
				WindowDeactivatedHandle = NativeWindow->GetOnWindowDeactivatedEvent().AddLambda(
					[this]() { LogNativeInputBoundary(TEXT("OriginalWindowDeactivated")); });
			}
			if (!IsProductionChainCurrent())
			{
				FGGYGOQualifiedMovementIntent RejectedIntent;
				const EGGYGOMovementInputOriginQualification CurrentQualification = OriginalOrigin.IsValid() && PlayerInput.IsValid()
					? OriginalOrigin->GetQualification(PlayerInput.Get()) : EGGYGOMovementInputOriginQualification::Unavailable;
				const EGGYGOQualifiedMovementIntentQueryResult CurrentResult = Movement.IsValid()
					? Movement->QueryQualifiedMovementIntent(Scope, RejectedIntent, Error) : EGGYGOQualifiedMovementIntentQueryResult::Unavailable;
				LogPreflight(TEXT("NativeFocusIdentityLost"), CurrentQualification, CurrentResult, Error);
				Fail(TEXT("native focus changed the captured original production chain")); return;
			}
			FGGYGOQualifiedMovementIntent AfterFocus;
			const EGGYGOQualifiedMovementIntentQueryResult AfterFocusResult = Movement->QueryQualifiedMovementIntent(Scope, AfterFocus, Error);
			const EGGYGOMovementInputOriginQualification AfterFocusQualification = OriginalOrigin->GetQualification(PlayerInput.Get());
			LogPreflight(TEXT("AfterNativeFocus"), AfterFocusQualification, AfterFocusResult, Error);
			if (!IsProductionChainCurrent() || AfterFocusQualification != EGGYGOMovementInputOriginQualification::Cold
				|| (AfterFocusResult != EGGYGOQualifiedMovementIntentQueryResult::AwaitingPhysicalProof
					&& AfterFocusResult != EGGYGOQualifiedMovementIntentQueryResult::NotHeld))
			{
				Fail(TEXT("native focus did not preserve the original Cold first-press/Ready chain")); return;
			}
			if (PoseProbe != EComboPoseFailureProbe::None && !PreparePoseProbe()) { return; }
			if (FXProbe != EComboFXProbe::None && !PrepareFXProbe()) { return; }
			if (PoseProbe == EComboPoseFailureProbe::None && (bHeldDuringMain || EndAttackProbe != EComboEndAttackProbe::None))
			{
				MainPointProbeIndex = bHeldDuringMain ? 0 : 2;
				if (!FGGYGOPlayerComboLifecycleFixture::InstallProductionMainPointProbe(Combo, MainPointProbeIndex,
					OriginalPointProbeStep, MainPointProbe, Error))
				{
					Fail(TEXT("finite unsaved formal Main-point configuration failed: ") + Error); return;
				}
				Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.InterruptionProductionSmoke] AuthorPointInMain Step=%d Original=%s Transient=%s; original package/Sequences/windows unchanged, no save."),
					MainPointProbeIndex, *GetPathNameSafe(OriginalPointProbeStep.Montage), *GetPathNameSafe(MainPointProbe.Get())));
			}
			EndHandle = ProductionASC->OnAbilityEnded.AddLambda([this](const FAbilityEndedData& Data) { ObserveAbilityEnd(Data); });
			CompletedHandle = ProductionASC->OnAbilityTerminationCompleted().AddLambda([this](const FGGYGOAbilityTerminationCompletedNotice& Notice)
			{
				if (OriginalActivation.HasActivation() && Notice.GetOriginal().GetOriginalActivation().HasSameActivation(OriginalActivation))
				{
					++CompletedCount; Completion = Notice;
				}
				else if (PoseProbe != EComboPoseFailureProbe::None && PoseSuccessorActivation.HasActivation()
					&& Notice.GetOriginal().GetOriginalActivation().HasSameActivation(PoseSuccessorActivation))
				{
					++PoseSuccessorCompletedCount; PoseSuccessorCompletion = Notice;
				}
			});
			Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.EndProductionSmoke] NativeSlateRoute Map=%s GameMode=%s PC=%s Pawn=%s GA=%s Source=%s Origin=%s InputConfig=%s AttackKey=%s Scope=%llu. Automated native events; no HID/network/Cook claim."),
				*EditorMap, *GetPathNameSafe(World->GetAuthGameMode()), *PC->GetPathName(), *Pawn->GetPathName(), *Combo->GetPathName(),
				*Source->GetPathName(), *Origin->GetPathName(), *Config->GetPathName(), *AttackKey.ToString(), static_cast<unsigned long long>(Scope.GetScopeSerial())));
			Stage = EStage::Main;
			if (PoseProbe == EComboPoseFailureProbe::StartupMissingDeclaration) { Stage = EStage::PoseWaitFailure; }
			Deadline = FPlatformTime::Seconds() + 10.0;
			if (FXProbe != EComboFXProbe::None)
			{
				if (!InstallFXFrameObservation()) { return; }
				Stage = EStage::FXReady;
				Deadline = FPlatformTime::Seconds() + 30.0;
				return;
			}
			bAttackDown = true;
			SendNativeKey(AttackKey, true);
		}
		bool InstallFXFrameObservation()
		{
			if (!World.IsValid() || !World->PersistentLevel || !Mesh.IsValid() || !Movement.IsValid()
				|| !Mesh->PrimaryComponentTick.IsTickFunctionRegistered() || !Mesh->PrimaryComponentTick.IsTickFunctionEnabled()
				|| !Movement->PrimaryComponentTick.IsTickFunctionRegistered() || !Movement->PrimaryComponentTick.IsTickFunctionEnabled())
			{
				Fail(TEXT("原 FX 帧观察缺少已注册的生产 Mesh／CMC／Level。")); return false;
			}
			FXFrameObservationTick.Command = this;
			FXFrameObservationTick.AddPrerequisite(Mesh.Get(), Mesh->PrimaryComponentTick);
			FXFrameObservationTick.AddPrerequisite(Movement.Get(), Movement->PrimaryComponentTick);
			FXFrameObservationTick.RegisterTickFunction(World->PersistentLevel);
			if (!FXFrameObservationTick.IsTickFunctionRegistered())
			{
				ReleaseFXFrameObservation(); Fail(TEXT("原 FX 帧观察注册失败。")); return false;
			}
			return true;
		}
		void ReleaseFXFrameObservation()
		{
			if (!FXFrameObservationTick.Command && !FXFrameObservationTick.IsTickFunctionRegistered()) { return; }
			FXFrameObservationTick.Command = nullptr;
			FXFrameObservationTick.UnRegisterTickFunction();
			if (Mesh.IsValid()) { FXFrameObservationTick.RemovePrerequisite(Mesh.Get(), Mesh->PrimaryComponentTick); }
			if (Movement.IsValid()) { FXFrameObservationTick.RemovePrerequisite(Movement.Get(), Movement->PrimaryComponentTick); }
		}
		void ObserveFXNativeFrame()
		{
			if (bFailed || Stage == EStage::Ending || Stage == EStage::Done) { return; }
			if (!IsProductionChainCurrent()) { Fail(TEXT("原生产链在真实 FX 帧观察中改变。")); return; }
			if (Stage == EStage::FXReady)
			{
				if (!FXSystem.IsValid()) { Fail(TEXT("原 FX System 在原生启动等待中失效。")); return; }
				if (!FXSystem->IsReadyToRun()) { return; }
				if (FXReadyFrame == MAX_uint64)
				{
					FXReadyFrame = GFrameCounter; FXReadyWorldSeconds = World->GetTimeSeconds(); return;
				}
				if (GFrameCounter <= FXReadyFrame || World->GetTimeSeconds() <= FXReadyWorldSeconds) { return; }
				// 初始原生世界帧已真实完成；不改 DeltaTime、Montage、输入来源或播放速率。
				Stage = EStage::Main; Deadline = FPlatformTime::Seconds() + 3.0;
				if (!InstallFXInputObservation()) { return; }
				LogNativeInputBoundary(TEXT("StartupNativeFrameReady"));
				bAttackDown = true; SendNativeKey(AttackKey, true);
				return;
			}
			if (Stage == EStage::FXCancelWait) { VerifyFXCancellation(); return; }
			if (Stage == EStage::Natural || Stage == EStage::NaturalIdle)
			{
				if (!PlayerInput->IsPressed(AttackKey)) { bAttackDown = false; }
				ObserveFX();
				if (bFailed) { return; }
				if (Stage == EStage::Natural) { ObserveFullNaturalEnd(); }
				else if (Stage == EStage::NaturalIdle) { ObserveNaturalIdle(); }
			}
		}
		void LogNativeInputBoundary(const TCHAR* Boundary)
		{
			Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.FXProductionSmoke] NativeInputBoundary=%s Stage=%u Frame=%llu WorldSeconds=%.6f DeltaSeconds=%.6f WindowActive=%d ViewportFocus=%d AttackPressed=%d OwnAttackKey=%d Request=%d OriginalTask=%s CurrentTask=%s CurrentStep=%d."),
				Boundary, static_cast<uint32>(Stage), static_cast<unsigned long long>(GFrameCounter),
				World.IsValid() ? World->GetTimeSeconds() : -1.0f, World.IsValid() ? World->GetDeltaSeconds() : -1.0f,
				NativeWindow.IsValid() && NativeWindow->IsActive(),
				FSlateApplication::IsInitialized() && FSlateApplication::Get().GetKeyboardFocusedWidget() == ViewportWidget,
				PlayerInput.IsValid() && PlayerInput->IsPressed(AttackKey), bAttackDown,
				FGGYGOPlayerComboLifecycleFixture::ReadProductionLastInputRequest(Ability.Get()),
				*GetPathNameSafe(MontageTask.Get()), *GetPathNameSafe(FGGYGOPlayerComboLifecycleFixture::GetProductionMontageTask(Ability.Get())),
				Ability.IsValid() ? Ability->GetCurrentComboStep() : INDEX_NONE));
		}
		bool InstallFXInputObservation()
		{
			if (!IsProductionChainCurrent() || !World->PersistentLevel
				|| !Controller->PrimaryActorTick.IsTickFunctionRegistered() || !Controller->PrimaryActorTick.IsTickFunctionEnabled())
			{
				Fail(TEXT("FX observation requires the original live native Controller input tick")); return false;
			}
			FXInputObservationTick.Command = this;
			FXInputObservationTick.AddPrerequisite(Controller.Get(), Controller->PrimaryActorTick);
			// CMC can TickPose before the Mesh component tick; observe before both and the original Pawn.
			Character->PrimaryActorTick.AddPrerequisite(Character.Get(), FXInputObservationTick);
			Movement->PrimaryComponentTick.AddPrerequisite(Movement.Get(), FXInputObservationTick);
			Mesh->PrimaryComponentTick.AddPrerequisite(Mesh.Get(), FXInputObservationTick);
			FXInputObservationTick.RegisterTickFunction(World->PersistentLevel);
			if (!FXInputObservationTick.IsTickFunctionRegistered())
			{
				ReleaseFXInputObservation();
				Fail(TEXT("native FX input observation registration failed")); return false;
			}
			return true;
		}
		void ReleaseFXInputObservation()
		{
			if (!FXInputObservationTick.Command && !FXInputObservationTick.IsTickFunctionRegistered()) { return; }
			FXInputObservationTick.Command = nullptr;
			FXInputObservationTick.UnRegisterTickFunction();
			if (Controller.IsValid()) { FXInputObservationTick.RemovePrerequisite(Controller.Get(), Controller->PrimaryActorTick); }
			if (Character.IsValid()) { Character->PrimaryActorTick.RemovePrerequisite(Character.Get(), FXInputObservationTick); }
			if (Movement.IsValid()) { Movement->PrimaryComponentTick.RemovePrerequisite(Movement.Get(), FXInputObservationTick); }
			if (Mesh.IsValid()) { Mesh->PrimaryComponentTick.RemovePrerequisite(Mesh.Get(), FXInputObservationTick); }
		}
		void ObserveFXAfterNativeInput()
		{
			if (!bFailed && Stage == EStage::Main)
			{
				if (!IsProductionChainCurrent()) { Fail(TEXT("original production chain changed at native FX input boundary")); }
				else { TryCaptureMain(); }
			}
			if (bFailed || Stage != EStage::Main) { ReleaseFXInputObservation(); }
		}
		void LogPreflight(const TCHAR* Boundary, EGGYGOMovementInputOriginQualification Qualification,
			EGGYGOQualifiedMovementIntentQueryResult IntentResult, const FString& Diagnostic)
		{
			Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.EndProductionSmoke] Preflight Boundary=%s Mesh=%s Anim=%s Trace=%s Viewport=%s Widget=%d Scene=%d OriginalFocus=%d Origin=%s Qualification=%u IntentResult=%u Scope=%llu Binding=%llu OwnerNotice=%d OwnerState=%d OwnerSerial=%llu GAActive=%d ActionActive=%d CantMove=%d Attacking=%d Diagnostic=%s."),
				Boundary, *GetPathNameSafe(Mesh.Get()), *GetPathNameSafe(AnimInstance.Get()), *GetPathNameSafe(Trace.Get()), *GetPathNameSafe(Viewport.Get()),
				ViewportWidget.IsValid(), SceneViewport != nullptr,
				FSlateApplication::IsInitialized() && FSlateApplication::Get().GetKeyboardFocusedWidget() == ViewportWidget,
				*GetPathNameSafe(OriginalOrigin.Get()), static_cast<uint32>(Qualification),
				static_cast<uint32>(IntentResult), static_cast<unsigned long long>(Scope.GetScopeSerial()),
				Movement.IsValid() ? static_cast<unsigned long long>(Movement->GetMovementInputBindingSerial()) : 0ull,
				OwnerNotice.IsSet(), OwnerNotice.IsSet() ? static_cast<int32>(OwnerNotice.GetState()) : -1,
				static_cast<unsigned long long>(OwnerNotice.GetNoticeSerial()), Ability.IsValid() && Ability->IsActive(),
				Movement.IsValid() && Movement->HasActiveActionMotion(), Movement.IsValid() && Movement->IsMovementBlockedByTag(),
				ASC.IsValid() && ASC->HasMatchingGameplayTag(GGYGOGameplayTags::State_Attacking), *Diagnostic));
		}
		bool PrepareFXProbe()
		{
			FGGYGOComboStep Step;
			FString Error;
			if (!FGGYGOPlayerComboLifecycleFixture::ReadProductionFirstStep(Ability.Get(), Step)
				|| !Ability->ValidateComboConfiguration(Error)
				|| Step.FX.Mode != EGGYGOComboFXMode::MontageWindow || !Step.FX.System
				|| Step.FX.System->GetPathName() != TEXT("/Game/Characters/Player/Pyrios/FX/Skill/NS_Eff_Pyrois_Attack_Normal_01_01_Trail.NS_Eff_Pyrois_Attack_Normal_01_01_Trail")
				|| Step.FX.BeginEventTag != GGYGOGameplayTags::Event_Montage_FXWindowBegin
				|| Step.FX.EndEventTag != GGYGOGameplayTags::Event_Montage_FXWindowEnd
				|| Step.FX.Space != EGGYGONiagaraEffectSpace::Attached || !Step.FX.SocketName.IsNone()
				|| !Step.FX.LocalTransform.Equals(FTransform::Identity)
				|| Step.FX.NormalStopMode != EGGYGONiagaraEffectStopMode::DeactivateDetachKeepWorld
				|| Step.FX.AbortStopMode != EGGYGONiagaraEffectStopMode::DestroyImmediately)
			{
				Fail(TEXT("formal Normal01 seven-color FX configuration is not installed: ") + Error); return false;
			}
			FXSystem = Step.FX.System.Get();
			for (const FAnimNotifyEvent& Event : Step.Montage->Notifies)
			{
				const auto* Notify = Cast<UGGYGOAnimNotifyState_GameplayEventWindow>(Event.NotifyStateClass.Get());
				if (Notify && Notify->GetBeginEventTag() == Step.FX.BeginEventTag && Notify->GetEndEventTag() == Step.FX.EndEventTag)
				{
					FXWindowBegin = Event.GetTime(); FXWindowEnd = Event.GetTime() + Event.GetDuration();
				}
			}
			if (!FMath::IsNearlyEqual(FXWindowBegin, 0.05f) || !FMath::IsNearlyEqual(FXWindowEnd, 0.217f))
			{
				Fail(TEXT("formal FX project-test window differs from the selected .05-.217 seconds")); return false;
			}
			FXEventTags.AddTag(Step.FX.BeginEventTag); FXEventTags.AddTag(Step.FX.EndEventTag);
			FXEventHandle = ASC->AddGameplayEventTagContainerDelegate(FXEventTags,
				FGameplayEventTagMulticastDelegate::FDelegate::CreateLambda([this](FGameplayTag Tag, const FGameplayEventData* Data)
				{
					if (!Data || Data->EventTag != Tag || Data->OptionalObject2 != Mesh.Get() || !FMath::IsFinite(Data->EventMagnitude)) { return; }
					// Observe the exact old source before filtering current-playback facts. A filtered
					// old End count of zero cannot prove that native NotifyEnd has not been dispatched.
					if (Tag == GGYGOGameplayTags::Event_Montage_FXWindowEnd && Data->OptionalObject == Montage.Get()
						&& FXCancelledInstanceId != INDEX_NONE
						&& static_cast<double>(Data->EventMagnitude) == static_cast<double>(FXCancelledInstanceId) + 1.0)
					{
						if (FXCancelledEndFrame != MAX_uint64) { Fail(TEXT("cancelled native FX source dispatched duplicate End")); return; }
						FXCancelledEndFrame = GFrameCounter;
					}
					auto* Task = FGGYGOPlayerComboLifecycleFixture::GetProductionMontageTask(Ability.Get());
					FGGYGOMontageSectionSnapshot Snapshot;
					if (!Task || !Task->TryGetOriginalSectionSnapshot(Snapshot)
						|| Data->OptionalObject != Snapshot.Montage
						|| static_cast<double>(Data->EventMagnitude) != static_cast<double>(Snapshot.MontageInstanceId) + 1.0)
					{
						++FXRejectedOldEvents; return; // Old queued End/Begin never proves a successor's event.
					}
					if (Tag == GGYGOGameplayTags::Event_Montage_FXWindowBegin)
					{
						++FXBeginCounts.FindOrAdd(Snapshot.MontageInstanceId);
						if (bFXRestarted && Snapshot.MontageInstanceId != FXCancelledInstanceId) { FXRestartBeginFrame = GFrameCounter; }
					}
					else if (Tag == GGYGOGameplayTags::Event_Montage_FXWindowEnd) { ++FXEndCounts.FindOrAdd(Snapshot.MontageInstanceId); }
				}));
			if (!FXEventHandle.IsValid()) { Fail(TEXT("original FX event observation could not register")); return false; }
			return true;
		}
		bool HasOriginalActiveFXNotify(int32 OriginalInstanceId) const
		{
			if (!AnimInstance.IsValid() || AnimInstance->ActiveAnimNotifyState.Num() != AnimInstance->ActiveAnimNotifyEventReference.Num()) { return false; }
			int32 Matches = 0;
			for (int32 Index = 0; Index < AnimInstance->ActiveAnimNotifyState.Num(); ++Index)
			{
				const auto* Notify = Cast<UGGYGOAnimNotifyState_GameplayEventWindow>(AnimInstance->ActiveAnimNotifyState[Index].NotifyStateClass.Get());
				const auto* Context = AnimInstance->ActiveAnimNotifyEventReference[Index].GetContextData<UE::Anim::FAnimNotifyMontageInstanceContext>();
				if (Notify && Notify->GetOuter() == Montage.Get() && Context && Context->MontageInstanceID == OriginalInstanceId
					&& Notify->GetBeginEventTag() == GGYGOGameplayTags::Event_Montage_FXWindowBegin
					&& Notify->GetEndEventTag() == GGYGOGameplayTags::Event_Montage_FXWindowEnd) { ++Matches; }
			}
			return Matches == 1;
		}
		bool FXComponentRetired() const
		{
			UNiagaraComponent* Component = FXComponent.Get(true);
			return !Component || (Component->IsBeingDestroyed() && !Component->IsRegistered());
		}
		void ObserveFX()
		{
			if (bFXVerified || bFailed) { return; }
			const int32 Begins = FXBeginCounts.FindRef(InstanceId), Ends = FXEndCounts.FindRef(InstanceId);
			if (Begins > 1 || Ends > 1) { Fail(TEXT("original native FX window duplicated Begin/End")); return; }
			if (!bFXCaptured)
			{
				TInlineComponentArray<UNiagaraComponent*> Components(Character.Get());
				UNiagaraComponent* Found = nullptr;
				for (UNiagaraComponent* Component : Components)
				{
					if (!IsValid(Component) || Component->GetAsset() != FXSystem.Get()) { continue; }
					if (Found) { Fail(TEXT("multiple components of the selected NS in the original Pawn")); return; }
					Found = Component;
				}
				if (!Found) { return; }
				if (Begins != 1 || Ends != 0 || Found->GetAttachParent() != Mesh.Get() || Found->GetWorld() != World.Get())
				{
					Fail(TEXT("seven-color component was not observed inside its real original attached FX window")); return;
				}
				auto* Task = FGGYGOPlayerComboLifecycleFixture::GetProductionFXTask(Ability.Get());
				if (!Task || Task->GetStartResult() != EGGYGONiagaraEffectStartResult::SpawnAndActivationRequested)
				{
					Fail(TEXT("original component has no accepted original FX Task request")); return;
				}
				FXTask.Reset(Task); FXComponent = Found; bFXCaptured = true;
			}
			UNiagaraComponent* Component = FXComponent.Get();
			if (Component && !bFXParticles)
			{
				const auto NiagaraController = Component->GetSystemInstanceController();
				if (NiagaraController.IsValid() && NiagaraController->IsValid()
					&& !NiagaraController->IsPendingSpawn() && !NiagaraController->IsComplete() && NiagaraController->GetAge() > 0.0f)
				{
					if (!FXCache.Get()) { FXCache.Reset(NewObject<UNiagaraSimCache>(GetTransientPackage())); }
					FNiagaraSimCacheCreateParameters Parameters;
					Parameters.bAllowDataInterfaceCaching = false;
					UNiagaraSimCache* Captured = nullptr;
					// The exported capture validates the original simulation and current data
					// through Niagara's native BeginWrite/WriteFrame before accepting a frame.
					if (!UNiagaraSimCacheFunctionLibrary::CaptureNiagaraSimCacheImmediate(FXCache.Get(), Parameters, Component, Captured, false)
						|| Captured != FXCache.Get() || Captured->GetSystem() != FXSystem.Get() || Captured->GetNumFrames() != 1
						|| Captured->GetNumEmitters() != 7)
					{
						Fail(TEXT("native non-advancing particle capture failed; it cannot stand in for zero particles")); return;
					}
					int32 Particles = 0;
					for (int32 Emitter = 0; Emitter < Captured->GetNumEmitters(); ++Emitter)
					{
						Particles += Captured->GetEmitterNumInstances(Emitter, 0);
					}
					if (Particles > 0)
					{
						bFXParticles = true;
						Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.FXProductionSmoke] Probe=%u Instance=%d Component=%s ActualParticles=%d CaptureAdvance=0."),
							static_cast<uint32>(FXProbe), InstanceId, *Component->GetPathName(), Particles));
					}
				}
			}
			if (FXProbe == EComboFXProbe::WindowCancel && !bFXRestarted)
			{
				if (!bFXParticles) { return; }
				FGGYGOMontageSectionSnapshot Snapshot;
				if (Ends != 0 || !ReadOriginalPlayback(Snapshot) || Snapshot.PositionSeconds >= FXWindowEnd || !Component)
				{
					Fail(TEXT("particle-bearing FX cancellation missed the real author window")); return;
				}
				CancelFXOriginal(); return;
			}
			if (FXProbe == EComboFXProbe::WindowCancel && bFXRestarted && Begins == 1 && Ends == 0
				&& FXCancelledEndFrame != MAX_uint64
				&& (!Component || !FXTask.Get() || FXTask->GetState() == EGameplayTaskState::Finished
					|| FGGYGOPlayerComboLifecycleFixture::GetProductionFXTask(Ability.Get()) != FXTask.Get()
					|| Component->GetAttachParent() != Mesh.Get()))
			{
				Fail(TEXT("old native FX End retired or detached the exact successor resource before its real authored End")); return;
			}
			if (Ends == 1 && Component && !bFXDetached)
			{
				const auto NiagaraController = Component->GetSystemInstanceController();
				if (Component->GetAttachParent() || FGGYGOPlayerComboLifecycleFixture::GetProductionFXTask(Ability.Get())
					|| !FXTask.Get() || FXTask->GetState() != EGameplayTaskState::Finished
					|| !NiagaraController.IsValid() || !NiagaraController->IsValid()
					|| NiagaraController->GetRequestedExecutionState() != ENiagaraExecutionState::Inactive)
				{
					Fail(TEXT("real author End did not detach the exact original tail and request native inactive state")); return;
				}
				bFXDetached = true;
			}
			if (FXComponentRetired())
			{
				if (Begins != 1 || Ends != 1 || !bFXParticles || !bFXDetached || !FXTask.Get()
					|| FXTask->GetState() != EGameplayTaskState::Finished)
				{
					Fail(TEXT("original tail retired without observed particles, authored End and native detach/inactive facts")); return;
				}
				if (FXProbe == EComboFXProbe::WindowCancel && bFXNativePlaybackOverlap && FXCancelledEndFrame == MAX_uint64)
				{
					Fail(TEXT("observed native playback overlap never dispatched the exact old active FX NotifyEnd")); return;
				}
				if (FXProbe == EComboFXProbe::WindowCancel)
				{
					Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.FXProductionSmoke] NoMergePlaybackOverlap=%s CancelFrame=%llu NewPlaybackObservedFrame=%llu NewBeginFrame=%llu OldEndFrame=%llu OldInstance=%d NewInstance=%d; frame numbers are diagnostics, NOT_OBSERVED leaves NoMerge dynamic coverage open."),
						bFXNativePlaybackOverlap ? TEXT("OBSERVED") : TEXT("NOT_OBSERVED"),
						static_cast<unsigned long long>(FXCancelFrame), static_cast<unsigned long long>(FXRestartPlaybackFrame),
						static_cast<unsigned long long>(FXRestartBeginFrame), static_cast<unsigned long long>(FXCancelledEndFrame), FXCancelledInstanceId, InstanceId));
				}
				bFXVerified = true;
				Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.FXProductionSmoke] Probe=%u Instance=%d Begin=1 End=1 TailDetached=1 ActualComponentRetired=1 OldEventsRejected=%d."),
					static_cast<uint32>(FXProbe), InstanceId, FXRejectedOldEvents));
			}
		}
		void CancelFXOriginal()
		{
			bFXCancelledWithActiveNotify = FXProbe == EComboFXProbe::WindowCancel && HasOriginalActiveFXNotify(InstanceId);
			FXCancelledActivation = OriginalActivation; FXCancelledInstanceId = InstanceId;
			FXCancelFrame = GFrameCounter;
			Stage = EStage::FXCancelWait; Deadline = FPlatformTime::Seconds() + 3.0;
			ASC->CancelAbilityHandle(SpecHandle); // Explicit native GAS cancellation; never pretend this is movement before CancelPoint.
			if (FXProbe == EComboFXProbe::WindowCancel) { VerifyFXCancellation(); }
		}
		void VerifyFXCancellation()
		{
			if (EndCount == 0 || CompletedCount == 0 || PlayerInput->IsPressed(AttackKey)) { return; }
			if (EndCount != 1 || CompletedCount != 1 || !EndData.bWasCancelled || !Completion.HasCompletion()
				|| !Completion.GetOriginal().GetOriginalActivation().HasSameActivation(FXCancelledActivation)
				|| Ability->IsActive() || !bResourcesRestoredAtEnd || !ResourcesRestored()
				|| FXInputObservationTick.IsTickFunctionRegistered()
				|| (bFXCaptured && (!FXComponentRetired() || !FXTask.Get() || FXTask->GetState() != EGameplayTaskState::Finished)))
			{
				Fail(TEXT("native FX cancellation did not retire the exact original GA/Task/component resources")); return;
			}
			Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.FXProductionSmoke] Probe=%u CancelledInstance=%d NativeEnd=1 Completed=1 ResourcesRestored=1 CapturedComponentRetired=%d."),
				static_cast<uint32>(FXProbe), FXCancelledInstanceId, bFXCaptured));
			EndCount = 0; CompletedCount = 0; EndData = {}; Completion = {};
			OriginalActivation = {}; MontageTask.Reset(); InputTask.Reset();
			bResourcesRestoredAtEnd = false; bEndSnapshotAvailable = false; bStoppedAtEnd = false;
			// Repeat the real same-Montage attack after both pre-window and active-window cancellation.
			// Key submission alone is not a same-frame activation or native notify overlap proof.
			FXTask.Reset(); FXComponent.Reset(); FXCache.Reset();
			bFXCaptured = false; bFXParticles = false; bFXDetached = false;
			bFXRestarted = true; Stage = EStage::Main; Deadline = FPlatformTime::Seconds() + 3.0;
			if (!InstallFXInputObservation()) { return; }
			bAttackDown = true; SendNativeKey(AttackKey, true);
		}
		bool HasMeshPrerequisite() const
		{
			return Trace.IsValid() && Mesh.IsValid() && Trace->PrimaryComponentTick.GetPrerequisites().ContainsByPredicate(
				[this](const FTickPrerequisite& Prerequisite) { return Prerequisite.Get() == &Mesh->PrimaryComponentTick; });
		}
		bool IsProductionChainCurrent() const
		{
			FGGYGOMovementOwnerSyncScopeId CurrentScope;
			FString Error;
			UGGYGOPawnExtensionComponent* Extension = UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(Character.Get());
			return IsOwnedWorldCurrent() && LocalPlayer.IsValid() && Controller.IsValid() && Character.IsValid()
				&& PlayerInput.IsValid() && ASC.IsValid() && Ability.IsValid() && Movement.IsValid() && Mesh.IsValid()
				&& AnimInstance.IsValid() && Trace.IsValid() && Viewport.IsValid() && ViewportWidget.IsValid()
				&& OriginalOrigin.IsValid() && LocalPlayer->GetMovementInputOriginResource() == OriginalOrigin.Get()
				&& OwnerNotice.IsSet() && OwnerNotice.GetScope() == Scope && OwnerNotice.GetState() == EGGYGOMovementOwnerSyncState::Ready
				&& LocalPlayer->PlayerController == Controller.Get() && Controller->GetPawn() == Character.Get()
				&& Character->GetController() == Controller.Get() && Controller->PlayerInput == PlayerInput.Get()
				&& Character->GetMesh() == Mesh.Get() && Mesh->GetAnimInstance() == AnimInstance.Get()
				&& Extension && Extension->GetGGYGOAbilitySystemComponent() == ASC.Get()
				&& World->GetGameViewport() == Viewport.Get() && Viewport->GetGameViewport() == SceneViewport
				&& Viewport->GetGameViewportWidget() == ViewportWidget
				&& FSlateApplication::Get().GetKeyboardFocusedWidget() == ViewportWidget
				&& Movement->GetMovementOwnerSyncScope(CurrentScope, Error) && CurrentScope == Scope;
		}
		bool ResolveNativeKeyboardCodes(const FKey& Key, uint32& OutKeyCode, uint32& OutCharacterCode)
		{
			const uint32* KeyCode = nullptr;
			const uint32* CharacterCode = nullptr;
			const FInputKeyManager& KeyManager = FInputKeyManager::Get();
			KeyManager.GetCodesFromKey(Key, KeyCode, CharacterCode);
			if (!KeyCode && !CharacterCode)
			{
				Fail(TEXT("configured keyboard key has no native platform encoding: ") + Key.ToString()); return false;
			}
			// Slate resolves the platform pair virtual-first, then character; either code may be absent.
			OutKeyCode = KeyCode ? *KeyCode : 0;
			OutCharacterCode = CharacterCode ? *CharacterCode : 0;
			const FKey RoundTripKey = KeyManager.GetKeyFromCodes(OutKeyCode, OutCharacterCode);
			if (RoundTripKey != Key)
			{
				Fail(FString::Printf(TEXT("native platform encoding did not resolve the original keyboard key: Key=%s KeyCode=%u CharacterCode=%u ResolvedKey=%s"),
					*Key.ToString(), OutKeyCode, OutCharacterCode, *RoundTripKey.ToString())); return false;
			}
			return true;
		}
		bool SendNativeKey(const FKey& Key, bool bDown)
		{
			FSlateApplication& Slate = FSlateApplication::Get();
			if (Key.IsMouseButton())
			{
				EMouseButtons::Type Button;
				if (Key == EKeys::LeftMouseButton) { Button = EMouseButtons::Left; }
				else if (Key == EKeys::RightMouseButton) { Button = EMouseButtons::Right; }
				else if (Key == EKeys::MiddleMouseButton) { Button = EMouseButtons::Middle; }
				else { Fail(TEXT("unsupported configured mouse attack key")); return false; }
				if (!ViewportWidget.IsValid() || !NativeWindow.IsValid() || !NativeWindow->GetNativeWindow().IsValid()
					|| ViewportWidget->GetCachedGeometry().GetLocalSize().X <= 0 || ViewportWidget->GetCachedGeometry().GetLocalSize().Y <= 0)
				{
					Fail(TEXT("native viewport geometry unavailable for mouse route")); return false;
				}
				const FVector2D Point = ViewportWidget->GetCachedGeometry().LocalToAbsolute(ViewportWidget->GetCachedGeometry().GetLocalSize() * 0.5f);
				return bDown ? Slate.OnMouseDown(NativeWindow->GetNativeWindow(), Button, Point) : Slate.OnMouseUp(Button, Point);
			}
			uint32 KeyCode = 0, CharacterCode = 0;
			if (!ResolveNativeKeyboardCodes(Key, KeyCode, CharacterCode)) { return false; }
			return bDown ? Slate.OnKeyDown(KeyCode, CharacterCode, false)
				: Slate.OnKeyUp(KeyCode, CharacterCode, false);
		}
		void TryCaptureMain()
		{
			if (EndCount != 0) { Fail(TEXT("attack ended before its original Main was observed")); return; }
			if (!Ability->IsActive()) { return; }
			const int32 ActualStep = Ability->GetCurrentComboStep();
			if (ActualStep != ExpectedStep)
			{
				if (ReplacedTask.Get() && ActualStep == ReplacedStep) { return; }
				Fail(TEXT("real attack input started an unexpected production step")); return;
			}
			FGGYGOComboStep Step;
			UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = FGGYGOPlayerComboLifecycleFixture::GetProductionMontageTask(Ability.Get());
			FGGYGOMontageSectionSnapshot Snapshot;
			if (!FGGYGOPlayerComboLifecycleFixture::ReadProductionStep(Ability.Get(), Step)
				|| !Task || !Task->TryGetOriginalSectionSnapshot(Snapshot)) { return; }
			if (Snapshot.Montage != Step.Montage || Snapshot.SectionName != Step.MainSection || !Movement->HasActiveActionMotion())
			{
				Fail(TEXT("normal activation did not expose original Main and CMC action resource")); return;
			}
			const FGGYGOAbilityActivationHandle Captured = Ability->CaptureCurrentActivation();
			if (!Captured.HasActivation() || (OriginalActivation.HasActivation() && !Captured.HasSameActivation(OriginalActivation)))
			{
				Fail(TEXT("End attack replaced the original GA activation instead of its owned step")); return;
			}
			if (ReplacedTask.Get())
			{
				const FAnimMontageInstance* Previous = AnimInstance->GetMontageInstanceForID(ReplacedInstanceId);
				// Allow native frame scheduling, never the original multi-second End duration.
				const float ObservationBudget = FMath::Max(0.25f, 3.0f * World->GetDeltaSeconds());
				if (Task == ReplacedTask.Get() || Snapshot.MontageInstanceId == ReplacedInstanceId
					|| World->GetTimeSeconds() - ReplacementRequestWorldTime > ObservationBudget
					|| ReplacedTask->GetState() != EGameplayTaskState::Finished
					|| ReplacedTask->EventReceived.IsBound() || ReplacedTask->SectionReceived.IsBound()
					|| ReplacedTask->NotifyReceived.IsBound()
					|| AnimInstance->OnPlayMontageNotifyBegin.Contains(ReplacedTask.Get(), FName(TEXT("OnOriginalMontageNotifyBegin")))
					|| ReplacedTask->OnCompleted.IsBound() || ReplacedTask->OnInterrupted.IsBound()
					|| ReplacedTask->OnBlendOut.IsBound() || (Previous && Previous->IsActive() && !Previous->IsStopped())
					|| !InputTask.Get() || InputTask.Get() != FGGYGOPlayerComboLifecycleFixture::GetProductionInputTask(Ability.Get()))
				{
					Fail(TEXT("End attack did not retire the exact old Task/playback while retaining the original input task")); return;
				}
				Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.EndProductionSmoke] EndAttackReplacement OldStep=%d OldInstance=%d Remaining=%.6f NewStep=%d NewInstance=%d ResponseSeconds=%.6f SameActivation=1 OldTaskFinished=1 ActionActive=1."),
					ReplacedStep, ReplacedInstanceId, ReplacedEndRemaining, ActualStep, Snapshot.MontageInstanceId,
					World->GetTimeSeconds() - ReplacementRequestWorldTime));
				ReplacedTask.Reset();
			}
			if (bCheckingMovementReset)
			{
				if (ActualStep != 0 || Captured.HasSameActivation(PreviousActivation))
				{
					Fail(TEXT("movement interruption did not reset the next real attack to a new activation at step 01")); return;
				}
				Test.AddInfo(TEXT("[Combat.PlayerCombo.EndProductionSmoke] MovementReset: next real attack begins at step 01 with a new original activation."));
				bCheckingMovementReset = false;
			}
			Montage = Snapshot.Montage.Get();
			InstanceId = Snapshot.MontageInstanceId;
			MainSection = Step.MainSection; EndSection = Step.EndSection;
			const int32 EndIndex = Montage->GetSectionIndex(EndSection);
			if (EndIndex == INDEX_NONE) { Fail(TEXT("configured original End section missing")); return; }
			Montage->GetSectionStartAndEndTime(EndIndex, EndStart, EndEnd);
			if (!FMath::IsFinite(EndStart) || !FMath::IsFinite(EndEnd) || EndEnd <= EndStart)
			{
				Fail(TEXT("original End range invalid")); return;
			}
			FGGYGOMontageNotifyFact PointSnapshot;
			float PointReadPosition = 0.0f;
			if (!Task->TryGetOriginalMontageNotifySnapshotByName(Step.InterruptionNotifyName, PointSnapshot, PointReadPosition)
				|| PointSnapshot.Montage != Snapshot.Montage || PointSnapshot.MontageInstanceId != Snapshot.MontageInstanceId
				|| PointSnapshot.NotifyName != Step.InterruptionNotifyName || PointSnapshot.Mesh != Mesh.Get()
				|| (PointReadPosition < PointSnapshot.NotifyPositionSeconds && Ability->IsInterruptionOpen())
				|| (Snapshot.SectionName == Step.MainSection && Ability->IsMovementInterruptionOpen()))
			{
				Fail(TEXT("new original playback did not reset its author-point permission or snapshot identity")); return;
			}
			AuthorPointPosition = PointSnapshot.NotifyPositionSeconds;
			OriginalAuthorPoint = PointSnapshot;
			OriginalMovementPoint = {};
			if (Step.MovementInterruptionMode == EGGYGOActionInterruptionMode::AuthoredNotify)
			{
				float MovementReadPosition = 0.0f;
				if (!Task->TryGetOriginalMontageNotifySnapshotByName(Step.MovementInterruptionNotifyName,
					OriginalMovementPoint, MovementReadPosition)
					|| OriginalMovementPoint.NotifyName != Step.MovementInterruptionNotifyName
					|| OriginalMovementPoint.Montage != Snapshot.Montage || OriginalMovementPoint.Mesh != Mesh.Get()
					|| OriginalMovementPoint.MontageInstanceId != Snapshot.MontageInstanceId
					|| OriginalMovementPoint.NotifyAuthoredPositionSeconds != EndStart
					|| OriginalMovementPoint.NotifyPositionSeconds > EndStart
					|| (MovementReadPosition < EndStart && Ability->IsMovementInterruptionOpen()))
				{
					Fail(TEXT("new original movement point identity or exact Main-end permission floor is invalid")); return;
				}
			}
			NaturalEarliestEndWorldSeconds = World->GetTimeSeconds() + (EndEnd - Snapshot.PositionSeconds) / Task->GetEffectivePlayRate();
			MontageTask.Reset(Task);
			InputTask.Reset(FGGYGOPlayerComboLifecycleFixture::GetProductionInputTask(Ability.Get()));
			OriginalActivation = Captured;
			NaturalInputRequestId = FGGYGOPlayerComboLifecycleFixture::ReadProductionLastInputRequest(Ability.Get());
			if (!OriginalActivation.HasActivation() || !InputTask.IsValid()) { Fail(TEXT("original GA/input task identity unavailable")); return; }
			SendNativeKey(AttackKey, false);
			if (FXProbe == EComboFXProbe::None || !PlayerInput->IsPressed(AttackKey)) { bAttackDown = false; }
			if (FXProbe != EComboFXProbe::None)
			{
				LogNativeInputBoundary(TEXT("OriginalMainKeyUpSubmitted"));
				ReleaseFXInputObservation();
			}
			Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.EndProductionSmoke] OriginalMain Montage=%s Instance=%d Position=%.6f End=[%.6f,%.6f)."),
				*Montage->GetPathName(), InstanceId, Snapshot.PositionSeconds, EndStart, EndEnd));
			if (FXProbe != EComboFXProbe::None)
			{
				if (bFXRestarted && (InstanceId == FXCancelledInstanceId || Captured.HasSameActivation(FXCancelledActivation)))
				{
					Fail(TEXT("FX restart reused the cancelled native playback or original activation")); return;
				}
				if (bFXRestarted && FXProbe == EComboFXProbe::WindowCancel)
				{
					// This exact Task snapshot proves the new native playback already exists.
					// An activated callback or key submission alone occurs too early to prove that.
					FXRestartPlaybackFrame = GFrameCounter;
					bFXNativePlaybackOverlap = bFXCancelledWithActiveNotify && FXCancelledEndFrame == MAX_uint64
						&& HasOriginalActiveFXNotify(FXCancelledInstanceId);
				}
				if (FXProbe == EComboFXProbe::EarlyCancelRestart && !bFXRestarted)
				{
					if (Snapshot.PositionSeconds >= FXWindowBegin || FXBeginCounts.FindRef(InstanceId) != 0
						|| FGGYGOPlayerComboLifecycleFixture::GetProductionFXTask(Ability.Get()))
					{
						Fail(TEXT("early FX cancellation was not observed before the real author Begin")); return;
					}
					ReleaseFXInputObservation();
					Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.FXProductionSmoke] EarlyCancelBoundary=AfterOriginalControllerInputBeforePawnCMCAndMesh Position=%.6f Begin=%.6f Instance=%d; real Task/CMC established, no manual input or pose tick."),
						Snapshot.PositionSeconds, FXWindowBegin, InstanceId));
					CancelFXOriginal();
					return;
				}
				Stage = EStage::Natural;
				Deadline = FPlatformTime::Seconds() + EndEnd + 5.0;
				return;
			}
			if (PoseProbe == EComboPoseFailureProbe::RuntimeInvalidated) { BeginPoseRuntimeFault(); return; }
			if (bRestartSent)
			{
				bRestartCaptured = true;
				Stage = EStage::Natural;
				Deadline = FPlatformTime::Seconds() + EndEnd + 5.0;
				return;
			}
			if (bFinalResetAttack)
			{
				Stage = EStage::Natural;
				Deadline = FPlatformTime::Seconds() + EndEnd + 5.0;
				return;
			}
			if (ActualStep < TargetStep)
			{
				Stage = EStage::AdvanceEnd;
				Deadline = FPlatformTime::Seconds() + EndEnd + 5.0;
				return;
			}
			if (EndAttackProbe != EComboEndAttackProbe::None)
			{
				Stage = EndAttackProbe == EComboEndAttackProbe::Restart ? EStage::MainAttackPress : EStage::Natural;
				Deadline = FPlatformTime::Seconds() + EndEnd + 5.0;
				return;
			}
			// 移动取消只由本段独立移动作者点授予；生产资产未配置时明确失败，不以攻击点或 End 代开。
			if (Step.MovementInterruptionMode != EGGYGOActionInterruptionMode::AuthoredNotify
				|| Step.MovementInterruptionNotifyName.IsNone()
				|| Step.MovementInterruptionNotifyName == Step.InterruptionNotifyName)
			{
				Fail(TEXT("production step has no independent authored movement point; movement cancellation is not granted")); return;
			}
			Stage = EStage::End;
			Deadline = FPlatformTime::Seconds() + 10.0;
			if (bHeldDuringMain) { bWDown = true; SendNativeKey(EKeys::W, true); }
		}
		bool ReadOriginalPlayback(FGGYGOMontageSectionSnapshot& Snapshot)
		{
			if (EndCount != 0 || CompletedCount != 0 || !Ability->IsActive()
				|| !Ability->CaptureCurrentActivation().HasSameActivation(OriginalActivation)
				|| !MontageTask.Get() || !MontageTask->TryGetOriginalSectionSnapshot(Snapshot)
				|| Snapshot.Montage != Montage.Get() || Snapshot.MontageInstanceId != InstanceId)
			{
				Fail(TEXT("original native playback/activation changed before the issued End input")); return false;
			}
			return true;
		}
		void SendEndAttack(const FGGYGOMontageSectionSnapshot& Snapshot, int32 NextStep)
		{
			FGGYGOComboStep Step;
			if (!FGGYGOPlayerComboLifecycleFixture::ReadProductionStep(Ability.Get(), Step)
				|| Step.NextStepIndex != NextStep || !Ability->IsInterruptionOpen()
				|| Snapshot.PositionSeconds < AuthorPointPosition || Snapshot.PositionSeconds >= EndEnd
				|| !Movement->HasActiveActionMotion() || PlayerInput->IsPressed(AttackKey))
			{
				Fail(TEXT("configured End target, remaining trajectory or real released attack edge is unavailable")); return;
			}
			ReplacedTask.Reset(MontageTask.Get());
			ReplacedStep = Ability->GetCurrentComboStep();
			ReplacedInstanceId = InstanceId;
			ReplacedEndRemaining = EndEnd - Snapshot.PositionSeconds;
			ReplacementRequestWorldTime = World->GetTimeSeconds();
			ExpectedStep = NextStep;
			Stage = EStage::Main;
			Deadline = FPlatformTime::Seconds() + 3.0;
			bAttackDown = true;
			SendNativeKey(AttackKey, true);
		}
		void AdvanceAtEnd()
		{
			FGGYGOMontageSectionSnapshot Snapshot;
			if (!ReadOriginalPlayback(Snapshot) || Snapshot.SectionName != EndSection) { return; }
			SendEndAttack(Snapshot, Ability->GetCurrentComboStep() + 1);
		}
		void PressAttackDuringTerminalMain()
		{
			FGGYGOMontageSectionSnapshot Snapshot;
			if (!ReadOriginalPlayback(Snapshot)) { return; }
			if (Snapshot.SectionName != MainSection) { Fail(TEXT("terminal Main ended before its real rejection probe")); return; }
			if (PlayerInput->IsPressed(AttackKey)) { return; } // The previously issued key-up must reach native input.
			MainAttackRequest = FGGYGOPlayerComboLifecycleFixture::ReadProductionLastInputRequest(Ability.Get());
			bAttackDown = true;
			SendNativeKey(AttackKey, true);
			Stage = EStage::MainAttackRejected;
		}
		void ObserveRejectedMainAttack()
		{
			FGGYGOMontageSectionSnapshot Snapshot;
			if (!ReadOriginalPlayback(Snapshot)) { return; }
			if (FGGYGOPlayerComboLifecycleFixture::ReadProductionLastInputRequest(Ability.Get()) <= MainAttackRequest) { return; }
			if (Ability->GetCurrentComboStep() != TargetStep || Snapshot.SectionName != MainSection
				|| Snapshot.PositionSeconds >= AuthorPointPosition || Ability->IsInterruptionOpen()
				|| Ability->HasPendingComboRequest() || !Movement->HasActiveActionMotion())
			{
				Fail(TEXT("terminal Main real attack was allowed to replace its original step")); return;
			}
			bMainAttackRejected = true;
			SendNativeKey(AttackKey, false); bAttackDown = false;
			Test.AddInfo(TEXT("[Combat.PlayerCombo.EndProductionSmoke] TerminalMain: a delivered new attack request was rejected; original step/instance remains."));
			Stage = EStage::EndAttack;
		}
		void ObserveEndAttack()
		{
			FGGYGOMontageSectionSnapshot Snapshot;
			if (!ReadOriginalPlayback(Snapshot) || !Ability->IsInterruptionOpen()) { return; }
			if (Ability->IsMovementInterruptionOpen() && Snapshot.SectionName == MainSection)
			{
				Fail(TEXT("attack continuation point opened movement interruption inside Main")); return;
			}
			if (!bMainAttackRejected) { Fail(TEXT("terminal Main rejection was not observed before End restart")); return; }
			if (Snapshot.SectionName != MainSection || Snapshot.PositionSeconds >= EndStart)
			{
				Fail(TEXT("author point was not able to open attack replacement inside Main")); return;
			}
			bRestartSent = true;
			SendEndAttack(Snapshot, 0);
		}
		void ObserveNaturalEnd()
		{
			if (EndCount != 0 || CompletedCount != 0) { VerifyCancellation(); return; }
			FGGYGOMontageSectionSnapshot Snapshot;
			if (!Ability->IsActive() || !MontageTask.IsValid() || !MontageTask->TryGetOriginalSectionSnapshot(Snapshot)
				|| Snapshot.Montage != Montage.Get() || Snapshot.MontageInstanceId != InstanceId
				|| !Ability->CaptureCurrentActivation().HasSameActivation(OriginalActivation))
			{
				Fail(TEXT("original active montage/GA observation lost before cancellation")); return;
			}
			FGGYGOQualifiedMovementIntent Intent;
			FString Error;
			const EGGYGOQualifiedMovementIntentQueryResult Result = Movement->QueryQualifiedMovementIntent(Scope, Intent, Error);
			// 移动作者点可按 OffsetBefore 在 Main 内触发，但真实位置未达 Main 末端（End 起点）前不得开门或取消。
			if (Ability->IsMovementInterruptionOpen() && Snapshot.PositionSeconds < EndStart)
			{
				Fail(TEXT("movement interruption opened before the original playback reached the authored Main end")); return;
			}
			if (bHeldDuringMain && Snapshot.SectionName == MainSection)
			{
				if (Result == EGGYGOQualifiedMovementIntentQueryResult::Qualified
					&& Intent.Provenance == EGGYGOQualifiedMovementIntentProvenance::LocalSourceHeld && PlayerInput->IsPressed(EKeys::W))
				{
					if (bSawQualifiedMain && !(Intent == MainIntent)) { Fail(TEXT("Held changed the original request during Main")); return; }
					MainIntent = Intent; bSawQualifiedMain = true;
				}
				return;
			}
			if (Snapshot.SectionName != EndSection) { return; }
			if (Snapshot.PositionSeconds < EndStart || Snapshot.PositionSeconds >= EndEnd)
			{
				Fail(TEXT("natural End has no remaining original timeline for new input")); return;
			}
			bSawNaturalEnd = true;
			if (!Movement->HasActiveActionMotion())
			{
				Fail(TEXT("original CMC action resource was released at End entry before real movement input")); return;
			}
			if (!bHeldDuringMain)
			{
				const EGGYGOMovementInputOriginQualification Qualification = OriginalOrigin->GetQualification(PlayerInput.Get());
				LogPreflight(TEXT("NaturalEndBeforeFirstW"), Qualification, Result, Error);
				const bool bOriginalFirstPress = TargetStep == 0
					&& Qualification == EGGYGOMovementInputOriginQualification::Cold
					&& (Result == EGGYGOQualifiedMovementIntentQueryResult::AwaitingPhysicalProof
						|| Result == EGGYGOQualifiedMovementIntentQueryResult::NotHeld);
				const bool bOriginalReleasedRequest = TargetStep > 0
					&& Qualification == EGGYGOMovementInputOriginQualification::Rearm
					&& Result == EGGYGOQualifiedMovementIntentQueryResult::NotHeld;
				if (PlayerInput->IsPressed(EKeys::W) || (!bOriginalFirstPress && !bOriginalReleasedRequest))
				{
					Fail(TEXT("new-press case lacks its original Cold or real released/rearmed source state before End")); return;
				}
				bWDown = true; SendNativeKey(EKeys::W, true);
			}
			Stage = EStage::Cancel;
		}
		void ObserveAbilityEnd(const FAbilityEndedData& Data)
		{
			if (Data.AbilityThatEnded != Ability.Get()) { return; }
			if (PoseProbe != EComboPoseFailureProbe::None) { ObservePoseAbilityEnd(Data); return; }
			++EndCount; EndData = Data;
			EndObservedWorldSeconds = World.IsValid() ? World->GetTimeSeconds() : -1.0f;
			if (Data.AbilitySpecHandle != SpecHandle || !OriginalActivation.HasActivation())
			{
				Fail(TEXT("original activation/spec was unavailable at native ability end")); return;
			}
			FAnimMontageInstance* Instance = AnimInstance.IsValid() && Mesh.IsValid() && Mesh->GetAnimInstance() == AnimInstance.Get()
				? AnimInstance->GetMontageInstanceForID(InstanceId) : nullptr;
			// Stop does not normally delete this instance in the synchronous End stack.
			// Legal reentrant destruction is an unavailable observation, never a cached successful End proof.
			bEndSnapshotAvailable = Instance && Instance->Montage == Montage.Get();
			if (bEndSnapshotAvailable)
			{
				EndPosition = Instance->GetPosition();
				EndedSection = Instance->GetCurrentSection();
				bStoppedAtEnd = Instance->IsStopped() || !Instance->IsActive();
			}
			FString Error;
			EndIntentResult = Movement->QueryQualifiedMovementIntent(Scope, EndIntent, Error);
			bResourcesRestoredAtEnd = ResourcesRestored();
			PositionAtEnd = Character->GetActorLocation();
			Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.EndProductionSmoke] NativeEnd Cancelled=%d Snapshot=%d Montage=%s Instance=%d Section=%s Position=%.6f EndEnd=%.6f Remaining=%.6f Stopped=%d Qualified=%u Session=%llu Request=%llu Execution=%llu ResourcesRestored=%d Diagnostic=%s."),
				Data.bWasCancelled, bEndSnapshotAvailable, *GetPathNameSafe(Montage.Get()), InstanceId, *EndedSection.ToString(),
				EndPosition, EndEnd, EndEnd - EndPosition, bStoppedAtEnd, static_cast<uint32>(EndIntentResult),
				static_cast<unsigned long long>(EndIntent.SessionSerial), static_cast<unsigned long long>(EndIntent.RequestSerial),
				static_cast<unsigned long long>(EndIntent.ExecutionRequestSerial), bResourcesRestoredAtEnd, *Error));
		}
		bool ResourcesRestored() const
		{
			return FGGYGOPlayerComboLifecycleFixture::ProductionResourcesReleased(Ability.Get(), World.Get())
				&& Movement.IsValid() && !Movement->HasActiveActionMotion() && !Movement->IsMovementBlockedByTag()
				&& MontageTask.IsValid() && MontageTask->GetState() == EGameplayTaskState::Finished
				&& InputTask.IsValid() && InputTask->GetState() == EGameplayTaskState::Finished
				&& !MontageTask->OnCompleted.IsBound() && !MontageTask->OnInterrupted.IsBound()
				&& !MontageTask->OnCancelled.IsBound() && !MontageTask->OnBlendOut.IsBound()
				&& !MontageTask->EventReceived.IsBound() && !MontageTask->SectionReceived.IsBound()
				&& !MontageTask->NotifyReceived.IsBound() && AnimInstance.IsValid()
				&& !AnimInstance->OnPlayMontageNotifyBegin.Contains(MontageTask.Get(), FName(TEXT("OnOriginalMontageNotifyBegin")))
				&& !InputTask->OnPress.IsBound() && Trace.IsValid() && !Trace->IsTracing()
				&& Mesh.IsValid() && Mesh->VisibilityBasedAnimTickOption == SavedTick
				&& Mesh->bEnableUpdateRateOptimizations == bSavedURO && HasMeshPrerequisite() == bSavedPrerequisite
				&& Character.IsValid() && Character->GetAnimRootMotionTranslationScale() == SavedRootScale;
		}
		void VerifyCancellation()
		{
			if (EndCount != 1 || CompletedCount != 1 || !EndData.bWasCancelled || !Completion.HasCompletion()
				|| Completion.GetReason() != EGGYGOAbilityTerminationReason::None || !bEndSnapshotAvailable
				// Main 内持续 Held 也只能在原实例真实到达 Main 末端（End 起点）之后取消；
				// 攻击接续点、提前触发的移动点事实或 Main 内位置都不是合法取消证据。
				|| EndedSection != EndSection
				|| !FMath::IsFinite(EndPosition) || EndPosition < EndStart || EndPosition >= EndEnd
				|| !bStoppedAtEnd || EndIntentResult != EGGYGOQualifiedMovementIntentQueryResult::Qualified
				|| EndIntent.Provenance != EGGYGOQualifiedMovementIntentProvenance::LocalSourceHeld
				|| EndIntent.SessionSerial == 0 || EndIntent.RequestSerial == 0 || EndIntent.ExecutionRequestSerial == 0
				|| (bHeldDuringMain ? (!bSawQualifiedMain || !(EndIntent == MainIntent)) : !bSawNaturalEnd)
				|| !bResourcesRestoredAtEnd || !ResourcesRestored() || Ability->IsActive() || !PlayerInput->IsPressed(EKeys::W))
			{
				Fail(TEXT("strict original End-before-finish cancellation/source identity/resource proof failed; see NativeEnd and EndDiagnostics")); return;
			}
			const UGGYGOMovementSet* Set = Movement->GetMovementSet();
			if (!Set || !FMath::IsFinite(Set->WalkToRunHoldSeconds) || Set->WalkToRunHoldSeconds <= 0.0f)
			{
				Fail(TEXT("normal configured Walk/Run hold policy unavailable")); return;
			}
			Stage = EStage::Locomotion;
			Deadline = FPlatformTime::Seconds() + Set->WalkToRunHoldSeconds + 5.0;
		}
		void ObserveLocomotion()
		{
			FGGYGOQualifiedMovementIntent Intent;
			FString Error;
			if (Movement->QueryQualifiedMovementIntent(Scope, Intent, Error) != EGGYGOQualifiedMovementIntentQueryResult::Qualified
				|| !(Intent == EndIntent) || !PlayerInput->IsPressed(EKeys::W) || Ability->IsActive() || !ResourcesRestored())
			{
				Fail(TEXT("same Held request or released GA resources changed during normal locomotion recovery: ") + Error); return;
			}
			const EGGYGOGait Gait = Movement->GetResolvedGait();
			bSawWalk |= Gait == EGGYGOGait::Walk;
			const bool bRun = Gait == EGGYGOGait::Run;
			const UGGYGOMovementSet* Set = Movement->GetMovementSet();
			if (bRun && Movement->HasMoveInput() && Movement->GetHorizontalSpeed() > 10.0f
				&& FVector::Dist2D(Character->GetActorLocation(), PositionAtEnd) > 1.0f
				&& Set && (!Set->bUseCurveDrivenSpeed || Movement->IsCurveDrivingSpeed()))
			{
				LogLocomotionSnapshot(TEXT("CancellationAndLocomotionObserved"));
				if (bHeldDuringMain)
				{
					if (LocomotionObservation == EComboLocomotionObservation::RunHoldAndRelease)
					{
						RunHoldUntilWorldSeconds = World->GetTimeSeconds() + 3.0f;
						Stage = EStage::RunHold;
						Deadline = FPlatformTime::Seconds() + 8.0;
						Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.RunObservation] Boundary=HoldStarted WorldSeconds=%.6f HoldUntil=%.6f; same real W held for 3 game seconds after first Run."),
							World->GetTimeSeconds(), RunHoldUntilWorldSeconds));
					}
					else { BeginEnding(); }
					return;
				}
				PreviousActivation = OriginalActivation;
				OriginalActivation = {};
				EndCount = 0; CompletedCount = 0; EndData = {}; Completion = {};
				bSawQualifiedMain = false; bSawNaturalEnd = false; bSawWalk = false;
				bEndSnapshotAvailable = false; bStoppedAtEnd = false; bResourcesRestoredAtEnd = false;
				MainIntent = {}; EndIntent = {}; EndIntentResult = EGGYGOQualifiedMovementIntentQueryResult::Unavailable;
				bFinalResetAttack = TargetStep >= LastMovementStep;
				if (!bFinalResetAttack) { ++TargetStep; }
				ExpectedStep = 0; bCheckingMovementReset = true;
				Stage = EStage::ResetRelease;
				Deadline = FPlatformTime::Seconds() + 5.0;
				SendNativeKey(EKeys::W, false); bWDown = false;
			}
		}
		void ObserveRunHold()
		{
			FGGYGOQualifiedMovementIntent Intent;
			FString Error;
			if (Movement->QueryQualifiedMovementIntent(Scope, Intent, Error) != EGGYGOQualifiedMovementIntentQueryResult::Qualified
				|| !(Intent == EndIntent) || !PlayerInput->IsPressed(EKeys::W) || !bWDown
				|| PlayerInput->IsPressed(AttackKey) || Ability->IsActive() || !ResourcesRestored())
			{
				Fail(TEXT("original Held request or retired action resources changed during Run observation: ") + Error); return;
			}
			if (World->GetTimeSeconds() < RunHoldUntilWorldSeconds) { return; }
			LogLocomotionSnapshot(TEXT("RunHoldCompleted"));
			SendNativeKey(EKeys::W, false);
			if (bFailed) { return; }
			// Retain the cleanup key obligation until native PlayerInput/Source/CMC
			// confirm release; a Slate reply alone is not release provenance.
			Stage = EStage::RunRelease;
			Deadline = FPlatformTime::Seconds() + 6.0;
			Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.RunObservation] Boundary=ReleaseRequested WorldSeconds=%.6f; real Slate W key-up, awaiting original release facts."), World->GetTimeSeconds()));
		}
		void ObserveRunRelease()
		{
			if (PlayerInput->IsPressed(AttackKey) || Ability->IsActive() || !ResourcesRestored())
			{
				Fail(TEXT("original ended action regained input or resources during real W release observation")); return;
			}
			FGGYGOQualifiedMovementIntent Intent;
			FString Error;
			const auto Result = Movement->QueryQualifiedMovementIntent(Scope, Intent, Error);
			if (Result == EGGYGOQualifiedMovementIntentQueryResult::Unavailable
				|| Result == EGGYGOQualifiedMovementIntentQueryResult::ExecutionFailed)
			{
				Fail(TEXT("original movement dependency failed during real W release: ") + Error); return;
			}
			const bool bReleased = !PlayerInput->IsPressed(EKeys::W)
				&& Result == EGGYGOQualifiedMovementIntentQueryResult::NotHeld
				&& OriginalOrigin->GetQualification(PlayerInput.Get()) == EGGYGOMovementInputOriginQualification::Rearm
				&& !Movement->HasMoveInput();
			if (!bReleased)
			{
				if (bRunReleaseObserved) { Fail(TEXT("original released W request became held or unqualified during recovery observation")); }
				return;
			}
			if (!bRunReleaseObserved)
			{
				bRunReleaseObserved = true;
				bWDown = false;
				RunReleaseUntilWorldSeconds = World->GetTimeSeconds() + 1.0f;
				LogLocomotionSnapshot(TEXT("RunReleaseObserved"));
				Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.RunObservation] Boundary=ReleaseObserved WorldSeconds=%.6f ObserveUntil=%.6f; original PlayerInput released, Scope NotHeld, Source Rearm and CMC no move input."),
					World->GetTimeSeconds(), RunReleaseUntilWorldSeconds));
			}
			if (World->GetTimeSeconds() < RunReleaseUntilWorldSeconds) { return; }
			LogLocomotionSnapshot(TEXT("RunReleaseObservationCompleted"));
			Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.RunObservation] Boundary=Completed WorldSeconds=%.6f; original release facts retained for 1 game second, owned PIE teardown follows."), World->GetTimeSeconds()));
			BeginEnding();
		}
		void RestartAfterMovement()
		{
			if (Ability->IsActive() || !ResourcesRestored()) { Fail(TEXT("ended attack regained resources before the next real press")); return; }
			FGGYGOQualifiedMovementIntent Intent;
			FString Error;
			const auto Result = Movement->QueryQualifiedMovementIntent(Scope, Intent, Error);
			if (PlayerInput->IsPressed(EKeys::W) || Result != EGGYGOQualifiedMovementIntentQueryResult::NotHeld) { return; }
			if (OriginalOrigin->GetQualification(PlayerInput.Get()) != EGGYGOMovementInputOriginQualification::Rearm
				|| PlayerInput->IsPressed(AttackKey))
			{
				Fail(TEXT("real move release did not preserve the original rearmed source and released attack key")); return;
			}
			Stage = EStage::Main;
			Deadline = FPlatformTime::Seconds() + 3.0;
			bAttackDown = true;
			SendNativeKey(AttackKey, true);
		}
		void ObserveFullNaturalEnd()
		{
			if (EndCount == 0 && (FGGYGOPlayerComboLifecycleFixture::GetProductionMontageTask(Ability.Get()) != MontageTask.Get()
				|| Ability->GetCurrentComboStep() != ExpectedStep
				|| FGGYGOPlayerComboLifecycleFixture::ReadProductionLastInputRequest(Ability.Get()) != NaturalInputRequestId))
			{
				LogNativeInputBoundary(TEXT("NaturalOriginalReplaced"));
				Fail(TEXT("无新增攻击输入的自然观察出现新请求／新 Step／替代 Task，不能作为原 Task 自然完成等待。")); return;
			}
			if (EndCount == 0 || CompletedCount == 0)
			{
				if (EndCount == 0 && MontageTask.Get() && MontageTask->GetState() == EGameplayTaskState::Finished)
				{
					if (Ability->IsAttackContinuationOpen() || Ability->IsMovementInterruptionOpen())
					{
						Fail(TEXT("original native Task finished but its pending CMC owner retained interruption permission")); return;
					}
					if (!bObservedCompletedWait)
					{
						FString ReplayError;
						if (!FGGYGOPlayerComboLifecycleFixture::ReplayCompletedProductionPointForTest(
							Ability.Get(), MontageTask.Get(), OriginalAuthorPoint, Movement.Get(), ReplayError))
						{
							Fail(TEXT("late original point reopened permission or retired the pending original CMC owner: ") + ReplayError); return;
						}
						if (!OriginalMovementPoint.NotifyName.IsNone()
							&& !FGGYGOPlayerComboLifecycleFixture::ReplayCompletedProductionPointForTest(
								Ability.Get(), MontageTask.Get(), OriginalMovementPoint, Movement.Get(), ReplayError))
						{
							Fail(TEXT("late original movement point changed the completed resource or reopened permission: ") + ReplayError); return;
						}
						bObservedCompletedWait = true;
						Test.AddInfo(TEXT("[Combat.PlayerCombo.InterruptionProductionSmoke] NativeTaskFinishedBeforeOwnerEnd: original resource retained, permission closed and negative late-point replay rejected; awaiting native CMC completion."));
					}
				}
				FGGYGOMontageSectionSnapshot Snapshot;
				if (EndCount == 0 && MontageTask.Get() && MontageTask->TryGetOriginalSectionSnapshot(Snapshot))
				{
					if (Snapshot.Montage != Montage.Get() || Snapshot.MontageInstanceId != InstanceId
						|| Ability->GetCurrentComboStep() != ExpectedStep)
					{
						Fail(TEXT("without a new request the original natural step/instance changed")); return;
					}
					if (Snapshot.SectionName == EndSection && Snapshot.PositionSeconds >= EndStart
						&& Snapshot.PositionSeconds < EndEnd)
					{
						if (!bSawNaturalEnd && !Movement->HasActiveActionMotion())
						{
							Fail(TEXT("natural End lost its original continuous CMC trajectory at entry")); return;
						}
						bSawNaturalEnd = true;
						NaturalLastPosition = Snapshot.PositionSeconds;
					}
				}
				if (EndCount > 1 || CompletedCount > 1 || (EndCount == 0 && !Ability->IsActive())
					|| (EndCount == 0 && !Ability->CaptureCurrentActivation().HasSameActivation(OriginalActivation)))
				{
					Fail(TEXT("natural original activation vanished or duplicated its End/Completed"));
				}
				return; // Task Completed may precede the exact CMC final-consumption callback.
			}
			if (EndCount != 1 || CompletedCount != 1 || EndData.bWasCancelled || !Completion.HasCompletion()
				|| Completion.GetReason() != EGGYGOAbilityTerminationReason::None
				|| Completion.GetOriginal().GetRequestKind() != EGGYGOAbilityTerminationRequestKind::End
				|| !bSawNaturalEnd || !bResourcesRestoredAtEnd || !ResourcesRestored() || Ability->IsActive()
				|| !FMath::IsFinite(NaturalEarliestEndWorldSeconds)
				|| EndObservedWorldSeconds + FMath::Max(0.05f, World->GetDeltaSeconds()) < NaturalEarliestEndWorldSeconds
				|| PlayerInput->IsPressed(AttackKey) || PlayerInput->IsPressed(EKeys::W)
				|| (EndAttackProbe == EComboEndAttackProbe::Restart && (!bRestartCaptured || !bMainAttackRejected)))
			{
				Fail(TEXT("no-request natural End failed original completion/cleanup or restarted automatically")); return;
			}
			Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.EndProductionSmoke] NaturalEnd Step=%d Instance=%d EndEnd=%.6f LastObservedPosition=%.6f End=1 Completed=1 Cancelled=0 NoAutomaticRestart=1 RestartObserved=%d."),
				ExpectedStep, InstanceId, EndEnd, NaturalLastPosition, bRestartCaptured));
			Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.InterruptionProductionSmoke] NativeTaskBeforeMotionWaitObserved=%d; native frame order is observed, never forced."), bObservedCompletedWait));
			NaturalIdleUntilWorldSeconds = World->GetTimeSeconds() + 0.25f;
			Stage = EStage::NaturalIdle;
			Deadline = FPlatformTime::Seconds() + 3.0;
		}
		void ObserveNaturalIdle()
		{
			if (Ability->IsActive() || EndCount != 1 || CompletedCount != 1 || !ResourcesRestored()
				|| ASC->HasMatchingGameplayTag(GGYGOGameplayTags::State_Attacking)
				|| PlayerInput->IsPressed(AttackKey) || PlayerInput->IsPressed(EKeys::W))
			{
				Fail(TEXT("without a new request the naturally ended attack restarted or reacquired resources")); return;
			}
			if (World->GetTimeSeconds() >= NaturalIdleUntilWorldSeconds
				&& (FXProbe == EComboFXProbe::None || bFXVerified)) { BeginEnding(); }
		}
		void LogLocomotionSnapshot(const TCHAR* Boundary)
		{
			const UGGYGOCharacterMovementComponent* OriginalMovement = Movement.Get();
			const ACharacter* OriginalCharacter = Character.Get();
			const UGGYGOAbilitySystemComponent* OriginalASC = ASC.Get();
			if (!OriginalMovement || !OriginalCharacter || !OriginalASC)
			{
				Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.EndProductionSmoke] %s Case=%s Available=0 Movement=%s Character=%s ASC=%s; original locomotion objects unavailable."),
					Boundary, bHeldDuringMain ? TEXT("HeldDuringMain") : TEXT("NewPressDuringEnd"),
					*GetPathNameSafe(OriginalMovement), *GetPathNameSafe(OriginalCharacter), *GetPathNameSafe(OriginalASC)));
				return;
			}
			const UGGYGOMovementSet* Set = OriginalMovement->GetMovementSet();
			const FString Policy = Set ? FString::Printf(TEXT("MovementSet=%s UseCurve=%d RootMotionScale=%.6f WalkToRunHoldSeconds=%.6f"),
				*Set->GetPathName(), Set->bUseCurveDrivenSpeed, Set->RootMotionScale, Set->WalkToRunHoldSeconds)
				: TEXT("MovementSet=Unavailable");
			const FGGYGOLocomotionCurveSample Curve = OriginalMovement->GetCurveMotion();
			const EGGYGOGait Gait = OriginalMovement->GetResolvedGait();
			Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.EndProductionSmoke] %s Case=%s Available=1 OriginalEndRemaining=%.6f WalkObserved=%d RunObserved=%d Gait=%u WalkRunAlpha=%.6f MotionType=%u Mode=%u Acceleration=%s HasMoveInput=%d Speed=%.3f Displacement=%.3f CurveSource=%d CurveSampleSpeed=%.6f CurveDriving=%d %s WalkTagCount=%d RunStartTagCount=%d RunLoopTagCount=%d; normal policy only, native teardown pending."),
				Boundary, bHeldDuringMain ? TEXT("HeldDuringMain") : TEXT("NewPressDuringEnd"), EndEnd - EndPosition, bSawWalk,
				Gait == EGGYGOGait::Run, static_cast<uint32>(Gait), OriginalMovement->GetWalkRunBlendAlpha(),
				static_cast<uint32>(OriginalMovement->GetLocomotionMotionType()), static_cast<uint32>(OriginalMovement->MovementMode),
				*OriginalMovement->GetCurrentAcceleration().ToString(), OriginalMovement->HasMoveInput(), OriginalMovement->GetHorizontalSpeed(),
				FVector::Dist2D(OriginalCharacter->GetActorLocation(), PositionAtEnd), Curve.bHasCurveSource, Curve.Speed,
				OriginalMovement->IsCurveDrivingSpeed(), *Policy, OriginalASC->GetTagCount(GGYGOGameplayTags::State_WalkLoop),
				OriginalASC->GetTagCount(GGYGOGameplayTags::State_RunStart), OriginalASC->GetTagCount(GGYGOGameplayTags::State_RunLoop)));
		}
		const TCHAR* PoseCaseName() const
		{
			return PoseProbe == EComboPoseFailureProbe::StartupMissingDeclaration
				? TEXT("StartupMissingDeclaration") : TEXT("RuntimeInvalidatedUncancelable");
		}
		bool PreparePoseProbe()
		{
			FGGYGOComboStep Step;
			UGGYGOMontageGuardAnimInstance* Guard = Cast<UGGYGOMontageGuardAnimInstance>(AnimInstance.Get());
			if (!Guard || !FGGYGOPlayerComboLifecycleFixture::ReadProductionFirstStep(Ability.Get(), Step)
				|| !Step.Montage || Step.Montage->SlotAnimTracks.Num() != 1
				|| Guard->RequiredPoseCorrectionSlot.IsNone() || Guard->RequiredPoseCorrectionSlot != Step.MotionSlotName
				|| Step.Montage->SlotAnimTracks[0].SlotName != Step.MotionSlotName)
			{
				Fail(TEXT("requires the actual migrated production ABP and its declared unique original correction slot")); return false;
			}
			FString Diagnostic;
			const EGGYGOActionPoseContractAcquireResult Acquired = Guard->AcquireActionPoseContract(Step.Montage, PoseOriginalTicket, Diagnostic);
			if (!IsProductionChainCurrent() || AnimInstance.Get() != Guard
				|| Acquired != EGGYGOActionPoseContractAcquireResult::RequiredReady || !PoseOriginalTicket.IsValid()
				|| PoseOriginalTicket.GetSlotName() != Step.MotionSlotName)
			{
				Fail(FString::Printf(TEXT("actual baseline RequiredReady is unavailable (result=%u): %s"), static_cast<uint32>(Acquired), *Diagnostic)); return false;
			}
			PoseGuard = Guard;
			PoseMeshAsset = Mesh->GetSkeletalMeshAsset();
			SavedPoseSlot = Guard->RequiredPoseCorrectionSlot;
			Montage = Step.Montage;
			MainSection = Step.MainSection; EndSection = Step.EndSection;
			FAutomationTestExecutionInfo Info;
			Test.GetExecutionInfo(Info);
			PoseDiagnosticStart = Info.GetEntries().Num();
			PoseActivatedHandle = ASC->AbilityActivatedCallbacks.AddLambda([this](UGameplayAbility* Activated)
			{
				if (Activated != Ability.Get()) { return; }
				const FGGYGOAbilityActivationHandle Captured = Ability->CaptureCurrentActivation();
				if (!Captured.HasActivation()) { Fail(TEXT("native activated callback did not expose its issued original identity")); return; }
				if (!OriginalActivation.HasActivation()) { OriginalActivation = Captured; }
				else if (Stage == EStage::PoseSuccessorMain && !Captured.HasSameActivation(OriginalActivation)) { PoseSuccessorActivation = Captured; }
				else { Fail(TEXT("unexpected activation replaced the pose probe's original or successor")); }
			});
			if (!PoseActivatedHandle.IsValid()) { Fail(TEXT("native activation observation installation failed")); return false; }
			if (PoseProbe == EComboPoseFailureProbe::RuntimeInvalidated)
			{
				PosePolicyProperty = FindFProperty<FEnumProperty>(UGGYGOGameplayAbility::StaticClass(), TEXT("SelfPolicy"));
				if (!PosePolicyProperty || PosePolicyProperty->GetEnum() != StaticEnum<EGGYGOAbilitySelfPolicy>())
				{
					Fail(TEXT("existing reflected SelfPolicy enum is unavailable; cannot establish the legal uncancelable mode")); return false;
				}
				SavedPoseSelfPolicy = Ability->GetSelfPolicy();
				bSavedPoseCanCancel = Ability->CanBeCanceled();
				bPosePolicyModified = true;
				PosePolicyProperty->GetUnderlyingProperty()->SetIntPropertyValue(PosePolicyProperty->ContainerPtrToValuePtr<void>(Ability.Get()),
					static_cast<int64>(EGGYGOAbilitySelfPolicy::Exclusive));
				if (Ability->GetSelfPolicy() != EGGYGOAbilitySelfPolicy::Exclusive)
				{
					Fail(TEXT("test instance did not accept its existing Exclusive configuration")); return false;
				}
			}
			Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.PoseProductionSmoke] Case=%s BaselineRequiredReady=1 Anim=%s Mesh=%s Montage=%s Slot=%s; actual compiled production provider, no fabricated ticket or asset changes."),
				PoseCaseName(), *Guard->GetPathName(), *Mesh->GetPathName(), *Step.Montage->GetPathName(), *SavedPoseSlot.ToString()));
			if (PoseProbe == EComboPoseFailureProbe::StartupMissingDeclaration)
			{
				bPoseSlotModified = true;
				Guard->RequiredPoseCorrectionSlot = NAME_None; // GT-only role declaration; the actual producer stays intact.
				FGGYGOActionPoseContractTicket RejectedTicket;
				const EGGYGOActionPoseContractAcquireResult Rejected = Guard->AcquireActionPoseContract(Step.Montage, RejectedTicket, Diagnostic);
				if (!IsProductionChainCurrent() || Rejected != EGGYGOActionPoseContractAcquireResult::Rejected || RejectedTicket.IsValid())
				{
					Fail(TEXT("missing role declaration did not reject the real compiled producer before playback: ") + Diagnostic); return false;
				}
			}
			return true;
		}
		void BeginPoseRuntimeFault()
		{
			PoseOriginalTaskPath = GetPathNameSafe(MontageTask.Get());
			FailedPoseTask.Reset(MontageTask.Get());
			static_cast<UGameplayAbility*>(Ability.Get())->SetCanBeCanceled(false);
			bPoseUncancelableBeforeFault = !Ability->CanBeCanceled();
			const FGGYGOAbilityTerminationResult Cancel = Ability->RequestAbilityCancel(OriginalActivation, true);
			if (!bPoseUncancelableBeforeFault || Cancel.Outcome != EGGYGOAbilityTerminationOutcome::Rejected
				|| Cancel.Reason != EGGYGOAbilityTerminationReason::NotCancelable || !Ability->IsActive()
				|| !Ability->CaptureCurrentActivation().HasSameActivation(OriginalActivation) || !Movement->HasActiveActionMotion()
				|| !IsProductionChainCurrent() || PoseGuard.Get() != AnimInstance.Get())
			{
				Fail(TEXT("legal Exclusive original did not reject user cancellation while retaining its exact Main resources")); return;
			}
			Stage = EStage::PoseWaitFailure;
			Deadline = FPlatformTime::Seconds() + 10.0;
			bPoseSlotModified = true;
			PoseGuard->RequiredPoseCorrectionSlot = NAME_None;
			FString Diagnostic;
			const EGGYGOActionPoseContractPollResult Polled = PoseGuard->PollActionPoseContract(PoseOriginalTicket, Diagnostic);
			if (Polled != EGGYGOActionPoseContractPollResult::Invalidated)
			{
				Fail(TEXT("original real ticket did not invalidate after its own role declaration changed: ") + Diagnostic); return;
			}
			Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.PoseProductionSmoke] Case=%s UserCancelRejected=1 OriginalInstance=%d TicketInvalidated=1; waiting for original UE Task Tick, no manual Tick or failure injection."), PoseCaseName(), InstanceId));
		}
		bool PoseResourcesRestored() const
		{
			if (PoseProbe == EComboPoseFailureProbe::RuntimeInvalidated)
			{
				return ResourcesRestored() && !MontageTask->OnFailed.IsBound();
			}
			return FGGYGOPlayerComboLifecycleFixture::ProductionResourcesReleased(Ability.Get(), World.Get())
				&& Movement.IsValid() && !Movement->HasActiveActionMotion() && !Movement->IsMovementBlockedByTag()
				&& Trace.IsValid() && !Trace->IsTracing() && Mesh.IsValid()
				&& Mesh->VisibilityBasedAnimTickOption == SavedTick && Mesh->bEnableUpdateRateOptimizations == bSavedURO
				&& HasMeshPrerequisite() == bSavedPrerequisite && Character.IsValid()
				&& Character->GetAnimRootMotionTranslationScale() == SavedRootScale;
		}
		void ObservePoseAbilityEnd(const FAbilityEndedData& Data)
		{
			if (Data.AbilitySpecHandle != SpecHandle) { Fail(TEXT("pose native End changed the original Spec")); return; }
			if (Stage == EStage::PoseSuccessorMain || Stage == EStage::PoseSuccessorEnd)
			{
				++PoseSuccessorEndCount;
				bPoseSuccessorCancelled = Data.bWasCancelled;
				bPoseSuccessorResourcesAtEnd = ResourcesRestored();
				return;
			}
			++EndCount; EndData = Data;
			bResourcesRestoredAtEnd = PoseResourcesRestored();
			if (PoseProbe == EComboPoseFailureProbe::RuntimeInvalidated)
			{
				FAnimMontageInstance* Instance = AnimInstance->GetMontageInstanceForID(InstanceId);
				bEndSnapshotAvailable = Instance && Instance->Montage == Montage.Get();
				if (bEndSnapshotAvailable)
				{
					EndPosition = Instance->GetPosition(); EndedSection = Instance->GetCurrentSection();
					bStoppedAtEnd = Instance->IsStopped() || !Instance->IsActive();
				}
			}
			else
			{
				FAnimMontageInstance* Active = AnimInstance->GetActiveMontageInstance();
				bPoseStartupNoPlayback = (!Active || Active->Montage != Montage.Get()) && ASC->GetCurrentMontage() != Montage.Get();
			}
			Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.PoseProductionSmoke] Case=%s NativeEnd Cancelled=%d Instance=%d Snapshot=%d Stopped=%d Position=%.6f StartupNoPlayback=%d ResourcesRestored=%d."),
				PoseCaseName(), Data.bWasCancelled, InstanceId, bEndSnapshotAvailable, bStoppedAtEnd, EndPosition, bPoseStartupNoPlayback, bResourcesRestoredAtEnd));
		}
		bool HasPoseFailureDiagnostics() const
		{
			FAutomationTestExecutionInfo Info;
			Test.GetExecutionInfo(Info); // Read the unfiltered events; never remove/expect/suppress a production Error.
			const FString Owner = FString::Printf(TEXT("Ability='%s'"), *GetPathNameSafe(Ability.Get()));
			const FString Asset = FString::Printf(TEXT("Montage='%s'"), *GetPathNameSafe(Montage.Get()));
			const FString Slot = FString::Printf(TEXT("Slot='%s'"), *SavedPoseSlot.ToString());
			const FString StageText = FString::Printf(TEXT(" Stage=%d "), PoseProbe == EComboPoseFailureProbe::RuntimeInvalidated ? 1 : 0);
			const FString InstanceSpace = FString::Printf(TEXT(" Instance=%d "), InstanceId);
			const FString InstanceSemi = FString::Printf(TEXT(" Instance=%d;"), InstanceId);
			int32 TaskFacts = 0, GAFacts = 0, EndResults = 0;
			for (int32 Index = PoseDiagnosticStart; Index < Info.GetEntries().Num(); ++Index)
			{
				const FString& Message = Info.GetEntries()[Index].Event.Message;
				if (!Message.Contains(Owner) || !Message.Contains(Asset) || !Message.Contains(Slot)
					|| !Message.Contains(StageText) || (!Message.Contains(InstanceSpace) && !Message.Contains(InstanceSemi))) { continue; }
				if (!PoseOriginalTaskPath.IsEmpty() && !Message.Contains(FString::Printf(TEXT("Task='%s'"), *PoseOriginalTaskPath))) { continue; }
				if (Message.Contains(TEXT("[AbilitySystem.MontageTask.Pose]"))
					&& (PoseProbe != EComboPoseFailureProbe::StartupMissingDeclaration || Message.Contains(TEXT("Call=0 ")))) { ++TaskFacts; }
				if (Message.Contains(TEXT("[Combat.PlayerCombo.PoseFailure] Boundary=RequiredDependencyFailed"))) { ++GAFacts; }
				if (Message.Contains(TEXT("[Combat.PlayerCombo.PoseFailure] Boundary=AbilityEndResult"))
					&& (Message.Contains(TEXT(" Outcome=1 ")) || Message.Contains(TEXT(" Outcome=2 "))
						|| Message.Contains(TEXT(" Outcome=3 ")) || Message.Contains(TEXT(" Outcome=4 ")))) { ++EndResults; }
			}
			return TaskFacts == 1 && GAFacts == 1 && EndResults == 1;
		}
		void RestorePoseProbeConfiguration()
		{
			if (bPoseSlotModified)
			{
				if (!PoseGuard.IsValid() || PoseGuard.Get() != AnimInstance.Get() || !Mesh.IsValid()
					|| Mesh->GetAnimInstance() != PoseGuard.Get() || Mesh->GetSkeletalMeshAsset() != PoseMeshAsset.Get())
				{
					Fail(TEXT("original provider/model changed; cannot restore the probe declaration onto another instance"));
				}
				else { PoseGuard->RequiredPoseCorrectionSlot = SavedPoseSlot; }
				bPoseSlotModified = false;
			}
			if (bPosePolicyModified)
			{
				if (!Ability.IsValid() || !PosePolicyProperty) { Fail(TEXT("original test instance policy cannot be restored")); }
				else if (Ability->IsActive() && !Ability->CaptureCurrentActivation().HasSameActivation(OriginalActivation))
				{
					Fail(TEXT("another activation owns the same GA instance; cannot restore old probe policy onto it"));
				}
				else
				{
					static_cast<UGameplayAbility*>(Ability.Get())->SetCanBeCanceled(bSavedPoseCanCancel);
					PosePolicyProperty->GetUnderlyingProperty()->SetIntPropertyValue(PosePolicyProperty->ContainerPtrToValuePtr<void>(Ability.Get()),
						static_cast<int64>(SavedPoseSelfPolicy));
					if (Ability->GetSelfPolicy() != SavedPoseSelfPolicy || Ability->CanBeCanceled() != bSavedPoseCanCancel)
					{
						Fail(TEXT("original policy/cancellation permission restore did not match its saved values"));
					}
				}
				bPosePolicyModified = false;
			}
		}
		void VerifyPoseFailure()
		{
			if (EndCount == 0 || CompletedCount == 0) { return; }
			const bool bRuntime = PoseProbe == EComboPoseFailureProbe::RuntimeInvalidated;
			if (EndCount != 1 || CompletedCount != 1 || !OriginalActivation.HasActivation() || !EndData.bWasCancelled
				|| !Completion.HasCompletion() || Completion.GetReason() != EGGYGOAbilityTerminationReason::None
				|| Completion.GetOriginal().GetRequestKind() != EGGYGOAbilityTerminationRequestKind::End
				|| Ability->IsActive() || !bResourcesRestoredAtEnd || !PoseResourcesRestored() || !HasPoseFailureDiagnostics()
				|| (bRuntime ? (!bPoseUncancelableBeforeFault || !bEndSnapshotAvailable || !bStoppedAtEnd
					|| EndedSection != MainSection || !FMath::IsFinite(EndPosition) || EndPosition < 0.0f || EndPosition >= EndStart)
					: (!bPoseStartupNoPlayback || InstanceId != INDEX_NONE)))
			{
				Fail(TEXT("exact typed pose failure/mandatory original End/Completed/cleanup proof failed; production diagnostics remain visible")); return;
			}
			RestorePoseProbeConfiguration();
			if (bFailed || !IsProductionChainCurrent()) { return; }
			Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.PoseProductionSmoke] Case=%s OriginalFailureAssertions=PASS; source diagnostics retained, starting a genuine subsequent Attack."), PoseCaseName()));
			if (bAttackDown) { SendNativeKey(AttackKey, false); bAttackDown = false; }
			Stage = EStage::PoseSuccessorPress;
			Deadline = FPlatformTime::Seconds() + 10.0;
		}
		void StartPoseSuccessor()
		{
			// A real UE frame commits our own key-up before a separate real key-down.
			if (Ability->IsActive() || PlayerInput->IsPressed(AttackKey) || !PoseResourcesRestored())
			{
				Fail(TEXT("own original release/cleanup was not committed before successor native input")); return;
			}
			Stage = EStage::PoseSuccessorMain;
			bAttackDown = true; SendNativeKey(AttackKey, true);
		}
		void ObservePoseSuccessorMain()
		{
			if (PoseSuccessorEndCount != 0) { Fail(TEXT("successor ended before its genuine Main observation")); return; }
			if (!Ability->IsActive()) { return; }
			UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = FGGYGOPlayerComboLifecycleFixture::GetProductionMontageTask(Ability.Get());
			FGGYGOMontageSectionSnapshot Snapshot;
			if (!Task || !Task->TryGetOriginalSectionSnapshot(Snapshot)) { return; }
			FAnimMontageInstance* OldInstance = InstanceId != INDEX_NONE ? AnimInstance->GetMontageInstanceForID(InstanceId) : nullptr;
			if (!PoseSuccessorActivation.HasActivation() || PoseSuccessorActivation.HasSameActivation(OriginalActivation)
				|| !Ability->CaptureCurrentActivation().HasSameActivation(PoseSuccessorActivation)
				|| Snapshot.Montage != Montage.Get() || Snapshot.SectionName != MainSection || Snapshot.MontageInstanceId == INDEX_NONE
				|| Snapshot.MontageInstanceId == InstanceId || Task == FailedPoseTask.Get() || !Movement->HasActiveActionMotion()
				|| (OldInstance && !OldInstance->IsStopped() && OldInstance->IsActive())
				|| !HasPoseFailureDiagnostics() || (FailedPoseTask.Get() && FailedPoseTask->GetState() != EGameplayTaskState::Finished))
			{
				Fail(TEXT("old failure affected successor identity/Main/CMC resources or replayed its failure envelope")); return;
			}
			MontageTask.Reset(Task);
			InputTask.Reset(FGGYGOPlayerComboLifecycleFixture::GetProductionInputTask(Ability.Get()));
			if (!InputTask.Get()) { Fail(TEXT("successor original input Task is unavailable")); return; }
			PoseSuccessorInstanceId = Snapshot.MontageInstanceId;
			const int32 EndIndex = Montage->GetSectionIndex(EndSection);
			if (EndIndex == INDEX_NONE) { Fail(TEXT("successor configured End is unavailable")); return; }
			Montage->GetSectionStartAndEndTime(EndIndex, EndStart, EndEnd);
			if (!FMath::IsFinite(EndStart) || !FMath::IsFinite(EndEnd) || EndEnd <= EndStart)
			{
				Fail(TEXT("successor original End range is invalid")); return;
			}
			SendNativeKey(AttackKey, false); bAttackDown = false;
			Stage = EStage::PoseSuccessorEnd;
			Deadline = FPlatformTime::Seconds() + 10.0;
		}
		void ObservePoseSuccessorEnd()
		{
			if (PoseSuccessorEndCount == 0 || PoseSuccessorCompletedCount == 0)
			{
				FGGYGOMontageSectionSnapshot Snapshot;
				if (PoseSuccessorEndCount == 0 && MontageTask.Get()
					&& MontageTask->TryGetOriginalSectionSnapshot(Snapshot))
				{
					if (Snapshot.Montage != Montage.Get() || Snapshot.MontageInstanceId != PoseSuccessorInstanceId)
					{
						Fail(TEXT("successor natural End observation changed its original playback identity")); return;
					}
					if (Snapshot.SectionName == EndSection && Snapshot.PositionSeconds >= EndStart
						&& Snapshot.PositionSeconds < EndEnd - KINDA_SMALL_NUMBER)
					{
						if (!bPoseSuccessorSawContinuousEnd)
						{
							if (!Movement->HasActiveActionMotion())
							{
								Fail(TEXT("successor lost the original CMC trajectory at natural End entry")); return;
							}
							Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.PoseProductionSmoke] NaturalEndContinuous Montage=%s Instance=%d Position=%.6f EndEnd=%.6f ActionActive=1."),
								*GetPathNameSafe(Montage.Get()), PoseSuccessorInstanceId, Snapshot.PositionSeconds, EndEnd));
						}
						bPoseSuccessorSawContinuousEnd = true;
					}
				}
				if (PoseSuccessorEndCount > 1 || PoseSuccessorCompletedCount > 1
					|| (PoseSuccessorEndCount == 0 && PoseSuccessorCompletedCount == 0
						&& (!Ability->IsActive() || !Ability->CaptureCurrentActivation().HasSameActivation(PoseSuccessorActivation))))
				{
					Fail(TEXT("successor activation vanished before its natural End/Completed"));
				}
				// Native End may precede controlled Completed; retain the existing bounded deadline.
				return;
			}
			if (PoseSuccessorEndCount != 1 || PoseSuccessorCompletedCount != 1 || bPoseSuccessorCancelled
				|| !PoseSuccessorCompletion.HasCompletion() || PoseSuccessorCompletion.GetReason() != EGGYGOAbilityTerminationReason::None
				|| PoseSuccessorCompletion.GetOriginal().GetRequestKind() != EGGYGOAbilityTerminationRequestKind::End
				|| !bPoseSuccessorSawContinuousEnd || !bPoseSuccessorResourcesAtEnd || !ResourcesRestored() || Ability->IsActive() || !HasPoseFailureDiagnostics()
				|| (FailedPoseTask.Get() && FailedPoseTask->GetState() != EGameplayTaskState::Finished))
			{
				Fail(TEXT("successor did not naturally complete with exact resource cleanup independent of the old failure")); return;
			}
			bPoseBehaviorVerified = true;
			BeginEnding();
		}
		void ReleaseGameplayObservations()
		{
			ReleaseFXInputObservation();
			ReleaseFXFrameObservation();
			if (NativeWindow.IsValid()) { NativeWindow->GetOnWindowDeactivatedEvent().Remove(WindowDeactivatedHandle); }
			WindowDeactivatedHandle.Reset();
			if (PoseProbe != EComboPoseFailureProbe::None)
			{
				RestorePoseProbeConfiguration();
				if (ASC.IsValid()) { ASC->AbilityActivatedCallbacks.Remove(PoseActivatedHandle); }
				PoseActivatedHandle.Reset();
			}
			if (OwnerSyncObserver.GetObserverSerial() != 0)
			{
				FString Error;
				if (!Movement.IsValid() || !Movement->UnsubscribeMovementOwnerSync(OwnerSyncObserver, TEXT("EndProductionSmokeCleanup"), Error))
				{
					Test.AddInfo(TEXT("[Combat.PlayerCombo.EndProductionSmoke] Original owner observer cleanup unavailable: ") + Error);
					Fail(TEXT("original owner observer cleanup rejected; original native lifetime retains teardown responsibility"));
				}
				OwnerSyncObserver = {};
			}
			if (ASC.IsValid())
			{
				ASC->OnAbilityEnded.Remove(EndHandle);
				ASC->OnAbilityTerminationCompleted().Remove(CompletedHandle);
				if (FXEventHandle.IsValid()) { ASC->RemoveGameplayEventTagContainerDelegate(FXEventTags, FXEventHandle); }
			}
			EndHandle.Reset(); CompletedHandle.Reset();
			FXEventHandle.Reset(); FXEventTags.Reset(); FXTask.Reset(); FXCache.Reset(); FXComponent.Reset();
			ReplacedTask.Reset(); MontageTask.Reset(); InputTask.Reset(); // Observation roots never survive this finite smoke.
			if (!FSlateApplication::IsInitialized()) { return; }
			FSlateApplication& Slate = FSlateApplication::Get();
			const TSharedPtr<SWidget> CurrentFocus = Slate.GetKeyboardFocusedWidget();
			if (bAttackDown || bWDown)
			{
				if (IsOwnedWorldCurrent() && Viewport.IsValid() && Viewport->GetGameViewportWidget() == ViewportWidget
					&& ViewportWidget.IsValid())
				{
					if (Slate.GetKeyboardFocusedWidget() != ViewportWidget)
					{
						Slate.SetKeyboardFocus(ViewportWidget, EFocusCause::SetDirectly);
					}
					if (Slate.GetKeyboardFocusedWidget() == ViewportWidget)
					{
						// Only return keys issued by this test to its still-original viewport.
						if (bAttackDown) { SendNativeKey(AttackKey, false); bAttackDown = false; }
						if (bWDown) { SendNativeKey(EKeys::W, false); bWDown = false; }
					}
					else { Fail(TEXT("owned key-up route did not acquire actual original viewport focus")); }
				}
				else { Fail(TEXT("owned key-up route unavailable; original PIE native teardown must release its input")); }
			}
			FocusBeforeTeardown = CurrentFocus;
			// 活跃 PIE 的焦点归还会触发真实 InputFlushed；由原生 EndPlay 先退役输入和 CMC。
		}
		void RestoreFocusAfterPIETeardown()
		{
			if (OwnsPIESession() || CountPIEWorlds() != 0 || !FSlateApplication::IsInitialized()) { return; }
			FSlateApplication& Slate = FSlateApplication::Get();
			const TSharedPtr<SWidget> SavedFocus = FocusBeforeTeardown.Pin();
			if (SavedFocus.IsValid() && SavedFocus != ViewportWidget) { Slate.SetKeyboardFocus(SavedFocus, EFocusCause::SetDirectly); }
			else if (PreviousFocus.IsValid()) { Slate.SetKeyboardFocus(PreviousFocus.Pin(), EFocusCause::SetDirectly); }
			FocusBeforeTeardown.Reset(); PreviousFocus.Reset();
		}
		void ReleaseMainPointProbeAfterPIETeardown()
		{
			if (!MainPointProbe.Get() || OwnsPIESession() || CountPIEWorlds() != 0) { return; }
			FGGYGOPlayerComboLifecycleFixture::RestoreProductionMainPointProbe(Ability.Get(), MainPointProbeIndex,
				OriginalPointProbeStep, MainPointProbe.Get());
			MainPointProbe.Reset();
		}
		void CloseOwnedPIE(bool bSynchronous)
		{
			if (!Editor.IsValid()) { return; }
			if (OwnsPIESession())
			{
				if (CountPIEWorlds() > 1) { Fail(TEXT("foreign PIE context prevents owned session teardown")); return; }
				for (const FWorldContext& Context : Editor->GetWorldContexts())
				{
					if (Context.WorldType == EWorldType::PIE && !ContextHandle.IsNone() && Context.ContextHandle != ContextHandle)
					{
						Fail(TEXT("replacement PIE context prevents original session teardown")); return;
					}
				}
				if (bSynchronous || !Editor->PlayWorld) { Editor->EndPlayMap(); }
				else { Editor->RequestEndPlayMap(); }
			}
			else
			{
				const TOptional<FRequestPlaySessionParams> Queued = Editor->GetPlaySessionRequest();
				if (Queued.IsSet() && RequestSettings.IsValid() && Queued->EditorPlaySettings == RequestSettings.Get())
				{
					Editor->CancelRequestPlaySession();
				}
			}
		}
		void BeginEnding()
		{
			Stage = EStage::Ending;
			ReleaseGameplayObservations();
			Deadline = FPlatformTime::Seconds() + 10.0;
			CloseOwnedPIE(false);
		}
		bool UpdateEnding()
		{
			if (OwnsPIESession() || (Editor.IsValid() && CountPIEWorlds() != 0))
			{
				if (FPlatformTime::Seconds() < Deadline) { return false; }
				Fail(TEXT("owned native PIE teardown timeout")); CloseOwnedPIE(true);
			}
			const bool bReleased = Test.TestTrue(TEXT("production smoke released its original PIE session/context"),
				!OwnsPIESession() && CountPIEWorlds() == 0);
			if (bReleased) { ReleaseMainPointProbeAfterPIETeardown(); RestoreFocusAfterPIETeardown(); }
			const bool bGlobalsRestored = Test.TestTrue(TEXT("native teardown restored outer world/viewport"),
				GWorld == OuterWorld && GEngine && GEngine->GameViewport.Get() == OuterViewport);
			if (!bFailed && bReleased && bGlobalsRestored)
			{
				if (PoseProbe == EComboPoseFailureProbe::None)
				{
					Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.EndProductionSmoke] PASS: Case=%u LastMovementStep=%d; exact original End/step resources, real input chain and native PIE teardown observed."),
						static_cast<uint32>(EndAttackProbe), LastMovementStep));
				}
				else if (bPoseBehaviorVerified)
				{
					Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.PoseProductionSmoke] Case=%s BehaviorAssertions=PASS including original cleanup, natural successor and owned PIE teardown. Production Error events remain unfiltered; this does not override the Automation result."), PoseCaseName()));
				}
			}
			Stage = EStage::Done;
			return true;
		}

		FAutomationTestBase& Test;
		const bool bHeldDuringMain;
		const EComboPoseFailureProbe PoseProbe;
		const int32 LastMovementStep;
		const EComboEndAttackProbe EndAttackProbe;
		const EComboLocomotionObservation LocomotionObservation;
		const EComboFXProbe FXProbe;
		FFXInputObservationTick FXInputObservationTick;
		FFXFrameObservationTick FXFrameObservationTick;
		uint64 FXReadyFrame = MAX_uint64;
		float FXReadyWorldSeconds = 0.0f;
		FDelegateHandle WindowDeactivatedHandle;
		TWeakObjectPtr<UNiagaraSystem> FXSystem;
		TWeakObjectPtr<UNiagaraComponent> FXComponent;
		TStrongObjectPtr<UGGYGOAbilityTask_PlayNiagaraEffect> FXTask;
		TStrongObjectPtr<UNiagaraSimCache> FXCache;
		FGameplayTagContainer FXEventTags;
		FDelegateHandle FXEventHandle;
		TMap<int32, int32> FXBeginCounts, FXEndCounts;
		FGGYGOAbilityActivationHandle FXCancelledActivation;
		int32 FXCancelledInstanceId = INDEX_NONE, FXRejectedOldEvents = 0;
		uint64 FXCancelFrame = MAX_uint64, FXRestartPlaybackFrame = MAX_uint64;
		uint64 FXRestartBeginFrame = MAX_uint64, FXCancelledEndFrame = MAX_uint64;
		float FXWindowBegin = 0.0f, FXWindowEnd = 0.0f;
		bool bFXCaptured = false, bFXParticles = false, bFXDetached = false, bFXVerified = false, bFXRestarted = false;
		bool bFXCancelledWithActiveNotify = false, bFXNativePlaybackOverlap = false;
		int32 TargetStep = 0, ExpectedStep = 0, ReplacedStep = INDEX_NONE, ReplacedInstanceId = INDEX_NONE, MainAttackRequest = 0;
		int32 NaturalInputRequestId = 0;
		bool bRestartSent = false, bRestartCaptured = false, bMainAttackRejected = false;
		bool bCheckingMovementReset = false, bFinalResetAttack = false;
		bool bObservedCompletedWait = false;
		bool bRunReleaseObserved = false;
		float RunHoldUntilWorldSeconds = 0.0f, RunReleaseUntilWorldSeconds = 0.0f;
		float ReplacedEndRemaining = 0.0f, NaturalLastPosition = 0.0f;
		float ReplacementRequestWorldTime = 0.0f, NaturalEarliestEndWorldSeconds = 0.0f;
		float EndObservedWorldSeconds = -1.0f, NaturalIdleUntilWorldSeconds = 0.0f;
		FGGYGOAbilityActivationHandle PreviousActivation;
		TStrongObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> ReplacedTask;
		float AuthorPointPosition = 0.0f;
		FGGYGOMontageNotifyFact OriginalAuthorPoint;
		FGGYGOMontageNotifyFact OriginalMovementPoint;
		int32 MainPointProbeIndex = INDEX_NONE;
		FGGYGOComboStep OriginalPointProbeStep;
		TStrongObjectPtr<UAnimMontage> MainPointProbe;
		EStage Stage = EStage::Start;
		bool bFailed = false, bSawPreBegin = false, bPIEStarted = false;
		bool bStartupCaptured = false, bLoggedOwnerWait = false;
		bool bAttackDown = false, bWDown = false, bSawQualifiedMain = false, bSawNaturalEnd = false, bSawWalk = false;
		bool bEndSnapshotAvailable = false, bStoppedAtEnd = false, bResourcesRestoredAtEnd = false;
		bool bSavedURO = false, bSavedPrerequisite = false;
		double Deadline = 0.0;
		float SavedRootScale = 0.0f, EndStart = 0.0f, EndEnd = 0.0f, EndPosition = 0.0f;
		int32 InstanceId = INDEX_NONE, EndCount = 0, CompletedCount = 0;
		EVisibilityBasedAnimTickOption SavedTick = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
		FString EditorMap;
		FName ContextHandle, MainSection, EndSection, EndedSection;
		FKey AttackKey;
		FVector PositionAtEnd = FVector::ZeroVector;
		FGameplayAbilitySpecHandle SpecHandle;
		FGGYGOAbilityActivationHandle OriginalActivation;
		FGGYGOMovementOwnerSyncScopeId Scope;
		FGGYGOMovementOwnerSyncObserverId OwnerSyncObserver;
		FGGYGOMovementOwnerSyncNotice OwnerNotice;
		FGGYGOQualifiedMovementIntent MainIntent, EndIntent;
		EGGYGOQualifiedMovementIntentQueryResult EndIntentResult = EGGYGOQualifiedMovementIntentQueryResult::Unavailable;
		FAbilityEndedData EndData;
		FGGYGOAbilityTerminationCompletedNotice Completion;
		FGGYGOAbilityActivationHandle PoseSuccessorActivation;
		FGGYGOAbilityTerminationCompletedNotice PoseSuccessorCompletion;
		FGGYGOActionPoseContractTicket PoseOriginalTicket;
		TWeakObjectPtr<UGGYGOMontageGuardAnimInstance> PoseGuard;
		TWeakObjectPtr<USkeletalMesh> PoseMeshAsset;
		TStrongObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> FailedPoseTask;
		FEnumProperty* PosePolicyProperty = nullptr;
		FName SavedPoseSlot;
		FString PoseOriginalTaskPath;
		EGGYGOAbilitySelfPolicy SavedPoseSelfPolicy = EGGYGOAbilitySelfPolicy::Coexist;
		bool bSavedPoseCanCancel = true, bPoseSlotModified = false, bPosePolicyModified = false;
		bool bPoseUncancelableBeforeFault = false, bPoseStartupNoPlayback = false, bPoseBehaviorVerified = false;
		bool bPoseSuccessorCancelled = false, bPoseSuccessorResourcesAtEnd = false, bPoseSuccessorSawContinuousEnd = false;
		int32 PoseDiagnosticStart = 0, PoseSuccessorEndCount = 0, PoseSuccessorCompletedCount = 0;
		int32 PoseSuccessorInstanceId = INDEX_NONE;
		FDelegateHandle PoseActivatedHandle;
		FDelegateHandle PreHandle, PostHandle, EndHandle, CompletedHandle;
		TWeakObjectPtr<UEditorEngine> Editor;
		TWeakObjectPtr<ULevelEditorPlaySettings> RequestSettings;
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<UGameInstance> GameInstance;
		TWeakObjectPtr<UGGYGOLocalPlayer> LocalPlayer;
		TWeakObjectPtr<AGGYGOPlayerController> Controller;
		TWeakObjectPtr<ACharacter> Character;
		TWeakObjectPtr<UGGYGOPlayerInput> PlayerInput;
		TWeakObjectPtr<UGGYGOMovementInputOriginResource> OriginalOrigin;
		TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC;
		TWeakObjectPtr<UGGYGOCharacterMovementComponent> Movement;
		TWeakObjectPtr<UGGYGOPlayerComboAbility> Ability;
		TWeakObjectPtr<USkeletalMeshComponent> Mesh;
		TWeakObjectPtr<UAnimInstance> AnimInstance;
		TWeakObjectPtr<UGGYGOMeleeTraceComponent> Trace;
		TWeakObjectPtr<UGameViewportClient> Viewport;
		TWeakObjectPtr<UAnimMontage> Montage;
		// Keep exact ended objects inspectable until assertions finish, without reactivating their lifecycle.
		TStrongObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> MontageTask;
		TStrongObjectPtr<UGGYGOAbilityTask_WaitComboInput> InputTask;
		TSharedPtr<SViewport> ViewportWidget;
		TSharedPtr<SWindow> NativeWindow;
		TWeakPtr<SWidget> PreviousFocus;
		TWeakPtr<SWidget> FocusBeforeTeardown;
		FSceneViewport* SceneViewport = nullptr;
		UWorld* OuterWorld = nullptr;
		UGameViewportClient* OuterViewport = nullptr;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOPlayerComboEndProductionHeldTest,
	"GGYGO.AbilitySystem.PlayerCombo.EndMovement.ProductionNativeHeld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGGYGOPlayerComboEndProductionHeldTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(FComboEndProductionPIECommand(*this, true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOPlayerComboFXProductionTest,
	"GGYGO.AbilitySystem.PlayerCombo.FX.ProductionNativeNormal01",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGGYGOPlayerComboFXProductionTest::RunTest(const FString& Parameters)
{
	for (EComboFXProbe Probe : { EComboFXProbe::NaturalTail, EComboFXProbe::WindowCancel, EComboFXProbe::EarlyCancelRestart })
	{
		ADD_LATENT_AUTOMATION_COMMAND(FComboEndProductionPIECommand(*this, false, EComboPoseFailureProbe::None, 0,
			EComboEndAttackProbe::None, EComboLocomotionObservation::Immediate, Probe));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOPlayerComboRunObservationTest,
	"GGYGO.AbilitySystem.PlayerCombo.EndMovement.RunObservation.ProductionNativeHeld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGGYGOPlayerComboRunObservationTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(FComboEndProductionPIECommand(*this, true, EComboPoseFailureProbe::None, 0,
		EComboEndAttackProbe::None, EComboLocomotionObservation::RunHoldAndRelease));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOPlayerComboEndProductionNewPressTest,
	"GGYGO.AbilitySystem.PlayerCombo.EndMovement.ProductionNativeNewPress",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGGYGOPlayerComboEndProductionNewPressTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(FComboEndProductionPIECommand(*this, false, EComboPoseFailureProbe::None, 2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOPlayerComboEndAttackProductionTest,
	"GGYGO.AbilitySystem.PlayerCombo.Interruption.ProductionNativeAttackAndNatural",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGGYGOPlayerComboEndAttackProductionTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(FComboEndProductionPIECommand(*this, false, EComboPoseFailureProbe::None, 0, EComboEndAttackProbe::Restart));
	ADD_LATENT_AUTOMATION_COMMAND(FComboEndProductionPIECommand(*this, false, EComboPoseFailureProbe::None, 0, EComboEndAttackProbe::Natural));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOPlayerComboPoseStartupMissingTest,
	"GGYGO.AbilitySystem.PlayerCombo.PoseFailure.ProductionNativeStartupMissing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGGYGOPlayerComboPoseStartupMissingTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(FComboEndProductionPIECommand(*this, false, EComboPoseFailureProbe::StartupMissingDeclaration));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOPlayerComboPoseRuntimeInvalidatedTest,
	"GGYGO.AbilitySystem.PlayerCombo.PoseFailure.ProductionNativeRuntimeInvalidatedUncancelable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGGYGOPlayerComboPoseRuntimeInvalidatedTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(FComboEndProductionPIECommand(*this, false, EComboPoseFailureProbe::RuntimeInvalidated));
	return true;
}
#endif // WITH_EDITOR
#endif
