/**
 * @file GGYGOMovementSetValidationTest.cpp
 * @brief Pure MovementSet configuration validation and editor adapter coverage.
 */
#include "Character/Data/GGYGOMovementSet.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Character/Data/GGYGOLocomotionMotionProfile.h"
#include "Curves/RichCurve.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif
#include <limits>

namespace
{
	struct FMovementSetScalarCase
	{
		const TCHAR* Field;
		float UGGYGOMovementSet::* Member;
		float Minimum;
		float Maximum;
		bool bHasMaximum;
	};

	// Expectations are authored from the frozen public contract, not production rule tables.
	const FMovementSetScalarCase MovementSetScalarCases[] = {
		{TEXT("WalkSpeed"), &UGGYGOMovementSet::WalkSpeed, 0.0f, 0.0f, false},
		{TEXT("RunSpeed"), &UGGYGOMovementSet::RunSpeed, 0.0f, 0.0f, false},
		{TEXT("StartStopSelectionSeconds"), &UGGYGOMovementSet::StartStopSelectionSeconds, 0.0f, 0.0f, false},
		{TEXT("RotationYawRate"), &UGGYGOMovementSet::RotationYawRate, 0.0f, 0.0f, false},
		{TEXT("MaxAcceleration"), &UGGYGOMovementSet::MaxAcceleration, 0.0f, 0.0f, false},
		{TEXT("BrakingDecelerationWalking"), &UGGYGOMovementSet::BrakingDecelerationWalking, 0.0f, 0.0f, false},
		{TEXT("GroundFriction"), &UGGYGOMovementSet::GroundFriction, 0.0f, 0.0f, false},
		{TEXT("RootMotionScale"), &UGGYGOMovementSet::RootMotionScale, 0.0f, 0.0f, false},
		{TEXT("TurnBackYawSettleDegrees"), &UGGYGOMovementSet::TurnBackYawSettleDegrees, 0.0f, 0.0f, false},
		{TEXT("TurnBackRunOutMinSpeed"), &UGGYGOMovementSet::TurnBackRunOutMinSpeed, 0.0f, 0.0f, false},
		{TEXT("TurnBackDurationSeconds"), &UGGYGOMovementSet::TurnBackDurationSeconds, 0.0f, 0.0f, false},
		{TEXT("WalkToRunHoldSeconds"), &UGGYGOMovementSet::WalkToRunHoldSeconds, 0.1f, 60.0f, true},
		{TEXT("WalkRunBlendInterpSpeed"), &UGGYGOMovementSet::WalkRunBlendInterpSpeed, 0.0f, 50.0f, true},
		{TEXT("TurnBackReverseInputDotThreshold"), &UGGYGOMovementSet::TurnBackReverseInputDotThreshold, -1.0f, 1.0f, true},
		{TEXT("TurnBackMinYawDegrees"), &UGGYGOMovementSet::TurnBackMinYawDegrees, 0.0f, 180.0f, true},
		{TEXT("TurnBackRunOutForwardThreshold"), &UGGYGOMovementSet::TurnBackRunOutForwardThreshold, 0.0f, 1.0f, true}
	};
	static_assert(UE_ARRAY_COUNT(MovementSetScalarCases) == 16);

	struct FMovementSetProfileCase
	{
		const TCHAR* Field;
		TObjectPtr<const UGGYGOLocomotionMotionProfile> UGGYGOMovementSet::* Member;
		bool bExpectedLoop;
	};
	const FMovementSetProfileCase MovementSetProfileCases[] = {
		{TEXT("WalkStartProfile"), &UGGYGOMovementSet::WalkStartProfile, false},
		{TEXT("WalkLoopProfile"), &UGGYGOMovementSet::WalkLoopProfile, true},
		{TEXT("RunLoopProfile"), &UGGYGOMovementSet::RunLoopProfile, true},
		{TEXT("StartStopProfile"), &UGGYGOMovementSet::StartStopProfile, false},
		{TEXT("WalkStopProfile"), &UGGYGOMovementSet::WalkStopProfile, false},
		{TEXT("RunStopProfile"), &UGGYGOMovementSet::RunStopProfile, false},
		{TEXT("TurnBackProfile"), &UGGYGOMovementSet::TurnBackProfile, false}
	};
	static_assert(UE_ARRAY_COUNT(MovementSetProfileCases) == 7);

