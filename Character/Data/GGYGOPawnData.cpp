/**
 * @file GGYGOPawnData.cpp
 * @brief 可操控单位静态配置的实现
 */
#include "Character/Data/GGYGOPawnData.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOPawnData)

UGGYGOPawnData::UGGYGOPawnData(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// 全部字段留空。空 PawnData 是合法的：InitState 只要求 PawnData **存在**，
	// 不要求内容非空，这样"还没配能力的占位角色"也能走完初始化链条。
	PawnClass = nullptr;
	AbilityGroupConfig = nullptr;
	TagRelationshipMapping = nullptr;
	MovementSet = nullptr;
}
