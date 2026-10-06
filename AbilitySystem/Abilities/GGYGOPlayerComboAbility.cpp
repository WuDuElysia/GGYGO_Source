/** @file GGYGOPlayerComboAbility.cpp */
#include "AbilitySystem/Abilities/GGYGOPlayerComboAbility.h"

#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystem/Abilities/GGYGOComboCorrection.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "AbilitySystem/Tasks/GGYGOAbilityTask_PlayMontageAndWaitForEvent.h"
#include "AbilitySystem/Tasks/GGYGOAbilityTask_WaitComboInput.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/Data/GGYGOActionMotionSourceBinding.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Character/Data/GGYGOActionMotionEvaluation.h"
#include "Combat/HitDetection/GGYGOMeleeTraceComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameplayEffect.h"
#include "System/GGYGOGameData.h"
#include "System/GGYGOGameplayTags.h"
#include "TimerManager.h"
#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOPlayerComboAbility)

namespace
{
	const TCHAR* DescribeQualifiedMovementResult(EGGYGOQualifiedMovementIntentQueryResult Result)
	{
		switch (Result)
		{
		case EGGYGOQualifiedMovementIntentQueryResult::Unavailable: return TEXT("Unavailable");
		case EGGYGOQualifiedMovementIntentQueryResult::Qualified: return TEXT("Qualified");
		case EGGYGOQualifiedMovementIntentQueryResult::NotHeld: return TEXT("NotHeld");
		case EGGYGOQualifiedMovementIntentQueryResult::AwaitingPhysicalProof: return TEXT("AwaitingPhysicalProof");
		case EGGYGOQualifiedMovementIntentQueryResult::WaitingForAdmission: return TEXT("WaitingForAdmission");
		case EGGYGOQualifiedMovementIntentQueryResult::ExecutionFailed: return TEXT("ExecutionFailed");
		default: return TEXT("UnsupportedQueryValue");
		}
	}
}

/** GA owns these resources; section interpretation is derived only from its original Task. */
struct UGGYGOPlayerComboAbility::FStepMotionResources
{
	enum class ESection : uint8 { AwaitingSnapshot, Main, End, Retired };
	FGGYGOAbilityActivationHandle Activation;
	uint64 StepToken = 0;
	int32 StepIndex = INDEX_NONE;
	TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> Task;
	TWeakObjectPtr<UGGYGOCharacterMovementComponent> Movement;
	FGGYGOActionMotionSourceBindingPtr Source;
	FGGYGOMovementOwnerSyncScopeId Scope;
	FGGYGOQualifiedMovementIntentObserverId Observer;
	int32 MontageInstanceId = INDEX_NONE;
	int32 MotionHandle = INDEX_NONE;
	ESection Section = ESection::AwaitingSnapshot;
	float EffectiveRate = 0.0f;
	float TranslationScale = 0.0f;
	FName EndSection;
	FString AbilityPath;
	/** Authenticated facts from this original Task and CMC handle, never a copied playback clock. */
	bool bMontageCompleted = false;
	bool bMotionCompleted = false;

	void Release(EGGYGOActionMotionReleaseReason Reason)
	{
		Section = ESection::Retired;
		const auto OriginalObserver = Observer;
		const int32 OriginalHandle = MotionHandle;
		Observer = {};
		MotionHandle = INDEX_NONE;
		const TWeakObjectPtr<UGGYGOCharacterMovementComponent> OriginalMovement = Movement;
		FString Error;
		if (OriginalMovement.IsValid() && OriginalObserver.IsSet()
			&& !OriginalMovement->UnsubscribeQualifiedMovementIntent(OriginalObserver, TEXT("PlayerCombo.ResourceReleased"), Error))
		{
			UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] original movement observer cleanup failed: %s"), *AbilityPath, *Error);
		}
		if (OriginalMovement.IsValid() && OriginalHandle != INDEX_NONE
			&& !OriginalMovement->ReleaseMontageActionMotion(OriginalHandle, Reason, Error))
		{
			UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] original Action handle %d cleanup failed: %s"), *AbilityPath, OriginalHandle, *Error);
		}
	}
};

UGGYGOPlayerComboAbility::UGGYGOPlayerComboAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	GroupTag = GGYGOGameplayTags::AbilityGroup_Attack;
	ActivationPriority = GGYGOAbilityGroupDefaults::Priority_LightAttack;
	SelfPolicy = EGGYGOAbilitySelfPolicy::Coexist;
	ActivationOwnedTags.AddTag(GGYGOGameplayTags::State_Attacking);
	ActivationBlockedTags.AddTag(GGYGOGameplayTags::State_Dead);
	ActivationBlockedTags.AddTag(GGYGOGameplayTags::Restriction_CantAttack);
	HitCueTag = GGYGOGameplayTags::GameplayCue_Hit_Flesh;
	bServerRespectsRemoteAbilityCancellation = false;
}

bool UGGYGOPlayerComboAbility::IsStepPlayable(int32 Index) const
{
	if (!ComboSteps.IsValidIndex(Index)) { return false; }
	const FGGYGOComboStep& Step = ComboSteps[Index];
	if (!Step.Montage || !FMath::IsFinite(Step.PlayRate) || Step.PlayRate <= 0.0f
		|| Step.MainSection == Step.EndSection || Step.MotionSlotName.IsNone()
		|| !FMath::IsFinite(Step.MotionTranslationScale) || Step.MotionTranslationScale <= 0.0f) { return false; }
	const int32 MainIndex = Step.Montage->GetSectionIndex(Step.MainSection);
	const int32 EndIndex = Step.Montage->GetSectionIndex(Step.EndSection);
	return MainIndex != INDEX_NONE && EndIndex != INDEX_NONE
		&& Step.Montage->GetAnimCompositeSection(MainIndex).GetTime() < Step.Montage->GetAnimCompositeSection(EndIndex).GetTime()
		&& Step.Montage->GetAnimCompositeSection(EndIndex).GetTime() < Step.Montage->GetPlayLength();
}

bool UGGYGOPlayerComboAbility::ValidateComboConfiguration(FString& OutError) const
{
	OutError.Reset();
	if (ComboSteps.IsEmpty()) { OutError = TEXT("ComboSteps 为空。"); return false; }
	if (!FMath::IsFinite(MontageCompletionGraceSeconds) || MontageCompletionGraceSeconds < 0.0f)
	{
		OutError = TEXT("MontageCompletionGraceSeconds 必须是非负有限秒数。");
		return false;
	}
	if (!FMath::IsFinite(InputBufferSeconds) || InputBufferSeconds < 0.0f || InputBufferSeconds > 1.0f)
	{
		OutError = TEXT("InputBufferSeconds 必须是 0 到 1 秒之间的有限值。");
		return false;
	}
	for (int32 Index = 0; Index < ComboSteps.Num(); ++Index)
	{
		const FGGYGOComboStep& Step = ComboSteps[Index];
		if (Step.Montage && (!Step.Montage->bEnableAutoBlendOut
			|| !FMath::IsFinite(Step.Montage->BlendOutTriggerTime)
			|| !FMath::IsFinite(Step.Montage->GetDefaultBlendOutTime()) || Step.Montage->GetDefaultBlendOutTime() < 0.0f))
		{
			OutError = FString::Printf(TEXT("段 %d Montage [%s] 必须启用自然混出，触发时间必须有限，混出时长必须有限且非负。"),
				Index, *GetPathNameSafe(Step.Montage));
			return false;
		}
		if (Step.MotionSlotName.IsNone() || !FMath::IsFinite(Step.MotionTranslationScale) || Step.MotionTranslationScale <= 0.0f)
		{
			OutError = FString::Printf(TEXT("段 %d Montage [%s] 必须显式配置 MotionSlotName，MotionTranslationScale 必须有限且大于0。"),
				Index, *GetPathNameSafe(Step.Montage));
			return false;
		}
		if (!IsStepPlayable(Index)
			|| !FMath::IsFinite(Step.Damage) || Step.Damage < 0.0f
			|| !FMath::IsFinite(Step.PoiseDamage) || Step.PoiseDamage < 0.0f)
		{
			OutError = FString::Printf(TEXT("段 %d Montage [%s] 的 Montage/Section/速率/伤害无效。"),
				Index, *GetPathNameSafe(Step.Montage));
			return false;
		}
		FString ShapeError;
		if (!UGGYGOMeleeTraceComponent::ValidateTraceShapeDefinition(Step.TraceShape, ShapeError))
		{
			OutError = FString::Printf(TEXT("段 %d Montage [%s] 的 TraceShape 无效：%s；旧 TraceStartSocket/TraceEndSocket/TraceRadius 仅历史存值，不参与运行，请显式配置 TraceShape。"),
				Index, *GetPathNameSafe(Step.Montage), *ShapeError);
			return false;
		}
		FGGYGOActionMotionSourceBindingPtr Source;
		FString MotionError;
		if (!GGYGOActionMotionSource::BuildSourceBindingRange(Step.Montage, Step.MotionSlotName,
			Step.MainSection, Step.EndSection, Source, MotionError)
			|| !GGYGOActionMotionEvaluation::ValidateSource(*Source, MotionError))
		{
			OutError = FString::Printf(TEXT("段 %d 的原动画动作来源无效：%s"), Index, *MotionError);
			return false;
		}
		if (Source->Sections.Num() != 2)
		{
			OutError = FString::Printf(TEXT("段 %d Montage [%s] 的 Main→End 必须相邻，原生默认路径不能跳过绑定内的 Section。"),
				Index, *GetPathNameSafe(Step.Montage));
			return false;
		}
		if (Step.NextStepIndex != INDEX_NONE && (Step.NextStepIndex <= Index || !ComboSteps.IsValidIndex(Step.NextStepIndex)))
		{
			OutError = FString::Printf(TEXT("段 %d 的 NextStepIndex 必须指向后面的有效段，终段使用 -1。"), Index);
			return false;
		}
	}
	return true;
}

#if WITH_EDITOR
EDataValidationResult UGGYGOPlayerComboAbility::IsDataValid(FDataValidationContext& Context) const
{
	const EDataValidationResult BaseResult = Super::IsDataValid(Context);
	FString Error;
	if (!ValidateComboConfiguration(Error))
	{
		Context.AddError(FText::FromString(Error));
		return EDataValidationResult::Invalid;
	}
	return BaseResult == EDataValidationResult::Invalid ? BaseResult : EDataValidationResult::Valid;
}
#endif

