/**
 * @file GGYGOGameplayAbility.cpp
 * @brief GameplayAbility 基类实现
 */
#include "AbilitySystem/Abilities/GGYGOGameplayAbility.h"

#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemGlobals.h"
#include "AbilitySystem/Abilities/GGYGOAbilityCost.h"
#include "AbilitySystem/Abilities/GGYGOAbilityFailureMessages.h"
#include "AbilitySystem/GGYGOAbilitySourceInterface.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "AbilitySystem/GGYGOGameplayEffectContext.h"
#include "Camera/GGYGOCameraComponent.h"
#include "Character/Components/GGYGOHeroComponent.h"
#include "Engine/HitResult.h"
#include "GameFramework/Character.h"
#include "GameFramework/Controller.h"
#include "GameFramework/GameplayMessageSubsystem.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Physics/GGYGOPhysicalMaterialWithTags.h"
#include "System/GGYGOGameplayTags.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOGameplayAbility)

// 失败反馈消息的两个通道 Tag 在此定义。
UE_DEFINE_GAMEPLAY_TAG(TAG_GGYGO_Ability_SimpleFailureMessage, "Ability.UserFacingSimpleActivateFail.Message");
UE_DEFINE_GAMEPLAY_TAG(TAG_GGYGO_Ability_PlayMontageFailureMessage, "Ability.PlayMontageOnActivateFail.Message");

namespace
{
void AppendPhysicalMaterialTags(const FHitResult& HitResult, FGameplayTagContainer& OutTags)
{
	if (const UGGYGOPhysicalMaterialWithTags* PhysMatWithTags =
		Cast<const UGGYGOPhysicalMaterialWithTags>(HitResult.PhysMaterial.Get()))
	{
		OutTags.AppendTags(PhysMatWithTags->Tags);
	}
}
}

/** Immutable original source, with weak allocation/objects; never an Active or spec-count cache. */
struct FGGYGOAbilityActivationHandle::FActivationProof
{
	TWeakObjectPtr<UGGYGOGameplayAbility> Ability;
	TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC;
	uint64 Serial = 0;
	FGameplayAbilitySpecHandle SpecHandle;
	FPredictionKey ActivationKey; // Native coherence only; Serial/proof is the activation identity.
	FGGYGOAvatarBindingContext BindingContext;
	TWeakPtr<const FGameplayAbilityActorInfo> Allocation;
	TWeakObjectPtr<UAbilitySystemComponent> ActorInfoASC;
	TWeakObjectPtr<AActor> OwnerActor;
	TWeakObjectPtr<AActor> AvatarActor;
	TWeakObjectPtr<APlayerController> PlayerController;
	TWeakObjectPtr<USkeletalMeshComponent> SkeletalMeshComponent;
	TWeakObjectPtr<UMovementComponent> MovementComponent;
	TWeakObjectPtr<UAnimInstance> ActorInfoAnimInstance;
	TWeakObjectPtr<UAnimInstance> ActualAnimInstance;
	TWeakObjectPtr<AActor> CachedOwnerActor;
	TWeakObjectPtr<AActor> CachedAvatarActor;
	FName ActorInfoAffectedAnimInstanceTag = NAME_None;
	FName ASCAffectedAnimInstanceTag = NAME_None;
};

FGGYGOAbilityActivationHandle UGGYGOGameplayAbility::IssueControlledActivation(
	UGGYGOAbilitySystemComponent* OriginalASC, FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, EGGYGOAbilityActivationRequestReason& OutReason)
{
	check(IsInGameThread());
	OutReason = EGGYGOAbilityActivationRequestReason::InvalidAbility;
	if (!IsValid(this) || !IsInstantiated() || !IsActive() || !IsValid(OriginalASC)
		|| CurrentSpecHandle != Handle || CurrentActorInfo != ActorInfo)
	{
		return {};
	}
	if (LastControlledActivationSerial == MAX_uint64)
	{
		OutReason = EGGYGOAbilityActivationRequestReason::IdentityExhausted;
		return {};
	}
	const FGameplayAbilitySpec* Spec = OriginalASC->FindAbilitySpecFromHandle(Handle);
	if (!Spec || !Spec->GetAbilityInstances().Contains(this))
	{
		OutReason = EGGYGOAbilityActivationRequestReason::InvalidSpec;
		return {};
	}
	UGGYGOAbilitySystemComponent::FActualAvatarBindingActorInfoSnapshot Snapshot;
	EGGYGOAvatarBindingReason SnapshotReason;
	if (!OriginalASC->CaptureAvatarBindingActualSnapshot(Snapshot, SnapshotReason)
		|| Snapshot.Allocation.Get() != ActorInfo
		|| !OriginalASC->ValidateAvatarBindingActualSnapshot(Snapshot, SnapshotReason))
	{
		OutReason = EGGYGOAbilityActivationRequestReason::InvalidActorInfo;
		return {};
	}
	TSharedRef<FGGYGOAbilityActivationHandle::FActivationProof> Proof =
		MakeShared<FGGYGOAbilityActivationHandle::FActivationProof>();
	Proof->Ability = this;
	Proof->ASC = OriginalASC;
	Proof->Serial = ++LastControlledActivationSerial;
	Proof->SpecHandle = Handle;
	Proof->ActivationKey = CurrentActivationInfo.GetActivationPredictionKey();
	Proof->BindingContext = OriginalASC->GetAvatarBindingContext();
	Proof->Allocation = Snapshot.Allocation;
	Proof->ActorInfoASC = Snapshot.AbilitySystemComponent;
	Proof->OwnerActor = Snapshot.OwnerActor;
	Proof->AvatarActor = Snapshot.AvatarActor;
	Proof->PlayerController = Snapshot.PlayerController;
	Proof->SkeletalMeshComponent = Snapshot.SkeletalMeshComponent;
	Proof->MovementComponent = Snapshot.MovementComponent;
	Proof->ActorInfoAnimInstance = Snapshot.ActorInfoAnimInstance;
	Proof->ActualAnimInstance = Snapshot.ActualAnimInstance;
	Proof->CachedOwnerActor = Snapshot.CachedOwnerActor;
	Proof->CachedAvatarActor = Snapshot.CachedAvatarActor;
	Proof->ActorInfoAffectedAnimInstanceTag = Snapshot.ActorInfoAffectedAnimInstanceTag;
	Proof->ASCAffectedAnimInstanceTag = Snapshot.ASCAffectedAnimInstanceTag;
	CurrentControlledActivation.Proof = Proof;
	OutReason = EGGYGOAbilityActivationRequestReason::None;
	return CurrentControlledActivation;
}

FGGYGOAbilityActivationHandle UGGYGOGameplayAbility::CaptureCurrentActivation() const
{
	check(IsInGameThread());
	return IsControlledActivationTerminationBusy() ? FGGYGOAbilityActivationHandle{}
		: ValidateCurrentControlledActivation();
}

FGGYGOAbilityActivationHandle UGGYGOGameplayAbility::ValidateCurrentControlledActivation(
	bool bRequireActive, bool bRequireSpec) const
{
	check(IsInGameThread());
	if (!IsValid(this) || !IsInstantiated() || (bRequireActive && !IsActive())
		|| !CurrentControlledActivation.Proof.IsValid())
	{
		return {};
	}
	const auto& Proof = *CurrentControlledActivation.Proof;
	UGGYGOAbilitySystemComponent* ASC = Proof.ASC.Get();
	if (!IsValid(ASC) || Proof.Ability.Get() != this || Proof.Serial == 0
		|| Proof.Serial != LastControlledActivationSerial || CurrentSpecHandle != Proof.SpecHandle
		|| CurrentActivationInfo.GetActivationPredictionKey() != Proof.ActivationKey)
	{
		return {};
	}
	const FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromHandle(Proof.SpecHandle);
	if (bRequireSpec && (!Spec || !Spec->GetAbilityInstances().Contains(this)))
	{
		return {};
	}
	UGGYGOAbilitySystemComponent::FActualAvatarBindingActorInfoSnapshot Original;
	Original.Allocation = Proof.Allocation.Pin();
	Original.AbilitySystemComponent = Proof.ActorInfoASC;
	Original.OwnerActor = Proof.OwnerActor;
	Original.AvatarActor = Proof.AvatarActor;
	Original.PlayerController = Proof.PlayerController;
	Original.SkeletalMeshComponent = Proof.SkeletalMeshComponent;
	Original.MovementComponent = Proof.MovementComponent;
	Original.ActorInfoAnimInstance = Proof.ActorInfoAnimInstance;
	Original.ActualAnimInstance = Proof.ActualAnimInstance;
	Original.CachedOwnerActor = Proof.CachedOwnerActor;
	Original.CachedAvatarActor = Proof.CachedAvatarActor;
	Original.ActorInfoAffectedAnimInstanceTag = Proof.ActorInfoAffectedAnimInstanceTag;
	Original.ASCAffectedAnimInstanceTag = Proof.ASCAffectedAnimInstanceTag;
	UGGYGOAbilitySystemComponent::FActualAvatarBindingActorInfoSnapshot Actual;
	EGGYGOAvatarBindingReason SnapshotReason;
	const FGGYGOAvatarBindingContext Context = ASC->GetAvatarBindingContext();
	if (!Original.Allocation.IsValid() || Original.Allocation.Get() != CurrentActorInfo
		|| Context.Binding.Serial != Proof.BindingContext.Binding.Serial
		|| !Context.Binding.Issuer.HasSameIndexAndSerialNumber(Proof.BindingContext.Binding.Issuer)
		|| Context.LastActorInfoWrite.Serial != Proof.BindingContext.LastActorInfoWrite.Serial
		|| !Context.LastActorInfoWrite.Issuer.HasSameIndexAndSerialNumber(Proof.BindingContext.LastActorInfoWrite.Issuer)
		|| !ASC->CaptureAvatarBindingActualSnapshot(Actual, SnapshotReason)
		|| !ASC->ValidateAvatarBindingActualSnapshot(Actual, SnapshotReason)
		|| !ASC->HasSameAvatarBindingActualSnapshot(Original, Actual))
	{
		return {};
	}
	return CurrentControlledActivation;
}

