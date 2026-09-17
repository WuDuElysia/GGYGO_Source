/** @file GGYGOBossCharacter.h @brief 不持有 ASC 的 Boss Avatar */
#pragma once

#include "Character/GGYGOCharacterBase.h"

#include "GGYGOBossCharacter.generated.h"

/** Boss 的通用 Avatar；战斗状态由 AGGYGOBossState 持有。 */
UCLASS(Blueprintable)
class GGYGO_API AGGYGOBossCharacter : public AGGYGOCharacterBase
{
	GENERATED_BODY()

public:
	AGGYGOBossCharacter(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};