bool UGGYGOPlayerComboAbility::ValidateDamageEffectDependency(FString& OutError,
	TSubclassOf<UGameplayEffect>* OutResolvedEffect) const
{
	OutError.Reset();
	if (OutResolvedEffect) { *OutResolvedEffect = TSubclassOf<UGameplayEffect>(); }
	// Read the configured class before TSubclassOf's type filter can turn an invalid selection into null.
	TSubclassOf<UGameplayEffect> ConfiguredEffect = DamageEffect;
	const UClass* ConfiguredClass = ConfiguredEffect.GetGCPtr().Get();
	const auto IsChosenClassValid = [&OutError](const UClass* Class, const TCHAR* Mode, const TCHAR* Configuration)
	{
		if (!IsValid(Class) || !Class->IsChildOf(UGameplayEffect::StaticClass()))
		{
			OutError = FString::Printf(TEXT("模式=%s，配置=%s [%s]：已选GE类失效或不是GameplayEffect。"),
				Mode, Configuration, *GetPathNameSafe(Class));
			return false;
		}
		if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			OutError = FString::Printf(TEXT("模式=%s，配置=%s [%s]：已选GE类为抽象、已弃用或已被新版本替换。"),
				Mode, Configuration, *GetPathNameSafe(Class));
			return false;
		}
		return true;
	};
	if (ConfiguredClass && !IsChosenClassValid(ConfiguredClass, TEXT("Override"), TEXT("DamageEffect")))
	{
		return false; // An invalid explicit selection cannot enable shared or no-GE playback.
	}

	TSubclassOf<UGameplayEffect> ResolvedEffect = UGGYGOGameData::ResolveDamageGameplayEffect(
		ConfiguredEffect, bUseSharedDamageEffectWhenUnset);
	if (!ConfiguredClass && !bUseSharedDamageEffectWhenUnset)
	{
		return true; // Explicitly selected no-GE mode; no required damage dependency.
	}
	const UClass* ResolvedClass = ResolvedEffect.GetGCPtr().Get();
	if (!ResolvedClass)
	{
		OutError = TEXT("模式=Shared，配置=GameData.DamageGameplayEffect_SetByCaller：必需的共享预载GE不可用。");
		return false;
	}
	if (!IsChosenClassValid(ResolvedClass, ConfiguredClass ? TEXT("Override") : TEXT("Shared"),
		ConfiguredClass ? TEXT("DamageEffect") : TEXT("GameData.DamageGameplayEffect_SetByCaller")))
	{
		return false;
	}
	if (OutResolvedEffect) { *OutResolvedEffect = ResolvedEffect; }
	return true;
}

bool UGGYGOPlayerComboAbility::CanActivateAbilityAdditional(FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, const FGameplayTagContainer* SourceTags,
	const FGameplayTagContainer* TargetTags, FGameplayTagContainer* OptionalRelevantTags) const
{
	if (!Super::CanActivateAbilityAdditional(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags))
	{
		return false;
	}
	FString Error;
	return ValidateComboConfiguration(Error) && ValidateDamageEffectDependency(Error);
}

void UGGYGOPlayerComboAbility::NativeOnAbilityFailedToActivate(const FGameplayTagContainer& FailedReason) const
{
	FString Error;
	if (!ValidateComboConfiguration(Error))
	{
		// Pure CanActivate queries never log; report the actual rejected request once.
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] 激活失败时连段配置无效：%s"),
			*GetPathNameSafe(this), *Error);
	}
	else if (!ValidateDamageEffectDependency(Error))
	{
		// This is an actual failed request; repeated pure CanActivate queries never log.
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] 激活失败时伤害GE依赖无效：%s"),
			*GetPathNameSafe(this), *Error);
	}
	Super::NativeOnAbilityFailedToActivate(FailedReason);
}

void UGGYGOPlayerComboAbility::InitializeAbilityActivation(const FGGYGOAbilityActivationHandle& Original)
{
	if (!Original.HasActivation() || !CaptureCurrentActivation().HasSameActivation(Original))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] Initialize rejected: missing or invalid GA original activation."),
			*GetPathNameSafe(this));
		return;
	}
	if (ResourceActivation.HasActivation() || StepMotionResources.IsValid() || MontageTask || InputTask || TraceComponent || ActiveMesh
		|| MontageCallbackRegistration.IsValid() || InputCallbackRegistration.IsValid()
		|| TraceWindow.HasWindow() || TraceHitSubscription.IsValid() || WatchdogHandle.IsValid()
		|| OriginalWorld.IsValid() || bChangedMeshTick || bAddedMeshPrerequisite)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] Initialize rejected: original resources have not been released."),
			*GetPathNameSafe(this));
		RequestAbilityEnd(Original, true, true);
		return;
	}
	ResourceActivation = Original;
	Window.Reset();
	CurrentStep = INDEX_NONE;
	LastRequestId = 0;
	StepSyncRevision = 0;
	CurrentStepToken = 0;
	Super::InitializeAbilityActivation(Original);
}

void UGGYGOPlayerComboAbility::ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
	FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	if (!IsActivationCurrent(Original))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] Body rejected: GA original activation/Initialize prerequisite is not current."),
			*GetPathNameSafe(this));
		if (Original.HasActivation()) { RequestAbilityEnd(Original, true, true); }
		return;
	}
	Super::ActivateAbilityBody(Original, Handle, ActorInfo, ActivationInfo, TriggerEventData);
	if (!IsActivationCurrent(Original)) { return; }

	// Blueprint activation is an external callback; revalidate before committing the original.
	FString ConfigurationError;
	if (!ValidateComboConfiguration(ConfigurationError))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] 提交前连段配置无效：%s"),
			*GetPathNameSafe(this), *ConfigurationError);
		RequestAbilityEnd(Original, true, true);
		return;
	}

	FString DamageDependencyError;
	if (!ValidateDamageEffectDependency(DamageDependencyError))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] 提交前拒绝激活：%s"),
			*GetPathNameSafe(this), *DamageDependencyError);
		RequestAbilityEnd(Original, true, true);
		return;
	}

	ACharacter* Character = GetCharacterFromActorInfo();
	ActiveMesh = Character ? Character->GetMesh() : nullptr;
	TraceComponent = Character ? Character->FindComponentByClass<UGGYGOMeleeTraceComponent>() : nullptr;
	OriginalWorld = GetWorld();
	if (!Character || !Cast<UGGYGOCharacterMovementComponent>(Character->GetCharacterMovement())
		|| !IsValid(ActiveMesh) || !IsValid(TraceComponent) || !OriginalWorld.IsValid() || !IsStepPlayable(0))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] Body rejected: Avatar/Mesh [%s], Trace [%s], World [%s] or first step is invalid."),
			*GetPathNameSafe(this), *GetPathNameSafe(ActiveMesh), *GetPathNameSafe(TraceComponent), *GetPathNameSafe(OriginalWorld.Get()));
		RequestAbilityEnd(Original, true, true);
		return;
	}
	const bool bCommitted = CommitAbility(Handle, ActorInfo, ActivationInfo);
	if (!IsActivationCurrent(Original)) { return; }
	if (!bCommitted)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo：激活提交失败。"));
		RequestAbilityEnd(Original, true, true);
		return;
	}
	if (!IsValid(ActiveMesh) || !IsValid(TraceComponent) || !OriginalWorld.IsValid())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] original Mesh/Trace/World became invalid during Commit."), *GetPathNameSafe(this));
		RequestAbilityEnd(Original, true, true);
		return;
	}

	if (HasAuthority(&ActivationInfo))
	{
		SavedMeshTick = ActiveMesh->VisibilityBasedAnimTickOption;
		bSavedUpdateRateOptimizations = ActiveMesh->bEnableUpdateRateOptimizations;
		ActiveMesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		ActiveMesh->bEnableUpdateRateOptimizations = false;
		bChangedMeshTick = true;
		const auto HasMeshPrerequisite = [Trace = TraceComponent.Get(), Mesh = ActiveMesh.Get()]()
		{
			return Trace->PrimaryComponentTick.GetPrerequisites().ContainsByPredicate([Mesh](const FTickPrerequisite& Prerequisite)
			{
				return Prerequisite.PrerequisiteObject.Get() == Mesh && Prerequisite.Get() == &Mesh->PrimaryComponentTick;
			});
		};
		if (!HasMeshPrerequisite())
		{
			TraceComponent->AddTickPrerequisiteComponent(ActiveMesh);
			if (!IsActivationCurrent(Original)) { return; }
			bAddedMeshPrerequisite = HasMeshPrerequisite();
			if (!bAddedMeshPrerequisite)
			{
				UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] Trace [%s] failed to add original Mesh [%s] tick prerequisite."),
					*GetPathNameSafe(this), *GetPathNameSafe(TraceComponent), *GetPathNameSafe(ActiveMesh));
				RequestAbilityEnd(Original, true, true);
				return;
			}
		}
	}
	UGGYGOAbilityTask_WaitComboInput* const StartedInputTask = UGGYGOAbilityTask_WaitComboInput::WaitComboInput(this);
	if (!IsActivationCurrent(Original))
	{
		if (IsValid(StartedInputTask)) { StartedInputTask->TaskOwnerEnded(); }
		return;
	}
	if (!IsValid(StartedInputTask))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] WaitComboInput factory failed."), *GetPathNameSafe(this));
		RequestAbilityEnd(Original, true, true);
		return;
	}
	InputTask = StartedInputTask;
	const TWeakObjectPtr<ThisClass> WeakThis(this);
	const TWeakObjectPtr<UGGYGOAbilityTask_WaitComboInput> WeakInputTask(StartedInputTask);
	const FDelegateHandle Registration = StartedInputTask->RegisterNativeCallback(
		FGGYGOComboInputNativeDelegate::CreateLambda([WeakThis, WeakInputTask, Original](int32 SourceStep, int32 RequestId)
		{
			ThisClass* Self = WeakThis.Get();
			if (Self && WeakInputTask.IsValid() && Self->InputTask == WeakInputTask.Get() && Self->IsActivationCurrent(Original))
			{
				Self->HandleInputPressed(Original, SourceStep, RequestId);
			}
		}));
	if (!IsActivationCurrent(Original) || InputTask != StartedInputTask)
	{
		if (WeakInputTask.IsValid())
		{
			StartedInputTask->UnregisterNativeCallback(Registration);
			if (WeakInputTask.IsValid()) { StartedInputTask->TaskOwnerEnded(); }
		}
		return;
	}
	InputCallbackRegistration = Registration;
	if (!Registration.IsValid())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] Input Task [%s] pre-Ready native registration failed."),
			*GetPathNameSafe(this), *GetPathNameSafe(StartedInputTask));
		RequestAbilityEnd(Original, true, true);
		return;
	}
	StartStep(Original, 0);
	if (!IsActivationCurrent(Original) || InputTask != StartedInputTask) { return; }
	if (!MontageTask || !ComboSteps.IsValidIndex(CurrentStep))
	{
		RequestAbilityEnd(Original, true, true);
		return;
	}
	if (!WeakInputTask.IsValid())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] original Input Task became invalid before Ready."), *GetPathNameSafe(this));
		RequestAbilityEnd(Original, true, true);
		return;
	}

	StartedInputTask->ReadyForActivation();
	if (!IsActivationCurrent(Original) || InputTask != StartedInputTask) { return; }
	if (!WeakInputTask.IsValid() || !StartedInputTask->IsActive())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] original Input Task did not remain active after Ready."), *GetPathNameSafe(this));
		RequestAbilityEnd(Original, true, true);
	}
}

bool UGGYGOPlayerComboAbility::IsActivationCurrent(const FGGYGOAbilityActivationHandle& Original) const
{
	return ResourceActivation.HasSameActivation(Original)
		&& CaptureCurrentActivation().HasSameActivation(Original);
}

bool UGGYGOPlayerComboAbility::IsStepCurrent(const FGGYGOAbilityActivationHandle& Original,
	uint64 ExpectedStepToken, const UGGYGOAbilityTask_PlayMontageAndWaitForEvent* ExpectedTask) const
{
	return IsActivationCurrent(Original) && ExpectedStepToken != 0 && CurrentStepToken == ExpectedStepToken
		&& IsValid(ExpectedTask) && MontageTask == ExpectedTask;
}

