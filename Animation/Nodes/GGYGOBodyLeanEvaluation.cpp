/**
 * @file GGYGOBodyLeanEvaluation.cpp
 * @brief 同帧整身姿态和原生双腿 IK 的纯计算
 */
#include "Animation/Nodes/GGYGOBodyLeanEvaluation.h"

#include "TwoBoneIK.h"

namespace
{
	bool IsPoseTransformValid(const FTransform& Transform)
	{
		const FVector Scale = Transform.GetScale3D();
		return !Transform.ContainsNaN() && Transform.GetRotation().IsNormalized()
			&& Scale.X > 0.0 && FMath::IsNearlyEqual(Scale.X, Scale.Y)
			&& FMath::IsNearlyEqual(Scale.X, Scale.Z);
	}

	bool IsUnitDirection(const FVector& Direction)
	{
		return !Direction.ContainsNaN() && FMath::IsNearlyEqual(Direction.SizeSquared(), 1.0, 1.e-4);
	}

	FTransform RotatePose(const FTransform& Source, const FQuat& Rotation, const FVector& Pivot)
	{
		return FTransform(Rotation * Source.GetRotation(),
			Pivot + Rotation.RotateVector(Source.GetTranslation() - Pivot), Source.GetScale3D());
	}

	/** 骨盆公共平移须位于两腿可达球的交集。求最靠近零的解，不靠拉长腿或移脚。 */
	bool FindPelvisAdjustment(const FVector (&Centers)[2], const double (&Radii)[2],
		const FVector& Up, const FVector& LeanAxis, FVector& Adjustment)
	{
		const auto Inside = [&Centers, &Radii](const FVector& Point, int32 Index)
		{
			return (Point - Centers[Index]).SizeSquared() <= FMath::Square(Radii[Index]) + 1.e-8;
		};
		if (Inside(FVector::ZeroVector, 0) && Inside(FVector::ZeroVector, 1))
		{
			Adjustment = FVector::ZeroVector;
			return true;
		}
		for (int32 Index = 0; Index < 2; ++Index)
		{
			const double Distance = Centers[Index].Size();
			if (Distance > Radii[Index])
			{
				const FVector Nearest = Centers[Index] * (1.0 - Radii[Index] / Distance);
				if (Inside(Nearest, 1 - Index))
				{
					Adjustment = Nearest;
					return true;
				}
			}
		}
		const FVector Difference = Centers[1] - Centers[0];
		const double Distance = Difference.Size();
		if (Distance <= DOUBLE_SMALL_NUMBER || Distance > Radii[0] + Radii[1]
			|| Distance < FMath::Abs(Radii[0] - Radii[1]))
		{
			return false;
		}
		const FVector Normal = Difference / Distance;
		const double Along = (FMath::Square(Radii[0]) - FMath::Square(Radii[1]) + FMath::Square(Distance)) / (2.0 * Distance);
		const FVector CircleCenter = Centers[0] + Normal * Along;
		const double Radius = FMath::Sqrt(FMath::Max(0.0, FMath::Square(Radii[0]) - FMath::Square(Along)));
		FVector TowardOrigin = -CircleCenter + Normal * FVector::DotProduct(CircleCenter, Normal);
		if (TowardOrigin.IsNearlyZero())
		{
			// 轴线退化有多个等距解，用明确输入的姿态基准选择，保持component空间等变。
			TowardOrigin = Up - Normal * FVector::DotProduct(Up, Normal);
			if (TowardOrigin.IsNearlyZero())
			{
				TowardOrigin = LeanAxis - Normal * FVector::DotProduct(LeanAxis, Normal);
			}
		}
		Adjustment = CircleCenter + TowardOrigin.GetSafeNormal() * Radius;
		return Inside(Adjustment, 0) && Inside(Adjustment, 1);
	}
}

