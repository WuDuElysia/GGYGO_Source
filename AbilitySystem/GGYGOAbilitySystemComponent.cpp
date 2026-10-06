/**
 * @file GGYGOAbilitySystemComponent.cpp
 * @brief 项目 ASC 实现
 */
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Animation/AnimInstance.h"
#include "GameFramework/MovementComponent.h"
#include "GameFramework/PlayerController.h"

#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "AbilitySystem/GGYGOAbilityTagRelationshipMapping.h"
#include "AbilitySystem/Groups/GGYGOAbilityGroupConfig.h"
#include "Animation/Runtime/GGYGOMontageGuardAnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"
#include "GameFramework/Pawn.h"
#include "System/GGYGOGameplayTags.h"
#include "Templates/UnrealTemplate.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOAbilitySystemComponent)

UE_DEFINE_GAMEPLAY_TAG(TAG_GGYGO_Gameplay_AbilityInputBlocked, "Gameplay.AbilityInputBlocked");

namespace
{
	bool IsCommittedAvatarBindingCleanupKind(EGGYGOAvatarBindingKind Kind)
	{
		return Kind == EGGYGOAvatarBindingKind::Clear
			|| Kind == EGGYGOAvatarBindingKind::CancelAbilities
			|| Kind == EGGYGOAvatarBindingKind::RemoveGameplayCues;
	}

	// History may be the never-issued Bootstrap value; compare all fields, never infer issuance.
	bool HasSameAvatarBindingContextValue(const FGGYGOAvatarBindingContext& First,
		const FGGYGOAvatarBindingContext& Second)
	{
		return First.Binding.Serial == Second.Binding.Serial
			&& First.Binding.Issuer.HasSameIndexAndSerialNumber(Second.Binding.Issuer)
			&& First.LastActorInfoWrite.Serial == Second.LastActorInfoWrite.Serial
			&& First.LastActorInfoWrite.Issuer.HasSameIndexAndSerialNumber(Second.LastActorInfoWrite.Issuer);
	}

	/** End/cancel evidence for this stack call, including same-instance reactivation with a zero key. */
	class FScopedMontageAbilityLifetime final
	{
	public:
		FScopedMontageAbilityLifetime(UGameplayAbility* InAbility, FGameplayAbilitySpecHandle InHandle)
			: OriginalAbility(InAbility), OriginalHandle(InHandle)
		{
			if (IsValid(InAbility) && InAbility->IsInstantiated() && InHandle.IsValid())
			{
				EndedHandle = InAbility->OnGameplayAbilityEndedWithData.AddLambda([this](const FAbilityEndedData& Data)
				{
					if (Data.AbilityThatEnded.Get() == OriginalAbility.Get() && Data.AbilitySpecHandle == OriginalHandle)
					{
						bEndedOrCancelled = true;
					}
				});
				CancelledHandle = InAbility->OnGameplayAbilityCancelled.AddLambda([this]()
				{
					bEndedOrCancelled = true;
				});
			}
		}

		~FScopedMontageAbilityLifetime()
		{
			if (UGameplayAbility* Ability = OriginalAbility.Get())
			{
				Ability->OnGameplayAbilityEndedWithData.Remove(EndedHandle);
				Ability->OnGameplayAbilityCancelled.Remove(CancelledHandle);
			}
		}

		FScopedMontageAbilityLifetime(const FScopedMontageAbilityLifetime&) = delete;
		FScopedMontageAbilityLifetime& operator=(const FScopedMontageAbilityLifetime&) = delete;
		FScopedMontageAbilityLifetime(FScopedMontageAbilityLifetime&&) = delete;
		FScopedMontageAbilityLifetime& operator=(FScopedMontageAbilityLifetime&&) = delete;

		bool HasEndedOrCancelled() const { return bEndedOrCancelled; }

	private:
		TWeakObjectPtr<UGameplayAbility> OriginalAbility;
		FGameplayAbilitySpecHandle OriginalHandle;
		FDelegateHandle EndedHandle;
		FDelegateHandle CancelledHandle;
		bool bEndedOrCancelled = false;
	};
}

UGGYGOAbilitySystemComponent::UGGYGOAbilitySystemComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	InputHeldSpecHandles.Reset();

	ActiveAbilitiesByGroup.Reset();
}

UGGYGOAbilitySystemComponent::FScopedNativeAbilityCleanup::FScopedNativeAbilityCleanup(
	UGGYGOAbilitySystemComponent* InASC, ENativeAbilityCleanupSource InSource,
	const FGameplayAbilitySpec* OnlySpec)
	: ASC(InASC), Previous(InASC->NativeAbilityCleanupScope), Source(InSource),
	  RemovedSpec(OnlySpec ? OnlySpec->Handle : FGameplayAbilitySpecHandle()),
	  Allocation(InASC->AbilityActorInfo)
{
	check(IsInGameThread());
	const auto CaptureSpec = [this](const FGameplayAbilitySpec& Spec)
	{
		for (UGameplayAbility* Instance : Spec.GetAbilityInstances())
		{
			UGGYGOGameplayAbility* Ability = Cast<UGGYGOGameplayAbility>(Instance);
			if (!Ability || !Ability->IsActive() || Ability->GetCurrentActorInfo() != Allocation.Get()) { continue; }
			FNativeAbilityCleanupTarget& Target = Targets.AddDefaulted_GetRef();
			Target.Ability = Ability;
			Target.Handle = Spec.Handle;
			Target.ActivationKey = Ability->GetCurrentActivationInfo().GetActivationPredictionKey();
			Target.Activation = Ability->NativeCleanupActivation;
		}
	};
	if (OnlySpec) { CaptureSpec(*OnlySpec); }
	else
	{
		for (const FGameplayAbilitySpec& Spec : InASC->ActivatableAbilities.Items) { CaptureSpec(Spec); }
	}
	InASC->NativeAbilityCleanupScope = this;
}

UGGYGOAbilitySystemComponent::FScopedNativeAbilityCleanup::~FScopedNativeAbilityCleanup()
{
	check(IsInGameThread());
	if (UGGYGOAbilitySystemComponent* OriginalASC = ASC.GetEvenIfUnreachable())
	{
		check(OriginalASC->NativeAbilityCleanupScope == this);
		OriginalASC->NativeAbilityCleanupScope = Previous;
	}
}

bool UGGYGOAbilitySystemComponent::GetNativeAbilityCleanupSource(
	const UGGYGOGameplayAbility* Ability, FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo,
	FGGYGOAbilityActivationHandle& OutActivation, bool bRequireActive) const
{
	OutActivation = {};
	const FScopedNativeAbilityCleanup* Scope = NativeAbilityCleanupScope;
	if (!Scope || !IsValid(Ability) || !Ability->IsInstantiated()
		|| (bRequireActive && !Ability->IsActive()) || !ActorInfo
		|| Scope->Allocation.Get() != ActorInfo || AbilityActorInfo.Get() != ActorInfo
		|| ActorInfo->AbilitySystemComponent.Get() != this
		|| Ability->GetCurrentActorInfo() != ActorInfo
		|| Ability->GetCurrentAbilitySpecHandle() != Handle
		|| Ability->GetCurrentActivationInfo().GetActivationPredictionKey() != ActivationInfo.GetActivationPredictionKey())
	{
		return false;
	}
	const FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(Handle);
	if (!Spec || !Spec->GetAbilityInstances().Contains(const_cast<UGGYGOGameplayAbility*>(Ability))) { return false; }
	for (const FNativeAbilityCleanupTarget& Target : Scope->Targets)
	{
		if (Target.Ability.Get() == Ability && Target.Handle == Handle
			&& Target.ActivationKey == ActivationInfo.GetActivationPredictionKey()
			&& (Target.Activation.HasActivation()
				? Target.Activation.HasSameActivation(Ability->NativeCleanupActivation)
				: !Ability->NativeCleanupActivation.HasActivation()))
		{
			OutActivation = Target.Activation;
			return true;
		}
	}
	return false;
}

bool UGGYGOAbilitySystemComponent::IsSpecUnderNativeAbilityCleanup(FGameplayAbilitySpecHandle Handle) const
{
	const FScopedNativeAbilityCleanup* Scope = NativeAbilityCleanupScope;
	return Scope && (Scope->Source != ENativeAbilityCleanupSource::SpecRemoval || Scope->RemovedSpec == Handle);
}

void UGGYGOAbilitySystemComponent::DestroyActiveState()
{
	FScopedAvatarBindingNativeWrite NativeWrite(this);
	FScopedNativeAbilityCleanup Cleanup(this, ENativeAbilityCleanupSource::DestroyActiveState);
	Super::DestroyActiveState();
}

void UGGYGOAbilitySystemComponent::OnRemoveAbility(FGameplayAbilitySpec& AbilitySpec)
{
	FScopedAvatarBindingNativeWrite NativeWrite(this);
	FScopedNativeAbilityCleanup Cleanup(this, ENativeAbilityCleanupSource::SpecRemoval, &AbilitySpec);
	Super::OnRemoveAbility(AbilitySpec);
}

/** Immutable proof of one completed native Local write; no playback executor or activity state. */
struct FGGYGOAbilityMontagePlaybackHandle::FPlaybackProof final
{
	TWeakObjectPtr<UGGYGOAbilitySystemComponent> Issuer;
	TWeakObjectPtr<UGameplayAbility> Ability;
	FGameplayAbilitySpecHandle SpecHandle;
	FPredictionKey ActivationKey;
	TWeakObjectPtr<UAnimMontage> Montage;
	FGGYGOMontagePlayGuardResult Guard;
	uint8 LocalPlayInstanceId = 0; // Coherence only; the nonreusing Guard call is the identity.
};

void UGGYGOAbilitySystemComponent::RetireMontagePlaybackOwnership()
{
	check(IsInGameThread());
	MontagePlaybackProvenance = {};
}

void UGGYGOAbilitySystemComponent::ReconcileMontagePlaybackOwnership()
{
	check(IsInGameThread());
	if (MontagePlaybackProvenance.Playback.HasPlayback()
		&& CheckMontagePlaybackOwnership(MontagePlaybackProvenance.Playback).Outcome
			!= EGGYGOAbilityMontagePlaybackOutcome::Succeeded)
	{
		RetireMontagePlaybackOwnership();
	}
}

FGGYGOAbilityMontageOwnershipCheck UGGYGOAbilitySystemComponent::CheckMontagePlaybackOwnership(
	const FGGYGOAbilityMontagePlaybackHandle& Original) const
{
	check(IsInGameThread());
	using EOutcome = EGGYGOAbilityMontagePlaybackOutcome;
	using EReason = EGGYGOAbilityMontagePlaybackReason;
	FGGYGOAbilityMontageOwnershipCheck Result;
	Result.Original = Original;
	if (!Original.Proof.IsValid()) { return Result; }
	Result.Guard = Original.Proof->Guard;
	const auto Fail = [&](EOutcome Outcome, EReason Reason)
	{
		Result.Outcome = Outcome;
		Result.Reason = Reason;
		return Result;
	};
	if (!IsValid(this) || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		return Fail(EOutcome::Failed, EReason::InvalidASC);
	}
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> Self(
		const_cast<UGGYGOAbilitySystemComponent*>(this));
	if (!Original.Proof->Issuer.HasSameIndexAndSerialNumber(Self))
	{
		return Fail(EOutcome::Rejected, EReason::WrongIssuer);
	}
	if (!MontagePlaybackProvenance.Playback.HasSamePlayback(Original))
	{
		return Fail(EOutcome::Stale, EReason::PlaybackReplaced);
	}
	FActualAvatarBindingActorInfoSnapshot Actual;
	EGGYGOAvatarBindingReason ActorReason;
	if (!CaptureAvatarBindingActualSnapshot(Actual, ActorReason))
	{
		return Fail(EOutcome::Failed, EReason::InvalidActorInfo);
	}
	if (!HasSameAvatarBindingActualSnapshot(MontagePlaybackProvenance.ActorInfo, Actual))
	{
		return Fail(EOutcome::Stale, EReason::ActorInfoChanged);
	}
	const FGGYGOAbilityMontagePlaybackHandle::FPlaybackProof& Proof = *Original.Proof;
	const UGGYGOGameplayAbility* ProjectAbility = Cast<UGGYGOGameplayAbility>(Proof.Ability.Get());
	FGGYGOAbilityActivationHandle CleanupActivation;
	bool bNativeCleanup = ProjectAbility && GetNativeAbilityCleanupSource(ProjectAbility,
		Proof.SpecHandle, Actual.Allocation.Get(), ProjectAbility->GetCurrentActivationInfo(),
		CleanupActivation, /*bRequireActive=*/false);
	if (!bNativeCleanup && ProjectAbility && ProjectAbility->OriginalTermination.IsValid())
	{
		const auto& Record = ProjectAbility->OriginalTermination;
		bNativeCleanup = Record->bHasNativeCleanupSource && Record->ASC.Get() == this
			&& Record->SpecHandle == Proof.SpecHandle
			&& Record->ActivationInfo.GetActivationPredictionKey() == Proof.ActivationKey
			&& ProjectAbility->CheckOriginalTerminationSource(*Record) == EGGYGOAbilityTerminationReason::None;
	}
	if (!ValidateAvatarBindingActualSnapshotForPurpose(Actual, bNativeCleanup
		? EAvatarBindingSnapshotPurpose::CommittedCleanup : EAvatarBindingSnapshotPurpose::WorkingBinding, ActorReason))
	{
		return Fail(EOutcome::Failed, EReason::InvalidActorInfo);
	}
	const UGGYGOMontageGuardAnimInstance* Guard = Cast<UGGYGOMontageGuardAnimInstance>(
		Proof.Guard.Identity.OriginalAnimInstance.Get());
	if (!Guard || !Guard->IsMontagePlayGuardIdentityCurrent(Proof.Guard.Identity))
	{
		return Fail(EOutcome::Stale, EReason::GuardLifecycleInvalid);
	}
	const UGameplayAbility* Ability = Proof.Ability.Get();
	if (!Ability || !Ability->IsInstantiated())
	{
		return Fail(EOutcome::Failed, EReason::InvalidAbility);
	}
	// Ended/inactive is not loss of this native-write resource. Never require IsActive here.
	if (Ability->GetCurrentActorInfo() != Actual.Allocation.Get()
		|| Ability->GetCurrentAbilitySpecHandle() != Proof.SpecHandle
		|| Ability->GetCurrentActivationInfo().GetActivationPredictionKey() != Proof.ActivationKey)
	{
		return Fail(EOutcome::Stale, EReason::AbilitySourceChanged);
	}
	if (LocalAnimMontageInfo.AnimatingAbility.Get() != Ability
		|| !Proof.Montage.IsValid() || LocalAnimMontageInfo.AnimMontage.Get() != Proof.Montage.Get()
		|| LocalAnimMontageInfo.PlayInstanceId != Proof.LocalPlayInstanceId
		|| Ability->GetCurrentMontage() != Proof.Montage.Get())
	{
		return Fail(EOutcome::Stale, EReason::LocalWriteMismatch);
	}
	return Fail(EOutcome::Succeeded, EReason::None);
}

FGGYGOAbilityMontageOwnershipCheck UGGYGOAbilitySystemComponent::CaptureMontagePlaybackOwnership(
	UGameplayAbility* OriginalAbility, FGameplayAbilitySpecHandle OriginalSpecHandle,
	FGameplayAbilityActivationInfo OriginalActivationInfo) const
{
	check(IsInGameThread());
	using EOutcome = EGGYGOAbilityMontagePlaybackOutcome;
	using EReason = EGGYGOAbilityMontagePlaybackReason;
	FGGYGOAbilityMontageOwnershipCheck Result;
	if (!IsValid(OriginalAbility) || !OriginalAbility->IsInstantiated() || !OriginalSpecHandle.IsValid())
	{
		return Result;
	}
	if (!IsValid(this) || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		Result.Outcome = EOutcome::Failed;
		Result.Reason = EReason::InvalidASC;
		return Result;
	}
	const FGGYGOAbilityMontagePlaybackHandle& Original = MontagePlaybackProvenance.Playback;
	if (!Original.Proof.IsValid() || Original.Proof->Ability.Get() != OriginalAbility)
	{
		Result.Outcome = EOutcome::NoOwnedPlayback;
		Result.Reason = EReason::NoOwnedPlayback;
		return Result;
	}
	Result.Guard = Original.Proof->Guard;
	if (Original.Proof->SpecHandle != OriginalSpecHandle
		|| Original.Proof->ActivationKey != OriginalActivationInfo.GetActivationPredictionKey())
	{
		Result.Outcome = EOutcome::Stale;
		Result.Reason = EReason::AbilitySourceChanged;
		return Result;
	}
	Result = CheckMontagePlaybackOwnership(Original);
	// A failed capture returns history, never another ability's/current playback resource.
	if (Result.Outcome != EOutcome::Succeeded) { Result.Original = {}; }
	return Result;
}

FGGYGOAbilityMontageClearResult UGGYGOAbilitySystemComponent::TryClearMontageAnimatingAbility(
	const FGGYGOAbilityMontagePlaybackHandle& Original)
{
	check(IsInGameThread());
	const FGGYGOAbilityMontageOwnershipCheck Checked = CheckMontagePlaybackOwnership(Original);
	FGGYGOAbilityMontageClearResult Result;
	Result.Outcome = Checked.Outcome;
	Result.Reason = Checked.Reason;
	Result.Guard = Checked.Guard;
	Result.Original = Original;
	if (Checked.Outcome != EGGYGOAbilityMontagePlaybackOutcome::Succeeded) { return Result; }
	UGameplayAbility* Ability = Original.Proof->Ability.Get();
	// The base setter is the native field assignment. Do not enter a virtual business hook
	// and then blindly clear a successor's Local pointer on that hook's return.
	Ability->UGameplayAbility::SetCurrentMontage(nullptr);
	LocalAnimMontageInfo.AnimatingAbility = nullptr;
	if (!LocalAnimMontageInfo.AnimatingAbility.IsExplicitlyNull() || Ability->GetCurrentMontage() != nullptr)
	{
		Result.Outcome = EGGYGOAbilityMontagePlaybackOutcome::Failed;
		Result.Reason = EGGYGOAbilityMontagePlaybackReason::LocalWriteMismatch;
		ReconcileMontagePlaybackOwnership();
		return Result;
	}
	RetireMontagePlaybackOwnership();
	Result.bCleared = true;
	return Result;
}

void UGGYGOAbilitySystemComponent::ClearAnimatingAbility(UGameplayAbility* Ability)
{
	check(IsInGameThread());
	if (!Ability)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[ASC Montage] ClearAnimatingAbility rejected null Ability: ASC=%s."), *GetPathName());
		return;
	}
	// Legacy/native callers keep native semantics; this cannot grant an exact resource.
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalASC(this);
	Super::ClearAnimatingAbility(Ability);
	if (OriginalASC.IsValid()) { ReconcileMontagePlaybackOwnership(); }
}

float UGGYGOAbilitySystemComponent::PlayMontageSimulated(UAnimMontage* Montage, float PlayRate,
	FName StartSectionName)
{
	check(IsInGameThread());
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalASC(this);
	const float Duration = Super::PlayMontageSimulated(Montage, PlayRate, StartSectionName);
	if (OriginalASC.IsValid())
	{
		// Native simulated play writes Local.AnimMontage last, even for the same asset.
		if (Duration > 0.0f) { RetireMontagePlaybackOwnership(); }
		else { ReconcileMontagePlaybackOwnership(); }
	}
	return Duration;
}

UAnimMontage* UGGYGOAbilitySystemComponent::PlaySlotAnimationAsDynamicMontage_WithFractionalLoops(
	UGameplayAbility* Ability, FGameplayAbilityActivationInfo ActivationInfo, UAnimSequenceBase* AnimAsset,
	FName SlotName, float BlendInTime, float BlendOutTime, float PlayRate, float StartTimeSeconds, float PlayCount)
{
	check(IsInGameThread());
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalASC(this);
	UAnimMontage* Montage = Super::PlaySlotAnimationAsDynamicMontage_WithFractionalLoops(Ability,
		ActivationInfo, AnimAsset, SlotName, BlendInTime, BlendOutTime, PlayRate, StartTimeSeconds, PlayCount);
	if (OriginalASC.IsValid()) { ReconcileMontagePlaybackOwnership(); }
	return Montage;
}

void UGGYGOAbilitySystemComponent::OnRep_ReplicatedAnimMontage()
{
	check(IsInGameThread());
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalASC(this);
	Super::OnRep_ReplicatedAnimMontage();
	// Native early exits/no write preserve the source; actual Local counter writes do not.
	if (OriginalASC.IsValid()) { ReconcileMontagePlaybackOwnership(); }
}

void UGGYGOAbilitySystemComponent::OnUnregister()
{
	check(IsInGameThread());
	RetireMontagePlaybackOwnership();
	Super::OnUnregister();
}

FGGYGOAbilityMontagePlaybackResult UGGYGOAbilitySystemComponent::TryPlayMontageWithOwnership(
	UGameplayAbility* Ability, FGameplayAbilityActivationInfo ActivationInfo, UAnimMontage* Montage,
	float PlayRate, FName StartSection, float StartTimeSeconds, TFunction<bool()> IsOriginalCallerCurrent)
{
	check(IsInGameThread());
	using EOutcome = EGGYGOAbilityMontagePlaybackOutcome;
	using EReason = EGGYGOAbilityMontagePlaybackReason;
	FGGYGOAbilityMontagePlaybackResult Result;
	if (!IsOriginalCallerCurrent)
	{
		Result.Reason = EReason::MissingCallerQuery;
		return Result;
	}
	if (!IsValid(this) || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		Result.Outcome = EOutcome::Failed;
		Result.Reason = EReason::InvalidASC;
		return Result;
	}
	if (!IsValid(Ability) || !Ability->IsInstantiated() || !IsValid(Montage)
		|| !FMath::IsFinite(PlayRate) || PlayRate <= 0.0f
		|| !FMath::IsFinite(StartTimeSeconds) || StartTimeSeconds < 0.0f)
	{
		return Result;
	}
	if (IsAvatarBindingNativeWriteBusy() || AvatarSwitchAbilityExitScope)
	{
		Result.Outcome = EOutcome::Busy;
		Result.Reason = EReason::NativeWriteBusy;
		return Result;
	}
	EReason OwnershipReason = EReason::LocalWriteMismatch;
	ExecuteMontagePlayWithGuard(Ability, ActivationInfo, Montage, PlayRate, StartSection, StartTimeSeconds,
		Result.Guard, MoveTemp(IsOriginalCallerCurrent), &Result.Playback, &OwnershipReason);
	Result.Duration = Result.Guard.CallerReturnValue;
	switch (Result.Guard.Outcome)
	{
	case EGGYGOMontagePlayGuardOutcome::Accepted:
		Result.Outcome = Result.Playback.HasPlayback() ? EOutcome::Succeeded : EOutcome::Failed;
		Result.Reason = Result.Playback.HasPlayback() ? EReason::None : OwnershipReason;
		break;
	case EGGYGOMontagePlayGuardOutcome::Superseded:
		Result.Outcome = EOutcome::Stale;
		Result.Reason = EReason::GuardSuperseded;
		break;
	case EGGYGOMontagePlayGuardOutcome::LifecycleInvalid:
		Result.Outcome = EOutcome::Stale;
		Result.Reason = EReason::GuardLifecycleInvalid;
		break;
	case EGGYGOMontagePlayGuardOutcome::Unsupported:
		Result.Reason = EReason::GuardUnsupported;
		break;
	case EGGYGOMontagePlayGuardOutcome::Failed:
		Result.Outcome = EOutcome::Failed;
		Result.Reason = EReason::NativePlayFailed;
		break;
	default:
		Result.Reason = EReason::GuardRejected;
		break;
	}
	return Result;
}

void UGGYGOAbilitySystemComponent::InitAbilityActorInfo(AActor* InOwnerActor, AActor* InAvatarActor)
{
	check(IsInGameThread());
	if (IsAvatarBindingNativeWriteBusy() || AvatarSwitchAbilityExitScope)
	{
		LogLegacyAvatarActorInfoWriteRejected(TEXT("InitAbilityActorInfo"), EGGYGOAvatarBindingReason::NativeWriteBusy);
		return;
	}
	FGameplayAbilityActorInfo* ActorInfo = AbilityActorInfo.Get();
	if (!ActorInfo || !InOwnerActor)
	{
		LogLegacyAvatarActorInfoWriteRejected(TEXT("InitAbilityActorInfo"), ActorInfo
			? EGGYGOAvatarBindingReason::InvalidOwner : EGGYGOAvatarBindingReason::InvalidActorInfo);
		return;
	}
	EGGYGOAvatarBindingReason LifecycleReason;
	if (!IsAvatarBindingNewWorkLifecycleOpen(LifecycleReason))
	{
		LogLegacyAvatarActorInfoWriteRejected(TEXT("InitAbilityActorInfo"), LifecycleReason);
		return;
	}
	if (!IsValid(InOwnerActor) || InOwnerActor->IsActorBeingDestroyed())
	{
		LogLegacyAvatarActorInfoWriteRejected(TEXT("InitAbilityActorInfo"), IsValid(InOwnerActor)
			? EGGYGOAvatarBindingReason::LifecycleClosed : EGGYGOAvatarBindingReason::InvalidOwner);
		return;
	}
	if (InAvatarActor && (!IsValid(InAvatarActor) || InAvatarActor->IsActorBeingDestroyed()))
	{
		LogLegacyAvatarActorInfoWriteRejected(TEXT("InitAbilityActorInfo"), IsValid(InAvatarActor)
			? EGGYGOAvatarBindingReason::LifecycleClosed : EGGYGOAvatarBindingReason::InvalidAvatar);
		return;
	}
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalASC(this);
	const bool bHasNewPawnAvatar = Cast<APawn>(InAvatarActor) && (InAvatarActor != ActorInfo->AvatarActor);
	{
		FScopedAvatarBindingNativeWrite NativeWrite(this);
		if (!NativeWrite.HasEntered())
		{
			LogLegacyAvatarActorInfoWriteRejected(TEXT("InitAbilityActorInfo"), EGGYGOAvatarBindingReason::NativeWriteBusy);
			return;
		}
		InvalidateAvatarBindingForLegacyActorInfoWrite();
		if (InAvatarActor != ActorInfo->AvatarActor)
		{
			ClearAbilityInput();
		}
		RetireMontagePlaybackOwnership(); // Actual native Init resets Local, including same endpoints.
		Super::InitAbilityActorInfo(InOwnerActor, InAvatarActor);
	}
	if (!OriginalASC.IsValid())
	{
		return;
	}

	if (bHasNewPawnAvatar)
	{
		// 通知已有能力实例重新绑定 Avatar 相关引用。
		for (const FGameplayAbilitySpec& AbilitySpec : ActivatableAbilities.Items)
		{
			// 回放等场景下可能没有实例，空数组是正常情况。
			TArray<UGameplayAbility*> Instances = AbilitySpec.GetAbilityInstances();
			for (UGameplayAbility* AbilityInstance : Instances)
			{
				if (UGGYGOGameplayAbility* GGYGOAbilityInstance = Cast<UGGYGOGameplayAbility>(AbilityInstance))
				{
					GGYGOAbilityInstance->OnPawnAvatarSet();
				}
			}
		}

		// Lyra 在这里还会注册 GlobalAbilitySystem（队伍范围的 Buff 广播）
		// 并把 AnimInstance 与 ASC 关联，让动画层能直接查询 Tag。
		// 两者在本项目都尚未实现，等它们就位后在此处补上注册。

		// 放在最后：确保 ActorInfo 已完整、能力实例已收到 Avatar 通知。
		TryActivateAbilitiesOnSpawn();
	}
}


void UGGYGOAbilitySystemComponent::ClearActorInfo()
{
	check(IsInGameThread());
	if (IsAvatarBindingNativeWriteBusy() || AvatarSwitchAbilityExitScope)
	{
		LogLegacyAvatarActorInfoWriteRejected(TEXT("ClearActorInfo"), EGGYGOAvatarBindingReason::NativeWriteBusy);
		return;
	}
	if (!AbilityActorInfo.IsValid())
	{
		LogLegacyAvatarActorInfoWriteRejected(TEXT("ClearActorInfo"), EGGYGOAvatarBindingReason::InvalidActorInfo);
		return;
	}
	FScopedAvatarBindingNativeWrite NativeWrite(this);
	if (!NativeWrite.HasEntered())
	{
		LogLegacyAvatarActorInfoWriteRejected(TEXT("ClearActorInfo"), EGGYGOAvatarBindingReason::NativeWriteBusy);
		return;
	}
	InvalidateAvatarBindingForLegacyActorInfoWrite();
	ClearAbilityInput();
	RetireMontagePlaybackOwnership();
	Super::ClearActorInfo();
}

void UGGYGOAbilitySystemComponent::RefreshAbilityActorInfo()
{
	check(IsInGameThread());
	if (IsAvatarBindingNativeWriteBusy() || AvatarSwitchAbilityExitScope)
	{
		LogLegacyAvatarActorInfoWriteRejected(TEXT("RefreshAbilityActorInfo"), EGGYGOAvatarBindingReason::NativeWriteBusy);
		return;
	}
	if (!AbilityActorInfo.IsValid())
	{
		LogLegacyAvatarActorInfoWriteRejected(TEXT("RefreshAbilityActorInfo"), EGGYGOAvatarBindingReason::InvalidActorInfo);
		return;
	}
	EGGYGOAvatarBindingReason LifecycleReason;
	if (!IsAvatarBindingNewWorkLifecycleOpen(LifecycleReason))
	{
		LogLegacyAvatarActorInfoWriteRejected(TEXT("RefreshAbilityActorInfo"), LifecycleReason);
		return;
	}
	const AActor* Owner = AbilityActorInfo->OwnerActor.Get();
	const AActor* Avatar = AbilityActorInfo->AvatarActor.Get();
	if (!Owner || Owner->IsActorBeingDestroyed()
		|| (!AbilityActorInfo->AvatarActor.IsExplicitlyNull() && (!Avatar || Avatar->IsActorBeingDestroyed())))
	{
		LogLegacyAvatarActorInfoWriteRejected(TEXT("RefreshAbilityActorInfo"), !Owner
			? EGGYGOAvatarBindingReason::InvalidOwner : (!Avatar && !AbilityActorInfo->AvatarActor.IsExplicitlyNull())
			? EGGYGOAvatarBindingReason::InvalidAvatar : EGGYGOAvatarBindingReason::LifecycleClosed);
		return;
	}
	FScopedAvatarBindingNativeWrite NativeWrite(this);
	if (!NativeWrite.HasEntered())
	{
		LogLegacyAvatarActorInfoWriteRejected(TEXT("RefreshAbilityActorInfo"), EGGYGOAvatarBindingReason::NativeWriteBusy);
		return;
	}
	InvalidateAvatarBindingForLegacyActorInfoWrite();
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalASC(this);
	ReconcileMontagePlaybackOwnership();
	Super::RefreshAbilityActorInfo();
	if (OriginalASC.IsValid()) { ReconcileMontagePlaybackOwnership(); }
}

