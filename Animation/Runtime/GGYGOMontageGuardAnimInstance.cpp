// Copyright Epic Games, Inc. All Rights Reserved.

#include "Animation/Runtime/GGYGOMontageGuardAnimInstance.h"

#include "Animation/AnimMontage.h"
#include "GameFramework/Actor.h"
#include "Templates/UnrealTemplate.h"

using EGuardOutcome = EGGYGOMontagePlayGuardOutcome;
using ENativeStage = EGGYGOMontagePlayGuardNativeStage;

struct FGGYGOMontagePlayGuardScope::FState
{
	explicit FState(FGGYGOMontagePlayGuardRequest&& InRequest)
		: Request(MoveTemp(InRequest))
	{
		Result.Identity.OriginalAnimInstance = Request.OriginalAnimInstance;
	}

	void Reject(EGuardOutcome Outcome)
	{
		if (!bSealed)
		{
			Result.Outcome = Outcome;
			Result.GuardedNativeReturnValue = 0.f;
		}
	}

	FGGYGOMontagePlayGuardRequest Request;
	FGGYGOMontagePlayGuardResult Result;
	TWeakObjectPtr<UGGYGOMontageGuardAnimInstance> Guard;
	/** Exact IDs of all instances immediately before this call's native Super. */
	TSet<int32> InstanceIdsBeforeNative;
	bool bRegistered = false;
	bool bEntryAuthorized = false;
	bool bSealed = false;
};

FGGYGOMontagePlayGuardScope::FGGYGOMontagePlayGuardScope(FGGYGOMontagePlayGuardRequest&& InRequest)
	: State(MakeUnique<FState>(MoveTemp(InRequest)))
{
	check(IsInGameThread());
	UAnimInstance* Original = State->Request.OriginalAnimInstance.Get();
	if (!Original)
	{
		State->Reject(EGuardOutcome::LifecycleInvalid);
		return;
	}
	State->Guard = Cast<UGGYGOMontageGuardAnimInstance>(Original);
	if (UGGYGOMontageGuardAnimInstance* Guard = State->Guard.Get())
	{
		Guard->RegisterScope(*this);
	}
	else
	{
		State->Reject(EGuardOutcome::Unsupported);
	}
}

FGGYGOMontagePlayGuardScope::~FGGYGOMontagePlayGuardScope()
{
	check(IsInGameThread());
	if (State->bRegistered)
	{
		if (UGGYGOMontageGuardAnimInstance* Guard = State->Guard.Get())
		{
			Guard->UnregisterScope(*this);
		}
		// A missing original object has no live registration to manipulate. Never find a new owner.
		State->bRegistered = false;
	}
}

bool FGGYGOMontagePlayGuardScope::CanExecute()
{
	check(IsInGameThread());
	if (State->bSealed || State->Result.Outcome != EGuardOutcome::NotExecuted)
	{
		return false;
	}
	if (UGGYGOMontageGuardAnimInstance* Guard = State->Guard.Get())
	{
		return Guard->CanExecuteScope(*this);
	}
	State->Reject(EGuardOutcome::LifecycleInvalid);
	return false;
}

const FGGYGOMontagePlayGuardResult& FGGYGOMontagePlayGuardScope::Complete(float InCallerReturnValue)
{
	check(IsInGameThread());
	if (!State->bSealed)
	{
		State->Result.CallerReturnValue = InCallerReturnValue;
		if (State->bRegistered)
		{
			if (UGGYGOMontageGuardAnimInstance* Guard = State->Guard.Get())
			{
				Guard->CompleteScope(*this);
			}
			else
			{
				State->Reject(EGuardOutcome::LifecycleInvalid);
			}
		}
		State->bSealed = true;
	}
	return State->Result;
}

const FGGYGOMontagePlayGuardResult& FGGYGOMontagePlayGuardScope::GetResult() const
{
	check(IsInGameThread());
	return State->Result;
}

void UGGYGOMontageGuardAnimInstance::NativeInitializeAnimation()
{
	check(IsInGameThread());
	InvalidateGuardLifecycle();
	Super::NativeInitializeAnimation();
	LifecycleOwner = GetOwningActor();
	bHadLifecycleOwner = LifecycleOwner.IsValid();
	bLifecycleReady = !bIdentityExhausted;
}