void UGGYGOGameplayAbility::RetireControlledActivation()
{
	CurrentControlledActivation = {};
}

void UGGYGOGameplayAbility::RetireControlledActivationForNativeEnd(FGameplayAbilitySpecHandle Handle)
{
	// A qualified/uncontrolled End cannot authenticate history: retire provenance, never infer completion.
	if (!ControlledActivationEndScope)
	{
		RetireControlledActivation();
		return;
	}
	const FGGYGOAbilityActivationHandle& Original = ControlledActivationEndScope->Original;
	if (Original.Proof.IsValid() && Original.Proof->SpecHandle == Handle
		&& CurrentControlledActivation.HasSameActivation(Original))
	{
		RetireControlledActivation();
	}
}

UGGYGOGameplayAbility::FScopedControlledActivationEnd::FScopedControlledActivationEnd(
	UGGYGOGameplayAbility* InAbility)
	: Ability(InAbility), Previous(InAbility->ControlledActivationEndScope),
	  Original(InAbility->CurrentControlledActivation), Termination(InAbility->OriginalTermination)
{
	check(IsInGameThread());
	InAbility->ControlledActivationEndScope = this;
	if (Termination.IsValid()) { ++Termination->OpenDispatches; }
}

UGGYGOGameplayAbility::FScopedControlledActivationEnd::~FScopedControlledActivationEnd()
{
	if (UGGYGOGameplayAbility* OriginalAbility = Ability.Get())
	{
		check(OriginalAbility->ControlledActivationEndScope == this);
		if (!OriginalAbility->IsActive() && OriginalAbility->CurrentControlledActivation.HasSameActivation(Original))
		{
			OriginalAbility->RetireControlledActivationForNativeEnd(OriginalAbility->CurrentSpecHandle);
		}
		OriginalAbility->ControlledActivationEndScope = Previous;
	}
	if (Termination.IsValid())
	{
		check(Termination->OpenDispatches > 0);
		--Termination->OpenDispatches;
		UGGYGOGameplayAbility::TryCompleteOriginalTermination(Termination);
	}
}

bool UGGYGOGameplayAbility::IsControlledActivationTerminationBusy() const
{
	return OriginalTermination.IsValid() || ControlledActivationEndScope != nullptr || bIsAbilityEnding;
}

struct FGGYGOAbilityTerminationHandle::FTerminationProof
{
	TWeakObjectPtr<UGGYGOGameplayAbility> Ability;
	uint64 Serial = 0;
	FGGYGOAbilityActivationHandle Activation;
};

FGGYGOAbilityTerminationResult UGGYGOGameplayAbility::GetOriginalTerminationResult(
	const FOriginalTerminationRecord& Record)
{
	FGGYGOAbilityTerminationResult Result;
	Result.Original = Record.Context;
	Result.Outcome = Record.Outcome;
	Result.Reason = Record.Reason;
	return Result;
}

TSharedPtr<UGGYGOGameplayAbility::FOriginalTerminationRecord> UGGYGOGameplayAbility::BeginOriginalTermination(
	const FGGYGOAbilityActivationHandle& Original, EGGYGOAbilityTerminationRequestKind Kind,
	bool bReplicate, bool bWasCancelled, FGGYGOAbilityTerminationResult& OutResult)
{
	check(IsInGameThread());
	using EReason = EGGYGOAbilityTerminationReason;
	using EOutcome = EGGYGOAbilityTerminationOutcome;
	OutResult = {};
	if (!Original.Proof.IsValid()) { return {}; }
	OutResult.Original.OriginalActivation = Original;
	if (Original.Proof->Ability.Get() != this) { OutResult.Reason = EReason::WrongIssuer; return {}; }
	if (OriginalTermination.IsValid())
	{
		if (OriginalTermination->Context.GetOriginalActivation().HasSameActivation(Original))
		{
			OutResult = GetOriginalTerminationResult(*OriginalTermination);
			if (OutResult.Outcome != EOutcome::Failed)
			{
				OutResult.Outcome = EOutcome::AlreadyPending;
			}
		}
		else { OutResult.Outcome = EOutcome::Busy; OutResult.Reason = EReason::TerminationInProgress; }
		return {};
	}
	if (!IsValid(this) || !IsInstantiated()) { OutResult.Reason = EReason::InvalidAbility; return {}; }
	if (!IsActive()) { OutResult.Outcome = EOutcome::Stale; OutResult.Reason = EReason::NotActive; return {}; }
	if (!CaptureCurrentActivation().HasSameActivation(Original))
	{
		OutResult.Outcome = EOutcome::Stale;
		OutResult.Reason = EReason::ActivationChanged;
		return {};
	}
	UGGYGOAbilitySystemComponent* ASC = Original.Proof->ASC.Get();
	const TSharedPtr<const FGameplayAbilityActorInfo> ActorInfo = Original.Proof->Allocation.Pin();
	if (!IsValid(ASC)) { OutResult.Reason = EReason::InvalidASC; return {}; }
	const FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromHandle(Original.Proof->SpecHandle);
	if (!Spec || Spec->PendingRemove) { OutResult.Reason = EReason::InvalidAbility; return {}; }
	if (!ActorInfo.IsValid() || !IsEndAbilityValid(Original.Proof->SpecHandle, ActorInfo.Get()))
	{
		// PreActivate's witness precedes native ActiveCount++; it is not yet endable.
		OutResult.Reason = EReason::NotActive;
		return {};
	}
	if (Kind == EGGYGOAbilityTerminationRequestKind::Cancel && !CanBeCanceled())
	{
		OutResult.Reason = EReason::NotCancelable;
		return {};
	}
	if (LastOriginalTerminationSerial == MAX_uint64) { OutResult.Reason = EReason::IdentityExhausted; return {}; }
	FGGYGOAbilityTerminationContext Context;
	Context.OriginalActivation = Original;
	Context.RequestKind = Kind;
	Context.bReplicateEndAbility = Kind == EGGYGOAbilityTerminationRequestKind::End && bReplicate;
	Context.bReplicateCancelAbility = Kind == EGGYGOAbilityTerminationRequestKind::Cancel && bReplicate;
	Context.bWasCancelled = Kind == EGGYGOAbilityTerminationRequestKind::Cancel || bWasCancelled;
	Context.OriginalMontageCapture = ASC->CaptureMontagePlaybackOwnership(this,
		Original.Proof->SpecHandle, CurrentActivationInfo);
	if (Context.OriginalMontageCapture.Outcome != EGGYGOAbilityMontagePlaybackOutcome::Succeeded
		&& Context.OriginalMontageCapture.Outcome != EGGYGOAbilityMontagePlaybackOutcome::NoOwnedPlayback)
	{
		OutResult.Original = Context;
		OutResult.Outcome = EOutcome::Failed;
		OutResult.Reason = EReason::MontageCaptureFailed;
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("AbilitySystem termination [%s] ASC [%s] Spec [%s] montage capture failed: outcome=%d reason=%d."),
			*GetPathName(), *GetPathNameSafe(ASC), *Original.Proof->SpecHandle.ToString(),
			static_cast<int32>(Context.OriginalMontageCapture.Outcome), static_cast<int32>(Context.OriginalMontageCapture.Reason));
		return {};
	}
	TSharedRef<FGGYGOAbilityTerminationHandle::FTerminationProof> Proof =
		MakeShared<FGGYGOAbilityTerminationHandle::FTerminationProof>();
	Proof->Ability = this;
	Proof->Serial = ++LastOriginalTerminationSerial;
	Proof->Activation = Original;
	Context.OriginalTermination.Proof = Proof;
	TSharedPtr<FOriginalTerminationRecord> Record = MakeShared<FOriginalTerminationRecord>();
	Record->Context = Context;
	Record->Ability = this;
	Record->ASC = ASC;
	Record->ActorInfo = ActorInfo;
	Record->SpecHandle = Original.Proof->SpecHandle;
	Record->ActivationInfo = CurrentActivationInfo;
	Record->CameraOffsetComponent = AppliedCameraOffsetComponent;
	Record->CameraOffsetHandle = AppliedCameraOffsetHandle;
	Record->CameraModeHero = AppliedCameraModeHeroComponent;
	Record->CameraModeSpec = AppliedCameraModeSpecHandle;
	Record->CameraModeGeneration = AppliedCameraModeRequestGeneration;
	OriginalTermination = Record; // Install before any native cancellation/cleanup callback.
	ASC->RegisterOriginalTerminationTryDependencies(Record);
	OutResult = GetOriginalTerminationResult(*Record);
	return Record;
}