float UGGYGOAbilitySystemComponent::PlayMontage(UGameplayAbility* InAnimatingAbility,
	FGameplayAbilityActivationInfo ActivationInfo, UAnimMontage* NewAnimMontage, float InPlayRate,
	FName StartSectionName, float StartTimeSeconds)
{
	check(IsInGameThread());
	UAnimInstance* OriginalAnimInstance = AbilityActorInfo.IsValid() ? AbilityActorInfo->GetAnimInstance() : nullptr;
	if (!Cast<UGGYGOMontageGuardAnimInstance>(OriginalAnimInstance))
	{
		// Legacy compatibility only. Neither top-level nor nested non-Guard playback is protected.
		const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalASC(this);
		const float Duration = Super::PlayMontage(InAnimatingAbility, ActivationInfo, NewAnimMontage,
			InPlayRate, StartSectionName, StartTimeSeconds);
		if (OriginalASC.IsValid()) { ReconcileMontagePlaybackOwnership(); }
		return Duration;
	}
	FGGYGOMontagePlayGuardResult Result;
	return PlayMontageWithGuard(InAnimatingAbility, ActivationInfo, NewAnimMontage,
		InPlayRate, StartSectionName, StartTimeSeconds, Result, nullptr);
}

float UGGYGOAbilitySystemComponent::PlayMontageWithGuard(UGameplayAbility* InAnimatingAbility,
	FGameplayAbilityActivationInfo ActivationInfo, UAnimMontage* NewAnimMontage, float InPlayRate,
	FName StartSectionName, float StartTimeSeconds, FGGYGOMontagePlayGuardResult& OutResult,
	TFunction<bool()> IsAdditionalCallerContextCurrent)
{
	return ExecuteMontagePlayWithGuard(InAnimatingAbility, ActivationInfo, NewAnimMontage,
		InPlayRate, StartSectionName, StartTimeSeconds, OutResult,
		MoveTemp(IsAdditionalCallerContextCurrent), nullptr, nullptr);
}

float UGGYGOAbilitySystemComponent::ExecuteMontagePlayWithGuard(UGameplayAbility* InAnimatingAbility,
	FGameplayAbilityActivationInfo ActivationInfo, UAnimMontage* NewAnimMontage, float InPlayRate,
	FName StartSectionName, float StartTimeSeconds, FGGYGOMontagePlayGuardResult& OutResult,
	TFunction<bool()> IsAdditionalCallerContextCurrent, FGGYGOAbilityMontagePlaybackHandle* OutPlayback,
	EGGYGOAbilityMontagePlaybackReason* OutOwnershipReason)
{
	check(IsInGameThread());
	if (OutPlayback) { *OutPlayback = {}; }
	if (OutOwnershipReason) { *OutOwnershipReason = EGGYGOAbilityMontagePlaybackReason::LocalWriteMismatch; }
	FActualAvatarBindingActorInfoSnapshot OriginalSource;
	EGGYGOAvatarBindingReason SnapshotReason;
	const bool bCapturedOriginalSource = CaptureAvatarBindingActualSnapshot(OriginalSource, SnapshotReason);
	// Retain the original ActorInfo allocation, but compare its mutable ownership fields to copies.
	const TSharedPtr<FGameplayAbilityActorInfo> OriginalActorInfo = AbilityActorInfo;
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalASC(this);
	const TWeakObjectPtr<UGameplayAbility> OriginalAbility(InAnimatingAbility);
	const TWeakObjectPtr<AActor> OriginalOwner = OriginalActorInfo.IsValid() ? OriginalActorInfo->OwnerActor.Get() : nullptr;
	const TWeakObjectPtr<AActor> OriginalAvatar = OriginalActorInfo.IsValid() ? OriginalActorInfo->AvatarActor.Get() : nullptr;
	const TWeakObjectPtr<USkeletalMeshComponent> OriginalMesh = OriginalActorInfo.IsValid()
		? OriginalActorInfo->SkeletalMeshComponent.Get() : nullptr;
	const TWeakObjectPtr<UAnimInstance> OriginalAnim = OriginalActorInfo.IsValid()
		? OriginalActorInfo->GetAnimInstance() : nullptr;
	const FGameplayAbilitySpecHandle OriginalHandle = IsValid(InAnimatingAbility) && InAnimatingAbility->IsInstantiated()
		? InAnimatingAbility->GetCurrentAbilitySpecHandle() : FGameplayAbilitySpecHandle();
	const FPredictionKey OriginalActivationKey = ActivationInfo.GetActivationPredictionKey();

	// Declared before the guard: borrowed query references and delegate observations outlive it.
	FScopedMontageAbilityLifetime AbilityLifetime(InAnimatingAbility, OriginalHandle);
	FGGYGOMontagePlayGuardRequest Request;
	Request.OriginalAnimInstance = OriginalAnim;
	Request.RequestedMontage = NewAnimMontage;
	Request.CallerIdentity = OriginalASC;
	Request.IsCallerContextCurrent = [OriginalASC, OriginalActorInfo, OriginalOwner, OriginalAvatar,
		OriginalMesh, OriginalAnim, OriginalAbility, OriginalHandle, OriginalActivationKey, &AbilityLifetime,
		AdditionalQuery = MoveTemp(IsAdditionalCallerContextCurrent)]()
	{
		// Invoke the optional pure query before taking a transient Spec pointer.
		if (AbilityLifetime.HasEndedOrCancelled() || (AdditionalQuery && !AdditionalQuery()))
		{
			return false;
		}
		const UGGYGOAbilitySystemComponent* ASC = OriginalASC.Get();
		const UGameplayAbility* Ability = OriginalAbility.Get();
		const USkeletalMeshComponent* Mesh = OriginalMesh.Get();
		const UAnimInstance* Anim = OriginalAnim.Get();
		const AActor* Owner = OriginalOwner.Get();
		const AActor* Avatar = OriginalAvatar.Get();
		const FGameplayAbilityActorInfo* CurrentInfo = ASC && ASC->AbilityActorInfo.IsValid()
			? ASC->AbilityActorInfo.Get() : nullptr;
		if (!OriginalActorInfo.IsValid() || CurrentInfo != OriginalActorInfo.Get()
			|| !Owner || !Avatar || !Mesh || !Anim || !Ability || !Ability->IsInstantiated()
			|| !Ability->IsActive() || !OriginalHandle.IsValid()
			|| CurrentInfo->AbilitySystemComponent.Get() != ASC
			|| CurrentInfo->OwnerActor.Get() != Owner || CurrentInfo->AvatarActor.Get() != Avatar
			|| CurrentInfo->SkeletalMeshComponent.Get() != Mesh || CurrentInfo->GetAnimInstance() != Anim
			|| Mesh->GetOwner() != Avatar || Anim->GetOwningActor() != Avatar || Anim->GetSkelMeshComponent() != Mesh
			|| Ability->GetCurrentActorInfo() != CurrentInfo || Ability->GetCurrentAbilitySpecHandle() != OriginalHandle
			|| Ability->GetCurrentActivationInfo().GetActivationPredictionKey() != OriginalActivationKey)
		{
			return false;
		}
		// This is a call-local read only; no Spec or montage-instance pointer survives an external call.
		const FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromHandle(OriginalHandle);
		return Spec && Spec->Ability && !Spec->PendingRemove && Spec->IsActive()
			&& Spec->GetAbilityInstances().Contains(Ability);
	};

	{
		FGGYGOMontagePlayGuardScope Scope(MoveTemp(Request));
		float CallerReturn = 0.0f;
		if (Scope.CanExecute())
		{
			// Keep the scope live through ALL GAS Local/GA/section/replication/prediction writeback.
			CallerReturn = Super::PlayMontage(InAnimatingAbility, ActivationInfo, NewAnimMontage,
				InPlayRate, StartSectionName, StartTimeSeconds);
		}
		Scope.Complete(CallerReturn);
		OutResult = Scope.GetResult();
	}
	// Publication is after the complete native write and the original Guard scope's return.
	if (OriginalASC.IsValid())
	{
		ReconcileMontagePlaybackOwnership();
		FActualAvatarBindingActorInfoSnapshot Actual;
		UGGYGOMontageGuardAnimInstance* Guard = Cast<UGGYGOMontageGuardAnimInstance>(OriginalAnim.Get());
		if (OutResult.Outcome == EGGYGOMontagePlayGuardOutcome::Accepted)
		{
			using EReason = EGGYGOAbilityMontagePlaybackReason;
			EReason ProofReason = EReason::None;
			if (!bCapturedOriginalSource || !CaptureAvatarBindingActualSnapshot(Actual, SnapshotReason)
				|| !ValidateAvatarBindingActualSnapshot(Actual, SnapshotReason))
			{
				ProofReason = EReason::InvalidActorInfo;
			}
			else if (!HasSameAvatarBindingActualSnapshot(OriginalSource, Actual))
			{
				ProofReason = EReason::ActorInfoChanged;
			}
			else if (!Guard || !Guard->IsMontagePlayGuardIdentityCurrent(OutResult.Identity)
				|| OutResult.Identity.CreatedInstanceId == INDEX_NONE)
			{
				ProofReason = EReason::GuardLifecycleInvalid;
			}
			else if (!OriginalAbility.IsValid() || OriginalAbility->GetCurrentActorInfo() != Actual.Allocation.Get()
				|| OriginalAbility->GetCurrentAbilitySpecHandle() != OriginalHandle
				|| OriginalAbility->GetCurrentActivationInfo().GetActivationPredictionKey() != OriginalActivationKey)
			{
				ProofReason = EReason::AbilitySourceChanged;
			}
			else if (LocalAnimMontageInfo.AnimatingAbility.Get() != OriginalAbility.Get()
				|| LocalAnimMontageInfo.AnimMontage.Get() != NewAnimMontage
				|| OriginalAbility->GetCurrentMontage() != NewAnimMontage)
			{
				ProofReason = EReason::LocalWriteMismatch;
			}
			if (ProofReason == EReason::None)
			{
				const TSharedRef<FGGYGOAbilityMontagePlaybackHandle::FPlaybackProof> Proof =
					MakeShared<FGGYGOAbilityMontagePlaybackHandle::FPlaybackProof>();
				Proof->Issuer = OriginalASC;
				Proof->Ability = OriginalAbility;
				Proof->SpecHandle = OriginalHandle;
				Proof->ActivationKey = OriginalActivationKey;
				Proof->Montage = NewAnimMontage;
				Proof->Guard = OutResult;
				Proof->LocalPlayInstanceId = LocalAnimMontageInfo.PlayInstanceId;
				FGGYGOAbilityMontagePlaybackHandle Playback;
				Playback.Proof = Proof;
				MontagePlaybackProvenance = {Playback, MoveTemp(Actual)};
				if (OutPlayback) { *OutPlayback = Playback; }
			}
			if (OutOwnershipReason) { *OutOwnershipReason = ProofReason; }
		}
	}
	else if (OutOwnershipReason) { *OutOwnershipReason = EGGYGOAbilityMontagePlaybackReason::InvalidASC; }
	// A1 owns outcome precedence. An exact proof failure never recasts the legacy Guard result.
	return OutResult.Outcome == EGGYGOMontagePlayGuardOutcome::Accepted ? OutResult.CallerReturnValue : 0.0f;
}

void UGGYGOAbilitySystemComponent::TryActivateAbilitiesOnSpawn()
{
	// 锁住列表：激活过程可能授予或移除能力，会让遍历中的容器失效。
	ABILITYLIST_SCOPE_LOCK();
	for (const FGameplayAbilitySpec& AbilitySpec : ActivatableAbilities.Items)
	{
		// 用 CDO 读配置，它只提供策略，不是运行时实例。
		if (const UGGYGOGameplayAbility* AbilityCDO = Cast<UGGYGOGameplayAbility>(AbilitySpec.Ability))
		{
			AbilityCDO->TryActivateAbilityOnSpawn(AbilityActorInfo.Get(), AbilitySpec);
		}
	}
}

bool UGGYGOAbilitySystemComponent::CanActivateAbilityByHandle(
	FGameplayAbilitySpecHandle Handle, FGameplayTagContainer& OutFailureTags) const
{
	OutFailureTags.Reset();
	FScopedAbilityActivationQuery QueryScope(this);
	const FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(Handle);
	return Spec && Spec->Ability && AbilityActorInfo.IsValid() &&
		Spec->Ability->CanActivateAbility(
			Handle, AbilityActorInfo.Get(), nullptr, nullptr, &OutFailureTags);
}

UGGYGOAbilitySystemComponent::FScopedControlledAbilityActivationCall::FScopedControlledAbilityActivationCall(
	UGGYGOAbilitySystemComponent* InASC, FControlledAbilityActivationCall& InCall)
	: ASC(InASC), Call(&InCall)
{
	check(IsInGameThread());
	InCall.Previous = InASC->ControlledAbilityActivationCall;
	InASC->ControlledAbilityActivationCall = &InCall;
}

UGGYGOAbilitySystemComponent::FScopedControlledAbilityActivationCall::~FScopedControlledAbilityActivationCall()
{
	if (UGGYGOAbilitySystemComponent* OriginalASC = ASC.Get())
	{
		check(OriginalASC->ControlledAbilityActivationCall == Call);
		OriginalASC->ControlledAbilityActivationCall = Call->Previous;
	}
	// Native Try has returned and its stack link is gone before any completion dispatch.
	TArray<TSharedPtr<UGGYGOGameplayAbility::FOriginalTerminationRecord>> Dependencies =
		MoveTemp(Call->TerminationExitDependencies);
	for (const auto& Record : Dependencies)
	{
		check(Record->OpenTryCalls > 0);
		--Record->OpenTryCalls;
		if (!ASC.IsValid())
		{
			UGGYGOGameplayAbility::FailOriginalTermination(Record, EGGYGOAbilityTerminationReason::InvalidASC);
		}
		UGGYGOGameplayAbility::TryCompleteOriginalTermination(Record);
	}
}

void UGGYGOAbilitySystemComponent::RegisterOriginalTerminationTryDependencies(
	const TSharedPtr<UGGYGOGameplayAbility::FOriginalTerminationRecord>& Record)
{
	check(IsInGameThread());
	check(Record.IsValid() && Record->ASC.Get() == this);
	for (FControlledAbilityActivationCall* Call = ControlledAbilityActivationCall; Call; Call = Call->Previous)
	{
		if (!Call->OriginalActivation.HasSameActivation(Record->Context.GetOriginalActivation())) { continue; }
		check(!Call->TerminationExitDependencies.Contains(Record));
		if (!Call->OriginalTerminationCompleted.IsValid())
		{
			Call->OriginalTerminationCompleted = MakeShared<FGGYGOAbilityTerminationCompletedNotice>();
		}
		Call->TerminationExitDependencies.Add(Record);
		Record->TryCompletionSlots.Add(Call->OriginalTerminationCompleted);
		++Record->OpenTryCalls;
	}
}

FGGYGOAbilityTerminationCompletedEvent& UGGYGOAbilitySystemComponent::OnAbilityTerminationCompleted()
{
	return AbilityTerminationCompletedEvent;
}

void UGGYGOAbilitySystemComponent::TryPublishOriginalTerminationCompleted(
	const TSharedPtr<UGGYGOGameplayAbility::FOriginalTerminationRecord>& Record)
{
	check(IsInGameThread());
	if (!Record.IsValid() || Record->bSealed || Record->ASC.Get() != this
		|| Record->Outcome == EGGYGOAbilityTerminationOutcome::Failed || Record->bHasNativeCleanupSource
		|| Record->OpenDispatches != 0 || Record->OpenTryCalls != 0 || Record->bContinuationQueued || Record->bContinuationReady
		|| !Record->bNativeEndObserved || !Record->bNativeEndReturned || !Record->bFullEndReturned
		|| (Record->Context.GetRequestKind() == EGGYGOAbilityTerminationRequestKind::Cancel && !Record->bCancelReturned))
	{
		return;
	}
	FGGYGOAbilityTerminationCompletedNotice Notice;
	Notice.Original = Record->Context;
	Notice.Outcome = EGGYGOAbilityTerminationOutcome::Completed;
	Notice.Reason = EGGYGOAbilityTerminationReason::None;
	Record->Outcome = Notice.Outcome;
	Record->Reason = Notice.Reason;
	Record->bSealed = true;
	// Seal synchronous original result slots before listeners can start a successor.
	for (const auto& WeakSlot : Record->TryCompletionSlots)
	{
		if (const auto Slot = WeakSlot.Pin()) { *Slot = Notice; }
	}
	Record->TryCompletionSlots.Reset();
	if (UGGYGOGameplayAbility* Ability = Record->Ability.Get())
	{
		if (Ability->CurrentControlledActivation.HasSameActivation(Record->Context.GetOriginalActivation()))
		{
			Ability->RetireControlledActivation();
		}
		if (Ability->OriginalTermination == Record) { Ability->OriginalTermination.Reset(); }
	}
	AbilityTerminationCompletedEvent.Broadcast(Notice); // No original state writes after callbacks.
}

EGGYGOAbilityActivationRequestReason UGGYGOAbilitySystemComponent::CheckControlledAbilityActivationStart(
	const FControlledAbilityActivationCall& Call) const
{
	using EReason = EGGYGOAbilityActivationRequestReason;
	if (!IsValid(this) || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) { return EReason::InvalidASC; }
	if (AvatarSwitchAbilityExitScope) { return EReason::TerminationInProgress; }
	const FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(Call.Handle);
	if (!Spec || Spec->PendingRemove || !IsValid(Spec->Ability.Get())
		|| Spec->Ability.Get() != Call.SpecAbility.Get()) { return EReason::InvalidSpec; }
	const UGGYGOGameplayAbility* ProjectAbility = Cast<UGGYGOGameplayAbility>(Spec->Ability.Get());
	if (!ProjectAbility || ProjectAbility->bRetriggerInstancedAbility
		|| Spec->Ability->GetInstancingPolicy() == EGameplayAbilityInstancingPolicy::NonInstanced)
	{
		return EReason::UnsupportedEntry;
	}
	FActualAvatarBindingActorInfoSnapshot Actual;
	EGGYGOAvatarBindingReason SnapshotReason;
	if (!CaptureAvatarBindingActualSnapshot(Actual, SnapshotReason)
		|| !ValidateAvatarBindingActualSnapshot(Actual, SnapshotReason)
		|| !Actual.OwnerActor.IsValid() || !Actual.AvatarActor.IsValid()
		|| !HasSameAvatarBindingActualSnapshot(Call.OriginalActual, Actual)
		|| !HasSameAvatarBindingContextValue(Call.OriginalContext, GetAvatarBindingContext()))
	{
		return EReason::InvalidActorInfo;
	}
	if (Spec->Ability->GetInstancingPolicy() == EGameplayAbilityInstancingPolicy::InstancedPerActor)
	{
		const UGGYGOGameplayAbility* Primary = Cast<UGGYGOGameplayAbility>(Spec->GetPrimaryInstance());
		if (Primary && Primary->IsControlledActivationTerminationBusy()) { return EReason::TerminationInProgress; }
		if (Primary && Primary->IsActive()) { return EReason::SameInstanceRetrigger; }
		for (const FControlledAbilityActivationCall* Parent = Call.Previous; Parent; Parent = Parent->Previous)
		{
			if (Parent->Handle == Call.Handle && Parent->SpecAbility == Call.SpecAbility)
			{
				return EReason::MissingActivationBoundary;
			}
		}
		if (Primary && Primary->LastControlledActivationSerial == MAX_uint64) { return EReason::IdentityExhausted; }
	}
	return EReason::None;
}

FGGYGOAbilityActivationRequestResult UGGYGOAbilitySystemComponent::TryActivateAbilityWithTerminationBoundary(
	FGameplayAbilitySpecHandle Handle, bool bAllowRemoteActivation)
{
	check(IsInGameThread());
	using EReason = EGGYGOAbilityActivationRequestReason;
	using EOutcome = EGGYGOAbilityActivationRequestOutcome;
	FGGYGOAbilityActivationRequestResult Result;
	if (!Handle.IsValid()) { return Result; }
	if (!IsValid(this) || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		Result.Reason = EReason::InvalidASC;
		return Result;
	}
	const FString OriginalASCPath = GetPathName();
	FControlledAbilityActivationCall Call;
	Call.Handle = Handle;
	const FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(Handle);
	if (!Spec || !Spec->Ability) { Result.Reason = EReason::InvalidSpec; return Result; }
	Call.SpecAbility = Spec->Ability.Get();
	Call.EvaluationAbility = Spec->GetPrimaryInstance() ? Spec->GetPrimaryInstance() : Spec->Ability.Get();
	Call.OriginalContext = GetAvatarBindingContext();
	Call.ParentEvaluationSerial = AbilityActivationEvaluations.IsEmpty() ? 0 : AbilityActivationEvaluations.Last().Serial;
	Call.QueryDepth = AbilityActivationQueryDepth;
	EGGYGOAvatarBindingReason SnapshotReason;
	if (!CaptureAvatarBindingActualSnapshot(Call.OriginalActual, SnapshotReason))
	{
		Result.Reason = EReason::InvalidActorInfo;
		return Result;
	}
	// Evaluate the parent call chain before linking this new stack-local request.
	Call.Previous = ControlledAbilityActivationCall;
	bool bEnclosingSameInstanceCall = false;
	if (Spec->Ability->GetInstancingPolicy() == EGameplayAbilityInstancingPolicy::InstancedPerActor)
	{
		for (const FControlledAbilityActivationCall* Parent = Call.Previous; Parent; Parent = Parent->Previous)
		{
			if (Parent->Handle == Handle && Parent->SpecAbility == Call.SpecAbility)
			{
				bEnclosingSameInstanceCall = true;
				break;
			}
		}
	}
	Call.Failure = CheckControlledAbilityActivationStart(Call);
	if (Call.Failure == EReason::None)
	{
		{
			FScopedControlledAbilityActivationCall Scope(this, Call);
			// Exactly one native call. The scope covers PreActivate/Activate and native late writes.
			Result.bNativeAccepted = Super::TryActivateAbility(Handle, bAllowRemoteActivation);
		}
		Result.OriginalActivation = Call.OriginalActivation;
		if (Call.OriginalTerminationCompleted.IsValid())
		{
			Result.OriginalTerminationCompleted = *Call.OriginalTerminationCompleted;
		}
	}
	if (Call.Failure != EReason::None)
	{
		Result.Reason = Call.Failure;
		Result.Outcome = (Call.Failure == EReason::TerminationInProgress || Call.Failure == EReason::SameInstanceRetrigger
			|| (Call.Failure == EReason::MissingActivationBoundary && bEnclosingSameInstanceCall))
			? EOutcome::Busy
			: (Call.Failure == EReason::IdentityExhausted || Call.Failure == EReason::MissingActivationBoundary)
				? EOutcome::Failed : EOutcome::Rejected;
		UE_LOG(LogGGYGOAbilitySystem, Verbose,
			TEXT("AbilitySystem controlled Try [%s] Spec [%s] Ability [%s] rejected, reason=%d nativeAccepted=%d."),
			*OriginalASCPath, *Handle.ToString(), *GetNameSafe(Call.SpecAbility.Get()),
			static_cast<int32>(Call.Failure), Result.bNativeAccepted ? 1 : 0);
		return Result;
	}
	Result.Outcome = Result.bNativeAccepted ? EOutcome::Accepted : EOutcome::Rejected;
	Result.Reason = Result.bNativeAccepted ? EReason::None : EReason::NativeActivationRejected;
	// Result contains sealed original history; never reread current GA after completion callbacks.
	return Result;
}

uint64 UGGYGOAbilitySystemComponent::BeginControlledAbilityActivationEvaluation(
	const UGGYGOGameplayAbility* Ability, FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo)
{
	FControlledAbilityActivationCall* Call = ControlledAbilityActivationCall;
	if (!Call || AbilityActivationEvaluations.IsEmpty()) { return 0; }
	const FAbilityActivationEvaluationToken& Evaluation = AbilityActivationEvaluations.Last();
	if (Call->Handle != Handle || Call->EvaluationAbility.Get() != Ability
		|| Call->OriginalActual.Allocation.Get() != ActorInfo
		|| Call->ParentEvaluationSerial != Evaluation.ParentSerial
		|| Call->QueryDepth != AbilityActivationQueryDepth)
	{
		return 0; // Legacy, nested raw or QueryOnly evaluation; never borrow this request's provenance.
	}
	if (Call->bEvaluationClaimed)
	{
		if (!Call->bLocalWitnessSeen && Call->Failure == EGGYGOAbilityActivationRequestReason::None)
		{
			// An unscoped same-source reentry between original Can and witness is ambiguous.
			// Preserve native behavior, but refuse to authenticate either as the original request.
			Call->Failure = EGGYGOAbilityActivationRequestReason::MissingActivationBoundary;
		}
		return 0;
	}
	Call->bEvaluationClaimed = true;
	Call->EvaluationSerial = Evaluation.Serial;
	return Evaluation.Serial;
}

bool UGGYGOAbilitySystemComponent::CompleteControlledAbilityActivationEvaluation(
	uint64 EvaluationSerial, bool bNativeRulesPassed)
{
	FControlledAbilityActivationCall* Call = ControlledAbilityActivationCall;
	if (!Call || !Call->bEvaluationClaimed || Call->EvaluationSerial != EvaluationSerial)
	{
		return bNativeRulesPassed;
	}
	if (Call->Failure == EGGYGOAbilityActivationRequestReason::None)
	{
		Call->Failure = CheckControlledAbilityActivationStart(*Call);
	}
	Call->bCanAdmitted = bNativeRulesPassed && Call->Failure == EGGYGOAbilityActivationRequestReason::None;
	return Call->bCanAdmitted;
}

void UGGYGOAbilitySystemComponent::ObserveControlledAbilityActivation(
	FGameplayAbilitySpecHandle Handle, UGGYGOGameplayAbility* Ability)
{
	// This is the actual native PreActivate notification, before public activation listeners.
	// GA alone issues the identity; a controlled Try supplies only its outer return provenance.
	// The preceding lease cannot authenticate a later native activation, including
	// an unsupported raw entry that bypassed admission and fails to issue a new one.
	Ability->NativeCleanupActivation = {};
	if (Ability->IsControlledActivationTerminationBusy())
	{
		Ability->InvalidateOriginalTerminationForActivation();
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("AbilitySystem native activation ASC [%s] Spec [%s] Instance [%s] rejected: original lifecycle is still busy; raw entry bypassed admission."),
			*GetPathName(), *Handle.ToString(), *GetPathNameSafe(Ability));
		return;
	}
	Ability->RetireControlledActivation();
	FControlledAbilityActivationCall* Call = ControlledAbilityActivationCall;
	const bool bMatchesCall = Call && Call->Handle == Handle && !Call->bLocalWitnessSeen;
	FActualAvatarBindingActorInfoSnapshot Actual;
	EGGYGOAvatarBindingReason SnapshotReason;
	const bool bHasActual = CaptureAvatarBindingActualSnapshot(Actual, SnapshotReason)
		&& ValidateAvatarBindingActualSnapshot(Actual, SnapshotReason);
	const FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(Handle);
	const bool bHasTryBoundary = bMatchesCall && Call->bCanAdmitted
		&& Call->Failure == EGGYGOAbilityActivationRequestReason::None
		&& Spec && Spec->Ability.Get() == Call->SpecAbility.Get()
		&& bHasActual && HasSameAvatarBindingActualSnapshot(Call->OriginalActual, Actual)
		&& HasSameAvatarBindingContextValue(Call->OriginalContext, GetAvatarBindingContext());
	EGGYGOAbilityActivationRequestReason IssueReason = EGGYGOAbilityActivationRequestReason::InvalidActorInfo;
	const FGGYGOAbilityActivationHandle Original = bHasActual
		? Ability->IssueControlledActivation(this, Handle, Actual.Allocation.Get(), bHasTryBoundary, IssueReason)
		: FGGYGOAbilityActivationHandle{};
	if (!Original.HasActivation())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("AbilitySystem native activation ASC [%s] Spec [%s] Instance [%s] could not issue original history: reason=%d."),
			*GetPathName(), *Handle.ToString(), *GetPathNameSafe(Ability), static_cast<int32>(IssueReason));
	}
	if (!bMatchesCall) { return; }
	Call->bLocalWitnessSeen = true; // First witness sealed before Super can broadcast/reenter.
	if (bHasTryBoundary && Original.HasActivation())
	{
		Call->OriginalActivation = Original;
	}
	else if (Call->Failure == EGGYGOAbilityActivationRequestReason::None)
	{
		Call->Failure = Original.HasActivation()
			? EGGYGOAbilityActivationRequestReason::MissingActivationBoundary : IssueReason;
	}
}

void UGGYGOAbilitySystemComponent::CancelAbilitiesByFunc(TShouldCancelAbilityFunc ShouldCancelFunc, bool bReplicateCancelAbility)
{
	ABILITYLIST_SCOPE_LOCK();
	for (const FGameplayAbilitySpec& AbilitySpec : ActivatableAbilities.Items)
	{
		// 非项目能力不参与本取消流程。用 Cast 而非 CastChecked：
		// 第三方或引擎能力被授予到同一个 ASC 上是允许的，不该因此崩掉。
		const UGGYGOGameplayAbility* AbilityCDO = Cast<UGGYGOGameplayAbility>(AbilitySpec.Ability);
		if (!AbilityCDO)
		{
			continue;
		}

		// 一个 Spec 可能有多个运行时实例，逐个判断。
		TArray<UGameplayAbility*> Instances = AbilitySpec.GetAbilityInstances();
		for (UGameplayAbility* AbilityInstance : Instances)
		{
			UGGYGOGameplayAbility* GGYGOAbilityInstance = Cast<UGGYGOGameplayAbility>(AbilityInstance);
			if (!GGYGOAbilityInstance || !GGYGOAbilityInstance->IsActive()
				|| GGYGOAbilityInstance->IsCurrentAbilityGroupAdmissionRejected()
				|| !ShouldCancelFunc(GGYGOAbilityInstance, AbilitySpec.Handle))
			{
				continue;
			}

			if (GGYGOAbilityInstance->CanBeCanceled())
			{
				if (GGYGOAbilityInstance->IsAbilityGroupAdmissionPending())
				{
					// Pending attempt 仍在 ActivateAbility 准入调用栈中。这里只记录拒绝，
					// 由对应 attempt 在自身安全点结束，避免同步重入误减另一实例计数。
					GGYGOAbilityInstance->RejectCurrentAbilityGroupAdmission();
				}
				else
				{
					GGYGOAbilityInstance->CancelAbility(AbilitySpec.Handle, AbilityActorInfo.Get(),
						GGYGOAbilityInstance->GetCurrentActivationInfo(), bReplicateCancelAbility);
				}
			}
			else
			{
				// 不强制打断，只记录。能力声明自己不可取消时强行取消会破坏它的状态机。
				UE_LOG(LogGGYGOAbilitySystem, Error,
					TEXT("CancelAbilitiesByFunc: 无法取消能力 [%s]，它的 CanBeCanceled 为 false。"),
					*GGYGOAbilityInstance->GetName());
			}
		}
	}
}