bool UGGYGOPlayerComboAbility::IsMotionResourceCurrent(const TSharedPtr<FStepMotionResources>& Resource) const
{
	return Resource.IsValid() && StepMotionResources == Resource && Resource->Section != FStepMotionResources::ESection::Retired
		&& CurrentStep == Resource->StepIndex
		&& IsActivationCurrent(Resource->Activation) && Resource->StepToken != 0 && CurrentStepToken == Resource->StepToken
		// Completed is authenticated while the original Task is live. Its subsequent EndTask
		// may invalidate the weak sender before CMC consumes the final native movement interval.
		&& (Resource->bMontageCompleted || IsStepCurrent(Resource->Activation, Resource->StepToken, Resource->Task.Get()));
}

void UGGYGOPlayerComboAbility::FailStepMotion(const TSharedPtr<FStepMotionResources>& Resource, const FString& Reason)
{
	if (!IsMotionResourceCurrent(Resource)) { return; }
	UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] Step %d Montage [%s] Slot [%s] CMC [%s] motion failed: %s"),
		*Resource->AbilityPath, Resource->StepIndex, *GetPathNameSafe(Resource->Source->Montage.Get()),
		*Resource->Source->SlotName.ToString(), *GetPathNameSafe(Resource->Movement.Get()), *Reason);
	// A required dependency failure must end this original even if user cancellation is disabled.
	RequestAbilityEnd(Resource->Activation, true, true);
}

bool UGGYGOPlayerComboAbility::InitializeStepMotion(const TSharedPtr<FStepMotionResources>& Resource)
{
	if (!IsMotionResourceCurrent(Resource)) { return false; }
	FGGYGOMontageSectionSnapshot Snapshot;
	if (!Resource->Task->TryGetOriginalSectionSnapshot(Snapshot)
		|| Snapshot.Montage != Resource->Source->Montage.Get() || Snapshot.MontageInstanceId == INDEX_NONE
		|| !FMath::IsFinite(Snapshot.PositionSeconds) || !FMath::IsFinite(Snapshot.InstancePlayRate)
		|| Snapshot.InstancePlayRate <= 0.0f
		|| Snapshot.InstancePlayRate * Resource->Source->MontageRateScale != Resource->EffectiveRate)
	{
		FailStepMotion(Resource, TEXT("Original Task section/position/rate snapshot is unavailable or differs from the fixed playback rate."));
		return false;
	}
	if (!IsMotionResourceCurrent(Resource)) { return false; }
	Resource->MontageInstanceId = Snapshot.MontageInstanceId;
	const auto* MainSource = Resource->Source->FindSection(Resource->Source->SectionName);
	const auto* EndSource = Resource->Source->FindSection(Resource->EndSection);
	if (!MainSource || !EndSource || Resource->Source->Sections.Num() != 2)
	{
		FailStepMotion(Resource, TEXT("Original binding does not contain the configured adjacent Main→End sections."));
		return false;
	}
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("[Combat.PlayerCombo.EndDiagnostics] Boundary=OriginalSnapshot Ability='%s' Step=%d StepToken=%llu Montage='%s' Instance=%d Section='%s' Position=%.9g Main='%s' MainRange=[%.9g,%.9g] End='%s' EndRangeAvailable=%d EndRange=[%.9g,%.9g] MontageLength=%.9g EffectiveRate=%.9g"),
		*Resource->AbilityPath, Resource->StepIndex, static_cast<unsigned long long>(Resource->StepToken),
		*GetPathNameSafe(Snapshot.Montage), Snapshot.MontageInstanceId, *Snapshot.SectionName.ToString(), Snapshot.PositionSeconds,
		*Resource->Source->SectionName.ToString(), MainSource->MontageStartSeconds, MainSource->MontageEndSeconds,
		*Resource->EndSection.ToString(), true, EndSource->MontageStartSeconds, EndSource->MontageEndSeconds,
		Resource->Source->MontageLength, Resource->EffectiveRate);
	FString Error;
	if (!Resource->Movement.IsValid() || !Resource->Movement->GetMovementOwnerSyncScope(Resource->Scope, Error))
	{
		FailStepMotion(Resource, FString::Printf(TEXT("Original movement owner scope is unavailable: %s"), *Error));
		return false;
	}
	const bool bStartsInEnd = Snapshot.SectionName == Resource->EndSection;
	if (!bStartsInEnd && Snapshot.SectionName != Resource->Source->SectionName)
	{
		FailStepMotion(Resource, FString::Printf(TEXT("Original initial section [%s] is neither configured Main nor End."), *Snapshot.SectionName.ToString()));
		return false;
	}
	Resource->Section = FStepMotionResources::ESection::Main;
	const bool bStarted = Resource->Movement->BeginMontageActionMotion(Resource->Source, Snapshot.MontageInstanceId,
		Snapshot.PositionSeconds, Resource->EffectiveRate, Resource->TranslationScale, Resource->MotionHandle, Error);
	if (!IsMotionResourceCurrent(Resource))
	{
		Resource->Release(EGGYGOActionMotionReleaseReason::OwnerInvalidated);
		return false;
	}
	if (!bStarted || Resource->MotionHandle == INDEX_NONE)
	{
		FailStepMotion(Resource, FString::Printf(TEXT("Original Main→End action request was rejected: %s"), *Error));
		return false;
	}
	const TWeakObjectPtr<ThisClass> WeakThis(this);
	const TWeakPtr<FStepMotionResources> WeakResource(Resource);
	const bool bObserved = Resource->Movement->ObserveMontageActionMotionFailure(Resource->MotionHandle,
		FGGYGOActionMotionFailureDelegate::CreateLambda([WeakThis, WeakResource](int32 OriginalHandle, const FString& Diagnostic)
		{
			ThisClass* Self = WeakThis.Get();
			const TSharedPtr<FStepMotionResources> OriginalResource = WeakResource.Pin();
			if (Self && Self->IsMotionResourceCurrent(OriginalResource) && OriginalResource->MotionHandle == OriginalHandle)
			{
				Self->FailStepMotion(OriginalResource, Diagnostic);
			}
		}), Error);
	if (!IsMotionResourceCurrent(Resource)) { return false; }
	if (!bObserved)
	{
		FailStepMotion(Resource, FString::Printf(TEXT("Original action resource failure observation could not be installed: %s"), *Error));
		return false;
	}
	const bool bCompletionObserved = Resource->Movement->ObserveMontageActionMotionCompletion(Resource->MotionHandle,
		FGGYGOActionMotionCompletionDelegate::CreateLambda([WeakThis, WeakResource](int32 OriginalHandle)
		{
			ThisClass* Self = WeakThis.Get();
			const TSharedPtr<FStepMotionResources> OriginalResource = WeakResource.Pin();
			if (!Self || !Self->IsMotionResourceCurrent(OriginalResource)
				|| OriginalResource->MotionHandle != OriginalHandle || OriginalResource->bMotionCompleted) { return; }
			OriginalResource->bMotionCompleted = true;
			Self->TryCompleteStepMotion(OriginalResource);
		}), Error);
	if (!IsMotionResourceCurrent(Resource)) { return false; }
	if (!bCompletionObserved)
	{
		FailStepMotion(Resource, FString::Printf(TEXT("Original action resource completion observation could not be installed: %s"), *Error));
		return false;
	}
	// A correction starting inside End still executes its genuine remaining trajectory.
	if (bStartsInEnd) { EnterStepEnd(Resource); }
	return IsMotionResourceCurrent(Resource);
}

void UGGYGOPlayerComboAbility::HandleMontageSection(const TSharedPtr<FStepMotionResources>& Resource,
	const FGGYGOMontageSectionFact& Fact)
{
	if (!IsMotionResourceCurrent(Resource) || Resource->MontageInstanceId == INDEX_NONE) { return; }
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("[Combat.PlayerCombo.EndDiagnostics] Boundary=SectionReceived Ability='%s' Step=%d StepToken=%llu Montage='%s' Instance=%d Section='%s' Looped=%d ExpectedInstance=%d End='%s' DerivedSection=%d"),
		*Resource->AbilityPath, Resource->StepIndex, static_cast<unsigned long long>(Resource->StepToken),
		*GetPathNameSafe(Fact.Montage), Fact.MontageInstanceId, *Fact.SectionName.ToString(), Fact.bLooped,
		Resource->MontageInstanceId, *Resource->EndSection.ToString(), static_cast<int32>(Resource->Section));
	if (Fact.Montage != Resource->Source->Montage.Get() || Fact.MontageInstanceId != Resource->MontageInstanceId)
	{
		FailStepMotion(Resource, TEXT("Section fact does not identify this step's original playback."));
		return;
	}
	if (Fact.bLooped)
	{
		FailStepMotion(Resource, TEXT("Original Combo section loop is not an allowed Main→End path."));
		return;
	}
	if (Fact.SectionName == Resource->EndSection) { EnterStepEnd(Resource); }
	else if (Fact.SectionName != Resource->Source->SectionName)
	{
		FailStepMotion(Resource, FString::Printf(TEXT("Original playback entered unconfigured section [%s]."), *Fact.SectionName.ToString()));
	}
	// A queued initial Main fact is history. It never reopens Main after an accepted End fact.
}

