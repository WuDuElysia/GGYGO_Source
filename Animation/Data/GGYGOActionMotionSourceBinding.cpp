#include "Animation/Data/GGYGOActionMotionSourceBinding.h"

namespace GGYGOActionMotionSourcePrivate
{
	constexpr int32 MaxMappedIntervals = 4096;

	bool Fail(UAnimMontage* Montage, FName Slot, FName Section, const TCHAR* Field,
		const FString& Reason, FString& OutError)
	{
		OutError = FString::Printf(TEXT("Animation.ActionMotionSource Montage='%s' Slot='%s' Section='%s' %s: %s"),
			*GetPathNameSafe(Montage), *Slot.ToString(), *Section.ToString(), Field, *Reason);
		return false;
	}

	/** Resolve metadata in build mode, or compare in validation mode without rebuilding a binding. */
	bool Resolve(UAnimMontage* Montage, FName Slot, FName Section,
		FGGYGOActionMotionSourceBinding* Building, const FGGYGOActionMotionSourceBinding* Expected,
		FString& OutError)
	{
		const auto Reject = [&](const TCHAR* Field, const FString& Reason)
		{ return Fail(Montage, Slot, Section, Field, Reason, OutError); };
		if (!IsInGameThread()) { return Reject(TEXT("Thread"), TEXT("source asset queries require the game thread")); }
		if (!IsValid(Montage)) { return Reject(TEXT("Montage"), TEXT("original asset is missing or invalid")); }
		if (Slot.IsNone() || Section.IsNone())
		{ return Reject(TEXT("Route"), TEXT("both slot and section must be explicitly configured")); }
		if (!FMath::IsFinite(Montage->RateScale) || Montage->RateScale <= 0.0f)
		{ return Reject(TEXT("RateScale"), TEXT("asset rate must be finite and positive")); }
		if (Montage->TimeStretchCurve.IsValid())
		{ return Reject(TEXT("TimeStretchCurve"), TEXT("baked time stretch is unsupported by the fixed-rate source contract")); }
		if (Montage->SlotAnimTracks.Num() != 1 || Montage->SlotAnimTracks[0].SlotName != Slot)
		{ return Reject(TEXT("SlotAnimTracks"), TEXT("requires one track with the explicitly configured slot; mixed slots are unsupported")); }
		const float MontageLength = Montage->GetPlayLength();
		if (!FMath::IsFinite(MontageLength) || MontageLength <= 0.0f)
		{ return Reject(TEXT("PlayLength"), TEXT("montage must have a finite positive length")); }

		int32 SectionIndex = INDEX_NONE;
		float PreviousSectionTime = -1.0f;
		for (int32 Index = 0; Index < Montage->CompositeSections.Num(); ++Index)
		{
			const FCompositeSection& Current = Montage->CompositeSections[Index];
			const float Time = Current.GetTime();
			if (!FMath::IsFinite(Time) || Time < 0.0f || Time >= MontageLength || Time <= PreviousSectionTime)
			{ return Reject(TEXT("CompositeSections"), TEXT("section starts must be finite, strictly ordered and inside the montage")); }
			PreviousSectionTime = Time;
			if (Current.SectionName == Section)
			{
				if (SectionIndex != INDEX_NONE)
				{ return Reject(TEXT("SectionName"), TEXT("requested section name is ambiguous")); }
				SectionIndex = Index;
			}
		}
		if (SectionIndex == INDEX_NONE) { return Reject(TEXT("SectionName"), TEXT("requested section does not exist")); }
		float SectionStart = 0.0f, SectionEnd = 0.0f;
		Montage->GetSectionStartAndEndTime(SectionIndex, SectionStart, SectionEnd);
		if (!FMath::IsFinite(SectionStart) || !FMath::IsFinite(SectionEnd)
			|| SectionStart < 0.0f || SectionEnd <= SectionStart || SectionEnd > MontageLength)
		{ return Reject(TEXT("SectionRange"), TEXT("requested section has an invalid actual range")); }
		const FAnimTrack& Track = Montage->SlotAnimTracks[0].AnimTrack;
		const bool bHasRootMotion = Montage->HasRootMotion();
		if (Expected && (Expected->TrackIndex != 0 || Expected->SectionIndex != SectionIndex
			|| Expected->TrackSegmentCount != Track.AnimSegments.Num()
			|| Expected->SectionCount != Montage->CompositeSections.Num()
			|| Expected->MontageStartSeconds != SectionStart || Expected->MontageEndSeconds != SectionEnd
			|| Expected->MontageLength != MontageLength || Expected->MontageRateScale != Montage->RateScale
			|| Expected->bMontageHasRootMotion != bHasRootMotion))
		{ return Reject(TEXT("Configuration"), TEXT("original montage routing, range, rate or root-motion configuration changed")); }
		if (Building)
		{
			Building->Montage = TStrongObjectPtr<UAnimMontage>(Montage);
			Building->SlotName = Slot;
			Building->SectionName = Section;
			Building->TrackIndex = 0;
			Building->SectionIndex = SectionIndex;
			Building->TrackSegmentCount = Track.AnimSegments.Num();
			Building->SectionCount = Montage->CompositeSections.Num();
			Building->MontageStartSeconds = SectionStart;
			Building->MontageEndSeconds = SectionEnd;
			Building->MontageLength = MontageLength;
			Building->MontageRateScale = Montage->RateScale;
			Building->bMontageHasRootMotion = bHasRootMotion;
		}

		float CoveredUntil = SectionStart;
		float PreviousSegmentEnd = 0.0f;
		int32 BindingIndex = 0;
		for (int32 Index = 0; Index < Track.AnimSegments.Num(); ++Index)
		{
			const FAnimSegment& Original = Track.AnimSegments[Index];
			UAnimSequence* Sequence = Cast<UAnimSequence>(Original.GetAnimReference().Get());
			const auto RejectSegment = [&](const FString& Reason)
			{
				return Reject(TEXT("Segment"), FString::Printf(TEXT("index=%d Source='%s': %s"),
					Index, *GetPathNameSafe(Original.GetAnimReference().Get()), *Reason));
			};
			if (!Original.IsValid() || !IsValid(Sequence))
			{ return RejectSegment(TEXT("requires a valid original UAnimSequence; nested sources are unsupported")); }
			const float SequenceLength = Sequence->GetPlayLength();
			if (!FMath::IsFinite(SequenceLength) || SequenceLength <= 0.0f
				|| !FMath::IsFinite(Original.AnimStartTime) || !FMath::IsFinite(Original.AnimEndTime)
				|| Original.AnimStartTime < 0.0f || Original.AnimEndTime <= Original.AnimStartTime
				|| Original.AnimEndTime > SequenceLength || Original.LoopingCount <= 0)
			{ return RejectSegment(TEXT("requires finite in-sequence trim bounds and a positive loop count")); }
			if (!FMath::IsFinite(Original.AnimPlayRate) || Original.AnimPlayRate <= 0.0f
				|| !FMath::IsFinite(Sequence->RateScale) || Sequence->RateScale <= 0.0f)
			{ return RejectSegment(TEXT("segment and sequence rates must be finite and positive; reverse playback is unsupported")); }
			const float SourceRate = Original.AnimPlayRate * Sequence->RateScale;
			if (!FMath::IsFinite(SourceRate) || SourceRate <= 0.0f || FMath::IsNearlyZero(SourceRate))
			{ return RejectSegment(TEXT("combined source rate is invalid or would trigger the engine's near-zero-to-one substitution")); }
			// Raw rates are checked before calling helpers which otherwise silently substitute one.
			const float SegmentEnd = Original.GetEndPos();
			if (!FMath::IsFinite(Original.StartPos) || Original.StartPos < 0.0f
				|| !FMath::IsFinite(SegmentEnd) || SegmentEnd <= Original.StartPos
				|| (Index > 0 && Original.StartPos < PreviousSegmentEnd))
			{ return RejectSegment(TEXT("requires finite ordered segment ranges without overlaps")); }
			const double LoopDuration = static_cast<double>(Original.AnimEndTime - Original.AnimStartTime) / SourceRate;
			const double LastLoopStart = Original.StartPos + static_cast<double>(Original.LoopingCount - 1) * LoopDuration;
			if (!FMath::IsFinite(LoopDuration) || LoopDuration <= 0.0
				|| !FMath::IsFinite(LastLoopStart) || LastLoopStart >= SegmentEnd)
			{ return RejectSegment(TEXT("native segment coordinates cannot represent a positive duration for every source loop")); }
			PreviousSegmentEnd = SegmentEnd;
			if (SegmentEnd <= SectionStart || Original.StartPos >= SectionEnd) { continue; }
			const float CoveredStart = FMath::Max(Original.StartPos, SectionStart);
			const float CoveredEnd = FMath::Min(SegmentEnd, SectionEnd);
			if (CoveredStart != CoveredUntil)
			{ return RejectSegment(TEXT("requested section contains a source gap or overlap")); }
			CoveredUntil = CoveredEnd;

			if (Expected)
			{
				if (!Expected->Segments.IsValidIndex(BindingIndex))
				{ return RejectSegment(TEXT("original section's source membership changed")); }
				const FGGYGOActionMotionSourceSegment& Saved = Expected->Segments[BindingIndex];
				if (Saved.Sequence.Get() != Sequence || Saved.SegmentIndex != Index
					|| Saved.StartPos != Original.StartPos || Saved.MontageEndSeconds != SegmentEnd
					|| Saved.AnimStartTime != Original.AnimStartTime || Saved.AnimEndTime != Original.AnimEndTime
					|| Saved.LoopingCount != Original.LoopingCount || Saved.AnimPlayRate != Original.AnimPlayRate
					|| Saved.SequenceRateScale != Sequence->RateScale || Saved.SequenceLength != SequenceLength
					|| Saved.SourceSecondsPerMontageSecond != SourceRate
					|| Saved.bEnableRootMotion != Sequence->bEnableRootMotion)
				{ return RejectSegment(TEXT("original source, trim, rate, loop or root-motion configuration changed")); }
			}
			if (Building)
			{
				FGGYGOActionMotionSourceSegment& Saved = Building->Segments.AddDefaulted_GetRef();
				Saved.Sequence = TStrongObjectPtr<UAnimSequence>(Sequence);
				Saved.SegmentIndex = Index;
				Saved.StartPos = Original.StartPos;
				Saved.MontageEndSeconds = SegmentEnd;
				Saved.AnimStartTime = Original.AnimStartTime;
				Saved.AnimEndTime = Original.AnimEndTime;
				Saved.LoopingCount = Original.LoopingCount;
				Saved.AnimPlayRate = Original.AnimPlayRate;
				Saved.SequenceRateScale = Sequence->RateScale;
				Saved.SourceSecondsPerMontageSecond = SourceRate;
				Saved.SequenceLength = SequenceLength;
				Saved.bEnableRootMotion = Sequence->bEnableRootMotion;
			}
			++BindingIndex;
		}
		if (BindingIndex == 0 || CoveredUntil != SectionEnd
			|| (Expected && Expected->Segments.Num() != BindingIndex))
		{ return Reject(TEXT("Coverage"), TEXT("original sources must cover the complete requested section")); }
		return true;
	}
}