void UGGYGOAbilitySystemComponent::CancelInputActivatedAbilities(bool bReplicateCancelAbility)
{
	auto ShouldCancelFunc = [](const UGGYGOGameplayAbility* Ability, FGameplayAbilitySpecHandle Handle)
	{
		const EGGYGOAbilityActivationPolicy Policy = Ability->GetActivationPolicy();
		return (Policy == EGGYGOAbilityActivationPolicy::OnInputTriggered) || (Policy == EGGYGOAbilityActivationPolicy::WhileInputActive);
	};

	CancelAbilitiesByFunc(ShouldCancelFunc, bReplicateCancelAbility);
}

FGGYGOAbilityInputRequestResult UGGYGOAbilitySystemComponent::MakeAbilityInputRequestFailure(
	EGGYGOAbilityInputRequestOutcome Outcome, EGGYGOAbilityInputRequestReason Reason,
	const FGGYGOAbilityInputRequestIdentity& Identity, FGameplayTag Tag, double Deadline) const
{
	static const TCHAR* const Names[] = {
		TEXT("None"), TEXT("InvalidRequest"), TEXT("InvalidTag"), TEXT("InvalidDeadline"),
		TEXT("InvalidPreviousIdentity"), TEXT("InvalidEndKind"), TEXT("InvalidASC"),
		TEXT("WrongASC"), TEXT("WrongRevision"), TEXT("UnknownRequest"), TEXT("RequestEnded"),
		TEXT("RequestMismatch"), TEXT("ActorInfoUnavailable"), TEXT("InputBlocked"),
		TEXT("NoMatchingSpec"), TEXT("Expired"), TEXT("NoQueuedAdmission"), TEXT("NotHeld"),
		TEXT("SerialExhausted"), TEXT("RevisionExhausted")
	};
	const int32 Index = static_cast<int32>(Reason);
	const TCHAR* Name = Index >= 0 && Index < UE_ARRAY_COUNT(Names) ? Names[Index] : TEXT("InvalidReason");
	UE_LOG(LogGGYGOAbilitySystem, Warning,
		TEXT("AbilitySystem InputRequest rejected: ASC=%s Source=%s Tag=%s Revision=%llu Request=%llu Deadline=%.6f Outcome=%d Reason=%s"),
		*GetPathName(), *GetPathNameSafe(Identity.SourceASC.Get()), *Tag.ToString(),
		Identity.InputRevision, Identity.RequestSerial, Deadline, static_cast<int32>(Outcome), Name);
	FGGYGOAbilityInputRequestResult Result;
	Result.Outcome = Outcome;
	Result.Reason = Reason;
	return Result; // Failure never issues an identity or substitutes another request.
}

const UGGYGOAbilitySystemComponent::FAbilityInputRequestRecord*
UGGYGOAbilitySystemComponent::FindAbilityInputRequest(const FGGYGOAbilityInputRequestIdentity& Identity) const
{
	if (!Identity.IsAssigned() || Identity.InputRevision != AbilityInputRevision
		|| !Identity.SourceASC.HasSameIndexAndSerialNumber(TWeakObjectPtr<UGGYGOAbilitySystemComponent>(
			const_cast<UGGYGOAbilitySystemComponent*>(this))))
	{
		return nullptr;
	}
	const FAbilityInputRequestRecord* Record = AbilityInputRequests.Find(Identity.RequestSerial);
	return Record && Record->Request.Identity == Identity ? Record : nullptr;
}

bool UGGYGOAbilitySystemComponent::IsAbilityInputContextCurrent(const TWeakObjectPtr<AActor>& Owner,
	const TWeakObjectPtr<AActor>& Avatar, const TWeakObjectPtr<UWorld>& World) const
{
	return IsValid(this) && !HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		&& AbilityActorInfo.IsValid() && AbilityActorInfo->AbilitySystemComponent.Get() == this
		&& Owner.IsValid() && Avatar.IsValid() && World.IsValid()
		&& !Owner->IsActorBeingDestroyed() && !Avatar->IsActorBeingDestroyed()
		&& World.Get() == GetWorld()
		&& AbilityActorInfo->OwnerActor.HasSameIndexAndSerialNumber(Owner)
		&& AbilityActorInfo->AvatarActor.HasSameIndexAndSerialNumber(Avatar)
		&& Owner.HasSameIndexAndSerialNumber(TWeakObjectPtr<AActor>(GetOwnerActor()))
		&& Avatar.HasSameIndexAndSerialNumber(TWeakObjectPtr<AActor>(GetAvatarActor()));
}

bool UGGYGOAbilitySystemComponent::IsAbilityInputRequestCurrent(const FGGYGOAbilityInputRetryRequest& Request,
	FGameplayAbilitySpecHandle Handle, bool bRequireDeadline, bool bRequireQueued, bool bRequireHeld) const
{
	const FAbilityInputRequestRecord* Record = FindAbilityInputRequest(Request.Identity);
	if (!Record || Record->Request.InputTag != Request.InputTag
		|| Record->Request.OriginalDeadline != Request.OriginalDeadline
		|| !IsAbilityInputContextCurrent(Record->OwnerActor, Record->AvatarActor, Record->World)
		|| !Record->SpecHandles.Contains(Handle) || (bRequireHeld && !Record->bHeld)
		|| (bRequireQueued && !Record->RetryableSpecHandles.Contains(Handle)))
	{
		return false;
	}
	if (bRequireDeadline && (!FMath::IsFinite(Request.OriginalDeadline)
		|| static_cast<double>(Record->World->GetTimeSeconds()) >= Request.OriginalDeadline))
	{
		return false;
	}
	const FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(Handle);
	return Spec && Spec->Ability && !Spec->PendingRemove && !Spec->RemoveAfterActivation
		&& Spec->GetDynamicSpecSourceTags().HasTagExact(Request.InputTag);
}

bool UGGYGOAbilitySystemComponent::IsAbilityInputEdgeCurrent(const FAbilityInputEdge& Edge) const
{
	if (Edge.InputRevision != AbilityInputRevision
		|| !IsAbilityInputContextCurrent(Edge.OwnerActor, Edge.AvatarActor, Edge.World))
	{
		return false;
	}
	if (!Edge.bPressed)
	{
		return true; // A recorded real release is history, not a new request or a retry permit.
	}
	for (const FGGYGOAbilityInputRequestIdentity& Identity : Edge.Sources)
	{
		if (const FAbilityInputRequestRecord* Record = FindAbilityInputRequest(Identity))
		{
			if (IsAbilityInputRequestCurrent(Record->Request, Edge.Handle, false, false, false))
			{
				return true;
			}
		}
	}
	return false;
}

void UGGYGOAbilitySystemComponent::RebuildAbilityInputHeldHandles()
{
	InputHeldSpecHandles.Reset();
	TArray<uint64> Serials;
	AbilityInputRequests.GetKeys(Serials);
	Serials.Sort();
	for (const uint64 Serial : Serials)
	{
		const FAbilityInputRequestRecord& Record = AbilityInputRequests.FindChecked(Serial);
		if (Record.bHeld)
		{
			for (const FGameplayAbilitySpecHandle Handle : Record.SpecHandles)
			{
				if (IsAbilityInputRequestCurrent(Record.Request, Handle, false, false, true))
				{
					InputHeldSpecHandles.AddUnique(Handle);
				}
			}
		}
	}
}

void UGGYGOAbilitySystemComponent::PruneAbilityInputRequests()
{
	TArray<FGGYGOAbilityInputRequestIdentity> Retired;
	for (const TPair<uint64, FAbilityInputRequestRecord>& Entry : AbilityInputRequests)
	{
		const FAbilityInputRequestRecord& Record = Entry.Value;
		if (!IsAbilityInputContextCurrent(Record.OwnerActor, Record.AvatarActor, Record.World))
		{
			Retired.Add(Record.Request.Identity);
			continue;
		}
		// Held survives its retry deadline. Released storage also survives pending real edges/in-flight snapshots.
		if (!Record.bHeld && !bProcessingAbilityInput
			&& static_cast<double>(Record.World->GetTimeSeconds()) >= Record.Request.OriginalDeadline
			&& !PendingAbilityInputEdges.ContainsByPredicate([&Record](const FAbilityInputEdge& Edge)
				{ return Edge.Sources.Contains(Record.Request.Identity); }))
		{
			Retired.Add(Record.Request.Identity);
		}
	}
	for (const FGGYGOAbilityInputRequestIdentity& Identity : Retired)
	{
		EndAbilityInputRequest(Identity, EGGYGOAbilityInputRequestEndKind::Invalidated);
	}
	RebuildAbilityInputHeldHandles();
}

void UGGYGOAbilitySystemComponent::ConsumeSuccessfulAbilityInputRequests(
	const TArray<FGGYGOAbilityInputRetryRequest>& Requests)
{
	for (const FGGYGOAbilityInputRetryRequest& Request : Requests)
	{
		FAbilityInputRequestRecord* Record = AbilityInputRequests.Find(Request.Identity.RequestSerial);
		if (Record && Record->Request.Identity == Request.Identity
			&& Record->Request.InputTag == Request.InputTag
			&& Record->Request.OriginalDeadline == Request.OriginalDeadline)
		{
			Record->RetryableSpecHandles.Reset();
			PendingAbilityInputRetries.RemoveAll([&Request](const FGGYGOAbilityInputRetryRequest& Pending)
				{ return Pending.Identity == Request.Identity; });
		}
	}
}

FGGYGOAbilityInputRequestResult UGGYGOAbilitySystemComponent::ReceiveAbilityInputRequest(
	const FGameplayTag& InputTag, const FGGYGOAbilityInputRequestIdentity& PreviousIdentity, double OriginalDeadline)
{
	check(IsInGameThread());
	using EOutcome = EGGYGOAbilityInputRequestOutcome;
	using EReason = EGGYGOAbilityInputRequestReason;
	const auto Fail = [&](EOutcome Outcome, EReason Reason)
		{ return MakeAbilityInputRequestFailure(Outcome, Reason, PreviousIdentity, InputTag, OriginalDeadline); };
	if (!IsValid(this) || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) { return Fail(EOutcome::Rejected, EReason::InvalidASC); }
	if (!InputTag.IsValid()) { return Fail(EOutcome::Rejected, EReason::InvalidTag); }
	if (!FMath::IsFinite(OriginalDeadline) || OriginalDeadline < 0.0) { return Fail(EOutcome::Rejected, EReason::InvalidDeadline); }
	const bool bEmptyPrevious = PreviousIdentity.RequestSerial == 0 && PreviousIdentity.InputRevision == 0
		&& PreviousIdentity.SourceASC.IsExplicitlyNull();
	if (!bEmptyPrevious && !PreviousIdentity.IsAssigned()) { return Fail(EOutcome::Rejected, EReason::InvalidPreviousIdentity); }
	PruneAbilityInputRequests();
	if (!bEmptyPrevious)
	{
		if (!PreviousIdentity.SourceASC.HasSameIndexAndSerialNumber(TWeakObjectPtr<UGGYGOAbilitySystemComponent>(this)))
			{ return Fail(EOutcome::Stale, EReason::WrongASC); }
		if (PreviousIdentity.InputRevision != AbilityInputRevision) { return Fail(EOutcome::Stale, EReason::WrongRevision); }
		const FAbilityInputRequestRecord* Existing = FindAbilityInputRequest(PreviousIdentity);
		if (!Existing) { return Fail(EOutcome::Stale, EReason::UnknownRequest); }
		if (Existing->Request.InputTag != InputTag || Existing->Request.OriginalDeadline != OriginalDeadline)
			{ return Fail(EOutcome::Rejected, EReason::RequestMismatch); }
		if (!Existing->bHeld) { return Fail(EOutcome::Stale, EReason::RequestEnded); }
		if (HasMatchingGameplayTag(TAG_GGYGO_Gameplay_AbilityInputBlocked)) { return Fail(EOutcome::Rejected, EReason::InputBlocked); }
		return FGGYGOAbilityInputRequestResult{EOutcome::AlreadyApplied, EReason::None, Existing->Request.Identity};
	}
	const TWeakObjectPtr<AActor> Owner(GetOwnerActor());
	const TWeakObjectPtr<AActor> Avatar(GetAvatarActor());
	const TWeakObjectPtr<UWorld> World(GetWorld());
	if (!IsAbilityInputContextCurrent(Owner, Avatar, World)) { return Fail(EOutcome::Rejected, EReason::ActorInfoUnavailable); }
	if (HasMatchingGameplayTag(TAG_GGYGO_Gameplay_AbilityInputBlocked)) { return Fail(EOutcome::Rejected, EReason::InputBlocked); }
	if (static_cast<double>(World->GetTimeSeconds()) > OriginalDeadline) { return Fail(EOutcome::Rejected, EReason::Expired); }
	if (AbilityInputRevision == MAX_uint64) { return Fail(EOutcome::Rejected, EReason::RevisionExhausted); }
	if (LastAbilityInputRequestSerial == MAX_uint64) { return Fail(EOutcome::Rejected, EReason::SerialExhausted); }
	FAbilityInputRequestRecord Record;
	Record.Request.InputTag = InputTag;
	Record.Request.OriginalDeadline = OriginalDeadline;
	Record.OwnerActor = Owner;
	Record.AvatarActor = Avatar;
	Record.World = World;
	for (const FGameplayAbilitySpec& Spec : ActivatableAbilities.Items)
	{
		if (Spec.Ability && !Spec.PendingRemove && !Spec.RemoveAfterActivation
			&& Spec.GetDynamicSpecSourceTags().HasTagExact(InputTag))
		{
			Record.SpecHandles.Add(Spec.Handle);
		}
	}
	if (Record.SpecHandles.IsEmpty()) { return Fail(EOutcome::Rejected, EReason::NoMatchingSpec); }
	Record.Request.Identity = FGGYGOAbilityInputRequestIdentity{this, AbilityInputRevision, ++LastAbilityInputRequestSerial};
	const FGGYGOAbilityInputRequestIdentity Identity = Record.Request.Identity;
	const TArray<FGameplayAbilitySpecHandle> PreviousHeld = InputHeldSpecHandles;
	const TArray<FGameplayAbilitySpecHandle> ReceivedSpecs = Record.SpecHandles;
	AbilityInputRequests.Add(Identity.RequestSerial, MoveTemp(Record));
	for (const FGameplayAbilitySpecHandle Handle : ReceivedSpecs)
	{
		if (!PreviousHeld.Contains(Handle))
		{
			FAbilityInputEdge Edge;
			Edge.Handle = Handle;
			Edge.InputRevision = AbilityInputRevision;
			Edge.bPressed = true;
			Edge.OwnerActor = Owner;
			Edge.AvatarActor = Avatar;
			Edge.World = World;
			Edge.Sources.Add(Identity);
			PendingAbilityInputEdges.Add(MoveTemp(Edge));
		}
		else
		{
			// A second source contributes to the same unconsumed aggregate press; never emits another press.
			for (int32 Index = PendingAbilityInputEdges.Num() - 1; Index >= 0; --Index)
			{
				FAbilityInputEdge& Edge = PendingAbilityInputEdges[Index];
				if (Edge.Handle == Handle)
				{
					if (Edge.bPressed) { Edge.Sources.AddUnique(Identity); }
					break;
				}
			}
		}
	}
	RebuildAbilityInputHeldHandles();
	return FGGYGOAbilityInputRequestResult{EOutcome::Accepted, EReason::None, Identity};
}

void UGGYGOAbilitySystemComponent::ClientCorrectAbilityState_Implementation(FGameplayAbilitySpecHandle AbilityHandle,
	FPredictionKey ActivationKey, const FGameplayAbilityTargetDataHandle& Correction)
{
	FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(AbilityHandle);
	if (!Spec || !Spec->IsActive())
	{
		return;
	}

	// 同一 Spec 可能有多个实例（例如 InstancedPerExecution）；只把纠正交给当前仍活跃且
	// 激活预测键匹配的 GGYGO 实例。ASC 不解释 TargetData 的业务类型。
	for (UGameplayAbility* AbilityInstance : Spec->GetAbilityInstances())
	{
		UGGYGOGameplayAbility* GGYGOAbility = Cast<UGGYGOGameplayAbility>(AbilityInstance);
		if (GGYGOAbility && GGYGOAbility->IsActive()
			&& GGYGOAbility->GetCurrentAbilitySpecHandle() == AbilityHandle
			&& GGYGOAbility->GetCurrentActivationInfo().GetActivationPredictionKey() == ActivationKey)
		{
			GGYGOAbility->ReceiveAbilityCorrection(Correction);
			return;
		}
	}
}

FGGYGOAbilityInputRequestResult UGGYGOAbilitySystemComponent::EndAbilityInputRequest(
	const FGGYGOAbilityInputRequestIdentity& Identity, EGGYGOAbilityInputRequestEndKind EndKind)
{
	check(IsInGameThread());
	using EOutcome = EGGYGOAbilityInputRequestOutcome;
	using EReason = EGGYGOAbilityInputRequestReason;
	const auto Fail = [&](EOutcome Outcome, EReason Reason)
		{ return MakeAbilityInputRequestFailure(Outcome, Reason, Identity, FGameplayTag{}, NoAbilityInputRetryDeadline); };
	if (!IsValid(this) || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) { return Fail(EOutcome::Rejected, EReason::InvalidASC); }
	if (EndKind != EGGYGOAbilityInputRequestEndKind::Released && EndKind != EGGYGOAbilityInputRequestEndKind::Invalidated)
		{ return Fail(EOutcome::Rejected, EReason::InvalidEndKind); }
	if (!Identity.IsAssigned()) { return Fail(EOutcome::Rejected, EReason::InvalidRequest); }
	if (!Identity.SourceASC.HasSameIndexAndSerialNumber(TWeakObjectPtr<UGGYGOAbilitySystemComponent>(this)))
		{ return Fail(EOutcome::Stale, EReason::WrongASC); }
	if (Identity.InputRevision != AbilityInputRevision) { return Fail(EOutcome::Stale, EReason::WrongRevision); }
	FAbilityInputRequestRecord* Record = AbilityInputRequests.Find(Identity.RequestSerial);
	if (!Record || Record->Request.Identity != Identity) { return Fail(EOutcome::Stale, EReason::UnknownRequest); }
	const TArray<FGameplayAbilitySpecHandle> Affected = Record->SpecHandles;
	if (EndKind == EGGYGOAbilityInputRequestEndKind::Released)
	{
		if (!IsAbilityInputContextCurrent(Record->OwnerActor, Record->AvatarActor, Record->World))
			{ return Fail(EOutcome::Stale, EReason::ActorInfoUnavailable); }
		if (!Record->bHeld) { return FGGYGOAbilityInputRequestResult{EOutcome::AlreadyApplied, EReason::None, Identity}; }
		Record->bHeld = false; // Deadline does not gate a true release.
		RebuildAbilityInputHeldHandles();
		for (const FGameplayAbilitySpecHandle Handle : Affected)
		{
			if (!InputHeldSpecHandles.Contains(Handle))
			{
				FAbilityInputEdge Edge;
				Edge.Handle = Handle;
				Edge.InputRevision = AbilityInputRevision;
				Edge.OwnerActor = Record->OwnerActor;
				Edge.AvatarActor = Record->AvatarActor;
				Edge.World = Record->World;
				Edge.Sources.Add(Identity);
				PendingAbilityInputEdges.Add(MoveTemp(Edge));
			}
		}
	}
	else
	{
		// Original-ID retirement needs neither Ready, current actor context nor a live deadline.
		AbilityInputRequests.Remove(Identity.RequestSerial);
		PendingAbilityInputRetries.RemoveAll([&Identity](const FGGYGOAbilityInputRetryRequest& Request)
			{ return Request.Identity == Identity; });
		for (FAbilityInputEdge& Edge : PendingAbilityInputEdges)
		{
			if (Edge.bPressed) { Edge.Sources.Remove(Identity); }
		}
		PendingAbilityInputEdges.RemoveAll([](const FAbilityInputEdge& Edge)
			{ return Edge.bPressed && Edge.Sources.IsEmpty(); });
		RebuildAbilityInputHeldHandles();
		for (const FGameplayAbilitySpecHandle Handle : Affected)
		{
			if (!InputHeldSpecHandles.Contains(Handle))
			{
				if (FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(Handle)) { Spec->InputPressed = false; }
			}
		}
	}
	return FGGYGOAbilityInputRequestResult{EOutcome::Accepted, EReason::None, Identity};
}

FGGYGOAbilityInputRequestResult UGGYGOAbilitySystemComponent::QueueAbilityInputRetry(
	const FGGYGOAbilityInputRetryRequest& Request)
{
	check(IsInGameThread());
	using EOutcome = EGGYGOAbilityInputRequestOutcome;
	using EReason = EGGYGOAbilityInputRequestReason;
	const auto Fail = [&](EOutcome Outcome, EReason Reason)
		{ return MakeAbilityInputRequestFailure(Outcome, Reason, Request.Identity, Request.InputTag, Request.OriginalDeadline); };
	if (!IsValid(this) || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) { return Fail(EOutcome::Rejected, EReason::InvalidASC); }
	if (!Request.InputTag.IsValid()) { return Fail(EOutcome::Rejected, EReason::InvalidTag); }
	if (!Request.Identity.IsAssigned()) { return Fail(EOutcome::Rejected, EReason::InvalidRequest); }
	if (!FMath::IsFinite(Request.OriginalDeadline) || Request.OriginalDeadline < 0.0)
		{ return Fail(EOutcome::Rejected, EReason::InvalidDeadline); }
	if (!Request.Identity.SourceASC.HasSameIndexAndSerialNumber(TWeakObjectPtr<UGGYGOAbilitySystemComponent>(this)))
		{ return Fail(EOutcome::Stale, EReason::WrongASC); }
	if (Request.Identity.InputRevision != AbilityInputRevision) { return Fail(EOutcome::Stale, EReason::WrongRevision); }
	const FAbilityInputRequestRecord* Record = FindAbilityInputRequest(Request.Identity);
	if (!Record) { return Fail(EOutcome::Stale, EReason::UnknownRequest); }
	if (Record->Request.InputTag != Request.InputTag || Record->Request.OriginalDeadline != Request.OriginalDeadline)
		{ return Fail(EOutcome::Rejected, EReason::RequestMismatch); }
	if (!IsAbilityInputContextCurrent(Record->OwnerActor, Record->AvatarActor, Record->World))
		{ return Fail(EOutcome::Stale, EReason::ActorInfoUnavailable); }
	if (HasMatchingGameplayTag(TAG_GGYGO_Gameplay_AbilityInputBlocked)) { return Fail(EOutcome::Rejected, EReason::InputBlocked); }
	if (static_cast<double>(Record->World->GetTimeSeconds()) >= Request.OriginalDeadline)
		{ return Fail(EOutcome::Stale, EReason::Expired); }
	bool bEligible = false;
	bool bNeedsHeld = false;
	for (const FGameplayAbilitySpecHandle Handle : Record->RetryableSpecHandles)
	{
		const FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(Handle);
		const UGGYGOGameplayAbility* CDO = Spec ? Cast<UGGYGOGameplayAbility>(Spec->Ability) : nullptr;
		if (!CDO || Spec->IsActive()) { continue; }
		const EGGYGOAbilityActivationPolicy Policy = CDO->GetActivationPolicy();
		const bool bWhileHeld = Policy == EGGYGOAbilityActivationPolicy::WhileInputActive;
		bNeedsHeld |= bWhileHeld && !Record->bHeld;
		bEligible |= (Policy == EGGYGOAbilityActivationPolicy::OnInputTriggered || bWhileHeld)
			&& IsAbilityInputRequestCurrent(Request, Handle, true, true, bWhileHeld);
	}
	if (!bEligible) { return Fail(EOutcome::Rejected, bNeedsHeld ? EReason::NotHeld : EReason::NoQueuedAdmission); }
	const bool bQueued = PendingAbilityInputRetries.ContainsByPredicate([&Request](const FGGYGOAbilityInputRetryRequest& Pending)
		{ return Pending.Identity == Request.Identity; });
	if (!bQueued) { PendingAbilityInputRetries.Add(Request); }
	return FGGYGOAbilityInputRequestResult{bQueued ? EOutcome::AlreadyApplied : EOutcome::Accepted, EReason::None, Request.Identity};
}

void UGGYGOAbilitySystemComponent::ProcessAbilityInput(float DeltaTime, bool bGamePaused)
{
	check(IsInGameThread());
	if (bProcessingAbilityInput) { return; }
	if (HasMatchingGameplayTag(TAG_GGYGO_Gameplay_AbilityInputBlocked))
	{
		ClearAbilityInput();
		return;
	}
	PruneAbilityInputRequests();
	TGuardValue<bool> ProcessingGuard(bProcessingAbilityInput, true);
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalASC(this);
	const uint64 ProcessingRevision = AbilityInputRevision;
	const uint64 LastSnapshotRequestSerial = LastAbilityInputRequestSerial;
	const TArray<FGameplayAbilitySpecHandle> HeldHandles = InputHeldSpecHandles;
	const TWeakObjectPtr<AActor> OriginalOwner(GetOwnerActor());
	const TWeakObjectPtr<AActor> OriginalAvatar(GetAvatarActor());
	const TWeakObjectPtr<UWorld> OriginalWorld(GetWorld());
	const TArray<FAbilityInputEdge> Edges = MoveTemp(PendingAbilityInputEdges);
	const TArray<FGGYGOAbilityInputRetryRequest> Retries = MoveTemp(PendingAbilityInputRetries);
	PendingAbilityInputEdges.Reset();
	PendingAbilityInputRetries.Reset();

	struct FRetryWork
	{
		FGGYGOAbilityInputRetryRequest Request;
		TArray<FGameplayAbilitySpecHandle> Handles;
	};
	TArray<FRetryWork> RetryWork;
	TArray<TArray<FGGYGOAbilityInputRetryRequest>> InitialSourcesByEdge;
	InitialSourcesByEdge.SetNum(Edges.Num());
	TMap<FGameplayAbilitySpecHandle, TArray<FGGYGOAbilityInputRetryRequest>> HeldSources;
	// Freeze each press edge's own sources before the first native event or Try callback.
	for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
	{
		const FAbilityInputEdge& Edge = Edges[EdgeIndex];
		if (!Edge.bPressed) { continue; }
		for (const FGGYGOAbilityInputRequestIdentity& Identity : Edge.Sources)
		{
			if (const FAbilityInputRequestRecord* Record = FindAbilityInputRequest(Identity))
			{
				TArray<FGGYGOAbilityInputRetryRequest>& Sources = InitialSourcesByEdge[EdgeIndex];
				if (!Sources.ContainsByPredicate([&Identity](const FGGYGOAbilityInputRetryRequest& Request)
					{ return Request.Identity == Identity; })) { Sources.Add(Record->Request); }
			}
		}
	}
	for (const TPair<uint64, FAbilityInputRequestRecord>& Entry : AbilityInputRequests)
	{
		const FAbilityInputRequestRecord& Record = Entry.Value;
		if (Record.bHeld)
		{
			for (const FGameplayAbilitySpecHandle Handle : Record.SpecHandles)
			{
				if (IsAbilityInputRequestCurrent(Record.Request, Handle, false, false, true))
				{
					HeldSources.FindOrAdd(Handle).Add(Record.Request);
				}
			}
		}
	}
	for (const FGGYGOAbilityInputRetryRequest& Request : Retries)
	{
		if (const FAbilityInputRequestRecord* Record = FindAbilityInputRequest(Request.Identity))
		{
			RetryWork.Add(FRetryWork{Request, Record->RetryableSpecHandles});
		}
	}
	const auto CanContinue = [&, OriginalASC]()
	{
		if (!OriginalASC.IsValid()) { return false; }
		if (HasMatchingGameplayTag(TAG_GGYGO_Gameplay_AbilityInputBlocked))
		{
			ClearAbilityInput();
			return false;
		}
		return ProcessingRevision != MAX_uint64 && ProcessingRevision == AbilityInputRevision
			&& IsAbilityInputContextCurrent(OriginalOwner, OriginalAvatar, OriginalWorld);
	};
	const auto HasSupersedingInput = [&](FGameplayAbilitySpecHandle Handle)
	{
		// Native callbacks add facts to next consumption; an old batch must not overwrite them.
		if (PendingAbilityInputEdges.ContainsByPredicate([Handle](const FAbilityInputEdge& Edge)
			{ return Edge.Handle == Handle; })) { return true; }
		for (const TPair<uint64, FAbilityInputRequestRecord>& Entry : AbilityInputRequests)
		{
			if (Entry.Key > LastSnapshotRequestSerial && Entry.Value.bHeld
				&& Entry.Value.SpecHandles.Contains(Handle)) { return true; }
		}
		return false;
	};
	TArray<FGameplayAbilitySpecHandle> Attempted;
	TArray<FGameplayAbilitySpecHandle> DeferredHandles;
	TArray<FAbilityInputEdge> DeferredEdges;
	ON_SCOPE_EXIT
	{
		if (!DeferredEdges.IsEmpty() && OriginalASC.IsValid() && ProcessingRevision == AbilityInputRevision
			&& IsAbilityInputContextCurrent(OriginalOwner, OriginalAvatar, OriginalWorld))
		{
			// Preserve earlier true edges ahead of callback-created next-frame facts.
			DeferredEdges.Append(MoveTemp(PendingAbilityInputEdges));
			PendingAbilityInputEdges = MoveTemp(DeferredEdges);
		}
	};
	const auto TryOnce = [&](FGameplayAbilitySpecHandle Handle, EAbilityActivationInputOrigin Kind,
		const TArray<FGGYGOAbilityInputRetryRequest>& Sources)
	{
		if (!CanContinue() || Attempted.Contains(Handle) || HasSupersedingInput(Handle)) { return; }
		const FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(Handle);
		const UGGYGOGameplayAbility* CDO = Spec ? Cast<UGGYGOGameplayAbility>(Spec->Ability) : nullptr;
		if (!CDO || Spec->IsActive() || Spec->PendingRemove || Spec->RemoveAfterActivation) { return; }
		const EGGYGOAbilityActivationPolicy Policy = CDO->GetActivationPolicy();
		const bool bWhileHeld = Policy == EGGYGOAbilityActivationPolicy::WhileInputActive;
		if (Policy != EGGYGOAbilityActivationPolicy::OnInputTriggered && !bWhileHeld) { return; }
		TArray<FGGYGOAbilityInputRetryRequest> CurrentSources;
		for (const FGGYGOAbilityInputRetryRequest& Request : Sources)
		{
			if (IsAbilityInputRequestCurrent(Request, Handle, Kind == EAbilityActivationInputOrigin::FiniteRetry,
				Kind == EAbilityActivationInputOrigin::FiniteRetry, bWhileHeld))
			{
				CurrentSources.Add(Request);
			}
		}
		if (CurrentSources.IsEmpty()) { return; }
		FAbilityActivationFailureOrigin Origin;
		Origin.Kind = Kind;
		Origin.Handle = Handle;
		Origin.Requests = CurrentSources;
		Origin.OwnerActor = OriginalOwner;
		Attempted.Add(Handle); // Before external calls; no second Try for the same Spec in this batch.
		// Spend the old Queued permission before native callbacks. Only a new proven failure can renew it.
		for (const FGGYGOAbilityInputRetryRequest& Request : CurrentSources)
		{
			FAbilityInputRequestRecord* Record = AbilityInputRequests.Find(Request.Identity.RequestSerial);
			if (Record && Record->Request.Identity == Request.Identity) { Record->RetryableSpecHandles.Remove(Handle); }
		}
		bool bActivated = false;
		{
			FScopedAbilityInputActivation InputScope(this, Handle, Origin);
			bActivated = TryActivateAbilityWithTerminationBoundary(Handle).bNativeAccepted;
		}
		if (!CanContinue()) { return; }
		if (bActivated) { ConsumeSuccessfulAbilityInputRequests(CurrentSources); }
	};

	for (int32 EdgeIndex = 0; EdgeIndex < Edges.Num(); ++EdgeIndex)
	{
		const FAbilityInputEdge& Edge = Edges[EdgeIndex];
		if (!CanContinue()) { return; }
		if (DeferredHandles.Contains(Edge.Handle)) { DeferredEdges.Add(Edge); continue; }
		if (HasSupersedingInput(Edge.Handle) || !IsAbilityInputEdgeCurrent(Edge)) { continue; }
		FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(Edge.Handle);
		if (!Spec || !Spec->Ability || Spec->PendingRemove || Spec->RemoveAfterActivation) { continue; }
		if (Edge.bPressed)
		{
			if (!Spec->IsActive() && Attempted.Contains(Edge.Handle))
			{
				// A later real re-press must not be lost to this batch's one-Try rule.
				DeferredHandles.Add(Edge.Handle);
				DeferredEdges.Add(Edge);
				continue;
			}
			Spec->InputPressed = true;
			if (Spec->IsActive())
			{
				AbilitySpecInputPressed(*Spec); // Real aggregate first edge, never retry.
			}
			else
			{
				TryOnce(Edge.Handle, EAbilityActivationInputOrigin::InitialPress, InitialSourcesByEdge[EdgeIndex]);
			}
		}
		else
		{
			Spec->InputPressed = false;
			if (Spec->IsActive()) { AbilitySpecInputReleased(*Spec); }
		}
	}
	// Grouped by fixed Spec, so one failed Try can carry several real retry request identities.
	TMap<FGameplayAbilitySpecHandle, TArray<FGGYGOAbilityInputRetryRequest>> RetrySources;
	TArray<FGameplayAbilitySpecHandle> RetryHandles;
	for (const FRetryWork& Work : RetryWork)
	{
		for (const FGameplayAbilitySpecHandle Handle : Work.Handles)
		{
			RetrySources.FindOrAdd(Handle).Add(Work.Request);
			RetryHandles.AddUnique(Handle);
		}
	}
	for (const FGameplayAbilitySpecHandle Handle : RetryHandles)
	{
		if (!CanContinue()) { return; }
		TryOnce(Handle, EAbilityActivationInputOrigin::FiniteRetry, RetrySources.FindChecked(Handle));
	}
	for (const FGameplayAbilitySpecHandle Handle : HeldHandles)
	{
		if (!CanContinue()) { return; }
		const FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(Handle);
		const UGGYGOGameplayAbility* CDO = Spec ? Cast<UGGYGOGameplayAbility>(Spec->Ability) : nullptr;
		if (CDO && CDO->GetActivationPolicy() == EGGYGOAbilityActivationPolicy::WhileInputActive)
		{
			TryOnce(Handle, EAbilityActivationInputOrigin::None, HeldSources.FindChecked(Handle));
		}
	}
	// Held storage is retained until real release/retirement; only these local snapshots expire here.
}

