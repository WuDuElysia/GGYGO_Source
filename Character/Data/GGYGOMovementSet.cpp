/**
 * @file GGYGOMovementSet.cpp
 * @brief 移动参数资产实现
 */
#include "Character/Data/GGYGOMovementSet.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOMovementSet)

namespace GGYGOMovementSetDefaults
{
	/** 配置值非法时的兜底走跑阈值。与字段默认值一致。 */
	constexpr float WalkToRunHoldSeconds = 5.0f;

	/** 走跑阈值上限。超过一分钟的"持续走"在任何玩法下都是配置错误。 */
	constexpr float MaxWalkToRunHoldSeconds = 60.0f;
}

UGGYGOMovementSet::UGGYGOMovementSet(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// 字段默认值已在头文件声明处给出，构造函数不重复赋值，
	// 避免两处默认值不一致这种典型的维护陷阱。
}

float UGGYGOMovementSet::GetSpeedForGait(EGGYGOGait Gait) const
{
	switch (Gait)
	{
	case EGGYGOGait::Walk:
		return FMath::Max(WalkSpeed, 0.0f);

	case EGGYGOGait::Run:
		return FMath::Max(RunSpeed, 0.0f);

	case EGGYGOGait::None:
	default:
		return 0.0f;
	}
}

float UGGYGOMovementSet::GetSanitizedWalkToRunHoldSeconds() const
{
	// 非有限值（NaN / Inf）无法参与比较，会让计时器永远达不到或立刻达到阈值。
	if (!FMath::IsFinite(WalkToRunHoldSeconds) || WalkToRunHoldSeconds <= 0.0f)
	{
		return GGYGOMovementSetDefaults::WalkToRunHoldSeconds;
	}

	return FMath::Min(WalkToRunHoldSeconds, GGYGOMovementSetDefaults::MaxWalkToRunHoldSeconds);
}
