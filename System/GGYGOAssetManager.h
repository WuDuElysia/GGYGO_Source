/**
 * @file GGYGOAssetManager.h
 * @brief 项目 AssetManager —— 共享资产的加载与缓存
 *
 * 派生 `UAssetManager` 的理由是需要一个"整个进程只有一份、且生命周期
 * 覆盖所有关卡"的资产宿主。`UGGYGOGameData` 里的通用 GE 属于这一类：
 * 它们跨关卡不变，也不该随某个 World 卸载而被回收。
 *
 * GameMode、GameState 都不满足这个要求（随关卡重建），
 * 而全局变量又拿不到资产管理的好处（引用关系可见、路径自动更新）。
 */
#pragma once

#include "Engine/AssetManager.h"

#include "GGYGOAssetManager.generated.h"

class UGGYGOGameData;
class UObject;
class UPrimaryDataAsset;

UCLASS(Config = Game)
class GGYGO_API UGGYGOAssetManager : public UAssetManager
{
	GENERATED_BODY()

public:
	UGGYGOAssetManager();

	/**
	 * 取单例。
	 *
	 * 未在 `DefaultEngine.ini` 里把 `AssetManagerClassName` 指向本类时会
	 * 中止运行而不是返回 nullptr —— 那种配置缺失会让所有共享资产静默失效，
	 * 表现为"伤害不生效"之类完全不指向根因的症状，早崩比晚错好。
	 */
	static UGGYGOAssetManager& Get();

	/**
	 * 取项目共享资产。首次调用时同步加载。
	 *
	 * **可能返回 nullptr**：资产路径未配置或指向不存在的资产时。
	 * 不做成引用返回是因为项目早期这份资产往往还没创建，
	 * 那种情况下应当让依赖它的功能失效并报错，而不是让整个引擎起不来。
	 */
	const UGGYGOGameData* GetGameData();

protected:
	virtual void StartInitialLoading() override;

	/**
	 * 同步加载一个 PrimaryDataAsset 并缓存。
	 *
	 * 用同步加载是有意的：调用方（`UGGYGOGameData::Get`）是同步接口，
	 * 而它的调用点在伤害结算这类不能等待的路径上。
	 * 代价由 `StartInitialLoading` 的预加载抵消 —— 启动时就加载好，
	 * 运行时的"同步加载"实际上总是命中缓存。
	 */
	UPrimaryDataAsset* LoadGameDataOfClass(TSubclassOf<UPrimaryDataAsset> DataClass, const TSoftObjectPtr<UPrimaryDataAsset>& DataClassPath, FPrimaryAssetType PrimaryAssetType);

	/**
	 * 项目共享资产的路径。
	 *
	 * `Config = Game` 让它可以在 `DefaultGame.ini` 里配，
	 * 从而不必为了换一份 GameData 重新编译。
	 */
	UPROPERTY(Config)
	TSoftObjectPtr<UGGYGOGameData> GGYGOGameDataPath;

	/**
	 * 已加载的共享资产缓存。
	 *
	 * 持强引用防止 GC 回收。键是资产类，因为将来可能有多种共享资产类型。
	 */
	UPROPERTY(Transient)
	TMap<TObjectPtr<UClass>, TObjectPtr<UPrimaryDataAsset>> GameDataMap;

private:
	/** 保护 `GameDataMap`。资产加载可能从多个线程触发。 */
	FCriticalSection SyncObject;
};
