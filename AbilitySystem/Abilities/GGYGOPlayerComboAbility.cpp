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
		|| Step.MainSection == Step.EndSection) { return false; }
	const int32 MainIndex = Step.Montage->GetSectionIndex(Step.MainSection);
	const int32 EndIndex = Step.Montage->GetSectionIndex(Step.EndSection);
	return MainIndex != INDEX_NONE && EndIndex != INDEX_NONE
		&& Step.Montage->GetAnimCompositeSection(MainIndex).GetTime() < Step.Montage->GetAnimCompositeSection(EndIndex).GetTime()
		&& Step.Montage->GetAnimCompositeSection(EndIndex).GetTime() < Step.Montage->GetPlayLength();
}

bool UGGYGOPlayerComboAbility::ValidateComboConfiguration(FString& OutError) const
{
	if (ComboSteps.IsEmpty()) { OutError = TEXT("ComboSteps 为空。"); return false; }
	if (!FMath::IsFinite(InputBufferSeconds) || InputBufferSeconds < 0.0f || InputBufferSeconds > 1.0f)
	{
		OutError = TEXT("InputBufferSeconds 必须是 0 到 1 秒之间的有限值。");
		return false;
	}
	for (int32 Index = 0; Index < ComboSteps.Num(); ++Index)
	{
		const FGGYGOComboStep& Step = ComboSteps[Index];
		if (!IsStepPlayable(Index) || Step.TraceStartSocket.IsNone() || Step.TraceEndSocket.IsNone()
			|| !FMath::IsFinite(Step.TraceRadius) || Step.TraceRadius < 1.0f
			|| !FMath::IsFinite(Step.Damage) || Step.Damage < 0.0f
			|| !FMath::IsFinite(Step.PoiseDamage) || Step.PoiseDamage < 0.0f)
		{
			OutError = FString::Printf(TEXT("段 %d 的 Montage/Section/速率/伤害/Socket 无效。"), Index);
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
	return ValidateDamageEffectDependency(Error);
}

void UGGYGOPlayerComboAbility::NativeOnAbilityFailedToActivate(const FGameplayTagContainer& FailedReason) const
{
	FString Error;
	if (!ValidateDamageEffectDependency(Error))
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
	if (ResourceActivation.HasActivation() || MontageTask || InputTask || TraceComponent || ActiveMesh
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
	if (!IsValid(ActiveMesh) || !IsValid(TraceComponent) || !OriginalWorld.IsValid() || !IsStepPlayable(0))
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
	if (!ActiveMesh->DoesSocketExist(Step.TraceStartSocket) || !ActiveMesh->DoesSocketExist(Step.TraceEndSocket))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo：段 %d 的武器 Socket 不存在 [%s → %s]。"),
			Index, *Step.TraceStartSocket.ToString(), *Step.TraceEndSocket.ToString());
		RequestAbilityCancel(Original, true);
		return false;
	}
	const TWeakObjectPtr<UWorld> StepWorld = OriginalWorld;
	FTimerHandle PreviousWatchdog = WatchdogHandle;
	WatchdogHandle.Invalidate();
	StepWorld->GetTimerManager().ClearTimer(PreviousWatchdog);
	if (!IsActivationCurrent(Original)) { return false; }
	ReleaseTraceWindow(Original);
	if (!IsActivationCurrent(Original)) { return false; }
	ReleaseMontageTask(Original);
	if (!IsActivationCurrent(Original)) { return false; }
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
	const float EffectivePlayRate = StartedMontageTask->GetEffectivePlayRate();
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
	const auto ResolveOriginalCaller = [WeakThis, WeakTask, Original, ThisStepToken]() -> ThisClass*
	{
		ThisClass* Self = WeakThis.Get();
		return Self && Self->IsStepCurrent(Original, ThisStepToken, WeakTask.Get()) ? Self : nullptr;
	};
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent::FNativeCallbacks Callbacks;
	Callbacks.OnCompleted = FGGYGOPlayMontageAndWaitForEventDelegate::CreateLambda(
		[ResolveOriginalCaller, Original](FGameplayTag Tag, FGameplayEventData Data)
		{
			if (ThisClass* Self = ResolveOriginalCaller()) { Self->HandleMontageCompleted(Original, Tag, Data); }
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
	const float RemainingPlayTime = FMath::Max(0.0f, MontageLength - Position);
	const float Timeout = RemainingPlayTime / EffectivePlayRate + 2.0f;
	UWorld* const World = StepWorld.Get();
	if (!World || !FMath::IsFinite(Timeout))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] step %d watchdog rejected: original World or timeout is invalid."),
			*GetPathNameSafe(this), Index);
		RequestAbilityCancel(Original, true);
		return false;
	}
	const FTimerDelegate WatchdogDelegate = FTimerDelegate::CreateLambda([ResolveOriginalCaller, Original, ThisStepToken]()
	{
		if (ThisClass* Self = ResolveOriginalCaller()) { Self->HandleWatchdog(Original, ThisStepToken); }
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
	const uint64 ExpectedStepToken = CurrentStepToken;
	const TWeakObjectPtr<ThisClass> WeakThis(this);
	const TWeakObjectPtr<UGGYGOMeleeTraceComponent> OriginalTrace(TraceComponent.Get());
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> WeakTask(MontageTask.Get());
	ReleaseTraceWindow(Original);
	if (!IsStepCurrent(Original, ExpectedStepToken, WeakTask.Get())) { return; }
	if (!OriginalTrace.IsValid())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] owned trace open rejected: original Trace component is invalid."), *GetPathNameSafe(this));
		RequestAbilityCancel(Original, true);
		return;
	}
	FGGYGOMeleeTraceWindowHandle OpenedWindow;
	const EGGYGOMeleeTraceWindowOpenResult OpenResult = OriginalTrace->TryOpenOwnedTraceWindow(
		Step.TraceStartSocket, Step.TraceEndSocket, Step.TraceRadius, {}, OpenedWindow);
	if (!IsStepCurrent(Original, ExpectedStepToken, WeakTask.Get()))
	{
		if (OriginalTrace.IsValid() && OpenedWindow.HasWindow()) { OriginalTrace->CloseOwnedTraceWindow(OpenedWindow); }
		return;
	}
	if (OpenResult != EGGYGOMeleeTraceWindowOpenResult::Opened)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] owned trace open failed: Trace [%s], Montage [%s], sockets [%s -> %s], radius %.3f, result %d."),
			*GetPathNameSafe(this), *GetPathNameSafe(OriginalTrace.Get()), *GetPathNameSafe(Step.Montage),
			*Step.TraceStartSocket.ToString(), *Step.TraceEndSocket.ToString(), Step.TraceRadius, static_cast<int32>(OpenResult));
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

void UGGYGOPlayerComboAbility::HandleMontageCompleted(const FGGYGOAbilityActivationHandle& Original,
	FGameplayTag EventTag, FGameplayEventData EventData)
{
	if (!IsActivationCurrent(Original)) { return; }
	RequestAbilityEnd(Original, HasAuthority(&CurrentActivationInfo), false);
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
	UE_LOG(LogGGYGOAbilitySystem, Warning, TEXT("PlayerCombo：动画完成回调超时，释放动作。"));
	RequestAbilityCancel(Original, true);
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
		if (ResourceActivation.HasActivation() || MontageTask || InputTask || TraceComponent || ActiveMesh
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
