/**
 * @file GGYGOGameplayCueManager.h
 * @brief GameplayCue 的加载策略
 *
 * 引擎默认会在启动时扫描并加载**全部** Cue 资产。战斗游戏的 Cue 数量会随
 * 角色与技能线性增长，全量加载有两个后果：启动时间变长，以及大量当前关卡
 * 用不到的特效常驻内存。
 *
 * 本类改为按需加载，并处理由此引入的两个问题。
 *
 * ## 问题一：按需加载会让首次触发缺特效
 * 异步加载完成前 Cue 类还不存在，第一次命中就没有火花。
 * 解决办法是**预热**：在角色初始化时把它会用到的 Cue 提前加载
 * （`PreloadCuesForTags`），战斗开始时它们已经就位。
 *
 * ## 问题二：同步加载会卡顿
 * 引擎发现 Cue 缺失时可以选择同步加载补上，那会在命中瞬间卡一帧。
 * 本类禁止同步加载（`ShouldSyncLoadMissingGameplayCues` 返回 false），
 * 宁可这一次不播特效也不卡帧 —— 动作游戏里掉帧比少一个特效严重得多。
 *
 * ## Dedicated Server 完全跳过
 * 服务器没有渲染与音频，Cue 对它只有开销没有意义。
 * `ShouldSuppressGameplayCues` 在服务器上直接返回 true，
 * 连 Cue 对象都不创建。
 */
#pragma once

#include "GameplayCueManager.h"

#include "GGYGOGameplayCueManager.generated.h"

class UObject;
struct FGameplayTagContainer;

UCLASS()
class GGYGO_API UGGYGOGameplayCueManager : public UGameplayCueManager
{
	GENERATED_BODY()

public:
	UGGYGOGameplayCueManager(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/** 取全局实例。未配置本类为 CueManager 时返回 nullptr。 */
	static UGGYGOGameplayCueManager* Get();

	//~UGameplayCueManager interface
	virtual void OnCreated() override;

	/** 启动时是否异步加载全部 Cue 库。服务器不加载。 */
	virtual bool ShouldAsyncLoadRuntimeObjectLibraries() const override;

	/** 缺失的 Cue 是否同步加载。**恒为 false** —— 见文件头对卡顿的说明。 */
	virtual bool ShouldSyncLoadMissingGameplayCues() const override;

	/** 缺失的 Cue 是否异步加载。服务器不加载。 */
	virtual bool ShouldAsyncLoadMissingGameplayCues() const override;

	/** 是否完全跳过 Cue。Dedicated Server 上恒为 true。 */
	virtual bool ShouldSuppressGameplayCues(AActor* TargetActor) override;
	//~End of UGameplayCueManager interface

	/**
	 * 预加载一批 Cue。
	 *
	 * 应当在角色初始化时调用，把该角色会用到的 Cue 提前备好。
	 * 加载是异步的，本函数立即返回。
	 *
	 * @param CueTags 要预加载的 Cue Tag。非 Cue Tag 会被忽略。
	 */
	void PreloadCuesForTags(const FGameplayTagContainer& CueTags);

	/** 输出当前已预加载的 Cue，用于排查"特效没播"是不是加载问题。 */
	static void DumpPreloadedCues();

private:
	/** 当前运行环境是否需要 Cue。Dedicated Server 不需要。 */
	bool ShouldLoadCuesForThisEnvironment() const;

	/**
	 * 已预加载的 Cue 类。
	 *
	 * 必须持强引用：加载完成后若没有任何引用，GC 会立刻回收，
	 * 预热就白做了。用 `Transient` 因为它是运行时状态、不该被序列化。
	 */
	UPROPERTY(Transient)
	TSet<TObjectPtr<UClass>> PreloadedCues;

	/** 预加载完成回调。 */
	void OnCuePreloadComplete(FSoftObjectPath CuePath);
};
