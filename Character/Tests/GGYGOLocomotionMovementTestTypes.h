#pragma once

#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Animation/AnimInstance.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/AnimCurveTypes.h"
#include "Curves/RichCurve.h"
#include "GameFramework/Character.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Animation/Skeleton.h"
#include "UObject/UnrealType.h"
#endif

#include "GGYGOLocomotionMovementTestTypes.generated.h"

/** Mock values implement public scalar/collection contracts; real Model evidence uses native UAnimSequence. */
UCLASS(Transient)
class UGGYGOLocomotionTestSequence : public UAnimSequence
{
	GENERATED_BODY()
public:
	float TestLength = 1.0f;
	TMap<FName, FRichCurve> TestCurves;
	/** Once-only actual sampling boundary; metadata validation never invokes it. */
	mutable TFunction<void()> OnCurveRead;
	void InvokeCurveReadOnce() const
	{
		auto Callback = MoveTemp(OnCurveRead);
		if (Callback) Callback();
	}
	virtual float GetPlayLength() const override { return TestLength; }
	virtual bool HasCurveData(FName Name, bool bForceUseRawData) const override { return TestCurves.Contains(Name); }
	virtual float EvaluateCurveData(FName Name, const FAnimExtractContext& Context, bool bForceUseRawData) const override
	{
		InvokeCurveReadOnce();
		const FRichCurve* Curve = TestCurves.Find(Name);
		return Curve ? Curve->Eval(static_cast<float>(Context.CurrentTime)) : 0.0f;
	}
	virtual void EvaluateCurveData(FBlendedCurve& OutCurve, const FAnimExtractContext& Context, bool bForceUseRawData) const override
	{
		InvokeCurveReadOnce();
		OutCurve.Empty();
		for (const TPair<FName, FRichCurve>& Curve : TestCurves)
			OutCurve.Set(Curve.Key, Curve.Value.Eval(static_cast<float>(Context.CurrentTime)));
	}
};

UCLASS(Transient)
class UGGYGOLocomotionTestAnimInstance : public UAnimInstance
{
	GENERATED_BODY()
public:
	uint64 PublicationGeneration = 0;
};

#if WITH_DEV_AUTOMATION_TESTS
/** Keep the native Engine vtable: MinimalAPI BlendSpace virtuals are not exported for project subclassing. */
inline UBlendSpace1D* MakeGGYGOLocomotionTestBlendSpace(
	UObject* Outer, UAnimSequence* Walk, UAnimSequence* Run, FString& OutError)
{
	OutError.Reset();
#if WITH_EDITOR
	if (!IsValid(Outer) || !IsValid(Walk) || !IsValid(Run))
	{
		OutError = TEXT("Native locomotion BlendSpace fixture requires a live Outer and both source sequences.");
		return nullptr;
	}
	UBlendSpace1D* BS = NewObject<UBlendSpace1D>(Outer, NAME_None, RF_Transient);
	FStructProperty* AxisProperty = FindFProperty<FStructProperty>(UBlendSpace::StaticClass(), TEXT("BlendParameters"));
	if (!AxisProperty || AxisProperty->ArrayDim < 1 || !AxisProperty->Struct
		|| AxisProperty->Struct->GetFName() != FName(TEXT("BlendParameter"))
		|| AxisProperty->Struct->GetStructureSize() != sizeof(FBlendParameter))
	{
		OutError = TEXT("Native BlendSpace fixture cannot resolve the reflected BlendParameters configuration.");
		return nullptr;
	}
	// Author transient asset configuration through reflection; no protected runtime state or mixed output is injected.
	FBlendParameter* Axis = AxisProperty->ContainerPtrToValuePtr<FBlendParameter>(BS, 0);
	Axis->Min = 0.0f; Axis->Max = 1.0f; Axis->GridNum = 1;
	USkeleton* Skeleton = NewObject<USkeleton>(BS, NAME_None, RF_Transient);
	BS->SetSkeleton(Skeleton); Walk->SetSkeleton(Skeleton); Run->SetSkeleton(Skeleton);
	const int32 WalkIndex = BS->AddSample(Walk, FVector::ZeroVector);
	const int32 RunIndex = BS->AddSample(Run, FVector(1.0, 0.0, 0.0));
	if (WalkIndex != 0 || RunIndex != 1)
	{
		OutError = FString::Printf(TEXT("Native BlendSpace fixture rejected authored samples: Asset='%s', WalkIndex=%d, RunIndex=%d."),
			*BS->GetPathName(), WalkIndex, RunIndex);
		return nullptr;
	}
	BS->ResampleData();
	if (BS->GetBlendSamples().Num() != 2 || BS->GetBlendSpaceData().Segments.IsEmpty())
	{
		OutError = FString::Printf(TEXT("Native BlendSpace fixture failed to generate two-sample 1D data: Asset='%s'."), *BS->GetPathName());
		return nullptr;
	}
	return BS;
#else
	OutError = TEXT("Native locomotion BlendSpace fixture requires the Editor authoring APIs.");
	return nullptr;
#endif
}
#endif

UCLASS(Transient)
class UGGYGOLocomotionTestMovementComponent : public UGGYGOCharacterMovementComponent
{
	GENERATED_BODY()

public:
	void SetTestAcceleration(const FVector& InAcceleration) { Acceleration = InAcceleration; }
	void SetTestMotion(EGGYGOLocomotionMotionType Type, float Time, EGGYGOStopMotionType Stop = EGGYGOStopMotionType::None)
	{
		LocomotionMotionType = Type;
		LocomotionMotionTime = Time;
		StopMotionType = Stop;
	}
	void StageTestMotion(float DeltaSeconds, bool bHadMoveInput, EGGYGOGait PreviousGait)
	{
		CurrentRootMotion.CleanUpInvalidRootMotion(DeltaSeconds, *CharacterOwner, *this);
		UpdateLocomotionMotion(DeltaSeconds, bHadMoveInput, PreviousGait);
	}
	void AdvanceTestMotion(float DeltaSeconds, bool bHadMoveInput, EGGYGOGait PreviousGait)
	{
		StageTestMotion(DeltaSeconds, bHadMoveInput, PreviousGait);
		CurrentRootMotion.PrepareRootMotion(DeltaSeconds, *CharacterOwner, *this, true);
	}
	void AdvanceTestBlend(float DeltaSeconds) { AdvanceTestMotion(DeltaSeconds, true, ResolvedGait); }
	void SetTestGait(EGGYGOGait Gait) { ResolvedGait = Gait; }
	void SetTestTurnBackPhase(EGGYGOTurnBackPhase Phase) { TurnBackPhase = Phase; }
	/** Inject a previously evaluated sample to check per-update cleanup after rebinding. */
	void SetTestCurveMotion(const FGGYGOLocomotionCurveSample& InSample) { CurveMotion = InSample; }
	void SetAuthorityReplayForTest(bool bEnabled) { bReplayLocomotionFromAuthority = bEnabled; }
	void SetTestSequence(uint16 Sequence) { LocomotionMotionSequence = Sequence; }
	bool GetPublishedTurnBackForTest() const { return bReplicatedTurnBackCurveDriven; }
	float GetTestMotionTime() const { return LocomotionMotionTime; }
	uint16 GetTestSequence() const { return LocomotionMotionSequence; }
	const FGGYGOLocomotionCurveSample& GetTestCurveMotion() const { return CurveMotion; }
};

UCLASS(Transient)
class AGGYGOLocomotionTestCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	AGGYGOLocomotionTestCharacter(const FObjectInitializer& ObjectInitializer);
};
