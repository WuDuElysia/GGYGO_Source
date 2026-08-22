/**
 * @file ZZZAnimLog.h
 * @brief ZZZ 动画层统一日志类别声明
 *
 * `LogZZZAnim` 在代码库中只在本文件声明、只在 ZZZAnimLog.cpp 定义一次，
 * 避免多个翻译单元各自持有一份 `DEFINE_LOG_CATEGORY_STATIC` 而无法统一控制 verbosity。
 */
#pragma once

#include "CoreMinimal.h"

/** ZZZ 动画层统一日志类别，默认级别 Log，编译期最大级别 All。 */
DECLARE_LOG_CATEGORY_EXTERN(LogZZZAnim, Log, All);