EGGYGOAbilityTerminationReason UGGYGOGameplayAbility::CheckOriginalTerminationSource(
	const FOriginalTerminationRecord& Record) const
{
	using EReason = EGGYGOAbilityTerminationReason;
	if (!IsValid(this) || Record.Ability.Get() != this) { return EReason::InvalidAbility; }
	if (!Record.ASC.IsValid()) { return EReason::InvalidASC; }
	if (OriginalTermination.Get() != &Record
		|| !ValidateCurrentControlledActivation().HasSameActivation(Record.Context.GetOriginalActivation()))
	{
		return EReason::ActivationChanged;
	}
	const TSharedPtr<const FGameplayAbilityActorInfo> ActorInfo = Record.ActorInfo.Pin();
	if (!ActorInfo.IsValid() || ActorInfo.Get() != CurrentActorInfo) { return EReason::InvalidActorInfo; }
	const FGameplayAbilitySpec* Spec = Record.ASC->FindAbilitySpecFromHandle(Record.SpecHandle);
	if (!Spec || Spec->PendingRemove) { return EReason::InvalidAbility; }
	return EReason::None;
}

void UGGYGOGameplayAbility::FailOriginalTermination(const TSharedPtr<FOriginalTerminationRecord>& Record,
	EGGYGOAbilityTerminationReason Reason)
{
	if (!Record.IsValid() || Record->bSealed || Record->Outcome == EGGYGOAbilityTerminationOutcome::Failed) { return; }
	Record->Outcome = EGGYGOAbilityTerminationOutcome::Failed;
	Record->Reason = Reason;
	Record->bContinuationQueued = false; // Any retained native delegate can only retire this failed record.
	Record->bContinuationReady = false;
	UE_LOG(LogGGYGOAbilitySystem, Error,
		TEXT("AbilitySystem original termination Ability [%s] ASC [%s] Spec [%s] failed: reason=%d."),
		*GetPathNameSafe(Record->Ability.Get()), *GetPathNameSafe(Record->ASC.Get()),
		*Record->SpecHandle.ToString(), static_cast<int32>(Reason));
}

void UGGYGOGameplayAbility::TryCompleteOriginalTermination(const TSharedPtr<FOriginalTerminationRecord>& Record)
{
	if (!Record.IsValid() || Record->bSealed || Record->OpenDispatches != 0
		|| Record->OpenTryCalls != 0 || Record->bContinuationQueued || Record->bContinuationReady) { return; }
	if (Record->Outcome != EGGYGOAbilityTerminationOutcome::Failed)
	{
		if (!Record->bNativeEndObserved || !Record->bNativeEndReturned || !Record->bFullEndReturned
			|| (Record->Context.GetRequestKind() == EGGYGOAbilityTerminationRequestKind::Cancel && !Record->bCancelReturned))
		{
			FailOriginalTermination(Record, EGGYGOAbilityTerminationReason::NativeEndNotObserved);
		}
		else if (UGGYGOAbilitySystemComponent* ASC = Record->ASC.Get())
		{
			if (ASC->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
			{
				FailOriginalTermination(Record, EGGYGOAbilityTerminationReason::InvalidASC);
			}
			else
			{
				ASC->TryPublishOriginalTerminationCompleted(Record);
				return;
			}
		}
		else { FailOriginalTermination(Record, EGGYGOAbilityTerminationReason::InvalidASC); }
	}
	Record->bSealed = true;
	Record->TryCompletionSlots.Reset();
	if (UGGYGOGameplayAbility* Ability = Record->Ability.Get())
	{
		if (Ability->CurrentControlledActivation.HasSameActivation(Record->Context.GetOriginalActivation()))
		{
			Ability->RetireControlledActivation();
		}
		if (Ability->OriginalTermination == Record) { Ability->OriginalTermination.Reset(); }
	}
}

void UGGYGOGameplayAbility::ResumeOriginalTermination(TSharedPtr<FOriginalTerminationRecord> Record)
{
	check(IsInGameThread());
	if (!Record.IsValid() || Record->bSealed || Record->bDriving) { return; }
	if (Record->Outcome == EGGYGOAbilityTerminationOutcome::Failed) { TryCompleteOriginalTermination(Record); return; }
	const EGGYGOAbilityTerminationReason SourceReason = CheckOriginalTerminationSource(*Record);
	if (SourceReason != EGGYGOAbilityTerminationReason::None)
	{
		FailOriginalTermination(Record, SourceReason);
		TryCompleteOriginalTermination(Record);
		return;
	}
	if (ScopeLockCount > 0)
	{
		DeferOriginalTermination(Record);
		return;
	}
	const TSharedPtr<const FGameplayAbilityActorInfo> ActorInfo = Record->ActorInfo.Pin();
	const bool bEndable = IsEndAbilityValid(Record->SpecHandle, ActorInfo.Get());
	if (!bEndable
		|| (Record->Context.GetRequestKind() == EGGYGOAbilityTerminationRequestKind::Cancel
			&& !Record->bCancelEntered && !CanBeCanceled()))
	{
		FailOriginalTermination(Record, !bEndable ? EGGYGOAbilityTerminationReason::NotActive
			: EGGYGOAbilityTerminationReason::NotCancelable);
		TryCompleteOriginalTermination(Record);
		return;
	}
	Record->Outcome = EGGYGOAbilityTerminationOutcome::Accepted;
	Record->Reason = EGGYGOAbilityTerminationReason::None;
	Record->bDriving = true;
	++Record->OpenDispatches;
	if (Record->Context.GetRequestKind() == EGGYGOAbilityTerminationRequestKind::Cancel && !Record->bCancelEntered)
	{
		CancelAbility(Record->SpecHandle, ActorInfo.Get(), Record->ActivationInfo, Record->Context.GetReplicateCancelAbility());
	}
	else
	{
		EndAbility(Record->SpecHandle, ActorInfo.Get(), Record->ActivationInfo,
			Record->Context.GetReplicateEndAbility(), Record->Context.WasCancelled());
	}
	// Only immutable original history is touched after the virtual call, even if it destroyed GA.
	Record->bDriving = false;
	Record->bFullEndReturned = Record->bNativeEndReturned;
	check(Record->OpenDispatches > 0);
	--Record->OpenDispatches;
	if (Record->bContinuationReady)
	{
		// Native unlock occurred inside the original virtual call. Resume only after that call
		// returned; this consumes its one native delegate, without scheduling another executor.
		Record->bContinuationReady = false;
		if (UGGYGOGameplayAbility* Ability = Record->Ability.Get()) { Ability->ResumeOriginalTermination(Record); }
		else { FailOriginalTermination(Record, EGGYGOAbilityTerminationReason::InvalidAbility); }
	}
	if (!Record->bNativeEndObserved && !Record->bContinuationQueued)
	{
		FailOriginalTermination(Record, EGGYGOAbilityTerminationReason::NativeEndNotObserved);
	}
	TryCompleteOriginalTermination(Record);
}

void UGGYGOGameplayAbility::DeferOriginalTermination(const TSharedPtr<FOriginalTerminationRecord>& Record)
{
	if (Record->bContinuationQueued) { return; }
	if (Record->bContinuationUsed)
	{
		FailOriginalTermination(Record, EGGYGOAbilityTerminationReason::ScopeLocked);
		TryCompleteOriginalTermination(Record);
		return;
	}
	Record->bContinuationUsed = true;
	Record->bContinuationQueued = true;
	Record->Outcome = EGGYGOAbilityTerminationOutcome::Deferred;
	Record->Reason = EGGYGOAbilityTerminationReason::ScopeLocked;
	WaitingToExecute.Add(FPostLockDelegate::CreateLambda([Record]()
	{
		Record->bContinuationQueued = false;
		if (Record->bSealed || Record->Outcome == EGGYGOAbilityTerminationOutcome::Failed)
		{
			UGGYGOGameplayAbility::TryCompleteOriginalTermination(Record);
			return;
		}
		if (Record->bDriving)
		{
			Record->bContinuationReady = true;
			return;
		}
		if (UGGYGOGameplayAbility* Ability = Record->Ability.Get()) { Ability->ResumeOriginalTermination(Record); }
		else
		{
			UGGYGOGameplayAbility::FailOriginalTermination(Record, EGGYGOAbilityTerminationReason::InvalidAbility);
			UGGYGOGameplayAbility::TryCompleteOriginalTermination(Record);
		}
	}));
}

FGGYGOAbilityTerminationResult UGGYGOGameplayAbility::RequestAbilityEnd(
	const FGGYGOAbilityActivationHandle& Original, bool bReplicateEndAbility, bool bWasCancelled)
{
	FGGYGOAbilityTerminationResult Result;
	const TSharedPtr<FOriginalTerminationRecord> Record = BeginOriginalTermination(Original,
		EGGYGOAbilityTerminationRequestKind::End, bReplicateEndAbility, bWasCancelled, Result);
	if (Record.IsValid()) { ResumeOriginalTermination(Record); Result = GetOriginalTerminationResult(*Record); }
	return Result;
}

FGGYGOAbilityTerminationResult UGGYGOGameplayAbility::RequestAbilityCancel(
	const FGGYGOAbilityActivationHandle& Original, bool bReplicateCancelAbility)
{
	FGGYGOAbilityTerminationResult Result;
	const TSharedPtr<FOriginalTerminationRecord> Record = BeginOriginalTermination(Original,
		EGGYGOAbilityTerminationRequestKind::Cancel, bReplicateCancelAbility, true, Result);
	if (Record.IsValid()) { ResumeOriginalTermination(Record); Result = GetOriginalTerminationResult(*Record); }
	return Result;
}

void UGGYGOGameplayAbility::ObserveOriginalNativeEnd(UGGYGOAbilitySystemComponent* OriginalASC,
	FGameplayAbilitySpecHandle Handle)
{
	const TSharedPtr<FOriginalTerminationRecord> Record = OriginalTermination;
	if (Record.IsValid() && !Record->bSealed && Record->bNativeEndStarted
		&& Record->ASC.Get() == OriginalASC && Record->SpecHandle == Handle && ControlledActivationEndScope
		&& ControlledActivationEndScope->Original.HasSameActivation(Record->Context.GetOriginalActivation()))
	{
		const FGameplayAbilitySpec* Spec = OriginalASC->FindAbilitySpecFromHandle(Handle);
		if (!Spec || Spec->PendingRemove || !Spec->GetAbilityInstances().Contains(this))
		{
			FailOriginalTermination(Record, EGGYGOAbilityTerminationReason::InvalidAbility);
			return;
		}
		// Native already cleared IsActive before this notification; compare original provenance
		// and ActorInfo directly, without manufacturing a fresh active identity.
		if (!ValidateCurrentControlledActivation(false).HasSameActivation(Record->Context.GetOriginalActivation()))
		{
			FailOriginalTermination(Record, EGGYGOAbilityTerminationReason::ActivationChanged);
			return;
		}
		Record->bNativeEndObserved = true;
	}
}

void UGGYGOGameplayAbility::InvalidateOriginalTerminationForActivation()
{
	const TSharedPtr<FOriginalTerminationRecord> Record = OriginalTermination;
	if (Record.IsValid())
	{
		FailOriginalTermination(Record, EGGYGOAbilityTerminationReason::ActivationChanged);
		TryCompleteOriginalTermination(Record);
	}
}

UGGYGOGameplayAbility::UGGYGOGameplayAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// 不复制能力对象本身，规格与激活状态由 ASC 复制。
	ReplicationPolicy = EGameplayAbilityReplicationPolicy::ReplicateNo;

	// 每个 Actor 一个实例，运行期状态可以安全放在成员变量里。
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;

	// 客户端先预测再由服务器确认。动作游戏的输入响应感依赖这一条。
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;

	// 两端都能发起请求，具体校验交给 GAS 的网络验证流程。
	NetSecurityPolicy = EGameplayAbilityNetSecurityPolicy::ClientOrServer;

	ActivationPolicy = EGGYGOAbilityActivationPolicy::OnInputTriggered;

	// 默认不参与组仲裁：GroupTag 为空 + Coexist + 最低优先级。
	// 派生能力必须显式配置这三项才会进入仲裁。
	ActivationPriority = GGYGOAbilityGroupDefaults::Priority_Passive;
	SelfPolicy = EGGYGOAbilitySelfPolicy::Coexist;
}

