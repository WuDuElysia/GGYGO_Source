/**
 * @file GGYGOAnimCurveSampler.cpp
 * @brief 动画曲线采样实现
 */
#include "Character/Components/GGYGOAnimCurveSampler.h"

#include "Animation/AnimInstance.h"

namespace GGYGOAnimCurveNames
{
	/** 从动画段起点累计的位移。X 左右、Y 前后。 */
	const FName PosX(TEXT("RM_PosX"));
	const FName PosY(TEXT("RM_PosY"));

	/** 从动画段起点累计的路程。单调递增。 */
	const FName Distance(TEXT("RM_Dist"));

	/** 该帧速度（cm/s）。 */
	const FName Speed(TEXT("RM_Speed"));

	/** 从动画段起点累计的转角（度）。 */
	const FName Yaw(TEXT("RM_Yaw"));

	/** 烘焙好的速度方向分量。 */
	const FName VelocityDirX(TEXT("RM_VelocityDirX"));
	const FName VelocityDirY(TEXT("RM_VelocityDirY"));
}

namespace
{
	/** 非有限值一律当 0。曲线缺失时 GetCurveValue 返回 0，但坏数据可能是 NaN。 */
	float SanitizeSample(const float Value)
	{
		return FMath::IsFinite(Value) ? Value : 0.0f;
	}

	/** 速度与路程不允许为负，负值同样按 0 处理。 */
	float SanitizeNonNegativeSample(const float Value)
	{
		return FMath::IsFinite(Value) ? FMath::Max(Value, 0.0f) : 0.0f;
	}
}

void FGGYGOAnimCurveMotion::Reset()
{
	Speed = 0.0f;
	YawDeltaDegrees = 0.0f;
	PositionDelta = FVector::ZeroVector;
	Velocity = FVector::ZeroVector;
	Direction = FVector::ZeroVector;
	DirectionAngle = 0.0f;
	bHasAuthoredDirection = false;
	bHasPositionDelta = false;
	bHasCurveSource = false;
}

bool FGGYGOAnimCurveMotion::HasUsableSpeed() const
{
	return bHasCurveSource && Speed > KINDA_SMALL_NUMBER;
}

void FGGYGOAnimCurveSampler::ResetBaseline()
{
	PreviousPosX = 0.0f;
	PreviousPosY = 0.0f;
	PreviousDistance = 0.0f;
	PreviousYaw = 0.0f;
	bHasBaseline = false;
}

void FGGYGOAnimCurveSampler::Sample(const UAnimInstance& AnimInstance, float DeltaTime, FGGYGOAnimCurveMotion& OutMotion)
{
	OutMotion.Reset();

	const float CurrentPosX = SanitizeSample(AnimInstance.GetCurveValue(GGYGOAnimCurveNames::PosX));
	const float CurrentPosY = SanitizeSample(AnimInstance.GetCurveValue(GGYGOAnimCurveNames::PosY));
	const float CurrentDistance = SanitizeNonNegativeSample(AnimInstance.GetCurveValue(GGYGOAnimCurveNames::Distance));
	const float CurrentSpeed = SanitizeNonNegativeSample(AnimInstance.GetCurveValue(GGYGOAnimCurveNames::Speed));
	const float CurrentYaw = SanitizeSample(AnimInstance.GetCurveValue(GGYGOAnimCurveNames::Yaw));

	const FVector AuthoredDirection(
		SanitizeSample(AnimInstance.GetCurveValue(GGYGOAnimCurveNames::VelocityDirX)),
		SanitizeSample(AnimInstance.GetCurveValue(GGYGOAnimCurveNames::VelocityDirY)),
		0.0f);
	const bool bHasAuthoredDirection = !AuthoredDirection.IsNearlyZero(KINDA_SMALL_NUMBER);

	// 三个信号任一存在就说明这个动画带曲线数据。
	// 只看 Speed 是不够的：起步动画的第一帧速度是 0，但它确实有曲线，
	// 此时应当让角色停住而不是回退到固定速度。
	OutMotion.bHasCurveSource =
		CurrentSpeed > KINDA_SMALL_NUMBER
		|| bHasAuthoredDirection
		|| CurrentDistance > KINDA_SMALL_NUMBER;

	// 速度不依赖差分，拿到就能用。
	OutMotion.Speed = CurrentSpeed;

	const auto UpdateBaseline = [&]()
	{
		PreviousPosX = CurrentPosX;
		PreviousPosY = CurrentPosY;
		PreviousDistance = CurrentDistance;
		PreviousYaw = CurrentYaw;
		bHasBaseline = true;
	};

	// 退化帧只更新基线。继续算会除零或让位移量偏大。
	if (!FMath::IsFinite(DeltaTime) || DeltaTime <= KINDA_SMALL_NUMBER)
	{
		UpdateBaseline();
		return;
	}

	// 首帧没有可比对的上一帧，把动画段起点的累计值当成本帧增量会让角色瞬移。
	if (!bHasBaseline)
	{
		UpdateBaseline();
		return;
	}

	// 累计路程回退说明换了动画段（新段的累计值从 0 重新开始）。
	// 此时位移差分会得到一个指向反方向的大位移，必须丢弃并重建基线。
	// 用 RM_Dist 而不是 RM_PosX/Y 判断，因为路程单调递增，而位移可以来回摆动。
	if (CurrentDistance < PreviousDistance - KINDA_SMALL_NUMBER)
	{
		UpdateBaseline();
		return;
	}

	const FVector PositionDelta(
		CurrentPosX - PreviousPosX,
		CurrentPosY - PreviousPosY,
		0.0f);

	if (!FMath::IsFinite(PositionDelta.X) || !FMath::IsFinite(PositionDelta.Y))
	{
		UpdateBaseline();
		return;
	}

	// RM_Yaw 是累计角度，曲线不变时差分自然为 0，不需要额外判断。
	OutMotion.YawDeltaDegrees = SanitizeSample(CurrentYaw - PreviousYaw);

	if (!PositionDelta.IsNearlyZero())
	{
		OutMotion.PositionDelta = PositionDelta;
		OutMotion.Velocity = PositionDelta / DeltaTime;
		OutMotion.bHasPositionDelta = true;
	}

	// 烘焙方向优先于位移差分：低速时位移差分很小，归一化后方向抖动明显。
	const FVector EffectiveDirection = bHasAuthoredDirection
		? AuthoredDirection.GetSafeNormal2D()
		: PositionDelta.GetSafeNormal2D();

	if (!EffectiveDirection.IsNearlyZero())
	{
		OutMotion.Direction = EffectiveDirection;
		OutMotion.bHasAuthoredDirection = bHasAuthoredDirection;
		OutMotion.DirectionAngle = FMath::RadiansToDegrees(
			FMath::Atan2(EffectiveDirection.Y, EffectiveDirection.X));
	}

	UpdateBaseline();
}