void UGGYGOPlayerComboAbility::EnterStepEnd(const TSharedPtr<FStepMotionResources>& Resource)
{
	if (!IsMotionResourceCurrent(Resource) || Resource->Section == FStepMotionResources::ESection::End) { return; }
	Resource->Section = FStepMotionResources::ESection::End;
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("[Combat.PlayerCombo.EndDiagnostics] Boundary=EnterEnd Ability='%s' Step=%d StepToken=%llu Instance=%d MotionHandle=%d Scope=%llu"),
		*Resource->AbilityPath, Resource->StepIndex, static_cast<unsigned long long>(Resource->StepToken),
		Resource->MontageInstanceId, Resource->MotionHandle, static_cast<unsigned long long>(Resource->Scope.GetScopeSerial()));
	FString Error;
	if (!Resource->Movement.IsValid())
	{
		FailStepMotion(Resource, TEXT("Original CMC expired at End."));
		return;
	}
	// Entering End changes interruption permission; the same CMC handle retains the trajectory.
	const TWeakObjectPtr<ThisClass> WeakThis(this);
	const TWeakPtr<FStepMotionResources> WeakResource(Resource);
	const auto ReceiveIntent = [WeakThis, WeakResource](EGGYGOQualifiedMovementIntentQueryResult Result,
		const FGGYGOQualifiedMovementIntent& Intent, const FString& Diagnostic, const TCHAR* Delivery)
	{
		ThisClass* Self = WeakThis.Get();
		const TSharedPtr<FStepMotionResources> OriginalResource = WeakResource.Pin();
		if (!Self || !Self->IsMotionResourceCurrent(OriginalResource)
			|| OriginalResource->Section != FStepMotionResources::ESection::End) { return; }
		// Read the exact Task once at this boundary; an unavailable snapshot is diagnostic only.
		FGGYGOMontageSectionSnapshot CurrentSnapshot;
		const auto* OriginalTask = OriginalResource->Task.Get();
		const bool bSnapshotAvailable = OriginalTask && OriginalTask->TryGetOriginalSectionSnapshot(CurrentSnapshot);
		const FString OriginalLogContext = FString::Printf(
			TEXT("Ability='%s' Step=%d StepToken=%llu Montage='%s' Instance=%d MotionHandle=%d Scope=%llu"),
			*OriginalResource->AbilityPath, OriginalResource->StepIndex, static_cast<unsigned long long>(OriginalResource->StepToken),
			*GetPathNameSafe(OriginalResource->Source->Montage.Get()), OriginalResource->MontageInstanceId,
			OriginalResource->MotionHandle, static_cast<unsigned long long>(OriginalResource->Scope.GetScopeSerial()));
		UE_LOG(LogGGYGOAbilitySystem, Display,
			TEXT("[Combat.PlayerCombo.EndDiagnostics] Boundary=EndIntent Delivery=%s %s Result=%s(%d) Provenance=%d IntentScope=%llu Binding=%llu Session=%llu Request=%llu Execution=%llu SnapshotAvailable=%d SnapshotSection='%s' Position=%.9g MontageLength=%.9g Diagnostic='%s'"),
			Delivery, *OriginalLogContext, DescribeQualifiedMovementResult(Result), static_cast<int32>(Result), static_cast<int32>(Intent.Provenance),
			static_cast<unsigned long long>(Intent.Scope.GetScopeSerial()), static_cast<unsigned long long>(Intent.BindingSerial),
			static_cast<unsigned long long>(Intent.SessionSerial), static_cast<unsigned long long>(Intent.RequestSerial),
			static_cast<unsigned long long>(Intent.ExecutionRequestSerial), bSnapshotAvailable,
			*CurrentSnapshot.SectionName.ToString(), CurrentSnapshot.PositionSeconds, OriginalResource->Source->MontageLength, *Diagnostic);
		if (Result == EGGYGOQualifiedMovementIntentQueryResult::Unavailable
			|| Result == EGGYGOQualifiedMovementIntentQueryResult::ExecutionFailed)
		{
			Self->FailStepMotion(OriginalResource, FString::Printf(TEXT("Original End movement dependency failed: %s"), *Diagnostic));
			return;
		}
		if (Result != EGGYGOQualifiedMovementIntentQueryResult::Qualified) { return; }
		if (Intent.Scope != OriginalResource->Scope || !OriginalResource->Movement.IsValid())
		{
			Self->FailStepMotion(OriginalResource, TEXT("Qualified End intent belongs to a different owner scope or expired CMC."));
			return;
		}
		FString CancelError;
		FGGYGOQualifiedMovementIntent CurrentIntent;
		const auto CurrentResult = OriginalResource->Movement->QueryQualifiedMovementIntent(OriginalResource->Scope, CurrentIntent, CancelError);
		UE_LOG(LogGGYGOAbilitySystem, Display,
			TEXT("[Combat.PlayerCombo.EndDiagnostics] Boundary=IntentRevalidation %s Result=%s(%d) SameIntent=%d Diagnostic='%s'"),
			*OriginalLogContext, DescribeQualifiedMovementResult(CurrentResult), static_cast<int32>(CurrentResult), CurrentIntent == Intent, *CancelError);
		if (CurrentResult != EGGYGOQualifiedMovementIntentQueryResult::Qualified || !(CurrentIntent == Intent))
		{
			if (CurrentResult == EGGYGOQualifiedMovementIntentQueryResult::Unavailable
				|| CurrentResult == EGGYGOQualifiedMovementIntentQueryResult::ExecutionFailed)
			{
				Self->FailStepMotion(OriginalResource, FString::Printf(TEXT("Original End intent revalidation failed: %s"), *CancelError));
			}
			return; // A retired/replaced notification cannot cancel with a successor request.
		}
		if (OriginalResource->MotionHandle != INDEX_NONE)
		{
			const bool bMotionCancelled = OriginalResource->Movement->CancelMontageActionMotionForMovement(OriginalResource->MotionHandle, Intent, CancelError);
			UE_LOG(LogGGYGOAbilitySystem, Display,
				TEXT("[Combat.PlayerCombo.EndDiagnostics] Boundary=MotionCancellation %s Succeeded=%d Diagnostic='%s'"),
				*OriginalLogContext, bMotionCancelled, *CancelError);
			if (!bMotionCancelled)
			{
				Self->FailStepMotion(OriginalResource, FString::Printf(TEXT("Original End movement cancellation was rejected: %s"), *CancelError));
				return;
			}
		}
		if (!Self->IsMotionResourceCurrent(OriginalResource)) { return; }
		OriginalResource->MotionHandle = INDEX_NONE;
		// The original CMC cancellation precedes this GA's selected End lifecycle rule.
		const FGGYGOAbilityTerminationResult EndResult = Self->RequestAbilityEnd(OriginalResource->Activation, true, true);
		switch (EndResult.Outcome)
		{
		case EGGYGOAbilityTerminationOutcome::Completed:
		case EGGYGOAbilityTerminationOutcome::Accepted:
		case EGGYGOAbilityTerminationOutcome::Deferred:
		case EGGYGOAbilityTerminationOutcome::AlreadyPending:
			UE_LOG(LogGGYGOAbilitySystem, Display,
				TEXT("[Combat.PlayerCombo.EndDiagnostics] Boundary=AbilityEndResult %s Outcome=%d Reason=%d"),
				*OriginalLogContext, static_cast<int32>(EndResult.Outcome), static_cast<int32>(EndResult.Reason));
			break;
		default:
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("[Combat.PlayerCombo.EndDiagnostics] Boundary=AbilityEndResult %s Outcome=%d Reason=%d; original End request was not accepted"),
				*OriginalLogContext, static_cast<int32>(EndResult.Outcome), static_cast<int32>(EndResult.Reason));
			break;
		}
		// Only local immutable diagnostic data is accessed after the external End request.
	};
	FGGYGOQualifiedMovementIntent InitialIntent;
	const auto InitialResult = Resource->Movement->QueryQualifiedMovementIntent(Resource->Scope, InitialIntent, Error);
	ReceiveIntent(InitialResult, InitialIntent, Error, TEXT("EndEntry"));
	if (!IsMotionResourceCurrent(Resource)) { return; }
	const bool bSubscribed = Resource->Movement->SubscribeQualifiedMovementIntent(Resource->Scope,
		FGGYGOQualifiedMovementIntentDelegate::CreateLambda(
			[WeakThis, WeakResource, ReceiveIntent](const FGGYGOQualifiedMovementIntentObserverId& OriginalObserver,
				EGGYGOQualifiedMovementIntentQueryResult Result, const FGGYGOQualifiedMovementIntent& Intent, const FString& Diagnostic)
			{
				ThisClass* Self = WeakThis.Get();
				const TSharedPtr<FStepMotionResources> OriginalResource = WeakResource.Pin();
				if (Self && Self->IsMotionResourceCurrent(OriginalResource)
					&& OriginalResource->Observer == OriginalObserver && OriginalObserver.GetScope() == OriginalResource->Scope)
				{
					ReceiveIntent(Result, Intent, Diagnostic, TEXT("Observer"));
				}
			}), Resource->Observer, Error);
	// OutObserver is installed before Replay, so reentrant termination can clean the original record.
	if (!IsMotionResourceCurrent(Resource)) { return; }
	if (!bSubscribed || !Resource->Observer.IsSet())
	{
		FailStepMotion(Resource, FString::Printf(TEXT("Original End movement subscription failed: %s"), *Error));
	}
}

void UGGYGOPlayerComboAbility::ReleaseStepMotion(EGGYGOActionMotionReleaseReason Reason)
{
	const TSharedPtr<FStepMotionResources> Original = MoveTemp(StepMotionResources);
	if (Original.IsValid()) { Original->Release(Reason); }
}