UGGYGOAbilitySystemComponent* UGGYGOGameplayAbility::GetGGYGOAbilitySystemComponentFromActorInfo() const
{
	return (CurrentActorInfo ? Cast<UGGYGOAbilitySystemComponent>(CurrentActorInfo->AbilitySystemComponent.Get()) : nullptr);
}

APlayerController* UGGYGOGameplayAbility::GetPlayerControllerFromActorInfo() const
{
	return (CurrentActorInfo ? Cast<APlayerController>(CurrentActorInfo->PlayerController.Get()) : nullptr);
}

AController* UGGYGOGameplayAbility::GetControllerFromActorInfo() const
{
	if (!CurrentActorInfo)
	{
		return nullptr;
	}

	// 最直接的来源，避免遍历。
	if (AController* PC = CurrentActorInfo->PlayerController.Get())
	{
		return PC;
	}

	// 两级 ASC 布局下角色 ASC 的 Owner 就是角色自己，ActorInfo 里没有 PlayerController，
	// 所以要沿 Owner 链找，最终靠 Pawn->GetController() 拿到控制器。
	AActor* TestActor = CurrentActorInfo->OwnerActor.Get();
	while (TestActor)
	{
		if (AController* C = Cast<AController>(TestActor))
		{
			return C;
		}

		if (APawn* Pawn = Cast<APawn>(TestActor))
		{
			return Pawn->GetController();
		}

		TestActor = TestActor->GetOwner();
	}

	return nullptr;
}

ACharacter* UGGYGOGameplayAbility::GetCharacterFromActorInfo() const
{
	return (CurrentActorInfo ? Cast<ACharacter>(CurrentActorInfo->AvatarActor.Get()) : nullptr);
}

void UGGYGOGameplayAbility::NativeOnAbilityFailedToActivate(const FGameplayTagContainer& FailedReason) const
{
	// 文本只发一条：同时缺耐力又在冷却时，弹两条提示反而更糟。
	bool bSimpleFailureFound = false;

	for (const FGameplayTag& Reason : FailedReason)
	{
		if (!bSimpleFailureFound)
		{
			if (const FText* UserFacingMessage = FailureTagToUserFacingMessages.Find(Reason))
			{
				FGGYGOAbilitySimpleFailureMessage Message;
				Message.PlayerController = GetActorInfo().PlayerController.Get();
				// 带上完整原因集合，接收方可以做更细的判断。
				Message.FailureTags = FailedReason;
				Message.UserFacingReason = *UserFacingMessage;

				UGameplayMessageSubsystem& MessageSystem = UGameplayMessageSubsystem::Get(GetWorld());
				MessageSystem.BroadcastMessage(TAG_GGYGO_Ability_SimpleFailureMessage, Message);

				bSimpleFailureFound = true;
			}
		}

		// Montage 逐个 Tag 都发：不同失败原因可能配了不同的失败动作。
		if (UAnimMontage* Montage = FailureTagToAnimMontage.FindRef(Reason))
		{
			FGGYGOAbilityMontageFailureMessage Message;
			Message.PlayerController = GetActorInfo().PlayerController.Get();
			Message.AvatarActor = GetActorInfo().AvatarActor.Get();
			Message.FailureTags = FailedReason;
			Message.FailureMontage = Montage;

			UGameplayMessageSubsystem& MessageSystem = UGameplayMessageSubsystem::Get(GetWorld());
			MessageSystem.BroadcastMessage(TAG_GGYGO_Ability_PlayMontageFailureMessage, Message);
		}
	}
}

bool UGGYGOGameplayAbility::CanActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags, FGameplayTagContainer* OptionalRelevantTags) const
{
	UGGYGOAbilitySystemComponent* EvaluationASC = ActorInfo
		? Cast<UGGYGOAbilitySystemComponent>(ActorInfo->AbilitySystemComponent.Get()) : nullptr;
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalEvaluationASC(EvaluationASC);
	const bool bHadProjectEvaluationASC = EvaluationASC != nullptr;
	UGGYGOAbilitySystemComponent::FScopedAbilityActivationEvaluation Evaluation(
		EvaluationASC, this, Handle, ActorInfo);
	const uint64 ControlledEvaluationSerial = EvaluationASC
		? EvaluationASC->BeginControlledAbilityActivationEvaluation(this, Handle, ActorInfo) : 0;
	bool bCanActivate = [&]() -> bool
	{
		if (!ActorInfo || !ActorInfo->AbilitySystemComponent.IsValid())
		{
			return false;
		}

		// 父类先做冷却、消耗、Tag 需求等通用检查。
		if (!Super::CanActivateAbility(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags))
		{
			return false;
		}

		// 再做组仲裁。用 Cast 而不是 CastChecked：Ability 可能被授予到非项目 ASC 上
		// （例如测试用的裸 ASC），那种情况下跳过组检查而不是崩掉。
		// Native/BP/cost callbacks may invalidate or replace the receiver. Reacquire only the original ASC.
		if (const UGGYGOAbilitySystemComponent* GGYGOASC = OriginalEvaluationASC.Get())
		{
			EGGYGOAbilityGroupBlockReason BlockReason = EGGYGOAbilityGroupBlockReason::NotBlocked;
			if (GGYGOASC->IsActivationBlockedByGroup(this, BlockReason))
			{
				if (OptionalRelevantTags)
				{
					// 把"该不该重试"编码进失败 Tag，意图层不必反查配置表就能决定
					// 把请求留在缓冲里还是丢弃。
					OptionalRelevantTags->AddTag(BlockReason == EGGYGOAbilityGroupBlockReason::GroupOccupiedQueued
						? GGYGOGameplayTags::Ability_ActivateFail_ActivationGroupQueued
						: GGYGOGameplayTags::Ability_ActivateFail_ActivationGroup);
				}
				return false;
			}
		}
		else if (bHadProjectEvaluationASC)
		{
			return false;
		}

		return CanActivateAbilityAdditional(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags);
	}();
	if (ControlledEvaluationSerial != 0)
	{
		UGGYGOAbilitySystemComponent* OriginalASC = OriginalEvaluationASC.Get();
		bCanActivate = OriginalASC
			? OriginalASC->CompleteControlledAbilityActivationEvaluation(ControlledEvaluationSerial, bCanActivate)
			: false;
	}
	Evaluation.Complete(bCanActivate);
	return bCanActivate;
}

bool UGGYGOGameplayAbility::CanActivateAbilityAdditional(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags, FGameplayTagContainer* OptionalRelevantTags) const
{
	// 明确的默认模式：没有附加准入条件；核心检查由 final 入口先完成。
	return true;
}

