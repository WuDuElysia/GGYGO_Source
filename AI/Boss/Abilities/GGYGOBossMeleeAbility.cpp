/** @file GGYGOBossMeleeAbility.cpp @brief Original melee resource ownership, playback and CMC motion requests. */
#include "AI/Boss/Abilities/GGYGOBossMeleeAbility.h"

#include "AbilitySystem/Groups/GGYGOAbilityGroupTypes.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "AbilitySystem/Tasks/GGYGOAbilityTask_PlayMontageAndWaitForEvent.h"
#include "Animation/Data/GGYGOActionMotionSourceBinding.h"
#include "Combat/HitDetection/GGYGOMeleeTraceComponent.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Character/Data/GGYGOActionMotionProfile.h"
#include "Character/Data/GGYGOActionMotionEvaluation.h"
#include "System/GGYGOGameData.h"
#include "System/GGYGOGameplayTags.h"

#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "AIController.h"
#include "Animation/AnimMontage.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameplayEffect.h"
#include "TimerManager.h"
#include "UObject/StrongObjectPtr.h"
#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOBossMeleeAbility)

struct UGGYGOBossMeleeAbility::FOriginalMeleeResources
{
	FOriginalMeleeResources(UGGYGOBossMeleeAbility* InAbility, const FGGYGOAbilityActivationHandle& InOriginal)
		: Original(InOriginal), Ability(InAbility) {}

	const FGGYGOAbilityActivationHandle Original;
	const TWeakObjectPtr<UGGYGOBossMeleeAbility> Ability;
	TWeakObjectPtr<UAbilitySystemComponent> ASC;
	TWeakObjectPtr<AActor> Avatar;
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<UAnimMontage> Montage;
	TWeakObjectPtr<USkeletalMeshComponent> Mesh;
	TWeakObjectPtr<UGGYGOMeleeTraceComponent> Trace;
	TWeakObjectPtr<UGGYGOCharacterMovementComponent> Movement;
	TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> Task;
	FDelegateHandle TaskCallbacks;
	FGGYGOMeleeTraceWindowHandle Window;
	// 原 Task 持有声明引用；本副本只关联自有 Trace 窗口，随 Window 清理，不授予工作资格。
	FGGYGOMontageWindowDeclaration WindowSource;
	FDelegateHandle WindowHits;
	FTimerHandle Watchdog;
	int32 MotionHandle = INDEX_NONE;
	FGGYGOActionMotionSourceBindingPtr MotionSource;
	int32 MontageInstanceId = INDEX_NONE;
	float MotionTranslationScale = 1.0f;
	// Independent original playback and native-physics facts; neither renews GA admission.
	bool bMontageCompleted = false;
	bool bMotionCompleted = false;
	float EffectivePlayRate = 0.0f;
	EVisibilityBasedAnimTickOption SavedMeshTick = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
	bool bSavedUpdateRateOptimizations = false;
	bool bRestoreMesh = false;
	bool bRemovePrerequisite = false;
};

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
	FGGYGOActionMotionSourceBindingPtr Source;
	return ResolveMeleeConfiguration(OutError, Source);
}

bool UGGYGOBossMeleeAbility::ResolveMeleeConfiguration(FString& OutError,
	FGGYGOActionMotionSourceBindingPtr& OutSource) const
{
	OutSource.Reset();
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
	if (bUseMontageSourceMotion)
	{
		if (ActionMotionProfile || !FMath::IsFinite(MotionTranslationScale) || MotionTranslationScale < 0.0f)
		{
			OutError = FString::Printf(TEXT("BossMelee Montage [%s]: 原 Montage XYZ 与 Profile [%s] 互斥，位移缩放须有限非负。"),
				*GetPathNameSafe(AttackMontage), *GetPathNameSafe(ActionMotionProfile));
			return false;
		}
		if (!GGYGOActionMotionSource::BuildSourceBinding(AttackMontage, MotionSlot, MotionSection, OutSource, OutError)) { return false; }
		if (OutSource->SectionCount != 1 || OutSource->MontageStartSeconds != 0.0f
			|| OutSource->MontageEndSeconds != OutSource->MontageLength)
		{
			OutError = FString::Printf(TEXT("BossMelee Montage [%s] Slot [%s] Section [%s]: 本能力要求覆盖完整播放的单 Section。"),
				*GetPathNameSafe(AttackMontage), *MotionSlot.ToString(), *MotionSection.ToString());
			OutSource.Reset();
			return false;
		}
		if (!GGYGOActionMotionEvaluation::ValidateSource(*OutSource, OutError))
		{
			OutSource.Reset();
			return false;
		}
	}
	else if (ActionMotionProfile)
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

void UGGYGOBossMeleeAbility::InitializeAbilityActivation(const FGGYGOAbilityActivationHandle& Original)
{
	if (!Original.HasActivation() || OriginalResources)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("BossMelee [%s] Initialize 拒绝：原身份为空或前一资源批次尚未清理；Montage [%s]。"),
			*GetPathName(), *GetNameSafe(AttackMontage));
		return;
	}
	const TSharedPtr<FOriginalMeleeResources> Resources = MakeShared<FOriginalMeleeResources>(this, Original);
	OriginalResources = Resources;
	Resources->ASC = GetAbilitySystemComponentFromActorInfo();
	Resources->Avatar = GetAvatarActorFromActorInfo();
	if (AActor* Avatar = Resources->Avatar.Get()) { Resources->World = Avatar->GetWorld(); }
	Super::InitializeAbilityActivation(Original);
}

bool UGGYGOBossMeleeAbility::OwnsOriginalResources(
	const TSharedPtr<FOriginalMeleeResources>& Resources, const FGGYGOAbilityActivationHandle& Original) const
{
	return Resources && OriginalResources == Resources && Resources->Original.HasSameActivation(Original)
		&& Resources->Ability.Get() == this && IsValid(this)
		&& !HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed);
}

bool UGGYGOBossMeleeAbility::IsOriginalResourcesCurrent(
	const TSharedPtr<FOriginalMeleeResources>& Resources, const FGGYGOAbilityActivationHandle& Original) const
{
	return OwnsOriginalResources(Resources, Original)
		// Read GA admission only to compare with the already fixed Original. Never replace its source.
		&& CaptureCurrentActivation().HasSameActivation(Original);
}

