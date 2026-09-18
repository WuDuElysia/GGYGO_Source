/**
 * @file GGYGOAnimationStateFrame.h
 * @brief C++ 权威层向动画蓝图发布的一帧只读语义事实
 */
#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Character/Data/GGYGOMovementTypes.h"
#include "GameFramework/CharacterMovementComponent.h"

#include "GGYGOAnimationStateFrame.generated.h"

/**
 * 动画表现层的一帧输入。
 *
 * 这里只描述角色在本帧真实发生了什么，不描述 AnimBP 有哪些状态、节点或过渡线。
 * 数据在游戏线程集中抓取，随后只读地交给动画线程与 AnimGraph 使用。
 */
USTRUCT(BlueprintType)
struct FGGYGOAnimationStateFrame
{
	GENERATED_BODY()

	/** 角色当前世界速度。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	FVector WorldVelocity = FVector::ZeroVector;

	/** 本帧 CMC 使用的世界加速度。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	FVector WorldAcceleration = FVector::ZeroVector;

	/** 水平速度大小，单位 cm/s。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	float HorizontalSpeed = 0.0f;

	/** 水平速度的世界空间单位方向。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	FVector WorldVelocityDirection = FVector::ZeroVector;

	/** 实际水平速度相对角色朝向的角度：0 为前，+90 为右。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	float LocalVelocityAngle = 0.0f;

	/** 实际水平速度的 AnimBP 分量：X 为右，Y 为前。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	FVector2D LocalVelocityBlend = FVector2D::ZeroVector;

	/** CMC 已解算的步态。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	EGGYGOGait Gait = EGGYGOGait::None;

	/** 引擎移动模式。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	TEnumAsByte<EMovementMode> MovementMode = MOVE_None;

	/** CMC 权威转身动作的当前相位。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	EGGYGOTurnBackPhase TurnBackPhase = EGGYGOTurnBackPhase::None;

	/** 当前是否存在可预测的移动输入。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	bool bHasMoveInput = false;

	/** 水平速度是否超过移动判定阈值。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	bool bMovingHorizontally = false;

	/** 角色是否处于地面移动模式。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	bool bGrounded = true;

	/** GAS 是否禁止普通移动。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	bool bMovementBlocked = false;

	/** 转身是否已经进入把方向交回输入的 RunOut 段。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Movement")
	bool bTurnBackRunOut = false;

	/** ASC 在本帧拥有的状态 Tag，只供表现层查询，不允许反向写入。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|State")
	FGameplayTagContainer OwnedStateTags;
};