void UGGYGOGameplayAbility::SetCanBeCanceled(bool bCanBeCanceled)
{
	// 只有 Exclusive 能力可以拒绝被取消。
	// Coexist 能力随时可能被同组高优先级或跨组 Exclusive 顶掉，
	// 如果它声明自己不可取消，仲裁就无法执行，组规则会失效。
	if (!bCanBeCanceled && (SelfPolicy != EGGYGOAbilitySelfPolicy::Exclusive))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("SetCanBeCanceled: 能力 [%s] 不能拒绝取消，因为它的 SelfPolicy 不是 Exclusive。"),
			*GetName());
		return;
	}

	Super::SetCanBeCanceled(bCanBeCanceled);
}

void UGGYGOGameplayAbility::OnGiveAbility(const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilitySpec& Spec)
{
	Super::OnGiveAbility(ActorInfo, Spec);

	K2_OnAbilityAdded();

	TryActivateAbilityOnSpawn(ActorInfo, Spec);
}

void UGGYGOGameplayAbility::OnRemoveAbility(const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilitySpec& Spec)
{
	// 先让蓝图在状态还完整时做清理，再交给父类。
	K2_OnAbilityRemoved();

	Super::OnRemoveAbility(ActorInfo, Spec);
}

bool UGGYGOGameplayAbility::CheckCost(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, OUT FGameplayTagContainer* OptionalRelevantTags) const
{
	if (!Super::CheckCost(Handle, ActorInfo, OptionalRelevantTags) || !ActorInfo)
	{
		return false;
	}

	// 按资产配置顺序检查，保证行为可预期。
	for (const TObjectPtr<UGGYGOAbilityCost>& AdditionalCost : AdditionalCosts)
	{
		// 允许数组里有空槽，不当作失败。
		if (AdditionalCost != nullptr)
		{
			if (!AdditionalCost->CheckCost(this, Handle, ActorInfo, /*inout*/ OptionalRelevantTags))
			{
				// 一项付不起就直接拒绝，不必检查剩下的。
				return false;
			}
		}
	}

	return true;
}

void UGGYGOGameplayAbility::ApplyCost(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo) const
{
	Super::ApplyCost(Handle, ActorInfo, ActivationInfo);

	check(ActorInfo);

	// 判断本次能力是否真的命中了目标。只有服务器有权威的命中数据，
	// 客户端预测端不能据此扣除"命中才扣"的消耗。
	auto DetermineIfAbilityHitTarget = [&]()
	{
		if (ActorInfo->IsNetAuthority())
		{
			if (UGGYGOAbilitySystemComponent* ASC = Cast<UGGYGOAbilitySystemComponent>(ActorInfo->AbilitySystemComponent.Get()))
			{
				FGameplayAbilityTargetDataHandle TargetData;
				ASC->GetAbilityTargetData(Handle, ActivationInfo, TargetData);

				for (int32 TargetDataIdx = 0; TargetDataIdx < TargetData.Data.Num(); ++TargetDataIdx)
				{
					if (UAbilitySystemBlueprintLibrary::TargetDataHasHitResult(TargetData, TargetDataIdx))
					{
						return true;
					}
				}
			}
		}

		return false;
	};

	// 缓存命中判定结果，避免多个"命中才扣"的消耗重复查询同一份目标数据。
	bool bAbilityHitTarget = false;
	bool bHasDeterminedIfAbilityHitTarget = false;

	for (const TObjectPtr<UGGYGOAbilityCost>& AdditionalCost : AdditionalCosts)
	{
		if (AdditionalCost != nullptr)
		{
			if (AdditionalCost->ShouldOnlyApplyCostOnHit())
			{
				// 惰性求值：没有"命中才扣"的消耗时完全不查目标数据。
				if (!bHasDeterminedIfAbilityHitTarget)
				{
					bAbilityHitTarget = DetermineIfAbilityHitTarget();
					bHasDeterminedIfAbilityHitTarget = true;
				}

				if (!bAbilityHitTarget)
				{
					continue;
				}
			}

			AdditionalCost->ApplyCost(this, Handle, ActorInfo, ActivationInfo);
		}
	}
}

FGameplayEffectContextHandle UGGYGOGameplayAbility::MakeEffectContext(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo) const
{
	FGameplayEffectContextHandle ContextHandle = Super::MakeEffectContext(Handle, ActorInfo);

	// 这里 check 而不是判空跳过：拿不到自定义上下文说明
	// DefaultGame.ini 里的 AbilitySystemGlobalsClassName 没配好，
	// 静默降级会让伤害衰减和材质分流失效且很难排查。
	FGGYGOGameplayEffectContext* EffectContext = FGGYGOGameplayEffectContext::ExtractEffectContext(ContextHandle);
	check(EffectContext);

	check(ActorInfo);

	AActor* EffectCauser = nullptr;
	const IGGYGOAbilitySourceInterface* AbilitySource = nullptr;
	float SourceLevel = 0.0f;
	GetAbilitySource(Handle, ActorInfo, /*out*/ SourceLevel, /*out*/ AbilitySource, /*out*/ EffectCauser);

	UObject* SourceObject = GetSourceObject(Handle, ActorInfo);

	AActor* Instigator = ActorInfo->OwnerActor.Get();

	EffectContext->SetAbilitySource(AbilitySource, SourceLevel);
	EffectContext->AddInstigator(Instigator, EffectCauser);
	EffectContext->AddSourceObject(SourceObject);

	return ContextHandle;
}

void UGGYGOGameplayAbility::ApplyAbilityTagsToGameplayEffectSpec(FGameplayEffectSpec& Spec, FGameplayAbilitySpec* AbilitySpec) const
{
	Super::ApplyAbilityTagsToGameplayEffectSpec(Spec, AbilitySpec);

	// 把命中表面的 Tag 并进目标 Tag，让 Cue 能按材质分流、Execution 能按材质减伤。
	if (const FHitResult* HitResult = Spec.GetContext().GetHitResult())
	{
		AppendPhysicalMaterialTags(*HitResult, Spec.CapturedTargetTags.GetSpecTags());
	}
}

