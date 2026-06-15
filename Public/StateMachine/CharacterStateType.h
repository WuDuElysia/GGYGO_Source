/**
 * @file CharacterStateType.h
 * @brief 角色状态枚举
 *
 * 纯 C++ 状态机的状态标识。
 * 标记 BlueprintType 以便蓝图读取（动画蓝图、UI 等只读场景）。
 */
#pragma once

#include "CoreMinimal.h"
#include "CharacterStateType.generated.h"

UENUM(BlueprintType)
enum class ECharacterStateType : uint8
{
	Idle,
	RunStart,
	RunLoop,
	RunEnd,
	InAir,
	Attacking,
	Dodging,
	HitStun,
	Stunned,
	Dead,
	Interacting
};
