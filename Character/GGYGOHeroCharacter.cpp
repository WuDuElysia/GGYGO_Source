/**
 * @file GGYGOHeroCharacter.cpp
 * @brief 玩家操控角色实现
 */
#include "Character/GGYGOHeroCharacter.h"

#include "Character/Components/GGYGOHeroComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOHeroCharacter)

AGGYGOHeroCharacter::AGGYGOHeroCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	HeroComponent = CreateDefaultSubobject<UGGYGOHeroComponent>(TEXT("HeroComponent"));
}