bool UGGYGOGameplayAbility::BuildHitEffectPayload(UAbilitySystemComponent* TargetAbilitySystemComponent,
	TSubclassOf<UGameplayEffect> DamageEffectClass, float EffectLevel,
	const FHitResult& HitResult, const FVector& Origin,
	FGGYGOHitEffectPayload& OutPayload) const
{
	OutPayload = FGGYGOHitEffectPayload();
	FGGYGOHitEffectPayload Payload;
	const FGameplayAbilitySpecHandle OriginalSpecHandle = CurrentSpecHandle;
	UAbilitySystemComponent* SourceAbilitySystemComponent = CurrentActorInfo
		? GetAbilitySystemComponentFromActorInfo() : nullptr;
	const TWeakObjectPtr<const UGGYGOGameplayAbility> OriginalAbility(this);
	const TWeakObjectPtr<UAbilitySystemComponent> OriginalSource(SourceAbilitySystemComponent);
	const TWeakObjectPtr<UAbilitySystemComponent> OriginalTarget(TargetAbilitySystemComponent);
	const bool bRequiresEffectSpec = DamageEffectClass != nullptr;
	const TWeakObjectPtr<UClass> OriginalEffectClass(DamageEffectClass.Get());
	// Diagnostic strings survive real extension callbacks destroying their source objects.
	const FString AbilityPath = GetPathNameSafe(this);
	const FString SourcePath = GetPathNameSafe(SourceAbilitySystemComponent);
	const FString TargetPath = GetPathNameSafe(TargetAbilitySystemComponent);
	const FString EffectClassPath = GetPathNameSafe(DamageEffectClass.Get());
	const auto Fail = [&](const TCHAR* Reason)
	{
		OutPayload = FGGYGOHitEffectPayload();
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("AbilitySystem BuildHitEffectPayload Ability [%s] Source [%s] Target [%s] GE [%s] failed: %s."),
			*AbilityPath, *SourcePath, *TargetPath, *EffectClassPath, Reason);
		return false;
	};
	const auto HasOriginalObjects = [&]()
	{
		return OriginalAbility.IsValid() && OriginalSource.IsValid() && OriginalTarget.IsValid();
	};
	const auto HasValidRequiredSpec = [&]()
	{
		return OriginalEffectClass.IsValid() && Payload.EffectSpec.IsValid()
			&& IsValid(Payload.EffectSpec.Data->Def.Get())
			&& Payload.EffectSpec.Data->GetContext().IsValid();
	};
	if (!CurrentActorInfo) { return Fail(TEXT("MissingActorInfo")); }
	if (!OriginalSource.IsValid()) { return Fail(TEXT("InvalidSourceASC")); }
	if (!OriginalTarget.IsValid()) { return Fail(TEXT("InvalidTargetASC")); }
	if (bRequiresEffectSpec && !OriginalEffectClass.IsValid()) { return Fail(TEXT("InvalidRequiredGEClass")); }

	Payload.EffectContext = MakeEffectContext(OriginalSpecHandle, CurrentActorInfo);
	if (!HasOriginalObjects()) { return Fail(TEXT("SourceOrTargetInvalidAfterMakeEffectContext")); }
	if (!Payload.EffectContext.IsValid()) { return Fail(TEXT("InvalidEffectContext")); }

	// AddHitResult(reset=true) 会用 TraceStart 改写 Origin，所以显式 Origin 必须最后写入。
	Payload.EffectContext.AddHitResult(HitResult, /*bReset=*/true);
	FGGYGOGameplayEffectContext* GGYGOContext =
		FGGYGOGameplayEffectContext::ExtractEffectContext(Payload.EffectContext);
	check(GGYGOContext);
	GGYGOContext->SetSourceOriginSnapshot(Origin);

	if (bRequiresEffectSpec)
	{
		Payload.EffectSpec = SourceAbilitySystemComponent->MakeOutgoingSpec(
			DamageEffectClass, EffectLevel, Payload.EffectContext);
		if (!HasOriginalObjects()) { return Fail(TEXT("SourceOrTargetInvalidAfterMakeOutgoingSpec")); }
		if (!HasValidRequiredSpec()) { return Fail(TEXT("RequiredSpecCreationFailed")); }
		FGameplayAbilitySpec* AbilitySpec = SourceAbilitySystemComponent->FindAbilitySpecFromHandle(OriginalSpecHandle);
		const bool bHasAbilitySpec = AbilitySpec != nullptr;
		const TMap<FGameplayTag, float> OriginalSetByCallerMagnitudes = AbilitySpec
			? AbilitySpec->SetByCallerTagMagnitudes : TMap<FGameplayTag, float>{};
		ApplyAbilityTagsToGameplayEffectSpec(*Payload.EffectSpec.Data.Get(), AbilitySpec);
		if (!HasOriginalObjects()) { return Fail(TEXT("SourceOrTargetInvalidAfterApplyAbilityTags")); }
		if (!HasValidRequiredSpec()) { return Fail(TEXT("RequiredSpecInvalidAfterApplyAbilityTags")); }

		// The extension may remove/reenter the native Spec; use this hit's original values.
		if (bHasAbilitySpec)
		{
			Payload.EffectSpec.Data->SetByCallerTagMagnitudes = OriginalSetByCallerMagnitudes;
		}
		BP_EditSpecValues(Payload.EffectSpec);
		if (!HasOriginalObjects()) { return Fail(TEXT("SourceOrTargetInvalidAfterBP_EditSpecValues")); }
		if (!HasValidRequiredSpec()) { return Fail(TEXT("RequiredSpecInvalidAfterBP_EditSpecValues")); }
		UAbilitySystemGlobals::Get().InitGameplayCueParameters_GESpec(
			Payload.CueParameters, *Payload.EffectSpec.Data.Get());
		if (!HasValidRequiredSpec()) { return Fail(TEXT("RequiredSpecInvalidAfterCueInitialization")); }
	}
	else
	{
		UAbilitySystemGlobals::Get().InitGameplayCueParameters(
			Payload.CueParameters, Payload.EffectContext);
	}
	if (!HasOriginalObjects()) { return Fail(TEXT("SourceOrTargetInvalidAfterCueInitialization")); }
	if (!Payload.EffectContext.IsValid()) { return Fail(TEXT("EffectContextInvalidAfterCueInitialization")); }

	// 带输出参数的 GetOwnedGameplayTags 会先 Reset 容器；这里读取 const 集合后追加，
	// 才不会覆盖 GESpec 已聚合的 Tag。物理材质最后追加，保证有/无 GE 两条路径一致。
	Payload.CueParameters.AggregatedTargetTags.AppendTags(
		TargetAbilitySystemComponent->GetOwnedGameplayTags());
	AppendPhysicalMaterialTags(HitResult, Payload.CueParameters.AggregatedTargetTags);
	Payload.CueParameters.Location = HitResult.ImpactPoint;
	Payload.CueParameters.Normal = HitResult.ImpactNormal;
	Payload.CueParameters.Instigator = Payload.EffectContext.GetInstigator();
	Payload.CueParameters.EffectCauser = Payload.EffectContext.GetEffectCauser();
	OutPayload = MoveTemp(Payload);

	return true;
}

bool UGGYGOGameplayAbility::DoesAbilitySatisfyTagRequirements(const UAbilitySystemComponent& AbilitySystemComponent, const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags, OUT FGameplayTagContainer* OptionalRelevantTags) const
{
	// 相比父类多做两件事：展开 ASC 的 Tag 关系表，以及把"因死亡失败"单独标记出来。

	bool bBlocked = false;
	bool bMissing = false;

	UAbilitySystemGlobals& AbilitySystemGlobals = UAbilitySystemGlobals::Get();
	const FGameplayTag& BlockedTag = AbilitySystemGlobals.ActivateFailTagsBlockedTag;
	const FGameplayTag& MissingTag = AbilitySystemGlobals.ActivateFailTagsMissingTag;

	if (AbilitySystemComponent.AreAbilityTagsBlocked(GetAssetTags()))
	{
		bBlocked = true;
	}

	const UGGYGOAbilitySystemComponent* GGYGOASC = Cast<UGGYGOAbilitySystemComponent>(&AbilitySystemComponent);

	// 复制一份再扩展，绝不能直接改资产上的 ActivationRequiredTags / ActivationBlockedTags。
	FGameplayTagContainer AllRequiredTags = ActivationRequiredTags;
	FGameplayTagContainer AllBlockedTags = ActivationBlockedTags;

	if (GGYGOASC)
	{
		GGYGOASC->GetAdditionalActivationTagRequirements(GetAssetTags(), AllRequiredTags, AllBlockedTags);
	}

	if (AllBlockedTags.Num() || AllRequiredTags.Num())
	{
		FGameplayTagContainer AbilitySystemComponentTags;
		AbilitySystemComponent.GetOwnedGameplayTags(AbilitySystemComponentTags);

		if (AbilitySystemComponentTags.HasAny(AllBlockedTags))
		{
			// 死亡是最常见的失败原因，单独给一个 Tag，方便表现层区别对待
			// （死亡时不该弹"耐力不足"这类提示）。
			if (OptionalRelevantTags && AbilitySystemComponentTags.HasTag(GGYGOGameplayTags::State_Dead))
			{
				OptionalRelevantTags->AddTag(GGYGOGameplayTags::Ability_ActivateFail_IsDead);
			}

			bBlocked = true;
		}

		if (!AbilitySystemComponentTags.HasAll(AllRequiredTags))
		{
			bMissing = true;
		}
	}

	if (SourceTags != nullptr)
	{
		if (SourceBlockedTags.Num() || SourceRequiredTags.Num())
		{
			if (SourceTags->HasAny(SourceBlockedTags))
			{
				bBlocked = true;
			}

			if (!SourceTags->HasAll(SourceRequiredTags))
			{
				bMissing = true;
			}
		}
	}

	if (TargetTags != nullptr)
	{
		if (TargetBlockedTags.Num() || TargetRequiredTags.Num())
		{
			if (TargetTags->HasAny(TargetBlockedTags))
			{
				bBlocked = true;
			}

			if (!TargetTags->HasAll(TargetRequiredTags))
			{
				bMissing = true;
			}
		}
	}

	// blocked 优先于 missing：两者同时成立时只报阻断，反馈更准确。
	if (bBlocked)
	{
		if (OptionalRelevantTags && BlockedTag.IsValid())
		{
			OptionalRelevantTags->AddTag(BlockedTag);
		}
		return false;
	}

	if (bMissing)
	{
		if (OptionalRelevantTags && MissingTag.IsValid())
		{
			OptionalRelevantTags->AddTag(MissingTag);
		}
		return false;
	}

	return true;
}

void UGGYGOGameplayAbility::OnPawnAvatarSet()
{
	K2_OnPawnAvatarSet();
}

void UGGYGOGameplayAbility::ReceiveAbilityCorrection(const FGameplayAbilityTargetDataHandle& Correction)
{
	// 默认基类只提供安全分发入口；业务载荷由派生能力解释。
}

uint64 UGGYGOGameplayAbility::BeginAbilityGroupAdmissionAttempt(uint64 AdmissionSequence)
{
	// ASC 分配的序号跨实例单调递增；0 表示分配失败，按拒绝处理。
	FAbilityGroupAdmissionAttempt& Attempt = AbilityGroupAdmissionAttempts.AddDefaulted_GetRef();
	Attempt.Sequence = AdmissionSequence;
	Attempt.bRejected = AdmissionSequence == 0;
	return Attempt.Sequence;
}

uint64 UGGYGOGameplayAbility::GetCurrentAbilityGroupAdmissionSequence() const
{
	return AbilityGroupAdmissionAttempts.IsEmpty() ? 0 : AbilityGroupAdmissionAttempts.Last().Sequence;
}

void UGGYGOGameplayAbility::RejectCurrentAbilityGroupAdmission()
{
	RejectAbilityGroupAdmission(GetCurrentAbilityGroupAdmissionSequence());
}

void UGGYGOGameplayAbility::RejectAbilityGroupAdmission(uint64 AdmissionSequence)
{
	if (FAbilityGroupAdmissionAttempt* Attempt = AbilityGroupAdmissionAttempts.FindByPredicate(
		[AdmissionSequence](const FAbilityGroupAdmissionAttempt& Candidate) { return Candidate.Sequence == AdmissionSequence; }))
	{
		Attempt->bRejected = true;
	}
}

bool UGGYGOGameplayAbility::IsCurrentAbilityGroupAdmissionRejected() const
{
	return !AbilityGroupAdmissionAttempts.IsEmpty() && AbilityGroupAdmissionAttempts.Last().bRejected;
}

bool UGGYGOGameplayAbility::IsAbilityGroupAdmissionRejected(uint64 AdmissionSequence) const
{
	const FAbilityGroupAdmissionAttempt* Attempt = AbilityGroupAdmissionAttempts.FindByPredicate(
		[AdmissionSequence](const FAbilityGroupAdmissionAttempt& Candidate) { return Candidate.Sequence == AdmissionSequence; });
	return Attempt && Attempt->bRejected;
}

