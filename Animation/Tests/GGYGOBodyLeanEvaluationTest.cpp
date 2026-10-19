/**
 * @file GGYGOBodyLeanEvaluationTest.cpp
 * @brief 整身倾身的脚轨迹、骨长、空间基准与拒绝契约
 */
#include "Animation/Nodes/GGYGOBodyLeanEvaluation.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	FGGYGOBodyLeanInput MakePose()
	{
		FGGYGOBodyLeanInput Input;
		Input.Body = FTransform(FQuat::Identity, FVector(0.0, 0.0, 100.0));
		Input.LeanAxisCS = -FVector::ForwardVector;
		Input.UpDirectionCS = FVector::UpVector;
		for (int32 Index = 0; Index < 2; ++Index)
		{
			const double Side = Index == 0 ? -10.0 : 10.0;
			Input.Legs[Index].Upper = FTransform(FQuat::Identity, FVector(0.0, Side, 90.0));
			Input.Legs[Index].Lower = FTransform(FQuat::Identity, FVector(8.0, Side, 45.0));
			Input.Legs[Index].Foot = FTransform(FQuat::Identity, FVector(0.0, Side, 0.0));
			Input.Legs[Index].KneeBendDirectionCS = FVector::ForwardVector;
		}
		return Input;
	}

	FTransform ChangeBasis(const FTransform& Transform, const FQuat& Rotation, const FVector& Translation)
	{
		return FTransform(Rotation * Transform.GetRotation(),
			Rotation.RotateVector(Transform.GetTranslation()) + Translation, Transform.GetScale3D());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOBodyLeanPoseContractTest,
	"GGYGO.Animation.BodyLean.PoseContract", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOBodyLeanPoseContractTest::RunTest(const FString& Parameters)
{
	FGGYGOBodyLeanInput Input = MakePose();
	FGGYGOBodyLeanResult Result;
	FString Diagnostic;
	if (!TestTrue(TEXT("中立姿态求解成功"), FGGYGOBodyLeanEvaluation::Evaluate(Input, Result, Diagnostic)))
	{
		AddError(Diagnostic);
		return false;
	}
	TestTrue(TEXT("零倾身完整保留原Body变换"), Result.Body.Equals(Input.Body, 0.0));
	for (int32 LegIndex = 0; LegIndex < 2; ++LegIndex)
	{
		TestTrue(TEXT("零倾身保留原膝姿态"), Result.Legs[LegIndex].Lower.Equals(Input.Legs[LegIndex].Lower, 0.0));
	}
	for (const float Angle : { -47.515743f, -20.0f, 20.0f, 47.515743f, 80.0f })
	{
		Input.LeanDegrees = Angle;
		if (!TestTrue(TEXT("目标正负倾身可达"), FGGYGOBodyLeanEvaluation::Evaluate(Input, Result, Diagnostic)))
		{
			AddError(FString::Printf(TEXT("Angle=%f %s"), Angle, *Diagnostic));
			return false;
		}
		const FQuat ExpectedRotation(Input.LeanAxisCS, FMath::DegreesToRadians(Angle));
		TestTrue(TEXT("Body按实际倾身轴完整旋转"), Result.Body.GetRotation().Equals(ExpectedRotation * Input.Body.GetRotation(), 1.e-5));
		for (int32 LegIndex = 0; LegIndex < 2; ++LegIndex)
		{
			const FGGYGOBodyLeanLegPose& Source = Input.Legs[LegIndex];
			const FGGYGOBodyLeanLegPose& Solved = Result.Legs[LegIndex];
			TestTrue(TEXT("脚位置与旋转保留当前动画目标"), Solved.Foot.Equals(Source.Foot, 1.e-4));
			TestTrue(TEXT("大腿不被拉长"), FMath::IsNearlyEqual(
				FVector::Distance(Solved.Upper.GetTranslation(), Solved.Lower.GetTranslation()),
				FVector::Distance(Source.Upper.GetTranslation(), Source.Lower.GetTranslation()), 1.e-4));
			TestTrue(TEXT("小腿不被拉长"), FMath::IsNearlyEqual(
				FVector::Distance(Solved.Lower.GetTranslation(), Solved.Foot.GetTranslation()),
				FVector::Distance(Source.Lower.GetTranslation(), Source.Foot.GetTranslation()), 1.e-4));
		}
	}
	// 更换本次输入脚轨迹后必须采用新目标，不能保留上次输出或世界锁脚。
	Input.Legs[0].Foot.AddToTranslation(FVector(3.0, 2.0, 4.0));
	if (TestTrue(TEXT("当前输入姿态可以变化"), FGGYGOBodyLeanEvaluation::Evaluate(Input, Result, Diagnostic)))
	{
		TestTrue(TEXT("消费本次源脚而非旧脚"), Result.Legs[0].Foot.Equals(Input.Legs[0].Foot, 1.e-4));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOBodyLeanBasisContractTest,
	"GGYGO.Animation.BodyLean.ComponentBasis", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOBodyLeanBasisContractTest::RunTest(const FString& Parameters)
{
	FGGYGOBodyLeanInput Input = MakePose();
	Input.LeanDegrees = 47.515743f;
	FGGYGOBodyLeanResult Original;
	FString Diagnostic;
	if (!TestTrue(TEXT("基准姿态求解"), FGGYGOBodyLeanEvaluation::Evaluate(Input, Original, Diagnostic)))
	{
		AddError(Diagnostic);
		return false;
	}
	const FQuat Rotation = FRotator(23.0, 137.0, -17.0).Quaternion();
	const FVector Translation(12.0, -37.0, 8.0);
	Input.Body = ChangeBasis(Input.Body, Rotation, Translation);
	Input.LeanAxisCS = Rotation.RotateVector(Input.LeanAxisCS);
	Input.UpDirectionCS = Rotation.RotateVector(Input.UpDirectionCS);
	for (FGGYGOBodyLeanLegPose& Leg : Input.Legs)
	{
		Leg.Upper = ChangeBasis(Leg.Upper, Rotation, Translation);
		Leg.Lower = ChangeBasis(Leg.Lower, Rotation, Translation);
		Leg.Foot = ChangeBasis(Leg.Foot, Rotation, Translation);
		Leg.KneeBendDirectionCS = Rotation.RotateVector(Leg.KneeBendDirectionCS);
	}
	FGGYGOBodyLeanResult Changed;
	if (!TestTrue(TEXT("任意component基准求解"), FGGYGOBodyLeanEvaluation::Evaluate(Input, Changed, Diagnostic)))
	{
		AddError(Diagnostic);
		return false;
	}
	TestTrue(TEXT("Body结果随基准等变，不假定Mesh固定Yaw或世界Z"),
		Changed.Body.Equals(ChangeBasis(Original.Body, Rotation, Translation), 1.e-4));
	for (int32 LegIndex = 0; LegIndex < 2; ++LegIndex)
	{
		TestTrue(TEXT("腿求解随基准等变"), Changed.Legs[LegIndex].Lower.Equals(
			ChangeBasis(Original.Legs[LegIndex].Lower, Rotation, Translation), 1.e-4));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOBodyLeanRejectContractTest,
	"GGYGO.Animation.BodyLean.RejectInvalid", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOBodyLeanRejectContractTest::RunTest(const FString& Parameters)
{
	FGGYGOBodyLeanResult Result;
	FString Diagnostic;
	FGGYGOBodyLeanInput Input = MakePose();
	Input.LeanDegrees = 80.0f;
	Input.LeanAxisCS = -FVector::RightVector;
	for (int32 Index = 0; Index < 2; ++Index)
	{
		const double Side = Index == 0 ? -100.0 : 100.0;
		Input.Legs[Index].Upper.SetTranslation(FVector(Side, 0.0, 90.0));
		Input.Legs[Index].Lower.SetTranslation(FVector(Side + 8.0, 0.0, 45.0));
		Input.Legs[Index].Foot.SetTranslation(FVector(Side, 0.0, 0.0));
	}
	TestFalse(TEXT("双腿可达球不相交时不以拉长腿或移脚成功"), FGGYGOBodyLeanEvaluation::Evaluate(Input, Result, Diagnostic));
	TestFalse(TEXT("拒绝包含可定位原因"), Diagnostic.IsEmpty());
	Input = MakePose();
	Input.Legs[0].KneeBendDirectionCS = FVector::ZeroVector;
	TestFalse(TEXT("缺配置不当成功中立姿态"), FGGYGOBodyLeanEvaluation::Evaluate(Input, Result, Diagnostic));
	Input = MakePose();
	Input.Legs[0].Lower = Input.Legs[0].Upper;
	TestFalse(TEXT("退化腿长明确拒绝"), FGGYGOBodyLeanEvaluation::Evaluate(Input, Result, Diagnostic));
	Input = MakePose();
	Input.Body.SetScale3D(FVector(1.0, 2.0, 1.0));
	TestFalse(TEXT("不支持的非均匀缩放明确拒绝"), FGGYGOBodyLeanEvaluation::Evaluate(Input, Result, Diagnostic));
	return !HasAnyErrors();
}
#endif
