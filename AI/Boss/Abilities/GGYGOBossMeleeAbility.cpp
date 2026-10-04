/** @file GGYGOBossMeleeAbility.cpp */
#include "AI/Boss/Abilities/GGYGOBossMeleeAbility.h"

#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "AIController.h"
#include "AbilitySystem/Groups/GGYGOAbilityGroupTypes.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "AbilitySystem/Tasks/GGYGOAbilityTask_PlayMontageAndWaitForEvent.h"
#include "Animation/AnimMontage.h"
#include "Combat/HitDetection/GGYGOMeleeTraceComponent.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Character/Data/GGYGOActionMotionProfile.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameplayEffect.h"
#include "System/GGYGOGameData.h"
#include "System/GGYGOGameplayTags.h"
#include "TimerManager.h"
#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOBossMeleeAbility)

UGGYGOBossMeleeAbility::UGGYGOBossMeleeAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;
	NetSecurityPolicy = EGameplayAbilityNetSecurityPolicy::ServerOnly;
	GroupTag = GGYGOGameplayTags::AbilityGroup_Attack;
	ActivationPriority = GGYGOAbilityGroupDefaults::Priority_LightAttack;
	SelfPolicy = EGGYGOAbilitySelfPolicy::Coexist;
	ActionTag = GGYGOGameplayTags::BossAction_Attack_Melee;
	HitCueTag = GGYGOGameplayTags::GameplayCue_Hit_Flesh;
}

bool UGGYGOBossMeleeAbility::ValidateMeleeConfiguration(FString& OutError) const
{
	const TSubclassOf<UGameplayEffect> ResolvedDamageEffect = UGGYGOGameData::ResolveDamageGameplayEffect(
		DamageEffect, bUseSharedDamageEffectWhenUnset);
	if (!AttackMontage || !ResolvedDamageEffect || TraceStartSocket.IsNone() || TraceEndSocket.IsNone()
		|| !FMath::IsFinite(AttackMontage->GetPlayLength()) || AttackMontage->GetPlayLength() <= 0.0f
		|| !FMath::IsFinite(AttackMontage->RateScale) || AttackMontage->RateScale <= 0.0f
		|| !FMath::IsFinite(MontagePlayRate) || MontagePlayRate <= 0.0f
		|| !FMath::IsFinite(TraceRadius) || TraceRadius < 1.0f
		|| !FMath::IsFinite(Damage) || Damage < 0.0f
		|| !FMath::IsFinite(PoiseDamage) || PoiseDamage < 0.0f)
	{
		OutError = TEXT("需要有效 Montage、伤害GE（DamageEffect覆盖，或显式启用且预载可用的共享GE）、Socket 和有限非负伤害；TraceRadius 至少为 1。");
		return false;
	}
	if (ActionMotionProfile)
	{
		if (!ActionMotionProfile->ValidateMotion(OutError)) { return false; }
		if (!FMath::IsNearlyEqual(ActionMotionProfile->Duration, AttackMontage->GetPlayLength(), 0.001f)
			|| AttackMontage->HasRootMotion())
		{
			OutError = TEXT("MotionProfile 与 Montage 时长须一致（误差不超过 1ms），且 Montage 不得启用原生 RootMotion。");
			return false;
		}
	}
	OutError.Reset();
	return true;
}

#if WITH_EDITOR
EDataValidationResult UGGYGOBossMeleeAbility::IsDataValid(FDataValidationContext& Context) const
{
	const EDataValidationResult BaseResult = Super::IsDataValid(Context);
	FString Error;
	if (!ValidateMeleeConfiguration(Error))
	{
		Context.AddError(FText::FromString(Error));
		return EDataValidationResult::Invalid;
	}
	return BaseResult == EDataValidationResult::Invalid ? BaseResult : EDataValidationResult::Valid;
}
#endif

