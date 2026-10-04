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
#include "Templates/SubclassOf.h"

#include "GGYGOAssetManager.generated.h"

class UGGYGOGameData;
class UGameplayEffect;

/** 启动快照状态；DependencyUnavailable 表示GameData不可用，无法读取GE配置。 */
enum class EGGYGOSharedAssetLoadState : uint8
{
	NotReady,
	NotConfigured,
	LoadFailed,
	DependencyUnavailable,
	Ready
};

UCLASS(Config = Game)
class GGYGO_API UGGYGOAssetManager : public UAssetManager
{
	GENERATED_BODY()

public:
	UGGYGOAssetManager();

	/**
	 * 获取引擎实际持有的项目 AssetManager，可能返回 nullptr。
	 *
	 * 仅限游戏线程；线程不符、引擎或 AssetManager 未就绪、类型不符时诊断并返回空。
	 * 调用方必须判空；此接口不创建兜底实例，也不替换引擎的 AssetManager。
	 * 无效的 AssetManagerClassName 路径可能在引擎初始化阶段先触发 Fatal，
	 * 这里的可空契约只覆盖执行到本接口时的获取失败。
	 */
	static UGGYGOAssetManager* TryGet();

	/**
	 * 以下接口仅在游戏线程读取启动快照，不加载或重试。
	 * 整次预载完成前返回空/NotReady/false；完成不代表每项成功，须判空或检查逐项状态。
	 * 失败结果同样冻结；运行中更改配置不刷新快照，重新启动后才生效。
	 */
	const UGGYGOGameData* GetGameData() const;
	TSubclassOf<UGameplayEffect> GetSharedDamageGameplayEffect() const;
	TSubclassOf<UGameplayEffect> GetSharedHealGameplayEffect() const;
	TSubclassOf<UGameplayEffect> GetSharedSelfDestructGameplayEffect() const;

	EGGYGOSharedAssetLoadState GetGameDataLoadState() const;
	EGGYGOSharedAssetLoadState GetSharedDamageLoadState() const;
	EGGYGOSharedAssetLoadState GetSharedHealLoadState() const;
	EGGYGOSharedAssetLoadState GetSharedSelfDestructLoadState() const;
	bool HasCompletedSharedAssetPreload() const;

protected:
	virtual void StartInitialLoading() override;

	/**
	 * 项目共享资产的路径。
	 *
	 * `Config = Game` 让它可以在 `DefaultGame.ini` 里配，
	 * 从而不必为了换一份 GameData 重新编译。
	 */
	UPROPERTY(Config)
	TSoftObjectPtr<UGGYGOGameData> GGYGOGameDataPath;

private:
	bool CanReadSharedAssets() const;

	/** Manager是预载资源的唯一宿主，UPROPERTY强引用覆盖其生命周期，不手动Root。 */
	UPROPERTY(Transient)
	TObjectPtr<UGGYGOGameData> LoadedGameData;

	UPROPERTY(Transient)
	TSubclassOf<UGameplayEffect> SharedDamageGameplayEffect;

	UPROPERTY(Transient)
	TSubclassOf<UGameplayEffect> SharedHealGameplayEffect;

	UPROPERTY(Transient)
	TSubclassOf<UGameplayEffect> SharedSelfDestructGameplayEffect;

	EGGYGOSharedAssetLoadState GameDataLoadState = EGGYGOSharedAssetLoadState::NotReady;
	EGGYGOSharedAssetLoadState SharedDamageLoadState = EGGYGOSharedAssetLoadState::NotReady;
	EGGYGOSharedAssetLoadState SharedHealLoadState = EGGYGOSharedAssetLoadState::NotReady;
	EGGYGOSharedAssetLoadState SharedSelfDestructLoadState = EGGYGOSharedAssetLoadState::NotReady;
	bool bSharedAssetsPreloadStarted = false;
	bool bSharedAssetsPreloadCompleted = false;
};
