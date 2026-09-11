/**
 * @file GGYGOLocalPlayer.h
 * @brief 本地玩家 —— 跨关卡存活的玩家级数据宿主
 *
 * 存在的理由只有一个：给编队预设这类"每个本地玩家一份、跨关卡不变"的存档
 * 提供一个缓存点。
 *
 * ## 为什么需要缓存而不是每次读盘
 * `ULocalPlayerSaveGame::LoadOrCreateSaveGameForLocalPlayer` 每次调用都返回
 * **一个新对象**。编成面板与装配流程各调一次就会拿到两份互不相干的副本：
 * 面板改完存盘，装配那边读到的却是自己那份旧数据，
 * 表现为"编好的队进关卡不生效"，而且不报任何错。
 *
 * 缓存在 LocalPlayer 上而不是 PlayerState 或 GameInstance：
 * PlayerState 每次关卡切换都重建；GameInstance 只有一份，
 * 分屏时两个玩家的编队会互相覆盖。LocalPlayer 正好是
 * "一个本地玩家，跨关卡存活"这个粒度。
 *
 * 配置在 `DefaultEngine.ini` 的 `LocalPlayerClassName`。没配时引擎用
 * 基类 `ULocalPlayer`，编队存档会退化成每次读盘的无缓存行为。
 */
#pragma once

#include "Engine/LocalPlayer.h"

#include "GGYGOLocalPlayer.generated.h"

class UGGYGOSquadPresets;
class UObject;

UCLASS(Transient)
class GGYGO_API UGGYGOLocalPlayer : public ULocalPlayer
{
	GENERATED_BODY()

public:
	/**
	 * 本玩家保存的编队预设。首次调用时同步读盘，之后返回同一份对象。
	 *
	 * 编成面板与装配流程都经这里取，才能保证两边看到的是同一份数据。
	 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Player")
	UGGYGOSquadPresets* GetSquadPresets() const;

private:
	/**
	 * 编队预设缓存。
	 *
	 * `mutable` 是为了让 `GetSquadPresets()` 保持 const —— 调用方拿编队是读操作，
	 * 首次读盘属于实现细节，不应该迫使所有调用点持有非常量指针。
	 */
	UPROPERTY(Transient)
	mutable TObjectPtr<UGGYGOSquadPresets> SquadPresets;
};
