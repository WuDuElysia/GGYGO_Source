/**
 * @file GGYGOAnimInstanceBase.cpp
 * @brief 项目通用动画实例基类实现
 */
#include "Animation/Runtime/GGYGOAnimInstanceBase.h"

#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Character.h"

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOAnimationSource, Log, All);

void UGGYGOAnimInstanceBase::NativeInitializeAnimation()
{
	Super::NativeInitializeAnimation();
	ResetAnimationLifecycleState();

	ACharacter* CurrentOwner = Cast<ACharacter>(TryGetPawnOwner());
	CharacterOwner = CurrentOwner;
	bHadCharacterOwner = CurrentOwner != nullptr;
	RefreshLocomotionSourceBinding();
	RefreshAnimationStateFrame();
}

void UGGYGOAnimInstanceBase::NativeUninitializeAnimation()
{
	ResetAnimationLifecycleState();
	Super::NativeUninitializeAnimation();
}

void UGGYGOAnimInstanceBase::NativeUpdateAnimation(float DeltaSeconds)
{
	Super::NativeUpdateAnimation(DeltaSeconds);

	ACharacter* CurrentOwner = Cast<ACharacter>(TryGetPawnOwner());
	const bool bPreviousOwnerExpired = bHadCharacterOwner && !CharacterOwner.IsValid();
	if (CharacterOwner.Get() != CurrentOwner || bPreviousOwnerExpired)
	{
		ResetAnimationLifecycleState();
		CharacterOwner = CurrentOwner;
		bHadCharacterOwner = CurrentOwner != nullptr;
	}

	RefreshLocomotionSourceBinding();
	RefreshAnimationStateFrame();
}

bool UGGYGOAnimInstanceBase::HasAnimationStateTag(FGameplayTag Tag) const
{
	return Tag.IsValid() && AnimationState.OwnedStateTags.HasTag(Tag);
}

bool UGGYGOAnimInstanceBase::IsAnimationRunGait() const
{
	return AnimationState.Gait == EGGYGOGait::Run;
}

bool UGGYGOAnimInstanceBase::IsTurnBackCurveDriven() const
{
	return AnimationState.TurnBackPhase == EGGYGOTurnBackPhase::Turning
		|| AnimationState.TurnBackPhase == EGGYGOTurnBackPhase::Braking;
}

void UGGYGOAnimInstanceBase::OnAnimationLifecycleReset()
{
}

void UGGYGOAnimInstanceBase::ResolveLocomotionSourceBinding(FGGYGOLocomotionSourceBinding& OutBinding) const
{
	OutBinding = {};
	OutBinding.Status = EGGYGOLocomotionSourceStatus::Missing;
	OutBinding.Error = FString::Printf(TEXT("[Animation][LocomotionSource] Producer=%s has no configured source resolver"), *GetPathName());
}

bool UGGYGOAnimInstanceBase::IsLocomotionSourceConfigurationCurrent(const FGGYGOLocomotionSourceBinding& Binding) const
{
	return false;
}

void UGGYGOAnimInstanceBase::ResetAnimationLifecycleState()
{
	RetireLocomotionSourceBinding();
	if (AnimationLifecycleGeneration == MAX_uint64)
	{
		if (!bSourceGenerationExhausted)
		{
			UE_LOG(LogGGYGOAnimationSource, Error,
				TEXT("[Animation][LocomotionSource] Producer=%s lifecycle generation exhausted"), *GetPathName());
		}
		bSourceGenerationExhausted = true;
	}
	else
	{
		++AnimationLifecycleGeneration;
	}
	CharacterOwner.Reset();
	bHadCharacterOwner = false;
	AnimationState = {};
	AnimationDebug = {};
	OnAnimationLifecycleReset();
}

void UGGYGOAnimInstanceBase::RefreshAnimationStateFrame()
{
	StateCapture.Capture(AnimationState, AnimationDebug, CharacterOwner.Get());
}