bool UGGYGOGameplayAbility::IsAbilityGroupAdmissionPending(uint64 AdmissionSequence) const
{
	return AbilityGroupAdmissionAttempts.ContainsByPredicate(
		[AdmissionSequence](const FAbilityGroupAdmissionAttempt& Candidate) { return Candidate.Sequence == AdmissionSequence; });
}

bool UGGYGOGameplayAbility::ConsumeAbilityGroupAdmissionRejection(uint64 AdmissionSequence)
{
	FAbilityGroupAdmissionAttempt* Attempt = AbilityGroupAdmissionAttempts.FindByPredicate(
		[AdmissionSequence](const FAbilityGroupAdmissionAttempt& Candidate) { return Candidate.Sequence == AdmissionSequence; });
	const bool bRejected = Attempt && Attempt->bRejected;
	if (Attempt)
	{
		Attempt->bRejected = false;
	}
	return bRejected;
}

void UGGYGOGameplayAbility::CompleteAbilityGroupAdmissionAttempt(uint64 AdmissionSequence)
{
	AbilityGroupAdmissionAttempts.RemoveAll([AdmissionSequence](const FAbilityGroupAdmissionAttempt& Attempt)
	{
		return Attempt.Sequence == AdmissionSequence;
	});
}

void UGGYGOGameplayAbility::GetAbilitySource(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, float& OutSourceLevel, const IGGYGOAbilitySourceInterface*& OutAbilitySource, AActor*& OutEffectCauser) const
{
	// 先给确定的默认值，避免调用方读到未初始化数据。
	OutSourceLevel = 0.0f;
	OutAbilitySource = nullptr;
	OutEffectCauser = nullptr;

	// 默认由 Avatar 承担"造成伤害的物体"。武器类能力可以在派生实现里换成武器 Actor。
	OutEffectCauser = ActorInfo->AvatarActor.Get();

	// SourceObject 实现了来源接口时才提供衰减信息（例如武器实例）。
	UObject* SourceObject = GetSourceObject(Handle, ActorInfo);
	OutAbilitySource = Cast<IGGYGOAbilitySourceInterface>(SourceObject);
}

void UGGYGOGameplayAbility::TryActivateAbilityOnSpawn(const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilitySpec& Spec) const
{
	if (ActorInfo && !Spec.IsActive() && (ActivationPolicy == EGGYGOAbilityActivationPolicy::OnSpawn))
	{
		UAbilitySystemComponent* ASC = ActorInfo->AbilitySystemComponent.Get();
		const AActor* AvatarActor = ActorInfo->AvatarActor.Get();

		// 正在断开或即将销毁的 Avatar 不激活，等新 Avatar 绑定后重新走授予流程。
		if (ASC && AvatarActor && !AvatarActor->GetTearOff() && (AvatarActor->GetLifeSpan() <= 0.0f))
		{
			const bool bIsLocalExecution = (NetExecutionPolicy == EGameplayAbilityNetExecutionPolicy::LocalPredicted) || (NetExecutionPolicy == EGameplayAbilityNetExecutionPolicy::LocalOnly);
			const bool bIsServerExecution = (NetExecutionPolicy == EGameplayAbilityNetExecutionPolicy::ServerOnly) || (NetExecutionPolicy == EGameplayAbilityNetExecutionPolicy::ServerInitiated);

			const bool bClientShouldActivate = ActorInfo->IsLocallyControlled() && bIsLocalExecution;
			const bool bServerShouldActivate = ActorInfo->IsNetAuthority() && bIsServerExecution;

			// 只有角色与策略匹配的一端发起，避免两端重复激活。
			if (bClientShouldActivate || bServerShouldActivate)
			{
				ASC->TryActivateAbility(Spec.Handle);
			}
		}
	}
}

void UGGYGOGameplayAbility::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	const uint64 AdmissionSequence = GetCurrentAbilityGroupAdmissionSequence();
	// Current instance may already have been rejected by a newer attempt in a PreActivate callback.
	// In that case it must not run its own resolver and cancel any more abilities.
	const bool bRejectedBeforeFinalization = IsAbilityGroupAdmissionRejected(AdmissionSequence);
	bool bResolverAdmitted = true;
	UGGYGOAbilitySystemComponent* GGYGOASC = ActorInfo
		? Cast<UGGYGOAbilitySystemComponent>(ActorInfo->AbilitySystemComponent.Get())
		: nullptr;
	if (!bRejectedBeforeFinalization && GGYGOASC && AdmissionSequence != 0)
	{
		bResolverAdmitted = GGYGOASC->FinalizeAbilityGroupAdmission(this, AdmissionSequence);
	}
	// A recursive newer attempt may reject this one while the resolver cancels competitors.
	const bool bRejectedAfterFinalization = ConsumeAbilityGroupAdmissionRejection(AdmissionSequence);
	const bool bAdmissionRejected = bRejectedBeforeFinalization || !bResolverAdmitted || bRejectedAfterFinalization;

	// Keep pending set throughout Resolve/Consume so recursive cancellation only marks this attempt.
	CompleteAbilityGroupAdmissionAttempt(AdmissionSequence);
	if (bAdmissionRejected)
	{
		// PreActivate 尚不能 End（Spec.ActiveCount 未增加）；进入这里时 GAS 已完成递增，
		// 因此可安全结束并阻止镜头与蓝图业务启动。
		EndAbility(Handle, ActorInfo, ActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/true);
		return;
	}

	// 先应用相机配置再调父类。父类会触发蓝图的激活事件，
	// 蓝图里可能立刻用 SetCameraMode 覆盖成别的模式，那应当赢。
	if (AbilityCameraMode)
	{
		SetCameraMode(AbilityCameraMode);
	}

	if (!CameraOffset.IsNearlyZero())
	{
		ApplyCameraOffset(CameraOffset);
	}

	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);
}

void UGGYGOGameplayAbility::SetCameraMode(TSubclassOf<UGGYGOCameraMode> CameraMode)
{
	if (OriginalTermination.IsValid())
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning, TEXT("AbilitySystem camera mode [%s] asset [%s] rejected during original termination."),
			*GetPathName(), *GetNameSafe(CameraMode.Get()));
		return;
	}
	// 能力实例是 InstancedPerActor；显式替换时先释放它自己保存的旧接收者请求。
	ClearCameraMode();

	if (!CameraMode)
	{
		return;
	}

	// HeroComponent 保存“当前有效模式”及其所有者。CameraComponent 每次被引擎拉取时
	// 再读取这个结果，因此能力不需要自己每帧重复 Push。
	if (UGGYGOHeroComponent* HeroComponent = UGGYGOHeroComponent::FindHeroComponent(GetAvatarActorFromActorInfo()))
	{
		const uint64 RequestGeneration = HeroComponent->SetAbilityCameraMode(CameraMode, CurrentSpecHandle);
		if (RequestGeneration != 0)
		{
			AppliedCameraModeHeroComponent = HeroComponent;
			AppliedCameraModeSpecHandle = CurrentSpecHandle;
			AppliedCameraModeRequestGeneration = RequestGeneration;
			ActiveCameraMode = CameraMode;
		}
	}
}

void UGGYGOGameplayAbility::ClearCameraMode()
{
	if (AppliedCameraModeRequestGeneration != 0)
	{
		if (UGGYGOHeroComponent* HeroComponent = AppliedCameraModeHeroComponent.Get())
		{
			HeroComponent->ClearAbilityCameraMode(AppliedCameraModeSpecHandle, AppliedCameraModeRequestGeneration);
		}
	}

	// 不按当前 Avatar 重新查找接收者。Avatar 变化后旧请求不转移到新 Pawn。
	AppliedCameraModeHeroComponent.Reset();
	AppliedCameraModeSpecHandle = FGameplayAbilitySpecHandle();
	AppliedCameraModeRequestGeneration = 0;
	ActiveCameraMode = nullptr;
}

void UGGYGOGameplayAbility::ApplyCameraOffset(const FGGYGOCameraOffset& Offset)
{
	if (OriginalTermination.IsValid())
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning, TEXT("AbilitySystem camera offset [%s] rejected during original termination."), *GetPathName());
		return;
	}
	// Offset 是相机组件的单槽资源；替换时只撤销本能力持有的旧 token。
	ClearCameraOffset();

	if (UGGYGOCameraComponent* CameraComponent = UGGYGOCameraComponent::FindCameraComponent(GetAvatarActorFromActorInfo()))
	{
		const FGGYGOCameraOffsetHandle Handle = CameraComponent->SetCameraOffset(Offset);
		if (Handle.IsValid())
		{
			AppliedCameraOffsetHandle = Handle;
			AppliedCameraOffsetComponent = CameraComponent;
		}
	}
}

void UGGYGOGameplayAbility::ClearCameraOffset()
{
	if (UGGYGOCameraComponent* CameraComponent = AppliedCameraOffsetComponent.Get())
	{
		CameraComponent->ClearCameraOffset(AppliedCameraOffsetHandle);
	}

	// 清理只归还申请时保存的相机与 token；当前 Avatar 不参与资源查找。
	AppliedCameraOffsetComponent.Reset();
	AppliedCameraOffsetHandle = FGGYGOCameraOffsetHandle();
}

