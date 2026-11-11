#include "Character/Components/GGYGOActionCurveRootMotionSource.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Character/Data/GGYGOActionMotionEvaluation.h"
#include "Character/Data/GGYGOActionMotionProfile.h"
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
	// This is the explicit ground Profile default. Original Montage execution configures XYZ.
	Settings.SetFlag(ERootMotionSourceSettingsFlags::IgnoreZAccumulate);
}

void FRootMotionSource_GGYGOActionCurve::NeutralizeForRemoval()
{
	bExplicitlyCancelled = true;
	RootMotionParams.Set(FTransform::Identity);
	// 原生累加遍历仍可看见待移除的源。零 Additive 不占用 Override 槽。
	AccumulateMode = ERootMotionAccumulateMode::Additive;
	Status.SetFlag(ERootMotionSourceStatusFlags::MarkedForRemoval);
}

FRootMotionSource* FRootMotionSource_GGYGOActionCurve::Clone() const
{
	return new FRootMotionSource_GGYGOActionCurve(*this);
}

bool FRootMotionSource_GGYGOActionCurve::Matches(const FRootMotionSource* Other) const
{
	if (!Other || Other->GetScriptStruct() != GetScriptStruct()) return false;
	const auto* Typed = static_cast<const FRootMotionSource_GGYGOActionCurve*>(Other);
	if (SourceMode != Typed->SourceMode) return false;
	if (!FRootMotionSource::Matches(Other))
	{
		const auto IsNeutralized = [](const FRootMotionSource_GGYGOActionCurve& Source)
		{
			return Source.AccumulateMode == ERootMotionAccumulateMode::Additive && Source.bExplicitlyCancelled
				&& Source.Status.HasFlag(ERootMotionSourceStatusFlags::MarkedForRemoval);
		};
		// 原生寻找新映射时把入站 ID 清为 Invalid。只允许已有映射的同一源跨取消状态匹配。
		if (SourceMode != EGGYGOActionCurveSourceMode::OriginalMontage
			|| LocalID == static_cast<uint16>(ERootMotionSourceID::Invalid) || LocalID != Typed->LocalID
			|| !((AccumulateMode == ERootMotionAccumulateMode::Override && IsNeutralized(*Typed))
				|| (Typed->AccumulateMode == ERootMotionAccumulateMode::Override && IsNeutralized(*this)))) return false;
		// 只在临时比较值中对齐累加模式，其余身份字段仍由原生 Matches 核对。
		FRootMotionSource_GGYGOActionCurve MatchView;
		static_cast<FRootMotionSource&>(MatchView) = *Other;
		MatchView.AccumulateMode = AccumulateMode;
		if (!FRootMotionSource::Matches(&MatchView)) return false;
	}
	if (OriginalResource.IsValid() && Typed->OriginalResource.IsValid()
		&& OriginalResource != Typed->OriginalResource) return false;
	if (SourceMode == EGGYGOActionCurveSourceMode::OriginalMontage)
	{
		return OriginalBinding.IsValid() && Typed->OriginalBinding.IsValid()
			&& OriginalBinding->HasSameConfiguration(*Typed->OriginalBinding)
			&& RotationMode == Typed->RotationMode && MontageStartSeconds == Typed->MontageStartSeconds && PlayRate == Typed->PlayRate
			&& Duration == Typed->Duration
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
	return bCompletionRequested == Typed->bCompletionRequested && bExplicitlyCancelled == Typed->bExplicitlyCancelled
		&& (SourceMode != EGGYGOActionCurveSourceMode::OriginalMontage || EntryActorRotation == Typed->EntryActorRotation);
}

bool FRootMotionSource_GGYGOActionCurve::UpdateStateFrom(const FRootMotionSource* Other, bool bMarkForSimulatedCatchup)
{
	if (!Other || !Matches(Other)) return false;
	const auto* Typed = static_cast<const FRootMotionSource_GGYGOActionCurve*>(Other);
	if (SourceMode == EGGYGOActionCurveSourceMode::OriginalMontage
		&& (Typed->EntryActorRotation.ContainsNaN() || !Typed->EntryActorRotation.IsNormalized())) return false;
	if (!FRootMotionSource::UpdateStateFrom(Other, bMarkForSimulatedCatchup)) return false;
	// 基底是同一原 RMS 的可纠正状态，不作另一个动作身份；原生复制/回放继续沿原时钟。
	if (SourceMode == EGGYGOActionCurveSourceMode::OriginalMontage) EntryActorRotation = Typed->EntryActorRotation;
	bCompletionRequested = Typed->bCompletionRequested;
	if (Typed->bExplicitlyCancelled) NeutralizeForRemoval();
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
		bPreparedRotationConsumed = false;
		PreparedSourceIntervals.Reset();
		bPreparedNaturalEnd = false;
		if (bExplicitlyCancelled) { NeutralizeForRemoval(); return; }
		FString Error;
		const UGGYGOCharacterMovementComponent* CMC = Cast<UGGYGOCharacterMovementComponent>(&MoveComponent);
		if (!CMC || !CMC->ValidateMontageActionRuntime(*this, SimulationTime, Error)
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
			NeutralizeForRemoval();
			if (CMC) const_cast<UGGYGOCharacterMovementComponent*>(CMC)->FailMontageActionMotion(OriginalResource, Error);
			return;
		}
		const float From = FMath::Min(MontageStartSeconds + FMath::Clamp(GetTime(), 0.0f, Duration) * PlayRate,
			OriginalBinding->MontageEndSeconds);
		const float To = FMath::Min(MontageStartSeconds + FMath::Clamp(GetTime() + SimulationTime, 0.0f, Duration) * PlayRate,
			OriginalBinding->MontageEndSeconds);
		FGGYGOActionMotionEvaluationResult Evaluation;
		if (!GGYGOActionMotionEvaluation::EvaluateInterval(*OriginalBinding, From, To, RotationMode, Evaluation, Error))
		{
			UE_LOG(LogRootMotion, Error, TEXT("%s"), *Error);
			NeutralizeForRemoval();
			const_cast<UGGYGOCharacterMovementComponent*>(CMC)->FailMontageActionMotion(OriginalResource, Error);
			return;
		}
		const FVector WorldDelta = EntryActorRotation.RotateVector(Evaluation.Translation * TranslationScale);
		const FVector OverrideVelocity = WorldDelta / MovementTickTime;
		if (OverrideVelocity.ContainsNaN())
		{
			UE_LOG(LogRootMotion, Error, TEXT("[Movement.ActionMotion] Owner='%s' Montage='%s' Reason='scaled interval is not finite'"),
				*Character.GetPathName(), *GetPathNameSafe(OriginalBinding->Montage.Get()));
			NeutralizeForRemoval();
			const_cast<UGGYGOCharacterMovementComponent*>(CMC)->FailMontageActionMotion(OriginalResource,
				TEXT("[Movement.ActionMotion] scaled original position interval is not finite"));
			return;
		}
		const float NativeEnd = GetTime() + SimulationTime;
		if (!FMath::IsFinite(NativeEnd))
		{
			NeutralizeForRemoval();
			const_cast<UGGYGOCharacterMovementComponent*>(CMC)->FailMontageActionMotion(OriginalResource,
				TEXT("[Movement.ActionMotion] original native clock overflowed"));
			return;
		}
		PreparedNativeStartSeconds = GetTime();
		PreparedNativeEndSeconds = NativeEnd;
		PreparedMontageStartSeconds = From;
		PreparedMontageEndSeconds = To;
		PreparedYawDeltaDegrees = Evaluation.YawDeltaDegrees;
		PreparedSourceIntervals = MoveTemp(Evaluation.SourceIntervals);
		// 原 XYZ 保持入口 Actor 基底，Yaw 是该 native 区间的增量，不能二次旋转轨迹。
		const FQuat LocalYaw = FRotator(0.0f, PreparedYawDeltaDegrees, 0.0f).Quaternion();
		const FQuat WorldYaw = EntryActorRotation * LocalYaw * EntryActorRotation.Inverse();
		RootMotionParams.Set(FTransform(WorldYaw, OverrideVelocity));
		SetTime(GetTime() + SimulationTime); // The only action clock, including zero-displacement intervals.
		bPreparedNaturalEnd = To == OriginalBinding->MontageEndSeconds && GetTime() >= Duration;
		return;
	}
	if (bExplicitlyCancelled) { NeutralizeForRemoval(); return; }
	const auto* CMC = Cast<UGGYGOCharacterMovementComponent>(&MoveComponent);
	const auto FailProfile = [&](const FString& Reason)
	{
		UE_LOG(LogRootMotion, Error, TEXT("[Movement.ActionMotion] Owner='%s' Profile='%s' Curve='%s' Reason='%s'"),
			*Character.GetPathName(), OriginalResource.IsValid() ? *GetPathNameSafe(OriginalResource->Profile.Get()) : TEXT("Standalone"),
			*GetPathNameSafe(TranslationCurve.Get()), *Reason);
		NeutralizeForRemoval();
		if (CMC) const_cast<UGGYGOCharacterMovementComponent*>(CMC)->FailProfileActionMotion(OriginalResource, Reason);
	};
	if (SourceMode != EGGYGOActionCurveSourceMode::Profile)
	{
		FailProfile(TEXT("explicit action source mode is invalid"));
		return;
	}
	// Leaving the ground normally ends this explicit ground-only source.
	if (!MoveComponent.IsMovingOnGround())
	{
		RootMotionParams.Set(FTransform(FVector::ZeroVector));
		Status.SetFlag(ERootMotionSourceStatusFlags::Finished);
		return;
	}
	FString Error;
	if (!TranslationCurve || !FMath::IsFinite(Duration) || Duration <= 0.0f
		|| !FMath::IsFinite(PlayRate) || PlayRate <= 0.0f
		|| !FMath::IsFinite(SimulationTime) || SimulationTime < 0.0f
		|| !FMath::IsFinite(MovementTickTime) || MovementTickTime <= 0.0f
		|| !FMath::IsFinite(GetTime()) || GetTime() < 0.0f
		|| TranslationScale.ContainsNaN() || EntryMeshRotation.ContainsNaN() || !EntryMeshRotation.IsNormalized()
		|| (OriginalResource.IsValid() && (!CMC || !CMC->ValidateProfileActionRuntime(*this, Error))))
	{
		FailProfile(Error.IsEmpty() ? TEXT("original Profile curve, native interval, playback mapping or entry transform is invalid") : Error);
		return;
	}
	const double NativeEndTime = static_cast<double>(GetTime()) + SimulationTime;
	if (!FMath::IsFinite(NativeEndTime) || NativeEndTime > TNumericLimits<float>::Max())
	{
		FailProfile(TEXT("original Profile native interval end time is outside finite float range"));
		return;
	}
	const float From = FMath::Clamp(GetTime(), 0.0f, Duration) * PlayRate;
	const float To = FMath::Clamp(static_cast<float>(NativeEndTime), 0.0f, Duration) * PlayRate;
	const FVector Delta = (TranslationCurve->GetVectorValue(To) - TranslationCurve->GetVectorValue(From)) * TranslationScale;
	FVector WorldDelta = EntryMeshRotation.RotateVector(Delta);
	if (!FMath::IsFinite(From) || !FMath::IsFinite(To) || WorldDelta.ContainsNaN())
	{
		FailProfile(TEXT("scaled original Profile position interval is non-finite"));
		return;
	}
	WorldDelta.Z = 0.0;
	const FVector OverrideVelocity = WorldDelta / MovementTickTime;
	if (OverrideVelocity.ContainsNaN())
	{
		FailProfile(TEXT("original Profile interval velocity is non-finite"));
		return;
	}
	// RMS translation is velocity; include the partial last tick and legitimate zero displacement.
	RootMotionParams.Set(FTransform(OverrideVelocity));
	SetTime(static_cast<float>(NativeEndTime));
}

