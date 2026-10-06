#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "UObject/StrongObjectPtr.h"

/** Original source configuration, not an evaluated trajectory or a playback resource. */
struct FGGYGOActionMotionSourceSegment
{
	TStrongObjectPtr<UAnimSequence> Sequence;
	int32 SegmentIndex = INDEX_NONE;
	float StartPos = 0.0f;
	/** Exact original GetEndPos identity; terminal clock quantization is resolved only during coverage/mapping. */
	float MontageEndSeconds = 0.0f;
	float AnimStartTime = 0.0f;
	float AnimEndTime = 0.0f;
	int32 LoopingCount = 0;
	float AnimPlayRate = 0.0f;
	float SequenceRateScale = 0.0f;
	/** The engine's segment rate: AnimPlayRate * SequenceRateScale, with no near-zero substitution. */
	float SourceSecondsPerMontageSecond = 0.0f;
	float SequenceLength = 0.0f;
	bool bEnableRootMotion = false;

	bool HasSameConfiguration(const FGGYGOActionMotionSourceSegment& Other) const
	{
		return Sequence.Get() == Other.Sequence.Get() && SegmentIndex == Other.SegmentIndex
			&& StartPos == Other.StartPos && MontageEndSeconds == Other.MontageEndSeconds
			&& AnimStartTime == Other.AnimStartTime && AnimEndTime == Other.AnimEndTime
			&& LoopingCount == Other.LoopingCount && AnimPlayRate == Other.AnimPlayRate
			&& SequenceRateScale == Other.SequenceRateScale
			&& SourceSecondsPerMontageSecond == Other.SourceSecondsPerMontageSecond
			&& SequenceLength == Other.SequenceLength && bEnableRootMotion == Other.bEnableRootMotion;
	}
};

/** One original section's immutable absolute montage coordinates, not a playback phase. */
struct FGGYGOActionMotionSourceSection
{
	FName SectionName;
	int32 SectionIndex = INDEX_NONE;
	float MontageStartSeconds = 0.0f;
	float MontageEndSeconds = 0.0f;

	bool HasSameConfiguration(const FGGYGOActionMotionSourceSection& Other) const
	{
		return SectionName == Other.SectionName && SectionIndex == Other.SectionIndex
			&& MontageStartSeconds == Other.MontageStartSeconds && MontageEndSeconds == Other.MontageEndSeconds;
	}
};

/**
 * One explicitly selected continuous section range of one original single-slot montage.
 * Animation resolves asset routing only. GAS owns playback permission, GA owns its resources,
 * and Movement owns the action clock, curve evaluation, replay and capsule execution.
 * Assets are retained only while the caller/current RMS/bounded saved moves retain this binding.
 * This has no curve copies, phase state, clock, graph output or animation instance observer.
 */
struct FGGYGOActionMotionSourceBinding
{
	TStrongObjectPtr<UAnimMontage> Montage;
	FName SlotName;
	/** The first selected section; this does not restrict playback to that section. */
	FName SectionName;
	int32 TrackIndex = INDEX_NONE;
	int32 SectionIndex = INDEX_NONE;
	int32 TrackSegmentCount = 0;
	int32 SectionCount = 0;
	float MontageStartSeconds = 0.0f;
	float MontageEndSeconds = 0.0f;
	float MontageLength = 0.0f;
	/** Asset rate only. The caller's effective montage rate already includes this exactly once. */
	float MontageRateScale = 0.0f;
	bool bMontageHasRootMotion = false;
	/** Every selected section, including both endpoints, in original montage order. Never empty. */
	TArray<FGGYGOActionMotionSourceSection> Sections;
	/** Only segments covering the requested range, in original track order. */
	TArray<FGGYGOActionMotionSourceSegment> Segments;

	const FGGYGOActionMotionSourceSection* FindSection(FName Name) const
	{
		return Sections.FindByPredicate([Name](const FGGYGOActionMotionSourceSection& Section)
			{ return Section.SectionName == Name; });
	}

