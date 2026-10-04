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

bool UGGYGOPlayerComboAbility::ValidateDamageEffectDependency(FString& OutError) const
{
	OutError.Reset();
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
	return IsChosenClassValid(ResolvedClass, ConfiguredClass ? TEXT("Override") : TEXT("Shared"),
		ConfiguredClass ? TEXT("DamageEffect") : TEXT("GameData.DamageGameplayEffect_SetByCaller"));
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

void UGGYGOPlayerComboAbility::ActivateAbility(FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	if (LocalActivationGeneration == MAX_uint64)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo：本地激活代次已耗尽，拒绝复用实例。"));
		Super::EndAbility(Handle, ActorInfo, ActivationInfo, false, true);
		return;
	}
	const uint64 ThisGeneration = ++LocalActivationGeneration;
	EndRequestedActivationGeneration = 0;
	bCleaningUp = false;
	Window.Reset();
	CurrentStep = INDEX_NONE;
	LastRequestId = 0;
	StepSyncRevision = 0;
	CurrentStepToken = 0;
	MontageTask = nullptr;
	InputTask = nullptr;
	TraceComponent = nullptr;
	ActiveMesh = nullptr;
	bChangedMeshTick = false;

	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);
	if (!IsActivationCurrent(ThisGeneration)) { return; }

	FString DamageDependencyError;
	if (!ValidateDamageEffectDependency(DamageDependencyError))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] 提交前拒绝激活：%s"),
			*GetPathNameSafe(this), *DamageDependencyError);
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	ACharacter* Character = GetCharacterFromActorInfo();
	ActiveMesh = Character ? Character->GetMesh() : nullptr;
	TraceComponent = Character ? Character->FindComponentByClass<UGGYGOMeleeTraceComponent>() : nullptr;
	if (!ActiveMesh || !TraceComponent || !IsStepPlayable(0))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo：Avatar、Trace 或第一段配置无效。"));
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	const bool bCommitted = CommitAbility(Handle, ActorInfo, ActivationInfo);
	if (!IsActivationCurrent(ThisGeneration)) { return; }
	if (!bCommitted)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo：激活提交失败。"));
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	if (HasAuthority(&ActivationInfo))
	{
		SavedMeshTick = ActiveMesh->VisibilityBasedAnimTickOption;
		bSavedUpdateRateOptimizations = ActiveMesh->bEnableUpdateRateOptimizations;
		ActiveMesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		ActiveMesh->bEnableUpdateRateOptimizations = false;
		bChangedMeshTick = true;
		TraceComponent->AddTickPrerequisiteComponent(ActiveMesh);
		TraceComponent->OnMeleeHit.AddUniqueDynamic(this, &ThisClass::HandleMeleeHit);
	}
	InputTask = UGGYGOAbilityTask_WaitComboInput::WaitComboInput(this);
	if (!InputTask)
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	InputTask->OnPress.AddDynamic(this, &ThisClass::HandleInputPressed);
	StartStep(0);
	if (!IsActivationCurrent(ThisGeneration)) { return; }
	if (!MontageTask || !ComboSteps.IsValidIndex(CurrentStep))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	UGGYGOAbilityTask_WaitComboInput* const StartedInputTask = InputTask;
	StartedInputTask->ReadyForActivation();
	if (!IsActivationCurrent(ThisGeneration) || InputTask != StartedInputTask) { return; }
}

bool UGGYGOPlayerComboAbility::IsActivationCurrent(uint64 ExpectedGeneration) const
{
	return LocalActivationGeneration == ExpectedGeneration
		&& EndRequestedActivationGeneration != ExpectedGeneration
		&& !bCleaningUp
		&& IsActive();
}

void UGGYGOPlayerComboAbility::HandleDeferredEnd(uint64 ExpectedGeneration, FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo,
	bool bReplicateEndAbility, bool bWasCancelled)
{
	if (LocalActivationGeneration != ExpectedGeneration
		|| EndRequestedActivationGeneration != ExpectedGeneration) { return; }
	EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}

