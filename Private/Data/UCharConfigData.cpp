/**
 * @file UCharConfigData.cpp
 * @brief 角色配置数据资产实现 — 查询方法
 */
#include "Data/UCharConfigData.h"

UCharConfigData::UCharConfigData()
{
}

float UCharConfigData::GetBlendInTime(ECharacterStateType TargetState) const
{
	const FStateTransitionBlend* Found = PerStateBlendOverrides.Find(TargetState);
	if (Found)
	{
		return Found->BlendInTime;
	}
	return DefaultBlendDuration;
}

float UCharConfigData::GetBlendOutTime(ECharacterStateType SourceState) const
{
	const FStateTransitionBlend* Found = PerStateBlendOverrides.Find(SourceState);
	if (Found)
	{
		return Found->BlendOutTime;
	}
	return DefaultBlendDuration;
}

float UCharConfigData::GetPlayRateForState(ECharacterStateType State) const
{
	switch (State)
	{
	case ECharacterStateType::Idle:
	case ECharacterStateType::Moving:
	case ECharacterStateType::InAir:
	case ECharacterStateType::Stunned:
	case ECharacterStateType::Interacting:
		return LoopAnimPlayRate;

	default:
		return NonLoopAnimPlayRate;
	}
}
