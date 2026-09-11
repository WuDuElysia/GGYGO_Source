/**
 * @file GGYGOLocalPlayer.cpp
 * @brief 本地玩家实现
 */
#include "Player/GGYGOLocalPlayer.h"

#include "Teams/GGYGOSquadPresets.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOLocalPlayer)

UGGYGOSquadPresets* UGGYGOLocalPlayer::GetSquadPresets() const
{
	if (!SquadPresets)
	{
		SquadPresets = UGGYGOSquadPresets::LoadOrCreateForLocalPlayer(this);
	}

	return SquadPresets;
}
