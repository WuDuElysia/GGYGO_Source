/**
 * @file FullBodyIKDecisions.h
 * @brief 顶层辅助 FullBodyIK 决策模块
 *
 * 封装 FullBodyIK 顶层辅助状态机的 2 个过渡判定函数（Inactive ↔ Activated）。
 * 纯 C++ 类，仅依赖 FAnimSnapshot 只读指针，不依赖 UObject 运行时。
 */
#pragma once

#include "CoreMinimal.h"

// 前向声明（编译隔离：头文件不需要完整定义）
struct FAnimSnapshot;

/**
 * 顶层辅助 FullBodyIK 决策类
 *
 * 负责全身 IK 顶层辅助状态机的 2 条过渡判定：
 *   Inactive → Activated  激活全身 IK
 *   Activated → Inactive  停用全身 IK
 *
 * 所有决策函数为 const，只读 Snap，线程安全。
 */
class FFullBodyIKDecisions
{
public:
	/** 每帧由 AnimInstance 调用，设置快照指针 */
	void SetSnap(const FAnimSnapshot* InSnap);

	/** #64 Inactive → Activated：需要全身 IK（bFullBodyIKNeeded） */
	bool FullBodyIK_Activate() const;

	/** #65 Activated → Inactive：不再需要全身 IK（!bFullBodyIKNeeded） */
	bool FullBodyIK_Deactivate() const;

private:
	/** 当前帧快照指针（游戏线程写入，worker 线程只读） */
	const FAnimSnapshot* Snap = nullptr;
};
