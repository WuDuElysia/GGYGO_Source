/**
 * @file IntentRuntimeModel.h
 * @brief 角色本帧动作与移动意图模型
 *
 * 这是输入/意图阶段的输出，不保存实际速度或跨帧状态。
 */
#pragma once

#include "CoreMinimal.h"

/** 本帧由输入管线解析出的角色意图。 */
struct FIntentRuntimeModel
{
	/** 攻击意图，由 FAttackIntentProcessor 写入。 */
	bool bWantsToAttack = false;

	/** 闪避意图，由 FDodgeIntentProcessor 写入。 */
	bool bWantsToDodge = false;

	/** 摄像机相对输入转换后的世界空间水平移动方向。 */
	FVector DesiredWorldMoveDir = FVector::ZeroVector;

	/** 只清理本帧动作意图；移动方向由 LocomotionIntentProcessor 每帧显式覆盖。 */
	void ResetFrameIntents()
	{
		bWantsToAttack = false;
		bWantsToDodge = false;
	}
};
