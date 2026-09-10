/**
 * @file GGYGOGameplayCueManager.cpp
 * @brief GameplayCue 加载策略实现
 */
#include "AbilitySystem/Cues/GGYGOGameplayCueManager.h"

#include "AbilitySystemGlobals.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "Engine/AssetManager.h"
#include "Engine/Engine.h"
#include "GameplayCueSet.h"
#include "GameplayTagContainer.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOGameplayCueManager)

UGGYGOGameplayCueManager::UGGYGOGameplayCueManager(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

UGGYGOGameplayCueManager* UGGYGOGameplayCueManager::Get()
{
	return Cast<UGGYGOGameplayCueManager>(UAbilitySystemGlobals::Get().GetGameplayCueManager());
}

void UGGYGOGameplayCueManager::OnCreated()
{
	Super::OnCreated();

	UE_LOG(LogGGYGOAbilitySystem, Log,
		TEXT("GameplayCueManager 就绪。本环境加载 Cue：%s"),
		ShouldLoadCuesForThisEnvironment() ? TEXT("是") : TEXT("否（Dedicated Server）"));
}

bool UGGYGOGameplayCueManager::ShouldLoadCuesForThisEnvironment() const
{
	// Dedicated Server 没有渲染与音频，Cue 只有开销。
	// 用 IsRunningDedicatedServer 而不是判断 NetMode：前者在引擎启动早期就有效，
	// 而 CueManager 的创建时机早于任何 World 存在。
	return !IsRunningDedicatedServer();
}

bool UGGYGOGameplayCueManager::ShouldAsyncLoadRuntimeObjectLibraries() const
{
	// 返回 false 会让引擎在启动时同步扫描并加载全部 Cue。
	// 客户端要异步（不阻塞启动），服务器干脆不加载。
	return ShouldLoadCuesForThisEnvironment();
}

bool UGGYGOGameplayCueManager::ShouldSyncLoadMissingGameplayCues() const
{
	// 恒为 false。同步加载会在命中瞬间卡一帧，
	// 而动作游戏里掉帧比少一个特效严重得多。
	return false;
}

bool UGGYGOGameplayCueManager::ShouldAsyncLoadMissingGameplayCues() const
{
	return ShouldLoadCuesForThisEnvironment();
}

bool UGGYGOGameplayCueManager::ShouldSuppressGameplayCues(AActor* TargetActor)
{
	if (!ShouldLoadCuesForThisEnvironment())
	{
		return true;
	}

	return Super::ShouldSuppressGameplayCues(TargetActor);
}

void UGGYGOGameplayCueManager::PreloadCuesForTags(const FGameplayTagContainer& CueTags)
{
	if (!ShouldLoadCuesForThisEnvironment() || CueTags.IsEmpty())
	{
		return;
	}

	const UGameplayCueSet* RuntimeCueSet = GetRuntimeCueSet();
	if (!RuntimeCueSet)
	{
		// Cue 库还没扫描完。这不是错误 —— 异步扫描完成后
		// 引擎会自行按需加载，只是失去了预热的提前量。
		return;
	}

	TArray<FSoftObjectPath> PathsToLoad;

	for (const FGameplayTag& CueTag : CueTags)
	{
		if (!CueTag.IsValid())
		{
			continue;
		}

		// 在 CueSet 里查这个 Tag 对应的资产路径。
		const int32* DataIndex = RuntimeCueSet->GameplayCueDataMap.Find(CueTag);
		if (!DataIndex || !RuntimeCueSet->GameplayCueData.IsValidIndex(*DataIndex))
		{
			// Tag 没有对应资产。可能是还没做特效，也可能是 Tag 拼错了。
			// 用 Verbose 而不是 Warning：开发早期大量 Cue 尚未制作，
			// 每个都警告会淹没真正的问题。
			UE_LOG(LogGGYGOAbilitySystem, Verbose,
				TEXT("PreloadCuesForTags: Cue Tag [%s] 没有对应资产。"), *CueTag.ToString());
			continue;
		}

		const FGameplayCueNotifyData& CueData = RuntimeCueSet->GameplayCueData[*DataIndex];

		// 已经加载过的跳过。重复请求同一路径会让 StreamableManager
		// 建立多个句柄，虽然无害但浪费。
		if (CueData.LoadedGameplayCueClass)
		{
			PreloadedCues.Add(CueData.LoadedGameplayCueClass);
			continue;
		}

		PathsToLoad.AddUnique(CueData.GameplayCueNotifyObj);
	}

	if (PathsToLoad.IsEmpty())
	{
		return;
	}

	UAssetManager& AssetManager = UAssetManager::Get();

	for (const FSoftObjectPath& Path : PathsToLoad)
	{
		// 逐个加载而不是打包成一个批次句柄：批次里任何一个失败会影响整批的
		// 完成回调，而 Cue 缺失是可以单独容忍的。
		AssetManager.GetStreamableManager().RequestAsyncLoad(
			Path,
			FStreamableDelegate::CreateUObject(this, &ThisClass::OnCuePreloadComplete, Path),
			FStreamableManager::DefaultAsyncLoadPriority,
			/*bManageActiveHandle=*/false,
			/*bStartStalled=*/false,
			TEXT("GGYGOCuePreload"));
	}
}

void UGGYGOGameplayCueManager::OnCuePreloadComplete(FSoftObjectPath CuePath)
{
	UClass* LoadedClass = Cast<UClass>(CuePath.ResolveObject());
	if (!LoadedClass)
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning,
			TEXT("OnCuePreloadComplete: [%s] 加载完成但解析不到类。"), *CuePath.ToString());
		return;
	}

	// 存进集合持强引用。不持引用的话 GC 会立刻回收刚加载的类，预热白做。
	PreloadedCues.Add(LoadedClass);
}

void UGGYGOGameplayCueManager::DumpPreloadedCues()
{
	const UGGYGOGameplayCueManager* CueManager = Get();
	if (!CueManager)
	{
		UE_LOG(LogGGYGOAbilitySystem, Log,
			TEXT("DumpPreloadedCues: 当前 CueManager 不是 UGGYGOGameplayCueManager。请检查 DefaultGame.ini 的 GameplayCueManagerClassName。"));
		return;
	}

	UE_LOG(LogGGYGOAbilitySystem, Log,
		TEXT("已预加载 %d 个 Cue："), CueManager->PreloadedCues.Num());

	for (const TObjectPtr<UClass>& CueClass : CueManager->PreloadedCues)
	{
		UE_LOG(LogGGYGOAbilitySystem, Log, TEXT("  %s"), *GetNameSafe(CueClass));
	}
}