bool UGGYGOPlayerComboAbility::StartStep(int32 Index, float Position)
{
	const uint64 ThisGeneration = LocalActivationGeneration;
	if (!IsActivationCurrent(ThisGeneration) || !IsStepPlayable(Index) || !ActiveMesh || !TraceComponent) { return false; }
	if (StepTokenCounter == MAX_uint64)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo：段激活令牌已耗尽。"));
		K2_CancelAbility();
		return false;
	}
	const FGGYGOComboStep& Step = ComboSteps[Index];
	if (!ActiveMesh->DoesSocketExist(Step.TraceStartSocket) || !ActiveMesh->DoesSocketExist(Step.TraceEndSocket))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo：段 %d 的武器 Socket 不存在 [%s → %s]。"),
			Index, *Step.TraceStartSocket.ToString(), *Step.TraceEndSocket.ToString());
		K2_CancelAbility();
		return false;
	}
	TraceComponent->EndTraceWindow();
	ReleaseMontageTask();
	if (!IsActivationCurrent(ThisGeneration)) { return false; }
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
	if (!StartedMontageTask)
	{
		K2_CancelAbility();
		return false;
	}
	MontageTask = StartedMontageTask;
	const float EffectivePlayRate = StartedMontageTask->GetEffectivePlayRate();
	const float MontageLength = Step.Montage->GetPlayLength();
	if (!FMath::IsFinite(EffectivePlayRate) || EffectivePlayRate <= 0.0f
		|| !FMath::IsFinite(MontageLength) || MontageLength <= 0.0f)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo：段 %d 的实际播放速率或时长无效。"), Index);
		K2_CancelAbility();
		return false;
	}
	StartedMontageTask->SetStartTimeSeconds(Position);
	StartedMontageTask->OnCompleted.AddDynamic(this, &ThisClass::HandleMontageCompleted);
	StartedMontageTask->OnInterrupted.AddDynamic(this, &ThisClass::HandleMontageInterrupted);
	StartedMontageTask->OnCancelled.AddDynamic(this, &ThisClass::HandleMontageInterrupted);
	StartedMontageTask->OnBlendOut.AddDynamic(this, &ThisClass::HandleMontageBlendOut);
	StartedMontageTask->EventReceived.AddDynamic(this, &ThisClass::HandleMontageEvent);
	UE_LOG(LogGGYGOAbilitySystem, Display, TEXT("PlayerCombo：%s 段 %d，Montage=%s，Rate=%.2f。"),
		HasAuthority(&CurrentActivationInfo) ? TEXT("服务器") : TEXT("预测端"), Index + 1,
		*GetNameSafe(Step.Montage), Step.PlayRate);
	StartedMontageTask->ReadyForActivation();
	if (!IsActivationCurrent(ThisGeneration) || CurrentStepToken != ThisStepToken
		|| CurrentStep != Index || MontageTask != StartedMontageTask) { return false; }
	// 默认收尾路径显式钉住，防止编辑器中意外关联到其他段或循环。
	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
	{
		ASC->CurrentMontageSetNextSectionName(Step.MainSection, Step.EndSection);
		ASC->CurrentMontageSetNextSectionName(Step.EndSection, NAME_None);
		if (!IsActivationCurrent(ThisGeneration) || CurrentStepToken != ThisStepToken
			|| CurrentStep != Index || MontageTask != StartedMontageTask) { return false; }
	}
	const float RemainingPlayTime = FMath::Max(0.0f, MontageLength - Position);
	const float Timeout = RemainingPlayTime / EffectivePlayRate + 2.0f;
	UWorld* const World = GetWorld();
	if (!World || !FMath::IsFinite(Timeout))
	{
		K2_CancelAbility();
		return false;
	}
	const FTimerDelegate WatchdogDelegate = FTimerDelegate::CreateUObject(
		this, &ThisClass::HandleWatchdog, ThisGeneration, ThisStepToken);
	World->GetTimerManager().SetTimer(WatchdogHandle, WatchdogDelegate, Timeout, false);
	return true;
}

void UGGYGOPlayerComboAbility::ReleaseMontageTask()
{
	if (!MontageTask) { return; }
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* PreviousTask = MontageTask;
	MontageTask = nullptr;
	PreviousTask->OnCompleted.Clear();
	PreviousTask->OnInterrupted.Clear();
	PreviousTask->OnCancelled.Clear();
	PreviousTask->OnBlendOut.Clear();
	PreviousTask->EventReceived.Clear();
	// EndTask 解除监听但保留动画，下一次 ASC PlayMontage 负责自然混合顶替。
	PreviousTask->EndTask();
}

