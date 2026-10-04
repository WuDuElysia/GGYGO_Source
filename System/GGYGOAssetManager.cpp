/**
 * @file GGYGOAssetManager.cpp
 * @brief 项目 AssetManager 实现
 */
#include "System/GGYGOAssetManager.h"

#include "Engine/Engine.h"
#include "GameplayEffect.h"
#include "System/GGYGOGameData.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOAssetManager)

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOSystem, Log, All);

namespace
{
	// 只由启动预载调用；每项独立记录结果，不写回GameData配置。
	TSubclassOf<UGameplayEffect> PreloadSharedGameplayEffect(
		const TSoftClassPtr<UGameplayEffect>& EffectPath, const TCHAR* FieldName,
		EGGYGOSharedAssetLoadState& OutState)
	{
		if (EffectPath.IsNull())
		{
			OutState = EGGYGOSharedAssetLoadState::NotConfigured;
			UE_LOG(LogGGYGOSystem, Warning, TEXT("共享GE [%s] 未配置。"), FieldName);
			return nullptr;
		}

		UClass* EffectClass = EffectPath.LoadSynchronous();
		// TSoftClassPtr的加载接口已校验UGameplayEffect继承关系。
		if (!EffectClass)
		{
			OutState = EGGYGOSharedAssetLoadState::LoadFailed;
			UE_LOG(LogGGYGOSystem, Warning, TEXT("共享GE [%s] 加载失败或类型不符，路径 [%s]。"),
				FieldName, *EffectPath.ToString());
			return nullptr;
		}

		OutState = EGGYGOSharedAssetLoadState::Ready;
		UE_LOG(LogGGYGOSystem, Log, TEXT("共享GE [%s] 预载就绪，路径 [%s]。"), FieldName, *EffectPath.ToString());
		return EffectClass;
	}
}

UGGYGOAssetManager::UGGYGOAssetManager()
{
}

UGGYGOAssetManager* UGGYGOAssetManager::TryGet()
{
	if (!IsInGameThread())
	{
		UE_LOG(LogGGYGOSystem, Error, TEXT("TryGet: 仅允许游戏线程读取AssetManager，返回空。"));
		return nullptr;
	}

	if (!GEngine || !GEngine->AssetManager)
	{
		UE_LOG(LogGGYGOSystem, Error,
			TEXT("TryGet: 引擎或 AssetManager 尚未就绪，返回空；调用方必须处理共享资产不可用。"));
		return nullptr;
	}

	if (UGGYGOAssetManager* Manager = Cast<UGGYGOAssetManager>(GEngine->AssetManager))
	{
		return Manager;
	}

	UE_LOG(LogGGYGOSystem, Error,
		TEXT("TryGet: 引擎实际持有的 AssetManager 类型 [%s] 不是 GGYGOAssetManager，返回空；调用方必须处理共享资产不可用。请检查 DefaultEngine.ini 的 [/Script/Engine.Engine] AssetManagerClassName。"),
		*GetNameSafe(GEngine->AssetManager->GetClass()));
	return nullptr;
}