bool UGGYGOPlayerComboAbility::StartStep(const FGGYGOAbilityActivationHandle& Original, int32 Index, float Position)
{
	if (!IsActivationCurrent(Original)) { return false; }
	if (!IsStepPlayable(Index) || !IsValid(ActiveMesh) || !IsValid(TraceComponent) || !OriginalWorld.IsValid())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] StartStep %d rejected: step/Mesh/Trace/original World is invalid."),
			*GetPathNameSafe(this), Index);
		RequestAbilityCancel(Original, true);
		return false;
	}
	if (StepTokenCounter == MAX_uint64)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo：段激活令牌已耗尽。"));
		RequestAbilityCancel(Original, true);
		return false;
	}
	const FGGYGOComboStep Step = ComboSteps[Index];
	FGGYGOActionMotionSourceBindingPtr MotionSource;
	FString MotionError;
	UGGYGOCharacterMovementComponent* const Movement = Cast<UGGYGOCharacterMovementComponent>(
		GetCharacterFromActorInfo() ? GetCharacterFromActorInfo()->GetCharacterMovement() : nullptr);
	if (!Movement || !GGYGOActionMotionSource::BuildSourceBindingRange(Step.Montage, Step.MotionSlotName,
		Step.MainSection, Step.EndSection, MotionSource, MotionError))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] step %d Montage [%s] Slot [%s] motion rejected: CMC [%s], %s"),
			*GetPathNameSafe(this), Index, *GetPathNameSafe(Step.Montage), *Step.MotionSlotName.ToString(), *GetPathNameSafe(Movement), *MotionError);
		RequestAbilityEnd(Original, true, true);
		return false;
	}
	FString ShapeError;
	if (!TraceComponent->ValidateTraceShapeOnCurrentMesh(Step.TraceShape, ActiveMesh.Get(), ShapeError))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] StartStep %d Montage [%s] Trace [%s] Mesh [%s] 的 TraceShape 无效：%s"),
			*GetPathNameSafe(this), Index, *GetPathNameSafe(Step.Montage),
			*GetPathNameSafe(TraceComponent.Get()), *GetPathNameSafe(ActiveMesh.Get()), *ShapeError);
		RequestAbilityCancel(Original, true);
		return false;
	}
	const TWeakObjectPtr<UWorld> StepWorld = OriginalWorld;
	const uint64 PreviousStepToken = CurrentStepToken;
	FTimerHandle PreviousWatchdog = WatchdogHandle;
	WatchdogHandle.Invalidate();
	StepWorld->GetTimerManager().ClearTimer(PreviousWatchdog);
	if (!IsActivationCurrent(Original) || CurrentStepToken != PreviousStepToken) { return false; }
	ReleaseTraceWindow(Original);
	if (!IsActivationCurrent(Original) || CurrentStepToken != PreviousStepToken) { return false; }
	ReleaseStepMotion(EGGYGOActionMotionReleaseReason::Replaced);
	if (!IsActivationCurrent(Original) || CurrentStepToken != PreviousStepToken) { return false; }
	ReleaseMontageTask(Original);
	if (!IsActivationCurrent(Original) || CurrentStepToken != PreviousStepToken) { return false; }
	Window.Reset();
	CurrentStep = Index;
	const uint64 ThisStepToken = ++StepTokenCounter;
	CurrentStepToken = ThisStepToken;
	if (InputTask) { InputTask->SetSourceStep(Index); }
	FGameplayTagContainer Events;
	Events.AddTag(GGYGOGameplayTags::Event_Montage_ComboWindowBegin);
	Events.AddTag(GGYGOGameplayTags::Event_Montage_ComboWindowEnd);
	Events.AddTag(GGYGOGameplayTags::Event_Montage_HitWindowBegin);
	Events.AddTag(GGYGOGameplayTags::Event_Montage_HitWindowEnd);
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* const StartedMontageTask =
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent::PlayMontageAndWaitForEvent(
		this, TEXT("PlayerCombo"), Step.Montage, Events, Step.PlayRate,
		Position > 0.0f ? NAME_None : Step.MainSection);
	if (!IsActivationCurrent(Original) || CurrentStepToken != ThisStepToken)
	{
		if (IsValid(StartedMontageTask)) { StartedMontageTask->TaskOwnerEnded(); }
		return false;
	}
	if (!IsValid(StartedMontageTask))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] step %d Montage Task factory failed for [%s]."),
			*GetPathNameSafe(this), Index, *GetPathNameSafe(Step.Montage));
		RequestAbilityCancel(Original, true);
		return false;
	}
	MontageTask = StartedMontageTask;
	const TSharedPtr<FStepMotionResources> Motion = MakeShared<FStepMotionResources>();
	Motion->Activation = Original;
	Motion->StepToken = ThisStepToken;
	Motion->StepIndex = Index;
	Motion->Task = StartedMontageTask;
	Motion->Movement = Movement;
	Motion->Source = MoveTemp(MotionSource);
	Motion->TranslationScale = Step.MotionTranslationScale;
	Motion->EndSection = Step.EndSection;
	Motion->AbilityPath = GetPathNameSafe(this);
	StepMotionResources = Motion;
	const float EffectivePlayRate = StartedMontageTask->GetEffectivePlayRate();
	Motion->EffectiveRate = EffectivePlayRate;
	const float MontageLength = Step.Montage->GetPlayLength();
	if (!FMath::IsFinite(EffectivePlayRate) || EffectivePlayRate <= 0.0f
		|| !FMath::IsFinite(MontageLength) || MontageLength <= 0.0f)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo：段 %d 的实际播放速率或时长无效。"), Index);
		RequestAbilityCancel(Original, true);
		return false;
	}
	StartedMontageTask->SetStartTimeSeconds(Position);
	const TWeakObjectPtr<ThisClass> WeakThis(this);
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> WeakTask(StartedMontageTask);
	const TWeakPtr<FStepMotionResources> WeakMotion(Motion);
	const auto ResolveOriginalCaller = [WeakThis, WeakTask, Original, ThisStepToken]() -> ThisClass*
	{
		ThisClass* Self = WeakThis.Get();
		return Self && Self->IsStepCurrent(Original, ThisStepToken, WeakTask.Get()) ? Self : nullptr;
	};
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent::FNativeCallbacks Callbacks;
	Callbacks.OnCompleted = FGGYGOPlayMontageAndWaitForEventDelegate::CreateLambda(
		[ResolveOriginalCaller, WeakMotion](FGameplayTag Tag, FGameplayEventData Data)
		{
			if (ThisClass* Self = ResolveOriginalCaller()) { Self->HandleMontageCompleted(WeakMotion.Pin()); }
		});
	Callbacks.OnInterrupted = FGGYGOPlayMontageAndWaitForEventDelegate::CreateLambda(
		[ResolveOriginalCaller, Original](FGameplayTag Tag, FGameplayEventData Data)
		{
			if (ThisClass* Self = ResolveOriginalCaller()) { Self->HandleMontageInterrupted(Original, Tag, Data); }
		});
	Callbacks.OnCancelled = Callbacks.OnInterrupted;
	Callbacks.OnBlendOut = FGGYGOPlayMontageAndWaitForEventDelegate::CreateLambda(
		[ResolveOriginalCaller, Original](FGameplayTag Tag, FGameplayEventData Data)
		{
			if (ThisClass* Self = ResolveOriginalCaller()) { Self->HandleMontageBlendOut(Original, Tag, Data); }
		});
	Callbacks.EventReceived = FGGYGOPlayMontageAndWaitForEventDelegate::CreateLambda(
		[ResolveOriginalCaller, Original](FGameplayTag Tag, FGameplayEventData Data)
		{
			if (ThisClass* Self = ResolveOriginalCaller()) { Self->HandleMontageEvent(Original, Tag, Data); }
		});
	Callbacks.SectionReceived = FGGYGOMontageSectionFactDelegate::CreateLambda(
		[WeakThis, WeakMotion](const FGGYGOMontageSectionFact& Fact)
		{
			if (ThisClass* Self = WeakThis.Get()) { Self->HandleMontageSection(WeakMotion.Pin(), Fact); }
		});
	Callbacks.OnFailed = FGGYGOMontageTaskFailureDelegate::CreateLambda(
		[WeakThis, WeakTask, WeakMotion, Original, ThisStepToken](const FGGYGOMontageTaskFailureFact& Fact)
		{
			ThisClass* Self = WeakThis.Get();
			UGGYGOAbilityTask_PlayMontageAndWaitForEvent* const OriginalTask = WeakTask.Get(true);
			const TSharedPtr<FStepMotionResources> OriginalResource = WeakMotion.Pin();
			// Native EndTask marks this historical sender Garbage. Admit its exact remaining object,
			// never a destroyed sender, and authenticate the GA's own original resources without live-task helpers.
			if (!Self || !OriginalTask || OriginalTask->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
				|| !OriginalResource.IsValid() || !Self->IsActivationCurrent(Original)
				|| ThisStepToken == 0 || Self->CurrentStepToken != ThisStepToken
				|| Self->MontageTask.Get() != OriginalTask || Self->StepMotionResources != OriginalResource
				|| OriginalResource->Section == FStepMotionResources::ESection::Retired
				|| Self->CurrentStep != OriginalResource->StepIndex || OriginalResource->StepToken != ThisStepToken
				|| !OriginalResource->Activation.HasSameActivation(Original) || OriginalResource->Task.Get(true) != OriginalTask) { return; }
			const bool bStartup = Fact.Stage == EGGYGOMontageTaskFailureStage::Startup;
			const bool bPlayback = Fact.Stage == EGGYGOMontageTaskFailureStage::Playback;
			const bool bOriginalInstance = bStartup
				? Fact.MontageInstanceId == INDEX_NONE && OriginalResource->MontageInstanceId == INDEX_NONE
				: bPlayback && OriginalResource->MontageInstanceId != INDEX_NONE
					&& Fact.MontageInstanceId == OriginalResource->MontageInstanceId;
			const FString OriginalLogContext = FString::Printf(
				TEXT("Ability='%s' Step=%d StepToken=%llu Task='%s' Montage='%s' Slot='%s' SourceValid=%d ExpectedMontage='%s' ExpectedSlot='%s' Stage=%u Instance=%d ExpectedInstance=%d Diagnostic='%s'"),
				*OriginalResource->AbilityPath, OriginalResource->StepIndex, static_cast<unsigned long long>(ThisStepToken),
				*GetPathNameSafe(OriginalTask), *GetPathNameSafe(Fact.Montage.Get()), *Fact.SlotName.ToString(),
				OriginalResource->Source.IsValid(),
				*GetPathNameSafe(OriginalResource->Source.IsValid() ? OriginalResource->Source->Montage.Get() : nullptr),
				*(OriginalResource->Source.IsValid() ? OriginalResource->Source->SlotName.ToString() : FString(TEXT("Unavailable"))),
				static_cast<uint32>(Fact.Stage), Fact.MontageInstanceId, OriginalResource->MontageInstanceId, *Fact.Diagnostic);
			if (!OriginalResource->Source.IsValid() || Fact.Montage != OriginalResource->Source->Montage.Get()
				|| Fact.SlotName != OriginalResource->Source->SlotName || !bOriginalInstance)
			{
				UE_LOG(LogGGYGOAbilitySystem, Error,
					TEXT("[Combat.PlayerCombo.PoseFailure] Boundary=RejectedFact %s; required failure did not identify this original resource"),
					*OriginalLogContext);
				return;
			}
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("[Combat.PlayerCombo.PoseFailure] Boundary=RequiredDependencyFailed %s"), *OriginalLogContext);
			// Required dependency failure ends this original even when user cancellation is disabled.
			const FGGYGOAbilityTerminationResult Result = Self->RequestAbilityEnd(Original, true, true);
			switch (Result.Outcome)
			{
			case EGGYGOAbilityTerminationOutcome::Completed:
			case EGGYGOAbilityTerminationOutcome::Accepted:
			case EGGYGOAbilityTerminationOutcome::Deferred:
			case EGGYGOAbilityTerminationOutcome::AlreadyPending:
				UE_LOG(LogGGYGOAbilitySystem, Display,
					TEXT("[Combat.PlayerCombo.PoseFailure] Boundary=AbilityEndResult %s Outcome=%d Reason=%d"),
					*OriginalLogContext, static_cast<int32>(Result.Outcome), static_cast<int32>(Result.Reason));
				break;
			default:
				UE_LOG(LogGGYGOAbilitySystem, Error,
					TEXT("[Combat.PlayerCombo.PoseFailure] Boundary=AbilityEndResult %s Outcome=%d Reason=%d; original End request was not accepted"),
					*OriginalLogContext, static_cast<int32>(Result.Outcome), static_cast<int32>(Result.Reason));
				break;
			}
			// Only copied diagnostic context and the returned result are accessed after the external End request.
		});
	const FDelegateHandle Registration = StartedMontageTask->RegisterNativeCallbacks(MoveTemp(Callbacks));
	if (!IsStepCurrent(Original, ThisStepToken, WeakTask.Get()))
	{
		if (WeakTask.IsValid())
		{
			StartedMontageTask->UnregisterNativeCallbacks(Registration);
			if (WeakTask.IsValid()) { StartedMontageTask->TaskOwnerEnded(); }
		}
		return false;
	}
	MontageCallbackRegistration = Registration;
	if (!Registration.IsValid())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] step %d Task [%s] pre-Ready native registration failed."),
			*GetPathNameSafe(this), Index, *GetPathNameSafe(StartedMontageTask));
		RequestAbilityCancel(Original, true);
		return false;
	}
	UE_LOG(LogGGYGOAbilitySystem, Display, TEXT("PlayerCombo：%s 段 %d，Montage=%s，Rate=%.2f。"),
		HasAuthority(&CurrentActivationInfo) ? TEXT("服务器") : TEXT("预测端"), Index + 1,
		*GetNameSafe(Step.Montage), Step.PlayRate);
	StartedMontageTask->ReadyForActivation();
	if (!IsStepCurrent(Original, ThisStepToken, WeakTask.Get()) || CurrentStep != Index) { return false; }
	if (!StartedMontageTask->IsActive())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] step %d Task [%s] did not remain active after Ready."),
			*GetPathNameSafe(this), Index, *GetPathNameSafe(StartedMontageTask));
		RequestAbilityCancel(Original, true);
		return false;
	}
	// 默认收尾路径显式钉住，防止编辑器中意外关联到其他段或循环。
	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
	{
		ASC->CurrentMontageSetNextSectionName(Step.MainSection, Step.EndSection);
		if (!IsStepCurrent(Original, ThisStepToken, WeakTask.Get())) { return false; }
		ASC->CurrentMontageSetNextSectionName(Step.EndSection, NAME_None);
		if (!IsStepCurrent(Original, ThisStepToken, WeakTask.Get())) { return false; }
	}
	else
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] step %d original ASC is unavailable."), *GetPathNameSafe(this), Index);
		RequestAbilityCancel(Original, true);
		return false;
	}
	if (!InitializeStepMotion(Motion) || !IsMotionResourceCurrent(Motion)) { return false; }
	UAnimInstance* const OriginalAnimInstance = ActiveMesh->GetAnimInstance();
	const FAnimMontageInstance* const OriginalPlayback = OriginalAnimInstance
		? OriginalAnimInstance->GetMontageInstanceForID(Motion->MontageInstanceId) : nullptr;
	if (!OriginalPlayback || OriginalPlayback->Montage != Step.Montage || !OriginalPlayback->bEnableAutoBlendOut
		|| !FMath::IsFinite(OriginalPlayback->DefaultBlendTimeMultiplier) || OriginalPlayback->DefaultBlendTimeMultiplier < 0.0f)
	{
		FailStepMotion(Motion, TEXT("Original native playback or its natural blend-out budget is unavailable/invalid."));
		return false;
	}
	// UE applies this native instance multiplier to asset BlendOut seconds, independently of play rate.
	// Conservatively include the whole configured mix: its last-frame trigger may extend past the motion endpoint.
	const float NativeBlendOutSeconds = Step.Montage->GetDefaultBlendOutTime() * OriginalPlayback->DefaultBlendTimeMultiplier;
	const float RemainingPlayTime = FMath::Max(0.0f, Motion->Source->MontageEndSeconds - Position);
	const float Timeout = RemainingPlayTime / EffectivePlayRate + NativeBlendOutSeconds + MontageCompletionGraceSeconds;
	UWorld* const World = StepWorld.Get();
	if (!World || !FMath::IsFinite(NativeBlendOutSeconds) || NativeBlendOutSeconds < 0.0f
		|| !FMath::IsFinite(MontageCompletionGraceSeconds) || MontageCompletionGraceSeconds < 0.0f
		|| !FMath::IsFinite(Timeout) || Timeout <= 0.0f)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] step %d watchdog rejected: original World or timeout is invalid."),
			*GetPathNameSafe(this), Index);
		RequestAbilityEnd(Original, true, true);
		return false;
	}
	const FTimerDelegate WatchdogDelegate = FTimerDelegate::CreateLambda([WeakThis, WeakMotion, Original, ThisStepToken]()
	{
		ThisClass* Self = WeakThis.Get();
		const TSharedPtr<FStepMotionResources> OriginalMotion = WeakMotion.Pin();
		if (Self && OriginalMotion.IsValid() && Self->StepMotionResources == OriginalMotion)
		{
			Self->HandleWatchdog(Original, ThisStepToken);
		}
	});
	FTimerHandle StartedWatchdog;
	World->GetTimerManager().SetTimer(StartedWatchdog, WatchdogDelegate, Timeout, false);
	if (!IsStepCurrent(Original, ThisStepToken, WeakTask.Get()))
	{
		if (StepWorld.IsValid()) { StepWorld->GetTimerManager().ClearTimer(StartedWatchdog); }
		return false;
	}
	WatchdogHandle = StartedWatchdog;
	return true;
}

