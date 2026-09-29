/** @file GGYGOPlayerComboAbility.cpp */
#include "AbilitySystem/Abilities/GGYGOPlayerComboAbility.h"

#include "AbilitySystemBlueprintLibrary.h"
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

void UGGYGOPlayerComboAbility::ActivateAbility(FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);
	bCleaningUp = false;
	CurrentStep = INDEX_NONE;
	LastRequestId = StepSyncRevision = 0;
	Window.Reset();
	ACharacter* Character = GetCharacterFromActorInfo();
	ActiveMesh = Character ? Character->GetMesh() : nullptr;
	TraceComponent = Character ? Character->FindComponentByClass<UGGYGOMeleeTraceComponent>() : nullptr;
	if (!ActiveMesh || !TraceComponent || !IsStepPlayable(0) || !CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo：Avatar、Trace 或第一段配置无效。"));
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
	InputTask->OnPress.AddDynamic(this, &ThisClass::HandleInputPressed);
	if (StartStep(0) && IsActive()) { InputTask->ReadyForActivation(); }
}

bool UGGYGOPlayerComboAbility::StartStep(int32 Index, float Position)
{
	if (!IsStepPlayable(Index) || !ActiveMesh || !TraceComponent) { return false; }
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
	Window.Reset();
	CurrentStep = Index;
	if (InputTask) { InputTask->SetSourceStep(Index); }
	FGameplayTagContainer Events;
	Events.AddTag(GGYGOGameplayTags::Event_Montage_ComboWindowBegin);
	Events.AddTag(GGYGOGameplayTags::Event_Montage_ComboWindowEnd);
	Events.AddTag(GGYGOGameplayTags::Event_Montage_HitWindowBegin);
	Events.AddTag(GGYGOGameplayTags::Event_Montage_HitWindowEnd);
	MontageTask = UGGYGOAbilityTask_PlayMontageAndWaitForEvent::PlayMontageAndWaitForEvent(
		this, TEXT("PlayerCombo"), Step.Montage, Events, Step.PlayRate,
		Position > 0.0f ? NAME_None : Step.MainSection);
	MontageTask->SetStartTimeSeconds(Position);
	MontageTask->OnCompleted.AddDynamic(this, &ThisClass::HandleMontageCompleted);
	MontageTask->OnInterrupted.AddDynamic(this, &ThisClass::HandleMontageInterrupted);
	MontageTask->OnCancelled.AddDynamic(this, &ThisClass::HandleMontageInterrupted);
	MontageTask->OnBlendOut.AddDynamic(this, &ThisClass::HandleMontageBlendOut);
	MontageTask->EventReceived.AddDynamic(this, &ThisClass::HandleMontageEvent);
	UE_LOG(LogGGYGOAbilitySystem, Display, TEXT("PlayerCombo：%s 段 %d，Montage=%s，Rate=%.2f。"),
		HasAuthority(&CurrentActivationInfo) ? TEXT("服务器") : TEXT("预测端"), Index + 1,
		*GetNameSafe(Step.Montage), Step.PlayRate);
	MontageTask->ReadyForActivation();
	if (!IsActive() || bCleaningUp) { return false; }
	// 默认收尾路径显式钉住，防止编辑器中意外关联到其他段或循环。
	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
	{
		ASC->CurrentMontageSetNextSectionName(Step.MainSection, Step.EndSection);
		ASC->CurrentMontageSetNextSectionName(Step.EndSection, NAME_None);
	}
	const float Timeout = (Step.Montage->GetPlayLength() - Position) / Step.PlayRate + 2.0f;
	GetWorld()->GetTimerManager().SetTimer(WatchdogHandle, this, &ThisClass::HandleWatchdog, Timeout, false);
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
	if (!IsActive() || bCleaningUp || !ComboSteps.IsValidIndex(CurrentStep)) { return; }
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
	if (!IsActive() || bCleaningUp || !ComboSteps.IsValidIndex(CurrentStep)) { return; }
	const double Now = GetWorld()->GetTimeSeconds();
	if (Window.HasExpired(Now))
	{
		const int32 ExpiredId = Window.PendingRequestId;
		Window.PendingRequestId = 0;
		RejectRequest(ExpiredId);
	}
	const int32 RequestId = Window.Consume(Now);
	if (RequestId == 0) { return; }
	const int32 Next = ComboSteps[CurrentStep].NextStepIndex;
	if (Next <= CurrentStep || !IsStepPlayable(Next)) { RejectRequest(RequestId); return; }
	if (StartStep(Next)) { SendAuthoritativeStep(RequestId, true); }
}

