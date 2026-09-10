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

protected:
	//~APlayerController interface
	virtual void PostProcessInput(const float DeltaTime, const bool bGamePaused) override;
	//~End of APlayerController interface
};
