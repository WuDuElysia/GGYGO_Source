/**
 * @file GGYGOAssetManager.cpp
 * @brief 项目 AssetManager 实现
 */
#include "System/GGYGOAssetManager.h"

#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "Misc/ScopeLock.h"
#include "System/GGYGOGameData.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOAssetManager)

UGGYGOAssetManager::UGGYGOAssetManager()
{
}

UGGYGOAssetManager& UGGYGOAssetManager::Get()
{
	if (GEngine)
	{
		if (UGGYGOAssetManager* Singleton = Cast<UGGYGOAssetManager>(GEngine->AssetManager))
		{
			return *Singleton;
		}
	}

	// 引擎用的不是本类 —— `DefaultEngine.ini` 里的 `AssetManagerClassName`
	// 没配或拼错了。
	//
	// 用 Error 而不是 Fatal：中止运行会让编辑器直接起不来，
	// 而这是一个在编辑器里就能改好的配置问题，把工具链堵死代价太大。
	// 兜底实例让引擎能继续跑，依赖共享资产的功能会各自报错。
	UE_LOG(LogGGYGOAbilitySystem, Error,
		TEXT("AssetManagerClassName 未设为 GGYGOAssetManager，共享资产不可用。请检查 DefaultEngine.ini 的 [/Script/Engine.Engine] 段。"));

	// 兜底实例要活到进程结束，所以 AddToRoot 防止被 GC。
	static UGGYGOAssetManager* FallbackManager = nullptr;
	if (!FallbackManager)
	{
		FallbackManager = NewObject<UGGYGOAssetManager>();
		FallbackManager->AddToRoot();
	}

	return *FallbackManager;
}

void UGGYGOAssetManager::StartInitialLoading()
{
	// 必须先调父类：它会扫描 PrimaryAssetType 并建立资产索引，
	// 之后才能按类型查找资产。
	Super::StartInitialLoading();

	// 启动时预加载，这样运行时的同步取用总是命中缓存。
	// 失败不阻塞启动 —— 见 LoadGameDataOfClass 里对缺失资产的处理。
	GetGameData();
}

const UGGYGOGameData* UGGYGOAssetManager::GetGameData()
{
	return Cast<const UGGYGOGameData>(
		LoadGameDataOfClass(UGGYGOGameData::StaticClass(), GGYGOGameDataPath, FPrimaryAssetType("GGYGOGameData")));
}

UPrimaryDataAsset* UGGYGOAssetManager::LoadGameDataOfClass(TSubclassOf<UPrimaryDataAsset> DataClass, const TSoftObjectPtr<UPrimaryDataAsset>& DataClassPath, FPrimaryAssetType PrimaryAssetType)
{
	UPrimaryDataAsset* Asset = nullptr;

	if (!DataClass)
	{
		return nullptr;
	}

	{
		FScopeLock Lock(&SyncObject);

		if (TObjectPtr<UPrimaryDataAsset>* Cached = GameDataMap.Find(DataClass))
		{
			return *Cached;
		}
	}

	if (!DataClassPath.IsNull())
	{
		// 同步加载。理由见头文件对 LoadGameDataOfClass 的说明。
		Asset = DataClassPath.LoadSynchronous();

		if (Asset)
		{
			// 加入 AlwaysCook 的 bundle，保证打包时这份资产被包含进去。
			// 少了这一步，编辑器里正常但打包后资产缺失。
			LoadPrimaryAssetsWithType(PrimaryAssetType);
		}
	}

	if (!Asset)
	{
		// 路径没配或指向了不存在的资产。
		//
		// 只警告不中止：这份资产是在编辑器里创建的，而项目早期它往往还不存在。
		// 若在这里中止，引擎会在能创建资产之前就起不来 —— 一个死循环。
		// 依赖它的功能各自判空并报错，症状会指向具体功能而不是整个引擎。
		UE_LOG(LogGGYGOAbilitySystem, Warning,
			TEXT("未能加载共享资产 [%s]（路径 [%s]）。依赖它的功能将失效。请创建该资产并在 DefaultGame.ini 的 [/Script/GGYGO.GGYGOAssetManager] 段配置 GGYGOGameDataPath。"),
			*DataClass->GetName(), *DataClassPath.ToString());
		return nullptr;
	}

	{
		FScopeLock Lock(&SyncObject);
		GameDataMap.Add(DataClass, Asset);
	}

	return Asset;
}