void UGGYGOAnimInstanceBase::RetireLocomotionSourceBinding()
{
	check(IsInGameThread());
	if (PublishedLocomotionSource)
	{
		if (UGGYGOCharacterMovementComponent* Consumer = LocomotionSourceConsumer.Get())
		{
			Consumer->RetireLocomotionSourceBinding(this, PublishedLocomotionSource->Identity.ConfigurationGeneration);
		}
	}
	PublishedLocomotionSource.Reset();
	LocomotionSourceConsumer.Reset();
}

void UGGYGOAnimInstanceBase::RefreshLocomotionSourceBinding()
{
	check(IsInGameThread());
	ACharacter* Character = CharacterOwner.Get();
	USkeletalMeshComponent* Mesh = GetSkelMeshComponent();
	UGGYGOCharacterMovementComponent* Consumer = Character
		? Cast<UGGYGOCharacterMovementComponent>(Character->GetCharacterMovement()) : nullptr;
	// Preview instances, post-process instances and detached/replaced producers cannot publish.
	if (!Character || !Consumer || !Mesh || Character->GetMesh() != Mesh || Mesh->GetAnimInstance() != this)
	{
		RetireLocomotionSourceBinding();
		return;
	}
	if (bSourceGenerationExhausted)
	{
		return;
	}

	const bool bSameRecipient = PublishedLocomotionSource && LocomotionSourceConsumer.Get() == Consumer
		&& PublishedLocomotionSource->Identity.Character.Get() == Character
		&& PublishedLocomotionSource->Identity.Mesh.Get() == Mesh
		&& PublishedLocomotionSource->Identity.LifecycleGeneration == AnimationLifecycleGeneration;
	if (bSameRecipient && IsLocomotionSourceConfigurationCurrent(*PublishedLocomotionSource))
	{
		return;
	}

	FGGYGOLocomotionSourceBinding Candidate;
	ResolveLocomotionSourceBinding(Candidate);
	if (bSameRecipient && PublishedLocomotionSource->HasSameConfiguration(Candidate))
	{
		return;
	}

	RetireLocomotionSourceBinding();
	if (bSourceGenerationExhausted || LocomotionSourceConfigurationGeneration == MAX_uint64)
	{
		// Exhaustion is permanent for this producer; no wraparound or borrowed successor identity.
		if (!bSourceGenerationExhausted)
		{
			UE_LOG(LogGGYGOAnimationSource, Error,
				TEXT("[Animation][LocomotionSource] Producer=%s Character=%s Mesh=%s configuration generation exhausted"),
				*GetPathName(), *GetPathNameSafe(Character), *GetPathNameSafe(Mesh));
		}
		bSourceGenerationExhausted = true;
		return;
	}
	Candidate.Identity.Producer = this;
	Candidate.Identity.Character = Character;
	Candidate.Identity.Mesh = Mesh;
	Candidate.Identity.LifecycleGeneration = AnimationLifecycleGeneration;
	Candidate.Identity.ConfigurationGeneration = ++LocomotionSourceConfigurationGeneration;
	PublishedLocomotionSource = MakeShared<FGGYGOLocomotionSourceBinding, ESPMode::ThreadSafe>(MoveTemp(Candidate));
	LocomotionSourceConsumer = Consumer;

	FString PublishError;
	const bool bAccepted = Consumer->PublishLocomotionSourceBinding(*PublishedLocomotionSource, PublishError);
	// Receipt acknowledges identity/configuration transport, including stored Missing/Invalid states.
	// Movement rejects those states when curve execution is required; Fixed/GA modes need no source.
	if (!bAccepted)
	{
		UE_LOG(LogGGYGOAnimationSource, Error,
			TEXT("[Animation][LocomotionSource] Producer=%s Character=%s Mesh=%s Generation=%llu Status=%d SourceError=%s PublishError=%s"),
			*GetPathName(), *GetPathNameSafe(Character), *GetPathNameSafe(Mesh),
			PublishedLocomotionSource->Identity.ConfigurationGeneration,
			static_cast<uint8>(PublishedLocomotionSource->Status), *PublishedLocomotionSource->Error, *PublishError);
	}
	// A rejected publication is diagnosed once. Re-publication requires an actual configuration/lifecycle change.
}