void UGGYGOAssetManager::StartInitialLoading()
{
	if (!IsInGameThread())
	{
		UE_LOG(LogGGYGOSystem, Error, TEXT("StartInitialLoading: 共享资产预载仅允许游戏线程执行。"));
		return;
	}
	if (bSharedAssetsPreloadStarted)
	{
		UE_LOG(LogGGYGOSystem, Warning, TEXT("StartInitialLoading: 已开始过共享资产预载，不重入或重试。"));
		return;
	}
	bSharedAssetsPreloadStarted = true;

	// 父类先扫描并建立索引。同步加载仅发生在此启动阶段；Cook规则由配置/资产管理规则决定。
	Super::StartInitialLoading();

	if (GGYGOGameDataPath.IsNull())
	{
		GameDataLoadState = EGGYGOSharedAssetLoadState::NotConfigured;
		UE_LOG(LogGGYGOSystem, Warning,
			TEXT("GameData未配置；请在DefaultGame.ini的[/Script/GGYGO.GGYGOAssetManager]配置GGYGOGameDataPath。"));
	}
	else
	{
		LoadedGameData = GGYGOGameDataPath.LoadSynchronous();
		GameDataLoadState = LoadedGameData ? EGGYGOSharedAssetLoadState::Ready : EGGYGOSharedAssetLoadState::LoadFailed;
		if (!LoadedGameData)
		{
			UE_LOG(LogGGYGOSystem, Warning, TEXT("GameData加载失败，路径 [%s]。"), *GGYGOGameDataPath.ToString());
		}
		else
		{
			UE_LOG(LogGGYGOSystem, Log, TEXT("GameData预载就绪，路径 [%s]。"), *GGYGOGameDataPath.ToString());
		}
	}

	if (LoadedGameData)
	{
		SharedDamageGameplayEffect = PreloadSharedGameplayEffect(LoadedGameData->DamageGameplayEffect_SetByCaller,
			TEXT("DamageGameplayEffect_SetByCaller"), SharedDamageLoadState);
		SharedHealGameplayEffect = PreloadSharedGameplayEffect(LoadedGameData->HealGameplayEffect_SetByCaller,
			TEXT("HealGameplayEffect_SetByCaller"), SharedHealLoadState);
		SharedSelfDestructGameplayEffect = PreloadSharedGameplayEffect(LoadedGameData->SelfDestructGameplayEffect,
			TEXT("SelfDestructGameplayEffect"), SharedSelfDestructLoadState);
	}
	else
	{
		SharedDamageLoadState = EGGYGOSharedAssetLoadState::DependencyUnavailable;
		SharedHealLoadState = EGGYGOSharedAssetLoadState::DependencyUnavailable;
		SharedSelfDestructLoadState = EGGYGOSharedAssetLoadState::DependencyUnavailable;
		UE_LOG(LogGGYGOSystem, Warning, TEXT("三项共享GE依赖的GameData不可用，未尝试加载。"));
	}

	// 发布整次尝试结果（含失败），避免加载期间的回调读到部分快照。
	bSharedAssetsPreloadCompleted = true;
}

bool UGGYGOAssetManager::CanReadSharedAssets() const
{
	if (!IsInGameThread())
	{
		UE_LOG(LogGGYGOSystem, Error, TEXT("共享资产getter仅允许游戏线程读取，返回空/NotReady/false。"));
		return false;
	}
	return bSharedAssetsPreloadCompleted;
}

const UGGYGOGameData* UGGYGOAssetManager::GetGameData() const
{
	return CanReadSharedAssets() ? LoadedGameData.Get() : nullptr;
}

TSubclassOf<UGameplayEffect> UGGYGOAssetManager::GetSharedDamageGameplayEffect() const
{
	return CanReadSharedAssets() ? SharedDamageGameplayEffect : TSubclassOf<UGameplayEffect>();
}

TSubclassOf<UGameplayEffect> UGGYGOAssetManager::GetSharedHealGameplayEffect() const
{
	return CanReadSharedAssets() ? SharedHealGameplayEffect : TSubclassOf<UGameplayEffect>();
}

TSubclassOf<UGameplayEffect> UGGYGOAssetManager::GetSharedSelfDestructGameplayEffect() const
{
	return CanReadSharedAssets() ? SharedSelfDestructGameplayEffect : TSubclassOf<UGameplayEffect>();
}

EGGYGOSharedAssetLoadState UGGYGOAssetManager::GetGameDataLoadState() const
{
	return CanReadSharedAssets() ? GameDataLoadState : EGGYGOSharedAssetLoadState::NotReady;
}

EGGYGOSharedAssetLoadState UGGYGOAssetManager::GetSharedDamageLoadState() const
{
	return CanReadSharedAssets() ? SharedDamageLoadState : EGGYGOSharedAssetLoadState::NotReady;
}

EGGYGOSharedAssetLoadState UGGYGOAssetManager::GetSharedHealLoadState() const
{
	return CanReadSharedAssets() ? SharedHealLoadState : EGGYGOSharedAssetLoadState::NotReady;
}

EGGYGOSharedAssetLoadState UGGYGOAssetManager::GetSharedSelfDestructLoadState() const
{
	return CanReadSharedAssets() ? SharedSelfDestructLoadState : EGGYGOSharedAssetLoadState::NotReady;
}

bool UGGYGOAssetManager::HasCompletedSharedAssetPreload() const
{
	return CanReadSharedAssets();
}