void UGGYGOPlayerComboAbility::ReleaseMontageTask(const FGGYGOAbilityActivationHandle& Original)
{
	if (!ResourceActivation.HasSameActivation(Original)) { return; }
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> PreviousTask(MontageTask.Get());
	const FDelegateHandle Registration = MontageCallbackRegistration;
	MontageTask = nullptr;
	MontageCallbackRegistration.Reset();
	if (!PreviousTask.IsValid()) { return; }
	PreviousTask->UnregisterNativeCallbacks(Registration);
	// EndTask 解除监听但保留动画，下一次 ASC PlayMontage 负责自然混合顶替。
	if (PreviousTask.IsValid()) { PreviousTask->EndTask(); }
}

void UGGYGOPlayerComboAbility::ReleaseTraceWindow(const FGGYGOAbilityActivationHandle& Original)
{
	if (!ResourceActivation.HasSameActivation(Original)) { return; }
	const TWeakObjectPtr<UGGYGOMeleeTraceComponent> OriginalTrace(TraceComponent.Get());
	const FGGYGOMeleeTraceWindowHandle OriginalWindow = TraceWindow;
	const FDelegateHandle Subscription = TraceHitSubscription;
	TraceWindow = {};
	TraceHitSubscription.Reset();
	if (!OriginalTrace.IsValid()) { return; }
	if (Subscription.IsValid()) { OriginalTrace->UnsubscribeWindowHit(Subscription); }
	if (OriginalTrace.IsValid() && OriginalWindow.HasWindow()) { OriginalTrace->CloseOwnedTraceWindow(OriginalWindow); }
}

void UGGYGOPlayerComboAbility::HandleInputPressed(const FGGYGOAbilityActivationHandle& Original, int32 SourceStep, int32 RequestId)
{
	if (!IsActivationCurrent(Original) || !ComboSteps.IsValidIndex(CurrentStep)) { return; }
	UWorld* const World = OriginalWorld.Get();
	if (!World)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] input rejected: original World is unavailable."), *GetPathNameSafe(this));
		RequestAbilityCancel(Original, true);
		return;
	}
	if (RequestId <= LastRequestId || RequestId > 65535) { return; }
	LastRequestId = RequestId;
	if (SourceStep != CurrentStep || ComboSteps[CurrentStep].NextStepIndex == INDEX_NONE
		|| !Window.Store(RequestId, World->GetTimeSeconds(), InputBufferSeconds))
	{
		RejectRequest(Original, RequestId);
		return;
	}
	TryAdvanceCombo(Original);
}

void UGGYGOPlayerComboAbility::TryAdvanceCombo(const FGGYGOAbilityActivationHandle& Original)
{
	if (!IsActivationCurrent(Original) || !ComboSteps.IsValidIndex(CurrentStep)) { return; }
	UWorld* const World = OriginalWorld.Get();
	if (!World)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] advance rejected: original World is unavailable."), *GetPathNameSafe(this));
		RequestAbilityCancel(Original, true);
		return;
	}
	const uint64 ExpectedStepToken = CurrentStepToken;
	const double Now = World->GetTimeSeconds();
	if (Window.HasExpired(Now))
	{
		const int32 ExpiredId = Window.PendingRequestId;
		Window.PendingRequestId = 0;
		RejectRequest(Original, ExpiredId);
		if (!IsActivationCurrent(Original) || CurrentStepToken != ExpectedStepToken) { return; }
	}
	const int32 RequestId = Window.Consume(Now);
	if (RequestId == 0) { return; }
	const int32 Next = ComboSteps[CurrentStep].NextStepIndex;
	if (Next <= CurrentStep || !IsStepPlayable(Next)) { RejectRequest(Original, RequestId); return; }
	if (StartStep(Original, Next) && IsActivationCurrent(Original)
		&& CurrentStep == Next && CurrentStepToken != 0)
	{
		SendAuthoritativeStep(Original, RequestId, true);
	}
}

void UGGYGOPlayerComboAbility::HandleMontageEvent(const FGGYGOAbilityActivationHandle& Original,
	FGameplayTag EventTag, FGameplayEventData EventData)
{
	if (!IsActivationCurrent(Original) || !ComboSteps.IsValidIndex(CurrentStep)
		|| EventData.OptionalObject != ComboSteps[CurrentStep].Montage
		|| (EventData.OptionalObject2 && EventData.OptionalObject2 != ActiveMesh)) { return; }
	if (EventTag == GGYGOGameplayTags::Event_Montage_ComboWindowBegin)
	{
		if (!Window.bClosed) { Window.bOpen = true; TryAdvanceCombo(Original); }
	}
	else if (EventTag == GGYGOGameplayTags::Event_Montage_ComboWindowEnd)
	{
		const int32 Pending = Window.PendingRequestId;
		Window.Close();
		if (Pending > 0) { RejectRequest(Original, Pending); }
	}
	else if (HasAuthority(&CurrentActivationInfo))
	{
		if (EventTag == GGYGOGameplayTags::Event_Montage_HitWindowBegin) { OpenTraceWindow(Original); }
		else if (EventTag == GGYGOGameplayTags::Event_Montage_HitWindowEnd) { ReleaseTraceWindow(Original); }
	}
}

void UGGYGOPlayerComboAbility::OpenTraceWindow(const FGGYGOAbilityActivationHandle& Original)
{
	if (!IsActivationCurrent(Original) || !ComboSteps.IsValidIndex(CurrentStep)) { return; }
	const FGGYGOComboStep Step = ComboSteps[CurrentStep];
	const int32 ExpectedStepIndex = CurrentStep;
	const uint64 ExpectedStepToken = CurrentStepToken;
	const TWeakObjectPtr<ThisClass> WeakThis(this);
	const TWeakObjectPtr<UGGYGOMeleeTraceComponent> OriginalTrace(TraceComponent.Get());
	const TWeakObjectPtr<USkeletalMeshComponent> OriginalMesh(ActiveMesh.Get());
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> WeakTask(MontageTask.Get());
	ReleaseTraceWindow(Original);
	if (!IsStepCurrent(Original, ExpectedStepToken, WeakTask.Get())) { return; }
	if (!OriginalTrace.IsValid() || !OriginalMesh.IsValid())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] owned trace open rejected: original Trace [%s] or Mesh [%s] is invalid."),
			*GetPathNameSafe(this), *GetPathNameSafe(OriginalTrace.Get()), *GetPathNameSafe(OriginalMesh.Get()));
		RequestAbilityCancel(Original, true);
		return;
	}
	FGGYGOMeleeTraceWindowHandle OpenedWindow;
	const EGGYGOMeleeTraceWindowOpenResult OpenResult = OriginalTrace->TryOpenOwnedTraceWindow(
		Step.TraceShape, OriginalMesh.Get(), {}, OpenedWindow);
	if (!IsStepCurrent(Original, ExpectedStepToken, WeakTask.Get()))
	{
		if (OriginalTrace.IsValid() && OpenedWindow.HasWindow()) { OriginalTrace->CloseOwnedTraceWindow(OpenedWindow); }
		return;
	}
	if (OpenResult != EGGYGOMeleeTraceWindowOpenResult::Opened)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] owned trace open failed: step %d, Trace [%s], Montage [%s], Mesh [%s], TraceShape Mode=%d Chains=%d, result %d."),
			*GetPathNameSafe(this), ExpectedStepIndex, *GetPathNameSafe(OriginalTrace.Get()), *GetPathNameSafe(Step.Montage),
			*GetPathNameSafe(OriginalMesh.Get()), static_cast<int32>(Step.TraceShape.Mode),
			Step.TraceShape.Chains.Num(), static_cast<int32>(OpenResult));
		RequestAbilityCancel(Original, true);
		return;
	}
	TraceWindow = OpenedWindow;
	FDelegateHandle Subscription;
	const EGGYGOMeleeTraceWindowSubscribeResult SubscribeResult = OriginalTrace->SubscribeWindowHit(OpenedWindow,
		FGGYGOMeleeTraceWindowHitDelegate::CreateLambda(
			[WeakThis, WeakTask, OriginalTrace, Original, ExpectedStepToken, OpenedWindow](
				const FGGYGOMeleeTraceWindowHandle& ReportedWindow, AActor* HitActor, const FHitResult& HitResult)
			{
				ThisClass* Self = WeakThis.Get();
				if (Self && Self->IsStepCurrent(Original, ExpectedStepToken, WeakTask.Get())
					&& OriginalTrace.IsValid() && Self->TraceComponent == OriginalTrace.Get()
					&& ReportedWindow == OpenedWindow && Self->TraceWindow == OpenedWindow
					&& OriginalTrace->QueryOwnedTraceWindow(OpenedWindow) == EGGYGOMeleeTraceWindowQueryResult::Active)
				{
					Self->HandleMeleeHit(Original, ExpectedStepToken, OpenedWindow, HitActor, HitResult);
				}
			}), Subscription);
	if (!IsStepCurrent(Original, ExpectedStepToken, WeakTask.Get()) || TraceWindow != OpenedWindow)
	{
		if (OriginalTrace.IsValid() && Subscription.IsValid()) { OriginalTrace->UnsubscribeWindowHit(Subscription); }
		if (OriginalTrace.IsValid()) { OriginalTrace->CloseOwnedTraceWindow(OpenedWindow); }
		return;
	}
	TraceHitSubscription = Subscription;
	if (SubscribeResult != EGGYGOMeleeTraceWindowSubscribeResult::Subscribed || !Subscription.IsValid())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] owned trace subscription failed: Trace [%s], result %d."),
			*GetPathNameSafe(this), *GetPathNameSafe(OriginalTrace.Get()), static_cast<int32>(SubscribeResult));
		RequestAbilityCancel(Original, true);
	}
}


