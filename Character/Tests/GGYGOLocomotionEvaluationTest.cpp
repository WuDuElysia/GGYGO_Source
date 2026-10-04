/** C33 / P1-T1: real-profile coverage for three frozen mathematical contracts. */
#include "Character/Data/GGYGOLocomotionEvaluation.h"
#include "Character/Data/GGYGOLocomotionMotionProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Curves/RichCurve.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace GGYGOLocomotionEvaluationT1
{
	constexpr const TCHAR* PreviousError = TEXT("C33 T1 stale evaluation error");

	void SetConstantCurve(FRuntimeFloatCurve& Curve, float Value)
	{
		Curve.GetRichCurve()->SetKeys(TArray<FRichCurveKey>{FRichCurveKey(0.0f, Value)});
	}

	void SetLinearCurve(FRuntimeFloatCurve& Curve, float Duration, float StartValue, float EndValue)
	{
		FRichCurveKey First(0.0f, StartValue);
		FRichCurveKey Last(Duration, EndValue);
		First.InterpMode = Last.InterpMode = RCIM_Linear;
		Curve.GetRichCurve()->SetKeys(TArray<FRichCurveKey>{First, Last});
	}

	TStrongObjectPtr<UGGYGOLocomotionMotionProfile> MakeConstantProfile(
		float Duration, bool bLoop, float Speed, float DirectionX, float DirectionY, float Yaw)
	{
		TStrongObjectPtr<UGGYGOLocomotionMotionProfile> Profile(
			NewObject<UGGYGOLocomotionMotionProfile>(GetTransientPackage()));
		Profile->Duration = Duration;
		Profile->bLoop = bLoop;
		SetConstantCurve(Profile->SpeedCurve, Speed);
		SetConstantCurve(Profile->DirectionXCurve, DirectionX);
		SetConstantCurve(Profile->DirectionYCurve, DirectionY);
		SetConstantCurve(Profile->YawCurve, Yaw);
		return Profile;
	}

	FGGYGOLocomotionEvaluationResult MakeDirtyMotion()
	{
		FGGYGOLocomotionEvaluationResult Result;
		Result.Sample.Speed = 11.0f;
		Result.Sample.YawDeltaDegrees = 12.0f;
		Result.Sample.YawTotalDegrees = 13.0f;
		Result.Sample.PositionDelta = FVector(14.0, 15.0, 16.0);
		Result.Sample.Velocity = FVector(17.0, 18.0, 19.0);
		Result.Sample.Direction = FVector(20.0, 21.0, 22.0);
		Result.Sample.DirectionAngle = 23.0f;
		Result.Sample.ClipLength = 24.0f;
		Result.Sample.bLoopClip = true;
		Result.Sample.bHasAuthoredDirection = true;
		Result.Sample.bHasPositionDelta = true;
		Result.Sample.bHasCurveSource = true;
		Result.ScaledSpeed = 25.0f;
		Result.ScaledVelocity = FVector(26.0, 27.0, 28.0);
		return Result;
	}

	FGGYGOWalkRunEvaluationResult MakeDirtyWalkRun()
	{
		FGGYGOWalkRunEvaluationResult Result;
		Result.Motion = MakeDirtyMotion();
		Result.EndCyclePosition = 29.0f;
		return Result;
	}

	void CheckVector(FAutomationTestBase& Test, const FString& Label,
		const FVector& Actual, const FVector& Expected, double Tolerance)
	{
		Test.TestEqual(Label + TEXT(".X"), Actual.X, Expected.X, Tolerance);
		Test.TestEqual(Label + TEXT(".Y"), Actual.Y, Expected.Y, Tolerance);
		Test.TestEqual(Label + TEXT(".Z"), Actual.Z, Expected.Z, Tolerance);
	}

	void CheckSample(FAutomationTestBase& Test, const FString& Label,
		const FGGYGOLocomotionCurveSample& Actual, const FGGYGOLocomotionCurveSample& Expected,
		double VectorTolerance = 0.0, float AngleTolerance = 0.0f)
	{
		Test.TestEqual(Label + TEXT(".Speed"), Actual.Speed, Expected.Speed, 0.0f);
		Test.TestEqual(Label + TEXT(".YawDeltaDegrees"), Actual.YawDeltaDegrees, Expected.YawDeltaDegrees, 0.0f);
		Test.TestEqual(Label + TEXT(".YawTotalDegrees"), Actual.YawTotalDegrees, Expected.YawTotalDegrees, 0.0f);
		CheckVector(Test, Label + TEXT(".PositionDelta"), Actual.PositionDelta, Expected.PositionDelta, 0.0);
		CheckVector(Test, Label + TEXT(".Velocity"), Actual.Velocity, Expected.Velocity, VectorTolerance);
		CheckVector(Test, Label + TEXT(".Direction"), Actual.Direction, Expected.Direction, VectorTolerance);
		Test.TestEqual(Label + TEXT(".DirectionAngle"), Actual.DirectionAngle, Expected.DirectionAngle, AngleTolerance);
		Test.TestEqual(Label + TEXT(".ClipLength"), Actual.ClipLength, Expected.ClipLength, 0.0f);
		Test.TestEqual(Label + TEXT(".bLoopClip"), Actual.bLoopClip, Expected.bLoopClip);
		Test.TestEqual(Label + TEXT(".bHasAuthoredDirection"), Actual.bHasAuthoredDirection, Expected.bHasAuthoredDirection);
		Test.TestEqual(Label + TEXT(".bHasPositionDelta"), Actual.bHasPositionDelta, Expected.bHasPositionDelta);
		Test.TestEqual(Label + TEXT(".bHasCurveSource"), Actual.bHasCurveSource, Expected.bHasCurveSource);
	}

	void CheckMotion(FAutomationTestBase& Test, const FString& Label,
		const FGGYGOLocomotionEvaluationResult& Actual, const FGGYGOLocomotionEvaluationResult& Expected,
		double VectorTolerance = 0.0, float AngleTolerance = 0.0f)
	{
		CheckSample(Test, Label + TEXT(".Sample"), Actual.Sample, Expected.Sample, VectorTolerance, AngleTolerance);
		Test.TestEqual(Label + TEXT(".ScaledSpeed"), Actual.ScaledSpeed, Expected.ScaledSpeed, 0.0f);
		CheckVector(Test, Label + TEXT(".ScaledVelocity"), Actual.ScaledVelocity, Expected.ScaledVelocity, VectorTolerance);
	}

	void CheckWalkRun(FAutomationTestBase& Test, const FString& Label,
		const FGGYGOWalkRunEvaluationResult& Actual, const FGGYGOWalkRunEvaluationResult& Expected,
		double VectorTolerance = 0.0, float AngleTolerance = 0.0f)
	{
		CheckMotion(Test, Label + TEXT(".Motion"), Actual.Motion, Expected.Motion, VectorTolerance, AngleTolerance);
		Test.TestEqual(Label + TEXT(".EndCyclePosition"), Actual.EndCyclePosition, Expected.EndCyclePosition, 0.0f);
	}

	void CheckReplacedError(FAutomationTestBase& Test, const FString& Label, const FString& Error)
	{
		Test.TestFalse(Label + TEXT(" reports an error"), Error.IsEmpty());
		Test.TestFalse(Label + TEXT(" replaces the old error"), Error.Contains(PreviousError, ESearchCase::CaseSensitive));
	}

	void CheckFailure(FAutomationTestBase& Test, const FString& Label,
		const FGGYGOLocomotionEvaluationResult& Result, const FString& Error)
	{
		CheckMotion(Test, Label + TEXT(" reset"), Result, FGGYGOLocomotionEvaluationResult());
		CheckReplacedError(Test, Label, Error);
	}

	void CheckFailure(FAutomationTestBase& Test, const FString& Label,
		const FGGYGOWalkRunEvaluationResult& Result, const FString& Error)
	{
		CheckWalkRun(Test, Label + TEXT(" reset"), Result, FGGYGOWalkRunEvaluationResult());
		CheckReplacedError(Test, Label, Error);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOLocomotionSingleIntervalRawAndScaledTest,
	"GGYGO.Movement.Locomotion.Evaluation.SingleInterval.RawAndScaled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOLocomotionSingleIntervalRawAndScaledTest::RunTest(const FString& Parameters)
{
	using namespace GGYGOLocomotionEvaluationT1;
	auto Profile = MakeConstantProfile(1.0f, false, 8.0f, 1.0f, 0.0f, 0.0f);
	SetLinearCurve(Profile->YawCurve, 1.0f, 0.0f, 40.0f);
	FGGYGOLocomotionCurveSample NativeSample;
	FString NativeError = PreviousError;
	if (!TestTrue(TEXT("Native single fixture evaluates"),
		Profile->EvaluateInterval(0.25f, 0.5f, NativeSample, &NativeError)))
	{
		return false;
	}
	TestTrue(TEXT("Native single fixture clears old error"), NativeError.IsEmpty());

	FGGYGOLocomotionEvaluationResult Expected;
	Expected.Sample.Speed = 8.0f;
	Expected.Sample.YawDeltaDegrees = 10.0f;
	Expected.Sample.YawTotalDegrees = 20.0f;
	Expected.Sample.Velocity = FVector(8.0, 0.0, 0.0);
	Expected.Sample.Direction = FVector(1.0, 0.0, 0.0);
	Expected.Sample.ClipLength = 1.0f;
	Expected.Sample.bHasAuthoredDirection = true;
	Expected.Sample.bHasCurveSource = true;
	CheckSample(*this, TEXT("Independent native fixture"), NativeSample, Expected.Sample);

	struct FScaleCase { float Scale; float ExpectedSpeed; FVector ExpectedVelocity; };
	const FScaleCase Cases[] = {
		{2.0f, 16.0f, FVector(16.0, 0.0, 0.0)},
		{0.0f, 0.0f, FVector::ZeroVector}};
	for (const FScaleCase& Case : Cases)
	{
		const FString Label = FString::Printf(TEXT("Single Scale=%.0f"), Case.Scale);
		FGGYGOLocomotionEvaluationResult Result = MakeDirtyMotion();
		FString Error = PreviousError;
		const bool bSucceeded = GGYGOLocomotionEvaluation::EvaluateSingleInterval(
			Profile.Get(), 0.25f, 0.5f, Case.Scale, Result, &Error);
		if (!TestTrue(Label + TEXT(" succeeds"), bSucceeded))
		{
			CheckFailure(*this, Label, Result, Error);
			return false;
		}
		Expected.ScaledSpeed = Case.ExpectedSpeed;
		Expected.ScaledVelocity = Case.ExpectedVelocity;
		CheckMotion(*this, Label, Result, Expected);
		CheckSample(*this, Label + TEXT(" preserves native sample"), Result.Sample, NativeSample);
		TestTrue(Label + TEXT(" clears old error"), Error.IsEmpty());
	}

	// A delegated asset failure must also clear fields not written by Profile.
	Profile->YawCurve.GetRichCurve()->Reset();
	FGGYGOLocomotionEvaluationResult FailedResult = MakeDirtyMotion();
	FString Error = PreviousError;
	TestFalse(TEXT("Single missing Yaw fails"), GGYGOLocomotionEvaluation::EvaluateSingleInterval(
		Profile.Get(), 0.25f, 0.5f, 2.0f, FailedResult, &Error));
	CheckFailure(*this, TEXT("Single missing Yaw"), FailedResult, Error);
	TestTrue(TEXT("Single error identifies Profile"), Error.Contains(Profile->GetPathName(), ESearchCase::CaseSensitive));
	TestTrue(TEXT("Single error identifies interval"), Error.Contains(TEXT("Interval=[0.25, 0.5]"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("Single error retains native missing Yaw reason"),
		Error.Contains(TEXT("YawCurve must contain at least one key."), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOLocomotionWalkRunRequiredEndpointsTest,
	"GGYGO.Movement.Locomotion.Evaluation.WalkRun.RequiredEndpoints",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOLocomotionWalkRunRequiredEndpointsTest::RunTest(const FString& Parameters)
{
	using namespace GGYGOLocomotionEvaluationT1;
	// Exact binary periods differ by 2^90; both are legal Profile durations.
	struct FPeriods { float Walk; float Run; };
	const FPeriods PeriodCases[] = {{0x1p80f, 0x1p-10f}, {0x1p-10f, 0x1p80f}};
	const float EndpointAlphas[] = {0.0f, 1.0f};
	for (const FPeriods& Periods : PeriodCases)
	{
		auto Walk = MakeConstantProfile(Periods.Walk, true, 8.0f, 1.0f, 0.0f, 11.0f);
		auto Run = MakeConstantProfile(Periods.Run, true, 24.0f, 0.0f, 1.0f, 17.0f);
		for (const float Alpha : EndpointAlphas)
		{
			const FString Label = FString::Printf(TEXT("WalkPeriod=%.9g RunPeriod=%.9g Alpha=%.0f"),
				Periods.Walk, Periods.Run, Alpha);
			FGGYGOLocomotionCurveSample WalkNative;
			FGGYGOLocomotionCurveSample RunNative;
			FString WalkError = PreviousError;
			FString RunError = PreviousError;
			const bool bWalkValid = Walk->EvaluateInterval(Periods.Walk * 0.25f, Periods.Walk * 0.5f, WalkNative, &WalkError);
			const bool bRunValid = Run->EvaluateInterval(Periods.Run * 0.25f, Periods.Run * 0.5f, RunNative, &RunError);
			if (!TestTrue(Label + TEXT(" real Walk evaluates"), bWalkValid)
				|| !TestTrue(Label + TEXT(" real Run evaluates"), bRunValid))
			{
				return false;
			}
			TestTrue(Label + TEXT(" Walk native error cleared"), WalkError.IsEmpty());
			TestTrue(Label + TEXT(" Run native error cleared"), RunError.IsEmpty());
			const bool bSelectWalk = Alpha == 0.0f;
			const float SelectedPeriod = bSelectWalk ? Periods.Walk : Periods.Run;
			FGGYGOWalkRunEvaluationResult Result = MakeDirtyWalkRun();
			FString Error = PreviousError;
			const bool bSucceeded = GGYGOLocomotionEvaluation::EvaluateWalkRunInterval(
				Walk.Get(), Run.Get(), 0.25f, SelectedPeriod * 0.25f, Alpha, 2.0f, Result, &Error);
			if (!TestTrue(Label + TEXT(" succeeds"), bSucceeded))
			{
				CheckFailure(*this, Label, Result, Error);
				return false;
			}
			FGGYGOWalkRunEvaluationResult Expected;
			Expected.Motion.Sample = bSelectWalk ? WalkNative : RunNative;
			Expected.Motion.ScaledSpeed = bSelectWalk ? 16.0f : 48.0f;
			Expected.Motion.ScaledVelocity = bSelectWalk ? FVector(16.0, 0.0, 0.0) : FVector(0.0, 48.0, 0.0);
			Expected.EndCyclePosition = 0.5f;
			CheckWalkRun(*this, Label, Result, Expected);
			TestEqual(Label + TEXT(" exact selected period"), Result.Motion.Sample.ClipLength, SelectedPeriod, 0.0f);
			TestEqual(Label + TEXT(" selected raw speed"), Result.Motion.Sample.Speed, bSelectWalk ? 8.0f : 24.0f, 0.0f);
			TestEqual(Label + TEXT(" selected raw Yaw"), Result.Motion.Sample.YawTotalDegrees, bSelectWalk ? 11.0f : 17.0f, 0.0f);
			TestTrue(Label + TEXT(" clears old error"), Error.IsEmpty());
		}
	}

	enum class EBadSide { MissingProfile, NonLoop, MissingYaw, NegativeSpeedKey };
	struct FBadCase { EBadSide Kind; const TCHAR* Label; const TCHAR* NativeField; const TCHAR* Reason; };
	const FBadCase BadCases[] = {
		{EBadSide::MissingProfile, TEXT("Missing unused Profile"), TEXT(""), TEXT("must reference a valid Profile object")},
		{EBadSide::NonLoop, TEXT("Unused non Loop"), TEXT(""), TEXT("must be true for a WalkRun interval")},
		{EBadSide::MissingYaw, TEXT("Unused missing Yaw"), TEXT("YawCurve"), TEXT("must contain at least one key")},
		{EBadSide::NegativeSpeedKey, TEXT("Unused negative Speed key"), TEXT("SpeedCurve.Keys[1].Value"), TEXT("speed must be non-negative")}};
	for (const float Alpha : EndpointAlphas)
	{
		for (const FBadCase& Case : BadCases)
		{
			auto Walk = MakeConstantProfile(1.0f, true, 8.0f, 1.0f, 0.0f, 11.0f);
			auto Run = MakeConstantProfile(1.0f, true, 24.0f, 0.0f, 1.0f, 17.0f);
			const bool bBadRun = Alpha == 0.0f;
			UGGYGOLocomotionMotionProfile* BadProfile = bBadRun ? Run.Get() : Walk.Get();
			const UGGYGOLocomotionMotionProfile* WalkInput = Walk.Get();
			const UGGYGOLocomotionMotionProfile* RunInput = Run.Get();
			switch (Case.Kind)
			{
			case EBadSide::MissingProfile:
				if (bBadRun) { RunInput = nullptr; } else { WalkInput = nullptr; }
				break;
			case EBadSide::NonLoop:
				BadProfile->bLoop = false;
				break;
			case EBadSide::MissingYaw:
				BadProfile->YawCurve.GetRichCurve()->Reset();
				break;
			case EBadSide::NegativeSpeedKey:
				SetLinearCurve(BadProfile->SpeedCurve, 1.0f, 8.0f, -1.0f);
				break;
			}
			const FString Side = bBadRun ? TEXT("RunLoopProfile") : TEXT("WalkLoopProfile");
			const FString Label = FString::Printf(TEXT("%s Alpha=%.0f rejects %s"), Case.Label, Alpha, *Side);
			FGGYGOWalkRunEvaluationResult Result = MakeDirtyWalkRun();
			FString Error = PreviousError;
			TestFalse(Label, GGYGOLocomotionEvaluation::EvaluateWalkRunInterval(
				WalkInput, RunInput, 0.25f, 0.25f, Alpha, 2.0f, Result, &Error));
			CheckFailure(*this, Label, Result, Error);
			const FString FailureField = Side + (Case.Kind == EBadSide::NonLoop ? TEXT(".bLoop") : TEXT(""));
			TestTrue(Label + TEXT(" reports unused side as failing field"),
				Error.Contains(TEXT(": ") + FailureField + TEXT(":"), ESearchCase::CaseSensitive));
			TestTrue(Label + TEXT(" retains reason"), Error.Contains(Case.Reason, ESearchCase::CaseSensitive));
			if (Case.Kind != EBadSide::MissingProfile)
			{
				TestTrue(Label + TEXT(" identifies bad asset path"), Error.Contains(BadProfile->GetPathName(), ESearchCase::CaseSensitive));
			}
			if (Case.Kind == EBadSide::MissingYaw || Case.Kind == EBadSide::NegativeSpeedKey)
			{
				TestTrue(Label + TEXT(" retains native field/key"), Error.Contains(Case.NativeField, ESearchCase::CaseSensitive));
				TestTrue(Label + TEXT(" identifies mapped Walk interval"), Error.Contains(TEXT("WalkInterval=[0.25, 0.5]"), ESearchCase::CaseSensitive));
				TestTrue(Label + TEXT(" identifies mapped Run interval"), Error.Contains(TEXT("RunInterval=[0.25, 0.5]"), ESearchCase::CaseSensitive));
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOLocomotionWalkRunSharedPhaseYawTest,
	"GGYGO.Movement.Locomotion.Evaluation.WalkRun.SharedPhaseYaw",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOLocomotionWalkRunSharedPhaseYawTest::RunTest(const FString& Parameters)
{
	using namespace GGYGOLocomotionEvaluationT1;
	auto Walk = MakeConstantProfile(2.0f, true, 4.0f, 1.0f, 0.0f, 0.0f);
	auto Run = MakeConstantProfile(4.0f, true, 8.0f, 0.0f, 1.0f, 0.0f);
	SetLinearCurve(Walk->SpeedCurve, 2.0f, 4.0f, 20.0f);
	SetLinearCurve(Walk->YawCurve, 2.0f, 0.0f, 80.0f);
	SetLinearCurve(Run->SpeedCurve, 4.0f, 8.0f, 72.0f);
	SetLinearCurve(Run->YawCurve, 4.0f, 0.0f, 160.0f);
	FGGYGOWalkRunEvaluationResult Result = MakeDirtyWalkRun();
	FString Error = PreviousError;
	const bool bSucceeded = GGYGOLocomotionEvaluation::EvaluateWalkRunInterval(
		Walk.Get(), Run.Get(), 0.75f, 6.25f, 0.25f, 2.0f, Result, &Error);
	if (!TestTrue(TEXT("Shared phase crossing multiple cycles succeeds"), bSucceeded))
	{
		CheckFailure(*this, TEXT("Shared phase"), Result, Error);
		return false;
	}

	// Independent numbers: endpoint cycle 3.25; Walk Yaw 260/200, Run 520/400.
	// Normalized (3,1,0), raw speed 12 and scaled speed 24 give the vectors below.
	FGGYGOWalkRunEvaluationResult Expected;
	Expected.EndCyclePosition = 3.25f;
	Expected.Motion.Sample.Speed = 12.0f;
	Expected.Motion.Sample.YawTotalDegrees = 325.0f;
	Expected.Motion.Sample.YawDeltaDegrees = 250.0f;
	Expected.Motion.Sample.Direction = FVector(0.9486832980505138, 0.31622776601683794, 0.0);
	Expected.Motion.Sample.Velocity = FVector(11.384199576606166, 3.794733192202055, 0.0);
	Expected.Motion.Sample.DirectionAngle = 18.4349488229f;
	Expected.Motion.Sample.ClipLength = 2.5f;
	Expected.Motion.Sample.bLoopClip = true;
	Expected.Motion.Sample.bHasAuthoredDirection = true;
	Expected.Motion.Sample.bHasCurveSource = true;
	Expected.Motion.ScaledSpeed = 24.0f;
	Expected.Motion.ScaledVelocity = FVector(22.768399153212332, 7.58946638440411, 0.0);
	CheckWalkRun(*this, TEXT("Independent shared phase values"), Result, Expected, 1e-6, 1e-4f);
	TestTrue(TEXT("Shared phase success clears old error"), Error.IsEmpty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
