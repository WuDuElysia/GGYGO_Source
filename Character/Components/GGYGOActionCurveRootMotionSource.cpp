#include "Character/Components/GGYGOActionCurveRootMotionSource.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Character/Data/GGYGOActionMotionEvaluation.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "GameFramework/Character.h"
#include "EngineLogs.h"
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
	if (SourceMode != Typed->SourceMode) return false;
	if (SourceMode == EGGYGOActionCurveSourceMode::OriginalMontage)
	{
		if (OriginalResource.IsValid() && Typed->OriginalResource.IsValid()
			&& OriginalResource != Typed->OriginalResource) return false;
		return OriginalBinding.IsValid() && Typed->OriginalBinding.IsValid()
			&& OriginalBinding->HasSameConfiguration(*Typed->OriginalBinding)
			&& MontageStartSeconds == Typed->MontageStartSeconds && PlayRate == Typed->PlayRate
			&& Duration == Typed->Duration && EntryActorRotation == Typed->EntryActorRotation
			&& TranslationScale == Typed->TranslationScale;
	}
	return TranslationCurve == Typed->TranslationCurve && EntryMeshRotation.Equals(Typed->EntryMeshRotation)
		&& TranslationScale.Equals(Typed->TranslationScale) && PlayRate == Typed->PlayRate
		&& Duration == Typed->Duration;
}

bool FRootMotionSource_GGYGOActionCurve::MatchesAndHasSameState(const FRootMotionSource* Other) const
{
	if (!FRootMotionSource::MatchesAndHasSameState(Other)) return false;
	const auto* Typed = static_cast<const FRootMotionSource_GGYGOActionCurve*>(Other);
	return bCompletionRequested == Typed->bCompletionRequested && bExplicitlyCancelled == Typed->bExplicitlyCancelled;
}

bool FRootMotionSource_GGYGOActionCurve::UpdateStateFrom(const FRootMotionSource* Other, bool bMarkForSimulatedCatchup)
{
	if (!Other || !Matches(Other)) return false;
	if (!FRootMotionSource::UpdateStateFrom(Other, bMarkForSimulatedCatchup)) return false;
	const auto* Typed = static_cast<const FRootMotionSource_GGYGOActionCurve*>(Other);
	bCompletionRequested = Typed->bCompletionRequested;
	bExplicitlyCancelled |= Typed->bExplicitlyCancelled;
	// Keep the recipient's original local identity. An imported source cannot adopt a current Montage.
	return true;
}