	uint32 FloatBitsForMovementSetValidation(float Value)
	{
		static_assert(sizeof(float) == sizeof(uint32));
		uint32 Bits;
		FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
		return Bits;
	}

	struct FMovementSetConfigurationSnapshot
	{
		uint32 FloatBits[17];
		bool bOrientRotationToMovement;
		bool bUseCurveDrivenSpeed;
		const UGGYGOLocomotionMotionProfile* Profiles[7];
	};

	FMovementSetConfigurationSnapshot TakeMovementSetValidationSnapshot(const UGGYGOMovementSet& Set)
	{
		FMovementSetConfigurationSnapshot Snapshot;
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(MovementSetScalarCases); ++Index)
		{
			Snapshot.FloatBits[Index] = FloatBitsForMovementSetValidation(Set.*MovementSetScalarCases[Index].Member);
		}
		Snapshot.FloatBits[16] = FloatBitsForMovementSetValidation(Set.MaxCurveDrivenSpeed);
		Snapshot.bOrientRotationToMovement = Set.bOrientRotationToMovement;
		Snapshot.bUseCurveDrivenSpeed = Set.bUseCurveDrivenSpeed;
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(MovementSetProfileCases); ++Index)
		{
			Snapshot.Profiles[Index] = (Set.*MovementSetProfileCases[Index].Member).Get();
		}
		return Snapshot;
	}

	TStrongObjectPtr<UGGYGOLocomotionMotionProfile> CreateMovementSetValidationProfile(bool bLoop, float Speed = 10.0f)
	{
		TStrongObjectPtr<UGGYGOLocomotionMotionProfile> Profile(
			NewObject<UGGYGOLocomotionMotionProfile>(GetTransientPackage()));
		Profile->Duration = 1.0f;
		Profile->bLoop = bLoop;
		const auto SetLinear = [](FRuntimeFloatCurve& Curve, float First, float Last)
		{
			Curve.GetRichCurve()->SetKeys(TArray<FRichCurveKey>{FRichCurveKey(0.0f, First), FRichCurveKey(1.0f, Last)});
		};
		SetLinear(Profile->SpeedCurve, Speed, Speed);
		SetLinear(Profile->DirectionXCurve, 1.0f, 1.0f);
		SetLinear(Profile->DirectionYCurve, 0.0f, 0.0f);
		SetLinear(Profile->YawCurve, 0.0f, 40.0f);
		return Profile;
	}

	void ConfigureMovementSetValidationProfiles(UGGYGOMovementSet& Set, bool bCurveMode,
		const UGGYGOLocomotionMotionProfile* SingleProfile, const UGGYGOLocomotionMotionProfile* LoopProfile)
	{
		Set.bUseCurveDrivenSpeed = bCurveMode;
		for (const FMovementSetProfileCase& Case : MovementSetProfileCases)
		{
			Set.*Case.Member = bCurveMode ? (Case.bExpectedLoop ? LoopProfile : SingleProfile) : nullptr;
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOMovementSetValidationTest,
	"GGYGO.Movement.Locomotion.MovementSet.StrictValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOMovementSetValidationTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UGGYGOMovementSet> Set(NewObject<UGGYGOMovementSet>(GetTransientPackage()));
	auto SingleProfile = CreateMovementSetValidationProfile(false);
	auto LoopProfile = CreateMovementSetValidationProfile(true);
	FString FixtureError;
	if (!TestNotNull(TEXT("Transient MovementSet fixture"), Set.Get())
		|| !TestTrue(TEXT("Non-loop fixture passes native Profile validation"), SingleProfile->ValidateProfile(FixtureError))
		|| !TestTrue(TEXT("Loop fixture passes native Profile validation"), LoopProfile->ValidateProfile(FixtureError)))
	{
		return false;
	}
	ConfigureMovementSetValidationProfiles(*Set, true, SingleProfile.Get(), LoopProfile.Get());

	const auto CheckUnchanged = [this](const FString& Case, const FMovementSetConfigurationSnapshot& Before,
		const UGGYGOMovementSet& After)
	{
		bool bPassed = true;
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(MovementSetScalarCases); ++Index)
		{
			bPassed &= TestEqual(Case + TEXT(" preserves ") + MovementSetScalarCases[Index].Field,
				FloatBitsForMovementSetValidation(After.*MovementSetScalarCases[Index].Member), Before.FloatBits[Index]);
		}
		bPassed &= TestEqual(Case + TEXT(" preserves MaxCurveDrivenSpeed"),
			FloatBitsForMovementSetValidation(After.MaxCurveDrivenSpeed), Before.FloatBits[16]);
		bPassed &= TestEqual(Case + TEXT(" preserves bOrientRotationToMovement"),
			After.bOrientRotationToMovement, Before.bOrientRotationToMovement);
		bPassed &= TestEqual(Case + TEXT(" preserves bUseCurveDrivenSpeed"),
			After.bUseCurveDrivenSpeed, Before.bUseCurveDrivenSpeed);
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(MovementSetProfileCases); ++Index)
		{
			bPassed &= TestTrue(Case + TEXT(" preserves ") + MovementSetProfileCases[Index].Field,
				(After.*MovementSetProfileCases[Index].Member).Get() == Before.Profiles[Index]);
		}
		return bPassed;
	};

	const auto CheckValidation = [this, &CheckUnchanged](const FString& Case, const UGGYGOMovementSet& Config,
		bool bExpectedSuccess, const TCHAR* Field, const TCHAR* Reason, FString* CapturedError = nullptr)
	{
		const FMovementSetConfigurationSnapshot Before = TakeMovementSetValidationSnapshot(Config);
		const FString PreviousError = TEXT("previous configuration error");
		FString Error = PreviousError;
		bool bPassed = TestEqual(Case + TEXT(" returns the expected result"),
			Config.ValidateMovementSet(Error), bExpectedSuccess);
		if (bExpectedSuccess)
		{
			bPassed &= TestTrue(Case + TEXT(" clears the previous error"), Error.IsEmpty());
		}
		else
		{
			bPassed &= TestFalse(Case + TEXT(" returns a non-empty error"), Error.IsEmpty());
			bPassed &= TestFalse(Case + TEXT(" replaces the previous error"), Error.Contains(PreviousError));
			bPassed &= TestTrue(Case + TEXT(" identifies MovementSet and its path"),
				Error.Contains(TEXT("MovementSet '")) && Error.Contains(Config.GetPathName(), ESearchCase::CaseSensitive));
			bPassed &= TestTrue(Case + TEXT(" identifies the field"), Error.Contains(Field, ESearchCase::CaseSensitive));
			bPassed &= TestTrue(Case + TEXT(" identifies the reason"), Error.Contains(Reason, ESearchCase::CaseSensitive));
		}
		bPassed &= CheckUnchanged(Case, Before, Config);
		if (CapturedError)
		{
			*CapturedError = Error;
		}
		return bPassed;
	};

	bool bPassed = CheckValidation(TEXT("Complete curve configuration"), *Set, true, nullptr, nullptr);
	if (!bPassed)
	{
		return false;
	}
	ConfigureMovementSetValidationProfiles(*Set, false, SingleProfile.Get(), LoopProfile.Get());
	if (!CheckValidation(TEXT("Explicit fixed mode with seven empty profiles"), *Set, true, nullptr, nullptr))
	{
		return false;
	}

	// 1. All sixteen scalar contracts: finite values, lower bounds, closed ranges and authored zero.
	const float InvalidFiniteValues[] = {
		std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
		-std::numeric_limits<float>::infinity()
	};
	for (const FMovementSetScalarCase& Case : MovementSetScalarCases)
	{
		const float Original = (*Set).*Case.Member;
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(InvalidFiniteValues); ++Index)
		{
			(*Set).*Case.Member = InvalidFiniteValues[Index];
			bPassed &= CheckValidation(FString::Printf(TEXT("Finite %s / %d"), Case.Field, Index),
				*Set, false, Case.Field, TEXT("must be finite"));
			(*Set).*Case.Member = Original;
		}
		(*Set).*Case.Member = Case.Minimum - 1.0f;
		bPassed &= CheckValidation(FString(Case.Field) + TEXT(" below minimum"), *Set, false, Case.Field,
			Case.bHasMaximum ? TEXT("must be in [") : TEXT("must be non-negative"));
		(*Set).*Case.Member = 0.0f;
		bPassed &= CheckValidation(FString(Case.Field) + TEXT(" authored zero"), *Set,
			Case.Minimum <= 0.0f, Case.Field, TEXT("must be in ["));
		if (Case.bHasMaximum)
		{
			(*Set).*Case.Member = Case.Maximum + 1.0f;
			bPassed &= CheckValidation(FString(Case.Field) + TEXT(" above maximum"), *Set, false, Case.Field, TEXT("must be in ["));
			(*Set).*Case.Member = Case.Minimum;
			bPassed &= CheckValidation(FString(Case.Field) + TEXT(" inclusive minimum"), *Set, true, nullptr, nullptr);
			(*Set).*Case.Member = Case.Maximum;
			bPassed &= CheckValidation(FString(Case.Field) + TEXT(" inclusive maximum"), *Set, true, nullptr, nullptr);
		}
		(*Set).*Case.Member = Original;
	}
	const float OriginalDeprecatedValue = Set->MaxCurveDrivenSpeed;
	Set->MaxCurveDrivenSpeed = std::numeric_limits<float>::quiet_NaN();
	bPassed &= CheckValidation(TEXT("Unused deprecated bound is not validated or repaired"), *Set, true, nullptr, nullptr);
	Set->MaxCurveDrivenSpeed = OriginalDeprecatedValue;

	// 2. Serialized Profile slots are migration history, not numeric MovementSet authority.
	for (const FMovementSetProfileCase& Case : MovementSetProfileCases)
	{
		ConfigureMovementSetValidationProfiles(*Set, true, SingleProfile.Get(), LoopProfile.Get());
		(*Set).*Case.Member = nullptr;
		bPassed &= CheckValidation(FString(Case.Field) + TEXT(" missing historical reference"), *Set, true, nullptr, nullptr);
	}

	// 3. Historical loop flags cannot authorize or reject a numeric Set; real source loop admission is tested at its consumer.
	const bool CurveModes[] = {false, true};
	for (const bool bCurveMode : CurveModes)
	{
		for (const FMovementSetProfileCase& Case : MovementSetProfileCases)
		{
			ConfigureMovementSetValidationProfiles(*Set, bCurveMode, SingleProfile.Get(), LoopProfile.Get());
			auto WrongLoopProfile = CreateMovementSetValidationProfile(!Case.bExpectedLoop);
			if (!TestTrue(FString(Case.Field) + TEXT(" wrong-loop fixture has valid curve data"),
				WrongLoopProfile->ValidateProfile(FixtureError)))
			{
				return false;
			}
			const bool OriginalLoop = WrongLoopProfile->bLoop;
			(*Set).*Case.Member = WrongLoopProfile.Get();
			FString Error;
			const FString Name = FString::Printf(TEXT("%s wrong Loop / mode %d"), Case.Field, int32(bCurveMode));
			bPassed &= CheckValidation(Name, *Set, true, nullptr, nullptr, &Error);
			bPassed &= TestEqual(Name + TEXT(" preserves the wrong Loop flag"), WrongLoopProfile->bLoop, OriginalLoop);
		}
	}

	// 4. Preserve Profile's original negative-key evidence without making it a second runtime source.
	auto NegativeProfile = CreateMovementSetValidationProfile(false, -1.0f);
	FString NativeProfileError;
	if (!TestFalse(TEXT("Negative Speed fixture fails native Profile validation"), NegativeProfile->ValidateProfile(NativeProfileError))
		|| !TestTrue(TEXT("Native negative-key error identifies its key and cause"),
			NativeProfileError.Contains(TEXT("SpeedCurve.Keys[0].Value"))
			&& NativeProfileError.Contains(TEXT("speed must be non-negative"))))
	{
		return false;
	}
	const TArray<FRichCurveKey> OriginalNegativeKeys = NegativeProfile->SpeedCurve.GetRichCurveConst()->GetConstRefOfKeys();
	const uint32 OriginalNegativeDuration = FloatBitsForMovementSetValidation(NegativeProfile->Duration);
	const bool OriginalNegativeLoop = NegativeProfile->bLoop;
	const auto CheckNegativeProfileUnchanged = [this, &NegativeProfile, &OriginalNegativeKeys,
		OriginalNegativeDuration, OriginalNegativeLoop](const FString& Case)
	{
		const TArray<FRichCurveKey>& Keys = NegativeProfile->SpeedCurve.GetRichCurveConst()->GetConstRefOfKeys();
		bool bUnchanged = TestEqual(Case + TEXT(" preserves Speed key count"), Keys.Num(), OriginalNegativeKeys.Num());
		// Native FRichCurveKey equality omits weights and some tangents; compare all nine fields.
		for (int32 Index = 0; Index < FMath::Min(Keys.Num(), OriginalNegativeKeys.Num()); ++Index)
		{
			const FRichCurveKey& Key = Keys[Index];
			const FRichCurveKey& Original = OriginalNegativeKeys[Index];
			const FString Prefix = FString::Printf(TEXT("%s preserves Speed key %d "), *Case, Index);
			bUnchanged &= TestEqual(Prefix + TEXT("Time"),
				FloatBitsForMovementSetValidation(Key.Time), FloatBitsForMovementSetValidation(Original.Time));
			bUnchanged &= TestEqual(Prefix + TEXT("Value"),
				FloatBitsForMovementSetValidation(Key.Value), FloatBitsForMovementSetValidation(Original.Value));
			bUnchanged &= TestEqual(Prefix + TEXT("ArriveTangent"),
				FloatBitsForMovementSetValidation(Key.ArriveTangent), FloatBitsForMovementSetValidation(Original.ArriveTangent));
			bUnchanged &= TestEqual(Prefix + TEXT("LeaveTangent"),
				FloatBitsForMovementSetValidation(Key.LeaveTangent), FloatBitsForMovementSetValidation(Original.LeaveTangent));
			bUnchanged &= TestEqual(Prefix + TEXT("ArriveTangentWeight"),
				FloatBitsForMovementSetValidation(Key.ArriveTangentWeight), FloatBitsForMovementSetValidation(Original.ArriveTangentWeight));
			bUnchanged &= TestEqual(Prefix + TEXT("LeaveTangentWeight"),
				FloatBitsForMovementSetValidation(Key.LeaveTangentWeight), FloatBitsForMovementSetValidation(Original.LeaveTangentWeight));
			bUnchanged &= TestEqual(Prefix + TEXT("InterpMode"),
				int32(Key.InterpMode.GetValue()), int32(Original.InterpMode.GetValue()));
			bUnchanged &= TestEqual(Prefix + TEXT("TangentMode"),
				int32(Key.TangentMode.GetValue()), int32(Original.TangentMode.GetValue()));
			bUnchanged &= TestEqual(Prefix + TEXT("TangentWeightMode"),
				int32(Key.TangentWeightMode.GetValue()), int32(Original.TangentWeightMode.GetValue()));
		}
		bUnchanged &= TestEqual(Case + TEXT(" preserves Profile Duration"),
			FloatBitsForMovementSetValidation(NegativeProfile->Duration), OriginalNegativeDuration);
		bUnchanged &= TestEqual(Case + TEXT(" preserves Profile Loop"), NegativeProfile->bLoop, OriginalNegativeLoop);
		return bUnchanged;
	};
	for (const bool bCurveMode : CurveModes)
	{
		ConfigureMovementSetValidationProfiles(*Set, bCurveMode, SingleProfile.Get(), LoopProfile.Get());
		Set->WalkStartProfile = NegativeProfile.Get();
		FString Error;
		const FString Name = FString::Printf(TEXT("Assigned negative Profile / mode %d"), int32(bCurveMode));
		bPassed &= CheckValidation(Name, *Set, true, nullptr, nullptr, &Error);
		bPassed &= CheckNegativeProfileUnchanged(Name);
	}

