/**
 * @file GGYGOBodyLeanEvaluation.h
 * @brief 输入姿态的整身倾身及双腿可达性求解，不持有跨帧姿态或玩法状态
 */
#pragma once

#include "CoreMinimal.h"

/** 同一次 Evaluate 的 component-space 骨姿态。 */
struct FGGYGOBodyLeanLegPose
{
	FTransform Upper;
	FTransform Lower;
	FTransform Foot;
	/** 直腿退化时使用角色明确配置的膝弯方向，已换算到 component space。 */
	FVector KneeBendDirectionCS = FVector::ZeroVector;
};

struct FGGYGOBodyLeanInput
{
	FTransform Body;
	FGGYGOBodyLeanLegPose Legs[2];
	FVector LeanAxisCS = FVector::ZeroVector;
	FVector UpDirectionCS = FVector::ZeroVector;
	float LeanDegrees = 0.0f;
};

struct FGGYGOBodyLeanResult
{
	FTransform Body;
	FGGYGOBodyLeanLegPose Legs[2];
	FVector PelvisAdjustmentCS = FVector::ZeroVector;
};

/** 纯姿态计算；脚目标来自本次输入姿态，不包含接触猜测、世界锁脚或角色位移。 */
class GGYGO_API FGGYGOBodyLeanEvaluation
{
public:
	/**
	 * 围绕输入双脚中心倾身；必要时最小调整骨盆以保持两腿原长度，并复用原生两骨 IK。
	 * @return 骨链几何或配置不合法时返回 false，结果不可消费，Diagnostic 给出原因。
	 */
	static bool Evaluate(const FGGYGOBodyLeanInput& Input, FGGYGOBodyLeanResult& Result, FString& Diagnostic);
};