void UGGYGOMontageGuardAnimInstance::NativeUninitializeAnimation()
{
	check(IsInGameThread());
	InvalidateGuardLifecycle();
	LifecycleOwner.Reset();
	bHadLifecycleOwner = false;
	Super::NativeUninitializeAnimation();
}

void UGGYGOMontageGuardAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
	check(IsInGameThread());
	RefreshLifecycleOwner();
	Super::NativeUpdateAnimation(DeltaSeconds);
}

bool UGGYGOMontageGuardAnimInstance::IsMontagePlayGuardIdentityCurrent(
	const FGGYGOMontagePlayGuardIdentity& Identity) const
{
	check(IsInGameThread());
	if (!bLifecycleReady || bIdentityExhausted || Identity.OriginalAnimInstance.Get() != this
		|| Identity.LifecycleGeneration == 0 || Identity.LifecycleGeneration != LifecycleGeneration
		|| Identity.CallId == 0 || Identity.CallId > LastCallId)
	{
		return false;
	}
	AActor* CurrentOwner = GetOwningActor();
	return LifecycleOwner.Get() == CurrentOwner && (!bHadLifecycleOwner || LifecycleOwner.IsValid());
}

bool UGGYGOMontageGuardAnimInstance::TryGetCurrentMontagePlayGuardStage(const UObject* CallerIdentity,
	EGGYGOMontagePlayGuardNativeStage& OutNativeStage, bool& bOutCompleted) const
{
	OutNativeStage = ENativeStage::NotEntered;
	bOutCompleted = false;
	check(IsInGameThread());
	if (!CallerIdentity || ScopeChain.IsEmpty())
	{
		return false;
	}
	const FGGYGOMontagePlayGuardScope* Scope = ScopeChain.Last();
	if (!Scope || !Scope->State)
	{
		return false;
	}
	const FGGYGOMontagePlayGuardScope::FState& Call = *Scope->State;
	if (!Call.bRegistered || Call.Guard.Get() != this || Call.Request.CallerIdentity.Get() != CallerIdentity
		|| !IsMontagePlayGuardIdentityCurrent(Call.Result.Identity))
	{
		return false;
	}
	OutNativeStage = Call.Result.NativeStage;
	bOutCompleted = Call.bSealed;
	return true;
}

void UGGYGOMontageGuardAnimInstance::InvalidateGuardLifecycle()
{
	bLifecycleReady = false;
	if (LifecycleGeneration == MAX_uint64)
	{
		bIdentityExhausted = true;
	}
	else
	{
		++LifecycleGeneration;
	}
	for (FGGYGOMontagePlayGuardScope* Scope : ScopeChain)
	{
		Scope->State->Reject(EGuardOutcome::LifecycleInvalid);
	}
	// Do not clear ScopeChain or reset LastCallId. Old stack scopes still have to unwind.
}

void UGGYGOMontageGuardAnimInstance::RefreshLifecycleOwner()
{
	if (!bLifecycleReady)
	{
		return;
	}
	AActor* CurrentOwner = GetOwningActor();
	if (LifecycleOwner.Get() != CurrentOwner || (bHadLifecycleOwner && !LifecycleOwner.IsValid()))
	{
		InvalidateGuardLifecycle();
		LifecycleOwner = CurrentOwner;
		bHadLifecycleOwner = CurrentOwner != nullptr;
		bLifecycleReady = !bIdentityExhausted;
	}
}

void UGGYGOMontageGuardAnimInstance::RegisterScope(FGGYGOMontagePlayGuardScope& Scope)
{
	RefreshLifecycleOwner();
	FGGYGOMontagePlayGuardScope::FState& Call = *Scope.State;
	if (bIdentityExhausted || LastCallId == MAX_uint64)
	{
		bIdentityExhausted = true;
		InvalidateGuardLifecycle();
		Call.Reject(EGuardOutcome::LifecycleInvalid);
		return;
	}
	Call.Result.Identity.CallId = ++LastCallId;
	Call.Result.Identity.LifecycleGeneration = LifecycleGeneration;
	Call.bRegistered = true;
	ScopeChain.Add(&Scope);
	// Even rejected scopes stay as the innermost scope until their own destructor.
	if (ValidateScopeContext(Scope))
	{
		ValidateEnclosingScope(Scope);
	}
}