void UGGYGOPlayerComboAbility::HandleInputPressed(int32 SourceStep, int32 RequestId)
{
	const uint64 ThisGeneration = LocalActivationGeneration;
	if (!IsActivationCurrent(ThisGeneration) || !ComboSteps.IsValidIndex(CurrentStep)) { return; }
	if (RequestId <= LastRequestId || RequestId > 65535) { return; }
	LastRequestId = RequestId;
	if (SourceStep != CurrentStep || ComboSteps[CurrentStep].NextStepIndex == INDEX_NONE
		|| !Window.Store(RequestId, GetWorld()->GetTimeSeconds(), InputBufferSeconds))
	{
		RejectRequest(RequestId);
		return;
	}
	TryAdvanceCombo();
}

void UGGYGOPlayerComboAbility::TryAdvanceCombo()
{
	const uint64 ThisGeneration = LocalActivationGeneration;
	if (!IsActivationCurrent(ThisGeneration) || !ComboSteps.IsValidIndex(CurrentStep)) { return; }
	const double Now = GetWorld()->GetTimeSeconds();
	if (Window.HasExpired(Now))
	{
		const int32 ExpiredId = Window.PendingRequestId;
		Window.PendingRequestId = 0;
		RejectRequest(ExpiredId);
		if (!IsActivationCurrent(ThisGeneration)) { return; }
	}
	const int32 RequestId = Window.Consume(Now);
	if (RequestId == 0) { return; }
	const int32 Next = ComboSteps[CurrentStep].NextStepIndex;
	if (Next <= CurrentStep || !IsStepPlayable(Next)) { RejectRequest(RequestId); return; }
	if (StartStep(Next) && IsActivationCurrent(ThisGeneration)
		&& CurrentStep == Next && CurrentStepToken != 0)
	{
		SendAuthoritativeStep(RequestId, true);
	}
}

void UGGYGOPlayerComboAbility::HandleMontageEvent(FGameplayTag EventTag, FGameplayEventData EventData)
{
	const uint64 ThisGeneration = LocalActivationGeneration;
	if (!IsActivationCurrent(ThisGeneration) || !ComboSteps.IsValidIndex(CurrentStep)
		|| EventData.OptionalObject != ComboSteps[CurrentStep].Montage
		|| (EventData.OptionalObject2 && EventData.OptionalObject2 != ActiveMesh)) { return; }
	if (EventTag == GGYGOGameplayTags::Event_Montage_ComboWindowBegin)
	{
		if (!Window.bClosed) { Window.bOpen = true; TryAdvanceCombo(); }
	}
	else if (EventTag == GGYGOGameplayTags::Event_Montage_ComboWindowEnd)
	{
		const int32 Pending = Window.PendingRequestId;
		Window.Close();
		if (Pending > 0) { RejectRequest(Pending); }
	}
	else if (HasAuthority(&CurrentActivationInfo) && TraceComponent)
	{
		if (EventTag == GGYGOGameplayTags::Event_Montage_HitWindowBegin)
		{
			const FGGYGOComboStep& Step = ComboSteps[CurrentStep];
			TraceComponent->BeginTraceWindow(Step.TraceStartSocket, Step.TraceEndSocket, Step.TraceRadius);
		}
		else if (EventTag == GGYGOGameplayTags::Event_Montage_HitWindowEnd) { TraceComponent->EndTraceWindow(); }
	}
}

void UGGYGOPlayerComboAbility::RejectRequest(int32 RequestId)
{
	SendAuthoritativeStep(RequestId, false);
}