void UGGYGOAbilitySystemComponent::ClearAbilityInput()
{
	check(IsInGameThread());
	TArray<FGameplayAbilitySpecHandle> Affected = InputHeldSpecHandles;
	for (const TPair<uint64, FAbilityInputRequestRecord>& Entry : AbilityInputRequests)
	{
		for (const FGameplayAbilitySpecHandle Handle : Entry.Value.SpecHandles) { Affected.AddUnique(Handle); }
	}
	for (const FAbilityInputEdge& Edge : PendingAbilityInputEdges) { Affected.AddUnique(Edge.Handle); }
	AbilityInputRequests.Reset();
	PendingAbilityInputEdges.Reset();
	PendingAbilityInputRetries.Reset();
	InputHeldSpecHandles.Reset();
	if (AbilityInputRevision != MAX_uint64) { ++AbilityInputRevision; }
	else { ensureMsgf(false, TEXT("AbilitySystem input revision exhausted: %s"), *GetPathName()); }
	AbilityInputActivationAttempt.bClaimed = true;
	AbilityInputActivationAttempt.Origin = FAbilityActivationFailureOrigin{};
	PendingAbilityActivationFailure = FAbilityActivationEvaluationToken{};
	for (const FGameplayAbilitySpecHandle Handle : Affected)
	{
		if (FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(Handle)) { Spec->InputPressed = false; }
	}
	// No native events, delegate dispatch, retry, actor writes or restoration.
}

void UGGYGOAbilitySystemComponent::AbilitySpecInputPressed(FGameplayAbilitySpec& Spec)
{
	Super::AbilitySpecInputPressed(Spec);

	// 不用 bReplicateInputDirectly，改走 replicated event，
	// 这样 WaitInputPress 之类的 AbilityTask 才能收到。
	if (Spec.IsActive())
	{
		PRAGMA_DISABLE_DEPRECATION_WARNINGS
		const UGameplayAbility* Instance = Spec.GetPrimaryInstance();
		// 必须用激活时的原始预测键，否则服务器无法把这个事件对应到正确的那次激活。
		FPredictionKey OriginalPredictionKey = Instance ? Instance->GetCurrentActivationInfo().GetActivationPredictionKey() : Spec.ActivationInfo.GetActivationPredictionKey();
		PRAGMA_ENABLE_DEPRECATION_WARNINGS

		InvokeReplicatedEvent(EAbilityGenericReplicatedEvent::InputPressed, Spec.Handle, OriginalPredictionKey);
	}
}

void UGGYGOAbilitySystemComponent::AbilitySpecInputReleased(FGameplayAbilitySpec& Spec)
{
	Super::AbilitySpecInputReleased(Spec);

	if (Spec.IsActive())
	{
		PRAGMA_DISABLE_DEPRECATION_WARNINGS
		const UGameplayAbility* Instance = Spec.GetPrimaryInstance();
		FPredictionKey OriginalPredictionKey = Instance ? Instance->GetCurrentActivationInfo().GetActivationPredictionKey() : Spec.ActivationInfo.GetActivationPredictionKey();
		PRAGMA_ENABLE_DEPRECATION_WARNINGS

		InvokeReplicatedEvent(EAbilityGenericReplicatedEvent::InputReleased, Spec.Handle, OriginalPredictionKey);
	}
}

const FGGYGOAbilityGroupRule& UGGYGOAbilitySystemComponent::ResolveGroupRule(FGameplayTag GroupTag) const
{
	// 未注入配置表时的兜底规则。static 保证能安全返回引用。
	// 取字段默认值（SingleInstance + 平手后来者胜），使"没配表"与
	// "配了表但没配这个组"两种情况行为一致。
	static const FGGYGOAbilityGroupRule FallbackRule;

	if (AbilityGroupConfig)
	{
		return AbilityGroupConfig->GetRuleForGroup(GroupTag);
	}

	return FallbackRule;
}

void UGGYGOAbilitySystemComponent::SetAbilityGroupConfig(const UGGYGOAbilityGroupConfig* InConfig)
{
	AbilityGroupConfig = InConfig;
}

int32 UGGYGOAbilitySystemComponent::GetActiveAbilityCountInGroup(FGameplayTag GroupTag) const
{
	const TArray<TWeakObjectPtr<UGGYGOGameplayAbility>>* Group = ActiveAbilitiesByGroup.Find(GroupTag);
	if (!Group)
	{
		return 0;
	}

	// 不能直接用 Num()：表里可能残留已被 GC 的弱引用（清理只在 Remove 时做）。
	int32 Count = 0;
	for (const TWeakObjectPtr<UGGYGOGameplayAbility>& Weak : *Group)
	{
		if (Weak.IsValid())
		{
			++Count;
		}
	}

	return Count;
}

bool UGGYGOAbilitySystemComponent::IsActivationBlockedByGroup(const UGGYGOGameplayAbility* Ability) const
{
	EGGYGOAbilityGroupBlockReason UnusedReason = EGGYGOAbilityGroupBlockReason::NotBlocked;
	return IsActivationBlockedByGroup(Ability, UnusedReason);
}

bool UGGYGOAbilitySystemComponent::IsActivationBlockedByGroup(const UGGYGOGameplayAbility* Ability, EGGYGOAbilityGroupBlockReason& OutReason) const
{
	OutReason = EGGYGOAbilityGroupBlockReason::NotBlocked;

	if (!Ability)
	{
		return false;
	}

	const int32 RequestPriority = Ability->GetActivationPriority();

	// 第一步：全局 Exclusive 排斥。跨组也生效，用于死亡、被击倒、大招这类"世界静止"的能力。
	for (const TPair<FGameplayTag, TArray<TWeakObjectPtr<UGGYGOGameplayAbility>>>& GroupPair : ActiveAbilitiesByGroup)
	{
		for (const TWeakObjectPtr<UGGYGOGameplayAbility>& WeakActive : GroupPair.Value)
		{
			const UGGYGOGameplayAbility* Active = WeakActive.Get();
			if (!Active || Active == Ability || Active->IsCurrentAbilityGroupAdmissionRejected())
			{
				continue;
			}

			if (Active->GetSelfPolicy() == EGGYGOAbilitySelfPolicy::Exclusive && Active->GetActivationPriority() > RequestPriority)
			{
				OutReason = EGGYGOAbilityGroupBlockReason::ExclusiveActive;
#if WITH_DEV_AUTOMATION_TESTS
				const uint64 ActiveSequence = Active->GetCurrentAbilityGroupAdmissionSequence();
				UE_LOG(LogGGYGOAbilitySystem, Display,
					TEXT("Admission blocker reason=ExclusiveActive self=%s@%p selfSeq=%llu other=%s@%p otherSeq=%llu selfPriority=%d otherPriority=%d otherPending=%d otherRejected=%d"),
					*GetNameSafe(Ability), static_cast<const void*>(Ability),
					static_cast<unsigned long long>(Ability->GetCurrentAbilityGroupAdmissionSequence()),
					*GetNameSafe(Active), static_cast<const void*>(Active), static_cast<unsigned long long>(ActiveSequence),
					RequestPriority, Active->GetActivationPriority(), Active->IsAbilityGroupAdmissionPending() ? 1 : 0,
					Active->IsAbilityGroupAdmissionRejected(ActiveSequence) ? 1 : 0);
#endif
				return true;
			}
		}
	}

	// GroupTag 为空表示不参与组仲裁。
	const FGameplayTag GroupTag = Ability->GetGroupTag();
	if (!GroupTag.IsValid())
	{
		return false;
	}

	// 第二步：同组冲突，按该组配置的规则判定。
	const FGGYGOAbilityGroupRule& Rule = ResolveGroupRule(GroupTag);
	if (Rule.Rule == EGGYGOAbilityGroupRule::Coexist)
	{
		// 组内无并发限制，连遍历都不必做。
		return false;
	}

	const TArray<TWeakObjectPtr<UGGYGOGameplayAbility>>* Group = ActiveAbilitiesByGroup.Find(GroupTag);
	if (!Group)
	{
		return false;
	}

	for (const TWeakObjectPtr<UGGYGOGameplayAbility>& WeakActive : *Group)
	{
		const UGGYGOGameplayAbility* Active = WeakActive.Get();
		if (!Active || Active == Ability || Active->IsCurrentAbilityGroupAdmissionRejected())
		{
			continue;
		}

		if (Rule.Rule == EGGYGOAbilityGroupRule::SingleInstanceQueued)
		{
			// 严格先来后到：不比优先级，组里有人在跑就一律拒绝。
			// 配合 AddAbilityToActivationGroup 里"Queued 组不取消同组实例"，
			// 已激活的那一个必然能播完 —— 这是连段不被自己打断的保证。
			OutReason = EGGYGOAbilityGroupBlockReason::GroupOccupiedQueued;
#if WITH_DEV_AUTOMATION_TESTS
			const uint64 ActiveSequence = Active->GetCurrentAbilityGroupAdmissionSequence();
			UE_LOG(LogGGYGOAbilitySystem, Display,
				TEXT("Admission blocker reason=GroupOccupiedQueued self=%s@%p selfSeq=%llu other=%s@%p otherSeq=%llu otherPending=%d otherRejected=%d"),
				*GetNameSafe(Ability), static_cast<const void*>(Ability),
				static_cast<unsigned long long>(Ability->GetCurrentAbilityGroupAdmissionSequence()),
				*GetNameSafe(Active), static_cast<const void*>(Active), static_cast<unsigned long long>(ActiveSequence),
				Active->IsAbilityGroupAdmissionPending() ? 1 : 0,
				Active->IsAbilityGroupAdmissionRejected(ActiveSequence) ? 1 : 0);
#endif
			return true;
		}

		// SingleInstance：不可取消的旧实例保留槽位；其余情况按优先级比较。
		// bNewcomerWinsOnTie 为 true 时用 >（平手不阻断，放新的进来顶掉旧的，D4）；
		// 为 false 时用 >=（平手也阻断，先激活者守住位置，受击组要的是这个）。
		if (!Active->CanBeCanceled())
		{
			OutReason = EGGYGOAbilityGroupBlockReason::UncancelableActive;
#if WITH_DEV_AUTOMATION_TESTS
			const uint64 ActiveSequence = Active->GetCurrentAbilityGroupAdmissionSequence();
			UE_LOG(LogGGYGOAbilitySystem, Display,
				TEXT("Admission blocker reason=UncancelableActive self=%s@%p selfSeq=%llu other=%s@%p otherSeq=%llu selfPriority=%d otherPriority=%d otherPending=%d otherRejected=%d"),
				*GetNameSafe(Ability), static_cast<const void*>(Ability),
				static_cast<unsigned long long>(Ability->GetCurrentAbilityGroupAdmissionSequence()),
				*GetNameSafe(Active), static_cast<const void*>(Active), static_cast<unsigned long long>(ActiveSequence),
				RequestPriority, Active->GetActivationPriority(), Active->IsAbilityGroupAdmissionPending() ? 1 : 0,
				Active->IsAbilityGroupAdmissionRejected(ActiveSequence) ? 1 : 0);
#endif
			return true;
		}

		const int32 ActivePriority = Active->GetActivationPriority();
		const bool bBlockedByPriority = Rule.bNewcomerWinsOnTie
			? (ActivePriority > RequestPriority)
			: (ActivePriority >= RequestPriority);

		if (bBlockedByPriority)
		{
			OutReason = EGGYGOAbilityGroupBlockReason::LowerPriority;
#if WITH_DEV_AUTOMATION_TESTS
			const uint64 ActiveSequence = Active->GetCurrentAbilityGroupAdmissionSequence();
			UE_LOG(LogGGYGOAbilitySystem, Display,
				TEXT("Admission blocker reason=LowerPriority self=%s@%p selfSeq=%llu other=%s@%p otherSeq=%llu selfPriority=%d otherPriority=%d newcomerWinsOnTie=%d otherPending=%d otherRejected=%d"),
				*GetNameSafe(Ability), static_cast<const void*>(Ability),
				static_cast<unsigned long long>(Ability->GetCurrentAbilityGroupAdmissionSequence()),
				*GetNameSafe(Active), static_cast<const void*>(Active), static_cast<unsigned long long>(ActiveSequence),
				RequestPriority, ActivePriority, Rule.bNewcomerWinsOnTie ? 1 : 0,
				Active->IsAbilityGroupAdmissionPending() ? 1 : 0,
				Active->IsAbilityGroupAdmissionRejected(ActiveSequence) ? 1 : 0);
#endif
			return true;
		}
	}

	return false;
}

void UGGYGOAbilitySystemComponent::AddAbilityToActivationGroup(UGGYGOGameplayAbility* Ability)
{
	if (!Ability)
	{
		return;
	}

	const FGameplayTag GroupTag = Ability->GetGroupTag();
	// PreActivate 只登记准入预留；竞争取消留给 ActiveCount 已递增后的 Finalize。
	if (GroupTag.IsValid())
	{
		ActiveAbilitiesByGroup.FindOrAdd(GroupTag).AddUnique(Ability);
	}
}

uint64 UGGYGOAbilitySystemComponent::AllocateAbilityGroupAdmissionSequence()
{
	// Do not wrap to zero or reuse a sequence while older attempts may still be on the stack.
	if (NextAbilityGroupAdmissionSequence == MAX_uint64)
	{
		ensureMsgf(false, TEXT("Ability group admission sequence exhausted on ASC [%s]."), *GetName());
		return 0;
	}

	return ++NextAbilityGroupAdmissionSequence;
}

bool UGGYGOAbilitySystemComponent::FinalizeAbilityGroupAdmission(UGGYGOGameplayAbility* Ability, uint64 AdmissionSequence)
{
#if WITH_DEV_AUTOMATION_TESTS
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("Admission Finalize entry self=%s@%p sequence=%llu pending=%d rejected=%d"),
		*GetNameSafe(Ability), static_cast<const void*>(Ability),
		static_cast<unsigned long long>(AdmissionSequence),
		Ability && Ability->IsAbilityGroupAdmissionPending(AdmissionSequence) ? 1 : 0,
		Ability && Ability->IsAbilityGroupAdmissionRejected(AdmissionSequence) ? 1 : 0);
#endif
	if (!Ability || !Ability->IsAbilityGroupAdmissionPending(AdmissionSequence)
		|| Ability->IsAbilityGroupAdmissionRejected(AdmissionSequence))
	{
#if WITH_DEV_AUTOMATION_TESTS
		const TCHAR* ExitReason = !Ability ? TEXT("invalid-ability")
			: !Ability->IsAbilityGroupAdmissionPending(AdmissionSequence) ? TEXT("not-pending")
			: TEXT("already-rejected");
		UE_LOG(LogGGYGOAbilitySystem, Display,
			TEXT("Admission Finalize exit self=%s@%p sequence=%llu reason=%s result=false"),
			*GetNameSafe(Ability), static_cast<const void*>(Ability),
			static_cast<unsigned long long>(AdmissionSequence), ExitReason);
#endif
		return false;
	}

	const FGameplayTag GroupTag = Ability->GetGroupTag();
	const int32 NewPriority = Ability->GetActivationPriority();

	// 复用组准入的取消阈值。SingleInstanceQueued 与 Coexist 不取消同组实例；
	// Exclusive 仍按既有规则取消所有更低优先级实例。
	const bool bCancelExclusiveTargets = Ability->GetSelfPolicy() == EGGYGOAbilitySelfPolicy::Exclusive;
	const FGGYGOAbilityGroupRule* Rule = GroupTag.IsValid() ? &ResolveGroupRule(GroupTag) : nullptr;
	const bool bCancelSameGroupTargets = Rule && Rule->Rule == EGGYGOAbilityGroupRule::SingleInstance;
	const bool bCancelTies = !Rule || Rule->bNewcomerWinsOnTie;
	auto ShouldCancelFunc = [Ability, AdmissionSequence, NewPriority, GroupTag, bCancelExclusiveTargets, bCancelSameGroupTargets, bCancelTies](
		const UGGYGOGameplayAbility* Other, FGameplayAbilitySpecHandle Handle)
	{
		// 新尝试可在同步取消回调中拒绝当前 attempt；旧裁决随即停止继续取消目标。
		if (Ability->IsAbilityGroupAdmissionRejected(AdmissionSequence) || Other == Ability)
		{
			return false;
		}

		const bool bExclusiveTarget = bCancelExclusiveTargets && Other->GetActivationPriority() < NewPriority;
		const bool bSameGroupTarget = bCancelSameGroupTargets && Other->GetGroupTag() == GroupTag
			&& (bCancelTies ? Other->GetActivationPriority() <= NewPriority : Other->GetActivationPriority() < NewPriority);
		return bExclusiveTarget || bSameGroupTarget;
	};

	// Resolve conflicting pending attempts before touching settled abilities. Their order is
	// ASC-wide, so a nested newer request can reject this older attempt without the older
	// resolver later cancelling the newer instance when its callback stack resumes.
	if (bCancelExclusiveTargets || bCancelSameGroupTargets)
	{
		ABILITYLIST_SCOPE_LOCK();
		for (const FGameplayAbilitySpec& AbilitySpec : ActivatableAbilities.Items)
		{
			if (!Cast<UGGYGOGameplayAbility>(AbilitySpec.Ability))
			{
				continue;
			}

			const TArray<UGameplayAbility*> Instances = AbilitySpec.GetAbilityInstances();
			for (UGameplayAbility* AbilityInstance : Instances)
			{
				UGGYGOGameplayAbility* Other = Cast<UGGYGOGameplayAbility>(AbilityInstance);
				if (!Other || Other == Ability || !Other->IsActive() || !Other->IsAbilityGroupAdmissionPending())
				{
					continue;
				}

				const uint64 OtherSequence = Other->GetCurrentAbilityGroupAdmissionSequence();
				if (OtherSequence == 0 || Other->IsAbilityGroupAdmissionRejected(OtherSequence)
					|| !ShouldCancelFunc(Other, AbilitySpec.Handle))
				{
					continue;
				}

#if WITH_DEV_AUTOMATION_TESTS
				UE_LOG(LogGGYGOAbilitySystem, Display,
					TEXT("Admission pending conflict self=%s@%p sequence=%llu pending=%d rejected=%d other=%s@%p sequence=%llu pending=%d rejected=%d"),
					*GetNameSafe(Ability), static_cast<const void*>(Ability),
					static_cast<unsigned long long>(AdmissionSequence),
					Ability->IsAbilityGroupAdmissionPending(AdmissionSequence) ? 1 : 0,
					Ability->IsAbilityGroupAdmissionRejected(AdmissionSequence) ? 1 : 0,
					*GetNameSafe(Other), static_cast<const void*>(Other),
					static_cast<unsigned long long>(OtherSequence), Other->IsAbilityGroupAdmissionPending(OtherSequence) ? 1 : 0,
					Other->IsAbilityGroupAdmissionRejected(OtherSequence) ? 1 : 0);
#endif
				if (OtherSequence < AdmissionSequence)
				{
#if WITH_DEV_AUTOMATION_TESTS
					UE_LOG(LogGGYGOAbilitySystem, Display,
						TEXT("Admission pending action=reject-other self=%s@%p sequence=%llu other=%s@%p sequence=%llu"),
						*GetNameSafe(Ability), static_cast<const void*>(Ability),
						static_cast<unsigned long long>(AdmissionSequence),
						*GetNameSafe(Other), static_cast<const void*>(Other), static_cast<unsigned long long>(OtherSequence));
#endif
					Other->RejectAbilityGroupAdmission(OtherSequence);
				}
				else
				{
					// Equal sequences are an invariant violation; fail closed like a newer competitor.
#if WITH_DEV_AUTOMATION_TESTS
					UE_LOG(LogGGYGOAbilitySystem, Display,
						TEXT("Admission pending action=reject-self self=%s@%p sequence=%llu other=%s@%p sequence=%llu"),
						*GetNameSafe(Ability), static_cast<const void*>(Ability),
						static_cast<unsigned long long>(AdmissionSequence),
						*GetNameSafe(Other), static_cast<const void*>(Other), static_cast<unsigned long long>(OtherSequence));
#endif
					Ability->RejectAbilityGroupAdmission(AdmissionSequence);
#if WITH_DEV_AUTOMATION_TESTS
					UE_LOG(LogGGYGOAbilitySystem, Display,
						TEXT("Admission Finalize exit self=%s@%p sequence=%llu reason=pending-newer-or-equal result=false"),
						*GetNameSafe(Ability), static_cast<const void*>(Ability),
						static_cast<unsigned long long>(AdmissionSequence));
#endif
					return false;
				}
			}
		}
	}

	if (bCancelExclusiveTargets || bCancelSameGroupTargets)
	{
		CancelAbilitiesByFunc(ShouldCancelFunc, /*bReplicateCancelAbility=*/true);
#if WITH_DEV_AUTOMATION_TESTS
		UE_LOG(LogGGYGOAbilitySystem, Display,
			TEXT("Admission after CancelAbilitiesByFunc self=%s@%p sequence=%llu pending=%d rejected=%d"),
			*GetNameSafe(Ability), static_cast<const void*>(Ability),
			static_cast<unsigned long long>(AdmissionSequence),
			Ability->IsAbilityGroupAdmissionPending(AdmissionSequence) ? 1 : 0,
			Ability->IsAbilityGroupAdmissionRejected(AdmissionSequence) ? 1 : 0);
#endif
	}
	if (Ability->IsAbilityGroupAdmissionRejected(AdmissionSequence))
	{
#if WITH_DEV_AUTOMATION_TESTS
		UE_LOG(LogGGYGOAbilitySystem, Display,
			TEXT("Admission Finalize exit self=%s@%p sequence=%llu reason=rejected-after-cancel result=false"),
			*GetNameSafe(Ability), static_cast<const void*>(Ability),
			static_cast<unsigned long long>(AdmissionSequence));
#endif
		return false;
	}

	// CancelAbility 可能被 CanBeCanceled 拒绝，也可能因同步回调改变了占用。
	// 取消后检查仍活跃的竞争者，并复用同一组准入规则判断是否应拒绝当前尝试。
	if (HasActiveAbilityMatching(ShouldCancelFunc) || IsActivationBlockedByGroup(Ability))
	{
		Ability->RejectAbilityGroupAdmission(AdmissionSequence);
#if WITH_DEV_AUTOMATION_TESTS
		UE_LOG(LogGGYGOAbilitySystem, Display,
			TEXT("Admission Finalize exit self=%s@%p sequence=%llu reason=post-cancel-conflict result=false"),
			*GetNameSafe(Ability), static_cast<const void*>(Ability),
			static_cast<unsigned long long>(AdmissionSequence));
#endif
		return false;
	}

#if WITH_DEV_AUTOMATION_TESTS
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("Admission Finalize exit self=%s@%p sequence=%llu result=true"),
		*GetNameSafe(Ability), static_cast<const void*>(Ability),
		static_cast<unsigned long long>(AdmissionSequence));
#endif
	return true;
}

bool UGGYGOAbilitySystemComponent::RemoveAbilityFromActivationGroup(UGGYGOGameplayAbility* Ability, bool bBroadcastGroupFreed)
{
	if (!Ability)
	{
		return false;
	}

	const FGameplayTag GroupTag = Ability->GetGroupTag();
	if (!GroupTag.IsValid())
	{
		return false;
	}

	bool bGroupBecameEmpty = false;

	if (TArray<TWeakObjectPtr<UGGYGOGameplayAbility>>* Group = ActiveAbilitiesByGroup.Find(GroupTag))
	{
		Group->Remove(Ability);

		// 顺手清掉已经失效的弱引用，避免长期运行后表里堆积空项。
		Group->RemoveAll([](const TWeakObjectPtr<UGGYGOGameplayAbility>& Weak) { return !Weak.IsValid(); });

		if (Group->Num() == 0)
		{
			ActiveAbilitiesByGroup.Remove(GroupTag);
			bGroupBecameEmpty = true;
		}
	}

	if (bGroupBecameEmpty && bBroadcastGroupFreed)
	{
		BroadcastAbilityGroupFreedIfEmpty(GroupTag);
	}

	return bGroupBecameEmpty;
}

void UGGYGOAbilitySystemComponent::BroadcastAbilityGroupFreedIfEmpty(FGameplayTag GroupTag)
{
	if (!GroupTag.IsValid() || GetActiveAbilityCountInGroup(GroupTag) > 0)
	{
		return;
	}

	// 清掉弱引用空槽后再广播；同步订阅者可能立即激活并重新登记同组能力。
	ActiveAbilitiesByGroup.Remove(GroupTag);
	OnAbilityGroupFreed.Broadcast(GroupTag);
}

bool UGGYGOAbilitySystemComponent::HasActiveAbilityMatching(TShouldCancelAbilityFunc ShouldCancelFunc) const
{
	for (const TPair<FGameplayTag, TArray<TWeakObjectPtr<UGGYGOGameplayAbility>>>& GroupPair : ActiveAbilitiesByGroup)
	{
		for (const TWeakObjectPtr<UGGYGOGameplayAbility>& WeakAbility : GroupPair.Value)
		{
			const UGGYGOGameplayAbility* GGYGOAbility = WeakAbility.Get();
			if (!GGYGOAbility || !GGYGOAbility->IsActive() || GGYGOAbility->IsCurrentAbilityGroupAdmissionRejected())
			{
				continue;
			}

			const FGameplayAbilitySpecHandle AbilityHandle = GGYGOAbility->GetCurrentAbilitySpecHandle();
			if (ShouldCancelFunc(GGYGOAbility, AbilityHandle))
			{
#if WITH_DEV_AUTOMATION_TESTS
				const uint64 AdmissionSequence = GGYGOAbility->GetCurrentAbilityGroupAdmissionSequence();
				UE_LOG(LogGGYGOAbilitySystem, Display,
					TEXT("Admission HasActiveAbilityMatching hit other=%s@%p sequence=%llu pending=%d rejected=%d spec=%s"),
					*GetNameSafe(GGYGOAbility), static_cast<const void*>(GGYGOAbility),
					static_cast<unsigned long long>(AdmissionSequence),
					GGYGOAbility->IsAbilityGroupAdmissionPending(AdmissionSequence) ? 1 : 0,
					GGYGOAbility->IsAbilityGroupAdmissionRejected(AdmissionSequence) ? 1 : 0,
					*AbilityHandle.ToString());
#endif
				return true;
			}
		}
	}

	return false;
}

void UGGYGOAbilitySystemComponent::NotifyAbilityActivated(const FGameplayAbilitySpecHandle Handle, UGameplayAbility* Ability)
{
	if (UGGYGOGameplayAbility* GGYGOAbility = Cast<UGGYGOGameplayAbility>(Ability))
	{
		ObserveControlledAbilityActivation(Handle, GGYGOAbility);
		GGYGOAbility->BeginAbilityGroupAdmissionAttempt(AllocateAbilityGroupAdmissionSequence());
		AddAbilityToActivationGroup(GGYGOAbility);
	}

	// Register the reservation before GAS broadcasts activation; listeners can synchronously
	// request another ability while this Spec's ActiveCount is still zero.
	Super::NotifyAbilityActivated(Handle, Ability);
}

