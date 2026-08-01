/**
 * @file CharacterStateType.h
 * @brief 角色状态枚举 + 状态组 + 状态关系类型
 *
 * 按需扩展：目前只有 Idle 和 Moving 两个核心状态。
 * GAS 接入后逐步添加 Attacking / Dodging / HitStun / Dead 等。
 */
#pragma once

#include "CoreMinimal.h"
#include "CharacterStateType.generated.h"

UENUM(BlueprintType)
enum class ECharacterStateType : uint8
{
	None    UMETA(DisplayName="无", Hidden),

	Idle    UMETA(DisplayName="待机"),
	Moving  UMETA(DisplayName="移动"),

	MAX     UMETA(Hidden),
};

/**
 * 状态组枚举
 * 当前仅有 Locomotion 组生效。Action / Overlay / System 组预留，
 * GAS 接入后逐步启用。
 */
UENUM(BlueprintType)
enum class EStateGroup : uint8
{
	Locomotion  UMETA(DisplayName="移动层"),
	Action      UMETA(DisplayName="动作层"),
	Overlay     UMETA(DisplayName="叠加层"),
	System      UMETA(DisplayName="系统层"),
	MAX         UMETA(Hidden),
};

UENUM(BlueprintType)
enum class EStateRelationType : uint8
{
	Independent  UMETA(DisplayName="并行共存"),
	Interrupted  UMETA(DisplayName="可打断"),
	Blocked      UMETA(DisplayName="互斥阻止"),
};

UENUM(BlueprintType)
enum class EMovementGait : uint8
{
	None    UMETA(DisplayName="无（静止）"),
	Walk    UMETA(DisplayName="行走"),
	Run     UMETA(DisplayName="跑步"),
	Sprint  UMETA(DisplayName="冲刺"),
};
