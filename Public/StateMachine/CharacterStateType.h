/**
 * @file CharacterStateType.h
 * @brief 角色状态枚举 + 状态组 + 状态关系类型
 *
 * 阶段 6 重构：从扁平枚举升级为分组管理的并行状态体系。
 * 标记 BlueprintType 以便蓝图读取（动画蓝图、UI 等只读场景）。
 */
#pragma once

#include "CoreMinimal.h"
#include "CharacterStateType.generated.h"

/**
 * 角色状态类型枚举
 *
 * 按 Group 分为四大组：
 * - Locomotion（移动层）：同一时间只有 1 个活跃，代表 PrimaryState
 * - Action（动作层）：同一时间只有 1 个活跃，与 Locomotion 叠加存在
 * - Overlay（叠加层）：可多个共存，受击/眩晕等
 * - System（系统层）：可多个共存，死亡最高优先级
 */
UENUM(BlueprintType)
enum class ECharacterStateType : uint8
{
	// === None ===
	None			UMETA(DisplayName="无", Hidden),

	// === Locomotion Group（移动层 — 排他性，同组只有1个活跃）===
	Idle			UMETA(DisplayName="待机",	Group="Locomotion"),
	Moving			UMETA(DisplayName="移动",	Group="Locomotion"),
	InAir			UMETA(DisplayName="空中",	Group="Locomotion"),

	// === Action Group（动作层 — 排他性，同组只有1个活跃，可叠加在 Locomotion 上）===
	Attacking		UMETA(DisplayName="攻击",	Group="Action"),
	Dodging			UMETA(DisplayName="闪避",	Group="Action"),

	// === Overlay Group（叠加层 — 可多个共存）===
	HitStun			UMETA(DisplayName="受击",	Group="Overlay"),
	Stunned			UMETA(DisplayName="眩晕",	Group="Overlay"),

	// === System Group（系统层 — 可多个共存，但 Dead 最高优先级）===
	Dead			UMETA(DisplayName="死亡",	Group="System"),
	Interacting		UMETA(DisplayName="交互",	Group="System"),

	MAX				UMETA(Hidden),
};

/**
 * 状态组枚举
 *
 * 决定状态的排他性行为：
 * - Locomotion / Action 组内只能有 1 个活跃状态
 * - Overlay / System 组可有多个并存
 */
UENUM(BlueprintType)
enum class EStateGroup : uint8
{
	Locomotion		UMETA(DisplayName="移动层"),	// 排他性：同组只能有1个（PrimaryState 来源）
	Action			UMETA(DisplayName="动作层"),	// 排他性：同组只能有1个
	Overlay			UMETA(DisplayName="叠加层"),	// 可并行共存
	System			UMETA(DisplayName="系统层"),	// 可并行共存（Dead 最高优）
	MAX				UMETA(Hidden),
};

/**
 * 状态关系类型（NTE EHTStateRelationType 对应物）
 *
 * 用于 DataTable 驱动的 N×N 关系矩阵，
 * 决定两个状态之间是允许并行、打断还是互斥。
 */
UENUM(BlueprintType)
enum class EStateRelationType : uint8
{
	Independent		UMETA(DisplayName="并行共存"),	// 两状态可同时存在（如 RunLoop + Attacking）
	Interrupted		UMETA(DisplayName="可打断"),	// 新状态挤掉旧状态（如 HitStun → 挤掉 RunLoop）
	Blocked			UMETA(DisplayName="互斥阻止"),	// 新状态被拒绝（如 Attacking ↔ Dodging 同时请求）
};

/**
 * 移动步态（优化：LocomotionIntentProcessor 统一解析，MotionDriver/AnimInstance 直接消费）
 */
UENUM(BlueprintType)
enum class EMovementGait : uint8
{
	None	UMETA(DisplayName="无（静止）"),
	Walk	UMETA(DisplayName="行走"),
	Run		UMETA(DisplayName="跑步"),
	Sprint	UMETA(DisplayName="冲刺"),
};
