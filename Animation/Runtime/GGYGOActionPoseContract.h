// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtrTemplates.h"

class UAnimMontage;
class USkeletalMesh;
class USkeletalMeshComponent;
class UGGYGOMontageGuardAnimInstance;

/** RequiredReady proves configuration capability, not a future pose evaluation. */
enum class EGGYGOActionPoseContractAcquireResult : uint8
{
	ExplicitNotRequired,
	RequiredReady,
	Rejected,
	Unavailable
};

enum class EGGYGOActionPoseContractPollResult : uint8
{
	Valid,
	Failed,
	Invalidated
};

/**
 * Animation-issued, game-thread read lease. No playback permission, instance ID,
 * pose, subscription, position or clock. The consumer separately authenticates
 * its original Guard/Task/GA resources. Never upgrade an old lease to new config.
 */
class GGYGO_API FGGYGOActionPoseContractTicket
{
public:
	void Reset() { *this = FGGYGOActionPoseContractTicket(); }
	bool IsValid() const
	{
		return GuardLifecycleGeneration != 0 && ContractConfigEpoch != 0
			&& !NodePropertyKey.IsNone() && !SlotName.IsNone()
			&& OriginalProvider.IsValid() && OriginalMeshComponent.IsValid()
			&& OriginalMeshAsset.IsValid() && OriginalMontage.IsValid();
	}
	FName GetSlotName() const { return SlotName; }

private:
	friend class UGGYGOMontageGuardAnimInstance;
	TWeakObjectPtr<UGGYGOMontageGuardAnimInstance> OriginalProvider;
	TWeakObjectPtr<USkeletalMeshComponent> OriginalMeshComponent;
	TWeakObjectPtr<USkeletalMesh> OriginalMeshAsset;
	TWeakObjectPtr<UAnimMontage> OriginalMontage;
	FName NodePropertyKey;
	FName SlotName;
	uint64 GuardLifecycleGeneration = 0;
	uint64 ContractConfigEpoch = 0;
	uint64 FailureSerialAtAcquire = 0;
};