void UGGYGOGameplayAbility::CleanupAbilityResourcesForTermination(const FGGYGOAbilityTerminationContext& Context)
{
	const TSharedPtr<FOriginalTerminationRecord> Record = OriginalTermination;
	if (!Record.IsValid() || !Record->Context.GetOriginalTermination().HasSameTermination(Context.GetOriginalTermination()))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("AbilitySystem original camera cleanup [%s] rejected: termination source mismatch."), *GetPathName());
		return;
	}
	// Detach only matching original members before calling their original receivers.
	if (AppliedCameraOffsetComponent == Record->CameraOffsetComponent && AppliedCameraOffsetHandle == Record->CameraOffsetHandle)
	{
		AppliedCameraOffsetComponent.Reset();
		AppliedCameraOffsetHandle = {};
	}
	if (AppliedCameraModeHeroComponent == Record->CameraModeHero && AppliedCameraModeSpecHandle == Record->CameraModeSpec
		&& AppliedCameraModeRequestGeneration == Record->CameraModeGeneration)
	{
		AppliedCameraModeHeroComponent.Reset();
		AppliedCameraModeSpecHandle = {};
		AppliedCameraModeRequestGeneration = 0;
		ActiveCameraMode = nullptr;
	}
	if (UGGYGOCameraComponent* Camera = Record->CameraOffsetComponent.Get())
	{
		Camera->ClearCameraOffset(Record->CameraOffsetHandle);
	}
	if (Record->CameraModeGeneration != 0)
	{
		if (UGGYGOHeroComponent* Hero = Record->CameraModeHero.Get())
		{
			Hero->ClearAbilityCameraMode(Record->CameraModeSpec, Record->CameraModeGeneration);
		}
	}
}

void UGGYGOGameplayAbility::CancelAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateCancelAbility)
{
	check(IsInGameThread());
	TSharedPtr<FOriginalTerminationRecord> Record = OriginalTermination;
	if (!Record.IsValid())
	{
		const FGGYGOAbilityActivationHandle Original = CaptureCurrentActivation();
		if (!Original.HasActivation())
		{
			if (CurrentControlledActivation.HasActivation())
			{
				UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("AbilitySystem native Cancel [%s] Spec [%s] rejected: issued controlled source is invalid or still ending."),
					*GetPathName(), *Handle.ToString());
				return;
			}
			// Transitional native entry: preserves untracked GAS behavior, issues no protocol completion.
			UE_LOG(LogGGYGOAbilitySystem, Verbose, TEXT("AbilitySystem native Cancel [%s] Spec [%s] has no controlled source; termination protocol unsupported."),
				*GetPathName(), *Handle.ToString());
			Super::CancelAbility(Handle, ActorInfo, ActivationInfo, bReplicateCancelAbility);
			return;
		}
		if (Handle != CurrentSpecHandle || ActorInfo != CurrentActorInfo
			|| ActivationInfo.GetActivationPredictionKey() != CurrentActivationInfo.GetActivationPredictionKey())
		{
			UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("AbilitySystem native Cancel [%s] Spec [%s] rejected: original source mismatch."),
				*GetPathName(), *Handle.ToString());
			return;
		}
		FGGYGOAbilityTerminationResult Result;
		Record = BeginOriginalTermination(Original, EGGYGOAbilityTerminationRequestKind::Cancel,
			bReplicateCancelAbility, true, Result);
		if (!Record.IsValid())
		{
			UE_LOG(LogGGYGOAbilitySystem, Warning, TEXT("AbilitySystem native Cancel [%s] Spec [%s] rejected: reason=%d."),
				*GetPathName(), *Handle.ToString(), static_cast<int32>(Result.Reason));
			return;
		}
		// The entry adapter holds Busy over the full inner virtual dispatch/native Cancel return.
		++Record->OpenDispatches;
		ResumeOriginalTermination(Record);
		if (Record->OpenTryCalls == 0 && !Record->bContinuationQueued && !Record->bContinuationReady
			&& Record->Outcome != EGGYGOAbilityTerminationOutcome::Failed)
		{
			// This synchronous raw entry cannot witness its own full virtual return. Typed callers
			// or a later native continuation have an outer dispatcher; a related Try has an exit
			// witness. Do not publish from this adapter's epilogue without either boundary.
			FailOriginalTermination(Record, EGGYGOAbilityTerminationReason::UnsupportedEntry);
		}
		--Record->OpenDispatches;
		TryCompleteOriginalTermination(Record);
		return;
	}
	if (!Record->bDriving || Record->bCancelEntered || Record->bSealed
		|| Record->Outcome == EGGYGOAbilityTerminationOutcome::Failed
		|| Record->Context.GetRequestKind() != EGGYGOAbilityTerminationRequestKind::Cancel
		|| Record->SpecHandle != Handle || Record->ActorInfo.Pin().Get() != ActorInfo
		|| Record->ActivationInfo.GetActivationPredictionKey() != ActivationInfo.GetActivationPredictionKey())
	{
		return; // Reentry cannot broadcast a second Cancel or replace the first request.
	}
	Record->bCancelEntered = true;
	Super::CancelAbility(Handle, ActorInfo, Record->ActivationInfo, Record->Context.GetReplicateCancelAbility());
	Record->bCancelReturned = true;
}

void UGGYGOGameplayAbility::EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled)
{
	if (!IsEndAbilityValid(Handle, ActorInfo))
	{
		return;
	}

	FScopedControlledActivationEnd OriginalEndScope(this);
	const TSharedPtr<FOriginalTerminationRecord> Record = OriginalTermination;
	if (Record.IsValid())
	{
		const FGameplayAbilitySpec* Spec = Record->ASC.IsValid()
			? Record->ASC->FindAbilitySpecFromHandle(Record->SpecHandle) : nullptr;
		if (!Record->bDriving && !Record->bNativeEndStarted && !Record->bSealed
			&& Record->ASC.IsValid() && (!Spec || Spec->PendingRemove)
			&& ValidateCurrentControlledActivation(true, false).HasSameActivation(Record->Context.GetOriginalActivation())
			&& Record->SpecHandle == Handle && Record->ActorInfo.Pin().Get() == ActorInfo
			&& ActorInfo && ActorInfo->AbilitySystemComponent.Get() == Record->ASC.Get()
			&& Record->ActivationInfo.GetActivationPredictionKey() == ActivationInfo.GetActivationPredictionKey())
		{
			// Native OnRemove must still tear down the exact original GAS resources. Removal
			// invalidates this protocol record; it can never turn that teardown into Completed.
			FailOriginalTermination(Record, EGGYGOAbilityTerminationReason::InvalidAbility);
			if (!Record->bCleanupStarted)
			{
				Record->bCleanupStarted = true;
				CleanupAbilityResourcesForTermination(Record->Context);
			}
			if (!Record->Ability.IsValid()) { return; }
			if (!ValidateCurrentControlledActivation(true, false).HasSameActivation(Record->Context.GetOriginalActivation())) { return; }
			if (ScopeLockCount > 0)
			{
				// GAS teardown of a removed, still locked instance has no supported return proof.
				// Release original camera resources, fail visibly, and add no second native delegate.
				UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("AbilitySystem original removal [%s] Spec [%s] is scope locked; native teardown return is unsupported."),
					*GetPathName(), *Handle.ToString());
				return;
			}
			Record->bNativeEndStarted = true;
			Super::EndAbility(Handle, ActorInfo, Record->ActivationInfo,
				Record->Context.GetReplicateEndAbility(), Record->Context.WasCancelled());
			Record->bNativeEndReturned = true;
			return;
		}
		if (!Record->bDriving || Record->bNativeEndStarted || Record->bSealed
			|| Record->Outcome == EGGYGOAbilityTerminationOutcome::Failed) { return; }
		if (Record->SpecHandle != Handle || Record->ActorInfo.Pin().Get() != ActorInfo
			|| Record->ActivationInfo.GetActivationPredictionKey() != ActivationInfo.GetActivationPredictionKey())
		{
			FailOriginalTermination(Record, EGGYGOAbilityTerminationReason::ActivationChanged);
			return;
		}
		if (ScopeLockCount > 0) { DeferOriginalTermination(Record); return; }
		EGGYGOAbilityTerminationReason SourceReason = CheckOriginalTerminationSource(*Record);
		if (SourceReason != EGGYGOAbilityTerminationReason::None) { FailOriginalTermination(Record, SourceReason); return; }
		Record->bNativeEndStarted = true;
		if (!Record->bCleanupStarted)
		{
			Record->bCleanupStarted = true;
			CleanupAbilityResourcesForTermination(Record->Context);
		}
		// The hook may call external code. Never continue native End against a replacement source.
		if (!Record->Ability.IsValid()) { FailOriginalTermination(Record, EGGYGOAbilityTerminationReason::InvalidAbility); return; }
		SourceReason = CheckOriginalTerminationSource(*Record);
		if (SourceReason != EGGYGOAbilityTerminationReason::None) { FailOriginalTermination(Record, SourceReason); return; }
		Super::EndAbility(Handle, ActorInfo, Record->ActivationInfo,
			Record->Context.GetReplicateEndAbility(), Record->Context.WasCancelled());
		Record->bNativeEndReturned = true;
		return; // Dispatcher witnesses the remaining derived End/Cancel return.
	}
	// Legacy direct derived End has no pre-cleanup/full-return proof. Preserve its native path;
	// it cannot publish protocol Completed until those overrides/callers are migrated.
	ClearCameraOffset();
	ClearCameraMode();

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}
