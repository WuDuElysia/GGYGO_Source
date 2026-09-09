/**
 * @file FStateRelationRow.h
 * @brief 状态关系矩阵 DataTable 的行结构
 *
 * 每一行代表一个"From 状态"到其他所有状态的关系。
 * DataTable 的 RowName = From 状态枚举值。
 * 每列（通过 Property 名称）对应一个 To 状态。
 *
 * 使用方式：
 *   在编辑器创建 DT_StateRelationMatrix (DataTable)，
 *   Row Structure 选此 struct，
 *   每行填 11 个 EStateRelationType 值。
 */
#pragma once

#include "CoreMinimal.h"
#include "StateMachine/CharacterStateType.h"
#include "FStateRelationRow.generated.h"

/**
 * 状态关系矩阵的一行数据
 *
 * 每个 UPROPERTY 代表一个 [From → To] 方向的关系。
 *
 * 命名规则: 列名必须和 ECharacterStateType 枚举值完全匹配，
 *           StateManagerComponent 通过 FProperty 反射按名称查找。
 */
USTRUCT(BlueprintType)
struct GGYGO_API FStateRelationRow
{
	GENERATED_BODY()

	// === Locomotion 组目标 ===

	/** From → Idle 的关系 */
	UPROPERTY(EditAnywhere)
	EStateRelationType Idle = EStateRelationType::Blocked;

	/** From → Moving 的关系 */
	UPROPERTY(EditAnywhere)
	EStateRelationType Moving = EStateRelationType::Blocked;

	/** From → InAir 的关系 */
	UPROPERTY(EditAnywhere)
	EStateRelationType InAir = EStateRelationType::Blocked;

	// === Action 组目标 ===

	/** From → Attacking 的关系 */
	UPROPERTY(EditAnywhere)
	EStateRelationType Attacking = EStateRelationType::Blocked;

	/** From → Dodging 的关系 */
	UPROPERTY(EditAnywhere)
	EStateRelationType Dodging = EStateRelationType::Blocked;

	// === Overlay 组目标 ===

	/** From → HitStun 的关系 */
	UPROPERTY(EditAnywhere)
	EStateRelationType HitStun = EStateRelationType::Blocked;

	/** From → Stunned 的关系 */
	UPROPERTY(EditAnywhere)
	EStateRelationType Stunned = EStateRelationType::Blocked;

	// === System 组目标 ===

	/** From → Dead 的关系 */
	UPROPERTY(EditAnywhere)
	EStateRelationType Dead = EStateRelationType::Blocked;

	/** From → Interacting 的关系 */
	UPROPERTY(EditAnywhere)
	EStateRelationType Interacting = EStateRelationType::Blocked;
};