void UGGYGOAbilitySystemComponent::NotifyAbilityFailed(const FGameplayAbilitySpecHandle Handle, UGameplayAbility* Ability, const FGameplayTagContainer& FailureReason)
{
	check(IsInGameThread());
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalASC(this);
	const TWeakObjectPtr<UGameplayAbility> FailedAbility(Ability);
	// Native GAS shares its failure-tag storage across nested activation attempts.
	const FGameplayTagContainer FailureTags = FailureReason;
	FAbilityActivationFailureOrigin Origin;
	const bool bHasInputOrigin = ConsumeAbilityActivationFailureOrigin(Handle, Ability, Origin);
	if (AbilityInputActivationAttempt.Serial != 0 && AbilityInputActivationAttempt.Handle == Handle)
	{
		// Native early rejection may notify without reaching Can. Its callbacks cannot claim the permit.
		AbilityInputActivationAttempt.bClaimed = true;
	}
	Super::NotifyAbilityFailed(Handle, Ability, FailureTags);
	if (!OriginalASC.IsValid())
	{
		return;
	}

	if (bHasInputOrigin && FailedAbility.IsValid()
		&& FailureTags.HasTagExact(GGYGOGameplayTags::Ability_ActivateFail_ActivationGroupQueued)
		&& IsAbilityActivationFailureOriginCurrent(Origin))
	{
		// The list is the actual Try's frozen payload, never a scan of current Tags or requests.
		for (const FGGYGOAbilityInputRetryRequest& Request : Origin.Requests)
		{
			if (!OriginalASC.IsValid() || !FailedAbility.IsValid()) { break; }
			const FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(Handle);
			const UGGYGOGameplayAbility* CDO = Spec ? Cast<UGGYGOGameplayAbility>(Spec->Ability) : nullptr;
			if (!CDO || HasMatchingGameplayTag(TAG_GGYGO_Gameplay_AbilityInputBlocked)) { continue; }
			const bool bWhileHeld = CDO->GetActivationPolicy() == EGGYGOAbilityActivationPolicy::WhileInputActive;
			if (!IsAbilityInputRequestCurrent(Request, Handle, true,
				false, bWhileHeld)) { continue; }
			FAbilityInputRequestRecord* Record = AbilityInputRequests.Find(Request.Identity.RequestSerial);
			check(Record);
			Record->RetryableSpecHandles.AddUnique(Handle); // Permission is only this real Queued Spec.
			OnAbilityInputRetryable.Broadcast(Request);
		}
	}
	if (!OriginalASC.IsValid() || !FailedAbility.IsValid())
	{
		return;
	}

	// Keep the existing local feedback and client RPC route; neither carries input authority.
	if (APawn* Avatar = Cast<APawn>(GetAvatarActor()))
	{
		if (!Avatar->IsLocallyControlled() && FailedAbility->IsSupportedForNetworking())
		{
			ClientNotifyAbilityFailed(FailedAbility.Get(), FailureTags);
			return;
		}
	}
	HandleAbilityFailed(FailedAbility.Get(), FailureTags);
}

void UGGYGOAbilitySystemComponent::NotifyAbilityEnded(FGameplayAbilitySpecHandle Handle, UGameplayAbility* Ability, bool bWasCancelled)
{
	UGGYGOGameplayAbility* GGYGOAbility = Cast<UGGYGOGameplayAbility>(Ability);
	if (GGYGOAbility)
	{
		GGYGOAbility->ObserveOriginalNativeEnd(this, Handle);
		GGYGOAbility->RetireControlledActivationForNativeEnd(Handle);
	}
	const FGameplayTag GroupTag = GGYGOAbility ? GGYGOAbility->GetGroupTag() : FGameplayTag();
	const bool bRemovedLastAbilityInGroup = GGYGOAbility
		? RemoveAbilityFromActivationGroup(GGYGOAbility, /*bBroadcastGroupFreed=*/false)
		: false;

	// Super 减少 Spec.ActiveCount 并同步广播 OnAbilityEnded。回调可以重激活同一
	// 实例；旧登记已先移除，新的 Add 不会被本次结束覆盖。
	Super::NotifyAbilityEnded(Handle, Ability, bWasCancelled);

	if (bRemovedLastAbilityInGroup)
	{
		// 仅在 Super 回调后组仍为空才通知排队方，且此时旧 Spec 已完成 ActiveCount--。
		BroadcastAbilityGroupFreedIfEmpty(GroupTag);
	}
}

void UGGYGOAbilitySystemComponent::ApplyAbilityBlockAndCancelTags(const FGameplayTagContainer& AbilityTags, UGameplayAbility* RequestingAbility, bool bEnableBlockTags, const FGameplayTagContainer& BlockTags, bool bExecuteCancelTags, const FGameplayTagContainer& CancelTags)
{
	// 拷贝后再扩展，不能改调用方传进来的容器（那可能直接指向能力资产上的配置）。
	FGameplayTagContainer ModifiedBlockTags = BlockTags;
	FGameplayTagContainer ModifiedCancelTags = CancelTags;

	if (TagRelationshipMapping)
	{
		TagRelationshipMapping->GetAbilityTagsToBlockAndCancel(AbilityTags, &ModifiedBlockTags, &ModifiedCancelTags);
	}

	Super::ApplyAbilityBlockAndCancelTags(AbilityTags, RequestingAbility, bEnableBlockTags, ModifiedBlockTags, bExecuteCancelTags, ModifiedCancelTags);
}

void UGGYGOAbilitySystemComponent::GetAdditionalActivationTagRequirements(const FGameplayTagContainer& AbilityTags, FGameplayTagContainer& OutActivationRequired, FGameplayTagContainer& OutActivationBlocked) const
{
	if (TagRelationshipMapping)
	{
		TagRelationshipMapping->GetRequiredAndBlockedActivationTags(AbilityTags, &OutActivationRequired, &OutActivationBlocked);
	}
}

void UGGYGOAbilitySystemComponent::SetTagRelationshipMapping(UGGYGOAbilityTagRelationshipMapping* NewMapping)
{
	// 不回溯修改已经应用的阻断状态，只影响之后的激活查询。
	TagRelationshipMapping = NewMapping;
}

void UGGYGOAbilitySystemComponent::ClientNotifyAbilityFailed_Implementation(const UGameplayAbility* Ability, const FGameplayTagContainer& FailureReason)
{
	HandleAbilityFailed(Ability, FailureReason);
}

void UGGYGOAbilitySystemComponent::HandleAbilityFailed(const UGameplayAbility* Ability, const FGameplayTagContainer& FailureReason)
{
	if (const UGGYGOGameplayAbility* GGYGOAbility = Cast<const UGGYGOGameplayAbility>(Ability))
	{
		GGYGOAbility->OnAbilityFailedToActivate(FailureReason);
	}
}

void UGGYGOAbilitySystemComponent::GetAbilityTargetData(const FGameplayAbilitySpecHandle AbilityHandle, FGameplayAbilityActivationInfo ActivationInfo, FGameplayAbilityTargetDataHandle& OutTargetDataHandle)
{
	// 键是 SpecHandle + 激活预测键的组合，这样同一能力的多次预测激活不会互相串数据。
	TSharedPtr<FAbilityReplicatedDataCache> ReplicatedData = AbilityTargetDataMap.Find(FGameplayAbilitySpecHandleAndPredictionKey(AbilityHandle, ActivationInfo.GetActivationPredictionKey()));
	if (ReplicatedData.IsValid())
	{
		OutTargetDataHandle = ReplicatedData->TargetData;
	}
	// 查不到时不动输出参数，保留调用方的原值。
}

namespace
{
	bool IsGGYGOAvatarBindingInvalidationReason(EGGYGOAvatarBindingReason Reason)
	{
		switch (Reason)
		{
		case EGGYGOAvatarBindingReason::LifecycleClosed:
		case EGGYGOAvatarBindingReason::ActorInfoMismatch:
		case EGGYGOAvatarBindingReason::OperationInvalidated:
		case EGGYGOAvatarBindingReason::RequestContextExpired:
			return true;
		default:
			return false;
		}
	}

	bool IsGGYGOAvatarBindingActorInfoKind(EGGYGOAvatarBindingKind Kind)
	{
		return Kind == EGGYGOAvatarBindingKind::Init
			|| Kind == EGGYGOAvatarBindingKind::Clear
			|| Kind == EGGYGOAvatarBindingKind::Refresh;
	}
}

FGGYGOAvatarBindingContext UGGYGOAbilitySystemComponent::GetAvatarBindingContext() const
{
	check(IsInGameThread());
	return AvatarBindingContext;
}

bool UGGYGOAbilitySystemComponent::CaptureAvatarBindingActualSnapshot(
	FActualAvatarBindingActorInfoSnapshot& OutSnapshot, EGGYGOAvatarBindingReason& OutReason) const
{
	OutSnapshot = {};
	OutReason = EGGYGOAvatarBindingReason::InvalidActorInfo;
	check(IsInGameThread());
	if (!IsValid(this) || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		OutReason = EGGYGOAvatarBindingReason::InvalidASC;
		return false;
	}
	if (!AbilityActorInfo.IsValid())
	{
		return false;
	}

	FActualAvatarBindingActorInfoSnapshot Candidate;
	Candidate.Allocation = AbilityActorInfo;
	Candidate.AbilitySystemComponent = AbilityActorInfo->AbilitySystemComponent;
	Candidate.OwnerActor = AbilityActorInfo->OwnerActor;
	Candidate.AvatarActor = AbilityActorInfo->AvatarActor;
	Candidate.PlayerController = AbilityActorInfo->PlayerController;
	Candidate.SkeletalMeshComponent = AbilityActorInfo->SkeletalMeshComponent;
	Candidate.MovementComponent = AbilityActorInfo->MovementComponent;
	Candidate.ActorInfoAnimInstance = AbilityActorInfo->AnimInstance;
	Candidate.ActualAnimInstance = AbilityActorInfo->GetAnimInstance();
	Candidate.CachedOwnerActor = GetOwnerActor();
	Candidate.CachedAvatarActor = GetAvatarActor_Direct();
	Candidate.ActorInfoAffectedAnimInstanceTag = AbilityActorInfo->AffectedAnimInstanceTag;
	Candidate.ASCAffectedAnimInstanceTag = AffectedAnimInstanceTag;
	OutSnapshot = MoveTemp(Candidate);
	OutReason = EGGYGOAvatarBindingReason::None;
	return true;
}

bool UGGYGOAbilitySystemComponent::ValidateAvatarBindingActualSnapshot(
	const FActualAvatarBindingActorInfoSnapshot& Snapshot, EGGYGOAvatarBindingReason& OutReason) const
{
	return ValidateAvatarBindingActualSnapshotForPurpose(Snapshot,
		EAvatarBindingSnapshotPurpose::WorkingBinding, OutReason);
}

bool UGGYGOAbilitySystemComponent::ValidateAvatarBindingActualSnapshotForPurpose(
	const FActualAvatarBindingActorInfoSnapshot& Snapshot, EAvatarBindingSnapshotPurpose Purpose,
	EGGYGOAvatarBindingReason& OutReason) const
{
	OutReason = EGGYGOAvatarBindingReason::InvalidActorInfo;
	check(IsInGameThread());
	if (Purpose != EAvatarBindingSnapshotPurpose::WorkingBinding
		&& Purpose != EAvatarBindingSnapshotPurpose::CommittedCleanup)
	{
		OutReason = EGGYGOAvatarBindingReason::InvalidRequest;
		return false;
	}
	if (!Snapshot.Allocation.IsValid())
	{
		return false;
	}
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> Self(
		const_cast<UGGYGOAbilitySystemComponent*>(this));
	if (!Snapshot.AbilitySystemComponent.HasSameIndexAndSerialNumber(Self)
		|| !Snapshot.OwnerActor.HasSameIndexAndSerialNumber(Snapshot.CachedOwnerActor)
		|| !Snapshot.AvatarActor.HasSameIndexAndSerialNumber(Snapshot.CachedAvatarActor))
	{
		OutReason = EGGYGOAvatarBindingReason::ActorInfoMismatch;
		return false;
	}

	// A fully cleared context is a valid recorded state, not a usable Owner/Avatar.
	if (Snapshot.OwnerActor.IsExplicitlyNull())
	{
		if (!Snapshot.AvatarActor.IsExplicitlyNull() || !Snapshot.PlayerController.IsExplicitlyNull()
			|| !Snapshot.SkeletalMeshComponent.IsExplicitlyNull()
			|| !Snapshot.MovementComponent.IsExplicitlyNull()
			|| !Snapshot.ActualAnimInstance.IsExplicitlyNull())
		{
			return false;
		}
		OutReason = EGGYGOAvatarBindingReason::None;
		return true;
	}
	const AActor* Owner = Snapshot.OwnerActor.Get();
	if (!Owner)
	{
		OutReason = EGGYGOAvatarBindingReason::InvalidOwner;
		return false;
	}
	if (Purpose == EAvatarBindingSnapshotPurpose::WorkingBinding && Owner->IsActorBeingDestroyed())
	{
		OutReason = EGGYGOAvatarBindingReason::LifecycleClosed;
		return false;
	}
	if (!Snapshot.PlayerController.IsExplicitlyNull() && !Snapshot.PlayerController.IsValid())
	{
		return false;
	}
	if (Snapshot.AvatarActor.IsExplicitlyNull())
	{
		if (!Snapshot.SkeletalMeshComponent.IsExplicitlyNull()
			|| !Snapshot.MovementComponent.IsExplicitlyNull()
			|| !Snapshot.ActualAnimInstance.IsExplicitlyNull())
		{
			return false;
		}
		OutReason = EGGYGOAvatarBindingReason::None;
		return true;
	}
	const AActor* Avatar = Snapshot.AvatarActor.Get();
	if (!Avatar)
	{
		OutReason = EGGYGOAvatarBindingReason::InvalidAvatar;
		return false;
	}
	if (Purpose == EAvatarBindingSnapshotPurpose::WorkingBinding && Avatar->IsActorBeingDestroyed())
	{
		OutReason = EGGYGOAvatarBindingReason::LifecycleClosed;
		return false;
	}
	if (!Snapshot.SkeletalMeshComponent.IsExplicitlyNull())
	{
		const USkeletalMeshComponent* Mesh = Snapshot.SkeletalMeshComponent.Get();
		if (!Mesh || Mesh->GetOwner() != Avatar)
		{
			return false;
		}
	}
	if (!Snapshot.MovementComponent.IsExplicitlyNull())
	{
		const UMovementComponent* Movement = Snapshot.MovementComponent.Get();
		if (!Movement || Movement->GetOwner() != Avatar)
		{
			return false;
		}
	}
	if (!Snapshot.ActualAnimInstance.IsExplicitlyNull())
	{
		const UAnimInstance* Anim = Snapshot.ActualAnimInstance.Get();
		if (!Anim || Anim->GetOwningActor() != Avatar
			|| Anim->GetSkelMeshComponent() != Snapshot.SkeletalMeshComponent.Get())
		{
			return false;
		}
	}
	// ActorInfo::AnimInstance is retained as field evidence; GAS uses GetAnimInstance().
	// Native Clear does not clear that field, so it is not required to resolve here.
	OutReason = EGGYGOAvatarBindingReason::None;
	return true;
}

bool UGGYGOAbilitySystemComponent::CheckOriginalAbilityBindingForTermination(
	const FGGYGOAvatarBindingContext& OriginalContext,
	const FActualAvatarBindingActorInfoSnapshot& OriginalSource) const
{
	check(IsInGameThread());
	EGGYGOAvatarBindingReason Reason;
	if (!OriginalContext.HasIssuedContext()
		|| CheckAvatarBindingIdentity(OriginalContext.Binding, Reason) != EGGYGOAvatarBindingOutcome::Succeeded)
	{
		return false;
	}
	// CheckAvatarBindingIdentity has authenticated the current committed full snapshot.
	// Refresh may change Controller/Mesh/Anim/Movement, never the original allocation,
	// ASC or Owner/Avatar endpoints. No current endpoint is used to manufacture a source.
	const FActualAvatarBindingActorInfoSnapshot& Committed = AvatarBindingActorInfoSnapshot;
	return OriginalSource.Allocation.IsValid() && Committed.Allocation.IsValid()
		&& OriginalSource.Allocation.Get() == Committed.Allocation.Get()
		&& OriginalSource.AbilitySystemComponent.HasSameIndexAndSerialNumber(Committed.AbilitySystemComponent)
		&& OriginalSource.OwnerActor.HasSameIndexAndSerialNumber(Committed.OwnerActor)
		&& OriginalSource.AvatarActor.HasSameIndexAndSerialNumber(Committed.AvatarActor)
		&& OriginalSource.CachedOwnerActor.HasSameIndexAndSerialNumber(Committed.CachedOwnerActor)
		&& OriginalSource.CachedAvatarActor.HasSameIndexAndSerialNumber(Committed.CachedAvatarActor);
}

bool UGGYGOAbilitySystemComponent::HasSameAvatarBindingActualSnapshot(
	const FActualAvatarBindingActorInfoSnapshot& First,
	const FActualAvatarBindingActorInfoSnapshot& Second) const
{
	check(IsInGameThread());
	return First.Allocation.IsValid() && Second.Allocation.IsValid()
		&& First.Allocation.Get() == Second.Allocation.Get()
		&& First.AbilitySystemComponent.HasSameIndexAndSerialNumber(Second.AbilitySystemComponent)
		&& First.OwnerActor.HasSameIndexAndSerialNumber(Second.OwnerActor)
		&& First.AvatarActor.HasSameIndexAndSerialNumber(Second.AvatarActor)
		&& First.PlayerController.HasSameIndexAndSerialNumber(Second.PlayerController)
		&& First.SkeletalMeshComponent.HasSameIndexAndSerialNumber(Second.SkeletalMeshComponent)
		&& First.MovementComponent.HasSameIndexAndSerialNumber(Second.MovementComponent)
		&& First.ActorInfoAnimInstance.HasSameIndexAndSerialNumber(Second.ActorInfoAnimInstance)
		&& First.ActualAnimInstance.HasSameIndexAndSerialNumber(Second.ActualAnimInstance)
		&& First.CachedOwnerActor.HasSameIndexAndSerialNumber(Second.CachedOwnerActor)
		&& First.CachedAvatarActor.HasSameIndexAndSerialNumber(Second.CachedAvatarActor)
		&& First.ActorInfoAffectedAnimInstanceTag == Second.ActorInfoAffectedAnimInstanceTag
		&& First.ASCAffectedAnimInstanceTag == Second.ASCAffectedAnimInstanceTag;
}

EGGYGOAvatarBindingOutcome UGGYGOAbilitySystemComponent::CheckAvatarBindingIdentity(
	const FGGYGOAvatarBindingIdentity& Expected, EGGYGOAvatarBindingReason& OutReason) const
{
	OutReason = EGGYGOAvatarBindingReason::InvalidRequest;
	check(IsInGameThread());
	if (!Expected.HasIssuedIdentity())
	{
		return EGGYGOAvatarBindingOutcome::Rejected;
	}
	if (!IsValid(this) || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		OutReason = EGGYGOAvatarBindingReason::InvalidASC;
		return EGGYGOAvatarBindingOutcome::Failed;
	}
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> Self(
		const_cast<UGGYGOAbilitySystemComponent*>(this));
	if (!Expected.Issuer.HasSameIndexAndSerialNumber(Self)
		|| !AvatarBindingContext.Binding.HasSameIdentity(Expected))
	{
		OutReason = EGGYGOAvatarBindingReason::ExpectedContextMismatch;
		return EGGYGOAvatarBindingOutcome::Stale;
	}
	if (AvatarBindingIdentityState != EAvatarBindingIdentityState::Current)
	{
		OutReason = EGGYGOAvatarBindingReason::OperationInvalidated;
		return EGGYGOAvatarBindingOutcome::Stale;
	}
	FActualAvatarBindingActorInfoSnapshot Actual;
	if (!CaptureAvatarBindingActualSnapshot(Actual, OutReason)
		|| !ValidateAvatarBindingActualSnapshot(Actual, OutReason))
	{
		return OutReason == EGGYGOAvatarBindingReason::ActorInfoMismatch
			? EGGYGOAvatarBindingOutcome::Stale : EGGYGOAvatarBindingOutcome::Failed;
	}
	if (!HasSameAvatarBindingActualSnapshot(AvatarBindingActorInfoSnapshot, Actual))
	{
		OutReason = EGGYGOAvatarBindingReason::ActorInfoMismatch;
		return EGGYGOAvatarBindingOutcome::Stale;
	}
	OutReason = EGGYGOAvatarBindingReason::None;
	return EGGYGOAvatarBindingOutcome::Succeeded;
}

EGGYGOAvatarBindingOutcome UGGYGOAbilitySystemComponent::CheckAvatarBindingContext(
	const FGGYGOAvatarBindingContext& Expected, EGGYGOAvatarBindingReason& OutReason) const
{
	OutReason = EGGYGOAvatarBindingReason::InvalidRequest;
	check(IsInGameThread());
	if (!Expected.HasIssuedContext())
	{
		return EGGYGOAvatarBindingOutcome::Rejected;
	}
	if (!AvatarBindingContext.HasSameContext(Expected))
	{
		OutReason = EGGYGOAvatarBindingReason::ExpectedContextMismatch;
		return EGGYGOAvatarBindingOutcome::Stale;
	}
	return CheckAvatarBindingIdentity(Expected.Binding, OutReason);
}

bool UGGYGOAbilitySystemComponent::IsAvatarBindingNewWorkLifecycleOpen(
	EGGYGOAvatarBindingReason& OutReason) const
{
	OutReason = EGGYGOAvatarBindingReason::InvalidASC;
	check(IsInGameThread());
	if (!IsValid(this) || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		return false;
	}
	// ActorInfo may become fully cleared during cleanup; that cannot reopen its closing component host.
	const AActor* ComponentOwner = GetOwner();
	if (ComponentOwner && (!IsValid(ComponentOwner) || ComponentOwner->IsActorBeingDestroyed()))
	{
		OutReason = EGGYGOAvatarBindingReason::LifecycleClosed;
		return false;
	}
	OutReason = EGGYGOAvatarBindingReason::None;
	return true;
}

EGGYGOAvatarBindingOutcome UGGYGOAbilitySystemComponent::CheckAvatarBindingCleanupContext(
	const FGGYGOAvatarBindingContext& Expected, EGGYGOAvatarBindingReason& OutReason) const
{
	OutReason = EGGYGOAvatarBindingReason::InvalidRequest;
	check(IsInGameThread());
	if (!Expected.HasIssuedContext())
	{
		return EGGYGOAvatarBindingOutcome::Rejected;
	}
	if (!IsValid(this) || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		OutReason = EGGYGOAvatarBindingReason::InvalidASC;
		return EGGYGOAvatarBindingOutcome::Failed;
	}
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> Self(
		const_cast<UGGYGOAbilitySystemComponent*>(this));
	if (!Expected.Binding.Issuer.HasSameIndexAndSerialNumber(Self)
		|| !Expected.LastActorInfoWrite.Issuer.HasSameIndexAndSerialNumber(Self)
		|| !AvatarBindingContext.HasSameContext(Expected))
	{
		OutReason = EGGYGOAvatarBindingReason::ExpectedContextMismatch;
		return EGGYGOAvatarBindingOutcome::Stale;
	}
	if ((AvatarBindingIdentityState != EAvatarBindingIdentityState::Current
		&& AvatarBindingIdentityState != EAvatarBindingIdentityState::Revoked)
		|| !AvatarBindingActorInfoSnapshot.Allocation.IsValid())
	{
		OutReason = EGGYGOAvatarBindingReason::OperationInvalidated;
		return EGGYGOAvatarBindingOutcome::Stale;
	}
	FActualAvatarBindingActorInfoSnapshot Actual;
	if (!CaptureAvatarBindingActualSnapshot(Actual, OutReason)
		|| !ValidateAvatarBindingActualSnapshotForPurpose(Actual,
			EAvatarBindingSnapshotPurpose::CommittedCleanup, OutReason))
	{
		return OutReason == EGGYGOAvatarBindingReason::ActorInfoMismatch
			? EGGYGOAvatarBindingOutcome::Stale : EGGYGOAvatarBindingOutcome::Failed;
	}
	if (!HasSameAvatarBindingActualSnapshot(AvatarBindingActorInfoSnapshot, Actual))
	{
		OutReason = EGGYGOAvatarBindingReason::ActorInfoMismatch;
		return EGGYGOAvatarBindingOutcome::Stale;
	}
	OutReason = EGGYGOAvatarBindingReason::None;
	return EGGYGOAvatarBindingOutcome::Succeeded;
}

bool UGGYGOAbilitySystemComponent::InvalidateAvatarBinding(
	const FGGYGOAvatarBindingContext& Expected, EGGYGOAvatarBindingReason Reason,
	EGGYGOAvatarBindingReason& OutRejectionReason)
{
	OutRejectionReason = EGGYGOAvatarBindingReason::InvalidRequest;
	check(IsInGameThread());
	if (!IsGGYGOAvatarBindingInvalidationReason(Reason) || !Expected.HasIssuedContext())
	{
		return false;
	}
	if (!AvatarBindingContext.HasSameContext(Expected))
	{
		OutRejectionReason = EGGYGOAvatarBindingReason::ExpectedContextMismatch;
		return false;
	}
	AvatarBindingIdentityState = EAvatarBindingIdentityState::Revoked;
	// Logical revocation retains exact committed cleanup provenance, never work/Ready permission.
	ReleaseAvatarBindingPublicationForContext(Expected);
	// Deliberately retain the pending operation: its own ID is required to revoke it.
	OutRejectionReason = EGGYGOAvatarBindingReason::None;
	return true;
}

bool UGGYGOAbilitySystemComponent::TryReserveAvatarBindingIdentityOperation(
	const FGGYGOAvatarBindingRequest& Request, EAvatarBindingIdentityAdmission Admission,
	FGGYGOAvatarBindingOperationIdentity& OutOperation, EGGYGOAvatarBindingReason& OutReason)
{
	OutOperation = {};
	OutReason = EGGYGOAvatarBindingReason::InvalidRequest;
	check(IsInGameThread());
	if (!IsValid(this) || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		OutReason = EGGYGOAvatarBindingReason::InvalidASC;
		return false;
	}
	switch (Request.Kind)
	{
	case EGGYGOAvatarBindingKind::Init:
	case EGGYGOAvatarBindingKind::Clear:
	case EGGYGOAvatarBindingKind::Refresh:
	case EGGYGOAvatarBindingKind::CancelAbilities:
	case EGGYGOAvatarBindingKind::RemoveGameplayCues:
		break;
	default:
		return false;
	}
	if (Request.Kind == EGGYGOAvatarBindingKind::Clear)
	{
		if (Request.ClearMode != EGGYGOAvatarBindingClearMode::PreserveOwner
			&& Request.ClearMode != EGGYGOAvatarBindingClearMode::ClearActorInfo)
		{
			return false;
		}
	}
	else if (Request.ClearMode != EGGYGOAvatarBindingClearMode::None)
	{
		return false;
	}
	if (Request.Kind != EGGYGOAvatarBindingKind::Init
		&& (!Request.OwnerActor.IsExplicitlyNull() || !Request.AvatarActor.IsExplicitlyNull()))
	{
		return false;
	}
	if (!Request.IsRequestContextCurrent)
	{
		OutReason = EGGYGOAvatarBindingReason::MissingContextQuery;
		return false;
	}
	if (ActiveAvatarBindingIdentityOperation.Identity.HasIssuedIdentity())
	{
		// Only an occupied identity slot; native-window admission is not implemented here.
		OutReason = EGGYGOAvatarBindingReason::NativeWriteBusy;
		return false;
	}
	if ((Request.Kind == EGGYGOAvatarBindingKind::Init || Request.Kind == EGGYGOAvatarBindingKind::Refresh)
		&& !IsAvatarBindingNewWorkLifecycleOpen(OutReason))
	{
		return false;
	}
	switch (Admission)
	{
	case EAvatarBindingIdentityAdmission::MatchCommittedCleanupContext:
		if (!IsCommittedAvatarBindingCleanupKind(Request.Kind)) { return false; }
		if (CheckAvatarBindingCleanupContext(Request.ExpectedContext, OutReason)
			!= EGGYGOAvatarBindingOutcome::Succeeded)
		{
			return false;
		}
		break;
	case EAvatarBindingIdentityAdmission::MatchCurrentContext:
		if (CheckAvatarBindingContext(Request.ExpectedContext, OutReason)
			!= EGGYGOAvatarBindingOutcome::Succeeded)
		{
			return false;
		}
		break;
	case EAvatarBindingIdentityAdmission::BootstrapNeverCommitted:
		if (Request.Kind != EGGYGOAvatarBindingKind::Init
			|| AvatarBindingIdentityState != EAvatarBindingIdentityState::Unissued
			|| AvatarBindingContext.HasIssuedContext()
			|| Request.ExpectedContext.Binding.Serial != 0
			|| !Request.ExpectedContext.Binding.Issuer.IsExplicitlyNull()
			|| Request.ExpectedContext.LastActorInfoWrite.Serial != 0
			|| !Request.ExpectedContext.LastActorInfoWrite.Issuer.IsExplicitlyNull())
		{
			return false;
		}
		break;
	case EAvatarBindingIdentityAdmission::ReplaceRevokedContext:
		if ((Request.Kind != EGGYGOAvatarBindingKind::Init && Request.Kind != EGGYGOAvatarBindingKind::Clear)
			|| AvatarBindingIdentityState != EAvatarBindingIdentityState::Revoked
			|| !Request.ExpectedContext.HasIssuedContext()
			|| !AvatarBindingContext.HasSameContext(Request.ExpectedContext))
		{
			return false;
		}
		if (Request.Kind == EGGYGOAvatarBindingKind::Clear
			&& CheckAvatarBindingCleanupContext(Request.ExpectedContext, OutReason)
				!= EGGYGOAvatarBindingOutcome::Succeeded)
		{
			return false;
		}
		break;
	default:
		return false;
	}

	const bool bCommittedCleanup = Admission == EAvatarBindingIdentityAdmission::MatchCommittedCleanupContext
		|| (Admission == EAvatarBindingIdentityAdmission::ReplaceRevokedContext
			&& Request.Kind == EGGYGOAvatarBindingKind::Clear);
	FAvatarBindingIdentityOperation Candidate;
	Candidate.Kind = Request.Kind;
	Candidate.ClearMode = Request.ClearMode;
	Candidate.Admission = Admission;
	Candidate.BeforeContext = AvatarBindingContext;
	if (!CaptureAvatarBindingActualSnapshot(Candidate.BeforeActual, OutReason))
	{
		return false;
	}
	if (Request.Kind == EGGYGOAvatarBindingKind::Init)
	{
		Candidate.ExpectedOwnerActor = Request.OwnerActor;
		Candidate.ExpectedAvatarActor = Request.AvatarActor;
	}
	else if (Request.Kind == EGGYGOAvatarBindingKind::Clear)
	{
		if (Request.ClearMode == EGGYGOAvatarBindingClearMode::PreserveOwner)
		{
			if (!IsAvatarBindingNewWorkLifecycleOpen(OutReason)) { return false; }
			Candidate.ExpectedOwnerActor = Candidate.BeforeActual.OwnerActor;
			if (!Candidate.BeforeActual.OwnerActor.HasSameIndexAndSerialNumber(
				Candidate.BeforeActual.CachedOwnerActor))
			{
				OutReason = EGGYGOAvatarBindingReason::ActorInfoMismatch;
				return false;
			}
		}
	}
	else
	{
		Candidate.ExpectedOwnerActor = Candidate.BeforeActual.OwnerActor;
		Candidate.ExpectedAvatarActor = Candidate.BeforeActual.AvatarActor;
	}
	if (Request.Kind != EGGYGOAvatarBindingKind::Clear
		|| Request.ClearMode != EGGYGOAvatarBindingClearMode::ClearActorInfo)
	{
		const AActor* Owner = Candidate.ExpectedOwnerActor.Get();
		if (!Owner)
		{
			OutReason = EGGYGOAvatarBindingReason::InvalidOwner;
			return false;
		}
		if (Owner->IsActorBeingDestroyed()
			&& (!bCommittedCleanup || Request.Kind == EGGYGOAvatarBindingKind::Clear))
		{
			OutReason = EGGYGOAvatarBindingReason::LifecycleClosed;
			return false;
		}
	}
	if (!Candidate.ExpectedAvatarActor.IsExplicitlyNull())
	{
		const AActor* Avatar = Candidate.ExpectedAvatarActor.Get();
		if (!Avatar)
		{
			OutReason = EGGYGOAvatarBindingReason::InvalidAvatar;
			return false;
		}
		if (!bCommittedCleanup && Avatar->IsActorBeingDestroyed())
		{
			OutReason = EGGYGOAvatarBindingReason::LifecycleClosed;
			return false;
		}
	}
	if (LastIssuedAvatarBindingSerial == MAX_uint64)
	{
		OutReason = EGGYGOAvatarBindingReason::SerialExhausted;
		return false;
	}

	Candidate.Identity.Issuer = this;
	Candidate.Identity.Serial = ++LastIssuedAvatarBindingSerial;
	ActiveAvatarBindingIdentityOperation = MoveTemp(Candidate);
	OutOperation = ActiveAvatarBindingIdentityOperation.Identity;
	OutReason = EGGYGOAvatarBindingReason::None;
	return true;
}

