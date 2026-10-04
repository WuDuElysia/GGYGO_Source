#pragma once

#include "GameFramework/RootMotionSource.h"
#include "GGYGOActionCurveRootMotionSource.generated.h"

class UCurveVector;

/** Finite, independently sampled ground trajectory; no AnimInstance or ability dependency. */
USTRUCT()
struct GGYGO_API FRootMotionSource_GGYGOActionCurve : public FRootMotionSource
{
	GENERATED_BODY()

	FRootMotionSource_GGYGOActionCurve();
	UPROPERTY() TObjectPtr<UCurveVector> TranslationCurve;
	UPROPERTY() FQuat EntryMeshRotation = FQuat::Identity;
	UPROPERTY() FVector TranslationScale = FVector::OneVector;
	UPROPERTY() float PlayRate = 1.f;

	virtual FRootMotionSource* Clone() const override;
	virtual bool Matches(const FRootMotionSource* Other) const override;
	virtual void PrepareRootMotion(float SimulationTime, float MovementTickTime,
		const ACharacter& Character, const UCharacterMovementComponent& MoveComponent) override;
	virtual bool NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess) override;
	virtual UScriptStruct* GetScriptStruct() const override;
	virtual FString ToSimpleString() const override;
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
};

template<> struct TStructOpsTypeTraits<FRootMotionSource_GGYGOActionCurve>
	: public TStructOpsTypeTraitsBase2<FRootMotionSource_GGYGOActionCurve>
{
	enum { WithNetSerializer = true, WithCopy = true };
};