bool UGGYGOBossMeleeAbility::AreOriginalReceiversCurrent(
	const TSharedPtr<FOriginalMeleeResources>& Resources) const
{
	UAbilitySystemComponent* ASC = Resources->ASC.Get();
	AActor* Avatar = Resources->Avatar.Get();
	UWorld* World = Resources->World.Get();
	return IsValid(ASC) && IsValid(Avatar) && IsValid(World)
		&& GetAbilitySystemComponentFromActorInfo() == ASC && GetAvatarActorFromActorInfo() == Avatar
		&& Avatar->GetWorld() == World
		&& (Resources->Mesh.IsExplicitlyNull() || (Resources->Mesh.IsValid()
			&& Cast<ACharacter>(Avatar) && Cast<ACharacter>(Avatar)->GetMesh() == Resources->Mesh.Get()))
		&& (Resources->Trace.IsExplicitlyNull() || (Resources->Trace.IsValid()
			&& Avatar->FindComponentByClass<UGGYGOMeleeTraceComponent>() == Resources->Trace.Get()))
		&& (Resources->Movement.IsExplicitlyNull() || (Resources->Movement.IsValid()
			&& Cast<ACharacter>(Avatar) && Cast<ACharacter>(Avatar)->GetCharacterMovement() == Resources->Movement.Get()));
}

bool UGGYGOBossMeleeAbility::IsOriginalTaskCurrent(
	const TSharedPtr<FOriginalMeleeResources>& Resources, const FGGYGOAbilityActivationHandle& Original,
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent>& OriginalTask) const
{
	return IsOriginalResourcesCurrent(Resources, Original) && OriginalTask.IsValid()
		&& Resources->Task.HasSameIndexAndSerialNumber(OriginalTask);
}

bool UGGYGOBossMeleeAbility::PrepareOriginalMeleeMesh(
	const FGGYGOAbilityActivationHandle& Original, USkeletalMeshComponent* Mesh)
{
	const TSharedPtr<FOriginalMeleeResources> Resources = OriginalResources;
	if (!IsOriginalResourcesCurrent(Resources, Original) || !AreOriginalReceiversCurrent(Resources)
		|| !IsValid(Mesh) || Resources->bRestoreMesh)
	{
		return false;
	}
	ACharacter* Character = Cast<ACharacter>(Resources->Avatar.Get());
	if (!Character || Character->GetMesh() != Mesh) { return false; }
	Resources->Mesh = Mesh;
	Resources->SavedMeshTick = Mesh->VisibilityBasedAnimTickOption;
	Resources->bSavedUpdateRateOptimizations = Mesh->bEnableUpdateRateOptimizations;
	Resources->bRestoreMesh = true;
	Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	Mesh->bEnableUpdateRateOptimizations = false;
	return true;
}

bool UGGYGOBossMeleeAbility::BeginOriginalMeleeMotion(
	const FGGYGOAbilityActivationHandle& Original, UGGYGOCharacterMovementComponent* Movement,
	const UGGYGOActionMotionProfile* Profile, float PlayRate)
{
	const TSharedPtr<FOriginalMeleeResources> Resources = OriginalResources;
	if (!IsOriginalResourcesCurrent(Resources, Original)) { return false; }
	const ACharacter* Character = Cast<ACharacter>(Resources->Avatar.Get());
	if (!AreOriginalReceiversCurrent(Resources) || !IsValid(Profile) || !IsValid(Movement)
		|| !Character || Character->GetCharacterMovement() != Movement
		|| Resources->MotionHandle != INDEX_NONE)
	{
		FailOriginalAction(Resources, Original, TEXT("Motion"),
			FString::Printf(TEXT("原 MotionProfile [%s]、CMC [%s] 或未持有位移的原批次无效。"),
				*GetNameSafe(Profile), *GetNameSafe(Movement)));
		return false;
	}
	Resources->Movement = Movement;
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> OriginalMovement = Movement;
	const int32 MotionHandle = Movement->BeginActionMotion(Profile, PlayRate);
	if (!IsOriginalResourcesCurrent(Resources, Original))
	{
		if (UGGYGOCharacterMovementComponent* Receiver = OriginalMovement.Get(); Receiver && MotionHandle != INDEX_NONE)
		{
			Receiver->EndActionMotion(MotionHandle);
		}
		return false;
	}
	Resources->MotionHandle = MotionHandle;
	if (MotionHandle == INDEX_NONE)
	{
		FailOriginalAction(Resources, Original, TEXT("Motion"), TEXT("原 CMC 拒绝动作位移。"));
		return false;
	}
	Movement = OriginalMovement.Get();
	if (!Movement || !AreOriginalReceiversCurrent(Resources))
	{
		FailOriginalAction(Resources, Original, TEXT("Motion"), TEXT("位移启动返回后原接收者已失效/改变。"));
		return false;
	}
	const TWeakObjectPtr<UGGYGOBossMeleeAbility> WeakAbility(this);
	const TWeakPtr<FOriginalMeleeResources> WeakResources(Resources);
	FString ObservationError;
	const bool bObserved = Movement->ObserveActionMotionFailure(MotionHandle,
		FGGYGOActionMotionFailureDelegate::CreateLambda(
			[WeakAbility, WeakResources, Original, OriginalMovement](int32 FailedHandle, const FString& Reason)
			{
				UGGYGOBossMeleeAbility* Ability = WeakAbility.Get();
				const TSharedPtr<FOriginalMeleeResources> Batch = WeakResources.Pin();
				// Movement has already retired its slot. Original End eligibility belongs to GAS,
				// independently of HasActiveActionMotion or the old resource-work admission.
				if (Ability && Batch
					&& Batch->Movement.HasSameIndexAndSerialNumber(OriginalMovement)
					&& Batch->MotionHandle == FailedHandle)
				{
					Ability->FailOriginalAction(Batch, Original, TEXT("Motion"), Reason);
				}
			}), ObservationError);
	if (!bObserved)
	{
		FailOriginalAction(Resources, Original, TEXT("Motion 失败观察"), ObservationError);
		return false;
	}
	if (!IsOriginalResourcesCurrent(Resources, Original)) { return false; }
	if (!AreOriginalReceiversCurrent(Resources))
	{
		FailOriginalAction(Resources, Original, TEXT("Motion"), TEXT("位移观察返回后原接收者已失效/改变。"));
		return false;
	}
	return true;
}