void UGGYGOAbilitySystemComponent::DiscardActiveAvatarBindingIdentityOperation(bool bRevokeBeforeContext)
{
	check(IsInGameThread());
	if (bRevokeBeforeContext
		&& IsGGYGOAvatarBindingActorInfoKind(ActiveAvatarBindingIdentityOperation.Kind)
		&& AvatarBindingContext.HasSameContext(ActiveAvatarBindingIdentityOperation.BeforeContext))
	{
		AvatarBindingIdentityState = EAvatarBindingIdentityState::Revoked;
		// A pre-write failure keeps old cleanup provenance; actual writes already retired it.
		ReleaseAvatarBindingPublicationForContext(ActiveAvatarBindingIdentityOperation.BeforeContext);
	}
	ActiveAvatarBindingIdentityOperation = {};
}

bool UGGYGOAbilitySystemComponent::TryCommitAvatarBindingActorInfoIdentity(
	const FGGYGOAvatarBindingOperationIdentity& Operation,
	FGGYGOAvatarBindingContext& OutCommittedContext, EGGYGOAvatarBindingReason& OutReason)
{
	OutCommittedContext = {};
	OutReason = EGGYGOAvatarBindingReason::OperationInvalidated;
	check(IsInGameThread());
	if (!ActiveAvatarBindingIdentityOperation.Identity.HasSameIdentity(Operation))
	{
		return false;
	}
	const auto RejectCommit = [this, &OutReason](EGGYGOAvatarBindingReason Reason)
	{
		OutReason = Reason;
		DiscardActiveAvatarBindingIdentityOperation(/*bRevokeBeforeContext=*/true);
		return false;
	};
	const FAvatarBindingIdentityOperation& Pending = ActiveAvatarBindingIdentityOperation;
	if (!IsGGYGOAvatarBindingActorInfoKind(Pending.Kind))
	{
		return RejectCommit(EGGYGOAvatarBindingReason::InvalidRequest);
	}
	if (Pending.Admission == EAvatarBindingIdentityAdmission::BootstrapNeverCommitted)
	{
		if (AvatarBindingIdentityState != EAvatarBindingIdentityState::Unissued
			|| AvatarBindingContext.HasIssuedContext())
		{
			return RejectCommit(EGGYGOAvatarBindingReason::ExpectedContextMismatch);
		}
	}
	else if (!AvatarBindingContext.HasSameContext(Pending.BeforeContext)
		|| (Pending.Kind == EGGYGOAvatarBindingKind::Refresh
			&& AvatarBindingIdentityState != EAvatarBindingIdentityState::Current))
	{
		return RejectCommit(EGGYGOAvatarBindingReason::OperationInvalidated);
	}

	FActualAvatarBindingActorInfoSnapshot Actual;
	if (!CaptureAvatarBindingActualSnapshot(Actual, OutReason)
		|| !ValidateAvatarBindingActualSnapshot(Actual, OutReason))
	{
		return RejectCommit(OutReason);
	}
	if (Actual.Allocation.Get() != Pending.BeforeActual.Allocation.Get()
		|| !Actual.OwnerActor.HasSameIndexAndSerialNumber(Pending.ExpectedOwnerActor)
		|| !Actual.AvatarActor.HasSameIndexAndSerialNumber(Pending.ExpectedAvatarActor)
		|| ((Pending.Kind == EGGYGOAvatarBindingKind::Init || Pending.Kind == EGGYGOAvatarBindingKind::Refresh)
			&& Actual.ActorInfoAffectedAnimInstanceTag != Actual.ASCAffectedAnimInstanceTag))
	{
		return RejectCommit(EGGYGOAvatarBindingReason::ActorInfoMismatch);
	}

	FGGYGOAvatarBindingContext Committed;
	Committed.Binding = Pending.Kind == EGGYGOAvatarBindingKind::Refresh
		? AvatarBindingContext.Binding : FGGYGOAvatarBindingIdentity{};
	if (Pending.Kind != EGGYGOAvatarBindingKind::Refresh)
	{
		Committed.Binding.Issuer = this;
		Committed.Binding.Serial = Operation.Serial;
	}
	Committed.LastActorInfoWrite = Operation;
	AvatarBindingContext = Committed;
	AvatarBindingActorInfoSnapshot = MoveTemp(Actual);
	AvatarBindingIdentityState = EAvatarBindingIdentityState::Current;
	DiscardActiveAvatarBindingIdentityOperation(/*bRevokeBeforeContext=*/false);
	OutCommittedContext = Committed;
	OutReason = EGGYGOAvatarBindingReason::None;
	return true;
}

bool UGGYGOAbilitySystemComponent::TryCompleteAvatarBindingIdentityOperation(
	const FGGYGOAvatarBindingOperationIdentity& Operation, EGGYGOAvatarBindingReason& OutReason)
{
	OutReason = EGGYGOAvatarBindingReason::OperationInvalidated;
	check(IsInGameThread());
	if (!ActiveAvatarBindingIdentityOperation.Identity.HasSameIdentity(Operation))
	{
		return false;
	}
	if (ActiveAvatarBindingIdentityOperation.Kind != EGGYGOAvatarBindingKind::CancelAbilities
		&& ActiveAvatarBindingIdentityOperation.Kind != EGGYGOAvatarBindingKind::RemoveGameplayCues)
	{
		OutReason = EGGYGOAvatarBindingReason::InvalidRequest;
		DiscardActiveAvatarBindingIdentityOperation(/*bRevokeBeforeContext=*/true);
		return false;
	}
	FActualAvatarBindingActorInfoSnapshot Actual;
	if (CheckAvatarBindingCleanupContext(ActiveAvatarBindingIdentityOperation.BeforeContext, OutReason)
			!= EGGYGOAvatarBindingOutcome::Succeeded
		|| !CaptureAvatarBindingActualSnapshot(Actual, OutReason)
		|| !HasSameAvatarBindingActualSnapshot(ActiveAvatarBindingIdentityOperation.BeforeActual, Actual))
	{
		if (OutReason == EGGYGOAvatarBindingReason::None)
		{
			OutReason = EGGYGOAvatarBindingReason::ActorInfoMismatch;
		}
		DiscardActiveAvatarBindingIdentityOperation(/*bRevokeBeforeContext=*/false);
		return false;
	}
	DiscardActiveAvatarBindingIdentityOperation(/*bRevokeBeforeContext=*/false);
	OutReason = EGGYGOAvatarBindingReason::None;
	return true;
}

bool UGGYGOAbilitySystemComponent::InvalidateAvatarBindingIdentityOperation(
	const FGGYGOAvatarBindingOperationIdentity& ExpectedOperation, EGGYGOAvatarBindingReason Reason)
{
	check(IsInGameThread());
	if (!IsGGYGOAvatarBindingInvalidationReason(Reason)
		|| !ActiveAvatarBindingIdentityOperation.Identity.HasSameIdentity(ExpectedOperation))
	{
		return false;
	}
	DiscardActiveAvatarBindingIdentityOperation(/*bRevokeBeforeContext=*/true);
	return true;
}

/**
 * Immutable operation history. No allocation ownership, caller closure or separate identity source.
 * Only ASC creates and installs proof after a typed native commit; legacy writes create none.
 */
struct FGGYGOAvatarBindingPublicationReceipt::FCommitPublicationProof final
{
	FCommitPublicationProof(EGGYGOAvatarBindingKind InOriginalKind,
		const FGGYGOAvatarBindingContext& InBeforeContext,
		const TWeakObjectPtr<AActor>& InBeforeOwnerActor,
		const TWeakObjectPtr<AActor>& InBeforeAvatarActor,
		const FGGYGOAvatarBindingResult& InCommitResult,
		const FGGYGOAvatarBindingNotice& InNotice)
		: OriginalKind(InOriginalKind)
		, BeforeContext(InBeforeContext)
		, BeforeOwnerActor(InBeforeOwnerActor)
		, BeforeAvatarActor(InBeforeAvatarActor)
		, CommitResult(InCommitResult)
		, Notice(InNotice)
	{
	}

	const EGGYGOAvatarBindingKind OriginalKind;
	const FGGYGOAvatarBindingContext BeforeContext;
	const TWeakObjectPtr<AActor> BeforeOwnerActor;
	const TWeakObjectPtr<AActor> BeforeAvatarActor;
	const FGGYGOAvatarBindingResult CommitResult;
	const FGGYGOAvatarBindingNotice Notice;
};

bool FGGYGOAvatarBindingPublicationReceipt::TryGetCommittedEvidence(
	FGGYGOAvatarBindingResult& OutResult, FGGYGOAvatarBindingNotice& OutNotice) const
{
	OutResult = FGGYGOAvatarBindingResult{};
	OutNotice = FGGYGOAvatarBindingNotice{};
	check(IsInGameThread());

	if (!Proof.IsValid())
	{
		return false;
	}

	OutResult = Proof->CommitResult;
	OutNotice = Proof->Notice;
	return true;
}

uint64 UGGYGOAbilitySystemComponent::AllocateAbilityActivationOriginSerial()
{
	check(IsInGameThread());
	if (LastAbilityActivationOriginSerial == MAX_uint64)
	{
		AbilityInputActivationAttempt.bClaimed = true;
		AbilityInputActivationAttempt.Origin = FAbilityActivationFailureOrigin{};
		PendingAbilityActivationFailure = FAbilityActivationEvaluationToken{};
		ensureMsgf(false, TEXT("AbilitySystem activation origin serial exhausted: %s"), *GetPathName());
		return 0;
	}
	return ++LastAbilityActivationOriginSerial;
}

bool UGGYGOAbilitySystemComponent::IsAbilityActivationFailureOriginCurrent(
	const FAbilityActivationFailureOrigin& Origin) const
{
	check(IsInGameThread());
	if (!IsValid(this) || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) { return false; }
	if (Origin.Kind != EAbilityActivationInputOrigin::InitialPress
		&& Origin.Kind != EAbilityActivationInputOrigin::FiniteRetry) { return false; }
	if (HasMatchingGameplayTag(TAG_GGYGO_Gameplay_AbilityInputBlocked)
		|| !Origin.OwnerActor.IsValid()
		|| !Origin.OwnerActor.HasSameIndexAndSerialNumber(TWeakObjectPtr<AActor>(GetOwnerActor()))) { return false; }
	const FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(Origin.Handle);
	const UGGYGOGameplayAbility* CDO = Spec ? Cast<UGGYGOGameplayAbility>(Spec->Ability) : nullptr;
	if (!CDO) { return false; }
	const EGGYGOAbilityActivationPolicy Policy = CDO->GetActivationPolicy();
	const bool bWhileHeld = Policy == EGGYGOAbilityActivationPolicy::WhileInputActive;
	if (Policy != EGGYGOAbilityActivationPolicy::OnInputTriggered && !bWhileHeld) { return false; }
	for (const FGGYGOAbilityInputRetryRequest& Request : Origin.Requests)
	{
		if (IsAbilityInputRequestCurrent(Request, Origin.Handle, true,
			false, bWhileHeld)) { return true; }
	}
	return false;
}

UGGYGOAbilitySystemComponent::FScopedAbilityInputActivation::FScopedAbilityInputActivation(
	UGGYGOAbilitySystemComponent* InASC, FGameplayAbilitySpecHandle Handle,
	const FAbilityActivationFailureOrigin& Origin)
	: ASC(InASC)
{
	check(IsInGameThread());
	if (!IsValid(InASC) || InASC->AbilityInputActivationAttempt.Serial != 0)
	{
		return;
	}
	Serial = InASC->AllocateAbilityActivationOriginSerial();
	if (Serial == 0)
	{
		return;
	}
	FAbilityInputActivationAttempt Attempt;
	Attempt.Serial = Serial;
	Attempt.ParentEvaluationSerial = InASC->AbilityActivationEvaluations.IsEmpty()
		? 0 : InASC->AbilityActivationEvaluations.Last().Serial;
	Attempt.QueryDepth = InASC->AbilityActivationQueryDepth;
	Attempt.Handle = Handle;
	Attempt.Origin = Origin;
	InASC->PendingAbilityActivationFailure = FAbilityActivationEvaluationToken{};
	InASC->AbilityInputActivationAttempt = Attempt;
}

UGGYGOAbilitySystemComponent::FScopedAbilityInputActivation::~FScopedAbilityInputActivation()
{
	check(IsInGameThread());
	if (UGGYGOAbilitySystemComponent* OwnerASC = ASC.Get())
	{
		if (Serial != 0 && OwnerASC->AbilityInputActivationAttempt.Serial == Serial)
		{
			if (OwnerASC->PendingAbilityActivationFailure.InputAttemptSerial == Serial)
			{
				OwnerASC->PendingAbilityActivationFailure = FAbilityActivationEvaluationToken{};
			}
			OwnerASC->AbilityInputActivationAttempt = FAbilityInputActivationAttempt{};
		}
	}
}

UGGYGOAbilitySystemComponent::FScopedAbilityActivationEvaluation::FScopedAbilityActivationEvaluation(
	UGGYGOAbilitySystemComponent* InASC, const UGGYGOGameplayAbility* Ability,
	FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo)
	: ASC(InASC)
{
	check(IsInGameThread());
	if (UGGYGOAbilitySystemComponent* OwnerASC = ASC.Get())
	{
		OwnerASC->BeginAbilityActivationEvaluation(Ability, Handle, ActorInfo, Token);
	}
}

UGGYGOAbilitySystemComponent::FScopedAbilityActivationEvaluation::~FScopedAbilityActivationEvaluation()
{
	check(IsInGameThread());
	if (!bFinished)
	{
		if (UGGYGOAbilitySystemComponent* OwnerASC = ASC.Get())
		{
			OwnerASC->AbortAbilityActivationEvaluation(Token);
		}
	}
}

void UGGYGOAbilitySystemComponent::FScopedAbilityActivationEvaluation::Complete(bool bCanActivate)
{
	check(IsInGameThread());
	if (!bFinished)
	{
		bFinished = true;
		if (UGGYGOAbilitySystemComponent* OwnerASC = ASC.Get())
		{
			OwnerASC->FinishAbilityActivationEvaluation(Token, bCanActivate);
		}
	}
}

UGGYGOAbilitySystemComponent::FScopedAbilityActivationQuery::FScopedAbilityActivationQuery(
	const UGGYGOAbilitySystemComponent* InASC)
	: ASC(InASC)
{
	check(IsInGameThread());
	if (const UGGYGOAbilitySystemComponent* OwnerASC = ASC.Get())
	{
		check(OwnerASC->AbilityActivationQueryDepth != MAX_uint32);
		Depth = ++OwnerASC->AbilityActivationQueryDepth;
		OwnerASC->PendingAbilityActivationFailure = FAbilityActivationEvaluationToken{};
	}
}

UGGYGOAbilitySystemComponent::FScopedAbilityActivationQuery::~FScopedAbilityActivationQuery()
{
	check(IsInGameThread());
	if (const UGGYGOAbilitySystemComponent* OwnerASC = ASC.Get())
	{
		if (Depth != 0 && OwnerASC->AbilityActivationQueryDepth == Depth)
		{
			--OwnerASC->AbilityActivationQueryDepth;
		}
	}
}

bool UGGYGOAbilitySystemComponent::BeginAbilityActivationEvaluation(
	const UGGYGOGameplayAbility* Ability, FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, FAbilityActivationEvaluationToken& OutToken)
{
	OutToken = FAbilityActivationEvaluationToken{};
	check(IsInGameThread());
	PendingAbilityActivationFailure = FAbilityActivationEvaluationToken{};
	if (!IsValid(this) || !IsValid(Ability) || !Handle.IsValid()
		|| ActorInfo != AbilityActorInfo.Get() || !ActorInfo
		|| ActorInfo->AbilitySystemComponent.Get() != this)
	{
		return false;
	}
	const FGameplayAbilitySpec* EvaluatedSpec = FindAbilitySpecFromHandle(Handle);
	if (!EvaluatedSpec || (EvaluatedSpec->Ability.Get() != Ability
		&& EvaluatedSpec->GetPrimaryInstance() != Ability))
	{
		return false;
	}
	const uint64 EvaluationSerial = AllocateAbilityActivationOriginSerial();
	if (EvaluationSerial == 0)
	{
		return false;
	}
	FAbilityActivationEvaluationToken Frame;
	Frame.Issuer = this;
	Frame.Serial = EvaluationSerial;
	Frame.ParentSerial = AbilityActivationEvaluations.IsEmpty()
		? 0 : AbilityActivationEvaluations.Last().Serial;
	Frame.InputAttemptSerial = AbilityInputActivationAttempt.Serial;
	Frame.Ability = Ability;
	Frame.Handle = Handle;
	if (AbilityInputActivationAttempt.Serial != 0 && !AbilityInputActivationAttempt.bClaimed
		&& AbilityInputActivationAttempt.ParentEvaluationSerial == Frame.ParentSerial
		&& AbilityInputActivationAttempt.QueryDepth == AbilityActivationQueryDepth
		&& AbilityInputActivationAttempt.Handle == Handle)
	{
		// Spend before any core, cost, Blueprint or additional condition can reenter.
		AbilityInputActivationAttempt.bClaimed = true;
		if (IsAbilityActivationFailureOriginCurrent(AbilityInputActivationAttempt.Origin))
		{
			Frame.Origin = AbilityInputActivationAttempt.Origin;
		}
	}
	AbilityActivationEvaluations.Add(Frame);
	OutToken = Frame;
	return true;
}

void UGGYGOAbilitySystemComponent::FinishAbilityActivationEvaluation(
	const FAbilityActivationEvaluationToken& Token, bool bCanActivate)
{
	check(IsInGameThread());
	if (Token.Serial == 0 || !Token.Issuer.HasSameIndexAndSerialNumber(
			TWeakObjectPtr<UGGYGOAbilitySystemComponent>(this))
		|| AbilityActivationEvaluations.IsEmpty()
		|| AbilityActivationEvaluations.Last().Serial != Token.Serial)
	{
		return;
	}
	FAbilityActivationEvaluationToken Completed = AbilityActivationEvaluations.Pop(EAllowShrinking::No);
	// Only the exact top frame may close and replace its result. Parent permits stay spent.
	PendingAbilityActivationFailure = FAbilityActivationEvaluationToken{};
	if (!bCanActivate && Completed.InputAttemptSerial != 0
		&& Completed.InputAttemptSerial == AbilityInputActivationAttempt.Serial
		&& AbilityInputActivationAttempt.bClaimed
		&& IsAbilityActivationFailureOriginCurrent(Completed.Origin))
	{
		// Raw and QueryOnly results still close their own frame, but issue no input proof.
		PendingAbilityActivationFailure = Completed;
	}
}

void UGGYGOAbilitySystemComponent::AbortAbilityActivationEvaluation(
	const FAbilityActivationEvaluationToken& Token)
{
	check(IsInGameThread());
	if (Token.Serial != 0 && Token.Issuer.HasSameIndexAndSerialNumber(
			TWeakObjectPtr<UGGYGOAbilitySystemComponent>(this))
		&& !AbilityActivationEvaluations.IsEmpty()
		&& AbilityActivationEvaluations.Last().Serial == Token.Serial)
	{
		AbilityActivationEvaluations.Pop(EAllowShrinking::No);
		if (PendingAbilityActivationFailure.Serial == Token.Serial)
		{
			PendingAbilityActivationFailure = FAbilityActivationEvaluationToken{};
		}
	}
}

bool UGGYGOAbilitySystemComponent::ConsumeAbilityActivationFailureOrigin(
	FGameplayAbilitySpecHandle Handle, const UGameplayAbility* Ability,
	FAbilityActivationFailureOrigin& OutOrigin)
{
	OutOrigin = FAbilityActivationFailureOrigin{};
	check(IsInGameThread());
	const FAbilityActivationEvaluationToken Completed = PendingAbilityActivationFailure;
	if (Completed.Serial == 0 || Completed.Handle != Handle
		|| !Completed.Ability.HasSameIndexAndSerialNumber(
			TWeakObjectPtr<const UGGYGOGameplayAbility>(Cast<UGGYGOGameplayAbility>(Ability))))
	{
		return false;
	}
	// Take this exact result before the first external notification; never restore it.
	PendingAbilityActivationFailure = FAbilityActivationEvaluationToken{};
	const uint64 ParentSerial = AbilityActivationEvaluations.IsEmpty()
		? 0 : AbilityActivationEvaluations.Last().Serial;
	if (!IsValid(Ability) || !Completed.Issuer.HasSameIndexAndSerialNumber(
			TWeakObjectPtr<UGGYGOAbilitySystemComponent>(this))
		|| Completed.ParentSerial != ParentSerial || Completed.InputAttemptSerial == 0
		|| Completed.InputAttemptSerial != AbilityInputActivationAttempt.Serial
		|| !AbilityInputActivationAttempt.bClaimed
		|| !IsAbilityActivationFailureOriginCurrent(Completed.Origin))
	{
		return false;
	}
	const FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(Handle);
	if (!Spec || Spec->PendingRemove || Spec->RemoveAfterActivation
		|| (Spec->Ability.Get() != Ability && Spec->GetPrimaryInstance() != Ability)
		|| Completed.Origin.Handle != Handle)
	{
		return false;
	}
	OutOrigin = Completed.Origin;
	return true;
}

UGGYGOAbilitySystemComponent::FScopedAvatarBindingNativeWrite::FScopedAvatarBindingNativeWrite(
	UGGYGOAbilitySystemComponent* InASC, const FGGYGOAvatarBindingOperationIdentity& InOperation)
	: ASC(InASC), Operation(InOperation)
{
	check(IsInGameThread());
	if (InASC && !InASC->bAvatarBindingNativeWriteBusy)
	{
		InASC->bAvatarBindingNativeWriteBusy = true;
		bEntered = true;
	}
}

UGGYGOAbilitySystemComponent::FScopedAvatarBindingNativeWrite::~FScopedAvatarBindingNativeWrite()
{
	check(IsInGameThread());
	// Cleanup only: retained weak identity may resolve a still allocated closing issuer.
	// This never grants execution permission or keeps the issuer alive.
	if (UGGYGOAbilitySystemComponent* OwnerASC = ASC.GetEvenIfUnreachable())
	{
		if (Operation.HasIssuedIdentity())
		{
			// Commit already discarded this operation; failure discards only this exact ID.
			OwnerASC->InvalidateAvatarBindingIdentityOperation(Operation,
				EGGYGOAvatarBindingReason::OperationInvalidated);
		}
		if (bEntered)
		{
			OwnerASC->bAvatarBindingNativeWriteBusy = false;
		}
	}
}

bool UGGYGOAbilitySystemComponent::IsAvatarBindingNativeWriteBusy() const
{
	check(IsInGameThread());
	return bAvatarBindingNativeWriteBusy;
}

void UGGYGOAbilitySystemComponent::ReleaseAvatarBindingPublicationForContext(
	const FGGYGOAvatarBindingContext& Expected)
{
	check(IsInGameThread());
	if (AvatarBindingPublicationRecord.Context.HasSameContext(Expected))
	{
		// This also closes successful Consumed metadata after its strong proof was released.
		AvatarBindingPublicationRecord.Publication = FGGYGOAvatarBindingPublicationReceipt{};
		AvatarBindingPublicationRecord.Phase = EAvatarBindingPublicationPhase::Closed;
	}
}

void UGGYGOAbilitySystemComponent::InvalidateAvatarBindingForLegacyActorInfoWrite()
{
	check(IsInGameThread());
	const FGGYGOAvatarBindingOperationIdentity Operation = ActiveAvatarBindingIdentityOperation.Identity;
	const FGGYGOAvatarBindingContext Before = AvatarBindingContext;
	if (Operation.HasIssuedIdentity())
	{
		InvalidateAvatarBindingIdentityOperation(Operation, EGGYGOAvatarBindingReason::OperationInvalidated);
	}
	if (Before.HasIssuedContext())
	{
		EGGYGOAvatarBindingReason Rejection;
		InvalidateAvatarBinding(Before, EGGYGOAvatarBindingReason::OperationInvalidated, Rejection);
	}
	// This admitted legacy writer creates no commit proof, even if it restores the same endpoints.
	RetireFailedAvatarActorInfoInitCleanup();
	AvatarBindingActorInfoSnapshot = {};
}

void UGGYGOAbilitySystemComponent::LogLegacyAvatarActorInfoWriteRejected(
	const TCHAR* Entry, EGGYGOAvatarBindingReason Reason) const
{
	check(IsInGameThread());
	const TCHAR* ReasonName = Reason == EGGYGOAvatarBindingReason::NativeWriteBusy
		? TEXT("NativeWriteBusy") : Reason == EGGYGOAvatarBindingReason::InvalidOwner
		? TEXT("InvalidOwner") : Reason == EGGYGOAvatarBindingReason::InvalidAvatar
		? TEXT("InvalidAvatar") : Reason == EGGYGOAvatarBindingReason::InvalidASC
		? TEXT("InvalidASC") : Reason == EGGYGOAvatarBindingReason::LifecycleClosed
		? TEXT("LifecycleClosed") : TEXT("InvalidActorInfo");
	UE_LOG(LogGGYGOAbilitySystem, Error,
		TEXT("AbilitySystem ActorInfo write rejected: ASC=%s Entry=%s CurrentOwner=%s CurrentAvatar=%s Reason=%s"),
		*GetPathNameSafe(this), Entry, *GetPathNameSafe(GetOwnerActor()),
		*GetPathNameSafe(GetAvatarActor_Direct()), ReasonName);
}

FGGYGOAvatarBindingResult UGGYGOAbilitySystemComponent::MakeAvatarBindingExecutionFailure(
	FGGYGOAvatarBindingResult Result, EGGYGOAvatarBindingReason Reason)
{
	check(IsInGameThread());
	Result.Reason = Reason;
	switch (Reason)
	{
	case EGGYGOAvatarBindingReason::InvalidRequest:
	case EGGYGOAvatarBindingReason::MissingContextQuery:
	case EGGYGOAvatarBindingReason::InvalidPublication:
	case EGGYGOAvatarBindingReason::PublicationAlreadyConsumed:
		Result.Outcome = EGGYGOAvatarBindingOutcome::Rejected;
		break;
	case EGGYGOAvatarBindingReason::NativeWriteBusy:
	case EGGYGOAvatarBindingReason::PublicationInProgress:
		Result.Outcome = EGGYGOAvatarBindingOutcome::Busy;
		break;
	case EGGYGOAvatarBindingReason::ExpectedContextMismatch:
	case EGGYGOAvatarBindingReason::RequestContextExpired:
	case EGGYGOAvatarBindingReason::ActorInfoMismatch:
	case EGGYGOAvatarBindingReason::OperationInvalidated:
		Result.Outcome = EGGYGOAvatarBindingOutcome::Stale;
		break;
	default:
		Result.Outcome = EGGYGOAvatarBindingOutcome::Failed;
		break;
	}
	return Result;
}