void UGGYGOPlayerComboAbility::HandleMontageEvent(FGameplayTag EventTag, FGameplayEventData EventData)
{
	if (!IsActive() || bCleaningUp || !ComboSteps.IsValidIndex(CurrentStep)
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
	if (!HasAuthority(&CurrentActivationInfo) || !CurrentActorInfo || CurrentActorInfo->IsLocallyControlled()) { return; }
	UGGYGOAbilitySystemComponent* ASC = GetGGYGOAbilitySystemComponentFromActorInfo();
	UAnimInstance* AnimInstance = CurrentActorInfo->GetAnimInstance();
	if (ASC && AnimInstance && ComboSteps.IsValidIndex(CurrentStep))
	{
		ASC->ClientCorrectComboStep(CurrentSpecHandle, CurrentActivationInfo.GetActivationPredictionKey(),
			++StepSyncRevision, RequestId, CurrentStep, AnimInstance->Montage_GetPosition(ComboSteps[CurrentStep].Montage),
			Window.bOpen, Window.bClosed, bAccepted);
	}
}

void UGGYGOPlayerComboAbility::CorrectPredictedStep(int32 Revision, int32 RequestId, int32 ServerStep, float Position,
	bool bWindowOpen, bool bWindowClosed, bool bAccepted)
{
	if (!IsActive() || bCleaningUp || HasAuthority(&CurrentActivationInfo) || Revision <= StepSyncRevision) { return; }
	StepSyncRevision = Revision;
	// 已确认的旧请求不回滚客户端更新的预测；服务器拒绝则撤销依赖该预测的段。
	if (bAccepted && CurrentStep >= ServerStep) { return; }
	if (!bAccepted) { Window.PendingRequestId = 0; }
	if (ServerStep != CurrentStep)
	{
		if (!IsStepPlayable(ServerStep)) { K2_CancelAbility(); return; }
		const float SafePosition = FMath::Clamp(Position, 0.0f, ComboSteps[ServerStep].Montage->GetPlayLength() - 0.001f);
		if (!StartStep(ServerStep, SafePosition)) { K2_CancelAbility(); return; }
		Window.bOpen = bWindowOpen;
		Window.bClosed = bWindowClosed;
	}
	UE_LOG(LogGGYGOAbilitySystem, Display, TEXT("PlayerCombo：请求 %d，服务器%s，同步到段 %d。"),
		RequestId, bAccepted ? TEXT("确认") : TEXT("拒绝"), ServerStep + 1);
}

void UGGYGOPlayerComboAbility::HandleMontageCompleted(FGameplayTag EventTag, FGameplayEventData EventData)
{
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, HasAuthority(&CurrentActivationInfo), false);
}

void UGGYGOPlayerComboAbility::HandleMontageInterrupted(FGameplayTag EventTag, FGameplayEventData EventData)
{
	if (!bCleaningUp) { K2_CancelAbility(); }
}

void UGGYGOPlayerComboAbility::HandleMontageBlendOut(FGameplayTag EventTag, FGameplayEventData EventData)
{
	const int32 Pending = Window.PendingRequestId;
	Window.Close();
	if (TraceComponent) { TraceComponent->EndTraceWindow(); }
	if (Pending > 0) { RejectRequest(Pending); }
}

void UGGYGOPlayerComboAbility::HandleWatchdog()
{
	UE_LOG(LogGGYGOAbilitySystem, Warning, TEXT("PlayerCombo：动画完成回调超时，释放动作。"));
	K2_CancelAbility();
}

void UGGYGOPlayerComboAbility::HandleMeleeHit(AActor* HitActor, const FHitResult& HitResult)
{
	if (!IsActive() || bCleaningUp || !HasAuthority(&CurrentActivationInfo)
		|| !HitActor || HitActor == GetAvatarActorFromActorInfo() || !ComboSteps.IsValidIndex(CurrentStep)) { return; }
	UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo();
	UAbilitySystemComponent* TargetASC = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(HitActor);
	if (!ASC || !TargetASC) { return; }
	const FGGYGOComboStep& Step = ComboSteps[CurrentStep];
	FGameplayEffectContextHandle Context = MakeEffectContext(CurrentSpecHandle, CurrentActorInfo);
	Context.AddHitResult(HitResult, true);
	if (DamageEffect)
	{
		FGameplayEffectSpecHandle Spec = ASC->MakeOutgoingSpec(DamageEffect, GetAbilityLevel(), Context);
		if (Spec.IsValid())
		{
			Spec.Data->SetSetByCallerMagnitude(GGYGOGameplayTags::SetByCaller_Damage, Step.Damage);
			Spec.Data->SetSetByCallerMagnitude(GGYGOGameplayTags::SetByCaller_PoiseDamage, Step.PoiseDamage);
			ASC->ApplyGameplayEffectSpecToTarget(*Spec.Data.Get(), TargetASC);
		}
	}
	if (HitCueTag.IsValid())
	{
		FGameplayCueParameters Cue;
		Cue.EffectContext = Context;
		Cue.Location = HitResult.ImpactPoint;
		Cue.Normal = HitResult.ImpactNormal;
		Cue.Instigator = GetAvatarActorFromActorInfo();
		Cue.EffectCauser = GetAvatarActorFromActorInfo();
		TargetASC->ExecuteGameplayCue(HitCueTag, Cue);
	}
}

void UGGYGOPlayerComboAbility::EndAbility(FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo,
	bool bReplicateEndAbility, bool bWasCancelled)
{
	if (bCleaningUp) { return; }
	bCleaningUp = true;
	Window.Close();
	if (UWorld* World = GetWorld()) { World->GetTimerManager().ClearTimer(WatchdogHandle); }
	if (TraceComponent)
	{
		TraceComponent->EndTraceWindow();
		TraceComponent->OnMeleeHit.RemoveDynamic(this, &ThisClass::HandleMeleeHit);
		if (ActiveMesh) { TraceComponent->RemoveTickPrerequisiteComponent(ActiveMesh); }
	}
	if (InputTask) { InputTask->EndTask(); InputTask = nullptr; }
	// 先让任务停止自己拥有的动画，再释放资产与 Mesh 引用。
	if (MontageTask)
	{
		MontageTask->OnInterrupted.Clear();
		MontageTask->OnCancelled.Clear();
	}
	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
	MontageTask = nullptr;
	if (ActiveMesh && bChangedMeshTick)
	{
		ActiveMesh->VisibilityBasedAnimTickOption = SavedMeshTick;
		ActiveMesh->bEnableUpdateRateOptimizations = bSavedUpdateRateOptimizations;
	}
	bChangedMeshTick = false;
	ActiveMesh = nullptr;
	TraceComponent = nullptr;
	CurrentStep = INDEX_NONE;
}