bool UGGYGOBossMeleeAbility::BeginOriginalMontageMotion(const TSharedPtr<FOriginalMeleeResources>& Resources)
{
	const FGGYGOAbilityActivationHandle Original = Resources->Original;
	if (!IsOriginalResourcesCurrent(Resources, Original)) { return false; }
	FGGYGOMontageSectionSnapshot Snapshot;
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = Resources->Task.Get();
	if (!AreOriginalReceiversCurrent(Resources) || !Task || !Resources->MotionSource.IsValid()
		|| !Resources->Movement.IsValid() || Resources->MotionHandle != INDEX_NONE
		|| !Task->TryGetOriginalSectionSnapshot(Snapshot)
		|| Snapshot.Montage != Resources->MotionSource->Montage.Get() || Snapshot.MontageInstanceId == INDEX_NONE
		|| Snapshot.SectionName != Resources->MotionSource->SectionName
		|| !FMath::IsFinite(Snapshot.InstancePlayRate) || Snapshot.InstancePlayRate <= 0.0f
		|| Snapshot.InstancePlayRate * Resources->MotionSource->MontageRateScale != Resources->EffectivePlayRate)
	{
		FailOriginalAction(Resources, Original, TEXT("Montage Motion 启动"), TEXT("原 Task 的实例/Section/固定速率快照或原 CMC 不可用。"));
		return false;
	}
	Resources->MontageInstanceId = Snapshot.MontageInstanceId;
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> OriginalMovement = Resources->Movement;
	FString Error;
	const bool bStarted = OriginalMovement->BeginMontageActionMotion(Resources->MotionSource, Snapshot.MontageInstanceId,
		Snapshot.PositionSeconds, Resources->EffectivePlayRate, Resources->MotionTranslationScale, Resources->MotionHandle, Error);
	if (!IsOriginalResourcesCurrent(Resources, Original))
	{
		if (UGGYGOCharacterMovementComponent* Receiver = OriginalMovement.Get(); Receiver && Resources->MotionHandle != INDEX_NONE)
		{
			Receiver->ReleaseMontageActionMotion(Resources->MotionHandle, EGGYGOActionMotionReleaseReason::OwnerInvalidated, Error);
		}
		return false;
	}
	if (!bStarted || !OriginalMovement.IsValid() || !AreOriginalReceiversCurrent(Resources))
	{
		FailOriginalAction(Resources, Original, TEXT("Montage Motion 启动"),
			bStarted ? TEXT("位移启动返回后原接收者已失效/改变。") : Error);
		return false;
	}
	const TWeakObjectPtr<UGGYGOBossMeleeAbility> WeakAbility(this);
	const TWeakPtr<FOriginalMeleeResources> WeakResources(Resources);
	const bool bFailureObserved = OriginalMovement->ObserveMontageActionMotionFailure(Resources->MotionHandle,
		FGGYGOActionMotionFailureDelegate::CreateLambda([WeakAbility, WeakResources, OriginalMovement](int32 Handle, const FString& Diagnostic)
		{
			UGGYGOBossMeleeAbility* Ability = WeakAbility.Get();
			const TSharedPtr<FOriginalMeleeResources> Batch = WeakResources.Pin();
			if (Ability && Batch && Batch->Movement.HasSameIndexAndSerialNumber(OriginalMovement) && Batch->MotionHandle == Handle)
			{
				Ability->FailOriginalAction(Batch, Batch->Original, TEXT("Montage Motion"), Diagnostic);
			}
		}), Error);
	if (!IsOriginalResourcesCurrent(Resources, Original)) { return false; }
	if (!bFailureObserved || !AreOriginalReceiversCurrent(Resources))
	{
		FailOriginalAction(Resources, Original, TEXT("Montage Motion 失败观察"),
			bFailureObserved ? TEXT("观察返回后原接收者已失效/改变。") : Error);
		return false;
	}
	const bool bCompletionObserved = OriginalMovement->ObserveMontageActionMotionCompletion(Resources->MotionHandle,
		FGGYGOActionMotionCompletionDelegate::CreateLambda([WeakAbility, WeakResources, OriginalMovement](int32 Handle)
		{
			UGGYGOBossMeleeAbility* Ability = WeakAbility.Get();
			const TSharedPtr<FOriginalMeleeResources> Batch = WeakResources.Pin();
			if (!Ability || !Batch || !Batch->Movement.HasSameIndexAndSerialNumber(OriginalMovement)
				|| Batch->MotionHandle != Handle || !Ability->IsOriginalResourcesCurrent(Batch, Batch->Original)) { return; }
			Batch->bMotionCompleted = true;
			Ability->TryCompleteOriginalMontageMotion(Batch);
		}), Error);
	if (!IsOriginalResourcesCurrent(Resources, Original)) { return false; }
	if (!bCompletionObserved || !AreOriginalReceiversCurrent(Resources))
	{
		FailOriginalAction(Resources, Original, TEXT("Montage Motion 完成观察"),
			bCompletionObserved ? TEXT("观察返回后原接收者已失效/改变。") : Error);
		return false;
	}
	return true;
}

void UGGYGOBossMeleeAbility::TryCompleteOriginalMontageMotion(const TSharedPtr<FOriginalMeleeResources>& Resources)
{
	if (IsOriginalResourcesCurrent(Resources, Resources->Original) && Resources->bMontageCompleted && Resources->bMotionCompleted)
	{
		if (!AreOriginalReceiversCurrent(Resources))
		{
			FailOriginalAction(Resources, Resources->Original, TEXT("Montage Motion 完成"), TEXT("原播放和位移完成后接收者已失效/改变。"));
			return;
		}
		RequestOriginalTermination(Resources->Original, false, false);
	}
}

void UGGYGOBossMeleeAbility::HandleOriginalTaskFailure(const TSharedPtr<FOriginalMeleeResources>& Resources,
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent>& OriginalTask, const FGGYGOMontageTaskFailureFact& Fact)
{
	// EndTask already marked this sender Garbage. Authenticate its fixed N0 registration,
	// never require live-task admission or look up another instance of the same Montage.
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = OriginalTask.Get(true);
	if (!OwnsOriginalResources(Resources, Resources->Original) || !Task
		|| Task->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		|| !Resources->Task.HasSameIndexAndSerialNumber(OriginalTask) || Fact.Montage != Resources->Montage.Get()) { return; }
	if (Resources->MontageInstanceId != INDEX_NONE && Fact.MontageInstanceId != Resources->MontageInstanceId) { return; }
	FailOriginalAction(Resources, Resources->Original, TEXT("Montage Task"), Fact.Diagnostic);
}

bool UGGYGOBossMeleeAbility::HasOriginalMeleeMeshResource() const
{
	return OriginalResources && OriginalResources->Mesh.IsValid() && OriginalResources->bRestoreMesh;
}