bool UGGYGOAbilitySystemComponent::RecheckAvatarBindingExecutionOperation(
	const FGGYGOAvatarBindingOperationIdentity& Operation, bool bCheckBeforeActual,
	EGGYGOAvatarBindingReason& OutReason) const
{
	OutReason = EGGYGOAvatarBindingReason::OperationInvalidated;
	check(IsInGameThread());
	if (!IsValid(this) || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		OutReason = EGGYGOAvatarBindingReason::InvalidASC;
		return false;
	}
	if (!ActiveAvatarBindingIdentityOperation.Identity.HasSameIdentity(Operation))
	{
		return false;
	}
	const FAvatarBindingIdentityOperation& Pending = ActiveAvatarBindingIdentityOperation;
	const bool bCommittedCleanup = Pending.Admission == EAvatarBindingIdentityAdmission::MatchCommittedCleanupContext
		|| (Pending.Admission == EAvatarBindingIdentityAdmission::ReplaceRevokedContext
			&& Pending.Kind == EGGYGOAvatarBindingKind::Clear);
	// Cleanup admission is sealed to original committed-resource kinds, never Init/Refresh.
	if (bCommittedCleanup && (!IsCommittedAvatarBindingCleanupKind(Pending.Kind)
		|| (bCheckBeforeActual && !HasSameAvatarBindingActualSnapshot(
			AvatarBindingActorInfoSnapshot, Pending.BeforeActual))))
	{
		return false;
	}
	// A retained operation and equal context do not restore explicitly revoked work admission.
	switch (Pending.Admission)
	{
	case EAvatarBindingIdentityAdmission::MatchCommittedCleanupContext:
		if ((AvatarBindingIdentityState != EAvatarBindingIdentityState::Current
			&& AvatarBindingIdentityState != EAvatarBindingIdentityState::Revoked)
			|| !AvatarBindingContext.HasSameContext(Pending.BeforeContext))
		{
			return false;
		}
		break;
	case EAvatarBindingIdentityAdmission::MatchCurrentContext:
		if (AvatarBindingIdentityState != EAvatarBindingIdentityState::Current
			|| !AvatarBindingContext.HasSameContext(Pending.BeforeContext))
		{
			return false;
		}
		break;
	case EAvatarBindingIdentityAdmission::BootstrapNeverCommitted:
		if (AvatarBindingIdentityState != EAvatarBindingIdentityState::Unissued
			|| Pending.BeforeContext.Binding.Serial != 0
			|| !Pending.BeforeContext.Binding.Issuer.IsExplicitlyNull()
			|| Pending.BeforeContext.LastActorInfoWrite.Serial != 0
			|| !Pending.BeforeContext.LastActorInfoWrite.Issuer.IsExplicitlyNull()
			|| AvatarBindingContext.Binding.Serial != 0
			|| !AvatarBindingContext.Binding.Issuer.IsExplicitlyNull()
			|| AvatarBindingContext.LastActorInfoWrite.Serial != 0
			|| !AvatarBindingContext.LastActorInfoWrite.Issuer.IsExplicitlyNull())
		{
			return false;
		}
		break;
	case EAvatarBindingIdentityAdmission::ReplaceRevokedContext:
		if (AvatarBindingIdentityState != EAvatarBindingIdentityState::Revoked
			|| !AvatarBindingContext.HasSameContext(Pending.BeforeContext))
		{
			return false;
		}
		break;
	default:
		return false;
	}
	if ((Pending.Kind == EGGYGOAvatarBindingKind::Init || Pending.Kind == EGGYGOAvatarBindingKind::Refresh
		|| (Pending.Kind == EGGYGOAvatarBindingKind::Clear
			&& Pending.ClearMode == EGGYGOAvatarBindingClearMode::PreserveOwner))
		&& !IsAvatarBindingNewWorkLifecycleOpen(OutReason))
	{
		return false;
	}
	if (Pending.Kind != EGGYGOAvatarBindingKind::Clear
		|| Pending.ClearMode != EGGYGOAvatarBindingClearMode::ClearActorInfo)
	{
		const AActor* Owner = Pending.ExpectedOwnerActor.Get();
		if (!Owner || (Owner->IsActorBeingDestroyed()
			&& (!bCommittedCleanup || Pending.Kind == EGGYGOAvatarBindingKind::Clear)))
		{
			OutReason = Owner ? EGGYGOAvatarBindingReason::LifecycleClosed
				: EGGYGOAvatarBindingReason::InvalidOwner;
			return false;
		}
	}
	if (!Pending.ExpectedAvatarActor.IsExplicitlyNull())
	{
		const AActor* Avatar = Pending.ExpectedAvatarActor.Get();
		if (!Avatar || (!bCommittedCleanup && Avatar->IsActorBeingDestroyed()))
		{
			OutReason = Avatar ? EGGYGOAvatarBindingReason::LifecycleClosed
				: EGGYGOAvatarBindingReason::InvalidAvatar;
			return false;
		}
	}
	if (bCheckBeforeActual)
	{
		FActualAvatarBindingActorInfoSnapshot Actual;
		if (!CaptureAvatarBindingActualSnapshot(Actual, OutReason))
		{
			return false;
		}
		if (!HasSameAvatarBindingActualSnapshot(Pending.BeforeActual, Actual))
		{
			OutReason = EGGYGOAvatarBindingReason::ActorInfoMismatch;
			return false;
		}
	}
	OutReason = EGGYGOAvatarBindingReason::None;
	return true;
}

TSharedPtr<const UGGYGOAbilitySystemComponent::FFailedAvatarActorInfoInitCleanupProof>
UGGYGOAbilitySystemComponent::CaptureReturnedAvatarActorInfoInitCleanup(
	const FAvatarBindingIdentityOperation& Original, EGGYGOAvatarBindingReason& OutReason) const
{
	OutReason = EGGYGOAvatarBindingReason::OperationInvalidated;
	check(IsInGameThread());
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> Self(
		const_cast<UGGYGOAbilitySystemComponent*>(this));
	if (!IsAvatarBindingNativeWriteBusy() || Original.Kind != EGGYGOAvatarBindingKind::Init
		|| !Original.Identity.HasIssuedIdentity()
		|| !Original.Identity.Issuer.HasSameIndexAndSerialNumber(Self)
		|| !ActiveAvatarBindingIdentityOperation.Identity.HasSameIdentity(Original.Identity)
		|| ActiveAvatarBindingIdentityOperation.Kind != EGGYGOAvatarBindingKind::Init)
	{
		return {};
	}
	FActualAvatarBindingActorInfoSnapshot Written;
	if (!CaptureAvatarBindingActualSnapshot(Written, OutReason))
	{
		return {};
	}
	// Invalid dependent fields may be why commit fails. Certify this writer, not work readiness.
	if (Written.Allocation.Get() != Original.BeforeActual.Allocation.Get()
		|| !Written.AbilitySystemComponent.HasSameIndexAndSerialNumber(Self)
		|| !Written.OwnerActor.HasSameIndexAndSerialNumber(Original.ExpectedOwnerActor)
		|| !Written.AvatarActor.HasSameIndexAndSerialNumber(Original.ExpectedAvatarActor)
		|| !Written.CachedOwnerActor.HasSameIndexAndSerialNumber(Written.OwnerActor)
		|| !Written.CachedAvatarActor.HasSameIndexAndSerialNumber(Written.AvatarActor))
	{
		OutReason = EGGYGOAvatarBindingReason::ActorInfoMismatch;
		return {};
	}
	const TSharedRef<FFailedAvatarActorInfoInitCleanupProof> Proof =
		MakeShared<FFailedAvatarActorInfoInitCleanupProof>();
	Proof->OriginalOperation = Original.Identity;
	Proof->Before = Original.BeforeContext;
	Proof->WrittenActual = MoveTemp(Written);
	OutReason = EGGYGOAvatarBindingReason::None;
	return Proof;
}

bool UGGYGOAbilitySystemComponent::ValidateFailedAvatarActorInfoInitCleanupSource(
	const TSharedPtr<const FFailedAvatarActorInfoInitCleanupProof>& Original,
	EGGYGOAvatarBindingReason& OutReason) const
{
	OutReason = EGGYGOAvatarBindingReason::OperationInvalidated;
	check(IsInGameThread());
	if (!IsValid(this) || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		OutReason = EGGYGOAvatarBindingReason::InvalidASC;
		return false;
	}
	if (!Original.IsValid())
	{
		return false;
	}
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> Self(
		const_cast<UGGYGOAbilitySystemComponent*>(this));
	if (!Original->OriginalOperation.HasIssuedIdentity()
		|| !Original->OriginalOperation.Issuer.HasSameIndexAndSerialNumber(Self)
		|| !Original->WrittenActual.Allocation.IsValid()
		|| !Original->WrittenActual.AbilitySystemComponent.HasSameIndexAndSerialNumber(Self))
	{
		return false;
	}
	if (AvatarBindingContext.LastActorInfoWrite.HasSameIdentity(Original->OriginalOperation)
		|| !HasSameAvatarBindingContextValue(AvatarBindingContext, Original->Before)
		|| (ActiveAvatarBindingIdentityOperation.Identity.HasIssuedIdentity()
			&& !ActiveAvatarBindingIdentityOperation.Identity.HasSameIdentity(Original->OriginalOperation)))
	{
		return false;
	}
	FActualAvatarBindingActorInfoSnapshot Actual;
	if (!CaptureAvatarBindingActualSnapshot(Actual, OutReason))
	{
		return false;
	}
	if (!HasSameAvatarBindingActualSnapshot(Original->WrittenActual, Actual))
	{
		OutReason = EGGYGOAvatarBindingReason::ActorInfoMismatch;
		return false;
	}
	OutReason = EGGYGOAvatarBindingReason::None;
	return true;
}

bool UGGYGOAbilitySystemComponent::RetainFailedAvatarActorInfoInitCleanup(
	const TSharedPtr<const FFailedAvatarActorInfoInitCleanupProof>& Original,
	EGGYGOAvatarBindingReason& OutReason)
{
	check(IsInGameThread());
	if (!ValidateFailedAvatarActorInfoInitCleanupSource(Original, OutReason))
	{
		return false;
	}
	FailedAvatarActorInfoInitCleanupProof = Original;
	return true;
}

void UGGYGOAbilitySystemComponent::RetireFailedAvatarActorInfoInitCleanup()
{
	check(IsInGameThread());
	FailedAvatarActorInfoInitCleanupProof.Reset();
}

FGGYGOAvatarBindingResult UGGYGOAbilitySystemComponent::TryCleanupFailedAvatarActorInfoInit(
	const FGGYGOAvatarBindingOperationIdentity& OriginalOperation,
	TFunction<bool()> IsOriginalCallerCurrent)
{
	FGGYGOAvatarBindingResult Result;
	check(IsInGameThread());
	if (!IsOriginalCallerCurrent)
	{
		return MakeAvatarBindingExecutionFailure(Result, EGGYGOAvatarBindingReason::MissingContextQuery);
	}
	if (!OriginalOperation.HasIssuedIdentity()) { return Result; }
	Result.Operation = OriginalOperation;
	if (!IsValid(this) || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		return MakeAvatarBindingExecutionFailure(Result, EGGYGOAvatarBindingReason::InvalidASC);
	}
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalASC(this);
	if (!OriginalOperation.Issuer.HasSameIndexAndSerialNumber(OriginalASC))
	{
		return MakeAvatarBindingExecutionFailure(Result, EGGYGOAvatarBindingReason::ExpectedContextMismatch);
	}
	if (IsAvatarBindingNativeWriteBusy() || AvatarSwitchAbilityExitScope
		|| ActiveAvatarBindingIdentityOperation.Identity.HasIssuedIdentity())
	{
		return MakeAvatarBindingExecutionFailure(Result, EGGYGOAvatarBindingReason::NativeWriteBusy);
	}
	const TSharedPtr<const FFailedAvatarActorInfoInitCleanupProof> Original =
		FailedAvatarActorInfoInitCleanupProof;
	if (!Original.IsValid() || !Original->OriginalOperation.HasSameIdentity(OriginalOperation))
	{
		return MakeAvatarBindingExecutionFailure(Result, EGGYGOAvatarBindingReason::OperationInvalidated);
	}
	Result.Before = Original->Before;
	EGGYGOAvatarBindingReason Reason;
	if (!ValidateFailedAvatarActorInfoInitCleanupSource(Original, Reason))
	{
		return MakeAvatarBindingExecutionFailure(Result, Reason);
	}
	FScopedAvatarBindingNativeWrite NativeWrite(this, OriginalOperation);
	if (!NativeWrite.HasEntered())
	{
		return MakeAvatarBindingExecutionFailure(Result, EGGYGOAvatarBindingReason::NativeWriteBusy);
	}
	const auto CheckOriginal = [&]()
	{
		UGGYGOAbilitySystemComponent* LiveASC = OriginalASC.Get();
		if (!LiveASC)
		{
			Reason = EGGYGOAvatarBindingReason::InvalidASC;
			return false;
		}
		if (LiveASC->FailedAvatarActorInfoInitCleanupProof != Original)
		{
			Reason = EGGYGOAvatarBindingReason::OperationInvalidated;
			return false;
		}
		if (LiveASC->ActiveAvatarBindingIdentityOperation.Identity.HasIssuedIdentity())
		{
			Reason = EGGYGOAvatarBindingReason::NativeWriteBusy;
			return false;
		}
		return LiveASC->ValidateFailedAvatarActorInfoInitCleanupSource(Original, Reason);
	};
	const auto RecheckOriginalCaller = [&]()
	{
		if (!CheckOriginal()) { return false; }
		const bool bCallerCurrent = IsOriginalCallerCurrent();
		if (!CheckOriginal()) { return false; }
		if (!bCallerCurrent)
		{
			Reason = EGGYGOAvatarBindingReason::RequestContextExpired;
			return false;
		}
		return true;
	};
	if (!RecheckOriginalCaller()) { return MakeAvatarBindingExecutionFailure(Result, Reason); }

	// Exact native Clear postcondition; retained AnimInstance/tag fields are not cleared by GAS.
	FActualAvatarBindingActorInfoSnapshot Cleared = Original->WrittenActual;
	Cleared.OwnerActor.Reset();
	Cleared.AvatarActor.Reset();
	Cleared.PlayerController.Reset();
	Cleared.SkeletalMeshComponent.Reset();
	Cleared.MovementComponent.Reset();
	Cleared.ActualAnimInstance.Reset();
	Cleared.CachedOwnerActor.Reset();
	Cleared.CachedAvatarActor.Reset();
	ReleaseAvatarBindingPublicationForContext(Original->Before);
	RetireFailedAvatarActorInfoInitCleanup(); // Consume before native callbacks; never reissue on failure.
	AvatarBindingActorInfoSnapshot = {};
	RetireMontagePlaybackOwnership();
	Super::ClearActorInfo();

	const auto CheckCleared = [&]()
	{
		UGGYGOAbilitySystemComponent* LiveASC = OriginalASC.Get();
		if (!LiveASC || !IsValid(LiveASC) || LiveASC->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
		{
			Reason = EGGYGOAvatarBindingReason::InvalidASC;
			return false;
		}
		if (LiveASC->FailedAvatarActorInfoInitCleanupProof.IsValid()
			|| LiveASC->ActiveAvatarBindingIdentityOperation.Identity.HasIssuedIdentity()
			|| !HasSameAvatarBindingContextValue(LiveASC->AvatarBindingContext, Original->Before))
		{
			Reason = EGGYGOAvatarBindingReason::OperationInvalidated;
			return false;
		}
		FActualAvatarBindingActorInfoSnapshot Actual;
		if (!LiveASC->CaptureAvatarBindingActualSnapshot(Actual, Reason)) { return false; }
		if (!LiveASC->HasSameAvatarBindingActualSnapshot(Cleared, Actual))
		{
			Reason = EGGYGOAvatarBindingReason::ActorInfoMismatch;
			return false;
		}
		Reason = EGGYGOAvatarBindingReason::None;
		return true;
	};
	if (!CheckCleared()) { return MakeAvatarBindingExecutionFailure(Result, Reason); }
	const bool bCallerCurrent = IsOriginalCallerCurrent();
	if (!CheckCleared()) { return MakeAvatarBindingExecutionFailure(Result, Reason); }
	if (!bCallerCurrent)
	{
		return MakeAvatarBindingExecutionFailure(Result, EGGYGOAvatarBindingReason::RequestContextExpired);
	}
	Result.Outcome = EGGYGOAvatarBindingOutcome::Succeeded;
	Result.Reason = EGGYGOAvatarBindingReason::None;
	// Physical cleanup only: original Init failure, binding metadata and publication remain unchanged.
	return Result;
}

FGGYGOAvatarBindingResult UGGYGOAbilitySystemComponent::TryExecuteAvatarActorInfoTransaction(
	const FGGYGOAvatarBindingRequest& Request, FGGYGOAvatarBindingPublicationReceipt& OutPublication)
{
	OutPublication = FGGYGOAvatarBindingPublicationReceipt{};
	return ExecuteAvatarActorInfoTransaction(Request,
		Request.Kind == EGGYGOAvatarBindingKind::Clear
			? EAvatarBindingIdentityAdmission::MatchCommittedCleanupContext
			: EAvatarBindingIdentityAdmission::MatchCurrentContext, OutPublication);
}

FGGYGOAvatarBindingResult UGGYGOAbilitySystemComponent::TryBootstrapAvatarActorInfoTransaction(
	const FGGYGOAvatarBindingRequest& Request, FGGYGOAvatarBindingPublicationReceipt& OutPublication)
{
	OutPublication = FGGYGOAvatarBindingPublicationReceipt{};
	return ExecuteAvatarActorInfoTransaction(Request,
		EAvatarBindingIdentityAdmission::BootstrapNeverCommitted, OutPublication);
}

FGGYGOAvatarBindingResult UGGYGOAbilitySystemComponent::TryReplaceRevokedAvatarActorInfoTransaction(
	const FGGYGOAvatarBindingRequest& Request, FGGYGOAvatarBindingPublicationReceipt& OutPublication)
{
	OutPublication = FGGYGOAvatarBindingPublicationReceipt{};
	return ExecuteAvatarActorInfoTransaction(Request,
		EAvatarBindingIdentityAdmission::ReplaceRevokedContext, OutPublication);
}

FGGYGOAvatarBindingResult UGGYGOAbilitySystemComponent::ExecuteAvatarActorInfoTransaction(
	const FGGYGOAvatarBindingRequest& Request, EAvatarBindingIdentityAdmission Admission,
	FGGYGOAvatarBindingPublicationReceipt& OutPublication)
{
	OutPublication = FGGYGOAvatarBindingPublicationReceipt{};
	FGGYGOAvatarBindingResult Result;
	check(IsInGameThread());
	if (IsAvatarBindingNativeWriteBusy() || AvatarSwitchAbilityExitScope)
	{
		return MakeAvatarBindingExecutionFailure(Result, EGGYGOAvatarBindingReason::NativeWriteBusy);
	}
	if (!IsGGYGOAvatarBindingActorInfoKind(Request.Kind))
	{
		return Result;
	}
	FGGYGOAvatarBindingOperationIdentity Operation;
	EGGYGOAvatarBindingReason Reason;
	if (!TryReserveAvatarBindingIdentityOperation(Request, Admission, Operation, Reason))
	{
		return MakeAvatarBindingExecutionFailure(Result, Reason);
	}
	Result.Operation = Operation;
	Result.Before = ActiveAvatarBindingIdentityOperation.BeforeContext;
	const FAvatarBindingIdentityOperation OriginalOperationRecord = ActiveAvatarBindingIdentityOperation;
	const FString OriginalASCName = GetPathNameSafe(this);
	// Reserve owns these values. Keep operation-local history before Commit clears its slot.
	const EGGYGOAvatarBindingKind OriginalKind = ActiveAvatarBindingIdentityOperation.Kind;
	const EGGYGOAvatarBindingClearMode ClearMode = ActiveAvatarBindingIdentityOperation.ClearMode;
	const TWeakObjectPtr<AActor> BeforeOwner = ActiveAvatarBindingIdentityOperation.BeforeActual.OwnerActor;
	const TWeakObjectPtr<AActor> BeforeAvatar = ActiveAvatarBindingIdentityOperation.BeforeActual.AvatarActor;
	const TWeakObjectPtr<AActor> AfterOwner = ActiveAvatarBindingIdentityOperation.ExpectedOwnerActor;
	const TWeakObjectPtr<AActor> AfterAvatar = ActiveAvatarBindingIdentityOperation.ExpectedAvatarActor;
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalASC(this);
	FScopedAvatarBindingNativeWrite NativeWrite(this, Operation);
	if (!NativeWrite.HasEntered())
	{
		return MakeAvatarBindingExecutionFailure(Result, EGGYGOAvatarBindingReason::NativeWriteBusy);
	}
	bool bNativeInitReturned = false;
	TSharedPtr<const FFailedAvatarActorInfoInitCleanupProof> ReturnedInitCleanup;
	EGGYGOAvatarBindingReason InitCleanupProofReason = EGGYGOAvatarBindingReason::InvalidActorInfo;
	const auto ReturnFailure = [&](EGGYGOAvatarBindingReason FailureReason)
	{
		if (bNativeInitReturned)
		{
			UGGYGOAbilitySystemComponent* LiveASC = OriginalASC.Get();
			const bool bRetained = LiveASC && ReturnedInitCleanup.IsValid()
				&& LiveASC->RetainFailedAvatarActorInfoInitCleanup(ReturnedInitCleanup, InitCleanupProofReason);
			if (!bRetained)
			{
				if (!LiveASC) { InitCleanupProofReason = EGGYGOAvatarBindingReason::InvalidASC; }
				UE_LOG(LogGGYGOAbilitySystem, Error,
					TEXT("[ASC ActorInfo] Failed Init has no authenticated cleanup source: ASC=%s Operation=%llu Owner=%s Avatar=%s FailureReason=%d ProofReason=%d."),
					*OriginalASCName, Operation.Serial, *GetPathNameSafe(AfterOwner.Get()),
					*GetPathNameSafe(AfterAvatar.Get()), static_cast<int32>(FailureReason),
					static_cast<int32>(InitCleanupProofReason));
			}
		}
		return MakeAvatarBindingExecutionFailure(Result, FailureReason);
	};
	const auto RecheckCallerAndOperation = [&](bool bCheckBeforeActual)
	{
		if (!OriginalASC.IsValid())
		{
			Reason = EGGYGOAvatarBindingReason::InvalidASC;
			return false;
		}
		if (!RecheckAvatarBindingExecutionOperation(Operation, bCheckBeforeActual, Reason))
		{
			return false;
		}
		const bool bCallerCurrent = Request.IsRequestContextCurrent();
		if (!OriginalASC.IsValid())
		{
			Reason = EGGYGOAvatarBindingReason::InvalidASC;
			return false;
		}
		if (!RecheckAvatarBindingExecutionOperation(Operation, bCheckBeforeActual, Reason))
		{
			return false;
		}
		if (!bCallerCurrent)
		{
			Reason = EGGYGOAvatarBindingReason::RequestContextExpired;
			return false;
		}
		return true;
	};
	if (!RecheckCallerAndOperation(/*bCheckBeforeActual=*/true))
	{
		return ReturnFailure(Reason);
	}
	ReleaseAvatarBindingPublicationForContext(Result.Before);
	if (OriginalKind == EGGYGOAvatarBindingKind::Clear
		|| !BeforeAvatar.HasSameIndexAndSerialNumber(AfterAvatar))
	{
		ClearAbilityInput();
	}
	// Input invalidation may invoke external listeners. Recheck before granting this native write.
	if (!RecheckCallerAndOperation(/*bCheckBeforeActual=*/true))
	{
		return ReturnFailure(Reason);
	}
	// Only this pending operation keeps BeforeActual. Old committed cleanup rights end at the write.
	RetireFailedAvatarActorInfoInitCleanup();
	AvatarBindingActorInfoSnapshot = {};
	// Never call the legacy project entry: it revokes evidence and owns old notifications.
	switch (OriginalKind)
	{
	case EGGYGOAvatarBindingKind::Init:
		RetireMontagePlaybackOwnership();
		Super::InitAbilityActorInfo(AfterOwner.Get(), AfterAvatar.Get());
		bNativeInitReturned = true;
		if (OriginalASC.IsValid())
		{
			// Capture now, before a caller query/commit failure; never reconstruct on cleanup entry.
			ReturnedInitCleanup = CaptureReturnedAvatarActorInfoInitCleanup(
				OriginalOperationRecord, InitCleanupProofReason);
		}
		break;
	case EGGYGOAvatarBindingKind::Clear:
		RetireMontagePlaybackOwnership();
		if (ClearMode == EGGYGOAvatarBindingClearMode::PreserveOwner)
		{
			Super::InitAbilityActorInfo(AfterOwner.Get(), nullptr);
		}
		else if (ClearMode == EGGYGOAvatarBindingClearMode::ClearActorInfo)
		{
			Super::ClearActorInfo();
		}
		else
		{
			return MakeAvatarBindingExecutionFailure(Result, EGGYGOAvatarBindingReason::InvalidRequest);
		}
		break;
	case EGGYGOAvatarBindingKind::Refresh:
		ReconcileMontagePlaybackOwnership();
		Super::RefreshAbilityActorInfo();
		if (OriginalASC.IsValid()) { ReconcileMontagePlaybackOwnership(); }
		break;
	default:
		// Reserve accepts only the three ActorInfo kinds at this public execution boundary.
		return MakeAvatarBindingExecutionFailure(Result, EGGYGOAvatarBindingReason::InvalidRequest);
	}
	if (!RecheckCallerAndOperation(/*bCheckBeforeActual=*/false))
	{
		return ReturnFailure(Reason);
	}
	FGGYGOAvatarBindingContext Committed;
	if (!TryCommitAvatarBindingActorInfoIdentity(Operation, Committed, Reason))
	{
		return ReturnFailure(Reason);
	}
	Result.Outcome = EGGYGOAvatarBindingOutcome::Succeeded;
	Result.Reason = EGGYGOAvatarBindingReason::None;
	Result.CommittedContext = Committed;
	Result.bCommitted = true;
	FGGYGOAvatarBindingNotice Notice;
	Notice.Operation = Operation;
	Notice.Before = Result.Before;
	Notice.After = Committed;
	switch (OriginalKind)
	{
	case EGGYGOAvatarBindingKind::Init:
		Notice.Kind = EGGYGOAvatarBindingNoticeKind::Initialized;
		break;
	case EGGYGOAvatarBindingKind::Clear:
		Notice.Kind = EGGYGOAvatarBindingNoticeKind::Released;
		break;
	case EGGYGOAvatarBindingKind::Refresh:
		Notice.Kind = EGGYGOAvatarBindingNoticeKind::Refreshed;
		break;
	default:
		return MakeAvatarBindingExecutionFailure(Result, EGGYGOAvatarBindingReason::InvalidRequest);
	}
	Notice.OwnerActor = OriginalKind == EGGYGOAvatarBindingKind::Clear ? BeforeOwner : AfterOwner;
	Notice.AvatarActor = OriginalKind == EGGYGOAvatarBindingKind::Clear ? BeforeAvatar : AfterAvatar;
	FGGYGOAvatarBindingPublicationReceipt Publication;
	Publication.Proof = MakeShared<FGGYGOAvatarBindingPublicationReceipt::FCommitPublicationProof>(
		OriginalKind, Result.Before, BeforeOwner, BeforeAvatar, Result, Notice);
	AvatarBindingPublicationRecord = {Publication, Committed, EAvatarBindingPublicationPhase::Pending};
	OutPublication = Publication;
	return Result;
}

UGGYGOAbilitySystemComponent::FScopedAvatarBindingPublication::FScopedAvatarBindingPublication(
	UGGYGOAbilitySystemComponent* InASC, const FGGYGOAvatarBindingPublicationReceipt& InPublication)
	: ASC(InASC), Publication(InPublication)
{
	check(IsInGameThread());
}

UGGYGOAbilitySystemComponent::FScopedAvatarBindingPublication::~FScopedAvatarBindingPublication()
{
	check(IsInGameThread());
	// Metadata cleanup only; this never grants a closing issuer execution permission.
	if (UGGYGOAbilitySystemComponent* OwnerASC = ASC.GetEvenIfUnreachable())
	{
		OwnerASC->CloseAvatarBindingPublicationIfMatching(Publication);
	}
}

void UGGYGOAbilitySystemComponent::CloseAvatarBindingPublicationIfMatching(
	const FGGYGOAvatarBindingPublicationReceipt& Publication)
{
	check(IsInGameThread());
	if (Publication.Proof.IsValid()
		&& AvatarBindingPublicationRecord.Publication.Proof == Publication.Proof
		&& AvatarBindingPublicationRecord.Context.HasSameContext(Publication.Proof->CommitResult.CommittedContext)
		&& (AvatarBindingPublicationRecord.Phase == EAvatarBindingPublicationPhase::Pending
			|| AvatarBindingPublicationRecord.Phase == EAvatarBindingPublicationPhase::Dispatching))
	{
		AvatarBindingPublicationRecord.Publication = FGGYGOAvatarBindingPublicationReceipt{};
		AvatarBindingPublicationRecord.Phase = EAvatarBindingPublicationPhase::Closed;
	}
}

bool UGGYGOAbilitySystemComponent::IsValidAvatarBindingPublicationProof(
	const FGGYGOAvatarBindingPublicationReceipt& Publication, EGGYGOAvatarBindingReason& OutReason) const
{
	OutReason = EGGYGOAvatarBindingReason::InvalidPublication;
	check(IsInGameThread());
	if (!IsValid(this) || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		OutReason = EGGYGOAvatarBindingReason::InvalidASC;
		return false;
	}
	if (!Publication.Proof.IsValid())
	{
		return false;
	}
	const FGGYGOAvatarBindingPublicationReceipt::FCommitPublicationProof& Proof = *Publication.Proof;
	const FGGYGOAvatarBindingResult& History = Proof.CommitResult;
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> Self(
		const_cast<UGGYGOAbilitySystemComponent*>(this));
	if (!History.bCommitted || History.Outcome != EGGYGOAvatarBindingOutcome::Succeeded
		|| History.Reason != EGGYGOAvatarBindingReason::None
		|| !History.Operation.HasIssuedIdentity()
		|| !History.Operation.Issuer.HasSameIndexAndSerialNumber(Self)
		|| !History.CommittedContext.HasIssuedContext()
		|| !History.CommittedContext.Binding.Issuer.HasSameIndexAndSerialNumber(Self)
		|| !History.CommittedContext.LastActorInfoWrite.HasSameIdentity(History.Operation)
		|| !Proof.Notice.Operation.HasSameIdentity(History.Operation)
		|| !Proof.Notice.After.HasSameContext(History.CommittedContext))
	{
		return false;
	}
	// Historical empty Bootstrap Before values are compared by value, not admitted as identities.
	const auto SameHistoryContext = [](const FGGYGOAvatarBindingContext& Left,
		const FGGYGOAvatarBindingContext& Right)
	{
		return Left.Binding.Serial == Right.Binding.Serial
			&& Left.Binding.Issuer.HasSameIndexAndSerialNumber(Right.Binding.Issuer)
			&& Left.LastActorInfoWrite.Serial == Right.LastActorInfoWrite.Serial
			&& Left.LastActorInfoWrite.Issuer.HasSameIndexAndSerialNumber(Right.LastActorInfoWrite.Issuer);
	};
	if (!SameHistoryContext(History.Before, Proof.BeforeContext)
		|| !SameHistoryContext(Proof.Notice.Before, Proof.BeforeContext))
	{
		return false;
	}
	switch (Proof.OriginalKind)
	{
	case EGGYGOAvatarBindingKind::Init:
		if (Proof.Notice.Kind != EGGYGOAvatarBindingNoticeKind::Initialized) { return false; }
		break;
	case EGGYGOAvatarBindingKind::Clear:
		if (Proof.Notice.Kind != EGGYGOAvatarBindingNoticeKind::Released
			|| !Proof.Notice.OwnerActor.HasSameIndexAndSerialNumber(Proof.BeforeOwnerActor)
			|| !Proof.Notice.AvatarActor.HasSameIndexAndSerialNumber(Proof.BeforeAvatarActor)) { return false; }
		break;
	case EGGYGOAvatarBindingKind::Refresh:
		if (Proof.Notice.Kind != EGGYGOAvatarBindingNoticeKind::Refreshed) { return false; }
		break;
	default:
		return false;
	}
	OutReason = EGGYGOAvatarBindingReason::None;
	return true;
}

bool UGGYGOAbilitySystemComponent::RecheckAvatarBindingPublication(
	const FGGYGOAvatarBindingPublicationReceipt& Publication,
	EAvatarBindingPublicationPhase ExpectedPhase, EGGYGOAvatarBindingReason& OutReason) const
{
	OutReason = EGGYGOAvatarBindingReason::OperationInvalidated;
	check(IsInGameThread());
	if (!IsValidAvatarBindingPublicationProof(Publication, OutReason))
	{
		return false;
	}
	if (bAvatarBindingNativeWriteBusy)
	{
		OutReason = EGGYGOAvatarBindingReason::NativeWriteBusy;
		return false;
	}
	const FGGYGOAvatarBindingContext& Committed = Publication.Proof->CommitResult.CommittedContext;
	if (CheckAvatarBindingContext(Committed, OutReason) != EGGYGOAvatarBindingOutcome::Succeeded)
	{
		return false;
	}
	if (AvatarBindingPublicationRecord.Phase != ExpectedPhase
		|| AvatarBindingPublicationRecord.Publication.Proof != Publication.Proof
		|| !AvatarBindingPublicationRecord.Context.HasSameContext(Committed))
	{
		OutReason = EGGYGOAvatarBindingReason::OperationInvalidated;
		return false;
	}
	if (Publication.Proof->OriginalKind != EGGYGOAvatarBindingKind::Clear
		&& (!AbilityActorInfo->OwnerActor.HasSameIndexAndSerialNumber(Publication.Proof->Notice.OwnerActor)
			|| !AbilityActorInfo->AvatarActor.HasSameIndexAndSerialNumber(Publication.Proof->Notice.AvatarActor)))
	{
		OutReason = EGGYGOAvatarBindingReason::ActorInfoMismatch;
		return false;
	}
	OutReason = EGGYGOAvatarBindingReason::None;
	return true;
}

bool UGGYGOAbilitySystemComponent::IsAvatarBindingNoticeDispatching(
	const FGGYGOAvatarBindingPublicationReceipt& Publication) const
{
	EGGYGOAvatarBindingReason Reason;
	return RecheckAvatarBindingPublication(Publication, EAvatarBindingPublicationPhase::Dispatching, Reason);
}

bool UGGYGOAbilitySystemComponent::IsAvatarBindingPublicationContextCurrent(
	const FGGYGOAvatarBindingContext& Expected) const
{
	check(IsInGameThread());
	if (!IsValid(this) || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		|| bAvatarBindingNativeWriteBusy
		|| (AvatarBindingPublicationRecord.Phase != EAvatarBindingPublicationPhase::Dispatching
			&& AvatarBindingPublicationRecord.Phase != EAvatarBindingPublicationPhase::Consumed)
		|| !AvatarBindingPublicationRecord.Context.HasSameContext(Expected))
	{
		return false;
	}
	EGGYGOAvatarBindingReason Reason;
	return CheckAvatarBindingContext(Expected, Reason) == EGGYGOAvatarBindingOutcome::Succeeded;
}

FGGYGOAvatarBindingNoticeEvent& UGGYGOAbilitySystemComponent::OnAvatarBindingNotice()
{
	check(IsInGameThread());
	return AvatarBindingNoticeEvent;
}

FGGYGOAvatarBindingResult UGGYGOAbilitySystemComponent::PublishAvatarBindingNotice(
	const FGGYGOAvatarBindingPublicationReceipt& Publication,
	TFunction<bool()> IsPublicationContextCurrent)
{
	check(IsInGameThread());
	// Independent stack copies survive a caller replacing its own Receipt during an external call.
	const FGGYGOAvatarBindingPublicationReceipt OwnPublication = Publication;
	FGGYGOAvatarBindingResult Result;
	if (OwnPublication.Proof.IsValid())
	{
		Result = OwnPublication.Proof->CommitResult;
	}
	EGGYGOAvatarBindingReason Reason;
	if (!IsValidAvatarBindingPublicationProof(OwnPublication, Reason))
	{
		return MakeAvatarBindingExecutionFailure(Result, Reason);
	}
	if (bAvatarBindingNativeWriteBusy)
	{
		return MakeAvatarBindingExecutionFailure(Result, EGGYGOAvatarBindingReason::NativeWriteBusy);
	}
	if (CheckAvatarBindingContext(Result.CommittedContext, Reason) != EGGYGOAvatarBindingOutcome::Succeeded)
	{
		// Invalid actual state closes only this proof, if it is still ours.
		CloseAvatarBindingPublicationIfMatching(OwnPublication);
		return MakeAvatarBindingExecutionFailure(Result, Reason);
	}
	if (!AvatarBindingPublicationRecord.Context.HasSameContext(Result.CommittedContext))
	{
		return MakeAvatarBindingExecutionFailure(Result, EGGYGOAvatarBindingReason::OperationInvalidated);
	}
	if (AvatarBindingPublicationRecord.Phase == EAvatarBindingPublicationPhase::Consumed)
	{
		return MakeAvatarBindingExecutionFailure(Result, EGGYGOAvatarBindingReason::PublicationAlreadyConsumed);
	}
	if (AvatarBindingPublicationRecord.Phase == EAvatarBindingPublicationPhase::Dispatching
		&& AvatarBindingPublicationRecord.Publication.Proof == OwnPublication.Proof)
	{
		return MakeAvatarBindingExecutionFailure(Result, EGGYGOAvatarBindingReason::PublicationInProgress);
	}
	if (!RecheckAvatarBindingPublication(OwnPublication, EAvatarBindingPublicationPhase::Pending, Reason))
	{
		CloseAvatarBindingPublicationIfMatching(OwnPublication);
		return MakeAvatarBindingExecutionFailure(Result, Reason);
	}
	FScopedAvatarBindingPublication PublicationScope(this, OwnPublication);
	if (!IsPublicationContextCurrent)
	{
		return MakeAvatarBindingExecutionFailure(Result, EGGYGOAvatarBindingReason::MissingContextQuery);
	}
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalASC(this);
	EAvatarBindingPublicationPhase ExpectedPhase = EAvatarBindingPublicationPhase::Pending;
	const auto Recheck = [&]()
	{
		UGGYGOAbilitySystemComponent* LiveASC = OriginalASC.Get();
		if (!LiveASC)
		{
			Reason = EGGYGOAvatarBindingReason::InvalidASC;
			return false;
		}
		if (!LiveASC->RecheckAvatarBindingPublication(OwnPublication, ExpectedPhase, Reason)) { return false; }
		const bool bCallerCurrent = IsPublicationContextCurrent();
		LiveASC = OriginalASC.Get();
		if (!LiveASC)
		{
			Reason = EGGYGOAvatarBindingReason::InvalidASC;
			return false;
		}
		if (!LiveASC->RecheckAvatarBindingPublication(OwnPublication, ExpectedPhase, Reason)) { return false; }
		if (!bCallerCurrent)
		{
			Reason = EGGYGOAvatarBindingReason::RequestContextExpired;
			return false;
		}
		return true;
	};
	if (!Recheck()) { return MakeAvatarBindingExecutionFailure(Result, Reason); }
	AvatarBindingPublicationRecord.Phase = EAvatarBindingPublicationPhase::Dispatching;
	ExpectedPhase = EAvatarBindingPublicationPhase::Dispatching;
	const FGGYGOAvatarBindingNotice Notice = OwnPublication.Proof->Notice;
	if (!Recheck()) { return MakeAvatarBindingExecutionFailure(Result, Reason); }
	AvatarBindingNoticeEvent.Broadcast(OwnPublication, Notice);
	if (!Recheck()) { return MakeAvatarBindingExecutionFailure(Result, Reason); }

	// Preserve the existing new-Pawn Init policy from immutable Before/After weak identities.
	const bool bHasNewPawnAvatar = OwnPublication.Proof->OriginalKind == EGGYGOAvatarBindingKind::Init
		&& Cast<APawn>(Notice.AvatarActor.Get())
		&& !OwnPublication.Proof->BeforeAvatarActor.HasSameIndexAndSerialNumber(Notice.AvatarActor);
	if (bHasNewPawnAvatar)
	{
		// No project calls while collecting. Weak liveness alone is not granted-resource ownership.
		struct FAvatarNotificationCandidate
		{
			FGameplayAbilitySpecHandle SpecHandle;
			TWeakObjectPtr<UGGYGOGameplayAbility> Instance;
		};
		TArray<FAvatarNotificationCandidate> Instances;
		for (const FGameplayAbilitySpec& Spec : ActivatableAbilities.Items)
		{
			for (UGameplayAbility* Instance : Spec.GetAbilityInstances())
			{
				if (UGGYGOGameplayAbility* ProjectInstance = Cast<UGGYGOGameplayAbility>(Instance))
				{
					Instances.Add({Spec.Handle, ProjectInstance});
				}
			}
		}
		for (const FAvatarNotificationCandidate& Candidate : Instances)
		{
			if (!Recheck()) { return MakeAvatarBindingExecutionFailure(Result, Reason); }
			UGGYGOGameplayAbility* LiveInstance = Candidate.Instance.Get();
			if (!LiveInstance) { continue; }
			bool bIsCurrentSpecInstance = false;
			{
				// Default lookup includes PendingRemove; a revoked grant must not receive this notice.
				const FGameplayAbilitySpec* CurrentSpec = FindAbilitySpecFromHandle(
					Candidate.SpecHandle, EConsiderPending::None);
				bIsCurrentSpecInstance = CurrentSpec && CurrentSpec->GetAbilityInstances().Contains(LiveInstance);
			} // No Spec pointer or instance-array snapshot crosses the external notification.
			if (!bIsCurrentSpecInstance) { continue; }
			LiveInstance->OnPawnAvatarSet();
			if (!Recheck()) { return MakeAvatarBindingExecutionFailure(Result, Reason); }
		}

		// Collect after OnPawnAvatarSet, as before; callbacks may have granted/removed Specs.
		TArray<FGameplayAbilitySpecHandle> SpawnHandles;
		for (const FGameplayAbilitySpec& Spec : ActivatableAbilities.Items)
		{
			if (Cast<UGGYGOGameplayAbility>(Spec.Ability)) { SpawnHandles.Add(Spec.Handle); }
		}
		for (FGameplayAbilitySpecHandle Handle : SpawnHandles)
		{
			if (!Recheck()) { return MakeAvatarBindingExecutionFailure(Result, Reason); }
			const FGameplayAbilitySpec* CurrentSpec = FindAbilitySpecFromHandle(Handle, EConsiderPending::None);
			if (!CurrentSpec) { continue; } // A previous callback removed or marked this resource for removal.
			const UGGYGOGameplayAbility* AbilityCDO = Cast<UGGYGOGameplayAbility>(CurrentSpec->Ability);
			if (!AbilityCDO) { continue; }
			const FGameplayAbilitySpec SpawnSpec = *CurrentSpec;
			AbilityCDO->TryActivateAbilityOnSpawn(AbilityActorInfo.Get(), SpawnSpec);
			if (!Recheck()) { return MakeAvatarBindingExecutionFailure(Result, Reason); }
		}
	}
	if (!Recheck()) { return MakeAvatarBindingExecutionFailure(Result, Reason); }
	// No external call between exact recheck and consumption. Scope cleanup cannot touch Consumed.
	AvatarBindingPublicationRecord.Phase = EAvatarBindingPublicationPhase::Consumed;
	AvatarBindingPublicationRecord.Publication = FGGYGOAvatarBindingPublicationReceipt{};
	return Result;
}

UGGYGOAbilitySystemComponent::FScopedAvatarSwitchAbilityExit::FScopedAvatarSwitchAbilityExit(
	UGGYGOAbilitySystemComponent* InASC, const TArray<FAvatarSwitchAbilityExitCandidate>& InCandidates)
	: Candidates(InCandidates), ASC(InASC)
{
	check(InASC && !InASC->AvatarSwitchAbilityExitScope);
	InASC->AvatarSwitchAbilityExitScope = this;
}

UGGYGOAbilitySystemComponent::FScopedAvatarSwitchAbilityExit::~FScopedAvatarSwitchAbilityExit()
{
	if (UGGYGOAbilitySystemComponent* OriginalASC = ASC.GetEvenIfUnreachable())
	{
		if (OriginalASC->AvatarSwitchAbilityExitScope == this) { OriginalASC->AvatarSwitchAbilityExitScope = nullptr; }
	}
}

bool UGGYGOAbilitySystemComponent::IsAvatarSwitchCancellationReserved(const UGGYGOGameplayAbility* Ability) const
{
	if (!AvatarSwitchAbilityExitScope || !AvatarSwitchAbilityExitScope->bCancellationReservationsActive) { return false; }
	for (const FAvatarSwitchAbilityExitCandidate& Candidate : AvatarSwitchAbilityExitScope->Candidates)
	{
		if (Candidate.Policy == EGGYGOAbilityAvatarExitPolicy::Cancel && Candidate.Ability.Get() == Ability
			&& Ability && Ability->IsActive() && Candidate.Original.HasSameActivation(Ability->CurrentControlledActivation))
		{
			return true;
		}
	}
	return false;
}

bool UGGYGOAbilitySystemComponent::CollectAvatarSwitchAbilityExitCandidates(
	const FGGYGOAvatarBindingContext& Expected, bool bRequireExited,
	TArray<FAvatarSwitchAbilityExitCandidate>& OutCandidates, FGGYGOAvatarSwitchAbilityExitResult& OutResult) const
{
	using EOutcome = EGGYGOAvatarSwitchAbilityExitOutcome;
	using EReason = EGGYGOAvatarSwitchAbilityExitReason;
	OutCandidates.Reset();
	OutResult = {};
	if (!IsValid(this) || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		OutResult.Reason = EReason::InvalidASC;
		return false;
	}
	if (!IsOwnerActorAuthoritative()) { OutResult.Reason = EReason::NotAuthority; return false; }
	if (!IsAvatarBindingNewWorkLifecycleOpen(OutResult.BindingReason)
		|| CheckAvatarBindingContext(Expected, OutResult.BindingReason) != EGGYGOAvatarBindingOutcome::Succeeded)
	{
		OutResult.Outcome = EOutcome::Stale;
		OutResult.Reason = EReason::BindingChanged;
		return false;
	}
	if (AbilityScopeLockCount > 0 || bAbilityPendingClearAll || !AbilityPendingAdds.IsEmpty()
		|| !AbilityPendingRemoves.IsEmpty() || NativeAbilityCleanupScope)
	{
		OutResult.Outcome = EOutcome::Busy;
		OutResult.Reason = EReason::PendingAbilityChanges;
		return false;
	}
	if (ControlledAbilityActivationCall || !AbilityActivationEvaluations.IsEmpty())
	{
		OutResult.Outcome = EOutcome::Busy;
		OutResult.Reason = EReason::AbilityNotReady;
		OutResult.TerminationReason = EGGYGOAbilityTerminationReason::ActivationCallInProgress;
		return false;
	}
	for (const FGameplayAbilitySpec& Spec : ActivatableAbilities.Items)
	{
		bool bHasActiveInstance = false;
		for (UGameplayAbility* NativeInstance : Spec.GetAbilityInstances())
		{
			if (!NativeInstance || !NativeInstance->IsActive()) { continue; }
			bHasActiveInstance = true;
			OutResult.BlockingSpec = Spec.Handle;
			UGGYGOGameplayAbility* Ability = Cast<UGGYGOGameplayAbility>(NativeInstance);
			if (!IsValid(Ability)) { OutResult.Reason = EReason::UnsupportedAbility; return false; }
			if (Spec.PendingRemove)
			{
				OutResult.Outcome = EOutcome::Busy;
				OutResult.Reason = EReason::PendingAbilityChanges;
				return false;
			}
			const EGGYGOAbilityAvatarExitPolicy Policy = Ability->GetAvatarExitPolicy();
			if (Policy != EGGYGOAbilityAvatarExitPolicy::Cancel && Policy != EGGYGOAbilityAvatarExitPolicy::ContinueInBackground)
			{
				OutResult.Reason = EReason::InvalidExitPolicy;
				UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("AbilitySystem Avatar switch [%s] Ability [%s] Spec [%s] has invalid AvatarExitPolicy=%d."),
					*GetPathName(), *Ability->GetPathName(), *Spec.Handle.ToString(), static_cast<int32>(Policy));
				return false;
			}
			if (bRequireExited && Policy == EGGYGOAbilityAvatarExitPolicy::Cancel)
			{
				OutResult.Reason = EReason::ActiveCancellationRequired;
				return false;
			}
			if (Ability->GetCurrentAbilitySpecHandle() != Spec.Handle || Ability->GetCurrentActorInfo() != AbilityActorInfo.Get())
			{
				OutResult.Outcome = EOutcome::Stale;
				OutResult.Reason = EReason::ActivationChanged;
				return false;
			}
			FGGYGOAbilityActivationHandle Original;
			OutResult.TerminationReason = Ability->CheckAvatarSwitchExitPreflight(Policy == EGGYGOAbilityAvatarExitPolicy::Cancel, Original);
			if (OutResult.TerminationReason != EGGYGOAbilityTerminationReason::None)
			{
				OutResult.Outcome = OutResult.TerminationReason == EGGYGOAbilityTerminationReason::TerminationInProgress
					|| OutResult.TerminationReason == EGGYGOAbilityTerminationReason::ActivationCallInProgress
					|| OutResult.TerminationReason == EGGYGOAbilityTerminationReason::ScopeLocked ? EOutcome::Busy : EOutcome::Rejected;
				OutResult.Reason = EReason::AbilityNotReady;
				return false;
			}
			FAvatarSwitchAbilityExitCandidate& Candidate = OutCandidates.AddDefaulted_GetRef();
			Candidate.Ability = Ability;
			Candidate.Spec = Spec.Handle;
			Candidate.Original = Original;
			Candidate.Policy = Policy;
		}
		if (Spec.IsActive() && !bHasActiveInstance)
		{
			OutResult.BlockingSpec = Spec.Handle;
			OutResult.Reason = EReason::UnsupportedAbility;
			return false; // Native active/count without an endable project instance cannot be called complete.
		}
	}
	OutResult.Outcome = EOutcome::Ready;
	OutResult.Reason = EReason::None;
	OutResult.BlockingSpec = {};
	return true;
}

