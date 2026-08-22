/**
 * @file GaitRuntimeModel.h
 * @brief 逻辑侧步态运行时模型
 */
#pragma once

#include "StateMachine/CharacterStateType.h"

/** GaitAuthority 输出的逻辑步态及跨帧闪避契约。 */
struct FGaitRuntimeModel
{
	/** 闪避结束后进入 Run 的跨帧契约。 */
	bool bDodgeRunPending = false;

	/** 当前帧统一解析出的逻辑步态。 */
	EMovementGait ResolvedGait = EMovementGait::None;
};
