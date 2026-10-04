/**
 * @file GGYGOGameData.cpp
 * @brief 项目级共享资产实现
 */
#include "System/GGYGOGameData.h"

#include "System/GGYGOAssetManager.h"
#include "GameplayEffect.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOGameData)

UGGYGOGameData::UGGYGOGameData()
{
}

const UGGYGOGameData* UGGYGOGameData::Get()
{
	if (UGGYGOAssetManager* Manager = UGGYGOAssetManager::TryGet())
	{
		return Manager->GetGameData();
	}
	return nullptr;
}

TSubclassOf<UGameplayEffect> UGGYGOGameData::GetSharedDamageGameplayEffect()
{
	if (UGGYGOAssetManager* Manager = UGGYGOAssetManager::TryGet())
	{
		return Manager->GetSharedDamageGameplayEffect();
	}
	return nullptr;
}

TSubclassOf<UGameplayEffect> UGGYGOGameData::GetSharedHealGameplayEffect()
{
	if (UGGYGOAssetManager* Manager = UGGYGOAssetManager::TryGet())
	{
		return Manager->GetSharedHealGameplayEffect();
	}
	return nullptr;
}

TSubclassOf<UGameplayEffect> UGGYGOGameData::GetSharedSelfDestructGameplayEffect()
{
	if (UGGYGOAssetManager* Manager = UGGYGOAssetManager::TryGet())
	{
		return Manager->GetSharedSelfDestructGameplayEffect();
	}
	return nullptr;
}

TSubclassOf<UGameplayEffect> UGGYGOGameData::ResolveDamageGameplayEffect(
	TSubclassOf<UGameplayEffect> EffectOverride, bool bUseSharedWhenUnset)
{
	if (EffectOverride)
	{
		return EffectOverride;
	}
	return bUseSharedWhenUnset ? GetSharedDamageGameplayEffect() : TSubclassOf<UGameplayEffect>();
}

TSubclassOf<UGameplayEffect> UGGYGOGameData::ResolveHealGameplayEffect(
	TSubclassOf<UGameplayEffect> EffectOverride, bool bUseSharedWhenUnset)
{
	if (EffectOverride)
	{
		return EffectOverride;
	}
	return bUseSharedWhenUnset ? GetSharedHealGameplayEffect() : TSubclassOf<UGameplayEffect>();
}