FGGYGOAbilityActivationHandle UGGYGOBossMeleeAbility::GetOriginalMeleeResourceActivation() const
{
	return OriginalResources ? OriginalResources->Original : FGGYGOAbilityActivationHandle();
}

void UGGYGOBossMeleeAbility::RequestOriginalTermination(
	const FGGYGOAbilityActivationHandle& Original, bool bCancel, bool bWasCancelled)
{
	const FString OriginalAbilityName = GetPathName();
	const TSharedPtr<FOriginalMeleeResources> Resources = OriginalResources;
	const FString OriginalMontageName = Resources && Resources->Original.HasSameActivation(Original)
		? GetNameSafe(Resources->Montage.Get()) : TEXT("<原资源批次不可用>");
	const FGGYGOAbilityTerminationResult Result = bCancel
		? RequestAbilityCancel(Original, true)
		: RequestAbilityEnd(Original, true, bWasCancelled);
	switch (Result.Outcome)
	{
	case EGGYGOAbilityTerminationOutcome::Completed:
	case EGGYGOAbilityTerminationOutcome::Accepted:
	case EGGYGOAbilityTerminationOutcome::Deferred:
	case EGGYGOAbilityTerminationOutcome::AlreadyPending:
		break;
	default:
		UE_LOG(LogGGYGOAbilitySystem, Warning,
			TEXT("BossMelee [%s] 原 %s 请求未获准：Outcome=%d Reason=%d；Montage [%s]。"),
			*OriginalAbilityName, bCancel ? TEXT("Cancel") : TEXT("End"), int32(Result.Outcome),
			int32(Result.Reason), *OriginalMontageName);
		break;
	}
	// The returned history never authorizes a replacement request or forced End.
}

void UGGYGOBossMeleeAbility::FailOriginalAction(
	const TSharedPtr<FOriginalMeleeResources>& Resources, const FGGYGOAbilityActivationHandle& Original,
	const TCHAR* Stage, const FString& Reason)
{
	// Failing a held original batch only requests termination; it never renews resource work.
	if (!OwnsOriginalResources(Resources, Original)) { return; }
	UE_LOG(LogGGYGOAbilitySystem, Error,
		TEXT("BossMelee [%s] 原动作在 [%s] 失败：Avatar [%s] ASC [%s] Montage [%s] Trace [%s]；%s"),
		*GetPathName(), Stage, *GetNameSafe(Resources->Avatar.Get()), *GetNameSafe(Resources->ASC.Get()),
		*GetNameSafe(Resources->Montage.Get()), *GetNameSafe(Resources->Trace.Get()), *Reason);
	RequestOriginalTermination(Original, false, true);
}