void UGGYGOBossMeleeAbility::ActivateAbility(
	const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	bCleaningUp = false;
	bBlendingOut = false;
	ActionMotionHandle = INDEX_NONE;
	ActiveMotionMovement = nullptr;
	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);
	if (!IsActive() || bCleaningUp) { return; }

	AActor* Avatar = GetAvatarActorFromActorInfo();
	ACharacter* Character = Cast<ACharacter>(Avatar);
	ActiveMesh = Character ? Character->GetMesh() : nullptr;
	ActiveTraceComponent = Avatar ? Avatar->FindComponentByClass<UGGYGOMeleeTraceComponent>() : nullptr;
	ActiveMotionMovement = Character && ActionMotionProfile
		? Cast<UGGYGOCharacterMovementComponent>(Character->GetCharacterMovement()) : nullptr;
	float TaskPlayRate = 0.0f;
	float EffectivePlayRate = 0.0f;
	const bool bHasValidPlayRate = UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolvePlayRate(
		AttackMontage, MontagePlayRate, TaskPlayRate, EffectivePlayRate);
	FString ConfigurationError;
	if (!HasAuthority(&ActivationInfo) || !ValidateMeleeConfiguration(ConfigurationError)
		|| !ActiveMesh || !ActiveMesh->GetAnimInstance() || !ActiveTraceComponent
		|| !ActiveMesh->DoesSocketExist(TraceStartSocket) || !ActiveMesh->DoesSocketExist(TraceEndSocket)
		|| !bHasValidPlayRate
		|| !FMath::IsFinite(AttackMontage->GetPlayLength() / EffectivePlayRate + 2.0f)
		|| (ActionMotionProfile && (!ActiveMotionMovement || ActiveMotionMovement->HasActiveActionMotion()))
		|| !CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("BossMelee [%s] 激活失败：Montage [%s]，Trace [%s]，Socket [%s -> %s]。%s"),
			*GetNameSafe(this), *GetNameSafe(AttackMontage), *GetNameSafe(ActiveTraceComponent),
			*TraceStartSocket.ToString(), *TraceEndSocket.ToString(), *ConfigurationError);
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	SavedMeshTick = ActiveMesh->VisibilityBasedAnimTickOption;
	bSavedUpdateRateOptimizations = ActiveMesh->bEnableUpdateRateOptimizations;
	ActiveMesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	ActiveMesh->bEnableUpdateRateOptimizations = false;
	bChangedMeshTick = true;
	ActiveTraceComponent->EndTraceWindow();
	ActiveTraceComponent->AddTickPrerequisiteComponent(ActiveMesh);
	// 寻路属于 Controller；动作结束后由行为树下一次请求决定是否继续移动。
	if (AAIController* Controller = Cast<AAIController>(Character->GetController())) { Controller->StopMovement(); }
	FGameplayTagContainer EventTags;
	EventTags.AddTag(GGYGOGameplayTags::Event_Montage_HitWindowBegin);
	EventTags.AddTag(GGYGOGameplayTags::Event_Montage_HitWindowEnd);
	MontageTask = UGGYGOAbilityTask_PlayMontageAndWaitForEvent::PlayMontageAndWaitForEvent(
		this, TEXT("BossMelee"), AttackMontage, EventTags, MontagePlayRate);
	// Commit 回调可能调整全局速率；位移与 watchdog 必须消费实际任务的同一份快照。
	EffectivePlayRate = MontageTask->GetEffectivePlayRate();
	if (!FMath::IsFinite(EffectivePlayRate) || EffectivePlayRate <= 0.0f
		|| !FMath::IsFinite(AttackMontage->GetPlayLength() / EffectivePlayRate + 2.0f))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("BossMelee [%s] 实际播放速率或超时时长无效。"), *GetNameSafe(this));
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	if (ActionMotionProfile)
	{
		ActionMotionHandle = ActiveMotionMovement
			? ActiveMotionMovement->BeginActionMotion(ActionMotionProfile, EffectivePlayRate) : INDEX_NONE;
		if (ActionMotionHandle == INDEX_NONE)
		{
			UE_LOG(LogGGYGOAbilitySystem, Warning, TEXT("BossMelee [%s] 动作位移启动失败，取消能力。"), *GetNameSafe(this));
			EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
			return;
		}
	}

	ActiveTraceComponent->OnMeleeHit.AddUniqueDynamic(this, &ThisClass::HandleMeleeHit);
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("BossMelee: [%s] 开始播放 [%s]。"), *GetNameSafe(Avatar), *GetNameSafe(AttackMontage));

	MontageTask->OnCompleted.AddDynamic(this, &ThisClass::HandleMontageCompleted);
	MontageTask->OnInterrupted.AddDynamic(this, &ThisClass::HandleMontageInterrupted);
	MontageTask->OnCancelled.AddDynamic(this, &ThisClass::HandleMontageInterrupted);
	MontageTask->OnBlendOut.AddDynamic(this, &ThisClass::HandleMontageBlendOut);
	MontageTask->EventReceived.AddDynamic(this, &ThisClass::HandleMontageEvent);
	MontageTask->ReadyForActivation();
	if (IsActive() && !bCleaningUp)
	{
		// 只处理异常循环或完成回调丢失；命中窗口始终由动画通知驱动。
		GetWorld()->GetTimerManager().SetTimer(MontageTimeoutHandle, this,
			&ThisClass::HandleMontageTimeout, AttackMontage->GetPlayLength() / EffectivePlayRate + 2.0f, false);
	}
}