void FRootMotionSource_GGYGOActionCurve::PrepareRootMotion(float SimulationTime, float MovementTickTime,
	const ACharacter& Character, const UCharacterMovementComponent& MoveComponent)
{
	RootMotionParams.Clear();
	if (SourceMode == EGGYGOActionCurveSourceMode::OriginalMontage)
	{
		bPreparedContributionConsumed = false;
		const auto Retire = [this]()
		{
			bExplicitlyCancelled = true;
			RootMotionParams.Set(FTransform::Identity);
			AccumulateMode = ERootMotionAccumulateMode::Additive;
			Status.SetFlag(ERootMotionSourceStatusFlags::MarkedForRemoval);
		};
		if (bExplicitlyCancelled) { Retire(); return; }
		FString Error;
		const UGGYGOCharacterMovementComponent* CMC = Cast<UGGYGOCharacterMovementComponent>(&MoveComponent);
		if (!CMC || !CMC->ValidateMontageActionRuntime(*this, Error)
			|| !FMath::IsFinite(SimulationTime) || SimulationTime < 0.0f
			|| !FMath::IsFinite(MovementTickTime) || MovementTickTime <= 0.0f
			|| !FMath::IsFinite(GetTime()) || !FMath::IsFinite(Duration) || Duration <= 0.0f
			|| !FMath::IsFinite(PlayRate) || PlayRate <= 0.0f
			|| TranslationScale.ContainsNaN() || EntryActorRotation.ContainsNaN() || !EntryActorRotation.IsNormalized())
		{
			if (Error.IsEmpty()) Error = TEXT("native interval, entry transform or playback mapping is invalid");
			UE_LOG(LogRootMotion, Error, TEXT("[Movement.ActionMotion] Owner='%s' Montage='%s' Slot='%s' Section='%s' Reason='%s'"),
				*Character.GetPathName(), OriginalBinding.IsValid() ? *GetPathNameSafe(OriginalBinding->Montage.Get()) : TEXT("None"),
				OriginalBinding.IsValid() ? *OriginalBinding->SlotName.ToString() : TEXT("None"),
				OriginalBinding.IsValid() ? *OriginalBinding->SectionName.ToString() : TEXT("None"), *Error);
			Retire();
			if (CMC) const_cast<UGGYGOCharacterMovementComponent*>(CMC)->FailMontageActionMotion(OriginalResource, Error);
			return;
		}
		const float From = FMath::Min(MontageStartSeconds + FMath::Clamp(GetTime(), 0.0f, Duration) * PlayRate,
			OriginalBinding->MontageEndSeconds);
		const float To = FMath::Min(MontageStartSeconds + FMath::Clamp(GetTime() + SimulationTime, 0.0f, Duration) * PlayRate,
			OriginalBinding->MontageEndSeconds);
		FVector Delta;
		if (!GGYGOActionMotionEvaluation::EvaluateInterval(*OriginalBinding, From, To, Delta, Error))
		{
			UE_LOG(LogRootMotion, Error, TEXT("%s"), *Error);
			Retire();
			const_cast<UGGYGOCharacterMovementComponent*>(CMC)->FailMontageActionMotion(OriginalResource, Error);
			return;
		}
		FVector WorldDelta = EntryActorRotation.RotateVector(Delta * TranslationScale);
		WorldDelta.Z = 0.0;
		const FVector OverrideVelocity = WorldDelta / MovementTickTime;
		if (OverrideVelocity.ContainsNaN())
		{
			UE_LOG(LogRootMotion, Error, TEXT("[Movement.ActionMotion] Owner='%s' Montage='%s' Reason='scaled interval is not finite'"),
				*Character.GetPathName(), *GetPathNameSafe(OriginalBinding->Montage.Get()));
			Retire();
			const_cast<UGGYGOCharacterMovementComponent*>(CMC)->FailMontageActionMotion(OriginalResource,
				TEXT("[Movement.ActionMotion] scaled original position interval is not finite"));
			return;
		}
		RootMotionParams.Set(FTransform(OverrideVelocity));
		SetTime(GetTime() + SimulationTime); // The only action clock, including zero-displacement intervals.
		return;
	}
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
	bOutSuccess = false;
	if (Ar.IsLoading())
	{
		OriginalResource.Reset(); OriginalBinding.Reset(); bNativeImported = false;
		bPreparedContributionConsumed = false;
	}
	else if (static_cast<uint8>(SourceMode) > static_cast<uint8>(EGGYGOActionCurveSourceMode::OriginalMontage)) return false;
	if (!FRootMotionSource::NetSerialize(Ar, Map, bOutSuccess)) return false;
	Ar << TranslationCurve;
	Ar << EntryMeshRotation;
	Ar << TranslationScale;
	Ar << PlayRate;
	uint8 Mode = static_cast<uint8>(SourceMode);
	Ar.SerializeBits(&Mode, 1);
	if (Ar.IsLoading()) SourceMode = static_cast<EGGYGOActionCurveSourceMode>(Mode);
	if (SourceMode == EGGYGOActionCurveSourceMode::OriginalMontage)
	{
		Ar << EntryActorRotation << MontageStartSeconds;
		Ar << bCompletionRequested << bExplicitlyCancelled;
		FGGYGOActionMotionSourceBinding Wire;
		if (Ar.IsSaving())
		{
			if (!OriginalBinding.IsValid()) { bOutSuccess = false; return false; }
			Wire = *OriginalBinding;
		}
		UAnimMontage* Montage = Wire.Montage.Get();
		Ar << Montage;
		if (Ar.IsLoading()) Wire.Montage = TStrongObjectPtr<UAnimMontage>(Montage);
		Ar << Wire.SlotName << Wire.SectionName << Wire.TrackIndex << Wire.SectionIndex;
		Ar << Wire.TrackSegmentCount << Wire.SectionCount << Wire.MontageStartSeconds << Wire.MontageEndSeconds;
		Ar << Wire.MontageLength << Wire.MontageRateScale << Wire.bMontageHasRootMotion;
		uint16 Count = static_cast<uint16>(Wire.Segments.Num());
		if (Ar.IsSaving() && (Wire.Segments.Num() <= 0 || Wire.Segments.Num() > 4096)) { bOutSuccess = false; return false; }
		Ar << Count;
		if (Count == 0 || Count > 4096) { bOutSuccess = false; return false; }
		if (Ar.IsLoading()) Wire.Segments.SetNum(Count);
		for (FGGYGOActionMotionSourceSegment& Segment : Wire.Segments)
		{
			UAnimSequence* Sequence = Segment.Sequence.Get();
			Ar << Sequence;
			if (Ar.IsLoading()) Segment.Sequence = TStrongObjectPtr<UAnimSequence>(Sequence);
			Ar << Segment.SegmentIndex << Segment.StartPos << Segment.MontageEndSeconds;
			Ar << Segment.AnimStartTime << Segment.AnimEndTime << Segment.LoopingCount;
			Ar << Segment.AnimPlayRate << Segment.SequenceRateScale << Segment.SourceSecondsPerMontageSecond;
			Ar << Segment.SequenceLength << Segment.bEnableRootMotion;
		}
		if (Ar.IsLoading())
		{
			OriginalResource.Reset();
			OriginalBinding.Reset();
			bNativeImported = true;
			bPreparedContributionConsumed = false;
			FString Error;
			if (Ar.IsError() || !GGYGOActionMotionEvaluation::ValidateSource(Wire, Error))
			{
				bOutSuccess = false; return false;
			}
			OriginalBinding = MakeShared<const FGGYGOActionMotionSourceBinding, ESPMode::ThreadSafe>(MoveTemp(Wire));
		}
	}
	else if (Ar.IsLoading())
	{
		OriginalResource.Reset(); OriginalBinding.Reset(); bNativeImported = false;
	}
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
