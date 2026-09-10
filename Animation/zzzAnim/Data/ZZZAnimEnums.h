/**
 * @file ZZZAnimEnums.h
 * @brief ZZZ 动画层使用的枚举类型
 */
#pragma once

#include "CoreMinimal.h"
#include "ZZZAnimEnums.generated.h"

/**
 * Locomotion 动画状态（AnimBP 拓扑中的状态，不是角色逻辑状态）
 *
 * 与移动层的状态严格区分：
 *   FZZZAnimSnapshot::bShouldMove / Gait   移动层状态，由 CMC 决定
 *   EZZZAnimLocomotionState  动画状态（Conduit / EnterMove / ...），由 AnimBP 决定
 */
UENUM(BlueprintType)
enum class EZZZAnimLocomotionState : uint8
{
	None       UMETA(DisplayName = "无"),
	NotMoving  UMETA(DisplayName = "待机"),
	Conduit    UMETA(DisplayName = "导管"),
	EnterMove  UMETA(DisplayName = "起步"),
	Moving     UMETA(DisplayName = "移动"),
};

/** Moving 内部子状态（不并入 EZZZAnimLocomotionState） */
UENUM(BlueprintType)
enum class EZZZAnimMovingSubState : uint8
{
	None      UMETA(DisplayName = "未进入"),
	WalkRun   UMETA(DisplayName = "走跑"),
	TurnBack  UMETA(DisplayName = "转身"),
};
