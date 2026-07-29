/**
 * @file DecisionTypes.h
 * @brief 动画决策模块公共类型定义
 *
 * 定义各 Decision_Module 共享的代理类型别名，用于解耦 AnimInstance
 * 的 GetCurveValue / GetInstanceStateWeight / GetRelevantAnimTimeRemaining 调用。
 */
#pragma once

#include "CoreMinimal.h"

// 前向声明：快照结构体定义在 NTEAnimInstance.h 中
struct FAnimSnapshot;

/** 曲线值查询代理：参数 (CurveName, OutValue)，返回是否找到 */
using FCurveValueDelegate = TFunction<bool(FName, float&)>;

/** 状态权重查询代理：参数 (MachineIndex, StateIndex)，返回权重值 */
using FStateWeightDelegate = TFunction<float(int32, int32)>;

/** 动画剩余时间查询代理：参数 (MachineIndex, StateIndex)，返回当前动画剩余时间（秒），包装 UAnimInstance::GetRelevantAnimTimeRemaining */
using FAnimTimeRemainingDelegate = TFunction<float(int32, int32)>;
