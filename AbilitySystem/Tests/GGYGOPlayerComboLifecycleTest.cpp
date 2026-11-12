#include "AbilitySystem/Tests/GGYGOPlayerComboLifecycleTestTypes.h"

#include "AbilitySystem/Abilities/GGYGOComboCorrection.h"
#include "AbilitySystem/Abilities/GGYGOMontageActionResources.h"
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
#include "NiagaraDataSet.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraScript.h"
#include "NiagaraSimCache.h"
#include "NiagaraSimCacheFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraSystemInstanceController.h"

#include <initializer_list>
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
	// 夹具在授予后改作者配置；按生产契约丢弃授予期预备配置，下次准入重新解析。
	InvalidatePreparedConfiguration();
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
	InvalidatePreparedConfiguration();
	for (FGGYGOComboStep& Step : ComboSteps) { Step.DamageMultiplier = Multiplier; }
}

void UGGYGOPlayerComboLifecycleTestAbility::SetFirstPlayRateForTest(float PlayRate)
{
	InvalidatePreparedConfiguration();
	if (ComboSteps.IsValidIndex(0)) { ComboSteps[0].PlayRate = PlayRate; }
}

void UGGYGOPlayerComboLifecycleTestAbility::CleanupAbilityResourcesForTermination(
	const FGGYGOAbilityTerminationContext& Context)
{
	LastTermination = Context;
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

bool UGGYGOPlayerComboLifecycleTestAbility::DeferTwoEndsForTest(
	const FGGYGOAbilityActivationHandle& Original, bool& bBothAcceptedWhileLocked)
{
	IncrementListLock();
	const bool bFirst = RequestAbilityEnd(Original, false, false).IsAccepted();
	const bool bSecond = RequestAbilityEnd(Original, false, true).IsAccepted();
	bBothAcceptedWhileLocked = bFirst && bSecond && IsActive();
	DecrementListLock();
	return !IsActive();
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
#include "Input/GGYGOPlayerInput.h"
#include "Input/Tests/GGYGOInputTestTypes.h"
#include "InputCoreTypes.h"
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
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMesh.h"
#include "EnhancedInputSubsystems.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/GameModeBase.h"
#include "GenericPlatform/GenericWindow.h"
#include "Input/GGYGOInputComponent.h"
#include "Input/GGYGOInputConfig.h"
#include "Input/GGYGOMovementInputOriginResource.h"
#include "Input/GGYGOPlayerInput.h"
#include "InputKeyEventArgs.h"
#include "MaterialShared.h"
#include "Materials/MaterialInterface.h"
#include "Player/GGYGOLocalPlayer.h"
#include "Player/GGYGOPlayerController.h"
#include "NiagaraDataSetAccessor.h"
#include "NiagaraMeshRendererProperties.h"
#include "PlayInEditorDataTypes.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "SceneInterface.h"
#include "SceneViewExtension.h"
#include "Serialization/JsonSerializer.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "StaticMeshResources.h"
#include "UObject/StructOnScope.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"
#endif

/**
 * Configures combo resources on the Input fixture's real local Pawn/Host ASC and is the single
 * narrow friend used to inspect private GA resources. The Input fixture owns all actors and teardown.
 */
namespace GGYGOPlayerComboTestLifecycle
{
	/** 一次原生激活请求：Try 返回值与 Try 内原生 PreActivate 签发的激活身份（Try 内同步结束时仍保留）。 */
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

	/** 原生 OnAbilityEnded 对应的那次结束清理参数。 */
	struct FEnded
	{
		FGGYGOAbilityTerminationContext Original;
		const FGGYGOAbilityTerminationContext& GetOriginal() const { return Original; }
		bool HasCompletion() const { return Original.GetOriginalActivation().HasActivation(); }
	};

	/** 只对夹具测试能力读取其最近一次结束清理参数；其它能力的结束通知不带激活身份。 */
	FDelegateHandle ObserveEnded(UAbilitySystemComponent* ASC, TFunction<void(const FEnded&)> Sink)
	{
		return ASC->OnAbilityEnded.AddLambda([Sink = MoveTemp(Sink)](const FAbilityEndedData& Data)
		{
			FEnded Ended;
			if (const UGGYGOPlayerComboLifecycleTestAbility* Ability = Cast<UGGYGOPlayerComboLifecycleTestAbility>(Data.AbilityThatEnded.Get()))
			{
				Ended.Original = Ability->GetLastTerminationForTest();
			}
			Sink(Ended);
		});
	}
}

struct FGGYGOPlayerComboLifecycleFixture
{
	FGGYGOInputTestFixture* Input = nullptr;
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
	static uint64 ReadProductionStepToken(const UGGYGOPlayerComboAbility* Production)
	{
		return Production ? Production->CurrentStepToken : 0;
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
		Production->InvalidatePreparedConfiguration();
		return true;
	}
	static void RestoreProductionMainPointProbe(UGGYGOPlayerComboAbility* Production, int32 StepIndex,
		const FGGYGOComboStep& Original, const UAnimMontage* Probe)
	{
		if (Production && Production->ComboSteps.IsValidIndex(StepIndex)
			&& Production->ComboSteps[StepIndex].Montage == Probe)
		{
			Production->ComboSteps[StepIndex] = Original;
			Production->InvalidatePreparedConfiguration();
		}
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

	/**
	 * Input 夹具绑定后配置连段运行资源；Pawn/PC/LocalPlayer/Host ASC 及其生命周期都属于 Input 夹具。
	 * Mesh/AnimInstance/Trace 已由 PreparePawnForTest 在 Host 绑定前组装。
	 */
	bool Initialize(FGGYGOInputTestFixture& InInput)
	{
		Input = &InInput;
		World = Input->GetWorld();
		AGGYGOInputTestPawn* const Pawn = Input->GetPawn();
		Controller = Input->GetController();
		LocalPlayer = Input->GetLocalPlayer();
		ASC = Pawn ? Pawn->GetASCForTest() : nullptr;
		Movement = Pawn ? Cast<UGGYGOCharacterMovementComponent>(Pawn->GetCharacterMovement()) : nullptr;
		if (!World || !Pawn || Character != Pawn || !Controller || !LocalPlayer || !ASC || !Movement
			|| !Mesh || !AnimInstance || Mesh->GetAnimInstance() != AnimInstance || !Trace) { return false; }
		Movement->SetMovementMode(MOVE_Walking);
		UGGYGOMovementSet* MovementSet = NewObject<UGGYGOMovementSet>(Character);
		MovementSet->bUseCurveDrivenSpeed = false; // Explicit fixed locomotion fixture; action still reads real original curves.
		FString MovementError;
		if (!Movement->SetMovementSet(MovementSet, &MovementError)) { return false; }
		// Enter the real component lifecycle for its native owner scope; no private identity is forged.
		Movement->BeginPlay();
		FGGYGOMovementOwnerSyncScopeId OwnerScope;
		if (!Movement->GetMovementOwnerSyncScope(OwnerScope, MovementError)) { return false; }

		// 与 Input 夹具的能力动作同一 InputTag：按键经 Hero 请求、PC PostProcessInput 交 ASC 处理后激活。
		FGameplayAbilitySpec AbilitySpec(UGGYGOPlayerComboLifecycleTestAbility::StaticClass(), 1);
		AbilitySpec.GetDynamicSpecSourceTags().AddTag(GGYGOGameplayTags::InputTag_Attack_Light);
		AbilityHandle = ASC->GiveAbility(AbilitySpec);
		FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromHandle(AbilityHandle);
		Ability = Spec ? Cast<UGGYGOPlayerComboLifecycleTestAbility>(Spec->GetPrimaryInstance()) : nullptr;
		if (!Ability) { return false; }
		Ability->ConfigureStepsForTest(Montage, TraceStartBone, TraceEndBone);
		return true;
	}

	/** Host 绑定前组装 Avatar：Engine SkeletalCube、临时 Montage、测试 AnimInstance 与 Trace 组件。 */
	bool PreparePawnForTest(AGGYGOInputTestPawn& Pawn)
	{
		Character = &Pawn;
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

		Trace = NewObject<UGGYGOMeleeTraceComponent>(Character);
		Trace->RegisterComponent();
		Trace->SetComponentTickEnabled(false);
		return true;
	}

	bool HasOriginalLocalActorInfo() const
	{
		const AActor* const Host = ASC ? ASC->GetOwner() : nullptr;
		return LocalPlayer && Controller && Character && ASC && Host && ASC->AbilityActorInfo.IsValid()
			&& Controller->GetLocalPlayer() == LocalPlayer && LocalPlayer->PlayerController == Controller
			&& Controller->GetPawn() == Character && Character->GetController() == Controller
			&& Cast<UGGYGOPlayerInput>(Controller->PlayerInput.Get()) != nullptr
			&& Controller->IsLocalController() && Character->IsLocallyControlled()
			&& Host->GetOwner() == Controller
			&& ASC->AbilityActorInfo->PlayerController.Get() == Controller
			&& ASC->AbilityActorInfo->OwnerActor.Get() == Host
			&& ASC->AbilityActorInfo->AvatarActor.Get() == Character
			&& ASC->AbilityActorInfo->IsLocallyControlled();
	}

	/**
	 * 按一次真实能力键：PlayerInput 入键并走 PC 原生输入帧，由 Hero → ASC ProcessAbilityInput 激活，随后释放。
	 * 结果只记录按键帧结束时仍活动的本地激活；按键帧内已结束的激活与被拒绝一样报告为未接受。
	 */
	GGYGOPlayerComboTestLifecycle::FActivation ActivateFromInputForTest(FAutomationTestBase& Test) const
	{
		GGYGOPlayerComboTestLifecycle::FActivation Result;
		if (!Input || !Ability || !Input->PressAbilityKeyForTest(Test)) { return Result; }
		if (Ability->IsActive())
		{
			Result.bNativeAccepted = true;
			Result.OriginalActivation = Ability->CaptureCurrentActivation();
		}
		Input->ReleaseAbilityKeyForTest(Test);
		return Result;
	}

	bool HasActiveMesh() const { return Ability && Ability->ActiveMesh != nullptr; }
	bool HasStepMotionResources() const { return Ability && Ability->StepMotionResources.IsValid(); }
	/** 仅在未激活时取可改的作者配置；按生产契约先丢弃授予期预备配置，下次准入按改后的配置解析。 */
	FGGYGOComboStep& GetFirstStepForTest() const
	{
		check(Ability && !Ability->IsActive() && Ability->ComboSteps.IsValidIndex(0));
		Ability->InvalidatePreparedConfiguration();
		return Ability->ComboSteps[0];
	}
	/** 仅修改启动前的作者配置，不创建另一条运行状态写入链。 */
	FGGYGOComboStep& UseSingleStepForTest() const
	{
		check(Ability && !Ability->IsActive() && Ability->ComboSteps.IsValidIndex(0));
		Ability->InvalidatePreparedConfiguration();
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
		// 预备配置含本 Montage 的窗口声明；改资产后丢弃。
		Ability->InvalidatePreparedConfiguration();
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
		check(Ability && !Ability->IsActive());
		Ability->InvalidatePreparedConfiguration();
		Ability->DamageEffect = EffectClass;
		Ability->bUseSharedDamageEffectWhenUnset = false;
		for (FGGYGOComboStep& Step : Ability->ComboSteps) { Step.HitCueTag = CueTag; }
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
	/**
	 * 每个用例独占的 Input 夹具：私有 GI/World/LocalPlayer/项目 PC/Pawn/Host ASC，析构时由它按原生顺序退出。
	 * 需要 MessageRouter 的倍率结算显式让 GI 走原生 Init 以启动 GI 子系统。
	 */
	struct FGGYGOComboLifecycleTestWorld
	{
		explicit FGGYGOComboLifecycleTestWorld(bool bInWithMessageRouter = false)
			: bWithMessageRouter(bInWithMessageRouter) {}

		FGGYGOInputTestFixture Input;
		UWorld* World = nullptr;
		TWeakObjectPtr<UGameInstance> GameInstance;
		const bool bWithMessageRouter;
	};

	bool InitializeFixture(FAutomationTestBase& Test, FGGYGOComboLifecycleTestWorld& TestWorld,
		FGGYGOPlayerComboLifecycleFixture& Fixture)
	{
		if (TestWorld.bWithMessageRouter) { TestWorld.Input.RequestGameInstanceSubsystemsForTest(); }
		TestWorld.Input.MapAbilityKeyForTest(EKeys::J);
		TestWorld.Input.PreparePawnBeforeBindingForTest(
			[&Fixture](AGGYGOInputTestPawn& Pawn) { return Fixture.PreparePawnForTest(Pawn); });
		if (!TestWorld.Input.Initialize(Test)) { return false; }
		TestWorld.World = TestWorld.Input.GetWorld();
		TestWorld.GameInstance = TestWorld.Input.GetGameInstance();
		// CMC 在 Fixture.Initialize 中进入 BeginPlay 后才有 OwnerSync 范围；之后完成映射重建，
		// 由 Hero 按生产流程建立移动输入会话，能力按键的 edge 才有原评价来源。
		return Test.TestTrue(TEXT("valid Character/Engine SkeletalMesh/Montage/ASC/Trace fixture"),
				Fixture.Initialize(TestWorld.Input))
			&& TestWorld.Input.CompleteNativeMappingRebuildForTest(Test)
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOPlayerComboFXAttachmentEntryTest,
	"GGYGO.AbilitySystem.PlayerCombo.FX.AttachmentEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOPlayerComboFXAttachmentEntryTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		const TCHAR* Name;
		bool bWorld;
		bool bInherit;
		bool bValidDefault;
		bool bExpectedSuccess;
	};
	const FCase Cases[] = {
		{TEXT("Attached 全覆盖忽略无效默认挂点"), false, false, false, true},
		{TEXT("Attached 有继承者拒绝无效默认挂点"), false, true, false, false},
		{TEXT("World 拒绝无效起点"), true, true, false, false},
		{TEXT("World 接受有效起点"), true, true, true, true}
	};
	for (const FCase& Case : Cases)
	{
		FGGYGOComboLifecycleTestWorld TestWorld;
		FGGYGOPlayerComboLifecycleFixture Fixture;
		if (!InitializeFixture(*this, TestWorld, Fixture)) { return false; }
		FGGYGOComboStep& Step = Fixture.UseSingleStepForTest();
		Step.NextStepIndex = INDEX_NONE;
		FGGYGOComboStepFX& FX = Step.FX;
		FX.Mode = EGGYGOComboFXMode::MontageWindow;
		FX.Space = Case.bWorld ? EGGYGONiagaraEffectSpace::World : EGGYGONiagaraEffectSpace::Attached;
		FX.SocketName = Case.bValidDefault ? Fixture.TraceStartBone : FName(TEXT("GGYGO.Test.MissingFXSocket"));
		FX.BeginEventTag = GGYGOGameplayTags::Event_Montage_FXWindowBegin;
		FX.EndEventTag = GGYGOGameplayTags::Event_Montage_FXWindowEnd;
		FX.NormalStopMode = EGGYGONiagaraEffectStopMode::DestroyImmediately;
		FX.AbortStopMode = EGGYGONiagaraEffectStopMode::DestroyImmediately;
		FX.RemoteSeekDeltaSeconds = 0.01f;
		FX.RemoteMaxSimTimeSeconds = 0.002f;
		FX.SystemUnits.SetNum(2);
		for (int32 Index = 0; Index < FX.SystemUnits.Num(); ++Index)
		{
			FGGYGONiagaraEffectUnit& Unit = FX.SystemUnits[Index];
			Unit.System = NewObject<UNiagaraSystem>(Fixture.Ability);
			Unit.FieldSettings.Mode = EGGYGOFXFieldMode::NoCustomField;
			Unit.bOverrideAttachment = !Case.bWorld && !(Case.bInherit && Index == 1);
			Unit.SocketName = Index == 0 ? Fixture.TraceStartBone : Fixture.TraceEndBone;
		}
		auto* Window = NewObject<UGGYGOAnimNotifyState_GameplayEventWindow>(Fixture.Montage);
		Window->InitializeEventTags(FX.BeginEventTag, FX.EndEventTag);
		Window->NotifyStateBehaviorFlags = static_cast<uint8>(EAnimNotifyStateBehaviorFlags::NoMergeOnConcurrentPlay);
		FAnimNotifyEvent Event;
		Event.NotifyStateClass = Window;
		Event.MontageTickType = EMontageNotifyTickType::Queued;
		Event.Link(Fixture.Montage, 0.2f);
		Event.SetDuration(0.2f);
		Fixture.Montage->Notifies.Add(Event);
		static_cast<UAnimSequenceBase*>(Fixture.Montage)->RefreshCacheData();
		FString Error;
		if (!TestTrue(TEXT("夹具 FX 作者声明有效"), Fixture.Ability->ValidateComboConfiguration(Error)))
		{
			AddError(Error);
			return false;
		}
		TestEqual(FString::Printf(TEXT("%s：窗口与入场共用的当前 Mesh 校验"), Case.Name),
			FGGYGOMontageActionResources::ValidateFXSocketsOnMesh(FX, *Fixture.Mesh, Error), Case.bExpectedSuccess);
		if (!Case.bExpectedSuccess)
		{
			TestTrue(TEXT("拒绝诊断包含实际缺失挂点"), Error.Contains(FX.SocketName.ToString()));
			AddExpectedMessagePlain(TEXT("configuration rejected: FX"), ELogVerbosity::Error,
				EAutomationExpectedMessageFlags::Contains, 1);
		}
		// 真正经 PlayerInput → Hero → ASC → ReadStepStartInputs；本测试停在 0.2 秒 FX 窗口前。
		const auto Activation = Fixture.ActivateFromInputForTest(*this);
		TestEqual(FString::Printf(TEXT("%s：真实输入入场结果"), Case.Name),
			Activation.bNativeAccepted, Case.bExpectedSuccess);
		TestEqual(TEXT("只有通过实际挂点校验的动作才能播放 Montage"),
			Fixture.AnimInstance->GetSuccessfulGuardReturnCountForTest(), Case.bExpectedSuccess ? 1 : 0);
		TestEqual(TEXT("原生 Montage Task 与入场结果一致"), Fixture.GetMontageTask() != nullptr, Case.bExpectedSuccess);
		if (Fixture.Ability->IsActive()) { Fixture.Ability->FinishForTest(); }
		TestEqual(TEXT("入口测试收尾不残留 Task"), Fixture.Ability->GetActiveTaskCountForTest(), 0);
		TestFalse(TEXT("入口测试收尾归还 CMC 动作"), Fixture.Movement->HasActiveActionMotion());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOPlayerComboObservationSourceTest,
	"GGYGO.AbilitySystem.PlayerCombo.Observation.OriginalSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOPlayerComboObservationSourceTest::RunTest(const FString& Parameters)
{
	FGGYGOComboLifecycleTestWorld TestWorld;
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
	const auto Activation = Fixture.ActivateFromInputForTest(*this);
	if (!TestTrue(TEXT("真实按键经 Hero/ASC 启动"), Activation.bNativeAccepted && Activation.OriginalActivation.HasActivation())
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
	const auto Completion = GGYGOPlayerComboTestLifecycle::ObserveEnded(Fixture.ASC, [&](const GGYGOPlayerComboTestLifecycle::FEnded& Notice)
		{
			FGGYGOPlayerComboObservationFacts Ended;
			bMatchedCompleted = Notice.HasCompletion() && Observation.ReadHistory(Ended) && Ended.bCleanupObserved
				&& Notice.GetOriginal().GetOriginalActivation().HasSameActivation(Ended.Activation);
		});
	const auto End = Fixture.Ability->RequestAbilityEnd(Activation.OriginalActivation, false, false);
	Fixture.ASC->OnAbilityEnded.Remove(Completion);
	TestTrue(TEXT("cleanup入口之前不提前签发cleanup历史"), bBeforeCleanupHistory);
	TestTrue(TEXT("原完整退出在cleanup之后且匹配固定proof"), bMatchedCompleted
		&& End.Outcome == EGGYGOAbilityTerminationOutcome::Accepted);
	TestTrue(TEXT("已退役原历史仍读回"), Observation.ReadHistory(Facts) && Facts.bCleanupObserved
		&& !Facts.bTaskNaturalEnded && !Facts.bConsumerAccepted && !Facts.bDrainAccepted);
	Snapshot.Montage = Fixture.Montage;
	TestFalse(TEXT("退出后live快照拒绝"),
		Fixture.Ability->ReadOriginalStepObservation(Observation, Snapshot, Error, &bResourceSourceCurrent));
	TestTrue(TEXT("失败live输出及GA源事实完整清空"),
		!Snapshot.Montage && Snapshot.MontageInstanceId == INDEX_NONE && !bResourceSourceCurrent);
	const auto Successor = Fixture.ActivateFromInputForTest(*this);
	if (TestTrue(TEXT("同资产同实例后继由新一次按键真实启动"), Successor.bNativeAccepted && Successor.OriginalActivation.HasActivation()))
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
	FGGYGOComboLifecycleTestWorld TestWorld;
	FGGYGOPlayerComboLifecycleFixture Fixture;
	if (!InitializeFixture(*this, TestWorld, Fixture)) { return false; }
	FGGYGOComboStep& Step = Fixture.UseSingleStepForTest();
	Step.InterruptionMode = EGGYGOActionInterruptionMode::Disabled;
	Step.InterruptionNotifyName = NAME_None;
	Step.NextStepIndex = INDEX_NONE;
	const auto Activation = Fixture.ActivateFromInputForTest(*this);
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
	FQuat OtherEntryRotation = FQuat::Identity;
	if (!TestTrue(TEXT("公开入口创建另一真实CMC令牌"), Fixture.Movement->BeginMontageActionMotion(Source,
		Snapshot.MontageInstanceId, Snapshot.PositionSeconds, Snapshot.InstancePlayRate * Source->MontageRateScale,
		Step.MotionTranslationScale, FGGYGOActionMotionStartFacing{}, OtherHandle, OtherEntryRotation, Error)
		&& OtherHandle != Original.MotionHandle)
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
	const auto Completion = GGYGOPlayerComboTestLifecycle::ObserveEnded(Fixture.ASC, [&](const GGYGOPlayerComboTestLifecycle::FEnded& Notice)
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
	Fixture.ASC->OnAbilityEnded.Remove(Completion);
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

	FGGYGOComboLifecycleTestWorld TestWorld;
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
	const GGYGOPlayerComboTestLifecycle::FActivation ShapeActivation = Fixture.ActivateFromInputForTest(*this);
	if (!TestTrue(TEXT("新 Shape 正常激活且忽略非法历史值"), ShapeActivation.bNativeAccepted)) { return false; }
	if (!TestTrue(TEXT("新 Shape 受控请求保留确切本地 Original"),
		ShapeActivation.bNativeAccepted
		&& ShapeActivation.OriginalActivation.HasActivation()
		&& Fixture.Ability->CaptureCurrentActivation().HasSameActivation(ShapeActivation.OriginalActivation))) { return false; }
	TestEqual(TEXT("正常路径提交一次"), CommitCount, 1);
	TestEqual(TEXT("正常路径实际 Montage 播放一次"), Fixture.AnimInstance->GetSuccessfulGuardReturnCountForTest(), 1);
	TestEqual(TEXT("正常路径保留原 Montage 与 Input 两任务"), Fixture.Ability->GetActiveTaskCountForTest(), 2);
	TestTrue(TEXT("正常Main已取得真实CMC原动作资源"), Fixture.Movement->HasActiveActionMotion());
	TestEqual(TEXT("Main动作接管时普通Walk/Run速度上限为0"), Fixture.Movement->GetMaxSpeed(), 0.0f);
	const FGGYGOAbilityTerminationResult ShapeEnd =
		Fixture.Ability->RequestAbilityEnd(ShapeActivation.OriginalActivation, false, false);
	TestTrue(TEXT("新 Shape 普通结束被接受"), ShapeEnd.Outcome == EGGYGOAbilityTerminationOutcome::Accepted);
	TestTrue(TEXT("新 Shape 结束清理收到固定 Original 和普通 End 参数"),
		Fixture.Ability->GetLastTerminationForTest().GetOriginalActivation().HasSameActivation(ShapeActivation.OriginalActivation)
		&& !Fixture.Ability->GetLastTerminationForTest().WasCancelled());
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
	FGGYGOComboLifecycleTestWorld TestWorld;
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
	const GGYGOPlayerComboTestLifecycle::FActivation Activation = Fixture.ActivateFromInputForTest(*this);
	if (!TestTrue(TEXT("无作者点的显式关闭动作通过真实按键启动"), Activation.bNativeAccepted
		&& Activation.bNativeAccepted
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
	TestTrue(TEXT("按原激活正常清理测试动作"), Cleanup.Outcome == EGGYGOAbilityTerminationOutcome::Accepted
		&& Fixture.Ability->GetLastTerminationForTest().GetOriginalActivation().HasSameActivation(Activation.OriginalActivation));
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
	FGGYGOComboLifecycleTestWorld TestWorld;
	FGGYGOPlayerComboLifecycleFixture Fixture;
	if (!InitializeFixture(*this, TestWorld, Fixture)) { return false; }
	FString Error;
	const FGGYGOComboStep& Step = Fixture.GetFirstStepForTest();
	if (!TestTrue(TEXT("攻击作者点＋默认移动 Disabled/None 的既有配置可用"),
		Step.MovementInterruptionMode == EGGYGOActionInterruptionMode::Disabled
		&& Step.MovementInterruptionNotifyName.IsNone() && Fixture.Ability->ValidateComboConfiguration(Error))) { return false; }
	const GGYGOPlayerComboTestLifecycle::FActivation Activation = Fixture.ActivateFromInputForTest(*this);
	if (!TestTrue(TEXT("通过真实按键启动"), Activation.bNativeAccepted
		&& Activation.bNativeAccepted
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
	TestTrue(TEXT("按原激活正常清理"), Cleanup.Outcome == EGGYGOAbilityTerminationOutcome::Accepted);
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
	FGGYGOComboLifecycleTestWorld TestWorld;
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
	FGGYGOComboLifecycleTestWorld TestWorld;
	FGGYGOPlayerComboLifecycleFixture Fixture;
	if (!InitializeFixture(*this, TestWorld, Fixture)) { return false; }
	FGGYGOComboStep& Step = Fixture.UseSingleStepForTest();
	Step.NextStepIndex = INDEX_NONE;
	const float MainEnd = Fixture.Montage->GetAnimCompositeSection(Fixture.Montage->GetSectionIndex(Step.EndSection)).GetTime();
	if (!TestTrue(TEXT("配置独立移动点"), Fixture.AddMovementPointForTest(MainEnd))) { return false; }
	const FName AttackName = Step.InterruptionNotifyName;
	const FName MovementName = Step.MovementInterruptionNotifyName;
	const GGYGOPlayerComboTestLifecycle::FActivation Activation = Fixture.ActivateFromInputForTest(*this);
	if (!TestTrue(TEXT("真实 GAS 激活双点动作"), Activation.bNativeAccepted
		&& Activation.bNativeAccepted)) { return false; }
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
	TestTrue(TEXT("原激活幂等清理"), Cleanup.Outcome == EGGYGOAbilityTerminationOutcome::Accepted);
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
	FGGYGOComboLifecycleTestWorld TestWorld;
	FGGYGOPlayerComboLifecycleFixture Fixture;
	if (!InitializeFixture(*this, TestWorld, Fixture)) { return false; }
	FGGYGOComboStep& Step = Fixture.UseSingleStepForTest();
	Step.NextStepIndex = INDEX_NONE;
	const int32 EndIndex = Fixture.Montage->GetSectionIndex(Step.EndSection);
	Fixture.Montage->GetAnimCompositeSection(EndIndex).Link(Fixture.Montage, 0.9f);
	static_cast<UAnimSequenceBase*>(Fixture.Montage)->RefreshCacheData();
	const float MainEnd = Fixture.Montage->GetAnimCompositeSection(EndIndex).GetTime();
	if (!TestTrue(TEXT("按当前 Section 时间配置移动点"), Fixture.AddMovementPointForTest(MainEnd))) { return false; }
	const GGYGOPlayerComboTestLifecycle::FActivation Activation = Fixture.ActivateFromInputForTest(*this);
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
	TestTrue(TEXT("原激活清理完成"), Cleanup.Outcome == EGGYGOAbilityTerminationOutcome::Accepted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOPlayerComboInvalidTraceShapeTest,
	"GGYGO.AbilitySystem.PlayerCombo.TraceShape.InvalidConfiguration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOPlayerComboInvalidTraceShapeTest::RunTest(const FString& Parameters)
{
	FGGYGOComboLifecycleTestWorld TestWorld;
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
	const FDelegateHandle CommitHandle = Fixture.ASC->AbilityCommittedCallbacks.AddLambda([&](UGameplayAbility* Ability)
	{
		if (Ability == Fixture.Ability) { ++CommitCount; }
	});
	const FDelegateHandle EndHandle = Fixture.ASC->OnAbilityEnded.AddLambda([&](const FAbilityEndedData& Data)
	{
		if (Data.AbilityThatEnded == Fixture.Ability) { ++EndCount; }
	});
	ON_SCOPE_EXIT
	{
		Fixture.ASC->AbilityCommittedCallbacks.Remove(CommitHandle);
		Fixture.ASC->OnAbilityEnded.Remove(EndHandle);
	};
	// 生产拒绝日志保持 Error 级别；声明为恰好一次的预期，少报或多报都使本项失败。
	AddExpectedMessagePlain(FString::Printf(TEXT("PlayerCombo [%s] 激活失败时连段配置无效："), *Fixture.Ability->GetPathName()),
		ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, 1);
	const GGYGOPlayerComboTestLifecycle::FActivation EmptyShapeRequest =
		GGYGOPlayerComboTestLifecycle::Activate(Fixture.ASC, Fixture.AbilityHandle);
	bBehaviorPassed &= TestFalse(TEXT("实际请求明确拒绝空 Shape"), EmptyShapeRequest.bNativeAccepted);
	bBehaviorPassed &= TestTrue(TEXT("准入拒绝没有签发激活"), !EmptyShapeRequest.OriginalActivation.HasActivation());
	// 作者配置只在未激活时改动；不再在蓝图激活回调里改 Shape 模拟提交前失效，该路径不属于生产可达输入。
	bBehaviorPassed &= TestEqual(TEXT("准入拒绝没有 Commit 通知"), CommitCount, 0);
	bBehaviorPassed &= TestEqual(TEXT("准入拒绝未创建激活结束通知"), EndCount, 0);
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
		FGGYGOComboLifecycleTestWorld TestWorld;
		FGGYGOPlayerComboLifecycleFixture Fixture;
		if (!InitializeFixture(*this, TestWorld, Fixture)) { return false; }

		bool bReactivatedFromBlueprintDispatch = false;
		Fixture.Ability->SetK2ActivateActionForTest([&]
		{
			bReactivatedFromBlueprintDispatch = Fixture.Ability->EndAndReactivateForTest();
		});
		TestTrue(TEXT("真实 Super Activate 分发期间 End 后可立即重激活同一 Spec"),
			Fixture.ActivateFromInputForTest(*this).bNativeAccepted);
		TestTrue(TEXT("K2_ActivateAbility 回调成功建立新激活"), bReactivatedFromBlueprintDispatch);
		TestTrue(TEXT("旧 Super Activate 栈返回后新激活仍活跃"), Fixture.Ability->IsActive());
		TestEqual(TEXT("旧 Super Activate 栈没有重建任务或覆盖新段"), Fixture.Ability->GetActiveTaskCountForTest(), 2);
		TestEqual(TEXT("新激活仍在首段"), Fixture.Ability->GetCurrentComboStep(), 0);
		Fixture.Ability->FinishForTest();
	}

	{
		FGGYGOComboLifecycleTestWorld TestWorld;
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
		Fixture.ActivateFromInputForTest(*this);
		Fixture.ASC->AbilityCommittedCallbacks.Remove(CommitHandle);
		TestTrue(TEXT("CommitAbility 的 AbilityCommittedCallbacks 内 End 后重激活成功"), bCommitCallbackReentered);
		TestTrue(TEXT("旧 CommitAbility 栈退出后新激活仍活跃"), Fixture.Ability->IsActive());
		TestEqual(TEXT("旧 CommitAbility 栈没有覆盖新任务"), Fixture.Ability->GetActiveTaskCountForTest(), 2);
		TestEqual(TEXT("新激活仍在首段"), Fixture.Ability->GetCurrentComboStep(), 0);
		Fixture.Ability->FinishForTest();
	}

	{
		FGGYGOComboLifecycleTestWorld TestWorld;
		FGGYGOPlayerComboLifecycleFixture Fixture;
		if (!InitializeFixture(*this, TestWorld, Fixture)) { return false; }

		// 原播放的真实创建回调内结束并用裸 Try 重激活：后继没有自己的输入来源，
		// Initial 路由明确拒绝它；原 Ready 栈返回后不得清理或复活后继，资源全部归还。
		bool bReadyCallbackReentered = false;
		int32 NativeStartedCount = 0;
		TStrongObjectPtr<UGGYGOMontageStartedDiagnosticObserver> StartedObserver(
			NewObject<UGGYGOMontageStartedDiagnosticObserver>(Fixture.Character));
		if (!TestNotNull(TEXT("真实 Started 观察者"), StartedObserver.Get())) { return false; }
		ON_SCOPE_EXIT
		{
			StartedObserver->DisarmForTest();
		};
		StartedObserver->ArmForTest(Fixture.AnimInstance, Fixture.Montage, [&]
		{
			++NativeStartedCount;
			bReadyCallbackReentered = Fixture.Ability->EndAndReactivateForTest();
		});
		AddExpectedError(TEXT("WaitComboInput"), EAutomationExpectedErrorFlags::Contains, 0);
		AddExpectedError(TEXT("original Input Task did not remain active after Ready"), EAutomationExpectedErrorFlags::Contains, 0);
		Fixture.ActivateFromInputForTest(*this);
		StartedObserver->DisarmForTest();
		TestTrue(TEXT("真实 OnMontageStarted 创建回调内原生重激活被接受"), bReadyCallbackReentered);
		TestTrue(TEXT("原播放真实创建"), NativeStartedCount >= 1);
		TestFalse(TEXT("没有自己输入来源的后继被明确拒绝，能力不再活动"), Fixture.Ability->IsActive());
		TestEqual(TEXT("拒绝后不残留任务"), Fixture.Ability->GetActiveTaskCountForTest(), 0);
		TestFalse(TEXT("拒绝后不残留段资源"), Fixture.HasStepMotionResources());
	}

	{
		FGGYGOComboLifecycleTestWorld TestWorld;
		FGGYGOPlayerComboLifecycleFixture Fixture;
		if (!InitializeFixture(*this, TestWorld, Fixture)) { return false; }
		if (!TestTrue(TEXT("结束重入场景启动前声明真实作者命中窗"), Fixture.AddHitWindowForTest())) { return false; }
		if (!TestTrue(TEXT("结束重入初始激活"), Fixture.ActivateFromInputForTest(*this).bNativeAccepted)) { return false; }
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
		bool bReentryAttempted = false;
		const FDelegateHandle EndHandle = Fixture.ASC->OnAbilityEnded.AddLambda([&](const FAbilityEndedData& Data)
		{
			// 后继在 Try 内被同步拒绝时会再次进入本广播；只对原激活的结束重入一次。
			if (Data.AbilityThatEnded == Fixture.Ability && !bReentryAttempted)
			{
				bReentryAttempted = true;
				bOriginalTraceWasClosedBeforeBroadcast = Fixture.Trace->QueryOwnedTraceWindow(EndingTraceWindow)
					== EGGYGOMeleeTraceWindowQueryResult::Inactive;
				bResourcesWereCleanBeforeBroadcast =
					Fixture.WereAllResourcesCleanedBeforeBroadcast(EndingMontageTask, EndingInputTask);
				bReactivatedFromEndBroadcast = Fixture.ASC->TryActivateAbility(Fixture.AbilityHandle);
			}
		});
		// 广播内的裸 Try 没有自己的输入来源，后继由 Initial 路由明确拒绝；旧结束栈只收口自身资源。
		AddExpectedError(TEXT("WaitComboInput"), EAutomationExpectedErrorFlags::Contains, 0);
		AddExpectedError(TEXT("original Input Task did not remain active after Ready"), EAutomationExpectedErrorFlags::Contains, 0);
		Fixture.Ability->FinishForTest();
		Fixture.ASC->OnAbilityEnded.Remove(EndHandle);
		TestTrue(TEXT("Super EndAbility 广播前准确原 Owned Trace 句柄已为 Inactive"),
			bOriginalTraceWasClosedBeforeBroadcast);
		TestTrue(TEXT("Super EndAbility 广播前旧资源、Montage、输入任务、Trace 与 Mesh 全部清理"),
			bResourcesWereCleanBeforeBroadcast);
		TestTrue(TEXT("Super EndAbility 同步广播内原生重激活被接受"), bReactivatedFromEndBroadcast);
		TestFalse(TEXT("没有自己输入来源的后继被明确拒绝，能力不再活动"), Fixture.Ability->IsActive());
		TestEqual(TEXT("旧 EndAbility 返回后不残留任务"), Fixture.Ability->GetActiveTaskCountForTest(), 0);
		TestFalse(TEXT("旧 EndAbility 返回后不残留段资源"), Fixture.HasStepMotionResources());
	}

	{
		FGGYGOComboLifecycleTestWorld TestWorld;
		FGGYGOPlayerComboLifecycleFixture Fixture;
		if (!InitializeFixture(*this, TestWorld, Fixture)) { return false; }
		if (!TestTrue(TEXT("ScopeLock 延期 End 初始激活"), Fixture.ActivateFromInputForTest(*this).bNativeAccepted)) { return false; }
		const FGGYGOAbilityActivationHandle Original = Fixture.Ability->CaptureCurrentActivation();
		int32 EndCount = 0;
		const FDelegateHandle EndHandle = Fixture.ASC->OnAbilityEnded.AddLambda([&](const FAbilityEndedData& Data)
		{
			if (Data.AbilityThatEnded == Fixture.Ability) { ++EndCount; }
		});
		bool bBothAcceptedWhileLocked = false;
		const bool bEndedAfterUnlock = Fixture.Ability->DeferTwoEndsForTest(Original, bBothAcceptedWhileLocked);
		Fixture.ASC->OnAbilityEnded.Remove(EndHandle);
		TestTrue(TEXT("ScopeLock 内两次结束请求都被接受并延后"), bBothAcceptedWhileLocked);
		TestTrue(TEXT("解锁后原激活结束"), bEndedAfterUnlock);
		TestEqual(TEXT("同一原激活只结束一次，第二次延后请求不再作用"), EndCount, 1);
		TestEqual(TEXT("延后结束后不残留任务"), Fixture.Ability->GetActiveTaskCountForTest(), 0);
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

	FGGYGOComboLifecycleTestWorld TestWorld;
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
		GGYGOPlayerComboTestLifecycle::FEnded CompletedNotice;
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
		const FDelegateHandle CompletedHandle = GGYGOPlayerComboTestLifecycle::ObserveEnded(Fixture.ASC, [&](const GGYGOPlayerComboTestLifecycle::FEnded& Notice)
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
		Fixture.ASC->OnAbilityEnded.Remove(CompletedHandle);

		const auto Check = [&](const TCHAR* Detail, bool bCondition)
		{
			return TestTrue(FString::Printf(TEXT("%s %s"), Label, Detail), bCondition);
		};
		bool bPassed = Check(TEXT("正常结束请求被接受"), EndResult.Outcome == EGGYGOAbilityTerminationOutcome::Accepted);
		bPassed &= Check(TEXT("原生结束广播恰好一次且参数匹配"), NativeEndCount == 1 && bNativeEndParametersMatch);
		bPassed &= Check(TEXT("原生结束广播前实际已持资源恢复"), bResourcesRestoredBeforeNativeBroadcast);
		bPassed &= Check(TEXT("ASC 结束通知恰好一次且对应一次结束清理"), CompletedCount == 1 && CompletedNotice.HasCompletion());
		bPassed &= Check(TEXT("结束通知匹配固定原激活与普通 End 参数"),
			CompletedNotice.GetOriginal().GetOriginalActivation().HasSameActivation(OriginalActivation)
			&& !CompletedNotice.GetOriginal().WasCancelled());
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
	const GGYGOPlayerComboTestLifecycle::FActivation ActivationA = Fixture.ActivateFromInputForTest(*this);
	OriginalForScopeCleanup = ActivationA.OriginalActivation;
	if (!TestTrue(TEXT("纠正接收路径初始激活"), ActivationA.bNativeAccepted)) { return false; }
	if (!TestTrue(TEXT("A 受控激活请求 Accepted/None"), ActivationA.bNativeAccepted)
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
	const GGYGOPlayerComboTestLifecycle::FActivation ActivationB = Fixture.ActivateFromInputForTest(*this);
	OriginalForScopeCleanup = ActivationB.OriginalActivation;
	if (!TestTrue(TEXT("A 完成后受控 B 请求 Accepted/None"), ActivationB.bNativeAccepted
		&& ActivationB.bNativeAccepted)
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

	/** InvalidRequiredGE 在实际请求准入被拒后恢复合法 GE 走正常命中；作者配置不在激活中途改动。 */
	enum class EComboRuntimeHitCase : uint8 { NoGE, ValidGE, InvalidRequiredGE, InvalidRequiredSpec,
		Multiplier, ConflictingDamageKey };

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
			// 生产上下文：Instigator 为 ASC Owner（Host），EffectCauser 为 Avatar。
			AActor* const SourceOwner = Fixture.ASC->GetOwner();
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
				[this, SourceOwner, SourceAvatar, CueTag](AActor* Actor, FGameplayTag Tag, EGameplayCueEvent::Type Event,
					const FGameplayCueParameters& Parameters, EGameplayCueExecutionOptions)
				{
					if (Actor == Target && Tag == CueTag && Event == EGameplayCueEvent::Executed)
					{
						++CueCount;
						bObservedSourcesMatch &= Parameters.Instigator.Get() == SourceOwner
							&& Parameters.EffectCauser.Get() == SourceAvatar;
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
		GGYGOPlayerComboTestLifecycle::FEnded Notice;
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
			CompletedHandle = GGYGOPlayerComboTestLifecycle::ObserveEnded(Fixture.ASC, [this](const GGYGOPlayerComboTestLifecycle::FEnded& InNotice)
				{
					++CompletedCount;
					CompletedOrder = ObservationOrder;
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
			Fixture.ASC->OnAbilityEnded.Remove(CompletedHandle);
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
				&& Notice.GetOriginal().GetOriginalActivation().HasSameActivation(Original)
				&& Notice.GetOriginal().WasCancelled() == bFaultEnd);
			bPassed &= Test.TestEqual(TEXT("真实 hit GA 原生结束先广播"), AbilityEndOrder, 1);
			bPassed &= Test.TestEqual(TEXT("真实 hit ASC 原生结束随后广播"), EndOrder, 2);
			bPassed &= Test.TestTrue(TEXT("真实 hit ASC 广播时 GA 回调已退出"), bEndAfterAbilityEndExit);
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
		const bool bMultiplier = Case == EComboRuntimeHitCase::Multiplier || Case == EComboRuntimeHitCase::ConflictingDamageKey;
		const bool bMultiplierFault = Case == EComboRuntimeHitCase::ConflictingDamageKey;
		const bool bUncancelableFault = Case == EComboRuntimeHitCase::InvalidRequiredSpec || bMultiplierFault;
		FGGYGOComboLifecycleTestWorld TestWorld(bMultiplier);
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
			Test.TestTrue(TEXT("真实 hit 退出清理正式原 End 被接受"), Result.Outcome == EGGYGOAbilityTerminationOutcome::Accepted
				&& Fixture.Ability->GetLastTerminationForTest().GetOriginalActivation().HasSameActivation(CleanupOriginal));
			Test.TestTrue(TEXT("真实 hit 退出清理恢复实际已持资源"), AreRuntimeResourcesRestored(Fixture, Resources, bHadPrerequisite));
		};
		const FGameplayTag CueTag = TAG_PlayerComboRuntimeHitCue.GetTag();
		if (!Probe.Initialize(Test, Fixture, CueTag)) { return false; }
		const bool bNoGE = Case == EComboRuntimeHitCase::NoGE;
		TStrongObjectPtr<UGGYGOHealthSet> TargetHealth;
		if (bMultiplier)
		{
			if (!Test.TestNotNull(TEXT("倍率结算原生GameInstance"), TestWorld.GameInstance.Get())
				|| !Test.TestTrue(TEXT("倍率结算World持有原GameInstance"), Fixture.World->GetGameInstance() == TestWorld.GameInstance.Get())
				|| !Test.TestNotNull(TEXT("倍率结算原生MessageRouter"),
					UGameInstance::GetSubsystem<UGameplayMessageSubsystem>(TestWorld.GameInstance.Get()))) { return false; }
			// Host 自带唯一 CombatSet（生产宿主）；经 ASC 公开入口设基础伤害，不另加同类属性集。
			if (!Test.TestNotNull(TEXT("倍率专项Source 宿主 CombatSet"), Fixture.ASC->GetSet<UGGYGOCombatSet>())) { return false; }
			Fixture.ASC->SetNumericAttributeBase(UGGYGOCombatSet::GetBaseDamageAttribute(), 10.0f);
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
			if (TargetHealth.IsValid()) { Probe.TargetASC->RemoveSpawnedAttribute(TargetHealth.Get()); }
		};
		const TSubclassOf<UGameplayEffect> DamageClass = bNoGE ? TSubclassOf<UGameplayEffect>()
			: bMultiplier ? TSubclassOf<UGameplayEffect>(UGGYGOPlayerComboLifecycleMultiplierEffect::StaticClass())
			: TSubclassOf<UGameplayEffect>(UGameplayEffect::StaticClass());
		if (Case == EComboRuntimeHitCase::InvalidRequiredGE)
		{
			// 非法必需 GE 由实际请求的准入拒绝并按 GE 依赖类别报告一次，不创建激活。
			const UClass* const InvalidClass = UGGYGOPlayerComboLifecycleInvalidDamageEffect::StaticClass();
			Fixture.ConfigureRuntimeHitDamage(UGGYGOPlayerComboLifecycleInvalidDamageEffect::StaticClass(), CueTag);
			Test.AddExpectedMessagePlain(FString::Printf(
				TEXT("LogGGYGOAbilitySystem: PlayerCombo [%s] 激活失败时伤害GE依赖无效：模式=Override，配置=DamageEffect [%s]：已选GE类为抽象、已弃用或已被新版本替换。"),
				*Fixture.Ability->GetPathName(), *InvalidClass->GetPathName()),
				ELogVerbosity::Error, EAutomationExpectedMessageFlags::Exact, 1);
			const GGYGOPlayerComboTestLifecycle::FActivation Rejected =
				GGYGOPlayerComboTestLifecycle::Activate(Fixture.ASC, Fixture.AbilityHandle);
			if (!Test.TestTrue(TEXT("非法必需 GE 在实际请求准入被拒且没有激活历史"), !Rejected.bNativeAccepted
				&& !Rejected.OriginalActivation.HasActivation() && !Fixture.Ability->IsActive()
				&& Probe.AppliedCount == 0 && Probe.CueCount == 0)) { return false; }
		}
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
		const GGYGOPlayerComboTestLifecycle::FActivation Activation = Fixture.ActivateFromInputForTest(Test);
		CleanupOriginal = Activation.OriginalActivation;
		if (!Test.TestTrue(TEXT("真实 hit 受控本地 Original Accepted"), Activation.bNativeAccepted
			&& Activation.bNativeAccepted
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
				Probe.bObservedMultiplier
				&& Fixture.ASC->GetNumericAttribute(UGGYGOCombatSet::GetBaseDamageAttribute()) == 10.0f
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
		const bool bFault = Case == EComboRuntimeHitCase::InvalidRequiredSpec || bMultiplierFault;
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
				// 冲突键来自运行期 AbilitySpec 的 SetByCaller，不是作者配置。
				FGameplayAbilitySpec* AbilitySpec = Fixture.ASC->FindAbilitySpecFromHandle(Fixture.AbilityHandle);
				if (!Test.TestNotNull(TEXT("冲突来自真实原AbilitySpec的SetByCaller"), AbilitySpec)) { return false; }
				AbilitySpec->SetByCallerTagMagnitudes.Add(GGYGOGameplayTags::SetByCaller_Damage, 7.0f);
				ExpectedFaultDiagnostics.Add(RuntimeDiagnostic(TEXT("ComboDamage: conflicting-damage-keys-or-multiplier")));
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
			bPassed &= Test.TestTrue(TEXT("真实 hit 正常对照原 End 请求被接受"), EndResult.Outcome == EGGYGOAbilityTerminationOutcome::Accepted
				&& EndObservation.Notice.GetOriginal().GetOriginalActivation().HasSameActivation(CleanupOriginal));
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
	// 缺失倍率由 Multiplier 用例的纯准入负例覆盖；作者配置不在激活中途改动。
	return RunComboRuntimeHitCase(*this, EComboRuntimeHitCase::Multiplier)
		&& RunComboRuntimeHitCase(*this, EComboRuntimeHitCase::ConflictingDamageKey);
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
	enum class EComboEndAttackProbe : uint8 { None, Restart, Natural, AuthoredRestart };
	enum class EComboLocomotionObservation : uint8 { Immediate, RunHoldAndRelease };
	enum class EComboFXProbe : uint8 { None, NaturalTail, WindowCancel, EarlyCancelRestart };
	const FName FXNormal01System(TEXT("NS_Pyrios_Normal01_MainComposition_2a7c90937cab"));
	const FName FXNormal01BrightLayers[2] = { TEXT("M00_root_Trail_rot_HighLight"), TEXT("M00_root_Trail_rot_Shining") };
	const FName FXNormal01ControlLayer(TEXT("M00_root_Trail_rot_light_edge"));
	const TCHAR* const FXDrawSamplePhases[] = { TEXT("birth"), TEXT("middle"), TEXT("fade") };

	struct FComboFXMeshDrawObservation
	{
		TStrongObjectPtr<UObject> Probe;
		TSharedPtr<FJsonObject> Result;
		FName ArmIssue;
		bool bConfirmed = false;
	};

	struct FComboFXBrightDrawView final : FSceneViewExtensionBase
	{
		FComboFXBrightDrawView(const FAutoRegister& Register, UWorld* InWorld, FViewport* InViewport, APawn* InPawn, int32 InPlayerIndex)
			: FSceneViewExtensionBase(Register), World(InWorld), Viewport(InViewport), Pawn(InPawn), PlayerIndex(InPlayerIndex) {}

		bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override
		{
			return Context.Viewport == Viewport && World.IsValid() && Context.GetWorld() == World.Get() && Pawn.IsValid();
		}

		void BeginRenderViewFamily(FSceneViewFamily& Family) override
		{
			if (!IsInGameThread() || DrawFrame != GFrameCounter || !Family.bIsMainViewFamily || Family.RenderTarget != Viewport
				|| !Family.Scene || !World.IsValid() || Family.Scene->GetWorld() != World.Get() || !Pawn.IsValid()) { return; }
			bMatchedViewportFamily = true;
			for (const FSceneView* View : Family.Views)
			{
				if (View && View->StereoPass == EStereoscopicPass::eSSP_FULL && View->PlayerIndex == PlayerIndex
					&& View->ViewActor.ActorUniqueId == Pawn->GetUniqueID())
				{
					++Matches;
					WorldToClip = View->ViewMatrices.GetViewProjectionMatrix();
					Rect = View->UnscaledViewRect;
				}
			}
		}

		bool HasView() const
		{
			return DrawFrame == GFrameCounter && Matches == 1 && Rect.Width() > 0 && Rect.Height() > 0 && !WorldToClip.ContainsNaN();
		}

		TWeakObjectPtr<UWorld> World;
		FViewport* const Viewport;
		TWeakObjectPtr<APawn> Pawn;
		const int32 PlayerIndex;
		uint64 DrawFrame = MAX_uint64;
		int32 Matches = 0;
		bool bMatchedViewportFamily = false;
		FMatrix WorldToClip = FMatrix::Identity;
		FIntRect Rect;
	};

	TArray<TSharedPtr<FJsonValue>> FXDrawNumbers(std::initializer_list<double> Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (double Value : Values) { Result.Add(MakeShared<FJsonValueNumber>(Value)); }
		return Result;
	}

	void WriteFXDrawVector4(FJsonObject& Particle, const TCHAR* Field, const FVector4f& Value)
	{
		const bool bFinite = FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y)
			&& FMath::IsFinite(Value.Z) && FMath::IsFinite(Value.W);
		Particle.SetStringField(FString(Field) + TEXT("_coverage"), bFinite ? TEXT("PublishedCPU") : TEXT("NonFinite"));
		if (bFinite) { Particle.SetArrayField(Field, FXDrawNumbers({Value.X, Value.Y, Value.Z, Value.W})); }
	}

	TSharedPtr<FJsonObject> ReadFXMeshDrawView(const UStruct* Type, const void* Value)
	{
		const auto View = MakeShared<FJsonObject>();
		const TCHAR* const RectNames[] = {TEXT("UnscaledViewRect"), TEXT("RenderViewRect")};
		const TCHAR* const RectFields[] = {TEXT("unscaled_view_rect"), TEXT("render_view_rect")};
		for (int32 RectIndex = 0; RectIndex < UE_ARRAY_COUNT(RectNames); ++RectIndex)
		{
			const auto* Rect = FindFProperty<FStructProperty>(Type, RectNames[RectIndex]);
			if (!Rect) { return nullptr; }
			const void* RectValue = Rect->ContainerPtrToValuePtr<void>(Value);
			int32 Coordinates[4];
			int32 CoordinateIndex = 0;
			for (const TCHAR* Corner : {TEXT("Min"), TEXT("Max")})
			{
				const auto* Point = FindFProperty<FStructProperty>(Rect->Struct, Corner);
				const auto* X = Point ? FindFProperty<FIntProperty>(Point->Struct, TEXT("X")) : nullptr;
				const auto* Y = Point ? FindFProperty<FIntProperty>(Point->Struct, TEXT("Y")) : nullptr;
				if (!X || !Y) { return nullptr; }
				const void* PointValue = Point->ContainerPtrToValuePtr<void>(RectValue);
				Coordinates[CoordinateIndex++] = X->GetPropertyValue_InContainer(PointValue);
				Coordinates[CoordinateIndex++] = Y->GetPropertyValue_InContainer(PointValue);
			}
			if (Coordinates[2] <= Coordinates[0] || Coordinates[3] <= Coordinates[1]) { return nullptr; }
			View->SetArrayField(RectFields[RectIndex], FXDrawNumbers({
				static_cast<double>(Coordinates[0]), static_cast<double>(Coordinates[1]),
				static_cast<double>(Coordinates[2]), static_cast<double>(Coordinates[3])}));
		}
		const auto* Matrix = FindFProperty<FArrayProperty>(Type, TEXT("ViewProjectionMatrix"));
		const auto* Element = Matrix ? CastField<FDoubleProperty>(Matrix->Inner) : nullptr;
		if (!Element) { return nullptr; }
		FScriptArrayHelper Array(Matrix, Matrix->ContainerPtrToValuePtr<void>(Value));
		if (Array.Num() != 16) { return nullptr; }
		TArray<TSharedPtr<FJsonValue>> Entries;
		for (int32 Index = 0; Index < Array.Num(); ++Index)
		{
			const double Number = Element->GetPropertyValue(Array.GetRawPtr(Index));
			if (!FMath::IsFinite(Number)) { return nullptr; }
			Entries.Add(MakeShared<FJsonValueNumber>(Number));
		}
		View->SetArrayField(TEXT("view_projection_matrix_row_major"), MoveTemp(Entries));
		View->SetStringField(TEXT("coverage"), TEXT("Covered"));
		return View;
	}

	const UNiagaraMeshRendererProperties* FindFXDrawMeshRenderer(const FNiagaraEmitterInstance& Emitter)
	{
		const FVersionedNiagaraEmitterData* EmitterData = Emitter.GetVersionedEmitter().GetEmitterData();
		if (!EmitterData) { return nullptr; }
		const UNiagaraMeshRendererProperties* Renderer = nullptr;
		for (const UNiagaraRendererProperties* Candidate : EmitterData->GetRenderers())
		{
			if (!Candidate || !Candidate->GetIsEnabled()) { continue; }
			if (Renderer) { return nullptr; }
			Renderer = Cast<UNiagaraMeshRendererProperties>(Candidate);
			if (!Renderer) { return nullptr; }
		}
		return Renderer;
	}

	const UStaticMesh* FindFXDrawConfiguredMesh(const UNiagaraMeshRendererProperties& Renderer)
	{
		if (Renderer.MeshesBinding.ResolvedParameter.IsValid() || Renderer.Meshes.Num() != 1
			|| Renderer.Meshes[0].MeshParameterBinding.ResolvedParameter.IsValid())
		{
			return nullptr;
		}
		return Renderer.Meshes[0].Mesh.Get();
	}

	UMaterialInterface* ReadFXDrawConfiguredMaterial(const UNiagaraMeshRendererProperties& Renderer,
		const FNiagaraEmitterInstance& Emitter, const UStaticMesh& Mesh, const FStaticMeshRenderData& MeshData)
	{
		if (MeshData.LODResources.Num() != 1 || MeshData.LODResources[0].Sections.Num() != 1
			|| MeshData.LODResources[0].Sections[0].MaterialIndex != 0)
		{
			return nullptr;
		}
		TArray<UMaterialInterface*> Materials{Mesh.GetMaterial(0)};
		Renderer.ApplyMaterialOverrides(&Emitter, Materials);
		return Materials[0];
	}

	TSharedPtr<FJsonObject> ReadFXMeshDrawCounts(const UStruct* Type, const void* Value)
	{
		const auto* MaterialPath = FindFProperty<FStrProperty>(Type, TEXT("MaterialPath"));
		const auto* MeshBatches = FindFProperty<FIntProperty>(Type, TEXT("MeshBatches"));
		const auto* PassCandidates = FindFProperty<FMapProperty>(Type, TEXT("PassCandidates"));
		const auto* PassName = PassCandidates ? CastField<FStrProperty>(PassCandidates->KeyProp) : nullptr;
		const auto* PassCount = PassCandidates ? CastField<FIntProperty>(PassCandidates->ValueProp) : nullptr;
		if (!MaterialPath || !MeshBatches || !PassName || !PassCount) { return nullptr; }

		const int32 Count = MeshBatches->GetPropertyValue_InContainer(Value);
		if (Count < 0) { return nullptr; }
		const auto Counts = MakeShared<FJsonObject>();
		const auto Passes = MakeShared<FJsonObject>();
		const FString Path = MaterialPath->GetPropertyValue_InContainer(Value);
		if (!Path.IsEmpty()) { Counts->SetStringField(TEXT("material_path"), Path); }
		Counts->SetNumberField(TEXT("mesh_batches"), Count);
		FScriptMapHelper Map(PassCandidates, PassCandidates->ContainerPtrToValuePtr<void>(Value));
		for (int32 Index = 0; Index < Map.GetMaxIndex(); ++Index)
		{
			if (Map.IsValidIndex(Index))
			{
				Passes->SetNumberField(PassName->GetPropertyValue(Map.GetKeyPtr(Index)),
					PassCount->GetPropertyValue(Map.GetValuePtr(Index)));
			}
		}
		Counts->SetObjectField(TEXT("pass_candidates"), Passes);
		return Counts;
	}

	TSharedPtr<FJsonObject> ReadFXMeshDrawRTProxy(const UStruct* Type, const void* Value)
	{
		const auto* ProxyName = FindFProperty<FStrProperty>(Type, TEXT("ProxyName"));
		const auto* ResourceStatus = FindFProperty<FNameProperty>(Type, TEXT("ResourceStatus"));
		const auto* BlendMode = FindFProperty<FIntProperty>(Type, TEXT("BlendMode"));
		const auto* MaterialDomain = FindFProperty<FIntProperty>(Type, TEXT("MaterialDomain"));
		const auto* AfterDOF = FindFProperty<FBoolProperty>(Type, TEXT("bAfterDOF"));
		const auto* AfterMotionBlur = FindFProperty<FBoolProperty>(Type, TEXT("bAfterMotionBlur"));
		const auto* ShaderMapComplete = FindFProperty<FBoolProperty>(Type, TEXT("bShaderMapComplete"));
		if (!ProxyName || !ResourceStatus || !BlendMode || !MaterialDomain || !AfterDOF || !AfterMotionBlur || !ShaderMapComplete)
		{
			return nullptr;
		}

		const FName Status = ResourceStatus->GetPropertyValue_InContainer(Value);
		if (Status.IsNone()) { return nullptr; }
		const auto Proxy = MakeShared<FJsonObject>();
		Proxy->SetStringField(TEXT("proxy_name"), ProxyName->GetPropertyValue_InContainer(Value));
		Proxy->SetStringField(TEXT("resource_status"), Status.ToString());
		if (Status != TEXT("NoMaterialFromProxy"))
		{
			const int32 Blend = BlendMode->GetPropertyValue_InContainer(Value);
			const int32 Domain = MaterialDomain->GetPropertyValue_InContainer(Value);
			if (Blend == INDEX_NONE || Domain == INDEX_NONE) { return nullptr; }
			Proxy->SetNumberField(TEXT("blend_mode"), Blend);
			Proxy->SetNumberField(TEXT("material_domain"), Domain);
			Proxy->SetBoolField(TEXT("after_dof"), AfterDOF->GetPropertyValue_InContainer(Value));
			Proxy->SetBoolField(TEXT("after_motion_blur"), AfterMotionBlur->GetPropertyValue_InContainer(Value));
			if (Status != TEXT("NoShaderMap"))
			{
				Proxy->SetBoolField(TEXT("shader_map_complete"), ShaderMapComplete->GetPropertyValue_InContainer(Value));
			}
		}
		return Proxy;
	}

	TSharedPtr<FJsonObject> ReadFXMeshDrawRTMaterial(const UStruct* Type, const void* Value)
	{
		const auto* BatchStatus = FindFProperty<FNameProperty>(Type, TEXT("BatchStatus"));
		const FName Status = BatchStatus ? BatchStatus->GetPropertyValue_InContainer(Value) : NAME_None;
		if (Status.IsNone()) { return nullptr; }
		const auto Report = MakeShared<FJsonObject>();
		Report->SetStringField(TEXT("batch_status"), Status.ToString());
		if (Status != TEXT("Read"))
		{
			Report->SetStringField(TEXT("coverage"), TEXT("Uncovered"));
			return Report;
		}

		const auto* FeatureLevel = FindFProperty<FIntProperty>(Type, TEXT("FeatureLevel"));
		const auto* ShaderPlatform = FindFProperty<FIntProperty>(Type, TEXT("ShaderPlatform"));
		const auto* VertexFactoryType = FindFProperty<FNameProperty>(Type, TEXT("VertexFactoryType"));
		const auto* UseForMaterial = FindFProperty<FBoolProperty>(Type, TEXT("bUseForMaterial"));
		const auto* SupportsGPUScene = FindFProperty<FBoolProperty>(Type, TEXT("bVertexFactorySupportsGPUScene"));
		const auto* Original = FindFProperty<FStructProperty>(Type, TEXT("Original"));
		const auto* FallbackCandidates = FindFProperty<FArrayProperty>(Type, TEXT("FallbackCandidates"));
		const auto* FallbackType = FallbackCandidates ? CastField<FStructProperty>(FallbackCandidates->Inner) : nullptr;
		const auto* FallbackStatus = FindFProperty<FNameProperty>(Type, TEXT("FallbackStatus"));
		const auto* ShaderKeyCoverage = FindFProperty<FNameProperty>(Type, TEXT("ShaderKeyCoverage"));
		if (!FeatureLevel || !ShaderPlatform || !VertexFactoryType || !UseForMaterial || !SupportsGPUScene
			|| !Original || !FallbackType || !FallbackStatus || !ShaderKeyCoverage)
		{
			return nullptr;
		}

		const auto OriginalProxy = ReadFXMeshDrawRTProxy(Original->Struct, Original->ContainerPtrToValuePtr<void>(Value));
		if (!OriginalProxy) { return nullptr; }
		FScriptArrayHelper FallbackArray(FallbackCandidates, FallbackCandidates->ContainerPtrToValuePtr<void>(Value));
		TArray<TSharedPtr<FJsonValue>> Fallbacks;
		for (int32 Index = 0; Index < FallbackArray.Num(); ++Index)
		{
			const auto Proxy = ReadFXMeshDrawRTProxy(FallbackType->Struct, FallbackArray.GetRawPtr(Index));
			if (!Proxy) { return nullptr; }
			Fallbacks.Add(MakeShared<FJsonValueObject>(Proxy));
		}

		Report->SetStringField(TEXT("coverage"), TEXT("Covered"));
		Report->SetNumberField(TEXT("feature_level"), FeatureLevel->GetPropertyValue_InContainer(Value));
		Report->SetNumberField(TEXT("shader_platform"), ShaderPlatform->GetPropertyValue_InContainer(Value));
		Report->SetBoolField(TEXT("use_for_material"), UseForMaterial->GetPropertyValue_InContainer(Value));
		const FName FactoryType = VertexFactoryType->GetPropertyValue_InContainer(Value);
		if (!FactoryType.IsNone())
		{
			Report->SetStringField(TEXT("vertex_factory_type"), FactoryType.ToString());
			Report->SetBoolField(TEXT("vertex_factory_supports_gpu_scene"), SupportsGPUScene->GetPropertyValue_InContainer(Value));
		}
		Report->SetObjectField(TEXT("original"), OriginalProxy);
		Report->SetArrayField(TEXT("fallback_candidates"), MoveTemp(Fallbacks));
		Report->SetStringField(TEXT("fallback_status"), FallbackStatus->GetPropertyValue_InContainer(Value).ToString());
		Report->SetStringField(TEXT("shader_key_coverage"), ShaderKeyCoverage->GetPropertyValue_InContainer(Value).ToString());
		return Report;
	}

	TSharedPtr<FJsonObject> ReadFXDrawRendererAdmissionGT(const FNiagaraEmitterInstance& Emitter,
		const FNiagaraDataBuffer& Data, EShaderPlatform Platform)
	{
		const auto Report = MakeShared<FJsonObject>();
		Report->SetStringField(TEXT("emitter_id"), Emitter.GetEmitterHandle().GetId().ToString());
		Report->SetNumberField(TEXT("particles"), Data.GetNumInstances());
		Report->SetBoolField(TEXT("waiting_for_pso"), Emitter.IsWaitingForPSOCaching());
		const UNiagaraMeshRendererProperties* Renderer = FindFXDrawMeshRenderer(Emitter);
		Report->SetStringField(TEXT("submitted_material_coverage"), TEXT("uncovered_rt_submission"));
		if (!Renderer)
		{
			Report->SetStringField(TEXT("mesh_coverage"), TEXT("uncovered_renderer"));
			return Report;
		}
		Report->SetStringField(TEXT("renderer"), Renderer->GetPathName());
		Report->SetBoolField(TEXT("platform_active"), Renderer->GetIsActive());
		const auto& Store = Emitter.GetRendererBoundVariables();
		const auto& Enabled = Renderer->RendererEnabledBinding.GetParamMapBindableVariable();
		Report->SetStringField(TEXT("enabled_binding"), Enabled.GetName().ToString());
		if (const uint8* Value = Enabled.IsValid() ? Store.GetParameterData(Enabled) : nullptr)
		{
			FNiagaraBool Bound;
			FMemory::Memcpy(&Bound, Value, sizeof(Bound));
			Report->SetBoolField(TEXT("enabled_bound"), Bound.GetValue());
		}

		const auto& Visibility = Renderer->RendererVisibilityTagBinding;
		Report->SetNumberField(TEXT("visibility_expected"), Renderer->RendererVisibility);
		if (Visibility.CanBindToHostParameterMap())
		{
			if (const uint8* Value = Store.GetParameterData(Visibility.GetParamMapBindableVariable()))
			{
				int32 Bound = 0;
				FMemory::Memcpy(&Bound, Value, sizeof(Bound));
				Report->SetNumberField(TEXT("visibility_host_value"), Bound);
			}
		}
		else
		{
			const auto Tags = FNiagaraDataSetAccessor<int32>::CreateReader(&Data, Visibility.GetDataSetBindableVariable().GetName());
			if (Tags.IsValid())
			{
				int32 Matches = 0;
				for (uint32 Index = 0; Index < Data.GetNumInstances(); ++Index)
				{
					Matches += uint32(Tags[Index]) == Renderer->RendererVisibility;
				}
				Report->SetNumberField(TEXT("visibility_particle_matches"), Matches);
			}
		}

		const UNiagaraRendererProperties* RendererProperties = Renderer;
		Report->SetBoolField(TEXT("needs_runtime_mids"), RendererProperties->NeedsMIDsForMaterials());
		const UStaticMesh* Mesh = FindFXDrawConfiguredMesh(*Renderer);
		if (!Mesh)
		{
			Report->SetStringField(TEXT("mesh_coverage"), TEXT("uncovered_static_mesh_configuration"));
			return Report;
		}
		Report->SetStringField(TEXT("configured_mesh"), Mesh->GetPathName());
		if (Mesh->IsCompiling())
		{
			Report->SetStringField(TEXT("mesh_coverage"), TEXT("uncovered_mesh_compiling"));
			return Report;
		}

		const FStaticMeshRenderData* MeshData = Mesh->GetRenderData();
		if (!MeshData || !MeshData->LODResources.IsValidIndex(0))
		{
			Report->SetStringField(TEXT("mesh_coverage"), TEXT("uncovered_mesh_render_data"));
			return Report;
		}
		if (MeshData->HasValidNaniteData()
			&& (Platform == EShaderPlatform::SP_NumPlatforms || !MeshData->HasNaniteFallbackMesh(Platform)))
		{
			Report->SetStringField(TEXT("mesh_coverage"), TEXT("uncovered_nanite_fallback"));
			return Report;
		}

		Report->SetStringField(TEXT("mesh_coverage"), TEXT("configured_static_mesh_gt"));
		Report->SetNumberField(TEXT("min_lod"), Mesh->GetMinLODIdx());
		// 驻留来自 GT 缓存，LOD0 Section 来自资产，两者独立于本 Draw 的 RT 选 LOD。
		const FStreamableRenderResourceState StreamingState = Mesh->GetStreamableResourceState();
		if (StreamingState.IsValid() && StreamingState.NumResidentLODs > 0)
		{
			Report->SetNumberField(TEXT("gt_resident_lods"), StreamingState.NumResidentLODs);
			Report->SetNumberField(TEXT("gt_requested_lods"), StreamingState.NumRequestedLODs);
			Report->SetNumberField(TEXT("gt_first_resident_asset_lod"),
				StreamingState.LODCountToAssetFirstLODIdx(StreamingState.NumResidentLODs));
		}
		const FStaticMeshLODResources& LOD0 = MeshData->LODResources[0];
		TArray<TSharedPtr<FJsonValue>> Sections;
		for (const FStaticMeshSection& Section : LOD0.Sections)
		{
			const auto Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("material_slot"), Section.MaterialIndex);
			Item->SetNumberField(TEXT("triangles"), Section.NumTriangles);
			Sections.Add(MakeShared<FJsonValueObject>(Item));
		}
		Report->SetArrayField(TEXT("lod0_sections"), MoveTemp(Sections));

		const UMaterialInterface* ConfiguredMaterial = ReadFXDrawConfiguredMaterial(*Renderer, Emitter, *Mesh, *MeshData);
		if (IsValid(ConfiguredMaterial))
		{
			const UMaterialInterface& Material = *ConfiguredMaterial;
			Report->SetStringField(TEXT("configured_material"), Material.GetPathName());
			Report->SetBoolField(TEXT("niagara_mesh_usage"), Material.GetUsageByFlag(MATUSAGE_NiagaraMeshParticles));
			const FMaterialResource* Resource = Platform != EShaderPlatform::SP_NumPlatforms ? Material.GetMaterialResource(Platform) : nullptr;
			const FMaterialShaderMap* Map = Resource ? Resource->GetGameThreadShaderMap() : nullptr;
			if (Resource)
			{
				Report->SetNumberField(TEXT("gt_blend_mode"), Resource->GetBlendMode());
				Report->SetBoolField(TEXT("gt_after_dof"), Resource->IsTranslucencyAfterDOFEnabled());
				Report->SetBoolField(TEXT("gt_after_motion_blur"), Resource->IsTranslucencyAfterMotionBlurEnabled());
			}
			Report->SetBoolField(TEXT("gt_shader_map"), Map != nullptr);
			const bool bMapComplete = Resource && Resource->IsGameThreadShaderMapComplete();
			Report->SetBoolField(TEXT("gt_shader_map_complete"), bMapComplete);
			if (Map && bMapComplete)
			{
				static const FHashedName MeshVF(TEXT("FNiagaraMeshVertexFactory"));
				Report->SetBoolField(TEXT("gt_niagara_mesh_vf_shader_map"), Map->GetMeshShaderMap(MeshVF) != nullptr);
			}
		}
		return Report;
	}

	const TCHAR* ProjectFXDrawMesh(const FComboFXBrightDrawView* View, const FNiagaraEmitterInstance& Emitter,
		const FNiagaraDataBuffer* Data, int32 Index, const FTransform& ComponentToWorld,
		const FVector& Position, const FQuat& Orientation, const FVector& Scale, FJsonObject& Particle)
	{
		if (!View || !View->HasView()) { return TEXT("uncovered_view"); }
		const UNiagaraMeshRendererProperties* Renderer = FindFXDrawMeshRenderer(Emitter);
		if (!Emitter.IsLocalSpace() || !Renderer || Renderer->SourceMode != ENiagaraRendererSourceDataMode::Particles
			|| Renderer->FacingMode != ENiagaraMeshFacingMode::Default || Renderer->bLockedAxisEnable
			|| Renderer->Meshes.Num() != 1 || Renderer->MeshesBinding.ResolvedParameter.IsValid())
		{
			return TEXT("uncovered_renderer_transform");
		}
		const FNiagaraMeshRendererMeshProperties& Mesh = Renderer->Meshes[0];
		const auto MatchesParticleBinding = [](const FNiagaraVariableAttributeBinding& Binding, FName Name)
		{
			return !Binding.CanBindToHostParameterMap() && Binding.GetDataSetBindableVariable().GetName() == Name;
		};
		if (!Mesh.Mesh || Mesh.MeshParameterBinding.ResolvedParameter.IsValid()
			|| (Mesh.PivotOffsetSpace != ENiagaraMeshPivotOffsetSpace::Mesh && !Mesh.PivotOffset.IsZero())
			|| !MatchesParticleBinding(Renderer->PositionBinding, TEXT("Position"))
			|| !MatchesParticleBinding(Renderer->MeshOrientationBinding, TEXT("MeshOrientation"))
			|| !MatchesParticleBinding(Renderer->ScaleBinding, TEXT("Scale"))
			|| Renderer->CameraOffsetBinding.CanBindToHostParameterMap() || Renderer->MeshIndexBinding.CanBindToHostParameterMap())
		{
			return TEXT("uncovered_renderer_transform");
		}
		const auto CameraOffset = FNiagaraDataSetAccessor<float>::CreateReader(Data, Renderer->CameraOffsetBinding.GetDataSetBindableVariable().GetName());
		const auto MeshIndex = FNiagaraDataSetAccessor<int32>::CreateReader(Data, Renderer->MeshIndexBinding.GetDataSetBindableVariable().GetName());
		if ((CameraOffset.IsValid() && CameraOffset[Index] != 0.0f) || (MeshIndex.IsValid() && MeshIndex[Index] != 0))
		{
			return TEXT("uncovered_renderer_transform");
		}
		const FBox Box = Mesh.Mesh->GetBoundingBox();
		if (!Box.IsValid || Box.Min.ContainsNaN() || Box.Max.ContainsNaN() || Mesh.Rotation.ContainsNaN()
			|| Mesh.Scale.ContainsNaN() || Mesh.PivotOffset.ContainsNaN() || ComponentToWorld.ContainsNaN())
		{
			return TEXT("uncovered_renderer_transform");
		}
		const FQuat MeshRotation = Orientation.GetNormalized() * Mesh.Rotation.Quaternion();
		const FVector MeshScale = Scale * Mesh.Scale;
		const FVector MeshOrigin = Position + MeshRotation.RotateVector(Mesh.PivotOffset * Scale);
		const FMatrix MeshToWorld = FTransform(MeshRotation, MeshOrigin, MeshScale).ToMatrixWithScale() * ComponentToWorld.ToMatrixWithScale();
		if (MeshToWorld.ContainsNaN()) { return TEXT("uncovered_renderer_transform"); }

		TArray<TSharedPtr<FJsonValue>> Matrix;
		for (int32 Row = 0; Row < 4; ++Row)
		{
			Matrix.Append(FXDrawNumbers({MeshToWorld.M[Row][0], MeshToWorld.M[Row][1], MeshToWorld.M[Row][2], MeshToWorld.M[Row][3]}));
		}
		Particle.SetStringField(TEXT("mesh"), Mesh.Mesh->GetPathName());
		Particle.SetArrayField(TEXT("mesh_to_world_pre_wpo_row_major"), MoveTemp(Matrix));
		FBox2D Pixels(ForceInit);
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector Point((Corner & 1) ? Box.Max.X : Box.Min.X, (Corner & 2) ? Box.Max.Y : Box.Min.Y, (Corner & 4) ? Box.Max.Z : Box.Min.Z);
			FVector2D Pixel;
			if (!FSceneView::ProjectWorldToScreen(FVector(MeshToWorld.TransformPosition(Point)), View->Rect, View->WorldToClip, Pixel)
				|| Pixel.ContainsNaN()) { return TEXT("uncovered_clip"); }
			Pixels += Pixel;
		}
		const FBox2D ViewPixels(FVector2D(View->Rect.Min.X, View->Rect.Min.Y), FVector2D(View->Rect.Max.X, View->Rect.Max.Y));
		Particle.SetArrayField(TEXT("mesh_bounds_pixels"), FXDrawNumbers({Pixels.Min.X, Pixels.Min.Y, Pixels.Max.X, Pixels.Max.Y}));
		Particle.SetBoolField(TEXT("mesh_bounds_intersects_view"), Pixels.Intersect(ViewPixels));
		return TEXT("pre_wpo_mesh_bounds");
	}

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
			// NewPress 的 End 等待先核对原 PIE 世界，再分别检查游戏时间与墙钟。
			if ((Stage != EStage::End || bHeldDuringMain) && FPlatformTime::Seconds() >= Deadline)
			{
				FString Reason = TEXT("bounded stage timeout; original input/section/cancellation/recovery condition not observed");
				if (Stage == EStage::Locomotion || Stage == EStage::RunHold || Stage == EStage::RunRelease)
				{
					LogLocomotionSnapshot(TEXT("LocomotionTimeout"));
				}
				else if (Stage == EStage::FXStartupFrame)
				{
					LogNativeInputBoundary(TEXT("ColdStartupFrameTimeoutBeforeInput"));
					LogFXStartupReadiness(TEXT("ColdStartupFrameTimeoutBeforeInput"));
				}
				else if (Stage == EStage::Player && !InputResourceWaitReason.IsEmpty())
				{
					Test.AddInfo(TEXT("[Combat.PlayerCombo.ProductionSmoke] InputResourcesNotReadyBeforeFirstPress ") + InputResourceWaitReason);
				}
				else if (FXProbe != EComboFXProbe::None && !bFXVerified
					&& (Stage == EStage::Natural || Stage == EStage::NaturalIdle))
				{
					Reason = TEXT("original FX authored End/tail observation timed out: ") + DescribeFXTailEvidence();
				}
				Fail(Reason);
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
			RunHold, RunRelease, FXCancelWait, FXStartupFrame };
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
			// 所有生产叶都沿原启动截止等待生产资源 Ready；Pending 期间的按下按生产契约被 Hero 丢弃且不重放，
			// 进程内首个 PIE 若提前按下，后续阶段永远等不到激活。首按仍只经原 Hero 输入链提交一次。
			if (!Extension->AreAbilityInputResourcesReady(GGYGOGameplayTags::InputTag_Attack_Light, InputResourceWaitReason)) { return; }
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
			if (PoseProbe == EComboPoseFailureProbe::None
				&& (bHeldDuringMain || (EndAttackProbe != EComboEndAttackProbe::None && !IsAuthoredRestartProbe())))
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
			if (PoseProbe == EComboPoseFailureProbe::None)
			{
				ActivatedHandle = ProductionASC->AbilityActivatedCallbacks.AddLambda([this](UGameplayAbility* Activated)
				{
					if (const UGGYGOGameplayAbility* ProjectAbility = Cast<UGGYGOGameplayAbility>(Activated))
					{
						LastActivated = ProjectAbility->CaptureCurrentActivation();
					}
				});
				CompletedHandle = ProductionASC->OnAbilityEnded.AddLambda([this](const FAbilityEndedData& Data)
				{
					GGYGOPlayerComboTestLifecycle::FEnded Notice;
					Notice.Original = FGGYGOAbilityTerminationContext(LastActivated, Data.bWasCancelled);
					if (OriginalActivation.HasActivation() && Notice.GetOriginal().GetOriginalActivation().HasSameActivation(OriginalActivation))
					{
						++CompletedCount; Completion = Notice;
					}
				});
			}
			Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.EndProductionSmoke] NativeSlateRoute Map=%s GameMode=%s PC=%s Pawn=%s GA=%s Source=%s Origin=%s InputConfig=%s AttackKey=%s Scope=%llu. Automated native events; no HID/network/Cook claim."),
				*EditorMap, *GetPathNameSafe(World->GetAuthGameMode()), *PC->GetPathName(), *Pawn->GetPathName(), *Combo->GetPathName(),
				*Source->GetPathName(), *Origin->GetPathName(), *Config->GetPathName(), *AttackKey.ToString(), static_cast<unsigned long long>(Scope.GetScopeSerial())));
			Stage = EStage::Main;
			if (PoseProbe == EComboPoseFailureProbe::StartupMissingDeclaration) { Stage = EStage::PoseWaitFailure; }
			Deadline = FPlatformTime::Seconds() + 10.0;
			if (FXProbe != EComboFXProbe::None)
			{
				if (!InstallFXFrameObservation()) { return; }
				InstallNewPressInputObservation(AttackAction, AttackKeys);
				Stage = EStage::FXStartupFrame;
				Deadline = FPlatformTime::Seconds() + 30.0;
				return;
			}
			if (IsAuthoredRestartProbe() || (!bHeldDuringMain && LastMovementStep > 0
				&& PoseProbe == EComboPoseFailureProbe::None && EndAttackProbe == EComboEndAttackProbe::None))
			{
				InstallNewPressInputObservation(AttackAction, AttackKeys);
				if (IsAuthoredRestartProbe() && (!NewPressViewportKeyHandle.IsValid() || NewPressAttackObservationHandles.Num() != 3))
				{
					Fail(TEXT("原包 03→01 缺少原生攻击 Action 观察，未覆盖。")); return;
				}
			}
			bAttackDown = true;
			SendNativeKey(AttackKey, true);
		}
		bool InstallFXFrameObservation()
		{
			if (!World.IsValid() || !World->PersistentLevel || !Viewport.IsValid() || !SceneViewport || !Mesh.IsValid() || !Movement.IsValid()
				|| !Mesh->PrimaryComponentTick.IsTickFunctionRegistered() || !Mesh->PrimaryComponentTick.IsTickFunctionEnabled()
				|| !Movement->PrimaryComponentTick.IsTickFunctionRegistered() || !Movement->PrimaryComponentTick.IsTickFunctionEnabled())
			{
				Fail(TEXT("原 FX 帧观察缺少已注册的生产 Mesh／CMC／Level／Viewport。")); return false;
			}
			FXFrameObservationTick.Command = this;
			FXFrameObservationTick.AddPrerequisite(Mesh.Get(), Mesh->PrimaryComponentTick);
			FXFrameObservationTick.AddPrerequisite(Movement.Get(), Movement->PrimaryComponentTick);
			FXFrameObservationTick.RegisterTickFunction(World->PersistentLevel);
			if (!FXFrameObservationTick.IsTickFunctionRegistered())
			{
				ReleaseFXFrameObservation(); Fail(TEXT("原 FX 帧观察注册失败。")); return false;
			}
			FXBeginDrawHandle = Viewport->OnBeginDraw().AddLambda([this]
			{
				bFXDrawInProgress = true;
				bFXDrawSawWorldEndOfFrame = false;
				if (FXBrightDrawView)
				{
					FXBrightDrawView->DrawFrame = GFrameCounter;
					FXBrightDrawView->Matches = 0;
					FXBrightDrawView->bMatchedViewportFamily = false;
				}
			});
			FXWorldEndOfFrameHandle = FWorldDelegates::OnWorldPreSendAllEndOfFrameUpdates.AddLambda([this](UWorld* DrawWorld)
			{
				if (bFXDrawInProgress && DrawWorld == World.Get()) { bFXDrawSawWorldEndOfFrame = true; }
			});
			if (FXProbe == EComboFXProbe::NaturalTail && GetDefault<UGGYGOPlayerComboFXDrawTestBridge>()->OnBrightDraw.IsBound())
			{
				FXBrightDrawView = FSceneViewExtensions::NewExtension<FComboFXBrightDrawView>(
					World.Get(), static_cast<FViewport*>(SceneViewport), Character.Get(), LocalPlayer->GetControllerId());
				FXBrightDrawnHandle = Viewport->OnDrawn().AddRaw(this, &FComboEndProductionPIECommand::ObserveFXOriginalDrawn);
			}
			FXViewportRenderedHandle = UGameViewportClient::OnViewportRendered().AddRaw(this, &FComboEndProductionPIECommand::ObserveFXViewportRendered);
			return true;
		}
		void ReleaseFXFrameObservation()
		{
			PollFXMeshDraw();
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(FXDrawSamplePhases); ++Index)
			{
				if (FXMeshDrawSamples[Index].Result)
				{
					FinishFXMeshDraw(FXMeshDrawSamples[Index], TEXT("Uncovered"), TEXT("CancelledBeforeTerminal"));
				}
				if (FXBrightDrawView && !bFXBrightDrawConsumed[Index])
				{
					Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.FXBrightDraw] SampleCoverage={\"sample_index\":%d,\"sample_phase\":\"%s\",\"coverage\":\"Uncovered\",\"reason\":\"NaturalPhaseNotObserved\"}"),
						Index, FXDrawSamplePhases[Index]));
				}
			}
			CancelFXMeshDraw(FXMeshDrawCandidate);
			if (Viewport.IsValid())
			{
				Viewport->OnBeginDraw().Remove(FXBeginDrawHandle);
				Viewport->OnDrawn().Remove(FXBrightDrawnHandle);
			}
			FXBrightDrawnHandle.Reset();
			FXBrightDrawView.Reset();
			FWorldDelegates::OnWorldPreSendAllEndOfFrameUpdates.Remove(FXWorldEndOfFrameHandle);
			UGameViewportClient::OnViewportRendered().Remove(FXViewportRenderedHandle);
			FXBeginDrawHandle.Reset();
			FXWorldEndOfFrameHandle.Reset();
			FXViewportRenderedHandle.Reset();
			bFXDrawInProgress = false;
			bFXDrawSawWorldEndOfFrame = false;
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
			if (Stage == EStage::FXStartupFrame)
			{
				for (const FFXUnitObservation& Unit : FXUnits)
				{
					if (!Unit.System.IsValid())
					{
						LogFXStartupReadiness(TEXT("ColdStartupSystemUnavailableBeforeInput"));
						Fail(TEXT("原 FX 单元在原生启动等待中失效。")); return;
					}
				}
				if (FXStartupFrame == MAX_uint64)
				{
					FXStartupFrame = GFrameCounter;
					FXStartupWorldSeconds = World->GetTimeSeconds();
					LogNativeInputBoundary(TEXT("ColdStartupFrameObserved"));
					return;
				}
				if (GFrameCounter <= FXStartupFrame || World->GetTimeSeconds() <= FXStartupWorldSeconds) { return; }
				// 原生帧已推进。就绪值仅用于诊断；让真实冷首击进入生产 FX 的准入与失败路径。
				Stage = EStage::Main; Deadline = FPlatformTime::Seconds() + 3.0;
				if (!InstallFXInputObservation()) { return; }
				LogFXStartupReadiness(TEXT("BeforeColdAttackKeyDown"));
				LogNativeInputBoundary(TEXT("BeforeColdAttackKeyDown"));
				bAttackDown = true;
				const bool bSlateHandled = SendNativeKey(AttackKey, true);
				if (!bFailed)
				{
					// Slate 的处理结果不等于 Controller 已消费输入。
					LogNativeInputBoundary(bSlateHandled ? TEXT("ColdAttackKeyDownSlateHandled") : TEXT("ColdAttackKeyDownSlateUnhandled"));
				}
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
		void LogFXStartupReadiness(const TCHAR* Boundary)
		{
			for (int32 Index = 0; Index < FXUnits.Num(); ++Index)
			{
				const UNiagaraSystem* System = FXUnits[Index].System.Get();
				if (!System)
				{
					Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.FXProductionSmoke] ReadinessBoundary=%s Frame=%llu Unit=%d SystemAvailable=0."),
						Boundary, static_cast<unsigned long long>(GFrameCounter), Index));
					continue;
				}
				const UNiagaraScript* Spawn = System->GetSystemSpawnScript();
				const UNiagaraScript* Update = System->GetSystemUpdateScript();
				Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.FXProductionSmoke] ReadinessBoundary=%s Frame=%llu Unit=%d System=%s SystemAvailable=1 NiagaraReady=%d NiagaraValid=%d OutstandingCompilationIncludingGPU=%d SpawnScriptPending=%d UpdateScriptPending=%d EmitterHandles=%d CompiledEmitters=%d."),
					Boundary, static_cast<unsigned long long>(GFrameCounter), Index, *System->GetPathName(),
					System->IsReadyToRun(), System->IsValid(), System->HasOutstandingCompilationRequests(true),
					Spawn ? static_cast<int32>(Spawn->IsScriptCompilationPending(false)) : INDEX_NONE,
					Update ? static_cast<int32>(Update->IsScriptCompilationPending(false)) : INDEX_NONE,
					System->GetEmitterHandles().Num(), System->GetEmitterCompiledData().Num()));
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
			if (IsAuthoredRestartProbe()) { ObserveAuthoredRestartAfterController(); return; }
			if (!bFailed && Stage == EStage::Main)
			{
				if (!IsProductionChainCurrent()) { Fail(TEXT("original production chain changed at native FX input boundary")); }
				else
				{
					if (!bLoggedColdFXControllerSample)
					{
						bLoggedColdFXControllerSample = true;
						LogNativeInputBoundary(TEXT("ColdAfterOriginalControllerTick"));
					}
					TryCaptureMain();
				}
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
				|| Step.FX.Mode != EGGYGOComboFXMode::MontageWindow
				|| Step.FX.BeginEventTag != GGYGOGameplayTags::Event_Montage_FXWindowBegin
				|| Step.FX.EndEventTag != GGYGOGameplayTags::Event_Montage_FXWindowEnd
				|| Step.FX.Space != EGGYGONiagaraEffectSpace::Attached || !Step.FX.SocketName.IsNone()
				|| !Step.FX.LocalTransform.Equals(FTransform::Identity)
				|| Step.FX.NormalStopMode != EGGYGONiagaraEffectStopMode::DeactivateDetachKeepWorld
				|| Step.FX.AbortStopMode != EGGYGONiagaraEffectStopMode::DestroyImmediately)
			{
				Fail(TEXT("生产首段完整 FX 配置未安装：") + Error); return false;
			}
			TArray<FGGYGONiagaraEffectUnit> Units = Step.FX.SystemUnits;
			if (Units.IsEmpty())
			{
				FGGYGONiagaraEffectUnit Unit;
				Unit.System = Step.FX.System;
				Unit.FieldSettings = Step.FX.FieldSettings;
				Units.Add(MoveTemp(Unit));
			}
			TSet<UNiagaraSystem*> Systems;
			for (const FGGYGONiagaraEffectUnit& Unit : Units)
			{
				if (!IsValid(Unit.System) || Unit.System->GetEmitterHandles().IsEmpty() || Systems.Contains(Unit.System))
				{
					Fail(TEXT("生产 FX 单元缺少真实 emitter 或重复使用同一 System 身份。")); return false;
				}
				Systems.Add(Unit.System);
				FFXUnitObservation& Observation = FXUnits.AddDefaulted_GetRef();
				Observation.System = Unit.System.Get();
				Observation.EmitterCount = Unit.System->GetEmitterHandles().Num();
			}
			const int32 MainIndex = Step.Montage->GetSectionIndex(Step.MainSection);
			const int32 EndIndex = Step.Montage->GetSectionIndex(Step.EndSection);
			if (MainIndex == INDEX_NONE || EndIndex == INDEX_NONE)
			{
				Fail(TEXT("生产 FX 探针缺少原 Main／End Section。")); return false;
			}
			const float MainBegin = Step.Montage->GetAnimCompositeSection(MainIndex).GetTime();
			const float MainEnd = Step.Montage->GetAnimCompositeSection(EndIndex).GetTime();
			FGGYGOMontageWindowDeclaration Declaration;
			if (!UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolveOriginalMontageWindowDeclaration(
				Step.Montage, Step.FX.BeginEventTag, Step.FX.EndEventTag, MainBegin, MainEnd, Declaration, Error)
				|| !Declaration.bQueued || !Declaration.bNoMergeOnConcurrentPlay
				|| !FMath::IsNearlyEqual(Declaration.BeginSeconds, MainBegin)
				|| !FMath::IsNearlyEqual(Declaration.EndSeconds, MainEnd))
			{
				Fail(TEXT("生产 FX 原作者窗未完整覆盖真实 Main 或缺少 Queued／NoMerge：") + Error); return false;
			}
			FXWindowBegin = Declaration.BeginSeconds;
			FXWindowEnd = Declaration.EndSeconds;
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
					else if (Tag == GGYGOGameplayTags::Event_Montage_FXWindowEnd)
					{
						const int32 Count = ++FXEndCounts.FindOrAdd(Snapshot.MontageInstanceId);
						if (Count == 1 && bFXCaptured && Task == MontageTask.Get() && Snapshot.MontageInstanceId == InstanceId)
						{
							// 在原作者 End 观察点确认哪些原单元已经自然退休。
							for (FFXUnitObservation& Unit : FXUnits)
							{
								Unit.bRetiredNaturallyBeforeEnd = Unit.NaturalCompletionCount == 1 && Unit.IsRetired();
							}
						}
						Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.FXProductionSmoke] Boundary=AuthoredEnd Instance=%d Task=%s Frame=%llu Position=%.6f Count=%d."),
							Snapshot.MontageInstanceId, *GetPathNameSafe(Task), static_cast<unsigned long long>(GFrameCounter), Snapshot.PositionSeconds, Count));
					}
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
		bool FXComponentsRetired() const
		{
			return !FXUnits.ContainsByPredicate([](const FFXUnitObservation& Unit) { return !Unit.IsRetired(); });
		}
		bool FXSimulationsCaptured() const
		{
			return !FXUnits.ContainsByPredicate([](const FFXUnitObservation& Unit) { return !Unit.bSimulationCaptured; });
		}
		bool FXComponentsAttached() const
		{
			return !FXUnits.ContainsByPredicate([this](const FFXUnitObservation& Unit)
			{
				if (Unit.NaturalCompletionCount == 1) { return !Unit.IsRetired(); }
				return !Unit.Component.IsValid() || Unit.Component->GetAttachParent() != Mesh.Get()
					|| Unit.Component->GetWorld() != World.Get();
			});
		}
		bool FXAllNaturallyCompleted() const
		{
			return !FXUnits.ContainsByPredicate([](const FFXUnitObservation& Unit)
			{
				return Unit.NaturalCompletionCount != 1 || !Unit.IsRetired();
			});
		}
		void ObserveFXNaturalCompletion(UGGYGOAbilityTask_PlayNiagaraEffect* Sender, UNiagaraComponent* Component)
		{
			if (Sender != FXTask.Get()) { return; }
			for (FFXUnitObservation& Unit : FXUnits)
			{
				if (Component && Unit.Component.Get(true) == Component)
				{
					++Unit.NaturalCompletionCount;
					Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.FXProductionSmoke] Boundary=OwnedNaturalCompletion Instance=%d Task=%s Component=%s Frame=%llu AuthoredEndCount=%d Count=%d."),
						InstanceId, *GetPathNameSafe(Sender), *GetPathNameSafe(Component), static_cast<unsigned long long>(GFrameCounter),
						FXEndCounts.FindRef(InstanceId), Unit.NaturalCompletionCount));
					return;
				}
			}
			bFXUnknownNaturalCompletion = true;
		}
		FString DescribeFXTailEvidence() const
		{
			int32 NaturallyCompleted = 0, RetiredNaturallyBeforeEnd = 0, DetachedInactive = 0, Retired = 0;
			for (const FFXUnitObservation& Unit : FXUnits)
			{
				NaturallyCompleted += Unit.NaturalCompletionCount == 1 ? 1 : 0;
				RetiredNaturallyBeforeEnd += Unit.bRetiredNaturallyBeforeEnd ? 1 : 0;
				DetachedInactive += Unit.bDetached ? 1 : 0;
				Retired += bFXCaptured && Unit.IsRetired() ? 1 : 0;
			}
			return FString::Printf(TEXT("Begin=%d End=%d Captured=%d Particles=%d AllSimulations=%d NaturallyCompleted=%d RetiredNaturallyBeforeEnd=%d DetachedInactive=%d Retired=%d Units=%d TaskFinished=%d TaskReleased=%d."),
				FXBeginCounts.FindRef(InstanceId), FXEndCounts.FindRef(InstanceId), bFXCaptured, bFXParticles, FXSimulationsCaptured(),
				NaturallyCompleted, RetiredNaturallyBeforeEnd, DetachedInactive, Retired, FXUnits.Num(),
				FXTask.Get() && FXTask->GetState() == EGameplayTaskState::Finished,
				FGGYGOPlayerComboLifecycleFixture::GetProductionFXTask(Ability.Get()) == nullptr);
		}
		void CancelFXMeshDraw(FComboFXMeshDrawObservation& Observation)
		{
			if (UObject* Probe = Observation.Probe.Get())
			{
				if (UFunction* Cancel = Probe->FindFunction(TEXT("Cancel"))) { Probe->ProcessEvent(Cancel, nullptr); }
				Observation.Probe.Reset();
			}
		}

		void ArmFXMeshDraw()
		{
			// 上一次 Draw 未被原亮层样本选中。每个候选只观察下一次原 Draw。
			CancelFXMeshDraw(FXMeshDrawCandidate);
			FXMeshDrawCandidate = {};
			FXMeshDrawCandidate.ArmIssue = TEXT("MissingOriginalNiagaraInstance");
			const FFXUnitObservation* Unit = FXUnits.FindByPredicate([](const FFXUnitObservation& Entry)
			{
				return Entry.System.IsValid() && Entry.System->GetFName() == FXNormal01System;
			});
			UNiagaraComponent* Component = Unit ? Unit->Component.Get() : nullptr;
			const auto NiagaraController = Component ? Component->GetSystemInstanceController() : nullptr;
			if (!NiagaraController || !NiagaraController->IsValid()) { return; }
			const FNiagaraSystemInstance* Instance = NiagaraController->GetSystemInstance_Unsafe();
			if (!Instance || Instance->GetSystem() != Unit->System.Get()) { return; }
			if (bFXBrightDrawConsumed[0] && static_cast<uint64>(Instance->GetId()) != FXBrightDrawNiagaraId) { return; }

			FXMeshDrawCandidate.ArmIssue = TEXT("MissingEditorProbe");
			UClass* ProbeClass = FindObject<UClass>(nullptr, TEXT("/Script/GGYGOEditor.GGYGONiagaraMeshDrawTestProbe"));
			if (!ProbeClass) { return; }
			UFunction* Begin = ProbeClass->FindFunctionByName(TEXT("BeginNextDraw"));
			const auto* PlayerArg = Begin ? FindFProperty<FObjectProperty>(Begin, TEXT("Player")) : nullptr;
			const auto* PawnArg = Begin ? FindFProperty<FObjectProperty>(Begin, TEXT("ViewPawn")) : nullptr;
			const auto* ComponentArg = Begin ? FindFProperty<FObjectProperty>(Begin, TEXT("Component")) : nullptr;
			const auto* MaterialsArg = Begin ? FindFProperty<FArrayProperty>(Begin, TEXT("Materials")) : nullptr;
			const auto* MaterialArg = MaterialsArg ? CastField<FObjectProperty>(MaterialsArg->Inner) : nullptr;
			const auto* Return = Begin ? CastField<FObjectProperty>(Begin->GetReturnProperty()) : nullptr;
			FXMeshDrawCandidate.ArmIssue = TEXT("InvalidBeginNextDrawContract");
			if (!PlayerArg || !PawnArg || !ComponentArg || !MaterialArg || !Return) { return; }

			TArray<UMaterialInterface*> Materials;
			for (FName Layer : {FXNormal01BrightLayers[0], FXNormal01BrightLayers[1], FXNormal01ControlLayer})
			{
				const FNiagaraEmitterInstanceRef* Match = Instance->GetEmitters().FindByPredicate(
					[Layer](const FNiagaraEmitterInstanceRef& Emitter) { return Emitter->GetEmitterHandle().GetName() == Layer; });
				const UNiagaraMeshRendererProperties* Renderer = Match ? FindFXDrawMeshRenderer(Match->Get()) : nullptr;
				const UStaticMesh* ConfiguredMesh = Renderer ? FindFXDrawConfiguredMesh(*Renderer) : nullptr;
				const FStaticMeshRenderData* MeshData = ConfiguredMesh && !ConfiguredMesh->IsCompiling()
					? ConfiguredMesh->GetRenderData() : nullptr;
				UMaterialInterface* Material = MeshData
					? ReadFXDrawConfiguredMaterial(*Renderer, Match->Get(), *ConfiguredMesh, *MeshData) : nullptr;
				if (!IsValid(Material))
				{
					FXMeshDrawCandidate.ArmIssue = TEXT("MissingConfiguredMaterial");
					return;
				}
				Materials.Add(Material);
			}

			FStructOnScope Params(Begin);
			void* Args = Params.GetStructMemory();
			PlayerArg->SetObjectPropertyValue_InContainer(Args, LocalPlayer.Get());
			PawnArg->SetObjectPropertyValue_InContainer(Args, Character.Get());
			ComponentArg->SetObjectPropertyValue_InContainer(Args, Component);
			FScriptArrayHelper MaterialArgs(MaterialsArg, MaterialsArg->ContainerPtrToValuePtr<void>(Args));
			MaterialArgs.AddValues(Materials.Num());
			for (int32 Index = 0; Index < Materials.Num(); ++Index)
			{
				MaterialArg->SetObjectPropertyValue(MaterialArgs.GetRawPtr(Index), Materials[Index]);
			}
			ProbeClass->GetDefaultObject()->ProcessEvent(Begin, Args);
			FXMeshDrawCandidate.Probe.Reset(Return->GetObjectPropertyValue_InContainer(Args));
			FXMeshDrawCandidate.ArmIssue = FXMeshDrawCandidate.Probe.Get() ? NAME_None : FName(TEXT("BeginNextDrawReturnedNull"));
		}

		void ConfirmFXMeshDraw(int32 SampleIndex, UNiagaraComponent& Component, FJsonObject& Draw)
		{
			FComboFXMeshDrawObservation& Observation = FXMeshDrawSamples[SampleIndex];
			Observation = MoveTemp(FXMeshDrawCandidate);
			FXMeshDrawCandidate = {};
			Observation.Result = MakeShared<FJsonObject>();
			for (const TCHAR* Field : {TEXT("draw_frame"), TEXT("world_path"), TEXT("world_object_id"), TEXT("viewport_identity"),
				TEXT("game_viewport_client_object_id"), TEXT("montage_task"), TEXT("montage_instance_id"),
				TEXT("fx_task"), TEXT("component"), TEXT("system"), TEXT("niagara_id"), TEXT("niagara_tick"), TEXT("sample_index"), TEXT("sample_phase")})
			{
				Observation.Result->SetField(Field, Draw.Values.FindChecked(Field));
			}

			UObject* Probe = Observation.Probe.Get();
			UFunction* Confirm = Probe ? Probe->FindFunction(TEXT("ConfirmOriginalDraw")) : nullptr;
			const auto* ComponentArg = Confirm ? FindFProperty<FObjectProperty>(Confirm, TEXT("Component")) : nullptr;
			const auto* Return = Confirm ? CastField<FBoolProperty>(Confirm->GetReturnProperty()) : nullptr;
			if (ComponentArg && Return)
			{
				FStructOnScope Params(Confirm);
				void* Args = Params.GetStructMemory();
				ComponentArg->SetObjectPropertyValue_InContainer(Args, &Component);
				Probe->ProcessEvent(Confirm, Args);
				Observation.bConfirmed = Return->GetPropertyValue_InContainer(Args);
				Observation.ArmIssue = Observation.bConfirmed ? NAME_None : FName(TEXT("OriginalDrawNotConfirmed"));
			}
			else if (Probe)
			{
				Observation.ArmIssue = TEXT("InvalidConfirmOriginalDrawContract");
			}
			Draw.SetStringField(TEXT("mesh_draw_probe"), Observation.bConfirmed ? TEXT("Pending") : TEXT("Uncovered"));
		}

		void FinishFXMeshDraw(FComboFXMeshDrawObservation& Observation, const TCHAR* Coverage, FName Reason)
		{
			Observation.Result->SetStringField(TEXT("coverage"), Coverage);
			if (!Reason.IsNone()) { Observation.Result->SetStringField(TEXT("reason"), Reason.ToString()); }
			FString Json;
			const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
			if (FJsonSerializer::Serialize(Observation.Result.ToSharedRef(), Writer))
			{
				Test.AddInfo(TEXT("[Combat.PlayerCombo.FXBrightDraw] MeshDrawResult=") + Json);
			}
			else
			{
				Test.AddInfo(TEXT("[Combat.PlayerCombo.FXBrightDraw] MeshDrawResult={\"coverage\":\"Uncovered\",\"reason\":\"ResultSerializationFailed\"}"));
			}
			Observation.Result.Reset();
			CancelFXMeshDraw(Observation);
		}

		void PollFXMeshDraw()
		{
			for (FComboFXMeshDrawObservation& Observation : FXMeshDrawSamples) { PollFXMeshDrawSample(Observation); }
		}

		void PollFXMeshDrawSample(FComboFXMeshDrawObservation& Observation)
		{
			if (!Observation.Result) { return; }
			UObject* Probe = Observation.Probe.Get();
			if (!Probe)
			{
				FinishFXMeshDraw(Observation, TEXT("Uncovered"), Observation.ArmIssue.IsNone() ? FName(TEXT("ProbeNotArmed")) : Observation.ArmIssue);
				return;
			}

			UFunction* Poll = Probe->FindFunction(TEXT("Poll"));
			const auto* Return = Poll ? CastField<FStructProperty>(Poll->GetReturnProperty()) : nullptr;
			const auto* Coverage = Return ? FindFProperty<FEnumProperty>(Return->Struct, TEXT("Coverage")) : nullptr;
			if (!Coverage)
			{
				FinishFXMeshDraw(Observation, TEXT("Uncovered"), TEXT("InvalidPollContract"));
				return;
			}
			FStructOnScope Params(Poll);
			void* Args = Params.GetStructMemory();
			Probe->ProcessEvent(Poll, Args);
			const void* Result = Return->ContainerPtrToValuePtr<void>(Args);
			const FString Status = Coverage->GetEnum()->GetNameStringByValue(
				Coverage->GetUnderlyingProperty()->GetSignedIntPropertyValue(Coverage->ContainerPtrToValuePtr<void>(Result)));
			if (Status == TEXT("Pending")) { return; }
			if (Status == TEXT("Uncovered"))
			{
				const auto* Reason = FindFProperty<FNameProperty>(Return->Struct, TEXT("Reason"));
				FinishFXMeshDraw(Observation, TEXT("Uncovered"), Reason ? Reason->GetPropertyValue_InContainer(Result) : Observation.ArmIssue);
				return;
			}
			if (Status != TEXT("Covered") || !Observation.bConfirmed || !ReadCoveredFXMeshDraw(Return->Struct, Result, *Observation.Result))
			{
				FinishFXMeshDraw(Observation, TEXT("Uncovered"), TEXT("InvalidCoveredDrawResult"));
				return;
			}
			FinishFXMeshDraw(Observation, TEXT("Covered"), NAME_None);
		}

		bool ReadCoveredFXMeshDraw(const UStruct* Type, const void* Value, FJsonObject& Report)
		{
			const auto ReadId = [Type, Value](FName Name)
			{
				const auto* Property = FindFProperty<FInt64Property>(Type, Name);
				return Property ? LexToString(Property->GetPropertyValue_InContainer(Value)) : FString();
			};
			const auto* ResultViewport = FindFProperty<FStrProperty>(Type, TEXT("ViewportIdentity"));
			const auto* ResultComponent = FindFProperty<FStrProperty>(Type, TEXT("ComponentPath"));
			if (!ResultViewport || !ResultComponent
				|| ReadId(TEXT("FrameCounter")) != Report.GetStringField(TEXT("draw_frame"))
				|| ReadId(TEXT("WorldObjectId")) != Report.GetStringField(TEXT("world_object_id"))
				|| ReadId(TEXT("NiagaraInstanceId")) != Report.GetStringField(TEXT("niagara_id"))
				|| ResultViewport->GetPropertyValue_InContainer(Value) != Report.GetStringField(TEXT("viewport_identity"))
				|| ResultComponent->GetPropertyValue_InContainer(Value) != Report.GetStringField(TEXT("component")))
			{
				return false;
			}

			const auto* ComponentBatches = FindFProperty<FIntProperty>(Type, TEXT("ComponentMeshBatches"));
			const auto* Materials = FindFProperty<FArrayProperty>(Type, TEXT("Materials"));
			const auto* BatchType = Materials ? CastField<FStructProperty>(Materials->Inner) : nullptr;
			const auto* Unattributed = FindFProperty<FStructProperty>(Type, TEXT("Unattributed"));
			if (!ComponentBatches || !BatchType || !Unattributed || ComponentBatches->GetPropertyValue_InContainer(Value) < 0) { return false; }
			FScriptArrayHelper Array(Materials, Materials->ContainerPtrToValuePtr<void>(Value));
			const FName Layers[] = {FXNormal01BrightLayers[0], FXNormal01BrightLayers[1], FXNormal01ControlLayer};
			if (Array.Num() != UE_ARRAY_COUNT(Layers)) { return false; }
			const auto* RTMaterial = FindFProperty<FStructProperty>(BatchType->Struct, TEXT("RTMaterial"));
			TArray<TSharedPtr<FJsonValue>> ByMaterial;
			for (int32 Index = 0; Index < Array.Num(); ++Index)
			{
				const void* Batch = Array.GetRawPtr(Index);
				const auto Counts = ReadFXMeshDrawCounts(BatchType->Struct, Batch);
				if (!Counts || !Counts->HasField(TEXT("material_path"))) { return false; }
				Counts->SetStringField(TEXT("configured_layer"), Layers[Index].ToString());
				auto Material = RTMaterial ? ReadFXMeshDrawRTMaterial(RTMaterial->Struct, RTMaterial->ContainerPtrToValuePtr<void>(Batch)) : nullptr;
				if (!Material)
				{
					Material = MakeShared<FJsonObject>();
					Material->SetStringField(TEXT("coverage"), TEXT("Uncovered"));
					Material->SetStringField(TEXT("reason"), RTMaterial ? TEXT("InvalidRTMaterialContract") : TEXT("MissingRTMaterialField"));
				}
				Counts->SetObjectField(TEXT("rt_material"), Material);
				ByMaterial.Add(MakeShared<FJsonValueObject>(Counts));
			}
			const auto Other = ReadFXMeshDrawCounts(Unattributed->Struct, Unattributed->ContainerPtrToValuePtr<void>(Value));
			if (!Other) { return false; }
			Report.SetStringField(TEXT("primitive_component_id"), ReadId(TEXT("PrimitiveComponentId")));
			Report.SetNumberField(TEXT("component_mesh_batches"), ComponentBatches->GetPropertyValue_InContainer(Value));
			Report.SetArrayField(TEXT("materials"), MoveTemp(ByMaterial));
			Report.SetObjectField(TEXT("unattributed"), Other);
			auto View = ReadFXMeshDrawView(Type, Value);
			if (!View)
			{
				View = MakeShared<FJsonObject>();
				View->SetStringField(TEXT("coverage"), TEXT("Uncovered"));
				View->SetStringField(TEXT("reason"), TEXT("MissingOrInvalidViewContract"));
			}
			Report.SetObjectField(TEXT("rt_view"), View);
			return true;
		}

		void ObserveFX()
		{
			PollFXMeshDraw();
			if (bFXVerified || bFailed) { return; }
			if (bFXUnknownNaturalCompletion || FXUnits.ContainsByPredicate([](const FFXUnitObservation& Unit)
				{ return Unit.NaturalCompletionCount > 1; }))
			{
				Fail(TEXT("组合 Task 的原自然完成事实重复或不属于已观察单元。")); return;
			}
			const int32 Begins = FXBeginCounts.FindRef(InstanceId), Ends = FXEndCounts.FindRef(InstanceId);
			if (Begins > 1 || Ends > 1) { Fail(TEXT("original native FX window duplicated Begin/End")); return; }
			if (!bFXCaptured)
			{
				TInlineComponentArray<UNiagaraComponent*> Components(Character.Get());
				for (FFXUnitObservation& Unit : FXUnits)
				{
					UNiagaraComponent* Found = nullptr;
					for (UNiagaraComponent* Component : Components)
					{
						if (!IsValid(Component) || Component->GetAsset() != Unit.System.Get()) { continue; }
						if (Found) { Fail(TEXT("原 Pawn 中同一 FX 单元出现重复组件。")); return; }
						Found = Component;
					}
					if (!Found)
					{
						const auto* OriginalFXTask = FGGYGOPlayerComboLifecycleFixture::GetProductionFXTask(Ability.Get());
						if (Ends != 0 || (OriginalFXTask && OriginalFXTask->GetState() == EGameplayTaskState::Finished))
						{
							Fail(TEXT("原 FX 单元在取得完整组件观测前已结束，不能据此签发动态覆盖：") + GetPathNameSafe(Unit.System.Get()));
						}
						return;
					}
					if (Begins != 1 || Ends != 0 || Found->GetAttachParent() != Mesh.Get() || Found->GetWorld() != World.Get())
					{
						Fail(TEXT("FX 单元未在真实原 Attached 作者窗内被观察到。")); return;
					}
					Unit.Component = Found;
				}
				auto* Task = FGGYGOPlayerComboLifecycleFixture::GetProductionFXTask(Ability.Get());
				if (!Task || Task->GetStartResult() != EGGYGONiagaraEffectStartResult::SpawnAndActivationRequested)
				{
					Fail(TEXT("original component has no accepted original FX Task request")); return;
				}
				FXTask.Reset(Task);
				FXNaturalCompletionHandle = Task->OnUnitNaturallyCompleted.AddLambda(
					[this](UGGYGOAbilityTask_PlayNiagaraEffect* Sender, UNiagaraComponent* Component)
					{ ObserveFXNaturalCompletion(Sender, Component); });
				bFXCaptured = true;
			}
			if (FXBrightDrawView && !bFXBrightDrawConsumed[2] && Stage == EStage::Natural && Begins == 1 && Ends == 0)
			{
				ArmFXMeshDraw();
			}
			if (FXProbe == EComboFXProbe::WindowCancel && !bFXRestarted)
			{
				if (!bFXParticles || !FXSimulationsCaptured()) { return; }
				FGGYGOMontageSectionSnapshot Snapshot;
				if (Ends != 0 || !ReadOriginalPlayback(Snapshot) || Snapshot.PositionSeconds >= FXWindowEnd
					|| !FXComponentsAttached() || FXComponentsRetired() || !FXTask.Get() || FXTask->GetState() == EGameplayTaskState::Finished)
				{
					Fail(TEXT("particle-bearing FX cancellation missed the real author window")); return;
				}
				CancelFXOriginal(); return;
			}
			if (FXProbe == EComboFXProbe::WindowCancel && bFXRestarted && Begins == 1 && Ends == 0
				&& FXCancelledEndFrame != MAX_uint64
				&& (!FXComponentsAttached() || !FXTask.Get()
					|| (FXTask->GetState() == EGameplayTaskState::Finished && !FXAllNaturallyCompleted())
					|| FGGYGOPlayerComboLifecycleFixture::GetProductionFXTask(Ability.Get()) != FXTask.Get()))
			{
				Fail(TEXT("old native FX End retired or detached the exact successor resource before its real authored End")); return;
			}
			if (Ends == 1)
			{
				for (FFXUnitObservation& Unit : FXUnits)
				{
					if (Unit.bRetiredNaturallyBeforeEnd) { continue; }
					UNiagaraComponent* Component = Unit.Component.Get();
					if (!Component || Unit.bDetached) { continue; }
					const auto NiagaraController = Component->GetSystemInstanceController();
					const bool bDetached = Component->GetAttachParent() == nullptr;
					const bool bInactive = NiagaraController.IsValid() && NiagaraController->IsValid()
						&& NiagaraController->GetRequestedExecutionState() == ENiagaraExecutionState::Inactive;
					const bool bReleased = FGGYGOPlayerComboLifecycleFixture::GetProductionFXTask(Ability.Get()) == nullptr;
					const bool bTaskFinished = FXTask.Get() && FXTask->GetState() == EGameplayTaskState::Finished;
					if (!bDetached || !bInactive || !bReleased || !bTaskFinished)
					{
						Fail(FString::Printf(TEXT("真实作者 End 后原单元缺少尾部事实。Component=%s Detached=%d Inactive=%d Released=%d TaskFinished=%d."),
							*GetPathNameSafe(Component), bDetached, bInactive, bReleased, bTaskFinished)); return;
					}
					Unit.bDetached = true;
					Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.FXProductionSmoke] Boundary=DetachedInactive Instance=%d Task=%s Component=%s Frame=%llu."),
						InstanceId, *GetPathNameSafe(FXTask.Get()), *GetPathNameSafe(Component), static_cast<unsigned long long>(GFrameCounter)));
				}
			}
			// 自然完成可早于 Montage 的作者 End。保留原播放观察直到收到 End 或到达原截止。
			if (Ends == 1 && FXComponentsRetired())
			{
				int32 NaturalRetiredBeforeEndUnits = 0, TailDetachedUnits = 0;
				for (const FFXUnitObservation& Unit : FXUnits)
				{
					if (Unit.bRetiredNaturallyBeforeEnd) { ++NaturalRetiredBeforeEndUnits; }
					else if (Unit.bDetached) { ++TailDetachedUnits; }
				}
				if (Begins != 1 || !bFXParticles || !FXSimulationsCaptured()
					|| NaturalRetiredBeforeEndUnits + TailDetachedUnits != FXUnits.Num()
					|| !FXTask.Get() || FXTask->GetState() != EGameplayTaskState::Finished
					|| FGGYGOPlayerComboLifecycleFixture::GetProductionFXTask(Ability.Get()))
				{
					Fail(TEXT("original FX authored End was observed but terminal proof is incomplete: ") + DescribeFXTailEvidence()); return;
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
				Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.FXProductionSmoke] Probe=%u Instance=%d Begin=1 End=1 ActualComponentRetired=1 OldEventsRejected=%d Units=%d NaturalRetiredBeforeEndUnits=%d TailDetachedUnits=%d LiveTailCoverage=%s."),
					static_cast<uint32>(FXProbe), InstanceId, FXRejectedOldEvents, FXUnits.Num(), NaturalRetiredBeforeEndUnits,
					TailDetachedUnits, TailDetachedUnits > 0 ? TEXT("OBSERVED") : TEXT("NOT_OBSERVED")));
			}
		}
		int32 SelectFXBrightDrawSample(const FNiagaraDataBuffer* const (&Buffers)[3]) const
		{
			// 首样仍取原先首个合格 Draw；后两样只消费真实粒子寿命窗，不等待或推进模拟。
			if (!bFXBrightDrawConsumed[0]) { return 0; }
			const float MinimumAge[] = {0.0f, 0.25f, 0.82f};
			const float MaximumAge[] = {1.0f, 0.65f, 1.0f};
			for (int32 SampleIndex = 1; SampleIndex < UE_ARRAY_COUNT(FXDrawSamplePhases); ++SampleIndex)
			{
				if (bFXBrightDrawConsumed[SampleIndex]) { continue; }
				bool bInPhase = true;
				for (int32 LayerIndex = 0; LayerIndex < 2 && bInPhase; ++LayerIndex)
				{
					const FNiagaraDataBuffer* Data = Buffers[LayerIndex];
					const auto Age = FNiagaraDataSetAccessor<float>::CreateReader(Data, TEXT("NormalizedAge"));
					if (!Age.IsValid()) { return INDEX_NONE; }
					for (int32 Index = 0; Index < static_cast<int32>(Data->GetNumInstances()); ++Index)
					{
						if (!FMath::IsFinite(Age[Index]) || Age[Index] < MinimumAge[SampleIndex] || Age[Index] >= MaximumAge[SampleIndex])
						{
							bInPhase = false; break;
						}
					}
				}
				if (bInPhase) { return SampleIndex; }
			}
			return INDEX_NONE;
		}

		void PublishFXBrightDraw(FViewport* DrawViewport, UNiagaraComponent& Component, const FNiagaraSystemInstance& NiagaraInstance,
			const FGGYGOMontageSectionSnapshot& Snapshot, int32 SampleIndex, const FName (&LayerNames)[3],
			const FNiagaraEmitterInstance* const (&Emitters)[3], const FNiagaraDataBuffer* const (&Buffers)[3])
		{
			const FTransform& Transform = Component.GetComponentTransform();
			const FVector Location = Transform.GetLocation();
			const FQuat Rotation = Transform.GetRotation();
			const FVector ComponentScale = Transform.GetScale3D();
			const TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
			Report->SetNumberField(TEXT("sample_index"), SampleIndex);
			Report->SetStringField(TEXT("sample_phase"), FXDrawSamplePhases[SampleIndex]);
			Report->SetStringField(TEXT("draw_frame"), LexToString(GFrameCounter));
			Report->SetStringField(TEXT("world_path"), World->GetPathName());
			Report->SetStringField(TEXT("world_object_id"), LexToString(World->GetUniqueID()));
			Report->SetStringField(TEXT("viewport_identity"), FString::Printf(TEXT("%p"), DrawViewport));
			Report->SetStringField(TEXT("game_viewport_client_object_id"), LexToString(Viewport->GetUniqueID()));
			Report->SetStringField(TEXT("montage_task"), MontageTask->GetPathName());
			Report->SetNumberField(TEXT("montage_instance_id"), InstanceId);
			Report->SetStringField(TEXT("fx_task"), FXTask->GetPathName());
			Report->SetStringField(TEXT("component"), Component.GetPathName());
			Report->SetStringField(TEXT("system"), NiagaraInstance.GetSystem()->GetPathName());
			Report->SetStringField(TEXT("niagara_id"), LexToString(NiagaraInstance.GetId()));
			Report->SetNumberField(TEXT("niagara_tick"), NiagaraInstance.GetTickCount());
			Report->SetNumberField(TEXT("system_age_seconds"), NiagaraInstance.GetAge());
			Report->SetStringField(TEXT("montage_section"), Snapshot.SectionName.ToString());
			Report->SetNumberField(TEXT("montage_position_seconds"), Snapshot.PositionSeconds);
			ConfirmFXMeshDraw(SampleIndex, Component, *Report);
			if (!Transform.ContainsNaN())
			{
				const auto WorldTransform = MakeShared<FJsonObject>();
				WorldTransform->SetArrayField(TEXT("position"), FXDrawNumbers({Location.X, Location.Y, Location.Z}));
				WorldTransform->SetArrayField(TEXT("rotation_xyzw"), FXDrawNumbers({Rotation.X, Rotation.Y, Rotation.Z, Rotation.W}));
				WorldTransform->SetArrayField(TEXT("scale"), FXDrawNumbers({ComponentScale.X, ComponentScale.Y, ComponentScale.Z}));
				Report->SetObjectField(TEXT("component_world_transform"), WorldTransform);
			}
			const FComboFXBrightDrawView* View = FXBrightDrawView.Get();
			if (View && View->HasView())
			{
				Report->SetArrayField(TEXT("view_rect"), FXDrawNumbers({
					static_cast<double>(View->Rect.Min.X), static_cast<double>(View->Rect.Min.Y),
					static_cast<double>(View->Rect.Max.X), static_cast<double>(View->Rect.Max.Y)}));
			}

			const EShaderPlatform Platform = World->Scene ? World->Scene->GetShaderPlatform() : EShaderPlatform::SP_NumPlatforms;
			Report->SetBoolField(TEXT("component_rendering_enabled_gt"), Component.GetRenderingEnabled());
			TArray<TSharedPtr<FJsonValue>> Layers;
			for (int32 LayerIndex = 0; LayerIndex < UE_ARRAY_COUNT(LayerNames); ++LayerIndex)
			{
				const FNiagaraDataBuffer* Data = Buffers[LayerIndex];
				const auto Layer = MakeShared<FJsonObject>();
				Layer->SetStringField(TEXT("name"), LayerNames[LayerIndex].ToString());
				if (!Emitters[LayerIndex] || !Data)
				{
					Layer->SetStringField(TEXT("coverage"), TEXT("Uncovered"));
					Layer->SetStringField(TEXT("reason"), TEXT("MissingPublishedCPUBuffer"));
					Layers.Add(MakeShared<FJsonValueObject>(Layer));
					continue;
				}
				const FNiagaraEmitterInstance& Emitter = *Emitters[LayerIndex];
				Layer->SetStringField(TEXT("emitter_id"), Emitter.GetEmitterHandle().GetId().ToString());
				Layer->SetStringField(TEXT("coverage"), TEXT("PublishedCPU"));
				const auto Admission = ReadFXDrawRendererAdmissionGT(Emitter, *Data, Platform);
				Layer->SetObjectField(TEXT("renderer_admission_gt"), Admission);
				if (LayerIndex == 2) { Report->SetObjectField(TEXT("light_edge_admission_gt"), Admission); }
				Layer->SetNumberField(TEXT("particle_count"), Data->GetNumInstances());
				Layer->SetStringField(TEXT("coordinate_space"), Emitter.IsLocalSpace() ? TEXT("local") : TEXT("world_lwc_simulation"));
				const auto Age = FNiagaraDataSetAccessor<float>::CreateReader(Data, TEXT("NormalizedAge"));
				const auto Color = FNiagaraDataSetAccessor<FLinearColor>::CreateReader(Data, TEXT("Color"));
				const FNiagaraDataSetReaderFloat<FVector4f> DynamicParameters[] = {
					FNiagaraDataSetAccessor<FVector4f>::CreateReader(Data, TEXT("DynamicMaterialParameter")),
					FNiagaraDataSetAccessor<FVector4f>::CreateReader(Data, TEXT("DynamicMaterialParameter1")),
					FNiagaraDataSetAccessor<FVector4f>::CreateReader(Data, TEXT("DynamicMaterialParameter2"))};
				const TCHAR* const DynamicFields[] = {
					TEXT("dynamic_material_parameter"), TEXT("dynamic_material_parameter1"), TEXT("dynamic_material_parameter2")};
				const auto Position = FNiagaraDataSetAccessor<FNiagaraPosition>::CreateReader(Data, TEXT("Position"));
				const auto Orientation = FNiagaraDataSetAccessor<FQuat4f>::CreateReader(Data, TEXT("MeshOrientation"));
				const auto Scale = FNiagaraDataSetAccessor<FVector3f>::CreateReader(Data, TEXT("Scale"));
				TArray<TSharedPtr<FJsonValue>> Particles;
				for (int32 Index = 0; Index < static_cast<int32>(Data->GetNumInstances()); ++Index)
				{
					const auto Particle = MakeShared<FJsonObject>();
					Particle->SetNumberField(TEXT("index"), Index);
					if (Age.IsValid() && FMath::IsFinite(Age[Index])) { Particle->SetNumberField(TEXT("normalized_age"), Age[Index]); }
					Particle->SetStringField(TEXT("normalized_age_coverage"), !Age.IsValid() ? TEXT("MissingAttribute")
						: FMath::IsFinite(Age[Index]) ? TEXT("PublishedCPU") : TEXT("NonFinite"));
					if (Color.IsValid() && FMath::IsFinite(Color[Index].A)) { Particle->SetNumberField(TEXT("color_alpha"), Color[Index].A); }
					if (Color.IsValid())
					{
						const FLinearColor Value = Color[Index];
						WriteFXDrawVector4(*Particle, TEXT("color_rgba"), FVector4f(Value.R, Value.G, Value.B, Value.A));
					}
					else { Particle->SetStringField(TEXT("color_rgba_coverage"), TEXT("MissingAttribute")); }
					for (int32 ParameterIndex = 0; ParameterIndex < UE_ARRAY_COUNT(DynamicParameters); ++ParameterIndex)
					{
						if (DynamicParameters[ParameterIndex].IsValid())
						{
							WriteFXDrawVector4(*Particle, DynamicFields[ParameterIndex], DynamicParameters[ParameterIndex][Index]);
						}
						else { Particle->SetStringField(FString(DynamicFields[ParameterIndex]) + TEXT("_coverage"), TEXT("MissingAttribute")); }
					}
					const FVector P = Position.IsValid() ? FVector(Position[Index]) : FVector::ZeroVector;
					const FQuat Q = Orientation.IsValid() ? FQuat(Orientation[Index]) : FQuat::Identity;
					const FVector S = Scale.IsValid() ? FVector(Scale[Index]) : FVector::ZeroVector;
					const bool bPosition = Position.IsValid() && !P.ContainsNaN();
					const bool bOrientation = Orientation.IsValid() && !Q.ContainsNaN() && FMath::IsFinite(Q.SizeSquared()) && Q.SizeSquared() > UE_SMALL_NUMBER;
					const bool bScale = Scale.IsValid() && !S.ContainsNaN();
					if (bPosition) { Particle->SetArrayField(TEXT("simulation_position"), FXDrawNumbers({P.X, P.Y, P.Z})); }
					if (bOrientation) { Particle->SetArrayField(TEXT("mesh_orientation_xyzw"), FXDrawNumbers({Q.X, Q.Y, Q.Z, Q.W})); }
					if (bScale) { Particle->SetArrayField(TEXT("scale"), FXDrawNumbers({S.X, S.Y, S.Z})); }
					Particle->SetStringField(TEXT("projection_status"), bPosition && bOrientation && bScale
						? ProjectFXDrawMesh(View, Emitter, Data, Index, Transform, P, Q, S, *Particle) : TEXT("uncovered_particle_pose"));
					Particles.Add(MakeShared<FJsonValueObject>(Particle));
				}
				Layer->SetArrayField(TEXT("particles"), MoveTemp(Particles));
				Layers.Add(MakeShared<FJsonValueObject>(Layer));
			}
			Report->SetArrayField(TEXT("layers"), MoveTemp(Layers));
			FString SampleJson;
			bFXBrightDrawConsumed[SampleIndex] = true;
			if (SampleIndex == 0) { FXBrightDrawNiagaraId = static_cast<uint64>(NiagaraInstance.GetId()); }
			if (!FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&SampleJson)))
			{
				FinishFXMeshDraw(FXMeshDrawSamples[SampleIndex], TEXT("Uncovered"), TEXT("OriginalDrawSerializationFailed"));
				return;
			}
			GetMutableDefault<UGGYGOPlayerComboFXDrawTestBridge>()->OnBrightDraw.Broadcast(World.Get(), SampleJson);
		}

		void ObserveFXOriginalDrawn()
		{
			if (!FXBrightDrawView || FXBrightDrawView->DrawFrame != GFrameCounter || !FXBrightDrawView->bMatchedViewportFamily
				|| bFXBrightDrawConsumed[2] || !bFXDrawInProgress || !bFXDrawSawWorldEndOfFrame || bFailed || bFXVerified || !bFXCaptured
				|| Stage != EStage::Natural || !GetDefault<UGGYGOPlayerComboFXDrawTestBridge>()->OnBrightDraw.IsBound()
				|| !IsProductionChainCurrent()) { return; }
			RecordFXBrightLayersForDraw(static_cast<FViewport*>(SceneViewport), true);
		}

		void RecordFXBrightLayersForDraw(FViewport* DrawViewport, bool bPublishBrightDraw = false)
		{
			if (FXProbe != EComboFXProbe::NaturalTail || !MontageTask.Get() || !FXTask.Get()
				|| !Ability->CaptureCurrentActivation().HasSameActivation(OriginalActivation)
				|| FGGYGOPlayerComboLifecycleFixture::GetProductionFXTask(Ability.Get()) != FXTask.Get()) { return; }

			FGGYGOMontageSectionSnapshot Snapshot;
			if (!MontageTask->TryGetOriginalSectionSnapshot(Snapshot) || Snapshot.Montage != Montage.Get()
				|| Snapshot.MontageInstanceId != InstanceId || Snapshot.SectionName != MainSection
				|| Snapshot.PositionSeconds < FXWindowBegin || Snapshot.PositionSeconds >= FXWindowEnd) { return; }

			if (bPublishBrightDraw && !FMath::IsFinite(Snapshot.PositionSeconds)) { return; }
			for (const FFXUnitObservation& Unit : FXUnits)
			{
				UNiagaraComponent* Component = Unit.Component.Get();
				if (!Component || Component->GetWorld() != World.Get() || Component->GetAttachParent() != Mesh.Get()
					|| Component->GetAsset() != Unit.System.Get()) { continue; }

				if (bPublishBrightDraw && (!Unit.System.IsValid() || Unit.System->GetFName() != FXNormal01System)) { continue; }
				const auto NiagaraController = Component->GetSystemInstanceController();
				if (!NiagaraController.IsValid() || !NiagaraController->IsValid()) { continue; }
				const FNiagaraSystemInstance* NiagaraInstance = NiagaraController->GetSystemInstance_Unsafe();
				if (NiagaraInstance->GetSystem() != Unit.System.Get()
					|| NiagaraInstance->SystemInstanceState != ENiagaraSystemInstanceState::Running
					|| NiagaraInstance->GetSystemInstanceIndex() < 0 || NiagaraInstance->HasPendingFinalize()
					|| NiagaraInstance->IsComplete() || NiagaraInstance->GetTickCount() <= 0) { continue; }

				const float Age = NiagaraInstance->GetAge();
				if (!FMath::IsFinite(Age) || Age <= 0.0f) { continue; }
				if (bPublishBrightDraw && bFXBrightDrawConsumed[0]
					&& static_cast<uint64>(NiagaraInstance->GetId()) != FXBrightDrawNiagaraId) { continue; }
				const FName LayerNames[] = {FXNormal01BrightLayers[0], FXNormal01BrightLayers[1], FXNormal01ControlLayer};
				const FNiagaraEmitterInstance* BrightEmitters[3] = {};
				const FNiagaraDataBuffer* BrightBuffers[3] = {};
				for (int32 LayerIndex = 0; LayerIndex < UE_ARRAY_COUNT(LayerNames); ++LayerIndex)
				{
					if (!bPublishBrightDraw && LayerIndex == 2) { continue; }
					const FName LayerName = LayerNames[LayerIndex];
					const FNiagaraEmitterInstanceRef* Match = NiagaraInstance->GetEmitters().FindByPredicate(
						[LayerName](const FNiagaraEmitterInstanceRef& Emitter) { return Emitter->GetEmitterHandle().GetName() == LayerName; });
					const FNiagaraEmitterInstance* Emitter = Match ? &Match->Get() : nullptr;
					const FNiagaraDataBuffer* Data = nullptr;
					if (Emitter && Emitter->GetEmitterHandle().GetIsEnabled() && !Emitter->IsDisabled()
						&& Emitter->AsStateful() && Emitter->GetSimTarget() == ENiagaraSimTarget::CPUSim)
					{
						Data = Emitter->GetParticleData().GetCurrentData();
					}

					if (bPublishBrightDraw)
					{
						BrightEmitters[LayerIndex] = Emitter;
						BrightBuffers[LayerIndex] = Data;
						continue;
					}
					Test.AddInfo(FString::Printf(
						TEXT("[Combat.PlayerCombo.FXDrawLayer] DrawFrame=%llu WorldId=%u Viewport=%p MontageTask='%s' Instance=%d FXTask='%s' Component='%s' System='%s' NiagaraId=%llu Tick=%d MainPosition=%.9f Age=%.9f Layer='%s' EmitterId=%s Coverage=%s Particles=%lld SpawnAttempts=%d."),
						static_cast<unsigned long long>(GFrameCounter), World->GetUniqueID(), DrawViewport,
						*MontageTask->GetPathName(), InstanceId, *FXTask->GetPathName(), *Component->GetPathName(), *Unit.System->GetPathName(),
						static_cast<unsigned long long>(NiagaraInstance->GetId()), NiagaraInstance->GetTickCount(),
						Snapshot.PositionSeconds, Age, *LayerName.ToString(),
						Emitter ? *Emitter->GetEmitterHandle().GetId().ToString() : TEXT("None"),
						Data ? TEXT("PublishedCPU") : TEXT("Uncovered"),
						Data ? static_cast<long long>(Data->GetNumInstances()) : -1LL,
						Data ? Emitter->GetTotalSpawnedParticles() : INDEX_NONE));
				}
				if (bPublishBrightDraw && BrightBuffers[0] && BrightBuffers[1]
					&& BrightBuffers[0]->GetNumInstances() > 0 && BrightBuffers[1]->GetNumInstances() > 0)
				{
					const int32 SampleIndex = SelectFXBrightDrawSample(BrightBuffers);
					if (SampleIndex != INDEX_NONE)
					{
						PublishFXBrightDraw(DrawViewport, *Component, *NiagaraInstance, Snapshot, SampleIndex, LayerNames, BrightEmitters, BrightBuffers);
					}
					return;
				}
			}
		}

		void ObserveFXViewportRendered(FViewport* DrawViewport)
		{
			if (DrawViewport != SceneViewport) { return; }
			const bool bPublishedDraw = bFXDrawInProgress && bFXDrawSawWorldEndOfFrame;
			bFXDrawInProgress = false;
			bFXDrawSawWorldEndOfFrame = false;
			if (!bPublishedDraw || bFailed || bFXVerified || !bFXCaptured
				|| (Stage != EStage::Natural && Stage != EStage::NaturalIdle)) { return; }
			if (!IsProductionChainCurrent()) { Fail(TEXT("原生产链在 FX Draw 采样时改变。")); return; }

			RecordFXBrightLayersForDraw(DrawViewport);

			// 原 Draw 已完成本 World 的 Niagara 帧末发布，才读取原实例。
			for (FFXUnitObservation& Unit : FXUnits)
			{
				UNiagaraComponent* Component = Unit.Component.Get();
				if (!Component || (Unit.bSimulationCaptured && bFXParticles)) { continue; }
				const auto NiagaraController = Component->GetSystemInstanceController();
				if (!NiagaraController.IsValid() || !NiagaraController->IsValid()) { continue; }
				const FNiagaraSystemInstance* NiagaraInstance = NiagaraController->GetSystemInstance_Unsafe();
				if (NiagaraInstance->SystemInstanceState == ENiagaraSystemInstanceState::Running
					&& NiagaraInstance->GetSystemInstanceIndex() >= 0 && !NiagaraInstance->HasPendingFinalize()
					&& !NiagaraController->IsComplete() && NiagaraController->GetAge() > 0.0f)
				{
					if (!Unit.Cache.Get()) { Unit.Cache.Reset(NewObject<UNiagaraSimCache>(GetTransientPackage())); }
					FNiagaraSimCacheCreateParameters Parameters;
					Parameters.bAllowDataInterfaceCaching = false;
					UNiagaraSimCache* Captured = nullptr;
					if (!UNiagaraSimCacheFunctionLibrary::CaptureNiagaraSimCacheImmediate(Unit.Cache.Get(), Parameters, Component, Captured, false)
						|| Captured != Unit.Cache.Get() || Captured->GetSystem() != Unit.System.Get() || Captured->GetNumFrames() != 1
						|| Captured->GetNumEmitters() != Unit.EmitterCount)
					{
						Fail(TEXT("native non-advancing particle capture failed; it cannot stand in for zero particles")); return;
					}
					Unit.bSimulationCaptured = true;
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
				|| (bFXCaptured && (!FXComponentsRetired() || !FXTask.Get() || FXTask->GetState() != EGameplayTaskState::Finished)))
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
			if (FXTask.Get()) { FXTask->OnUnitNaturallyCompleted.Remove(FXNaturalCompletionHandle); }
			FXNaturalCompletionHandle.Reset(); FXTask.Reset();
			for (FFXUnitObservation& Unit : FXUnits)
			{
				Unit.Component.Reset(); Unit.Cache.Reset(); Unit.bSimulationCaptured = false; Unit.bDetached = false;
				Unit.NaturalCompletionCount = 0; Unit.bRetiredNaturallyBeforeEnd = false;
			}
			bFXCaptured = false; bFXParticles = false; bFXUnknownNaturalCompletion = false;
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
		void LogNewPressInput(const TCHAR* Boundary, const FString& Detail)
		{
			const UGGYGOPlayerComboAbility* OriginalAbility = Ability.Get();
			Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.NewPressInput] Boundary=%s Frame=%llu Stage=%u Scope=%llu PlayerInput=%s GA=%s Spec=%s InputTask=%s OriginalInstance=%d Step=%d LastGARequest=%d AttackPressed=%d ActiveFixtureSubmission=%u LastFixtureSubmission=%u %s"),
				Boundary, static_cast<unsigned long long>(GFrameCounter), static_cast<uint32>(Stage),
				static_cast<unsigned long long>(Scope.GetScopeSerial()), *GetPathNameSafe(PlayerInput.Get()),
				*GetPathNameSafe(OriginalAbility), *SpecHandle.ToString(),
				*GetPathNameSafe(FGGYGOPlayerComboLifecycleFixture::GetProductionInputTask(OriginalAbility)), InstanceId,
				OriginalAbility ? OriginalAbility->GetCurrentComboStep() : INDEX_NONE,
				FGGYGOPlayerComboLifecycleFixture::ReadProductionLastInputRequest(OriginalAbility),
				PlayerInput.IsValid() && PlayerInput->IsPressed(AttackKey), ActiveFixtureSubmission, LastFixtureSubmission, *Detail));
		}
		void InstallNewPressInputObservation(const UInputAction* AttackAction, const TArray<FKey>& AttackKeys)
		{
			NewPressObservedKeys = AttackKeys;
			NewPressObservedKeys.AddUnique(EKeys::W);
			NewPressViewportKeyHandle = Viewport->OnInputKey().AddLambda([this](const FInputKeyEventArgs& Args)
			{
				if (!NewPressObservedKeys.Contains(Args.Key) || !IsOwnedWorldCurrent()) { return; }
				LogNewPressInput(TEXT("ViewportKey"), FString::Printf(TEXT("Key=%s Event=%s Device=%d OriginalScene=%d EngineSimulated=%d"),
					*Args.Key.ToString(), *UEnum::GetValueAsString(Args.Event), Args.InputDevice.GetId(),
					Args.Viewport == SceneViewport, Args.IsSimulatedInput()));
			});
			UGGYGOInputComponent* Component = Cast<UGGYGOInputComponent>(Character->InputComponent.Get());
			const ETriggerEvent Events[] = { ETriggerEvent::Triggered, ETriggerEvent::Completed, ETriggerEvent::Canceled };
			for (const ETriggerEvent Event : Events)
			{
				if (!Component || !Component->GetActionEventBindings().ContainsByPredicate([AttackAction, Event](const auto& Binding)
				{
					return Binding->GetAction() == AttackAction && Binding->GetTriggerEvent() == Event;
				}))
				{
					LogNewPressInput(TEXT("ActionObservationUnavailable"),
						FString::Printf(TEXT("Action=%s Component=%s MissingBinding=%s"),
							*GetPathNameSafe(AttackAction), *GetPathNameSafe(Component), *UEnum::GetValueAsString(Event)));
					return;
				}
			}
			NewPressObservedInputComponent = Component;
			for (const ETriggerEvent Event : Events)
			{
				// 仅旁听原组件已经接线的事件，避免为生产 Action 新增可消费的事件类型。
				FEnhancedInputActionEventBinding& Binding = Component->BindActionInstanceLambda(AttackAction, Event,
					[this](const FInputActionInstance& Instance)
					{
						if (!IsOwnedWorldCurrent() || !Controller.IsValid() || !PlayerInput.IsValid()
							|| Controller->PlayerInput != PlayerInput.Get() || !Character.IsValid()
							|| Character->InputComponent.Get() != NewPressObservedInputComponent.Get()) { return; }
						LogNewPressInput(TEXT("AttackAction"), FString::Printf(TEXT("Action=%s Event=%s"),
							*GetPathNameSafe(Instance.GetSourceAction().Get()), *UEnum::GetValueAsString(Instance.GetTriggerEvent())));
						if (IsAuthoredRestartProbe() && Instance.GetTriggerEvent() == ETriggerEvent::Triggered)
						{
							ObserveAuthoredRestartAttackAction();
						}
					});
				Binding.SetShouldConsume(false);
				NewPressAttackObservationHandles.Add(Binding.GetHandle());
			}
			LogNewPressInput(TEXT("ActionObservationInstalled"),
				FString::Printf(TEXT("Action=%s Component=%s ViewportObserved=%d Bindings=%d"), *GetPathNameSafe(AttackAction),
					*GetPathNameSafe(Component), NewPressViewportKeyHandle.IsValid(), NewPressAttackObservationHandles.Num()));
		}
		void ReleaseNewPressInputObservation()
		{
			if (Viewport.IsValid()) { Viewport->OnInputKey().Remove(NewPressViewportKeyHandle); }
			NewPressViewportKeyHandle.Reset();
			if (UGGYGOInputComponent* Component = NewPressObservedInputComponent.Get())
			{
				for (const uint32 Handle : NewPressAttackObservationHandles) { Component->RemoveBindingByHandle(Handle); }
			}
			NewPressAttackObservationHandles.Reset();
			NewPressObservedInputComponent.Reset();
			NewPressObservedKeys.Reset();
		}
		bool SendNativeKey(const FKey& Key, bool bDown)
		{
			if (!NewPressObservedKeys.Contains(Key)) { return SendNativeKeyToSlate(Key, bDown); }
			// 同栈标记只证明经夹具提交的调用；视口无此标记的事件不推断物理来源。
			TGuardValue<uint32> Submission(ActiveFixtureSubmission, ++LastFixtureSubmission);
			const FString Detail = FString::Printf(TEXT("Key=%s Down=%d"), *Key.ToString(), bDown);
			LogNewPressInput(TEXT("FixtureSubmit"), Detail);
			const bool bHandled = SendNativeKeyToSlate(Key, bDown);
			LogNewPressInput(TEXT("FixtureReturn"), FString::Printf(TEXT("%s SlateHandled=%d"), *Detail, bHandled));
			return bHandled;
		}
		bool SendNativeKeyToSlate(const FKey& Key, bool bDown)
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
				if (IsAuthoredRestartProbe() && bRestartSent)
				{
					const uint64 NewToken = FGGYGOPlayerComboLifecycleFixture::ReadProductionStepToken(Ability.Get());
					if (!bAuthoredAttackConsumed || ActualStep != 0 || NewToken == 0 || NewToken == AuthoredTerminalToken
						|| Snapshot.Montage->GetPathName() != TEXT("/Game/Characters/Player/Pyrios/Animation/Attack/AM_Pyrios_Attack_Normal_01.AM_Pyrios_Attack_Normal_01"))
					{
						Fail(TEXT("原包 03→01 未在真实新请求后交付同激活的新 01 原包和段 token。")); return;
					}
					Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.AuthoredRestart] OldToken=%llu NewToken=%llu OldInstance=%d NewInstance=%d Request=%d SameActivation=1 OldTaskFinished=1."),
						static_cast<unsigned long long>(AuthoredTerminalToken), static_cast<unsigned long long>(NewToken),
						ReplacedInstanceId, Snapshot.MontageInstanceId, MainAttackRequest + 1));
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
			if (IsAuthoredRestartProbe() && ActualStep == TargetStep)
			{
				const int32 MainIndex = Montage->GetSectionIndex(MainSection);
				float MainStart = 0.0f, MainEnd = 0.0f;
				if (MainIndex != INDEX_NONE) { Montage->GetSectionStartAndEndTime(MainIndex, MainStart, MainEnd); }
				AuthoredTerminalToken = FGGYGOPlayerComboLifecycleFixture::ReadProductionStepToken(Ability.Get());
				if (Montage->GetPathName() != TEXT("/Game/Characters/Player/Pyrios/Animation/Attack/AM_Pyrios_Attack_Normal_03.AM_Pyrios_Attack_Normal_03")
					|| MainPointProbe.Get() || AuthoredTerminalToken == 0 || Step.NextStepIndex != 0
					|| Step.MovementInterruptionMode != EGGYGOActionInterruptionMode::AuthoredNotify
					|| MainIndex == INDEX_NONE || MainEnd != EndStart || OriginalMovementPoint.NotifyAuthoredPositionSeconds != MainEnd
					|| !(MainStart < FMath::Min(AuthorPointPosition, OriginalAuthorPoint.NotifyAuthoredPositionSeconds)
						&& FMath::Max(AuthorPointPosition, OriginalAuthorPoint.NotifyAuthoredPositionSeconds) < MainEnd))
				{
					Fail(TEXT("原包 03 缺少可区分的原攻击点、Main 末移动点或 01 接续配置，未覆盖。")); return;
				}
				Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.AuthoredRestart] Original03=%s Instance=%d Token=%llu AuthoredAttack=%.9f Trigger=%.9f MainEnd=%.9f."),
					*Montage->GetPathName(), InstanceId, static_cast<unsigned long long>(AuthoredTerminalToken),
					OriginalAuthorPoint.NotifyAuthoredPositionSeconds, AuthorPointPosition, EndStart));
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
					// Main 可以从零开始；同位置尚未发生原生 Begin 与已发生 Begin 是不同事实。
					if (Snapshot.PositionSeconds > FXWindowBegin || FXBeginCounts.FindRef(InstanceId) != 0
						|| HasOriginalActiveFXNotify(InstanceId)
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
				Stage = (EndAttackProbe == EComboEndAttackProbe::Restart || IsAuthoredRestartProbe()) ? EStage::MainAttackPress : EStage::Natural;
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
			if (bHeldDuringMain)
			{
				Deadline = FPlatformTime::Seconds() + 10.0;
				bWDown = true; SendNativeKey(EKeys::W, true);
			}
			else
			{
				if (!IsOwnedWorldCurrent()) { Fail(TEXT("original PIE world retired before NewPress End observation")); return; }
				const double WallNow = FPlatformTime::Seconds();
				const double GameNow = World->GetTimeSeconds();
				// 保留 10 秒动作等待预算。独立墙钟上限负责无进展的 PIE。
				NewPressEndWorldDeadline = GameNow + 10.0;
				Deadline = WallNow + 30.0;
				LogNewPressEndWait(TEXT("Started"), WallNow, GameNow, &Snapshot);
			}
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
		bool IsAuthoredRestartProbe() const { return EndAttackProbe == EComboEndAttackProbe::AuthoredRestart; }
		void ObserveAuthoredRestartAttackAction()
		{
			const bool bBeforePoint = Stage == EStage::MainAttackRejected;
			if (bFailed || AuthoredAttackInputFrame != MAX_uint64
				|| (!bBeforePoint && !(Stage == EStage::Main && bRestartSent && !bRestartCaptured))) { return; }

			FGGYGOMontageSectionSnapshot Snapshot;
			if (!ReadOriginalPlayback(Snapshot)) { return; }
			const bool bInAttackWindow = bBeforePoint
				? Snapshot.PositionSeconds < FMath::Min(AuthorPointPosition, OriginalAuthorPoint.NotifyAuthoredPositionSeconds)
				: Snapshot.PositionSeconds >= FMath::Max(AuthorPointPosition, OriginalAuthorPoint.NotifyAuthoredPositionSeconds);
			const bool bAttackOpen = Ability->IsAttackContinuationOpen();
			const bool bMoveOpen = Ability->IsMovementInterruptionOpen();
			if (!bInAttackWindow || Snapshot.SectionName != MainSection || Snapshot.PositionSeconds >= EndStart)
			{
				Fail(TEXT("原 03 的真实攻击 Action 错过原点前或原点后至 Main 末的区间，未覆盖。")); return;
			}
			if (bAttackOpen == bBeforePoint || bMoveOpen || Ability->GetCurrentComboStep() != TargetStep
				|| FGGYGOPlayerComboLifecycleFixture::GetProductionMontageTask(Ability.Get()) != MontageTask.Get()
				|| FGGYGOPlayerComboLifecycleFixture::ReadProductionStepToken(Ability.Get()) != AuthoredTerminalToken
				|| FGGYGOPlayerComboLifecycleFixture::ReadProductionLastInputRequest(Ability.Get()) != MainAttackRequest)
			{
				Fail(TEXT("原 03 消费前的两道许可或原请求／段身份错误。")); return;
			}
			// 原 Action 在 Controller::PostProcessInput 消费 ASC 前触发。
			AuthoredAttackInputFrame = GFrameCounter;
			Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.AuthoredRestart] BeforeASC Frame=%llu Instance=%d RequestBefore=%d Position=%.9f AttackOpen=%d MovementOpen=%d."),
				static_cast<unsigned long long>(AuthoredAttackInputFrame), InstanceId, MainAttackRequest,
				Snapshot.PositionSeconds, bAttackOpen, bMoveOpen));
		}
		void ObserveAuthoredRestartAfterController()
		{
			if (!bFailed && !IsProductionChainCurrent()) { Fail(TEXT("原包 03→01 在原 Controller 输入边界失去生产链身份。")); }
			if (!bFailed && AuthoredAttackInputFrame == MAX_uint64)
			{
				FGGYGOMontageSectionSnapshot Snapshot;
				const float Limit = Stage == EStage::MainAttackRejected
					? FMath::Min(AuthorPointPosition, OriginalAuthorPoint.NotifyAuthoredPositionSeconds) : EndStart;
				if (ReadOriginalPlayback(Snapshot) && (Snapshot.SectionName != MainSection || Snapshot.PositionSeconds >= Limit))
				{
					Fail(TEXT("原 03 的真实区间已过，未收到本次新攻击 Action，未覆盖。"));
				}
				if (!bFailed) { return; }
			}
			if (!bFailed)
			{
				const bool bBeforePoint = Stage == EStage::MainAttackRejected;
				const int32 Request = FGGYGOPlayerComboLifecycleFixture::ReadProductionLastInputRequest(Ability.Get());
				const uint64 Token = FGGYGOPlayerComboLifecycleFixture::ReadProductionStepToken(Ability.Get());
				if (AuthoredAttackInputFrame != GFrameCounter || Request != MainAttackRequest + 1
					|| !Ability->CaptureCurrentActivation().HasSameActivation(OriginalActivation)
					|| (bBeforePoint ? (Ability->GetCurrentComboStep() != TargetStep || Token != AuthoredTerminalToken
						|| FGGYGOPlayerComboLifecycleFixture::GetProductionMontageTask(Ability.Get()) != MontageTask.Get())
						: (Ability->GetCurrentComboStep() != 0 || Token == 0 || Token == AuthoredTerminalToken)))
				{
					Fail(TEXT("原 Controller 同帧未交付原点前拒绝或原点后同激活 01 接续，未覆盖。"));
				}
				else
				{
					bAuthoredAttackConsumed = true;
					Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.AuthoredRestart] AfterController Frame=%llu Request=%d Step=%d Token=%llu."),
						static_cast<unsigned long long>(GFrameCounter), Request, Ability->GetCurrentComboStep(), static_cast<unsigned long long>(Token)));
					if (bBeforePoint) { ObserveRejectedMainAttack(); }
				}
			}
			ReleaseFXInputObservation();
		}
		void PressAttackDuringTerminalMain()
		{
			FGGYGOMontageSectionSnapshot Snapshot;
			if (!ReadOriginalPlayback(Snapshot)) { return; }
			if (Snapshot.SectionName != MainSection) { Fail(TEXT("terminal Main ended before its real rejection probe")); return; }
			if (PlayerInput->IsPressed(AttackKey)) { return; } // The previously issued key-up must reach native input.
			if (IsAuthoredRestartProbe())
			{
				if (Snapshot.PositionSeconds >= FMath::Min(AuthorPointPosition, OriginalAuthorPoint.NotifyAuthoredPositionSeconds))
				{
					Fail(TEXT("原 03 攻击点前已无可送入真实新按键的区间，未覆盖。")); return;
				}
				if (!InstallFXInputObservation()) { return; }
			}
			MainAttackRequest = FGGYGOPlayerComboLifecycleFixture::ReadProductionLastInputRequest(Ability.Get());
			bAttackDown = true;
			SendNativeKey(AttackKey, true);
			Stage = EStage::MainAttackRejected;
		}
		void ObserveRejectedMainAttack()
		{
			FGGYGOMontageSectionSnapshot Snapshot;
			if (!ReadOriginalPlayback(Snapshot)) { return; }
			if (IsAuthoredRestartProbe() && !bAuthoredAttackConsumed) { return; }
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
			if (!ReadOriginalPlayback(Snapshot)) { return; }
			if (IsAuthoredRestartProbe() && (Snapshot.SectionName != MainSection || Snapshot.PositionSeconds >= EndStart))
			{
				Fail(TEXT("原 03 攻击点后至 Main 末的真实区间已过，未覆盖。")); return;
			}
			if (!Ability->IsInterruptionOpen()) { return; }
			if (Ability->IsMovementInterruptionOpen() && Snapshot.SectionName == MainSection)
			{
				Fail(TEXT("attack continuation point opened movement interruption inside Main")); return;
			}
			if (!bMainAttackRejected) { Fail(TEXT("terminal Main rejection was not observed before End restart")); return; }
			if (Snapshot.SectionName != MainSection || Snapshot.PositionSeconds >= EndStart)
			{
				Fail(TEXT("author point was not able to open attack replacement inside Main")); return;
			}
			if (IsAuthoredRestartProbe())
			{
				if (Snapshot.PositionSeconds < OriginalAuthorPoint.NotifyAuthoredPositionSeconds || PlayerInput->IsPressed(AttackKey)) { return; }
				if (!InstallFXInputObservation()) { return; }
				MainAttackRequest = FGGYGOPlayerComboLifecycleFixture::ReadProductionLastInputRequest(Ability.Get());
				AuthoredAttackInputFrame = MAX_uint64;
				bAuthoredAttackConsumed = false;
			}
			bRestartSent = true;
			SendEndAttack(Snapshot, 0);
		}
		void LogNewPressEndWait(const TCHAR* Boundary, double WallNow, double GameNow, const FGGYGOMontageSectionSnapshot* Snapshot)
		{
			const FString Section = Snapshot ? Snapshot->SectionName.ToString() : TEXT("Unavailable");
			Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.EndProductionSmoke] NewPressEndWait=%s Frame=%llu WallSeconds=%.6f GameSeconds=%.6f GameDelta=%.6f WallRemaining=%.6f GameRemaining=%.6f Montage=%s OriginalInstance=%d ObservedInstance=%d Section=%s Position=%.6f MainEnd=%.6f."),
				Boundary, static_cast<unsigned long long>(GFrameCounter), WallNow, GameNow, World->GetDeltaSeconds(),
				Deadline - WallNow, NewPressEndWorldDeadline - GameNow, *GetPathNameSafe(Montage.Get()), InstanceId,
				Snapshot ? Snapshot->MontageInstanceId : INDEX_NONE, *Section, Snapshot ? Snapshot->PositionSeconds : -1.0f, EndStart));
		}
		void ObserveNaturalEnd()
		{
			const double WallNow = FPlatformTime::Seconds();
			const double GameNow = World->GetTimeSeconds();
			const bool bWallExpired = WallNow >= Deadline;
			if (!bHeldDuringMain && (bWallExpired || GameNow >= NewPressEndWorldDeadline))
			{
				FGGYGOMontageSectionSnapshot TimeoutSnapshot;
				const bool bObserved = MontageTask.IsValid() && MontageTask->TryGetOriginalSectionSnapshot(TimeoutSnapshot);
				LogNewPressEndWait(bWallExpired ? TEXT("WallTimeout") : TEXT("GameTimeout"),
					WallNow, GameNow, bObserved ? &TimeoutSnapshot : nullptr);
				Fail(bWallExpired ? TEXT("NewPress original End observation reached its wall-clock limit")
					: TEXT("NewPress original End was not observed within the PIE game-time budget"));
				BeginEnding();
				return;
			}
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
				LogNewPressEndWait(TEXT("EndObserved"), WallNow, GameNow, &Snapshot);
				// 取消阶段沿用剩余等待预算的墙钟约束，并受宿主上限约束。
				Deadline = FMath::Min(Deadline, WallNow + (NewPressEndWorldDeadline - GameNow));
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
			if (EndCount != 1 || CompletedCount != 1 || !EndData.bWasCancelled || !Completion.HasCompletion() || !bEndSnapshotAvailable
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
				|| !bSawNaturalEnd || !bResourcesRestoredAtEnd || !ResourcesRestored() || Ability->IsActive()
				|| !FMath::IsFinite(NaturalEarliestEndWorldSeconds)
				|| EndObservedWorldSeconds + FMath::Max(0.05f, World->GetDeltaSeconds()) < NaturalEarliestEndWorldSeconds
				|| PlayerInput->IsPressed(AttackKey) || PlayerInput->IsPressed(EKeys::W)
				|| ((EndAttackProbe == EComboEndAttackProbe::Restart || IsAuthoredRestartProbe())
					&& (!bRestartCaptured || !bMainAttackRejected)))
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
				if (!Captured.HasActivation() || Activated->GetCurrentAbilitySpecHandle() != SpecHandle
					|| Activated->GetAbilitySystemComponentFromActorInfo() != ASC.Get())
				{
					Fail(TEXT("native pose activation did not match the original GA/ASC/Spec")); return;
				}
				const bool bSuccessor = OriginalActivation.HasActivation();
				if (!bSuccessor) { OriginalActivation = Captured; }
				else if (Stage == EStage::PoseSuccessorMain && !PoseSuccessorActivation.HasActivation()
					&& !Captured.HasSameActivation(OriginalActivation)) { PoseSuccessorActivation = Captured; }
				else { Fail(TEXT("unexpected activation replaced the pose probe's original or successor")); return; }
				// GAS 在每次 GA 原生 End 广播后清空该委托。于原激活回调中绑定，覆盖同步启动失败。
				PoseGAEndHandle = Activated->OnGameplayAbilityEndedWithData.AddLambda(
					[this, Captured, bSuccessor](const FAbilityEndedData& Data) { ObservePoseGAEnd(Data, Captured, bSuccessor); });
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
			if (!bPoseUncancelableBeforeFault || Cancel.Outcome != EGGYGOAbilityTerminationOutcome::NotCancelable || !Ability->IsActive()
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
		void ObservePoseGAEnd(const FAbilityEndedData& Data, const FGGYGOAbilityActivationHandle& Captured, bool bSuccessor)
		{
			const FGGYGOAbilityActivationHandle& Expected = bSuccessor ? PoseSuccessorActivation : OriginalActivation;
			if (Data.AbilityThatEnded != Ability.Get() || Data.AbilitySpecHandle != SpecHandle || !Captured.HasSameActivation(Expected))
			{
				Fail(TEXT("pose native GA End did not match its subscribed activation/Spec")); return;
			}
			if (bSuccessor) { ++PoseSuccessorEndCount; bPoseSuccessorCancelled = Data.bWasCancelled; }
			else { ++EndCount; EndData = Data; }
			Test.AddInfo(FString::Printf(TEXT("[Combat.PlayerCombo.PoseProductionSmoke] Case=%s Boundary=NativeGAEnd Successor=%d Cancelled=%d."),
				PoseCaseName(), bSuccessor, Data.bWasCancelled));
		}
		void ObservePoseAbilityEnd(const FAbilityEndedData& Data)
		{
			if (Data.AbilitySpecHandle != SpecHandle || Ability->CaptureCurrentActivation().HasActivation())
			{
				Fail(TEXT("pose ASC End did not retire the original GA activation/Spec")); return;
			}
			if (Stage == EStage::PoseSuccessorMain || Stage == EStage::PoseSuccessorEnd)
			{
				if (PoseSuccessorEndCount != 1 || bPoseSuccessorCancelled != Data.bWasCancelled)
				{
					Fail(TEXT("pose successor ASC End did not follow its exact native GA End")); return;
				}
				++PoseSuccessorCompletedCount;
				PoseSuccessorCompletion.Original = FGGYGOAbilityTerminationContext(PoseSuccessorActivation, Data.bWasCancelled);
				bPoseSuccessorResourcesAtEnd = ResourcesRestored();
				return;
			}
			if (EndCount != 1 || EndData.bWasCancelled != Data.bWasCancelled)
			{
				Fail(TEXT("pose original ASC End did not follow its exact native GA End")); return;
			}
			++CompletedCount;
			Completion.Original = FGGYGOAbilityTerminationContext(OriginalActivation, Data.bWasCancelled);
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
			Test.GetExecutionInfo(Info); // 读取原始事件，保留生产 Error。
			const FString Owner = FString::Printf(TEXT("Ability='%s'"), *GetPathNameSafe(Ability.Get()));
			const FString OwnerASC = FString::Printf(TEXT(" ASC='%s'"), *GetPathNameSafe(ASC.Get()));
			const FString Asset = FString::Printf(TEXT("Montage='%s'"), *GetPathNameSafe(Montage.Get()));
			const FString Slot = FString::Printf(TEXT("Slot='%s'"), *SavedPoseSlot.ToString());
			const EGGYGOMontageTaskFailureStage FailureStage = PoseProbe == EComboPoseFailureProbe::RuntimeInvalidated
				? EGGYGOMontageTaskFailureStage::Playback : EGGYGOMontageTaskFailureStage::Startup;
			const FString StageText = FString::Printf(TEXT(" Stage=%d "), static_cast<int32>(FailureStage));
			const FString InstanceSpace = FString::Printf(TEXT(" Instance=%d "), InstanceId);
			const FString InstanceSemi = FString::Printf(TEXT(" Instance=%d;"), InstanceId);
			const FString Accepted = FString::Printf(TEXT(" Outcome=%d"), static_cast<int32>(EGGYGOAbilityTerminationOutcome::Accepted));
			const FString OriginalTask = PoseOriginalTaskPath.IsEmpty()
				? FString() : FString::Printf(TEXT(" Task='%s'"), *PoseOriginalTaskPath);
			const FString* TaskFact = nullptr;
			const FString* GAFact = nullptr;
			const FString* EndResult = nullptr;
			for (int32 Index = PoseDiagnosticStart; Index < Info.GetEntries().Num(); ++Index)
			{
				const FString& Message = Info.GetEntries()[Index].Event.Message;
				if (!Message.Contains(Owner) || !Message.Contains(Asset) || !Message.Contains(Slot)
					|| !Message.Contains(StageText) || (!Message.Contains(InstanceSpace) && !Message.Contains(InstanceSemi))
					|| (!OriginalTask.IsEmpty() && !Message.Contains(OriginalTask))) { continue; }
				if (Message.Contains(TEXT("[AbilitySystem.MontageTask.Pose]")))
				{
					if (TaskFact || !Message.Contains(OwnerASC)
						|| (FailureStage == EGGYGOMontageTaskFailureStage::Startup && !Message.Contains(TEXT(" Call=0 ")))) { return false; }
					TaskFact = &Message;
				}
				else if (Message.Contains(TEXT("[Combat.PlayerCombo.PoseFailure] Boundary=RequiredDependencyFailed")))
				{
					if (GAFact || !Message.Contains(TEXT(" SourceValid=1 "))) { return false; }
					GAFact = &Message;
				}
				else if (Message.Contains(TEXT("[Combat.PlayerCombo.PoseFailure] Boundary=AbilityEndResult")))
				{
					if (EndResult || !Message.Contains(TEXT(" SourceValid=1 ")) || !Message.EndsWith(Accepted)) { return false; }
					EndResult = &Message;
				}
			}
			// Startup 没有播放实例，仍要求两个 GA 回执与原 ASC 报告的同一 Task 匹配。
			FString TaskField, TaskPath;
			if (!TaskFact || !GAFact || !EndResult || !TaskFact->Split(TEXT(" Task='"), nullptr, &TaskField)
				|| !TaskField.Split(TEXT("'"), &TaskPath, nullptr) || TaskPath.IsEmpty()) { return false; }
			const FString ExactTask = FString::Printf(TEXT(" Task='%s'"), *TaskPath);
			return GAFact->Contains(ExactTask) && EndResult->Contains(ExactTask);
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
				|| !Completion.HasCompletion()
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
				|| !PoseSuccessorCompletion.HasCompletion()
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
			ON_SCOPE_EXIT { ReleaseNewPressInputObservation(); };
			ReleaseFXInputObservation();
			ReleaseFXFrameObservation();
			if (NativeWindow.IsValid()) { NativeWindow->GetOnWindowDeactivatedEvent().Remove(WindowDeactivatedHandle); }
			WindowDeactivatedHandle.Reset();
			if (PoseProbe != EComboPoseFailureProbe::None)
			{
				if (Ability.IsValid()) { Ability->OnGameplayAbilityEndedWithData.Remove(PoseGAEndHandle); }
				PoseGAEndHandle.Reset();
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
				ASC->OnAbilityEnded.Remove(CompletedHandle);
				ASC->AbilityActivatedCallbacks.Remove(ActivatedHandle);
				if (FXEventHandle.IsValid()) { ASC->RemoveGameplayEventTagContainerDelegate(FXEventTags, FXEventHandle); }
			}
			EndHandle.Reset(); CompletedHandle.Reset();
			FXEventHandle.Reset(); FXEventTags.Reset();
			if (FXTask.Get()) { FXTask->OnUnitNaturallyCompleted.Remove(FXNaturalCompletionHandle); }
			FXNaturalCompletionHandle.Reset(); FXTask.Reset(); FXUnits.Reset();
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
		FDelegateHandle FXBeginDrawHandle, FXWorldEndOfFrameHandle, FXViewportRenderedHandle, FXBrightDrawnHandle;
		TSharedPtr<FComboFXBrightDrawView, ESPMode::ThreadSafe> FXBrightDrawView;
		FComboFXMeshDrawObservation FXMeshDrawCandidate;
		FComboFXMeshDrawObservation FXMeshDrawSamples[UE_ARRAY_COUNT(FXDrawSamplePhases)];
		bool bFXBrightDrawConsumed[UE_ARRAY_COUNT(FXDrawSamplePhases)] = {};
		uint64 FXBrightDrawNiagaraId = 0;
		bool bFXDrawInProgress = false;
		bool bFXDrawSawWorldEndOfFrame = false;
		uint64 FXStartupFrame = MAX_uint64;
		float FXStartupWorldSeconds = 0.0f;
		FString InputResourceWaitReason;
		bool bLoggedColdFXControllerSample = false;
		FDelegateHandle WindowDeactivatedHandle;
		struct FFXUnitObservation
		{
			TWeakObjectPtr<UNiagaraSystem> System;
			TWeakObjectPtr<UNiagaraComponent> Component;
			TStrongObjectPtr<UNiagaraSimCache> Cache;
			int32 EmitterCount = 0;
			int32 NaturalCompletionCount = 0;
			bool bSimulationCaptured = false;
			bool bRetiredNaturallyBeforeEnd = false;
			bool bDetached = false;
			bool IsRetired() const
			{
				UNiagaraComponent* Original = Component.Get(true);
				return !Original || (Original->IsBeingDestroyed() && !Original->IsRegistered());
			}
		};
		TArray<FFXUnitObservation> FXUnits;
		TStrongObjectPtr<UGGYGOAbilityTask_PlayNiagaraEffect> FXTask;
		FDelegateHandle FXNaturalCompletionHandle;
		FGameplayTagContainer FXEventTags;
		FDelegateHandle FXEventHandle;
		TMap<int32, int32> FXBeginCounts, FXEndCounts;
		FGGYGOAbilityActivationHandle FXCancelledActivation;
		int32 FXCancelledInstanceId = INDEX_NONE, FXRejectedOldEvents = 0;
		uint64 FXCancelFrame = MAX_uint64, FXRestartPlaybackFrame = MAX_uint64;
		uint64 FXRestartBeginFrame = MAX_uint64, FXCancelledEndFrame = MAX_uint64;
		float FXWindowBegin = 0.0f, FXWindowEnd = 0.0f;
		bool bFXCaptured = false, bFXParticles = false, bFXVerified = false, bFXRestarted = false;
		bool bFXUnknownNaturalCompletion = false;
		bool bFXCancelledWithActiveNotify = false, bFXNativePlaybackOverlap = false;
		int32 TargetStep = 0, ExpectedStep = 0, ReplacedStep = INDEX_NONE, ReplacedInstanceId = INDEX_NONE, MainAttackRequest = 0;
		int32 NaturalInputRequestId = 0;
		bool bRestartSent = false, bRestartCaptured = false, bMainAttackRejected = false;
		uint64 AuthoredTerminalToken = 0, AuthoredAttackInputFrame = MAX_uint64;
		bool bAuthoredAttackConsumed = false;
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
		double NewPressEndWorldDeadline = 0.0;
		float SavedRootScale = 0.0f, EndStart = 0.0f, EndEnd = 0.0f, EndPosition = 0.0f;
		int32 InstanceId = INDEX_NONE, EndCount = 0, CompletedCount = 0;
		EVisibilityBasedAnimTickOption SavedTick = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
		FString EditorMap;
		FName ContextHandle, MainSection, EndSection, EndedSection;
		FKey AttackKey;
		FDelegateHandle NewPressViewportKeyHandle;
		TWeakObjectPtr<UGGYGOInputComponent> NewPressObservedInputComponent;
		TArray<uint32> NewPressAttackObservationHandles;
		TArray<FKey> NewPressObservedKeys;
		uint32 ActiveFixtureSubmission = 0, LastFixtureSubmission = 0;
		FVector PositionAtEnd = FVector::ZeroVector;
		FGameplayAbilitySpecHandle SpecHandle;
		FGGYGOAbilityActivationHandle OriginalActivation;
		FGGYGOMovementOwnerSyncScopeId Scope;
		FGGYGOMovementOwnerSyncObserverId OwnerSyncObserver;
		FGGYGOMovementOwnerSyncNotice OwnerNotice;
		FGGYGOQualifiedMovementIntent MainIntent, EndIntent;
		EGGYGOQualifiedMovementIntentQueryResult EndIntentResult = EGGYGOQualifiedMovementIntentQueryResult::Unavailable;
		FAbilityEndedData EndData;
		FDelegateHandle ActivatedHandle;
		FGGYGOAbilityActivationHandle LastActivated;
		GGYGOPlayerComboTestLifecycle::FEnded Completion;
		FGGYGOAbilityActivationHandle PoseSuccessorActivation;
		GGYGOPlayerComboTestLifecycle::FEnded PoseSuccessorCompletion;
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
		FDelegateHandle PoseActivatedHandle, PoseGAEndHandle;
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOPlayerComboAuthoredRestartProductionTest,
	"GGYGO.AbilitySystem.PlayerCombo.Interruption.ProductionNativeAuthored03To01",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGGYGOPlayerComboAuthoredRestartProductionTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(FComboEndProductionPIECommand(*this, false, EComboPoseFailureProbe::None, 0, EComboEndAttackProbe::AuthoredRestart));
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