void UGGYGOBossMeleeAbility::ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
	FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	const TSharedPtr<FOriginalMeleeResources> Resources = OriginalResources;
	// A previously accepted End/Cancel can still be deferred until this native activation returns.
	// The GA-owned query stops business work without inventing a derived ending flag.
	if (Original.HasActivation() && !CaptureCurrentActivation().HasSameActivation(Original)) { return; }
	if (!IsOriginalResourcesCurrent(Resources, Original))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("BossMelee [%s] Body 缺少匹配的原初始化批次；Montage [%s]。必须先共同启用原生身份发行与 Initialize。"),
			*GetPathName(), *GetNameSafe(AttackMontage));
		if (Original.HasActivation()) { RequestOriginalTermination(Original, false, true); }
		return;
	}
	if (!ActorInfo || ActorInfo->AbilitySystemComponent.Get() != Resources->ASC.Get()
		|| ActorInfo->AvatarActor.Get() != Resources->Avatar.Get())
	{
		FailOriginalAction(Resources, Original, TEXT("Body 入口"), TEXT("传入 ActorInfo 不属于初始化的原 ASC/Avatar。"));
		return;
	}
	const auto RecheckAfterExternal = [this, Resources, Original](const TCHAR* Stage)
	{
		if (!IsOriginalResourcesCurrent(Resources, Original)) { return false; }
		if (!AreOriginalReceiversCurrent(Resources))
		{
			FailOriginalAction(Resources, Original, Stage, TEXT("原 ASC、Avatar 或 World 已失效/改变。"));
			return false;
		}
		return true;
	};
	// This is the sole native/BP parent activation call for a valid original body.
	Super::ActivateAbilityBody(Original, Handle, ActorInfo, ActivationInfo, TriggerEventData);
	if (!RecheckAfterExternal(TEXT("BP Super"))) { return; }

	ACharacter* Character = Cast<ACharacter>(Resources->Avatar.Get());
	USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
	UGGYGOMeleeTraceComponent* Trace = Character ? Character->FindComponentByClass<UGGYGOMeleeTraceComponent>() : nullptr;
	Resources->Mesh = Mesh;
	Resources->Trace = Trace;
	Resources->Montage = AttackMontage;
	FString ConfigurationError;
	if (!HasAuthority(&ActivationInfo) || !ValidateMeleeConfiguration(ConfigurationError)
		|| !IsValid(Mesh) || !Mesh->GetAnimInstance() || !IsValid(Trace)
		|| !Mesh->DoesSocketExist(TraceStartSocket) || !Mesh->DoesSocketExist(TraceEndSocket))
	{
		FailOriginalAction(Resources, Original, TEXT("配置"),
			ConfigurationError.IsEmpty() ? TEXT("需要权威执行、有效 Mesh/AnimInstance/Trace 和两个 Socket。") : ConfigurationError);
		return;
	}
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		if (IsOriginalResourcesCurrent(Resources, Original))
		{
			FailOriginalAction(Resources, Original, TEXT("Commit"), TEXT("CommitAbility 返回失败。"));
		}
		return;
	}
	if (!RecheckAfterExternal(TEXT("Commit"))) { return; }
	// BP/Commit can change configuration. Freeze the actual montage and receivers used below.
	Resources->Montage = AttackMontage;
	const bool bRequiresMotion = bUseMontageSourceMotion || ActionMotionProfile != nullptr;
	UGGYGOCharacterMovementComponent* Movement = bRequiresMotion && Character
		? Cast<UGGYGOCharacterMovementComponent>(Character->GetCharacterMovement()) : nullptr;
	Resources->Movement = Movement;
	float TaskPlayRate = 0.0f;
	float EffectivePlayRate = 0.0f;
	if (!ResolveMeleeConfiguration(ConfigurationError, Resources->MotionSource)
		|| !IsValid(Mesh) || !IsValid(Trace)
		|| Character->GetMesh() != Mesh || Character->FindComponentByClass<UGGYGOMeleeTraceComponent>() != Trace
		|| !Mesh->GetAnimInstance() || !Mesh->DoesSocketExist(TraceStartSocket) || !Mesh->DoesSocketExist(TraceEndSocket)
		|| !UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolvePlayRate(
			AttackMontage, MontagePlayRate, TaskPlayRate, EffectivePlayRate)
		|| !FMath::IsFinite(AttackMontage->GetPlayLength() / EffectivePlayRate + 2.0f)
		|| (bRequiresMotion && (!IsValid(Movement) || Movement->HasActiveActionMotion())))
	{
		FailOriginalAction(Resources, Original, TEXT("Commit 后配置"),
			ConfigurationError.IsEmpty() ? TEXT("原 Mesh/Trace、播放速率、超时或动作位移前置条件无效。") : ConfigurationError);
		return;
	}
	const TWeakObjectPtr<const UGGYGOActionMotionProfile> MotionProfile = ActionMotionProfile.Get();
	Resources->MotionTranslationScale = MotionTranslationScale;
	const float MontageLength = AttackMontage->GetPlayLength();

	if (!PrepareOriginalMeleeMesh(Original, Mesh))
	{
		FailOriginalAction(Resources, Original, TEXT("Mesh 准备"), TEXT("原 Mesh 恢复资源不可用或已被本批次申请。"));
		return;
	}
	Resources->bRemovePrerequisite = !Trace->PrimaryComponentTick.GetPrerequisites().ContainsByPredicate(
		[Mesh](const FTickPrerequisite& Prerequisite)
		{
			return Prerequisite.PrerequisiteObject.Get() == Mesh && Prerequisite.Get() == &Mesh->PrimaryComponentTick;
		});
	if (Resources->bRemovePrerequisite) { Trace->AddTickPrerequisiteComponent(Mesh); }
	// Path following belongs to the controller, and resumes through the next BT request.
	if (AAIController* Controller = Cast<AAIController>(Character->GetController())) { Controller->StopMovement(); }
	if (!RecheckAfterExternal(TEXT("StopMovement"))) { return; }

	FGameplayTagContainer EventTags;
	EventTags.AddTag(GGYGOGameplayTags::Event_Montage_HitWindowBegin);
	EventTags.AddTag(GGYGOGameplayTags::Event_Montage_HitWindowEnd);
	TArray<FGGYGOMontageWindowDeclaration> HitWindows;
	FString WindowError;
	if (!UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolveOriginalMontageWindows(
		Resources->Montage.Get(), EventTags, 0.0f, MontageLength, HitWindows, WindowError))
	{
		FailOriginalAction(Resources, Original, TEXT("窗口声明"), WindowError);
		return;
	}
	// Keep this original pre-Ready UObject alive across external setup; the task owns its native execution.
	TStrongObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> Task(
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent::PlayMontageAndWaitForEvent(
			this, TEXT("BossMelee"), Resources->Montage.Get(), EventTags, MontagePlayRate));
	if (!IsOriginalResourcesCurrent(Resources, Original))
	{
		if (IsValid(Task.Get())) { Task->TaskOwnerEnded(); }
		return;
	}
	Resources->Task = Task.Get();
	if (!IsValid(Task.Get()))
	{
		FailOriginalAction(Resources, Original, TEXT("Task"), TEXT("无法创建原 Montage Task。"));
		return;
	}
	if (!RecheckAfterExternal(TEXT("Task 创建"))) { return; }
	if (!Task->ConfigureOriginalMontageWindows(HitWindows, WindowError))
	{
		// 配置失败时原 Task 尚未 Ready，先归还它，再按原 GA 资源归属收尾。
		Task->TaskOwnerEnded();
		FailOriginalAction(Resources, Original, TEXT("窗口配置"), WindowError);
		return;
	}
	Resources->EffectivePlayRate = Task->GetEffectivePlayRate();
	const float WatchdogDuration = MontageLength / Resources->EffectivePlayRate + 2.0f;
	if (!FMath::IsFinite(Resources->EffectivePlayRate) || Resources->EffectivePlayRate <= 0.0f
		|| !FMath::IsFinite(WatchdogDuration) || WatchdogDuration <= 0.0f)
	{
		FailOriginalAction(Resources, Original, TEXT("Task 速率"), TEXT("实际任务速率或 watchdog 时长无效。"));
		return;
	}

	const TWeakPtr<FOriginalMeleeResources> WeakResources = Resources;
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> OriginalTask = Task.Get();
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent::FNativeCallbacks Callbacks;
	const auto BindCallback = [this, WeakResources, Original, OriginalTask](
		FGGYGOPlayMontageAndWaitForEventDelegate& Callback, EMontageCallback Kind)
	{
		Callback.BindWeakLambda(this, [this, WeakResources, Original, OriginalTask, Kind](
			FGameplayTag, FGameplayEventData)
		{
			if (const TSharedPtr<FOriginalMeleeResources> Pinned = WeakResources.Pin())
			{
				HandleOriginalMontageCallback(Pinned, Original, OriginalTask, Kind);
			}
		});
	};
	BindCallback(Callbacks.OnCompleted, EMontageCallback::Completed);
	BindCallback(Callbacks.OnInterrupted, EMontageCallback::Interrupted);
	BindCallback(Callbacks.OnCancelled, EMontageCallback::Interrupted);
	BindCallback(Callbacks.OnBlendOut, EMontageCallback::BlendOut);
	Callbacks.WindowReceived.BindWeakLambda(this, [this, WeakResources, Original, OriginalTask](
		const FGGYGOMontageWindowFact& Fact)
	{
		if (const TSharedPtr<FOriginalMeleeResources> Pinned = WeakResources.Pin())
		{
			HandleOriginalHitWindow(Pinned, Original, OriginalTask, Fact);
		}
	});
	Callbacks.OnFailed.BindWeakLambda(this, [this, WeakResources, OriginalTask](const FGGYGOMontageTaskFailureFact& Fact)
	{
		if (const TSharedPtr<FOriginalMeleeResources> Pinned = WeakResources.Pin())
		{
			HandleOriginalTaskFailure(Pinned, OriginalTask, Fact);
		}
	});
	const FDelegateHandle Registration = Task->RegisterNativeCallbacks(MoveTemp(Callbacks));
	if (!IsOriginalResourcesCurrent(Resources, Original))
	{
		if (Registration.IsValid()) { Task->UnregisterNativeCallbacks(Registration); }
		return;
	}
	Resources->TaskCallbacks = Registration;
	if (!Registration.IsValid())
	{
		FailOriginalAction(Resources, Original, TEXT("N0 注册"), TEXT("原 Task 的 pre-Ready 回调注册失败。"));
		return;
	}
	if (!RecheckAfterExternal(TEXT("N0 注册"))) { return; }

	// Arm before further external setup/Ready; a rejected interruption clears this exact timer.
	UWorld* World = Resources->World.Get();
	if (!IsValid(World))
	{
		FailOriginalAction(Resources, Original, TEXT("watchdog"), TEXT("原 World 已失效。"));
		return;
	}
	FTimerDelegate Timeout;
	Timeout.BindWeakLambda(this, [this, WeakResources, Original, OriginalTask]()
	{
		if (const TSharedPtr<FOriginalMeleeResources> Pinned = WeakResources.Pin())
		{
			HandleOriginalMontageTimeout(Pinned, Original, OriginalTask);
		}
	});
	FTimerHandle Watchdog;
	World->GetTimerManager().SetTimer(Watchdog, MoveTemp(Timeout), WatchdogDuration, false);
	if (!IsOriginalResourcesCurrent(Resources, Original))
	{
		if (UWorld* Receiver = Resources->World.Get()) { Receiver->GetTimerManager().ClearTimer(Watchdog); }
		return;
	}
	Resources->Watchdog = Watchdog;
	if (!RecheckAfterExternal(TEXT("watchdog"))) { return; }
	if (!Watchdog.IsValid())
	{
		FailOriginalAction(Resources, Original, TEXT("watchdog"), TEXT("原 World 无法登记超时资源。"));
		return;
	}

	if (MotionProfile.IsValid())
	{
		if (!BeginOriginalMeleeMotion(Original, Resources->Movement.Get(), MotionProfile.Get(), Resources->EffectivePlayRate)) { return; }
	}
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("BossMelee: [%s] 开始原播放 [%s]。"),
		*GetNameSafe(Resources->Avatar.Get()), *GetNameSafe(Resources->Montage.Get()));
	Task->ReadyForActivation();
	if (!RecheckAfterExternal(TEXT("ReadyForActivation"))) { return; }
	if (Resources->MotionSource.IsValid()) { BeginOriginalMontageMotion(Resources); }
}