bool FRootMotionSource_GGYGOActionCurve::NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
{
	bOutSuccess = false;
	if (Ar.IsLoading())
	{
		OriginalResource.Reset(); OriginalBinding.Reset(); bNativeImported = false;
		RotationMode = EGGYGOMotionRotationMode::TranslationOnly;
		bPreparedContributionConsumed = false;
		bPreparedRotationConsumed = false;
		PreparedSourceIntervals.Reset();
		PreparedNativeStartSeconds = PreparedNativeEndSeconds = 0.0f;
		PreparedMontageStartSeconds = PreparedMontageEndSeconds = PreparedYawDeltaDegrees = 0.0f;
		bPreparedNaturalEnd = false;
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
		uint8 Rotation = static_cast<uint8>(RotationMode);
		if (Ar.IsSaving() && Rotation > static_cast<uint8>(EGGYGOMotionRotationMode::SourceYaw))
		{
			bOutSuccess = false;
			return false;
		}
		Ar.SerializeBits(&Rotation, 1);
		if (Ar.IsLoading()) RotationMode = static_cast<EGGYGOMotionRotationMode>(Rotation);
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
		uint16 SectionRangeCount = static_cast<uint16>(Wire.Sections.Num());
		if (Ar.IsSaving() && (Wire.Sections.Num() <= 0 || Wire.Sections.Num() > 4096))
		{ bOutSuccess = false; return false; }
		Ar << SectionRangeCount;
		if (SectionRangeCount == 0 || SectionRangeCount > 4096 || SectionRangeCount > Wire.SectionCount)
		{ bOutSuccess = false; return false; }
		if (Ar.IsLoading()) Wire.Sections.SetNum(SectionRangeCount);
		for (FGGYGOActionMotionSourceSection& Section : Wire.Sections)
		{
			Ar << Section.SectionName << Section.SectionIndex;
			Ar << Section.MontageStartSeconds << Section.MontageEndSeconds;
		}
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
			FString Error;
			EGGYGOMotionRotationMode ResolvedRotationMode;
			if (Ar.IsError() || !GGYGOActionMotionEvaluation::ValidateSource(Wire, ResolvedRotationMode, Error)
				|| RotationMode != ResolvedRotationMode)
			{
				bOutSuccess = false; return false;
			}
			OriginalBinding = MakeShared<const FGGYGOActionMotionSourceBinding, ESPMode::ThreadSafe>(MoveTemp(Wire));
			// Native base NetSerialize omits Settings. Derive the fixed execution policy from the
			// explicitly serialized mode, just as local Original Montage admission configures it.
			Settings.UnSetFlag(ERootMotionSourceSettingsFlags::IgnoreZAccumulate);
			Settings.SetFlag(ERootMotionSourceSettingsFlags::UseSensitiveLiftoffCheck);
		}
	}
	else if (Ar.IsLoading())
	{
		OriginalResource.Reset(); OriginalBinding.Reset(); bNativeImported = false;
		Settings.SetFlag(ERootMotionSourceSettingsFlags::IgnoreZAccumulate);
		Settings.UnSetFlag(ERootMotionSourceSettingsFlags::UseSensitiveLiftoffCheck);
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
