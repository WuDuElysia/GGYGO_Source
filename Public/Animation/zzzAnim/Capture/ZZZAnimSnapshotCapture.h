/**
 * @file ZZZAnimSnapshotCapture.h
 * @brief ZZZ 动画快照抓取器
 *
 * 从 ABaseCharacter::GetRuntimeData()->ZZZAnim 直读数据填入 FZZZAnimSnapshot。
 * 不保留拷贝、不走 Push Interface — 单一数据源原则。
 */
#pragma once

#include "CoreMinimal.h"
#include "Animation/zzzAnim/Data/ZZZAnimSnapshot.h"

class ABaseCharacter;

class FZZZAnimSnapshotCapture
{
public:
	/**
	 * 从角色 RuntimeData.ZZZAnim 直读数据，重置并填入快照
	 * @param OutSnap  输出的快照（先重置再填入）
	 * @param InOwner  所属角色（为空时快照保持默认值）
	 */
	void Capture(FZZZAnimSnapshot& OutSnap, ABaseCharacter* InOwner);
};
