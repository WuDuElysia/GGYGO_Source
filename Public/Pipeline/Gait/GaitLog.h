/**
 * @file GaitLog.h
 * @brief 逻辑侧步态统一日志类别声明
 *
 * `LogGait` 在代码库中只在本文件声明、只在 GaitLog.cpp 定义一次，
 * 便于统一控制 Gait_Authority 的诊断 verbosity。
 */
#pragma once

#include "CoreMinimal.h"

/** 逻辑侧步态统一日志类别，默认级别 Log，编译期最大级别 All。 */
DECLARE_LOG_CATEGORY_EXTERN(LogGait, Log, All);
