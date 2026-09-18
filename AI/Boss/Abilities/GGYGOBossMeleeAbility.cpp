/** @file GGYGOBossMeleeAbility.cpp */
#include "AI/Boss/Abilities/GGYGOBossMeleeAbility.h"

#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystem/Groups/GGYGOAbilityGroupTypes.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "AbilitySystem/Tasks/GGYGOAbilityTask_PlayMontageAndWaitForEvent.h"
#include "Combat/HitDetection/GGYGOMeleeTraceComponent.h"
#include "Engine/HitResult.h"
#include "System/GGYGOGameplayTags.h"

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

void UGGYGOBossMeleeAbility::ActivateAbility(
	const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);

	AActor* Avatar = GetAvatarActorFromActorInfo();
	ActiveTraceComponent = Avatar ? Avatar->FindComponentByClass<UGGYGOMeleeTraceComponent>() : nullptr;
	if (!AttackMontage || !ActiveTraceComponent || !CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("BossMelee [%s] 激活失败：Montage [%s]，TraceComponent [%s]。"),
			*GetNameSafe(this), *GetNameSafe(AttackMontage), *GetNameSafe(ActiveTraceComponent));
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	ActiveTraceComponent->OnMeleeHit.AddUniqueDynamic(this, &ThisClass::HandleMeleeHit);
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("BossMelee: [%s] 开始播放 [%s]。"), *GetNameSafe(Avatar), *GetNameSafe(AttackMontage));

	FGameplayTagContainer EventTags;
	EventTags.AddTag(GGYGOGameplayTags::Event_Montage_HitWindowBegin);
	EventTags.AddTag(GGYGOGameplayTags::Event_Montage_HitWindowEnd);
	MontageTask = UGGYGOAbilityTask_PlayMontageAndWaitForEvent::PlayMontageAndWaitForEvent(
		this, TEXT("BossMelee"), AttackMontage, EventTags);
	MontageTask->OnCompleted.AddDynamic(this, &ThisClass::HandleMontageCompleted);
	MontageTask->OnInterrupted.AddDynamic(this, &ThisClass::HandleMontageInterrupted);
	MontageTask->OnCancelled.AddDynamic(this, &ThisClass::HandleMontageInterrupted);
	MontageTask->EventReceived.AddDynamic(this, &ThisClass::HandleMontageEvent);
	MontageTask->ReadyForActivation();
}

void UGGYGOBossMeleeAbility::EndAbility(
	const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled)
{
	if (ActiveTraceComponent)
	{
		ActiveTraceComponent->EndTraceWindow();
		ActiveTraceComponent->OnMeleeHit.RemoveDynamic(this, &ThisClass::HandleMeleeHit);
	}
	ActiveTraceComponent = nullptr;
	MontageTask = nullptr;

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}

void UGGYGOBossMeleeAbility::HandleMontageCompleted(FGameplayTag EventTag, FGameplayEventData EventData)
{
	K2_EndAbility();
}

void UGGYGOBossMeleeAbility::HandleMontageInterrupted(FGameplayTag EventTag, FGameplayEventData EventData)
{
	K2_CancelAbility();
}

void UGGYGOBossMeleeAbility::HandleMontageEvent(FGameplayTag EventTag, FGameplayEventData EventData)
{
	if (!ActiveTraceComponent)
	{
		return;
	}

	if (EventTag == GGYGOGameplayTags::Event_Montage_HitWindowBegin)
	{
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
	if (!HitActor || HitActor == GetAvatarActorFromActorInfo() || !HasAuthority(&CurrentActivationInfo))
	{
		return;
	}

	UAbilitySystemComponent* SourceASC = GetAbilitySystemComponentFromActorInfo();
	UAbilitySystemComponent* TargetASC = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(HitActor);
	if (!SourceASC || !TargetASC)
	{
		return;
	}
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("BossMelee: [%s] 命中 [%s]，Damage [%.1f]，PoiseDamage [%.1f]。"),
		*GetNameSafe(GetAvatarActorFromActorInfo()), *GetNameSafe(HitActor), Damage, PoiseDamage);

	FGameplayEffectContextHandle EffectContext = MakeEffectContext(CurrentSpecHandle, CurrentActorInfo);
	EffectContext.AddHitResult(HitResult, true);
	if (DamageEffect)
	{
		FGameplayEffectSpecHandle DamageSpec = SourceASC->MakeOutgoingSpec(DamageEffect, GetAbilityLevel(), EffectContext);
		if (DamageSpec.IsValid())
		{
			DamageSpec.Data->SetSetByCallerMagnitude(GGYGOGameplayTags::SetByCaller_Damage, Damage);
			DamageSpec.Data->SetSetByCallerMagnitude(GGYGOGameplayTags::SetByCaller_PoiseDamage, PoiseDamage);
			SourceASC->ApplyGameplayEffectSpecToTarget(*DamageSpec.Data.Get(), TargetASC);
		}
	}

	if (HitCueTag.IsValid())
	{
		FGameplayCueParameters CueParameters;
		CueParameters.EffectContext = EffectContext;
		CueParameters.Location = HitResult.ImpactPoint;
		CueParameters.Normal = HitResult.ImpactNormal;
		CueParameters.Instigator = GetAvatarActorFromActorInfo();
		CueParameters.EffectCauser = GetAvatarActorFromActorInfo();
		TargetASC->ExecuteGameplayCue(HitCueTag, CueParameters);
	}
}