void UGGYGOPlayerComboAbility::RejectRequest(const FGGYGOAbilityActivationHandle& Original, int32 RequestId)
{
	SendAuthoritativeStep(Original, RequestId, false);
}

void UGGYGOPlayerComboAbility::SendAuthoritativeStep(const FGGYGOAbilityActivationHandle& Original, int32 RequestId, bool bAccepted)
{
	if (!IsActivationCurrent(Original) || !HasAuthority(&CurrentActivationInfo)
		|| !CurrentActorInfo || CurrentActorInfo->IsLocallyControlled()) { return; }
	UGGYGOAbilitySystemComponent* ASC = GetGGYGOAbilitySystemComponentFromActorInfo();
	UAnimInstance* AnimInstance = CurrentActorInfo->GetAnimInstance();
	if (ASC && AnimInstance && ComboSteps.IsValidIndex(CurrentStep))
	{
		if (StepSyncRevision == MAX_int32)
		{
			UE_LOG(LogGGYGOAbilitySystem, Warning, TEXT("PlayerCombo：纠正修订序号已耗尽，跳过后续同步。"));
			return;
		}
		const float MontagePosition = AnimInstance->Montage_GetPosition(ComboSteps[CurrentStep].Montage);
		if (!FMath::IsFinite(MontagePosition) || MontagePosition < 0.0f
			|| MontagePosition > ComboSteps[CurrentStep].Montage->GetPlayLength()) { return; }
		FGameplayAbilityTargetDataHandle Correction(new FGGYGOComboCorrectionData(
			++StepSyncRevision, RequestId, CurrentStep, MontagePosition, Window.bOpen, Window.bClosed, bAccepted));
		ASC->ClientCorrectAbilityState(CurrentSpecHandle,
			CurrentActivationInfo.GetActivationPredictionKey(), Correction);
	}
}

void UGGYGOPlayerComboAbility::ReceiveAbilityCorrection(const FGameplayAbilityTargetDataHandle& Correction)
{
	const FGGYGOAbilityActivationHandle Original = ResourceActivation;
	if (!IsActivationCurrent(Original) || HasAuthority(&CurrentActivationInfo) || Correction.Num() != 1) { return; }
	const FGameplayAbilityTargetData* TargetData = Correction.Get(0);
	if (!TargetData || TargetData->GetScriptStruct() != FGGYGOComboCorrectionData::StaticStruct()) { return; }
	if (!IsActivationCurrent(Original)) { return; }
	const FGGYGOComboCorrectionData& ComboCorrection = static_cast<const FGGYGOComboCorrectionData&>(*TargetData);
	if (!ComboCorrection.HasValidFields() || !IsStepPlayable(ComboCorrection.ServerStep)) { return; }
	const float ServerMontageLength = ComboSteps[ComboCorrection.ServerStep].Montage->GetPlayLength();
	if (!FMath::IsFinite(ServerMontageLength) || ComboCorrection.Position > ServerMontageLength) { return; }
	CorrectPredictedStepForActivation(Original, ComboCorrection.Revision, ComboCorrection.RequestId, ComboCorrection.ServerStep,
		ComboCorrection.Position, ComboCorrection.bWindowOpen, ComboCorrection.bWindowClosed, ComboCorrection.bAccepted);
}

void UGGYGOPlayerComboAbility::CorrectPredictedStep(int32 Revision, int32 RequestId, int32 ServerStep, float Position,
	bool bWindowOpen, bool bWindowClosed, bool bAccepted)
{
	const FGGYGOAbilityActivationHandle Original = ResourceActivation;
	CorrectPredictedStepForActivation(Original, Revision, RequestId, ServerStep, Position, bWindowOpen, bWindowClosed, bAccepted);
}

void UGGYGOPlayerComboAbility::CorrectPredictedStepForActivation(const FGGYGOAbilityActivationHandle& Original,
	int32 Revision, int32 RequestId, int32 ServerStep, float Position,
	bool bWindowOpen, bool bWindowClosed, bool bAccepted)
{
	if (!IsActivationCurrent(Original) || HasAuthority(&CurrentActivationInfo)
		|| Revision <= StepSyncRevision || RequestId <= 0 || RequestId > 65535
		|| !FMath::IsFinite(Position) || !IsStepPlayable(ServerStep)
		|| Position < 0.0f || Position > ComboSteps[ServerStep].Montage->GetPlayLength()
		|| (bWindowOpen && bWindowClosed)) { return; }
	StepSyncRevision = Revision;
	// 已确认的旧请求不回滚客户端更新的预测；服务器拒绝则撤销依赖该预测的段。
	if (bAccepted && CurrentStep >= ServerStep) { return; }
	if (!bAccepted) { Window.PendingRequestId = 0; }
	if (ServerStep != CurrentStep)
	{
		const int32 PreviousStep = CurrentStep;
		const uint64 PreviousStepToken = CurrentStepToken;
		const float SafePosition = FMath::Clamp(Position, 0.0f,
			FMath::Max(0.0f, ComboSteps[ServerStep].Montage->GetPlayLength() - 0.001f));
		if (!StartStep(Original, ServerStep, SafePosition))
		{
			if (!IsActivationCurrent(Original)) { return; }
			// ReadyForActivation can synchronously move the same activation to a newer step.
			// In that case this correction stack must leave the new task and its timer alone.
			if (CurrentStep != PreviousStep || CurrentStepToken != PreviousStepToken)
			{
				if (!IsValid(MontageTask) || !MontageTask->IsActive()) { RequestAbilityCancel(Original, true); }
				return;
			}
			RequestAbilityCancel(Original, true);
			return;
		}
		if (!IsActivationCurrent(Original) || CurrentStep != ServerStep) { return; }
		Window.bOpen = bWindowOpen;
		Window.bClosed = bWindowClosed;
	}
	UE_LOG(LogGGYGOAbilitySystem, Display, TEXT("PlayerCombo：请求 %d，服务器%s，同步到段 %d。"),
		RequestId, bAccepted ? TEXT("确认") : TEXT("拒绝"), ServerStep + 1);
}

void UGGYGOPlayerComboAbility::HandleMontageCompleted(const TSharedPtr<FStepMotionResources>& Resource)
{
	if (!IsMotionResourceCurrent(Resource) || Resource->bMontageCompleted) { return; }
	Resource->bMontageCompleted = true;
	FString Error;
	if (!Resource->Movement.IsValid() || Resource->MotionHandle == INDEX_NONE
		|| !Resource->Movement->ReleaseMontageActionMotion(Resource->MotionHandle, EGGYGOActionMotionReleaseReason::Completed, Error))
	{
		FailStepMotion(Resource, FString::Printf(TEXT("Original Task completed but its CMC natural drain was rejected: %s"), *Error));
		return;
	}
	if (IsMotionResourceCurrent(Resource)) { TryCompleteStepMotion(Resource); }
}

void UGGYGOPlayerComboAbility::TryCompleteStepMotion(const TSharedPtr<FStepMotionResources>& Resource)
{
	if (!IsMotionResourceCurrent(Resource)) { return; }
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("[Combat.PlayerCombo.EndDiagnostics] Boundary=NaturalCompletion Ability='%s' Step=%d StepToken=%llu Montage='%s' Instance=%d MotionHandle=%d TaskCompleted=%d MotionCompleted=%d"),
		*Resource->AbilityPath, Resource->StepIndex, static_cast<unsigned long long>(Resource->StepToken),
		*GetPathNameSafe(Resource->Source->Montage.Get()), Resource->MontageInstanceId, Resource->MotionHandle,
		Resource->bMontageCompleted, Resource->bMotionCompleted);
	if (Resource->bMontageCompleted && Resource->bMotionCompleted)
	{
		RequestAbilityEnd(Resource->Activation, HasAuthority(&CurrentActivationInfo), false);
	}
}

void UGGYGOPlayerComboAbility::HandleMontageInterrupted(const FGGYGOAbilityActivationHandle& Original,
	FGameplayTag EventTag, FGameplayEventData EventData)
{
	if (IsActivationCurrent(Original)) { RequestAbilityCancel(Original, true); }
}

void UGGYGOPlayerComboAbility::HandleMontageBlendOut(const FGGYGOAbilityActivationHandle& Original,
	FGameplayTag EventTag, FGameplayEventData EventData)
{
	if (!IsActivationCurrent(Original)) { return; }
	const TSharedPtr<FStepMotionResources> Resource = StepMotionResources;
	if (!IsMotionResourceCurrent(Resource)) { return; }
	FString Error;
	// The original Task sends this callback only for a native non-interrupted blend-out.
	// It grants CMC permission to consume the configured last-frame tail, never motion completion.
	if (!Resource->Movement.IsValid() || Resource->MotionHandle == INDEX_NONE
		|| !Resource->Movement->NotifyMontageActionNaturalBlendOut(Resource->MotionHandle, Resource->MontageInstanceId, Error))
	{
		FailStepMotion(Resource, FString::Printf(TEXT("Original natural blend-out fact was rejected by CMC: %s"), *Error));
		return;
	}
	if (!IsMotionResourceCurrent(Resource)) { return; }
	const int32 Pending = Window.PendingRequestId;
	Window.Close();
	const uint64 ExpectedStepToken = CurrentStepToken;
	ReleaseTraceWindow(Original);
	if (!IsActivationCurrent(Original) || CurrentStepToken != ExpectedStepToken) { return; }
	if (Pending > 0) { RejectRequest(Original, Pending); }
}

void UGGYGOPlayerComboAbility::HandleWatchdog(const FGGYGOAbilityActivationHandle& Original, uint64 ExpectedStepToken)
{
	if (!IsActivationCurrent(Original) || CurrentStepToken != ExpectedStepToken) { return; }
	const TSharedPtr<FStepMotionResources> Resource = StepMotionResources;
	if (!Resource.IsValid() || Resource->Section == FStepMotionResources::ESection::Retired
		|| Resource->StepToken != ExpectedStepToken || CurrentStep != Resource->StepIndex
		|| !Resource->Activation.HasSameActivation(Original)) { return; }
	// Missing completion may include a vanished Task. Authenticate our exact owned resource,
	// rather than requiring the failed dependency to stay alive before we can end its owner.
	UE_LOG(LogGGYGOAbilitySystem, Error,
		TEXT("PlayerCombo [%s] Step %d Montage [%s] CMC [%s] bounded natural completion timed out: StepToken=%llu Instance=%d Handle=%d TaskCompleted=%d MotionCompleted=%d TaskAvailable=%d."),
		*Resource->AbilityPath, Resource->StepIndex, *GetPathNameSafe(Resource->Source->Montage.Get()),
		*GetPathNameSafe(Resource->Movement.Get()), static_cast<unsigned long long>(Resource->StepToken),
		Resource->MontageInstanceId, Resource->MotionHandle, Resource->bMontageCompleted, Resource->bMotionCompleted, Resource->Task.IsValid());
	RequestAbilityEnd(Original, true, true);
}

