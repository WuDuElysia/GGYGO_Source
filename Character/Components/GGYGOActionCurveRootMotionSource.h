#pragma once

#include "GameFramework/RootMotionSource.h"
#include "Animation/Data/GGYGOActionMotionSourceBinding.h"
#include "GGYGOActionCurveRootMotionSource.generated.h"

class UCurveVector;
struct FGGYGOActionMotionResource;

enum class EGGYGOActionCurveSourceMode : uint8
{
	Profile, OriginalMontage
};

/** Original Montage XYZ trajectory or explicit ground Profile, using native RMS time and ownership. */
USTRUCT()
struct GGYGO_API FRootMotionSource_GGYGOActionCurve : public FRootMotionSource
{
	GENERATED_BODY()

	FRootMotionSource_GGYGOActionCurve();
	UPROPERTY() TObjectPtr<UCurveVector> TranslationCurve;
	UPROPERTY() FQuat EntryMeshRotation = FQuat::Identity;
	UPROPERTY() FVector TranslationScale = FVector::OneVector;
	UPROPERTY() float PlayRate = 1.f;
	/** Explicit source modes; missing montage data never selects the profile branch. */
	EGGYGOActionCurveSourceMode SourceMode = EGGYGOActionCurveSourceMode::Profile;
	FGGYGOActionMotionSourceBindingPtr OriginalBinding;
	TSharedPtr<const FGGYGOActionMotionResource> OriginalResource;
	/** Original Montage UE-local XYZ is transformed once by the captured Actor basis. */
	FQuat EntryActorRotation = FQuat::Identity;
	float MontageStartSeconds = 0.0f;
	bool bCompletionRequested = false;
	bool bExplicitlyCancelled = false;
	/** Only the native server-to-client RMS import sets this; no local execution grant. */
	bool bNativeImported = false;
	/** Derived native application evidence, copied with the original SavedMove source. */
	bool bPreparedContributionConsumed = false;

	virtual FRootMotionSource* Clone() const override;
	virtual bool Matches(const FRootMotionSource* Other) const override;
	virtual bool MatchesAndHasSameState(const FRootMotionSource* Other) const override;
	virtual bool UpdateStateFrom(const FRootMotionSource* Other, bool bMarkForSimulatedCatchup = false) override;
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
