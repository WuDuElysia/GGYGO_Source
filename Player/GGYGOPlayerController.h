/**
 * @file GGYGOPlayerController.h
 * @brief 玩家控制器
 *
 * 当前只承担一件事：在正确的时机驱动 ASC 消费本帧的输入缓存。
 *
 * ## 为什么必须由 Controller 驱动，且必须在 PostProcessInput 里
 * ASC 把输入分成 pressed / held / released 三个缓存，需要有人每帧统一消费。
 * 时机很关键 —— 必须在**所有** Enhanced Input 事件都派发完之后：
 * 若在派发中途消费，同一帧内后到的按下事件就会被推迟到下一帧，
 * 表现为偶发的一帧输入延迟。
 *
 * `PostProcessInput` 正是引擎保证的"本帧输入全部处理完"的点，
 * 而它只存在于 PlayerController 上，组件的 Tick 拿不到这个时机。
 */
#pragma once

#include "GameFramework/PlayerController.h"

#include "GGYGOPlayerController.generated.h"

class APawn;
class UGGYGOAbilitySystemComponent;
class UObject;

UCLASS(Config = Game, meta = (ShortTooltip = "GGYGO 玩家控制器"))
class GGYGO_API AGGYGOPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AGGYGOPlayerController(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/** 取当前操控角色的 ASC。未附身或角色没有 ASC 时返回 nullptr。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|PlayerController")
	UGGYGOAbilitySystemComponent* GetGGYGOAbilitySystemComponent() const;

	// 下面几个是编队的调试入口，在编成面板做出来之前也是唯一的编队操作方式。
	//
	// 声明不能用 `#if !UE_BUILD_SHIPPING` 包起来：UHT 不接受预处理块里的
	// `UFUNCTION`（只放行 `WITH_EDITORONLY_DATA`）。所以条件编译只包在
	// 实现里，Shipping 下这几个函数存在但是空的。

	/**
	 * 打印本地玩家保存的编队与出战选择。
	 *
	 * 控制台：`GGYGODumpSquadPresets`
	 */
	UFUNCTION(Exec)
	void GGYGODumpSquadPresets();

	/**
	 * 往指定编队的指定位置放一个角色，编队与位置不存在则补齐，改完立即存盘。
	 *
	 * 控制台：`GGYGOSetSquadMember 0 0 DA_Pawn_Pyrios`
	 *
	 * @param PawnDataName 角色配置资产名（`GGYGOPawnData` 的 PrimaryAssetId 名字部分）。
	 *                     传空字符串表示把该位置清空。
	 */
	UFUNCTION(Exec)
	void GGYGOSetSquadMember(int32 PresetIndex, int32 MemberIndex, const FString& PawnDataName);

	/**
	 * 选择出战编队，改完立即存盘。下一次进关卡生效 —— 局内不会换队。
	 *
	 * 控制台：`GGYGOSetActiveSquadPreset 0`
	 */
	UFUNCTION(Exec)
	void GGYGOSetActiveSquadPreset(int32 PresetIndex);

	/**
	 * 删除全部编队并存盘，用于回到"未编成"状态验证默认编队的回落。
	 *
	 * 控制台：`GGYGOClearSquadPresets`
	 */
	UFUNCTION(Exec)
	void GGYGOClearSquadPresets();

protected:
	//~APlayerController interface
	virtual void PostProcessInput(const float DeltaTime, const bool bGamePaused) override;
	//~End of APlayerController interface
};
