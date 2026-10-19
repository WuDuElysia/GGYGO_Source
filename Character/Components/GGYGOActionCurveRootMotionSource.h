#pragma once

#include "GameFramework/RootMotionSource.h"
#include "Animation/Data/GGYGOActionMotionSourceBinding.h"
#include "Character/Data/GGYGOMotionExecutionTypes.h"
#include "GGYGOActionCurveRootMotionSource.generated.h"

class UCurveVector;
struct FGGYGOActionMotionResource;

enum class EGGYGOActionCurveSourceMode : uint8
{
	Profile, OriginalMontage
};

/** 原 Montage XYZ／可选完整 Yaw 合同或显式地面 Profile，共用原 native RMS 时间与所有权。 */
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
	EGGYGOMotionRotationMode RotationMode = EGGYGOMotionRotationMode::TranslationOnly;
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
	/** 原 native 获选源在真实旋转之后认证，不由平移 Prepared 标志代替。 */
	bool bPreparedRotationConsumed = false;
	float PreparedNativeStartSeconds = 0.0f;
	float PreparedNativeEndSeconds = 0.0f;
	float PreparedMontageStartSeconds = 0.0f;
	float PreparedMontageEndSeconds = 0.0f;
	float PreparedYawDeltaDegrees = 0.0f;
	TArray<FGGYGOActionMotionSourceInterval> PreparedSourceIntervals;
	/** Derived from successful original final-range evaluation; native application still has to consume it. */
	bool bPreparedNaturalEnd = false;

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