#if WITH_EDITOR
	// 5. The editor and runtime report the same numeric contract, without warnings.
	const auto CheckEditor = [this, &CheckValidation, &CheckUnchanged](const FString& Case,
		const UGGYGOMovementSet& Config, bool bExpectedSuccess, const TCHAR* Field, const TCHAR* Reason)
	{
		const FMovementSetConfigurationSnapshot Before = TakeMovementSetValidationSnapshot(Config);
		FString RuntimeError;
		bool bEditorPassed = CheckValidation(Case + TEXT(" runtime"), Config, bExpectedSuccess, Field, Reason, &RuntimeError);
		FDataValidationContext Context;
		const EDataValidationResult Result = Config.IsDataValid(Context);
		TArray<FText> Warnings;
		TArray<FText> Errors;
		Context.SplitIssues(Warnings, Errors);
		bEditorPassed &= TestEqual(Case + TEXT(" editor result"), static_cast<int32>(Result),
			static_cast<int32>(bExpectedSuccess ? EDataValidationResult::Valid : EDataValidationResult::Invalid));
		bEditorPassed &= TestEqual(Case + TEXT(" context error count"), Context.GetNumErrors(), uint32(bExpectedSuccess ? 0 : 1));
		bEditorPassed &= TestEqual(Case + TEXT(" extracted error count"), Errors.Num(), bExpectedSuccess ? 0 : 1);
		bEditorPassed &= TestEqual(Case + TEXT(" context warning count"), Context.GetNumWarnings(), uint32(0));
		bEditorPassed &= TestEqual(Case + TEXT(" extracted warning count"), Warnings.Num(), 0);
		if (!bExpectedSuccess && Errors.IsValidIndex(0))
		{
			bEditorPassed &= TestEqual(Case + TEXT(" exact runtime/editor diagnostic"), Errors[0].ToString(), RuntimeError);
		}
		bEditorPassed &= CheckUnchanged(Case + TEXT(" editor"), Before, Config);
		return bEditorPassed;
	};
	ConfigureMovementSetValidationProfiles(*Set, true, SingleProfile.Get(), LoopProfile.Get());
	bPassed &= CheckEditor(TEXT("Valid configuration"), *Set, true, nullptr, nullptr);
	const float OriginalScale = Set->RootMotionScale;
	Set->RootMotionScale = std::numeric_limits<float>::quiet_NaN();
	bPassed &= CheckEditor(TEXT("Non-finite configuration"), *Set, false, TEXT("RootMotionScale"), TEXT("must be finite"));
	Set->RootMotionScale = OriginalScale;
	ConfigureMovementSetValidationProfiles(*Set, false, SingleProfile.Get(), LoopProfile.Get());
	Set->WalkStartProfile = NegativeProfile.Get();
	bPassed &= CheckEditor(TEXT("Historical invalid Profile does not change the numeric contract"), *Set, true, nullptr, nullptr);
	bPassed &= CheckNegativeProfileUnchanged(TEXT("Editor Profile validation"));
#endif

	return bPassed;
}

#endif // WITH_DEV_AUTOMATION_TESTS