void UGGYGOMontageGuardAnimInstance::UnregisterScope(FGGYGOMontagePlayGuardScope& Scope)
{
	FGGYGOMontagePlayGuardScope::FState& Call = *Scope.State;
	if (Call.bRegistered && Call.Guard.Get() == this && Call.Result.Identity.CallId != 0)
	{
		ScopeChain.RemoveSingle(&Scope);
		Call.bRegistered = false;
	}
	// No generation reset, engine stop, callback or current-owner lookup during cleanup.
}

bool UGGYGOMontageGuardAnimInstance::ValidateScopeContext(FGGYGOMontagePlayGuardScope& Scope)
{
	RefreshLifecycleOwner();
	FGGYGOMontagePlayGuardScope::FState& Call = *Scope.State;
	if (!bLifecycleReady || bIdentityExhausted || !Call.bRegistered
		|| Call.Guard.Get() != this || Call.Request.OriginalAnimInstance.Get() != this
		|| Call.Result.Identity.LifecycleGeneration != LifecycleGeneration)
	{
		Call.Reject(EGuardOutcome::LifecycleInvalid);
		return false;
	}
	if (!Call.Request.IsCallerContextCurrent || !Call.Request.RequestedMontage.IsValid())
	{
		Call.Reject(EGuardOutcome::Unsupported);
		return false;
	}
	if (!Call.Request.CallerIdentity.IsValid() || !Call.Request.IsCallerContextCurrent())
	{
		Call.Reject(EGuardOutcome::LifecycleInvalid);
		return false;
	}
	return true;
}

bool UGGYGOMontageGuardAnimInstance::ValidateEnclosingScope(FGGYGOMontagePlayGuardScope& Scope)
{
	FGGYGOMontagePlayGuardScope::FState& Call = *Scope.State;
	if (bUnscopedNativeExecuting || ScopeChain.IsEmpty() || ScopeChain.Last() != &Scope)
	{
		Call.Reject(EGuardOutcome::Unsupported);
		return false;
	}
	if (ScopeChain.Num() == 1)
	{
		return true;
	}
	FGGYGOMontagePlayGuardScope& Parent = *ScopeChain[ScopeChain.Num() - 2];
	const FGGYGOMontagePlayGuardScope::FState& ParentCall = *Parent.State;
	if (ParentCall.Result.Identity.LifecycleGeneration != LifecycleGeneration)
	{
		Call.Reject(EGuardOutcome::LifecycleInvalid);
		return false;
	}
	if (ParentCall.bSealed)
	{
		Call.Reject(EGuardOutcome::Unsupported);
		return false;
	}
	if (ParentCall.Result.NativeStage == ENativeStage::Returned)
	{
		Call.Reject(EGuardOutcome::PostWriteRejected);
		return false;
	}
	if (ParentCall.Result.NativeStage != ENativeStage::Executing || !ObserveCreatedInstance(Parent))
	{
		Call.Reject(EGuardOutcome::PrecreationRejected);
		return false;
	}
	if (ParentCall.Request.CallerIdentity != Call.Request.CallerIdentity
		|| ParentCall.Request.RequestedMontage->GetGroupName() != Call.Request.RequestedMontage->GetGroupName())
	{
		Call.Reject(EGuardOutcome::Unsupported);
		return false;
	}
	// Deliberately do not query the parent's caller callback. End A -> valid B is supported.
	return true;
}

bool UGGYGOMontageGuardAnimInstance::CanExecuteScope(FGGYGOMontagePlayGuardScope& Scope)
{
	FGGYGOMontagePlayGuardScope::FState& Call = *Scope.State;
	if (Call.bSealed || Call.Result.Outcome != EGuardOutcome::NotExecuted)
	{
		return false;
	}
	if (Call.Result.NativeStage != ENativeStage::NotEntered)
	{
		Call.Reject(EGuardOutcome::Unsupported);
		return false;
	}
	Call.bEntryAuthorized = ValidateScopeContext(Scope) && ValidateEnclosingScope(Scope);
	return Call.bEntryAuthorized;
}

