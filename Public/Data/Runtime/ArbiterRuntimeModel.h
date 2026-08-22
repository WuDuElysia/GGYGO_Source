/**
 * @file ArbiterRuntimeModel.h
 * @brief 动作限制与批准结果模型
 */
#pragma once

#include "StateMachine/CharacterStateType.h"

/** ArbiterPipeline 输出、状态机读取的本帧仲裁结果。 */
struct FArbiterRuntimeModel
{
	/** 阻止移动。 */
	bool bBlockMove = false;

	/** 阻止攻击。 */
	bool bBlockAttack = false;

	/** 阻止闪避。 */
	bool bBlockDodge = false;

	/** 获得批准的动作；Idle 表示没有动作被批准。 */
	ECharacterStateType ActionGranted = ECharacterStateType::Idle;
};
