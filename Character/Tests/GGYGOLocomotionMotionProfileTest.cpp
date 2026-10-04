/**
 * @file GGYGOLocomotionMotionProfileTest.cpp
 * @brief Pure locomotion profile failure, output reset, and authored zero-speed coverage.
 */
#include "Character/Data/GGYGOLocomotionMotionProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Curves/RichCurve.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	void SetProfileTestLinearCurve(FRuntimeFloatCurve& Curve, float StartValue, float EndValue)
	{
		Curve.GetRichCurve()->SetKeys(TArray<FRichCurveKey>{
			FRichCurveKey(0.0f, StartValue), FRichCurveKey(1.0f, EndValue)});
	}

	TStrongObjectPtr<UGGYGOLocomotionMotionProfile> MakeProfileTestFixture(
		float Speed = 10.0f, float DirectionX = 1.0f, float DirectionY = 0.0f)
	{
		TStrongObjectPtr<UGGYGOLocomotionMotionProfile> Profile(
			NewObject<UGGYGOLocomotionMotionProfile>(GetTransientPackage()));
		Profile->Duration = 1.0f;
		Profile->bLoop = false;
		SetProfileTestLinearCurve(Profile->SpeedCurve, Speed, Speed);
		SetProfileTestLinearCurve(Profile->DirectionXCurve, DirectionX, DirectionX);
		SetProfileTestLinearCurve(Profile->DirectionYCurve, DirectionY, DirectionY);
		SetProfileTestLinearCurve(Profile->YawCurve, 0.0f, 40.0f);
		return Profile;
	}

	FGGYGOLocomotionCurveSample MakeProfileTestDirtySample()
	{
		FGGYGOLocomotionCurveSample Sample;
		Sample.Speed = 11.0f;
		Sample.YawDeltaDegrees = 12.0f;
		Sample.YawTotalDegrees = 13.0f;
		Sample.PositionDelta = FVector(14.0, 15.0, 16.0);
		Sample.Velocity = FVector(17.0, 18.0, 19.0);
		Sample.Direction = FVector(20.0, 21.0, 22.0);
		Sample.DirectionAngle = 23.0f;
		Sample.ClipLength = 24.0f;
		Sample.bLoopClip = true;
		Sample.bHasAuthoredDirection = true;
		Sample.bHasPositionDelta = true;
		Sample.bHasCurveSource = true;
		return Sample;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOLocomotionMotionProfileTest,
	"GGYGO.Movement.Locomotion.Profile.StrictEvaluation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOLocomotionMotionProfileTest::RunTest(const FString& Parameters)
{
	const auto AssertReset = [this](const TCHAR* Case, const FGGYGOLocomotionCurveSample& Sample)
	{
		const FString Prefix = FString(Case) + TEXT(" resets ");
		TestEqual(Prefix + TEXT("Speed"), Sample.Speed, 0.0f, 0.0f);
		TestEqual(Prefix + TEXT("YawDeltaDegrees"), Sample.YawDeltaDegrees, 0.0f, 0.0f);
		TestEqual(Prefix + TEXT("YawTotalDegrees"), Sample.YawTotalDegrees, 0.0f, 0.0f);
		TestEqual(Prefix + TEXT("PositionDelta.X"), Sample.PositionDelta.X, 0.0, 0.0);
		TestEqual(Prefix + TEXT("PositionDelta.Y"), Sample.PositionDelta.Y, 0.0, 0.0);
		TestEqual(Prefix + TEXT("PositionDelta.Z"), Sample.PositionDelta.Z, 0.0, 0.0);
		TestEqual(Prefix + TEXT("Velocity.X"), Sample.Velocity.X, 0.0, 0.0);
		TestEqual(Prefix + TEXT("Velocity.Y"), Sample.Velocity.Y, 0.0, 0.0);
		TestEqual(Prefix + TEXT("Velocity.Z"), Sample.Velocity.Z, 0.0, 0.0);
		TestEqual(Prefix + TEXT("Direction.X"), Sample.Direction.X, 0.0, 0.0);
		TestEqual(Prefix + TEXT("Direction.Y"), Sample.Direction.Y, 0.0, 0.0);
		TestEqual(Prefix + TEXT("Direction.Z"), Sample.Direction.Z, 0.0, 0.0);
		TestEqual(Prefix + TEXT("DirectionAngle"), Sample.DirectionAngle, 0.0f, 0.0f);
		TestEqual(Prefix + TEXT("ClipLength"), Sample.ClipLength, 0.0f, 0.0f);
		TestFalse(Prefix + TEXT("bLoopClip"), Sample.bLoopClip);
		TestFalse(Prefix + TEXT("bHasAuthoredDirection"), Sample.bHasAuthoredDirection);
		TestFalse(Prefix + TEXT("bHasPositionDelta"), Sample.bHasPositionDelta);
		TestFalse(Prefix + TEXT("bHasCurveSource"), Sample.bHasCurveSource);
	};

	const auto AssertFailure = [this, &AssertReset](const TCHAR* Case,
		const UGGYGOLocomotionMotionProfile& Profile, float StartTime, float EndTime,
		FGGYGOLocomotionCurveSample& Sample, const TCHAR* Field, const TCHAR* Reason)
	{
		const FString PreviousError = TEXT("previous evaluation error");
		FString Error = PreviousError;
		TestFalse(FString(Case) + TEXT(" fails explicitly"),
			Profile.EvaluateInterval(StartTime, EndTime, Sample, &Error));
		AssertReset(Case, Sample);
		TestFalse(FString(Case) + TEXT(" returns a non-empty error"), Error.IsEmpty());
		TestFalse(FString(Case) + TEXT(" replaces the previous error"),
			Error.Contains(PreviousError, ESearchCase::CaseSensitive));
		TestTrue(FString(Case) + TEXT(" identifies the field"),
			Error.Contains(Field, ESearchCase::CaseSensitive));
		TestTrue(FString(Case) + TEXT(" identifies the reason"),
			Error.Contains(Reason, ESearchCase::CaseSensitive));
	};

	// 1. A negative authored key must fail validation rather than become zero speed.
	{
		auto Profile = MakeProfileTestFixture(-1.0f);
		FGGYGOLocomotionCurveSample Sample = MakeProfileTestDirtySample();
		AssertFailure(TEXT("Negative speed key"), *Profile, 0.0f, 0.5f, Sample,
			TEXT("SpeedCurve.Keys[0].Value"), TEXT("speed must be non-negative"));
	}

	// 2. Valid positive keys can still produce an invalid negative cubic sample.
	{
		auto Profile = MakeProfileTestFixture();
		FRichCurveKey First(0.0f, 1.0f);
		FRichCurveKey Last(1.0f, 1.0f);
		First.InterpMode = Last.InterpMode = RCIM_Cubic;
		First.TangentMode = Last.TangentMode = RCTM_User;
		First.TangentWeightMode = Last.TangentWeightMode = RCTWM_WeightedNone;
		First.ArriveTangent = First.LeaveTangent = -8.0f;
		Last.ArriveTangent = Last.LeaveTangent = 8.0f;
		FRichCurve* SpeedCurve = Profile->SpeedCurve.GetRichCurve();
		// SetKeys invokes AutoSetTangents; explicitly authored User tangents must survive it.
		SpeedCurve->SetKeys(TArray<FRichCurveKey>{First, Last});
		if (!TestEqual(TEXT("Cubic fixture retains first leave tangent"),
			SpeedCurve->GetFirstKey().LeaveTangent, -8.0f, 0.0f)
			|| !TestEqual(TEXT("Cubic fixture retains last arrive tangent"),
				SpeedCurve->GetLastKey().ArriveTangent, 8.0f, 0.0f)
			|| !TestEqual(TEXT("Native cubic midpoint is negative one"),
				SpeedCurve->Eval(0.5f), -1.0f, 0.0001f))
		{
			return false;
		}
		FString ValidationError = TEXT("previous validation error");
		if (!TestTrue(TEXT("Positive-key cubic fixture passes data validation"),
			Profile->ValidateProfile(ValidationError)))
		{
			return false;
		}
		TestTrue(TEXT("Valid cubic data clears its old validation error"), ValidationError.IsEmpty());
		FGGYGOLocomotionCurveSample Sample = MakeProfileTestDirtySample();
		AssertFailure(TEXT("Negative interpolated speed"), *Profile, 0.0f, 0.5f, Sample,
			TEXT("SpeedCurve"), TEXT("evaluated speed must be non-negative"));
	}

	// 3. Every required curve must exist, including Yaw for otherwise valid motion.
	{
		auto Profile = MakeProfileTestFixture();
		Profile->YawCurve.GetRichCurve()->Reset();
		FGGYGOLocomotionCurveSample Sample = MakeProfileTestDirtySample();
		AssertFailure(TEXT("Missing yaw curve"), *Profile, 0.0f, 0.5f, Sample,
			TEXT("YawCurve"), TEXT("must contain at least one key"));
	}

	// 4. Positive speed cannot silently substitute a forward direction.
	{
		auto Profile = MakeProfileTestFixture(10.0f, 0.0f, 0.0f);
		FGGYGOLocomotionCurveSample Sample = MakeProfileTestDirtySample();
		AssertFailure(TEXT("Positive speed without direction"), *Profile, 0.0f, 0.5f, Sample,
			TEXT("DirectionXCurve/DirectionYCurve"), TEXT("positive SpeedCurve value requires a valid direction"));
	}

	// 5. Authored zero speed and zero direction are valid and still evaluate Yaw.
	{
		auto Profile = MakeProfileTestFixture(0.0f, 0.0f, 0.0f);
		FGGYGOLocomotionCurveSample Sample = MakeProfileTestDirtySample();
		FString Error = TEXT("previous evaluation error");
		if (!TestTrue(TEXT("Authored zero speed succeeds"),
			Profile->EvaluateInterval(0.25f, 0.5f, Sample, &Error)))
		{
			return false;
		}
		TestTrue(TEXT("Zero-speed success clears its old error"), Error.IsEmpty());
		TestEqual(TEXT("Zero-speed sample retains exact zero speed"), Sample.Speed, 0.0f, 0.0f);
		TestEqual(TEXT("Zero-speed velocity"), Sample.Velocity, FVector::ZeroVector, 0.0f);
		TestEqual(TEXT("Zero-speed direction"), Sample.Direction, FVector::ZeroVector, 0.0f);
		TestEqual(TEXT("Zero-speed direction angle"), Sample.DirectionAngle, 0.0f, 0.0f);
		TestEqual(TEXT("Zero-speed yaw total"), Sample.YawTotalDegrees, 20.0f, 0.0001f);
		TestEqual(TEXT("Zero-speed yaw delta"), Sample.YawDeltaDegrees, 10.0f, 0.0001f);
		TestEqual(TEXT("Zero-speed clip length"), Sample.ClipLength, 1.0f, 0.0f);
		TestEqual(TEXT("Zero-speed position delta"), Sample.PositionDelta, FVector::ZeroVector, 0.0f);
		TestFalse(TEXT("Zero-speed sample is non-looping"), Sample.bLoopClip);
		TestFalse(TEXT("Zero-speed sample has no authored direction"), Sample.bHasAuthoredDirection);
		TestFalse(TEXT("Zero-speed sample has no position delta"), Sample.bHasPositionDelta);
		TestTrue(TEXT("Zero-speed sample retains a valid curve source"), Sample.bHasCurveSource);
	}

	// 6. Reusing a successful output for a failing interval must discard every old field.
	{
		auto Profile = MakeProfileTestFixture(10.0f, 0.0f, 1.0f);
		Profile->bLoop = true;
		FGGYGOLocomotionCurveSample Sample = MakeProfileTestDirtySample();
		FString Error = TEXT("previous evaluation error");
		if (!TestTrue(TEXT("Success-before-failure fixture evaluates"),
			Profile->EvaluateInterval(0.25f, 0.5f, Sample, &Error)))
		{
			return false;
		}
		TestTrue(TEXT("Positive-speed success clears its old error"), Error.IsEmpty());
		TestEqual(TEXT("Successful speed"), Sample.Speed, 10.0f, 0.0f);
		TestEqual(TEXT("Successful direction"), Sample.Direction, FVector(0.0, 1.0, 0.0), 0.0f);
		TestEqual(TEXT("Successful velocity"), Sample.Velocity, FVector(0.0, 10.0, 0.0), 0.0f);
		TestEqual(TEXT("Successful direction angle"), Sample.DirectionAngle, 90.0f, 0.0001f);
		TestEqual(TEXT("Successful yaw total"), Sample.YawTotalDegrees, 20.0f, 0.0001f);
		TestEqual(TEXT("Successful yaw delta"), Sample.YawDeltaDegrees, 10.0f, 0.0001f);
		TestEqual(TEXT("Successful clip length"), Sample.ClipLength, 1.0f, 0.0f);
		TestTrue(TEXT("Successful sample is looping"), Sample.bLoopClip);
		TestTrue(TEXT("Successful sample has authored direction"), Sample.bHasAuthoredDirection);
		TestTrue(TEXT("Successful sample has a curve source"), Sample.bHasCurveSource);
		TestEqual(TEXT("Successful sample has zero position delta"), Sample.PositionDelta, FVector::ZeroVector, 0.0f);
		TestFalse(TEXT("Successful sample has no position delta"), Sample.bHasPositionDelta);
		// These caller-owned sentinels cover fields the current producer leaves zero/false.
		Sample.PositionDelta = FVector(31.0, 32.0, 33.0);
		Sample.bHasPositionDelta = true;
		AssertFailure(TEXT("Reversed interval after success"), *Profile, 0.75f, 0.25f, Sample,
			TEXT("EndTime"), TEXT("must not precede StartTime"));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
