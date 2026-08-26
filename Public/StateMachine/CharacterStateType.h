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
};

/**
 * TurnBack 相位（急停转身的唯一真相，逻辑层写入，MotionDriver 与动画快照读取）
 *
 * None     不在转身
 * Frozen   已触发转身：逻辑时间轴第一段，输入不能打断
 * Released 已到达释放时间点：第一段继续到第二段，第二段允许无输入打断；该相位不直接写入 Actor rotation
 */
UENUM(BlueprintType)
enum class ETurnBackPhase : uint8
{
	None      UMETA(DisplayName="未转身"),
	Frozen    UMETA(DisplayName="转身等待"),
	Released  UMETA(DisplayName="已解冻"),
};