void UGGYGOBossMeleeAbility::CloseOriginalTraceWindow(const TSharedPtr<FOriginalMeleeResources>& Resources)
{
	const TWeakObjectPtr<UGGYGOMeleeTraceComponent> OriginalTrace = Resources->Trace;
	const FGGYGOMeleeTraceWindowHandle Window = Resources->Window;
	const FDelegateHandle Subscription = Resources->WindowHits;
	Resources->Window = {};
	Resources->WindowSource = {};
	Resources->WindowHits.Reset();
	if (UGGYGOMeleeTraceComponent* Trace = OriginalTrace.Get())
	{
		if (Subscription.IsValid()) { Trace->UnsubscribeWindowHit(Subscription); }
	}
	if (UGGYGOMeleeTraceComponent* Trace = OriginalTrace.Get(); Trace && Window.HasWindow())
	{
		Trace->CloseOwnedTraceWindow(Window);
	}
}

void UGGYGOBossMeleeAbility::CleanupAbilityResourcesForTermination(const FGGYGOAbilityTerminationContext& Context)
{
	TSharedPtr<FOriginalMeleeResources> Resources = OriginalResources;
	if (Resources && Resources->Original.HasSameActivation(Context.GetOriginalActivation()))
	{
		OriginalResources.Reset(); // Retire the entire original batch before calling any receiver.
		// Restore non-token Mesh/prerequisite resources before callbacks or task termination can reenter.
		USkeletalMeshComponent* Mesh = Resources->Mesh.Get();
		if (Mesh && Resources->bRestoreMesh)
		{
			Resources->bRestoreMesh = false;
			Mesh->VisibilityBasedAnimTickOption = Resources->SavedMeshTick;
			Mesh->bEnableUpdateRateOptimizations = Resources->bSavedUpdateRateOptimizations;
		}
		if (UGGYGOMeleeTraceComponent* Trace = Resources->Trace.Get(); Trace && Mesh && Resources->bRemovePrerequisite)
		{
			Resources->bRemovePrerequisite = false;
			Trace->RemoveTickPrerequisiteComponent(Mesh);
		}
		FTimerHandle Watchdog = Resources->Watchdog;
		Resources->Watchdog.Invalidate();
		if (UWorld* World = Resources->World.Get()) { World->GetTimerManager().ClearTimer(Watchdog); }
		const FDelegateHandle Registration = Resources->TaskCallbacks;
		Resources->TaskCallbacks.Reset();
		if (UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = Resources->Task.Get(); Task && Registration.IsValid())
		{
			Task->UnregisterNativeCallbacks(Registration);
		}
		CloseOriginalTraceWindow(Resources);
		const int32 MotionHandle = Resources->MotionHandle;
		Resources->MotionHandle = INDEX_NONE;
		if (UGGYGOCharacterMovementComponent* Movement = Resources->Movement.Get(); Movement && MotionHandle != INDEX_NONE)
		{
			if (Resources->MotionSource.IsValid())
			{
				const EGGYGOActionMotionReleaseReason Reason = !Context.WasCancelled()
					&& Resources->bMontageCompleted && Resources->bMotionCompleted
					? EGGYGOActionMotionReleaseReason::Completed
					: (Context.WasCancelled() ? EGGYGOActionMotionReleaseReason::Cancelled : EGGYGOActionMotionReleaseReason::OwnerInvalidated);
				FString Error;
				if (!Movement->ReleaseMontageActionMotion(MotionHandle, Reason, Error))
				{
					UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("BossMelee [%s] 原 CMC [%s] MotionHandle=%d 清理失败：%s"),
						*GetPathName(), *GetPathNameSafe(Movement), MotionHandle, *Error);
				}
			}
			else { Movement->EndActionMotion(MotionHandle); }
		}
		if (UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Task = Resources->Task.Get()) { Task->TaskOwnerEnded(); }
	}
	else if (Resources)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("BossMelee [%s] 拒绝资源清理：Context 不属于保存的原批次；Montage [%s]。"),
			*GetPathName(), *GetNameSafe(Resources->Montage.Get()));
	}
	Super::CleanupAbilityResourcesForTermination(Context);
	// No derived member writes after releasing the original receivers or parent cleanup.
}