	bool HasSameConfiguration(const FGGYGOActionMotionSourceBinding& Other) const
	{
		if (Montage.Get() != Other.Montage.Get() || SlotName != Other.SlotName
			|| SectionName != Other.SectionName || TrackIndex != Other.TrackIndex
			|| SectionIndex != Other.SectionIndex || TrackSegmentCount != Other.TrackSegmentCount
			|| SectionCount != Other.SectionCount || MontageStartSeconds != Other.MontageStartSeconds
			|| MontageEndSeconds != Other.MontageEndSeconds || MontageLength != Other.MontageLength
			|| MontageRateScale != Other.MontageRateScale
			|| bMontageHasRootMotion != Other.bMontageHasRootMotion
			|| Sections.Num() != Other.Sections.Num() || Segments.Num() != Other.Segments.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < Sections.Num(); ++Index)
		{
			if (!Sections[Index].HasSameConfiguration(Other.Sections[Index])) { return false; }
		}
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			if (!Segments[Index].HasSameConfiguration(Other.Segments[Index])) { return false; }
		}
		return true;
	}
};

using FGGYGOActionMotionSourceBindingPtr = TSharedPtr<const FGGYGOActionMotionSourceBinding, ESPMode::ThreadSafe>;

/** One contiguous piece in one source loop. Cumulative curve values must be differenced per piece. */
struct FGGYGOActionMotionSourceInterval
{
	/** Index into the binding's Segments, not the original montage's track. */
	int32 BindingSegmentIndex = INDEX_NONE;
	int32 LoopIndex = INDEX_NONE;
	double MontageStartSeconds = 0.0;
	double MontageEndSeconds = 0.0;
	double SequenceStartSeconds = 0.0;
	double SequenceEndSeconds = 0.0;
	float SourceSecondsPerMontageSecond = 0.0f;
};

namespace GGYGOActionMotionSource
{
	/**
	 * Game-thread asset queries. Requires explicit slot/section, one slot, forward UAnimSequence
	 * segments covering the entire section, and no baked TimeStretch data. No default route.
	 * Does not validate movement curves or grant a live playback; those belong to the consumers.
	 * Every failure resets OutBinding and supplies an asset/slot/section/field diagnostic.
	 */
	GGYGO_API bool BuildSourceBinding(UAnimMontage* Montage, FName SlotName, FName SectionName,
		FGGYGOActionMotionSourceBindingPtr& OutBinding, FString& OutError);

	/**
	 * Explicit ordered inclusive section range. Retains each original section and source segment.
	 * Does not infer runtime section links, read curves or grant playback/phase permissions.
	 * Missing/ambiguous/reversed sections or incomplete source coverage reject the entire range.
	 */
	GGYGO_API bool BuildSourceBindingRange(UAnimMontage* Montage, FName SlotName,
		FName StartSectionName, FName FinalSectionName,
		FGGYGOActionMotionSourceBindingPtr& OutBinding, FString& OutError);

	/** Exact source configuration validation; never replaces a stale binding with a new one. */
	GGYGO_API bool ValidateSourceBinding(const FGGYGOActionMotionSourceBinding& Binding, FString& OutError);

	/**
	 * Map ordered absolute montage coordinates within this range into original source pieces.
	 * Segment and loop boundaries are split; the zero-length interval is valid after validation.
	 * The final track endpoint may share the native montage endpoint only for a verified frame
	 * round trip within two float ULPs. It maps to the original trim end; internal seams remain exact.
	 * Does not apply task/global/Montage.RateScale, advance time, read curves or execute movement.
	 * Caller must provide a fixed effective montage rate and retire/restart on playback changes.
	 * A request requiring more than 4096 source pieces is rejected, not partially returned.
	 * Every failure empties OutIntervals. Outputs use double for source coordinate arithmetic.
	 */
	GGYGO_API bool MapMontageInterval(const FGGYGOActionMotionSourceBinding& Binding,
		float MontageStartSeconds, float MontageEndSeconds,
		TArray<FGGYGOActionMotionSourceInterval>& OutIntervals, FString& OutError);
}
