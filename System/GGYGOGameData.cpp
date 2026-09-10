/**
 * @file GGYGOGameData.cpp
 * @brief 项目级共享资产实现
 */
#include "System/GGYGOGameData.h"

#include "System/GGYGOAssetManager.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOGameData)

UGGYGOGameData::UGGYGOGameData()
{
}

const UGGYGOGameData* UGGYGOGameData::Get()
{
	return UGGYGOAssetManager::Get().GetGameData();
}
