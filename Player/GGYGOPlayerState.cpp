/**
 * @file GGYGOPlayerState.cpp
 * @brief 玩家状态实现
 */
#include "Player/GGYGOPlayerState.h"

#include "Teams/GGYGOSquadComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOPlayerState)

AGGYGOPlayerState::AGGYGOPlayerState(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SquadComponent = CreateDefaultSubobject<UGGYGOSquadComponent>(TEXT("SquadComponent"));

	// PlayerState 默认的复制频率偏低（它承载的多是分数之类的慢变量）。
	// 队伍的出战序号需要及时同步，否则换人后客户端会有可感知的延迟。
	SetNetUpdateFrequency(100.0f);
}