void UGGYGOBossMeleeAbility::HandleOriginalMontageCallback(
	const TSharedPtr<FOriginalMeleeResources>& Resources, const FGGYGOAbilityActivationHandle& Original,
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent>& OriginalTask,
	EMontageCallback Kind)
{
	// 启动取消可能早于 Task ActorInfo／Guard 建立，仍按本次预 Ready 资源识别回调。
	if (!IsOriginalTaskCurrent(Resources, Original, OriginalTask)) { return; }
	CloseOriginalTraceWindow(Resources);
	if (!IsOriginalTaskCurrent(Resources, Original, OriginalTask)) { return; }
	if (Kind == EMontageCallback::Completed)
	{
		if (Resources->MotionSource.IsValid())
		{
			Resources->bMontageCompleted = true;
			FString Error;
			if (!Resources->Movement.IsValid() || Resources->MotionHandle == INDEX_NONE
				|| !Resources->Movement->ReleaseMontageActionMotion(Resources->MotionHandle, EGGYGOActionMotionReleaseReason::Completed, Error))
			{
				FailOriginalAction(Resources, Original, TEXT("Montage 完成"),
					FString::Printf(TEXT("原 CMC 尾帧请求失败：%s"), *Error));
				return;
			}
			TryCompleteOriginalMontageMotion(Resources);
		}
		else { RequestOriginalTermination(Original, false, false); }
	}
	else if (Kind == EMontageCallback::Interrupted)
	{
		FTimerHandle Watchdog = Resources->Watchdog;
		Resources->Watchdog.Invalidate();
		if (UWorld* World = Resources->World.Get()) { World->GetTimerManager().ClearTimer(Watchdog); }
		if (IsOriginalTaskCurrent(Resources, Original, OriginalTask)) { RequestOriginalTermination(Original, true, true); }
	}
	else if (Kind == EMontageCallback::BlendOut && Resources->MotionSource.IsValid())
	{
		FString Error;
		if (!Resources->Movement.IsValid() || Resources->MotionHandle == INDEX_NONE
			|| !Resources->Movement->NotifyMontageActionNaturalBlendOut(Resources->MotionHandle, Resources->MontageInstanceId, Error))
		{
			FailOriginalAction(Resources, Original, TEXT("Montage 混出"), Error);
		}
	}
}

void UGGYGOBossMeleeAbility::HandleOriginalHitWindow(
	const TSharedPtr<FOriginalMeleeResources>& Resources, const FGGYGOAbilityActivationHandle& Original,
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent>& OriginalTask,
	const FGGYGOMontageWindowFact& Fact)
{
	if (!IsOriginalTaskCurrent(Resources, Original, OriginalTask)) { return; }
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* Sender = OriginalTask.Get();
	if (!Sender || !Sender->IsOriginalMontageWindowFact(Fact)) { return; }
	UGGYGOMeleeTraceComponent* Trace = Resources->Trace.Get();
	UAbilitySystemComponent* ASC = Resources->ASC.Get();
	if (!Trace || !ASC || !ASC->IsOwnerActorAuthoritative())
	{
		FailOriginalAction(Resources, Original, TEXT("认证 Hit 窗口"), TEXT("原 Trace/权威 ASC 不可用。"));
		return;
	}
	if (Fact.Phase == EGGYGOMontageWindowPhase::End)
	{
		if (Fact.Declaration.EndEventTag == GGYGOGameplayTags::Event_Montage_HitWindowEnd
			&& Resources->WindowSource.HasSameWindow(Fact.Declaration))
		{
			CloseOriginalTraceWindow(Resources);
		}
		return;
	}
	if (Fact.Declaration.BeginEventTag != GGYGOGameplayTags::Event_Montage_HitWindowBegin) { return; }
	if (Resources->Window.HasWindow())
	{
		const EGGYGOMeleeTraceWindowQueryResult Query = Trace->QueryOwnedTraceWindow(Resources->Window);
		if (Query == EGGYGOMeleeTraceWindowQueryResult::Active) { return; } // 保留原活动窗口及其去重，不由新 Begin 抢占。
		if (Query != EGGYGOMeleeTraceWindowQueryResult::Inactive)
		{
			FailOriginalAction(Resources, Original, TEXT("窗口查询"), TEXT("保存的原 Owned 窗口身份无效。"));
			return;
		}
		CloseOriginalTraceWindow(Resources);
		if (!IsOriginalTaskCurrent(Resources, Original, OriginalTask)) { return; }
	}
	Trace = Resources->Trace.Get();
	if (!Trace)
	{
		FailOriginalAction(Resources, Original, TEXT("窗口开启"), TEXT("原 Trace 在原窗口释放后已失效。"));
		return;
	}
	const TWeakObjectPtr<UGGYGOMeleeTraceComponent> OriginalTrace = Trace;
	FGGYGOMeleeTraceWindowHandle OpenedWindow;
	const EGGYGOMeleeTraceWindowOpenResult OpenResult = Trace->TryOpenOwnedTraceWindow(
		TraceStartSocket, TraceEndSocket, TraceRadius, FGGYGOMeleeTraceWindowHandle(), OpenedWindow);
	if (!IsOriginalTaskCurrent(Resources, Original, OriginalTask))
	{
		if (UGGYGOMeleeTraceComponent* Receiver = OriginalTrace.Get(); Receiver && OpenedWindow.HasWindow())
		{
			Receiver->CloseOwnedTraceWindow(OpenedWindow);
		}
		return;
	}
	if (OpenResult != EGGYGOMeleeTraceWindowOpenResult::Opened || !OpenedWindow.HasWindow())
	{
		FailOriginalAction(Resources, Original, TEXT("窗口开启"),
			FString::Printf(TEXT("原 Trace [%s] 开窗失败：Result=%d Socket=%s -> %s Radius=%.3f。"),
				*GetNameSafe(OriginalTrace.Get()), int32(OpenResult), *TraceStartSocket.ToString(), *TraceEndSocket.ToString(), TraceRadius));
		return;
	}
	Resources->Window = OpenedWindow;
	Resources->WindowSource = Fact.Declaration;
	Trace = OriginalTrace.Get();
	if (!Trace)
	{
		FailOriginalAction(Resources, Original, TEXT("窗口命中订阅"), TEXT("签发原窗口的 Trace 已失效。"));
		return;
	}
	const TWeakPtr<FOriginalMeleeResources> WeakResources = Resources;
	FGGYGOMeleeTraceWindowHitDelegate HitCallback;
	HitCallback.BindWeakLambda(this, [this, WeakResources, Original, OriginalTrace](
		const FGGYGOMeleeTraceWindowHandle& Window, AActor* HitActor, const FHitResult& HitResult)
	{
		if (const TSharedPtr<FOriginalMeleeResources> Pinned = WeakResources.Pin())
		{
			HandleOriginalMeleeHit(Pinned, Original, OriginalTrace, Window, HitActor, HitResult);
		}
	});
	FDelegateHandle Subscription;
	const EGGYGOMeleeTraceWindowSubscribeResult SubscribeResult =
		Trace->SubscribeWindowHit(OpenedWindow, MoveTemp(HitCallback), Subscription);
	if (!IsOriginalTaskCurrent(Resources, Original, OriginalTask) || Resources->Window != OpenedWindow)
	{
		if (UGGYGOMeleeTraceComponent* Receiver = OriginalTrace.Get())
		{
			if (Subscription.IsValid()) { Receiver->UnsubscribeWindowHit(Subscription); }
		}
		if (UGGYGOMeleeTraceComponent* Receiver = OriginalTrace.Get())
		{
			Receiver->CloseOwnedTraceWindow(OpenedWindow);
		}
		return;
	}
	Resources->WindowHits = Subscription;
	if (SubscribeResult != EGGYGOMeleeTraceWindowSubscribeResult::Subscribed || !Subscription.IsValid())
	{
		CloseOriginalTraceWindow(Resources);
		FailOriginalAction(Resources, Original, TEXT("窗口命中订阅"),
			FString::Printf(TEXT("原 Owned 窗口订阅失败：Result=%d。"), int32(SubscribeResult)));
	}
}