bool GGYGOActionMotionSource::BuildSourceBinding(UAnimMontage* Montage, FName SlotName,
	FName SectionName, FGGYGOActionMotionSourceBindingPtr& OutBinding, FString& OutError)
{
	OutBinding.Reset();
	OutError.Reset();
	const TSharedRef<FGGYGOActionMotionSourceBinding, ESPMode::ThreadSafe> Candidate =
		MakeShared<FGGYGOActionMotionSourceBinding, ESPMode::ThreadSafe>();
	if (!GGYGOActionMotionSourcePrivate::Resolve(Montage, SlotName, SectionName, &Candidate.Get(), nullptr, OutError))
	{ return false; }
	OutBinding = Candidate;
	return true;
}

bool GGYGOActionMotionSource::ValidateSourceBinding(const FGGYGOActionMotionSourceBinding& Binding, FString& OutError)
{
	OutError.Reset();
	return GGYGOActionMotionSourcePrivate::Resolve(Binding.Montage.Get(), Binding.SlotName,
		Binding.SectionName, nullptr, &Binding, OutError);
}

bool GGYGOActionMotionSource::MapMontageInterval(const FGGYGOActionMotionSourceBinding& Binding,
	float MontageStartSeconds, float MontageEndSeconds,
	TArray<FGGYGOActionMotionSourceInterval>& OutIntervals, FString& OutError)
{
	using namespace GGYGOActionMotionSourcePrivate;
	OutIntervals.Reset();
	OutError.Reset();
	if (!ValidateSourceBinding(Binding, OutError)) { return false; }
	const auto Reject = [&](const FString& Reason)
	{
		OutIntervals.Reset();
		return Fail(Binding.Montage.Get(), Binding.SlotName, Binding.SectionName, TEXT("Interval"),
			FString::Printf(TEXT("[%.9g, %.9g]: %s"), MontageStartSeconds, MontageEndSeconds, *Reason), OutError);
	};
	if (!FMath::IsFinite(MontageStartSeconds) || !FMath::IsFinite(MontageEndSeconds)
		|| MontageStartSeconds < Binding.MontageStartSeconds || MontageEndSeconds < MontageStartSeconds
		|| MontageEndSeconds > Binding.MontageEndSeconds)
	{ return Reject(TEXT("requires finite ordered absolute coordinates inside the original section; jumps/reverse ranges are not contiguous intervals")); }
	if (MontageStartSeconds == MontageEndSeconds) { return true; }

	double CoveredUntil = MontageStartSeconds;
	for (int32 BindingIndex = 0; BindingIndex < Binding.Segments.Num(); ++BindingIndex)
	{
		const FGGYGOActionMotionSourceSegment& Segment = Binding.Segments[BindingIndex];
		const double Start = FMath::Max(static_cast<double>(MontageStartSeconds), static_cast<double>(Segment.StartPos));
		const double End = FMath::Min(static_cast<double>(MontageEndSeconds), static_cast<double>(Segment.MontageEndSeconds));
		if (Start >= End) { continue; }
		if (Start != CoveredUntil) { return Reject(TEXT("original source pieces do not cover the requested interval")); }
		// The source span is the engine's float subtraction; retain its rate product exactly once.
		const double SourceSpan = static_cast<double>(Segment.AnimEndTime - Segment.AnimStartTime);
		const double Rate = Segment.SourceSecondsPerMontageSecond;
		const double LoopDuration = SourceSpan / Rate;
		if (!FMath::IsFinite(LoopDuration) || LoopDuration <= 0.0)
		{ return Reject(TEXT("original source loop has an unrepresentable duration")); }
		// A native float segment endpoint can round beyond the ideal final loop; it remains that loop's endpoint.
		const double FirstLoop = FMath::Min(FMath::FloorToDouble((Start - Segment.StartPos) / LoopDuration),
			static_cast<double>(Segment.LoopingCount - 1));
		if (!FMath::IsFinite(FirstLoop) || FirstLoop < 0.0 || FirstLoop > Segment.LoopingCount - 1)
		{ return Reject(TEXT("original source loop index is invalid")); }
		int32 LoopIndex = static_cast<int32>(FirstLoop);
		double Cursor = Start;
		while (Cursor < End)
		{
			if (LoopIndex >= Segment.LoopingCount || OutIntervals.Num() >= MaxMappedIntervals)
			{ return Reject(TEXT("source interval exceeds its loop range or the 4096-piece mapping limit")); }
			const double LoopStart = Segment.StartPos + static_cast<double>(LoopIndex) * LoopDuration;
			const double LoopEnd = LoopIndex == Segment.LoopingCount - 1
				? Segment.MontageEndSeconds : Segment.StartPos + static_cast<double>(LoopIndex + 1) * LoopDuration;
			const double PieceEnd = FMath::Min(End, LoopEnd);
			if (!FMath::IsFinite(LoopStart) || !FMath::IsFinite(LoopEnd) || Cursor < LoopStart || PieceEnd <= Cursor)
			{ return Reject(TEXT("original loop boundaries cannot represent a forward contiguous source interval")); }
			const auto SourceTime = [&](double Position)
			{
				if (Position == LoopStart) { return static_cast<double>(Segment.AnimStartTime); }
				if (Position == LoopEnd) { return static_cast<double>(Segment.AnimEndTime); }
				// Clamp to the authored trim at the engine's rounded native segment endpoint.
				return FMath::Clamp(static_cast<double>(Segment.AnimStartTime) + (Position - LoopStart) * Rate,
					static_cast<double>(Segment.AnimStartTime), static_cast<double>(Segment.AnimEndTime));
			};
			FGGYGOActionMotionSourceInterval& Piece = OutIntervals.AddDefaulted_GetRef();
			Piece.BindingSegmentIndex = BindingIndex;
			Piece.LoopIndex = LoopIndex;
			Piece.MontageStartSeconds = Cursor;
			Piece.MontageEndSeconds = PieceEnd;
			Piece.SequenceStartSeconds = SourceTime(Cursor);
			Piece.SequenceEndSeconds = SourceTime(PieceEnd);
			Piece.SourceSecondsPerMontageSecond = Segment.SourceSecondsPerMontageSecond;
			if (!FMath::IsFinite(Piece.SequenceStartSeconds) || !FMath::IsFinite(Piece.SequenceEndSeconds)
				|| Piece.SequenceEndSeconds < Piece.SequenceStartSeconds)
			{ return Reject(TEXT("mapped source coordinates are non-finite or reversed")); }
			Cursor = PieceEnd;
			++LoopIndex;
		}
		CoveredUntil = End;
	}
	if (CoveredUntil != static_cast<double>(MontageEndSeconds))
	{ return Reject(TEXT("original sources do not cover the entire requested interval")); }
	return true;
}