void UGGYGOPlayerComboAbility::SendAuthoritativeStep(int32 RequestId, bool bAccepted)
{
	const uint64 ThisGeneration = LocalActivationGeneration;
	if (!IsActivationCurrent(ThisGeneration) || !HasAuthority(&CurrentActivationInfo)
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
	const uint64 ThisGeneration = LocalActivationGeneration;
	if (!IsActivationCurrent(ThisGeneration) || HasAuthority(&CurrentActivationInfo) || Correction.Num() != 1) { return; }
	const FGameplayAbilityTargetData* TargetData = Correction.Get(0);
	if (!TargetData || TargetData->GetScriptStruct() != FGGYGOComboCorrectionData::StaticStruct()) { return; }
	const FGGYGOComboCorrectionData& ComboCorrection = static_cast<const FGGYGOComboCorrectionData&>(*TargetData);
	if (!ComboCorrection.HasValidFields() || !IsStepPlayable(ComboCorrection.ServerStep)) { return; }
	const float ServerMontageLength = ComboSteps[ComboCorrection.ServerStep].Montage->GetPlayLength();
	if (!FMath::IsFinite(ServerMontageLength) || ComboCorrection.Position > ServerMontageLength) { return; }
	CorrectPredictedStep(ComboCorrection.Revision, ComboCorrection.RequestId, ComboCorrection.ServerStep,
		ComboCorrection.Position, ComboCorrection.bWindowOpen, ComboCorrection.bWindowClosed, ComboCorrection.bAccepted);
}

void UGGYGOPlayerComboAbility::CorrectPredictedStep(int32 Revision, int32 RequestId, int32 ServerStep, float Position,
	bool bWindowOpen, bool bWindowClosed, bool bAccepted)
{
	const uint64 ThisGeneration = LocalActivationGeneration;
	if (!IsActivationCurrent(ThisGeneration) || HasAuthority(&CurrentActivationInfo)
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
		if (!StartStep(ServerStep, SafePosition))
		{
			if (!IsActivationCurrent(ThisGeneration)) { return; }
			// ReadyForActivation can synchronously move the same activation to a newer step.
			// In that case this correction stack must leave the new task and its timer alone.
			if (CurrentStep != PreviousStep || CurrentStepToken != PreviousStepToken)
			{
				if (!MontageTask || !MontageTask->IsActive()) { K2_CancelAbility(); }
				return;
			}
			K2_CancelAbility();
			return;
		}
		if (!IsActivationCurrent(ThisGeneration) || CurrentStep != ServerStep) { return; }
		Window.bOpen = bWindowOpen;
		Window.bClosed = bWindowClosed;
	}
	UE_LOG(LogGGYGOAbilitySystem, Display, TEXT("PlayerCombo：请求 %d，服务器%s，同步到段 %d。"),
		RequestId, bAccepted ? TEXT("确认") : TEXT("拒绝"), ServerStep + 1);
}

void UGGYGOPlayerComboAbility::HandleMontageCompleted(FGameplayTag EventTag, FGameplayEventData EventData)
{
	if (!IsActivationCurrent(LocalActivationGeneration)) { return; }
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, HasAuthority(&CurrentActivationInfo), false);
}

void UGGYGOPlayerComboAbility::HandleMontageInterrupted(FGameplayTag EventTag, FGameplayEventData EventData)
{
	if (IsActivationCurrent(LocalActivationGeneration)) { K2_CancelAbility(); }
}

void UGGYGOPlayerComboAbility::HandleMontageBlendOut(FGameplayTag EventTag, FGameplayEventData EventData)
{
	const uint64 ThisGeneration = LocalActivationGeneration;
	if (!IsActivationCurrent(ThisGeneration)) { return; }
	const int32 Pending = Window.PendingRequestId;
	Window.Close();
	if (TraceComponent) { TraceComponent->EndTraceWindow(); }
	if (Pending > 0) { RejectRequest(Pending); }
}

void UGGYGOPlayerComboAbility::HandleWatchdog(uint64 ExpectedActivationGeneration, uint64 ExpectedStepToken)
{
	if (!IsActivationCurrent(ExpectedActivationGeneration) || CurrentStepToken != ExpectedStepToken) { return; }
	UE_LOG(LogGGYGOAbilitySystem, Warning, TEXT("PlayerCombo：动画完成回调超时，释放动作。"));
	K2_CancelAbility();
}

void UGGYGOPlayerComboAbility::HandleMeleeHit(AActor* HitActor, const FHitResult& HitResult)
{
	const uint64 ThisGeneration = LocalActivationGeneration;
	if (!IsActivationCurrent(ThisGeneration) || !HasAuthority(&CurrentActivationInfo)
		|| !HitActor || HitActor == GetAvatarActorFromActorInfo() || !ComboSteps.IsValidIndex(CurrentStep)) { return; }
	UAbilitySystemComponent* TargetASC = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(HitActor);
	AActor* SourceAvatar = GetAvatarActorFromActorInfo();
	if (!TargetASC || !SourceAvatar) { return; }
	const FGGYGOComboStep& Step = ComboSteps[CurrentStep];
	FGGYGOHitEffectPayload HitPayload;
	const TSubclassOf<UGameplayEffect> ResolvedDamageEffect = UGGYGOGameData::ResolveDamageGameplayEffect(
		DamageEffect, bUseSharedDamageEffectWhenUnset);
	if (!DamageEffect && bUseSharedDamageEffectWhenUnset && !ResolvedDamageEffect)
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning,
			TEXT("PlayerCombo：[%s] 已选择共享伤害GE，但预载结果不可用；本次使用无伤害GE的命中Cue路径。请检查System启动诊断及GameData伤害GE配置。"),
			*GetNameSafe(SourceAvatar));
	}
	if (!BuildHitEffectPayload(TargetASC, ResolvedDamageEffect, GetAbilityLevel(), HitResult,
		SourceAvatar->GetActorLocation(), HitPayload))
	{
		return;
	}
	if (HitPayload.EffectSpec.IsValid())
	{
		HitPayload.EffectSpec.Data->SetSetByCallerMagnitude(GGYGOGameplayTags::SetByCaller_Damage, Step.Damage);
		HitPayload.EffectSpec.Data->SetSetByCallerMagnitude(GGYGOGameplayTags::SetByCaller_PoiseDamage, Step.PoiseDamage);
		if (UAbilitySystemComponent* SourceASC = GetAbilitySystemComponentFromActorInfo())
		{
			SourceASC->ApplyGameplayEffectSpecToTarget(*HitPayload.EffectSpec.Data.Get(), TargetASC);
		}
	}
	if (HitCueTag.IsValid())
	{
		// 碰撞已经成立；GE 被免疫或拒绝不应吞掉命中表现。
		TargetASC->ExecuteGameplayCue(HitCueTag, HitPayload.CueParameters);
	}
}