bool UGGYGOMontageGuardAnimInstance::ObserveCreatedInstance(FGGYGOMontagePlayGuardScope& Scope)
{
	FGGYGOMontagePlayGuardScope::FState& Call = *Scope.State;
	UAnimMontage* Requested = Call.Request.RequestedMontage.Get();
	if (!Requested)
	{
		return false;
	}
	if (Call.Result.Identity.CreatedInstanceId != INDEX_NONE)
	{
		const FAnimMontageInstance* Instance = GetMontageInstanceForID(Call.Result.Identity.CreatedInstanceId);
		return Instance && Instance->Montage == Requested;
	}
	int32 CandidateId = INDEX_NONE;
	int32 CandidateCount = 0;
	for (const FAnimMontageInstance* Instance : MontageInstances)
	{
		if (Instance && Instance->Montage == Requested
			&& !Call.InstanceIdsBeforeNative.Contains(Instance->GetInstanceID()))
		{
			CandidateId = Instance->GetInstanceID();
			++CandidateCount;
		}
	}
	if (CandidateCount != 1 || CandidateId == INDEX_NONE)
	{
		return false;
	}
	Call.Result.Identity.CreatedInstanceId = CandidateId;
	return true;
}

bool UGGYGOMontageGuardAnimInstance::IsCreatedInstanceActive(const FGGYGOMontagePlayGuardScope& Scope)
{
	const FGGYGOMontagePlayGuardScope::FState& Call = *Scope.State;
	const FAnimMontageInstance* Instance = GetMontageInstanceForID(Call.Result.Identity.CreatedInstanceId);
	return Instance && Instance->Montage == Call.Request.RequestedMontage.Get()
		&& Instance->IsActive() && Instance->IsPlaying();
}

void UGGYGOMontageGuardAnimInstance::PublishAcceptedCall(const FGGYGOMontagePlayGuardScope& Scope)
{
	const FGGYGOMontagePlayGuardScope::FState& Accepted = *Scope.State;
	for (FGGYGOMontagePlayGuardScope* Ancestor : ScopeChain)
	{
		if (Ancestor == &Scope)
		{
			break;
		}
		FGGYGOMontagePlayGuardScope::FState& Call = *Ancestor->State;
		if (!Call.bSealed && Call.Result.Identity.LifecycleGeneration == LifecycleGeneration
			&& Call.Result.NativeStage == ENativeStage::Executing)
		{
			Call.Result.SupersedingCallId = Accepted.Result.Identity.CallId;
			Call.Reject(EGuardOutcome::Superseded);
		}
	}
	// This reaches every native ancestor directly. A failed intermediate call cannot swallow it.
}

void UGGYGOMontageGuardAnimInstance::CompleteScope(FGGYGOMontagePlayGuardScope& Scope)
{
	FGGYGOMontagePlayGuardScope::FState& Call = *Scope.State;
	RefreshLifecycleOwner();
	if (!bLifecycleReady || Call.Result.Identity.LifecycleGeneration != LifecycleGeneration)
	{
		Call.Reject(EGuardOutcome::LifecycleInvalid);
		return;
	}
	if (ScopeChain.IsEmpty() || ScopeChain.Last() != &Scope || Call.Result.NativeStage == ENativeStage::Executing)
	{
		Call.Reject(EGuardOutcome::Unsupported);
		return;
	}
	if (Call.Result.SupersedingCallId != 0)
	{
		// Takeover happened while native ancestors were live; ending the successor cannot revive A.
		Call.Reject(EGuardOutcome::Superseded);
		return;
	}
	if (Call.Result.Outcome == EGuardOutcome::Accepted)
	{
		if (!ValidateScopeContext(Scope))
		{
			return;
		}
		if (!FMath::IsFinite(Call.Result.CallerReturnValue) || Call.Result.CallerReturnValue <= 0.f
			|| !IsCreatedInstanceActive(Scope))
		{
			Call.Reject(EGuardOutcome::Failed);
			return;
		}
		PublishAcceptedCall(Scope);
	}
	// Positive caller return never promotes NotExecuted/Failed/rejected to Accepted. No GAS rollback.
}