void UGGYGOBossMeleeAbility::HandleOriginalMontageTimeout(
	const TSharedPtr<FOriginalMeleeResources>& Resources, const FGGYGOAbilityActivationHandle& Original,
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent>& OriginalTask)
{
	// Task may already be finished/collected when a completion callback was lost.
	// Its captured weak identity is provenance; the GA query owns permission to cancel.
	if (!IsOriginalResourcesCurrent(Resources, Original)
		|| !Resources->Task.HasSameIndexAndSerialNumber(OriginalTask)) { return; }
	Resources->Watchdog.Invalidate();
	UE_LOG(LogGGYGOAbilitySystem, Warning,
		TEXT("BossMelee [%s] 原 Montage [%s] 完成回调超时，向原 Activation 请求取消。"),
		*GetPathName(), *GetNameSafe(Resources->Montage.Get()));
	CloseOriginalTraceWindow(Resources);
	if (IsOriginalResourcesCurrent(Resources, Original)
		&& Resources->Task.HasSameIndexAndSerialNumber(OriginalTask))
	{
		RequestOriginalTermination(Original, true, true);
	}
}

void UGGYGOBossMeleeAbility::HandleOriginalMeleeHit(
	const TSharedPtr<FOriginalMeleeResources>& Resources, const FGGYGOAbilityActivationHandle& Original,
	const TWeakObjectPtr<UGGYGOMeleeTraceComponent>& OriginalTrace,
	const FGGYGOMeleeTraceWindowHandle& Window, AActor* HitActor, const FHitResult& HitResult)
{
	const TWeakObjectPtr<AActor> OriginalHitActor = HitActor;
	const auto IsOriginalHitCurrent = [this, Resources, Original, OriginalTrace, Window, OriginalHitActor]()
	{
		UGGYGOMeleeTraceComponent* Trace = OriginalTrace.Get();
		UAbilitySystemComponent* ASC = Resources->ASC.Get();
		return IsOriginalResourcesCurrent(Resources, Original)
			&& Resources->Trace.HasSameIndexAndSerialNumber(OriginalTrace) && Resources->Window == Window
			&& Trace && Trace->QueryOwnedTraceWindow(Window) == EGGYGOMeleeTraceWindowQueryResult::Active
			&& OriginalHitActor.IsValid() && ASC && ASC->IsOwnerActorAuthoritative()
			&& AreOriginalReceiversCurrent(Resources);
	};
	if (!IsOriginalHitCurrent() || HitActor == Resources->Avatar.Get()) { return; }
	const TWeakObjectPtr<UAbilitySystemComponent> OriginalTargetASC =
		UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(HitActor);
	if (!IsOriginalHitCurrent() || !OriginalTargetASC.IsValid()) { return; }
	const TSubclassOf<UGameplayEffect> ResolvedDamageEffect = UGGYGOGameData::ResolveDamageGameplayEffect(
		DamageEffect, bUseSharedDamageEffectWhenUnset);
	if (!IsOriginalHitCurrent()) { return; }
	if (!ResolvedDamageEffect)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("BossMelee [%s] 原命中伤害GE不可用：需要DamageEffect覆盖，或显式启用且预载可用的共享GE；拒绝伤害与Cue。"),
			*GetPathName());
		return;
	}
	const float HitDamage = Damage;
	const float HitPoiseDamage = PoiseDamage;
	const FGameplayTag CueTag = HitCueTag;
	const FVector Origin = Resources->Avatar->GetActorLocation();
	FGGYGOHitEffectPayload HitPayload;
	if (!BuildHitEffectPayload(OriginalTargetASC.Get(), ResolvedDamageEffect, GetAbilityLevel(), HitResult, Origin, HitPayload))
	{
		return;
	}
	if (!IsOriginalHitCurrent() || !OriginalTargetASC.IsValid()) { return; }
	if (!HitPayload.EffectSpec.IsValid())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("BossMelee [%s] 原命中 Builder 未返回必需伤害 Spec；GE [%s] Target [%s]，拒绝伤害与Cue。"),
			*GetPathName(), *GetNameSafe(ResolvedDamageEffect.Get()), *GetNameSafe(OriginalHitActor.Get()));
		return;
	}
	HitPayload.EffectSpec.Data->SetSetByCallerMagnitude(GGYGOGameplayTags::SetByCaller_Damage, HitDamage);
	HitPayload.EffectSpec.Data->SetSetByCallerMagnitude(GGYGOGameplayTags::SetByCaller_PoiseDamage, HitPoiseDamage);
	UAbilitySystemComponent* SourceASC = Resources->ASC.Get();
	SourceASC->ApplyGameplayEffectSpecToTarget(*HitPayload.EffectSpec.Data.Get(), OriginalTargetASC.Get());
	if (!IsOriginalHitCurrent() || !OriginalTargetASC.IsValid()) { return; }
	// Immunity/application refusal does not discard a valid collision Cue payload.
	if (CueTag.IsValid()) { OriginalTargetASC->ExecuteGameplayCue(CueTag, HitPayload.CueParameters); }
}
