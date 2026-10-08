/**
 * @file GGYGOActorInfoSource.h
 * @brief Value-only ActorInfo field provenance shared by ASC snapshots and GA proofs.
 */
#pragma once

#include "CoreMinimal.h"

class AActor;
class APlayerController;
class UAbilitySystemComponent;
class UAnimInstance;
class UMovementComponent;
class USkeletalMeshComponent;

/** Read-only field evidence. Allocation ownership and validity policy belong to its enclosing resource. */
struct FGGYGOActorInfoSource
{
	TWeakObjectPtr<UAbilitySystemComponent> AbilitySystemComponent;
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