float UGGYGOMontageGuardAnimInstance::Montage_PlayInternal(UAnimMontage* MontageToPlay,
	const FMontageBlendSettings& BlendInSettings, float InPlayRate,
	EMontagePlayReturnType ReturnValueType, float InTimeToStartMontageAt, bool bStopAllMontages)
{
	check(IsInGameThread());
	if (ScopeChain.IsEmpty())
	{
		if (bUnscopedNativeExecuting)
		{
			return 0.f;
		}
		TGuardValue<bool> NativeFrame(bUnscopedNativeExecuting, true);
		// Compatibility only for ordinary unscoped top-level playback; no protected result promised.
		return Super::Montage_PlayInternal(MontageToPlay, BlendInSettings, InPlayRate,
			ReturnValueType, InTimeToStartMontageAt, bStopAllMontages);
	}

	FGGYGOMontagePlayGuardScope& Scope = *ScopeChain.Last();
	FGGYGOMontagePlayGuardScope::FState& Call = *Scope.State;
	if (Call.Result.NativeStage != ENativeStage::NotEntered || Call.bSealed)
	{
		// No new caller scope: a direct play interleaved with a protected frame must not execute.
		return 0.f;
	}
	if (!Call.bEntryAuthorized)
	{
		if (Call.Result.Outcome == EGuardOutcome::NotExecuted)
		{
			Call.Reject(EGuardOutcome::PrecreationRejected);
		}
		return 0.f;
	}
	if (!CanExecuteScope(Scope))
	{
		return 0.f;
	}
	if (!bStopAllMontages || MontageToPlay != Call.Request.RequestedMontage.Get())
	{
		Call.Reject(EGuardOutcome::Unsupported);
		return 0.f;
	}
	Call.InstanceIdsBeforeNative.Reset();
	for (const FAnimMontageInstance* Instance : MontageInstances)
	{
		if (!Instance)
		{
			continue;
		}
		const int32 Id = Instance->GetInstanceID();
		if (Id == INDEX_NONE || Call.InstanceIdsBeforeNative.Contains(Id)
			|| (Instance->IsActive() && Instance->Montage
				&& Instance->Montage->GetGroupName() != MontageToPlay->GetGroupName()))
		{
			Call.Reject(EGuardOutcome::Unsupported);
			return 0.f;
		}
		Call.InstanceIdsBeforeNative.Add(Id);
	}

	Call.Result.NativeStage = ENativeStage::Executing;
	const TWeakObjectPtr<UGGYGOMontageGuardAnimInstance> OriginalGuard(this);
	const float NativeReturn = Super::Montage_PlayInternal(MontageToPlay, BlendInSettings, InPlayRate,
		ReturnValueType, InTimeToStartMontageAt, bStopAllMontages);
	if (Call.bSealed)
	{
		// A premature Complete is unsupported. Do not mutate an already sealed snapshot.
		return 0.f;
	}
	Call.Result.NativeStage = ENativeStage::Returned;
	UGGYGOMontageGuardAnimInstance* LiveGuard = OriginalGuard.Get();
	if (!LiveGuard)
	{
		Call.Reject(EGuardOutcome::LifecycleInvalid);
		return 0.f;
	}
	LiveGuard->RefreshLifecycleOwner();
	if (!LiveGuard->bLifecycleReady || Call.Result.Identity.LifecycleGeneration != LiveGuard->LifecycleGeneration)
	{
		Call.Reject(EGuardOutcome::LifecycleInvalid);
		return 0.f;
	}
	const bool bObservedOwnInstance = LiveGuard->ObserveCreatedInstance(Scope);
	if (Call.Result.SupersedingCallId != 0)
	{
		// A may already have ended; a successful B still takes precedence over A's caller validity.
		Call.Reject(EGuardOutcome::Superseded);
		return 0.f;
	}
	if (!LiveGuard->ValidateScopeContext(Scope))
	{
		return 0.f;
	}
	if (!FMath::IsFinite(NativeReturn) || NativeReturn <= 0.f
		|| !bObservedOwnInstance || !LiveGuard->IsCreatedInstanceActive(Scope))
	{
		Call.Reject(EGuardOutcome::Failed);
		return 0.f;
	}
	Call.Result.Outcome = EGuardOutcome::Accepted;
	Call.Result.GuardedNativeReturnValue = NativeReturn;
	return NativeReturn;
}