void UGGYGOBossMeleeAbility::EndAbility(
	const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled)
{
	if (bCleaningUp) { return; }
	bCleaningUp = true;
	bBlendingOut = true;
	if (UWorld* World = GetWorld()) { World->GetTimerManager().ClearTimer(MontageTimeoutHandle); }
	if (ActiveTraceComponent)
	{
		ActiveTraceComponent->EndTraceWindow();
		ActiveTraceComponent->OnMeleeHit.RemoveDynamic(this, &ThisClass::HandleMeleeHit);
		if (ActiveMesh) { ActiveTraceComponent->RemoveTickPrerequisiteComponent(ActiveMesh); }
	}
	if (MontageTask)
	{
		MontageTask->OnCompleted.Clear();
		MontageTask->OnInterrupted.Clear();
		MontageTask->OnCancelled.Clear();
		MontageTask->OnBlendOut.Clear();
		MontageTask->EventReceived.Clear();
		// 使用 AbilityEnded 语义停止本任务拥有的 Montage，并在结束广播前解绑。
		MontageTask->TaskOwnerEnded();
	}
	if (ActiveMesh && bChangedMeshTick)
	{
		ActiveMesh->VisibilityBasedAnimTickOption = SavedMeshTick;
		ActiveMesh->bEnableUpdateRateOptimizations = bSavedUpdateRateOptimizations;
	}
	bChangedMeshTick = false;
	if (ActiveMotionMovement && ActionMotionHandle != INDEX_NONE)
	{
		ActiveMotionMovement->EndActionMotion(ActionMotionHandle);
	}
	ActionMotionHandle = INDEX_NONE;
	ActiveMotionMovement = nullptr;
	ActiveMesh = nullptr;
	ActiveTraceComponent = nullptr;
	MontageTask = nullptr;
	// OnAbilityEnded 可同步激活同一实例；广播后不能再改本实例状态。
	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}

void UGGYGOBossMeleeAbility::HandleMontageCompleted(FGameplayTag EventTag, FGameplayEventData EventData)
{
	if (!bCleaningUp) { K2_EndAbility(); }
}

void UGGYGOBossMeleeAbility::HandleMontageInterrupted(FGameplayTag EventTag, FGameplayEventData EventData)
{
	if (!bCleaningUp) { K2_CancelAbility(); }
}

void UGGYGOBossMeleeAbility::HandleMontageBlendOut(FGameplayTag EventTag, FGameplayEventData EventData)
{
	bBlendingOut = true;
	if (ActiveTraceComponent) { ActiveTraceComponent->EndTraceWindow(); }
}

void UGGYGOBossMeleeAbility::HandleMontageTimeout()
{
	UE_LOG(LogGGYGOAbilitySystem, Warning, TEXT("BossMelee [%s] 完成回调超时，取消动作。"), *GetNameSafe(this));
	if (!bCleaningUp) { K2_CancelAbility(); }
}

