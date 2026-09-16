/**
 * @file GGYGOAnimCurveSampler.cpp
 * @brief 动画曲线采样实现
 */

#include "Character/Components/GGYGOAnimCurveSampler.h"

#include "Animation/AnimInstance.h"

namespace GGYGOAnimCurveNames
{
	/** 从动画段起点累计的位移（厘米）。UE 轴序：X 前、Y 右。 */
	const FName PosX(TEXT("RootMotion_PosX"));
	const FName PosY(TEXT("RootMotion_PosY"));

	/** 从动画段起点累计的路程（厘米）。单调不减。 */
	const FName Distance(TEXT("RootMotion_Dist"));

	/** 该帧速度（cm/s）。 */
	const FName Speed(TEXT("RootMotion_Speed"));

	/** 从动画段起点累计的转角（度）。已解 ±180 折叠，连续。 */
	const FName Yaw(TEXT("RootMotion_Yaw"));

	/** 烘焙好的速度方向分量。UE 轴序：X 前、Y 右。 */
	const FName DirX(TEXT("RootMotion_DirX"));
	const FName DirY(TEXT("RootMotion_DirY"));

	/** 规范化有效时长（秒）。兼作"本动画烘焙过曲线"的存在性标记。 */
	const FName ClipLength(TEXT("Cfg_ClipLength"));

	/** 是否循环（0/1）。 */
	const FName LoopTime(TEXT("Cfg_LoopTime"));
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
	YawTotalDegrees = 0.0f;
	PositionDelta = FVector::ZeroVector;
	Velocity = FVector::ZeroVector;
	Direction = FVector::ZeroVector;
	DirectionAngle = 0.0f;
	ClipLength = 0.0f;
	bLoopClip = false;
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
	const float CurrentClipLength = SanitizeNonNegativeSample(AnimInstance.GetCurveValue(GGYGOAnimCurveNames::ClipLength));

	const FVector AuthoredDirection(
		SanitizeSample(AnimInstance.GetCurveValue(GGYGOAnimCurveNames::DirX)),
		SanitizeSample(AnimInstance.GetCurveValue(GGYGOAnimCurveNames::DirY)),
		0.0f);
	const bool bHasAuthoredDirection = !AuthoredDirection.IsNearlyZero(KINDA_SMALL_NUMBER);

	OutMotion.ClipLength = CurrentClipLength;
	OutMotion.bLoopClip = AnimInstance.GetCurveValue(GGYGOAnimCurveNames::LoopTime) > 0.5f;

	// `Cfg_ClipLength` 是烘焙管线给每个动画都写的常量曲线，恒大于 0，
	// 所以它能单独判定"这个动画烘焙过曲线"。
	//
	// 剩下三个信号是给它兜底的：动画可能来自旧的烘焙批次，只有 RootMotion_* 而没有 Cfg_*。
	// 只看 Speed 是不够的 —— 起步动画的第一帧速度是 0，但它确实有曲线，
	// 此时应当让角色停住而不是回退到固定速度。
	OutMotion.bHasCurveSource =
		CurrentClipLength > KINDA_SMALL_NUMBER
		|| CurrentSpeed > KINDA_SMALL_NUMBER
		|| bHasAuthoredDirection
		|| CurrentDistance > KINDA_SMALL_NUMBER;

	// 速度与累计转角不依赖差分，拿到就能用。
	OutMotion.Speed = CurrentSpeed;
	OutMotion.YawTotalDegrees = CurrentYaw;

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

	// 累计路程回退说明换了动画段，或循环动画绕回了循环点（两种情况下新的累计值都从 0 重新开始）。
	// 此时位移差分会得到一个指向反方向的大位移，必须丢弃并重建基线。
	// 用路程而不是位移判断，因为路程单调不减，而位移可以来回摆动 ——
	// 转身动画后半段的位移就是一路减小的，拿它判断会每帧都误判成换段。
	//
	// 代价是循环动画每个周期会丢掉一帧的位移增量与转角增量。这不影响速度
	// （速度直接来自曲线，不经差分），而走跑动画的方向由输入决定、不读这里的增量，
	// 所以目前无实际后果。
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

	// 累计转角曲线不变时差分自然为 0，不需要额外判断。
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
		// 曲线已是 UE 轴序（X 前、Y 右），所以 Atan2(Y, X) 得到的就是
		// "偏离段起点正前方多少度，右为正"。
		OutMotion.DirectionAngle = FMath::RadiansToDegrees(
			FMath::Atan2(EffectiveDirection.Y, EffectiveDirection.X));
	}

	UpdateBaseline();
}