bool FGGYGOBodyLeanEvaluation::Evaluate(
	const FGGYGOBodyLeanInput& Input, FGGYGOBodyLeanResult& Result, FString& Diagnostic)
{
	Result = {};
	Diagnostic.Reset();
	if (!FMath::IsFinite(Input.LeanDegrees) || FMath::Abs(Input.LeanDegrees) >= 90.0f
		|| !IsUnitDirection(Input.LeanAxisCS) || !IsUnitDirection(Input.UpDirectionCS)
		|| !FMath::IsNearlyZero(FVector::DotProduct(Input.LeanAxisCS, Input.UpDirectionCS), 1.e-4)
		|| !IsPoseTransformValid(Input.Body))
	{
		Diagnostic = TEXT("倾身角必须有限且绝对值小于90度，轴须为正交单位向量，Body姿态须有限且为正均匀缩放");
		return false;
	}
	double UpperLengths[2];
	double LowerLengths[2];
	for (int32 LegIndex = 0; LegIndex < 2; ++LegIndex)
	{
		const FGGYGOBodyLeanLegPose& Leg = Input.Legs[LegIndex];
		if (!IsPoseTransformValid(Leg.Upper) || !IsPoseTransformValid(Leg.Lower)
			|| !IsPoseTransformValid(Leg.Foot) || !IsUnitDirection(Leg.KneeBendDirectionCS))
		{
			Diagnostic = FString::Printf(TEXT("Leg[%d]输入姿态或明确膝弯方向无效"), LegIndex);
			return false;
		}
		UpperLengths[LegIndex] = FVector::Distance(Leg.Upper.GetTranslation(), Leg.Lower.GetTranslation());
		LowerLengths[LegIndex] = FVector::Distance(Leg.Lower.GetTranslation(), Leg.Foot.GetTranslation());
		if (UpperLengths[LegIndex] <= DOUBLE_KINDA_SMALL_NUMBER || LowerLengths[LegIndex] <= DOUBLE_KINDA_SMALL_NUMBER)
		{
			Diagnostic = FString::Printf(TEXT("Leg[%d]输入骨链长度退化"), LegIndex);
			return false;
		}
	}
	if (Input.LeanDegrees == 0.0f)
	{
		Result.Body = Input.Body;
		Result.Legs[0] = Input.Legs[0];
		Result.Legs[1] = Input.Legs[1];
		return true;
	}

	const FVector Pivot = (Input.Legs[0].Foot.GetTranslation() + Input.Legs[1].Foot.GetTranslation()) * 0.5;
	const FQuat Rotation(Input.LeanAxisCS, FMath::DegreesToRadians(Input.LeanDegrees));
	FGGYGOBodyLeanResult Candidate;
	Candidate.Body = RotatePose(Input.Body, Rotation, Pivot);
	FVector ReachCenters[2];
	double ReachRadii[2];
	for (int32 LegIndex = 0; LegIndex < 2; ++LegIndex)
	{
		const FGGYGOBodyLeanLegPose& Source = Input.Legs[LegIndex];
		FGGYGOBodyLeanLegPose& Leg = Candidate.Legs[LegIndex];
		Leg.Upper = RotatePose(Source.Upper, Rotation, Pivot);
		Leg.Lower = RotatePose(Source.Lower, Rotation, Pivot);
		Leg.Foot = RotatePose(Source.Foot, Rotation, Pivot);
		Leg.KneeBendDirectionCS = Rotation.RotateVector(Source.KneeBendDirectionCS);
		ReachCenters[LegIndex] = Source.Foot.GetTranslation() - Leg.Upper.GetTranslation();
		ReachRadii[LegIndex] = UpperLengths[LegIndex] + LowerLengths[LegIndex];
	}
	if (!FindPelvisAdjustment(ReachCenters, ReachRadii, Input.UpDirectionCS, Input.LeanAxisCS, Candidate.PelvisAdjustmentCS))
	{
		Diagnostic = TEXT("双腿不存在共同可达的骨盆位置，不能拉长骨骼或移动原脚目标掩盖失败");
		return false;
	}
	Candidate.Body.AddToTranslation(Candidate.PelvisAdjustmentCS);
	for (int32 LegIndex = 0; LegIndex < 2; ++LegIndex)
	{
		const FGGYGOBodyLeanLegPose& Source = Input.Legs[LegIndex];
		FGGYGOBodyLeanLegPose& Leg = Candidate.Legs[LegIndex];
		Leg.Upper.AddToTranslation(Candidate.PelvisAdjustmentCS);
		Leg.Lower.AddToTranslation(Candidate.PelvisAdjustmentCS);
		Leg.Foot.AddToTranslation(Candidate.PelvisAdjustmentCS);
		const FVector Target = Source.Foot.GetTranslation();
		const FVector Desired = Target - Leg.Upper.GetTranslation();
		const double MinimumLength = FMath::Abs(UpperLengths[LegIndex] - LowerLengths[LegIndex]);
		if (Desired.Size() < FMath::Max(MinimumLength, DOUBLE_KINDA_SMALL_NUMBER))
		{
			Diagnostic = FString::Printf(TEXT("Leg[%d]脚目标进入两骨链不可达的内侧范围"), LegIndex);
			return false;
		}
		const FVector DesiredDirection = Desired.GetSafeNormal();
		const FVector JointDelta = Leg.Lower.GetTranslation() - Leg.Upper.GetTranslation();
		FVector Bend = JointDelta - DesiredDirection * FVector::DotProduct(JointDelta, DesiredDirection);
		if (Bend.IsNearlyZero())
		{
			// 直腿是正常源姿态；其膝弯方向必须来自显式骨局部配置，不让原生 IK 任取世界轴。
			Bend = Leg.KneeBendDirectionCS - DesiredDirection * FVector::DotProduct(Leg.KneeBendDirectionCS, DesiredDirection);
		}
		if (Bend.IsNearlyZero())
		{
			Diagnostic = FString::Printf(TEXT("Leg[%d]明确膝弯方向与目标共线"), LegIndex);
			return false;
		}
		const FVector JointTarget = Leg.Upper.GetTranslation() + Bend.GetSafeNormal()
			* (UpperLengths[LegIndex] + LowerLengths[LegIndex]);
		AnimationCore::SolveTwoBoneIK(Leg.Upper, Leg.Lower, Leg.Foot, JointTarget, Target,
			UpperLengths[LegIndex], LowerLengths[LegIndex], false, 1.0, 1.0);
		Leg.Foot.SetRotation(Source.Foot.GetRotation());
		const double Tolerance = (UpperLengths[LegIndex] + LowerLengths[LegIndex]) * 1.e-5;
		if (!IsPoseTransformValid(Leg.Upper) || !IsPoseTransformValid(Leg.Lower) || !IsPoseTransformValid(Leg.Foot)
			|| !Leg.Foot.GetTranslation().Equals(Target, Tolerance))
		{
			Diagnostic = FString::Printf(TEXT("Leg[%d]原生IK没有保持有限姿态与本次输入脚目标"), LegIndex);
			return false;
		}
	}
	Result = Candidate;
	return true;
}
