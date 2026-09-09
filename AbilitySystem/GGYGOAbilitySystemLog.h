/**
 * @file GGYGOAbilitySystemLog.h
 * @brief GAS 层日志类别
 *
 * 与现有 `Pipeline/Gait/GaitLog.h`、`Animation/zzzAnim/ZZZAnimLog.h` 同一模式：
 * 单独声明一个日志类别，避免 GAS 的诊断信息混进 LogTemp。
 */
#pragma once

#include "Logging/LogMacros.h"

/** GAS 层日志。能力激活失败、组仲裁拒绝、配置错误等走这个类别。 */
GGYGO_API DECLARE_LOG_CATEGORY_EXTERN(LogGGYGOAbilitySystem, Log, All);
