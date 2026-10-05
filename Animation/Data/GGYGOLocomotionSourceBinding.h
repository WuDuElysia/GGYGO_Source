#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Character/Data/GGYGOMovementTypes.h"
#include "UObject/StrongObjectPtr.h"

class ACharacter;
class UAnimInstance;
class USkeletalMeshComponent;

/** Source availability is explicit; a failed publication never describes a usable source. */
enum class EGGYGOLocomotionSourceStatus : uint8
{
	Available,
	Missing,
	Invalid,
	Stale
};

/** One producer's identity. Generations are local lifecycle/configuration identities, not clocks. */
struct FGGYGOLocomotionSourceIdentity
{
	TWeakObjectPtr<UAnimInstance> Producer;
	TWeakObjectPtr<ACharacter> Character;
	TWeakObjectPtr<USkeletalMeshComponent> Mesh;
	uint64 LifecycleGeneration = 0;
	uint64 ConfigurationGeneration = 0;

	bool IsSameIdentity(const FGGYGOLocomotionSourceIdentity& Other) const
	{
		return Producer.HasSameIndexAndSerialNumber(Other.Producer)
			&& Character.HasSameIndexAndSerialNumber(Other.Character)
			&& Mesh.HasSameIndexAndSerialNumber(Other.Mesh)
			&& LifecycleGeneration == Other.LifecycleGeneration
			&& ConfigurationGeneration == Other.ConfigurationGeneration;
	}
};

/** Borrowed during resolution, strongly retained only by the published binding/move snapshots. */
struct FGGYGOLocomotionSequenceSource
{
	EGGYGOLocomotionMotionType MotionType = EGGYGOLocomotionMotionType::None;
	FName RouteKey;
	TStrongObjectPtr<UAnimSequence> Sequence;
	bool bLoop = false;
	float SequenceRateScale = 0.0f;
	float PlayLength = 0.0f;

	bool HasSameConfiguration(const FGGYGOLocomotionSequenceSource& Other) const
	{
		return MotionType == Other.MotionType && RouteKey == Other.RouteKey
			&& Sequence.Get() == Other.Sequence.Get() && bLoop == Other.bLoop
			&& SequenceRateScale == Other.SequenceRateScale && PlayLength == Other.PlayLength;
	}
};

/** Sample identities/rates come from the actual BlendSpace, never from a duplicate loop table. */
struct FGGYGOLocomotionBlendSpaceSampleSource
{
	FGGYGOLocomotionSequenceSource Source;
	int32 SampleIndex = INDEX_NONE;
	FVector SampleValue = FVector::ZeroVector;
	float SampleRateScale = 0.0f;

	bool HasSameConfiguration(const FGGYGOLocomotionBlendSpaceSampleSource& Other) const
	{
		return Source.HasSameConfiguration(Other.Source) && SampleIndex == Other.SampleIndex
			&& SampleValue.X == Other.SampleValue.X && SampleValue.Y == Other.SampleValue.Y
			&& SampleValue.Z == Other.SampleValue.Z && SampleRateScale == Other.SampleRateScale;
	}
};

/**
 * Animation resolves this data; Movement consumes const snapshots and owns move lifetimes.
 * Contains references and source metadata only: no copied curves, motion time, graph state,
 * evaluated output, or movement authority. Strong asset references last only as long as the
 * publisher/current binding and existing bounded SavedMove/Origin/Input snapshots retain it.
 * Status != Available carries a concrete reason and must never enable movement.
 */
struct FGGYGOLocomotionSourceBinding
{
	FGGYGOLocomotionSourceIdentity Identity;
	EGGYGOLocomotionSourceStatus Status = EGGYGOLocomotionSourceStatus::Missing;
	FString Error;
	TArray<FGGYGOLocomotionSequenceSource> SingleSources;
	FName WalkRunKey;
	TStrongObjectPtr<UBlendSpace> WalkRunBlendSpace;
	bool bWalkRunLoop = false;
	TArray<FGGYGOLocomotionBlendSpaceSampleSource> WalkRunSamples;

	const FGGYGOLocomotionSequenceSource* GetSingleSource(EGGYGOLocomotionMotionType MotionType) const
	{
		return SingleSources.FindByPredicate([MotionType](const FGGYGOLocomotionSequenceSource& Source)
		{
			return Source.MotionType == MotionType;
		});
	}

	const FGGYGOLocomotionBlendSpaceSampleSource* GetWalkRunSample(int32 SampleIndex) const
	{
		return WalkRunSamples.FindByPredicate([SampleIndex](const FGGYGOLocomotionBlendSpaceSampleSource& Sample)
		{
			return Sample.SampleIndex == SampleIndex;
		});
	}

	/** Exact comparison for publication invalidation; does not compare or advance producer generations. */
	bool HasSameConfiguration(const FGGYGOLocomotionSourceBinding& Other) const
	{
		if (Status != Other.Status || Error != Other.Error || WalkRunKey != Other.WalkRunKey
			|| WalkRunBlendSpace.Get() != Other.WalkRunBlendSpace.Get() || bWalkRunLoop != Other.bWalkRunLoop
			|| SingleSources.Num() != Other.SingleSources.Num() || WalkRunSamples.Num() != Other.WalkRunSamples.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < SingleSources.Num(); ++Index)
		{
			if (!SingleSources[Index].HasSameConfiguration(Other.SingleSources[Index]))
			{
				return false;
			}
		}
		for (int32 Index = 0; Index < WalkRunSamples.Num(); ++Index)
		{
			if (!WalkRunSamples[Index].HasSameConfiguration(Other.WalkRunSamples[Index]))
			{
				return false;
			}
		}
		return true;
	}
};

/** Consumers share one immutable publication; replay must not consult the current AnimInstance. */
using FGGYGOLocomotionSourceBindingPtr = TSharedPtr<const FGGYGOLocomotionSourceBinding, ESPMode::ThreadSafe>;