void UGGYGOBossMeleeAbility::HandleMontageEvent(FGameplayTag EventTag, FGameplayEventData EventData)
{
	// Task 已校验 Montage 播放实例与 ASC 所有权；此处再限定主 Mesh 与当前配置资产。
	if (!IsActive() || bCleaningUp || bBlendingOut || !HasAuthority(&CurrentActivationInfo)
		|| !ActiveTraceComponent || EventData.OptionalObject != AttackMontage
		|| EventData.OptionalObject2 != ActiveMesh)
	{
		return;
	}

	if (EventTag == GGYGOGameplayTags::Event_Montage_HitWindowBegin)
	{
		// 首版只接受非重叠窗口；重复 Begin 不得清空本窗口命中去重集合。
		if (ActiveTraceComponent->IsTracing()) { return; }
		UE_LOG(LogGGYGOAbilitySystem, Display,
			TEXT("BossMelee: 命中窗口开启 [%s -> %s, R=%.1f]。"),
			*TraceStartSocket.ToString(), *TraceEndSocket.ToString(), TraceRadius);
		ActiveTraceComponent->BeginTraceWindow(TraceStartSocket, TraceEndSocket, TraceRadius);
	}
	else if (EventTag == GGYGOGameplayTags::Event_Montage_HitWindowEnd)
	{
		UE_LOG(LogGGYGOAbilitySystem, Display, TEXT("BossMelee: 命中窗口关闭。"));
		ActiveTraceComponent->EndTraceWindow();
	}
}

void UGGYGOBossMeleeAbility::HandleMeleeHit(AActor* HitActor, const FHitResult& HitResult)
{
	if (!IsActive() || bCleaningUp || bBlendingOut || !ActiveTraceComponent || !ActiveTraceComponent->IsTracing()
		|| !HitActor || HitActor == GetAvatarActorFromActorInfo() || !HasAuthority(&CurrentActivationInfo))
	{
		return;
	}

	UAbilitySystemComponent* TargetASC = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(HitActor);
	AActor* SourceAvatar = GetAvatarActorFromActorInfo();
	if (!TargetASC || !SourceAvatar)
	{
		return;
	}
	const TSubclassOf<UGameplayEffect> ResolvedDamageEffect = UGGYGOGameData::ResolveDamageGameplayEffect(
		DamageEffect, bUseSharedDamageEffectWhenUnset);
	if (!ResolvedDamageEffect)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("BossMelee [%s] 伤害GE不可用：需要DamageEffect覆盖，或显式启用且预载可用的共享GE；拒绝本次命中执行。"),
			*GetNameSafe(this));
		return;
	}
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("BossMelee: [%s] 命中 [%s]，Damage [%.1f]，PoiseDamage [%.1f]。"),
		*GetNameSafe(GetAvatarActorFromActorInfo()), *GetNameSafe(HitActor), Damage, PoiseDamage);

	FGGYGOHitEffectPayload HitPayload;
	if (!BuildHitEffectPayload(TargetASC, ResolvedDamageEffect, GetAbilityLevel(), HitResult,
		SourceAvatar->GetActorLocation(), HitPayload))
	{
		return;
	}
	if (HitPayload.EffectSpec.IsValid())
	{
		HitPayload.EffectSpec.Data->SetSetByCallerMagnitude(GGYGOGameplayTags::SetByCaller_Damage, Damage);
		HitPayload.EffectSpec.Data->SetSetByCallerMagnitude(GGYGOGameplayTags::SetByCaller_PoiseDamage, PoiseDamage);
		if (UAbilitySystemComponent* SourceASC = GetAbilitySystemComponentFromActorInfo())
		{
			SourceASC->ApplyGameplayEffectSpecToTarget(*HitPayload.EffectSpec.Data.Get(), TargetASC);
		}
	}

	if (HitCueTag.IsValid())
	{
		TargetASC->ExecuteGameplayCue(HitCueTag, HitPayload.CueParameters);
	}
}