void UGGYGOPlayerComboAbility::HandleMeleeHit(const FGGYGOAbilityActivationHandle& Original, uint64 ExpectedStepToken,
	const FGGYGOMeleeTraceWindowHandle& OriginalWindow, AActor* HitActor, const FHitResult& HitResult)
{
	const TWeakObjectPtr<UGGYGOMeleeTraceComponent> OriginalTrace(TraceComponent.Get());
	const TWeakObjectPtr<ThisClass> WeakThis(this);
	const auto IsOriginalHitCurrent = [WeakThis, Original, ExpectedStepToken, OriginalWindow, OriginalTrace]()
	{
		const ThisClass* Self = WeakThis.Get();
		return Self && Self->IsActivationCurrent(Original) && Self->CurrentStepToken == ExpectedStepToken && Self->TraceWindow == OriginalWindow
			&& OriginalTrace.IsValid() && Self->TraceComponent == OriginalTrace.Get()
			&& OriginalTrace->QueryOwnedTraceWindow(OriginalWindow) == EGGYGOMeleeTraceWindowQueryResult::Active;
	};
	if (!IsOriginalHitCurrent() || !HasAuthority(&CurrentActivationInfo)
		|| !IsValid(HitActor) || HitActor == GetAvatarActorFromActorInfo() || !ComboSteps.IsValidIndex(CurrentStep)) { return; }
	const FGGYGOComboStep Step = ComboSteps[CurrentStep];
	const FGameplayTag OriginalHitCueTag = HitCueTag;
	UAbilitySystemComponent* TargetASC = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(HitActor);
	if (!IsOriginalHitCurrent()) { return; }
	AActor* SourceAvatar = GetAvatarActorFromActorInfo();
	if (!IsValid(TargetASC) || !IsValid(SourceAvatar)) { return; }
	const TWeakObjectPtr<UAbilitySystemComponent> OriginalTargetASC(TargetASC);
	const TWeakObjectPtr<UAbilitySystemComponent> OriginalSourceASC(GetAbilitySystemComponentFromActorInfo());
	const FString AbilityPath = GetPathNameSafe(this);
	const FString MontagePath = GetPathNameSafe(Step.Montage);
	const FString SourcePath = GetPathNameSafe(SourceAvatar);
	const FString TargetPath = GetPathNameSafe(HitActor);
	const int32 OriginalStep = CurrentStep;
	const auto AbortOriginalHit = [WeakThis, Original, AbilityPath, MontagePath, SourcePath, TargetPath, OriginalStep](const FString& Reason)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("PlayerCombo runtime hit Ability [%s] Source [%s] Target [%s] Step %d Montage [%s] failed: %s."),
			*AbilityPath, *SourcePath, *TargetPath, OriginalStep, *MontagePath, *Reason);
		if (ThisClass* Self = WeakThis.Get())
		{
			// Internal dependency failure ends this captured original, even if user cancellation is disabled.
			// GA validates stale/busy sources and owns cleanup/continuation; there is no alternate request.
			const FGGYGOAbilityTerminationResult Result = Self->RequestAbilityEnd(Original, true, true);
			switch (Result.Outcome)
			{
			case EGGYGOAbilityTerminationOutcome::Completed:
			case EGGYGOAbilityTerminationOutcome::Accepted:
			case EGGYGOAbilityTerminationOutcome::Deferred:
			case EGGYGOAbilityTerminationOutcome::AlreadyPending:
				break;
			default:
				UE_LOG(LogGGYGOAbilitySystem, Error,
					TEXT("PlayerCombo runtime hit Ability [%s] Step %d original failure End was not accepted: outcome %d, reason %d."),
					*AbilityPath, OriginalStep, static_cast<int32>(Result.Outcome), static_cast<int32>(Result.Reason));
				break;
			}
		}
	};
	TSubclassOf<UGameplayEffect> ResolvedDamageEffect;
	FString DamageDependencyError;
	if (!ValidateDamageEffectDependency(DamageDependencyError, &ResolvedDamageEffect))
	{
		AbortOriginalHit(DamageDependencyError);
		return;
	}
	const FString EffectPath = GetPathNameSafe(ResolvedDamageEffect.Get());
	FGGYGOHitEffectPayload HitPayload;
	if (!BuildHitEffectPayload(TargetASC, ResolvedDamageEffect, GetAbilityLevel(), HitResult,
		SourceAvatar->GetActorLocation(), HitPayload))
	{
		AbortOriginalHit(FString::Printf(TEXT("BuildHitEffectPayload returned false for GE [%s]; see the original Builder diagnostic."), *EffectPath));
		return;
	}
	if (!IsOriginalHitCurrent() || !OriginalTargetASC.IsValid()) { return; }
	if (HitPayload.EffectSpec.IsValid())
	{
		HitPayload.EffectSpec.Data->SetSetByCallerMagnitude(GGYGOGameplayTags::SetByCaller_Damage, Step.Damage);
		HitPayload.EffectSpec.Data->SetSetByCallerMagnitude(GGYGOGameplayTags::SetByCaller_PoiseDamage, Step.PoiseDamage);
		if (OriginalSourceASC.IsValid())
		{
			OriginalSourceASC->ApplyGameplayEffectSpecToTarget(*HitPayload.EffectSpec.Data.Get(), OriginalTargetASC.Get());
			if (!IsOriginalHitCurrent() || !OriginalTargetASC.IsValid()) { return; }
		}
	}
	if (OriginalHitCueTag.IsValid())
	{
		// 碰撞已经成立；GE 被免疫或拒绝不应吞掉命中表现。
		OriginalTargetASC->ExecuteGameplayCue(OriginalHitCueTag, HitPayload.CueParameters);
	}
}

void UGGYGOPlayerComboAbility::CleanupAbilityResourcesForTermination(const FGGYGOAbilityTerminationContext& Context)
{
	if (!ResourceActivation.HasSameActivation(Context.GetOriginalActivation()))
	{
		if (ResourceActivation.HasActivation() || StepMotionResources.IsValid() || MontageTask || InputTask || TraceComponent || ActiveMesh
			|| MontageCallbackRegistration.IsValid() || InputCallbackRegistration.IsValid()
			|| TraceWindow.HasWindow() || TraceHitSubscription.IsValid() || WatchdogHandle.IsValid()
			|| OriginalWorld.IsValid() || bChangedMeshTick || bAddedMeshPrerequisite)
		{
			UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] resource cleanup rejected: GA original activation does not match resource source."),
				*GetPathNameSafe(this));
		}
		Super::CleanupAbilityResourcesForTermination(Context);
		return;
	}
	const TWeakObjectPtr<UGGYGOAbilityTask_WaitComboInput> EndingInputTask(InputTask.Get());
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> EndingMontageTask(MontageTask.Get());
	const TWeakObjectPtr<UGGYGOMeleeTraceComponent> EndingTraceComponent(TraceComponent.Get());
	const TWeakObjectPtr<USkeletalMeshComponent> EndingMesh(ActiveMesh.Get());
	const TWeakObjectPtr<UWorld> EndingWorld = OriginalWorld;
	FTimerHandle EndingWatchdog = WatchdogHandle;
	const FDelegateHandle EndingInputRegistration = InputCallbackRegistration;
	const FDelegateHandle EndingMontageRegistration = MontageCallbackRegistration;
	const FGGYGOMeleeTraceWindowHandle EndingTraceWindow = TraceWindow;
	const FDelegateHandle EndingTraceSubscription = TraceHitSubscription;
	const bool bRestoreMesh = bChangedMeshTick;
	const bool bRemovePrerequisite = bAddedMeshPrerequisite;
	const EVisibilityBasedAnimTickOption MeshTickToRestore = SavedMeshTick;
	const bool bUpdateRateToRestore = bSavedUpdateRateOptimizations;
	const TSharedPtr<FStepMotionResources> EndingMotion = MoveTemp(StepMotionResources);

	// Detach every original member before unregistering, restoring or ending external resources.
	ResourceActivation = {};
	Window.Close();
	CurrentStep = INDEX_NONE;
	CurrentStepToken = 0;
	InputTask = nullptr;
	MontageTask = nullptr;
	TraceComponent = nullptr;
	ActiveMesh = nullptr;
	OriginalWorld.Reset();
	WatchdogHandle.Invalidate();
	InputCallbackRegistration.Reset();
	MontageCallbackRegistration.Reset();
	TraceWindow = {};
	TraceHitSubscription.Reset();
	bChangedMeshTick = false;
	bAddedMeshPrerequisite = false;
	bSavedUpdateRateOptimizations = false;
	SavedMeshTick = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
	if (EndingMotion.IsValid())
	{
		const EGGYGOActionMotionReleaseReason MotionReason = !Context.WasCancelled()
			&& EndingMotion->bMontageCompleted && EndingMotion->bMotionCompleted
			? EGGYGOActionMotionReleaseReason::Completed
			: (Context.WasCancelled() ? EGGYGOActionMotionReleaseReason::Cancelled : EGGYGOActionMotionReleaseReason::OwnerInvalidated);
		EndingMotion->Release(MotionReason);
	}

	if (EndingWorld.IsValid()) { EndingWorld->GetTimerManager().ClearTimer(EndingWatchdog); }
	if (EndingTraceComponent.IsValid())
	{
		if (EndingTraceSubscription.IsValid()) { EndingTraceComponent->UnsubscribeWindowHit(EndingTraceSubscription); }
		if (EndingTraceComponent.IsValid() && EndingTraceWindow.HasWindow()) { EndingTraceComponent->CloseOwnedTraceWindow(EndingTraceWindow); }
		if (EndingTraceComponent.IsValid() && EndingMesh.IsValid() && bRemovePrerequisite)
		{
			EndingTraceComponent->RemoveTickPrerequisiteComponent(EndingMesh.Get());
		}
	}
	if (EndingMesh.IsValid() && bRestoreMesh)
	{
		EndingMesh->VisibilityBasedAnimTickOption = MeshTickToRestore;
		EndingMesh->bEnableUpdateRateOptimizations = bUpdateRateToRestore;
	}
	if (EndingInputTask.IsValid())
	{
		EndingInputTask->UnregisterNativeCallback(EndingInputRegistration);
		if (EndingInputTask.IsValid()) { EndingInputTask->TaskOwnerEnded(); }
	}
	if (EndingMontageTask.IsValid())
	{
		EndingMontageTask->UnregisterNativeCallbacks(EndingMontageRegistration);
		// GAS normally ends tasks after OnGameplayAbilityEnded; stop this task's Montage before that notification.
		if (EndingMontageTask.IsValid()) { EndingMontageTask->TaskOwnerEnded(); }
	}
	Super::CleanupAbilityResourcesForTermination(Context);
}