FGGYGOAvatarSwitchAbilityExitResult UGGYGOAbilitySystemComponent::CheckAvatarSwitchAbilitiesExited(
	const FGGYGOAvatarBindingContext& Expected) const
{
	check(IsInGameThread());
	FGGYGOAvatarSwitchAbilityExitResult Result;
	if (IsAvatarBindingNativeWriteBusy() || AvatarSwitchAbilityExitScope)
	{
		Result.Outcome = EGGYGOAvatarSwitchAbilityExitOutcome::Busy;
		Result.Reason = EGGYGOAvatarSwitchAbilityExitReason::NativeWriteBusy;
		return Result;
	}
	TArray<FAvatarSwitchAbilityExitCandidate> Current;
	CollectAvatarSwitchAbilityExitCandidates(Expected, true, Current, Result);
	return Result;
}

FGGYGOAvatarSwitchAbilityExitResult UGGYGOAbilitySystemComponent::TryExitAbilitiesForAvatarSwitch(
	const FGGYGOAvatarBindingContext& Expected, TFunction<bool()> IsOriginalCallerCurrent)
{
	check(IsInGameThread());
	using EOutcome = EGGYGOAvatarSwitchAbilityExitOutcome;
	using EReason = EGGYGOAvatarSwitchAbilityExitReason;
	const FGGYGOAvatarBindingContext OriginalContext = Expected;
	FGGYGOAvatarSwitchAbilityExitResult Result;
	if (!IsOriginalCallerCurrent) { Result.Reason = EReason::MissingContextQuery; return Result; }
	if (IsAvatarBindingNativeWriteBusy() || AvatarSwitchAbilityExitScope)
	{
		Result.Outcome = EOutcome::Busy;
		Result.Reason = EReason::NativeWriteBusy;
		return Result;
	}
	TArray<FAvatarSwitchAbilityExitCandidate> Candidates;
	// Ability exit does not write ActorInfo. Its own lease excludes reentrant writes/activation
	// while the caller continues to authenticate the original Pawn's published Ready context.
	FScopedAvatarSwitchAbilityExit ExitScope(this, Candidates);
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalASC(this);
	const auto RecheckCaller = [&]()
	{
		UGGYGOAbilitySystemComponent* LiveASC = OriginalASC.Get();
		if (!LiveASC) { Result.Outcome = EOutcome::Stale; Result.Reason = EReason::InvalidASC; return false; }
		if (LiveASC->CheckAvatarBindingContext(OriginalContext, Result.BindingReason) != EGGYGOAvatarBindingOutcome::Succeeded)
		{
			Result.Outcome = EOutcome::Stale;
			Result.Reason = EReason::BindingChanged;
			return false;
		}
		const bool bCallerCurrent = IsOriginalCallerCurrent();
		LiveASC = OriginalASC.Get();
		if (!LiveASC) { Result.Outcome = EOutcome::Stale; Result.Reason = EReason::InvalidASC; return false; }
		if (LiveASC->CheckAvatarBindingContext(OriginalContext, Result.BindingReason) != EGGYGOAvatarBindingOutcome::Succeeded)
		{
			Result.Outcome = EOutcome::Stale;
			Result.Reason = EReason::BindingChanged;
			return false;
		}
		if (!bCallerCurrent) { Result.Outcome = EOutcome::Stale; Result.Reason = EReason::CallerExpired; return false; }
		return true;
	};
	if (!RecheckCaller() || !CollectAvatarSwitchAbilityExitCandidates(OriginalContext, false, Candidates, Result)
		|| !RecheckCaller()) { return Result; }
	// All default-Cancel instances have passed admission before the first native cancel.
	ExitScope.bCancellationReservationsActive = true;
	for (const FAvatarSwitchAbilityExitCandidate& Candidate : Candidates)
	{
		if (Candidate.Policy == EGGYGOAbilityAvatarExitPolicy::ContinueInBackground) { continue; }
		if (!RecheckCaller()) { return Result; }
		UGGYGOGameplayAbility* Ability = Candidate.Ability.Get();
		FGGYGOAbilityActivationHandle Current;
		Result.BlockingSpec = Candidate.Spec;
		Result.TerminationReason = Ability ? Ability->CheckAvatarSwitchExitPreflight(true, Current)
			: EGGYGOAbilityTerminationReason::InvalidAbility;
		if (Result.TerminationReason != EGGYGOAbilityTerminationReason::None || !Current.HasSameActivation(Candidate.Original)
			|| Ability->GetAvatarExitPolicy() != Candidate.Policy)
		{
			Result.Outcome = EOutcome::Stale;
			Result.Reason = EReason::ActivationChanged;
			return Result;
		}
		const FGGYGOAbilityTerminationResult Termination = Ability->RequestAbilityCancel(Candidate.Original, true);
		if (Termination.Outcome != EGGYGOAbilityTerminationOutcome::Completed
			|| !Termination.Original.GetOriginalActivation().HasSameActivation(Candidate.Original)
			|| Termination.Original.GetRequestKind() != EGGYGOAbilityTerminationRequestKind::Cancel)
		{
			Result.Outcome = EOutcome::Failed;
			Result.Reason = EReason::TerminationNotCompleted;
			Result.TerminationReason = Termination.Reason;
			return Result; // Accepted/Deferred/unsupported are never switch permission.
		}
		if (!RecheckCaller()) { return Result; }
	}
	TArray<FAvatarSwitchAbilityExitCandidate> Remaining;
	if (!CollectAvatarSwitchAbilityExitCandidates(OriginalContext, true, Remaining, Result))
	{
		if (Result.Outcome == EOutcome::Rejected) { Result.Outcome = EOutcome::Failed; }
		return Result; // This is a post-cancellation failure, not side-effect-free admission rejection.
	}
	if (!RecheckCaller()) { return Result; }
	Result.Outcome = EOutcome::Completed;
	return Result;
}

FGGYGOAvatarBindingResult UGGYGOAbilitySystemComponent::TryCancelAvatarBindingAbilities(
	const FGGYGOAvatarBindingContext& Expected,
	const FGameplayTagContainer* WithTags,
	const FGameplayTagContainer* WithoutTags,
	TFunction<bool()> IsOriginalCallerCurrent)
{
	FGGYGOAvatarBindingResult Result;
	check(IsInGameThread());
	// Existence only: the external query must run inside the native Busy window.
	if (!IsOriginalCallerCurrent)
	{
		return MakeAvatarBindingExecutionFailure(Result, EGGYGOAvatarBindingReason::MissingContextQuery);
	}
	if (IsAvatarBindingNativeWriteBusy() || AvatarSwitchAbilityExitScope)
	{
		return MakeAvatarBindingExecutionFailure(Result, EGGYGOAvatarBindingReason::NativeWriteBusy);
	}

	// Keep caller tag mutation across callbacks from changing this native traversal.
	const bool bHasWithTags = WithTags != nullptr;
	const bool bHasWithoutTags = WithoutTags != nullptr;
	FGameplayTagContainer WithTagsSnapshot;
	FGameplayTagContainer WithoutTagsSnapshot;
	if (WithTags) { WithTagsSnapshot = *WithTags; }
	if (WithoutTags) { WithoutTagsSnapshot = *WithoutTags; }

	FGGYGOAvatarBindingRequest Request;
	Request.Kind = EGGYGOAvatarBindingKind::CancelAbilities;
	Request.ExpectedContext = Expected;
	Request.IsRequestContextCurrent = MoveTemp(IsOriginalCallerCurrent);
	FGGYGOAvatarBindingOperationIdentity Operation;
	EGGYGOAvatarBindingReason Reason;
	if (!TryReserveAvatarBindingIdentityOperation(Request,
		EAvatarBindingIdentityAdmission::MatchCommittedCleanupContext, Operation, Reason))
	{
		return MakeAvatarBindingExecutionFailure(Result, Reason);
	}
	Result.Operation = Operation;
	Result.Before = ActiveAvatarBindingIdentityOperation.BeforeContext;
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalASC(this);
	FScopedAvatarBindingNativeWrite NativeWrite(this, Operation);
	if (!NativeWrite.HasEntered())
	{
		return MakeAvatarBindingExecutionFailure(Result, EGGYGOAvatarBindingReason::NativeWriteBusy);
	}
	const auto Recheck = [&]()
	{
		UGGYGOAbilitySystemComponent* LiveASC = OriginalASC.Get();
		if (!LiveASC)
		{
			Reason = EGGYGOAvatarBindingReason::InvalidASC;
			return false;
		}
		if (!LiveASC->RecheckAvatarBindingExecutionOperation(Operation, true, Reason)
			|| LiveASC->CheckAvatarBindingCleanupContext(Result.Before, Reason) != EGGYGOAvatarBindingOutcome::Succeeded)
		{
			return false;
		}
		const bool bCallerCurrent = Request.IsRequestContextCurrent();
		LiveASC = OriginalASC.Get();
		if (!LiveASC)
		{
			Reason = EGGYGOAvatarBindingReason::InvalidASC;
			return false;
		}
		if (!LiveASC->RecheckAvatarBindingExecutionOperation(Operation, true, Reason)
			|| LiveASC->CheckAvatarBindingCleanupContext(Result.Before, Reason) != EGGYGOAvatarBindingOutcome::Succeeded)
		{
			return false;
		}
		if (!bCallerCurrent)
		{
			Reason = EGGYGOAvatarBindingReason::RequestContextExpired;
			return false;
		}
		return true;
	};
	if (!Recheck()) { return MakeAvatarBindingExecutionFailure(Result, Reason); }
	ReleaseAvatarBindingPublicationForContext(Result.Before);
	// Include the entire native traversal and its list-lock destructor in this Busy scope.
	{
		FScopedNativeAbilityCleanup Cleanup(this, ENativeAbilityCleanupSource::AvatarBindingRelease);
		Super::CancelAbilities(
			bHasWithTags ? &WithTagsSnapshot : nullptr,
			bHasWithoutTags ? &WithoutTagsSnapshot : nullptr,
			nullptr);
	}
	if (!Recheck()) { return MakeAvatarBindingExecutionFailure(Result, Reason); }
	// No external call between final recheck and exact operation completion.
	if (!TryCompleteAvatarBindingIdentityOperation(Operation, Reason))
	{
		return MakeAvatarBindingExecutionFailure(Result, Reason);
	}
	Result.Outcome = EGGYGOAvatarBindingOutcome::Succeeded;
	Result.Reason = EGGYGOAvatarBindingReason::None;
	// Completion does not commit a binding; leave bCommitted and CommittedContext empty.
	return Result;
}

FGGYGOAvatarBindingResult UGGYGOAbilitySystemComponent::TryRemoveAvatarBindingGameplayCues(
	const FGGYGOAvatarBindingContext& Expected,
	TFunction<bool()> IsOriginalCallerCurrent)
{
	FGGYGOAvatarBindingResult Result;
	check(IsInGameThread());
	// Existence only; all caller queries run inside the existing native Busy scope.
	if (!IsOriginalCallerCurrent)
	{
		return MakeAvatarBindingExecutionFailure(Result, EGGYGOAvatarBindingReason::MissingContextQuery);
	}
	if (IsAvatarBindingNativeWriteBusy() || AvatarSwitchAbilityExitScope)
	{
		return MakeAvatarBindingExecutionFailure(Result, EGGYGOAvatarBindingReason::NativeWriteBusy);
	}
	FGGYGOAvatarBindingRequest Request;
	Request.Kind = EGGYGOAvatarBindingKind::RemoveGameplayCues;
	Request.ExpectedContext = Expected;
	Request.IsRequestContextCurrent = MoveTemp(IsOriginalCallerCurrent);
	FGGYGOAvatarBindingOperationIdentity Operation;
	EGGYGOAvatarBindingReason Reason;
	if (!TryReserveAvatarBindingIdentityOperation(Request,
		EAvatarBindingIdentityAdmission::MatchCommittedCleanupContext, Operation, Reason))
	{
		return MakeAvatarBindingExecutionFailure(Result, Reason);
	}
	Result.Operation = Operation;
	Result.Before = ActiveAvatarBindingIdentityOperation.BeforeContext;
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalASC(this);
	FScopedAvatarBindingNativeWrite NativeWrite(this, Operation);
	if (!NativeWrite.HasEntered())
	{
		return MakeAvatarBindingExecutionFailure(Result, EGGYGOAvatarBindingReason::NativeWriteBusy);
	}
	// Only existing identity/actual-state checks; no Cue candidates or execution state.
	const auto CheckOriginal = [&]()
	{
		UGGYGOAbilitySystemComponent* LiveASC = OriginalASC.Get();
		if (!LiveASC)
		{
			Reason = EGGYGOAvatarBindingReason::InvalidASC;
			return false;
		}
		return LiveASC->RecheckAvatarBindingExecutionOperation(Operation, true, Reason)
			&& LiveASC->CheckAvatarBindingCleanupContext(Result.Before, Reason) == EGGYGOAvatarBindingOutcome::Succeeded;
	};
	const auto Recheck = [&]()
	{
		if (!CheckOriginal()) { return false; }
		const bool bCallerCurrent = Request.IsRequestContextCurrent();
		if (!CheckOriginal()) { return false; }
		if (!bCallerCurrent)
		{
			Reason = EGGYGOAvatarBindingReason::RequestContextExpired;
			return false;
		}
		return true;
	};
	if (!Recheck()) { return MakeAvatarBindingExecutionFailure(Result, Reason); }
	ReleaseAvatarBindingPublicationForContext(Result.Before);
	// Native Active-tag traversal, callbacks and container cleanup must fully return first.
	Super::RemoveAllGameplayCues();
	if (!Recheck()) { return MakeAvatarBindingExecutionFailure(Result, Reason); }
	// No external call between final recheck and exact operation completion.
	if (!TryCompleteAvatarBindingIdentityOperation(Operation, Reason))
	{
		return MakeAvatarBindingExecutionFailure(Result, Reason);
	}
	Result.Outcome = EGGYGOAvatarBindingOutcome::Succeeded;
	Result.Reason = EGGYGOAvatarBindingReason::None;
	// Native return is neither a binding commit nor proof of all Cue/Notify completion.
	return Result;
}