void UGGYGOPlayerComboAbility::EndAbility(FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo,
	bool bReplicateEndAbility, bool bWasCancelled)
{
	if (!IsEndAbilityValid(Handle, ActorInfo)) { return; }
	if (ScopeLockCount > 0)
	{
		if (EndRequestedActivationGeneration != LocalActivationGeneration)
		{
			const uint64 ExpectedGeneration = LocalActivationGeneration;
			EndRequestedActivationGeneration = ExpectedGeneration;
			WaitingToExecute.Add(FPostLockDelegate::CreateUObject(this, &ThisClass::HandleDeferredEnd,
				ExpectedGeneration, Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled));
		}
		return;
	}
	if (bCleaningUp) { return; }

	bCleaningUp = true;
	const uint64 EndingGeneration = LocalActivationGeneration;
	Window.Close();
	if (UWorld* World = GetWorld()) { World->GetTimerManager().ClearTimer(WatchdogHandle); }
	CurrentStep = INDEX_NONE;
	CurrentStepToken = 0;

	UGGYGOAbilityTask_WaitComboInput* const EndingInputTask = InputTask;
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* const EndingMontageTask = MontageTask;
	UGGYGOMeleeTraceComponent* const EndingTraceComponent = TraceComponent;
	USkeletalMeshComponent* const EndingMesh = ActiveMesh;
	const bool bRestoreMesh = bChangedMeshTick;
	const EVisibilityBasedAnimTickOption MeshTickToRestore = SavedMeshTick;
	const bool bUpdateRateToRestore = bSavedUpdateRateOptimizations;

	InputTask = nullptr;
	MontageTask = nullptr;
	TraceComponent = nullptr;
	ActiveMesh = nullptr;
	bChangedMeshTick = false;
	bSavedUpdateRateOptimizations = false;
	SavedMeshTick = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;

	if (EndingTraceComponent)
	{
		EndingTraceComponent->EndTraceWindow();
		EndingTraceComponent->OnMeleeHit.RemoveDynamic(this, &ThisClass::HandleMeleeHit);
		if (EndingMesh) { EndingTraceComponent->RemoveTickPrerequisiteComponent(EndingMesh); }
	}
	if (EndingMesh && bRestoreMesh)
	{
		EndingMesh->VisibilityBasedAnimTickOption = MeshTickToRestore;
		EndingMesh->bEnableUpdateRateOptimizations = bUpdateRateToRestore;
	}
	if (EndingInputTask)
	{
		EndingInputTask->OnPress.Clear();
		EndingInputTask->TaskOwnerEnded();
	}
	if (EndingMontageTask)
	{
		EndingMontageTask->OnCompleted.Clear();
		EndingMontageTask->OnInterrupted.Clear();
		EndingMontageTask->OnCancelled.Clear();
		EndingMontageTask->OnBlendOut.Clear();
		EndingMontageTask->EventReceived.Clear();
		// GAS normally ends tasks after OnGameplayAbilityEnded; stop this task's Montage before that notification.
		EndingMontageTask->TaskOwnerEnded();
	}
	if (LocalActivationGeneration != EndingGeneration) { return; }

	// Do not touch instance state after Super: its end notification can reactivate this same instance.
	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}
