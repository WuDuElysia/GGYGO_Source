#include "Character/Components/GGYGOActionCurveRootMotionSource.h"
#include "Curves/CurveVector.h"
#include "GameFramework/CharacterMovementComponent.h"

FRootMotionSource_GGYGOActionCurve::FRootMotionSource_GGYGOActionCurve()
{
	AccumulateMode = ERootMotionAccumulateMode::Override;
	bInLocalSpace = false;
	Settings.SetFlag(ERootMotionSourceSettingsFlags::IgnoreZAccumulate);
}

FRootMotionSource* FRootMotionSource_GGYGOActionCurve::Clone() const
{
	return new FRootMotionSource_GGYGOActionCurve(*this);
}

bool FRootMotionSource_GGYGOActionCurve::Matches(const FRootMotionSource* Other) const
{
	if (!FRootMotionSource::Matches(Other)) return false;
	const auto* Typed = static_cast<const FRootMotionSource_GGYGOActionCurve*>(Other);
	return TranslationCurve == Typed->TranslationCurve && EntryMeshRotation.Equals(Typed->EntryMeshRotation)
		&& TranslationScale.Equals(Typed->TranslationScale) && PlayRate == Typed->PlayRate
		&& Duration == Typed->Duration;
}

void FRootMotionSource_GGYGOActionCurve::PrepareRootMotion(float SimulationTime, float MovementTickTime,
	const ACharacter& Character, const UCharacterMovementComponent& MoveComponent)
{
	RootMotionParams.Clear();
	// Keep gravity under CMC control. Leaving the ground stops this ground-only source.
	if (!TranslationCurve || !MoveComponent.IsMovingOnGround() || Duration <= 0.f || PlayRate <= 0.f)
	{
		RootMotionParams.Set(FTransform(FVector::ZeroVector));
		Status.SetFlag(ERootMotionSourceStatusFlags::Finished);
		return;
	}
	if (MovementTickTime > UE_SMALL_NUMBER && SimulationTime > 0.f)
	{
		const float From = FMath::Clamp(GetTime(), 0.f, Duration) * PlayRate;
		const float To = FMath::Clamp(GetTime() + SimulationTime, 0.f, Duration) * PlayRate;
		const FVector Delta = (TranslationCurve->GetVectorValue(To) - TranslationCurve->GetVectorValue(From)) * TranslationScale;
		FVector WorldDelta = EntryMeshRotation.RotateVector(Delta);
		WorldDelta.Z = 0.;
		if (WorldDelta.ContainsNaN())
		{
			Status.SetFlag(ERootMotionSourceStatusFlags::Finished);
			WorldDelta = FVector::ZeroVector;
		}
		// RMS translation is velocity; integrate the complete interval, including a partial last tick.
		RootMotionParams.Set(FTransform(WorldDelta / MovementTickTime));
	}
	// A zero-displacement interval remains an override until the duration expires.
	SetTime(GetTime() + SimulationTime);
}

bool FRootMotionSource_GGYGOActionCurve::NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
{
	if (!FRootMotionSource::NetSerialize(Ar, Map, bOutSuccess)) return false;
	Ar << TranslationCurve;
	Ar << EntryMeshRotation;
	Ar << TranslationScale;
	Ar << PlayRate;
	bOutSuccess = !Ar.IsError();
	return bOutSuccess;
}

UScriptStruct* FRootMotionSource_GGYGOActionCurve::GetScriptStruct() const
{
	return StaticStruct();
}

FString FRootMotionSource_GGYGOActionCurve::ToSimpleString() const
{
	return FString::Printf(TEXT("[ID:%u]ActionCurve %s t=%.3f/%.3f"), LocalID, *InstanceName.ToString(), GetTime(), Duration);
}

void FRootMotionSource_GGYGOActionCurve::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(TranslationCurve);
	FRootMotionSource::AddReferencedObjects(Collector);
}
