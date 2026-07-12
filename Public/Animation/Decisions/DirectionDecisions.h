/**
 * @file DirectionDecisions.h
 * @brief Layer 3.5 Direction Dispatcher 决策模块
 *
 * 封装起步方向分派的 18 个过渡判定函数：
 *   方向分区（Forward/Left/Right/Back/ForwardVariant/Exit）
 *   步态子分派（EnterForward Sprint/Run、EnterForwardVar Sprint/Run）
 *   后向脚分派（EnterBack L↔R）
 *   BackLeft / BackRight 子机内部过渡（Start→B→Exit）
 *
 * 纯 C++ 类，仅依赖 FAnimSnapshot 只读指针，不依赖 UObject 运行时。
 */
#pragma once

#include "CoreMinimal.h"

// 前向声明（编译隔离：头文件不需要完整定义）
struct FAnimSnapshot;

/**
 * Layer 3.5 Direction Dispatcher 决策类
 *
 * 负责起步方向分派状态机的 18 条过渡判定：
 *   方向四分区 + ForwardVariant + Exit
 *   Forward/ForwardVar 步态子路由（Sprint/Run）
 *   Back 左右脚子分派
 *   BackLeft/BackRight 内部过渡（Start→B→Exit，AutoRule）
 *
 * 所有决策函数为 const，只读 Snap，线程安全。
 */
class FDirectionDecisions
{
public:
	/** 每帧由 AnimInstance 调用，设置快照指针 */
	void SetSnap(const FAnimSnapshot* InSnap);

	// ---- Direction Dispatcher（方向四分区 + Variant + Exit）----

	/** 前向起步：移动角绝对值 ≤ 45° */
	bool Enter_Dir_Forward() const;

	/** 左向起步：移动角 ∈ [-135°, -45°) */
	bool Enter_Dir_Left() const;

	/** 右向起步：移动角 ∈ (45°, 135°] */
	bool Enter_Dir_Right() const;

	/** Forward Variant 分支：移动角绝对值 ≤ 45° */
	bool Enter_Dir_Forward_Variant() const;

	/** 后向起步：移动角绝对值 > 135° */
	bool Enter_Dir_Back() const;

	/** Enter 子机退出检查（AutoRule equivalent） */
	bool Enter_Exit_Check() const;

	// ---- Forward Gait 子路由 ----

	/** Forward → Sprint：步态等于 Sprint */
	bool EnterForward_Is_Sprint() const;

	/** Forward → Run：步态等于 Run 或 None */
	bool EnterForward_Is_Run() const;

	// ---- Forward Variant Gait 子路由 ----

	/** ForwardVar → Sprint：步态等于 Sprint */
	bool EnterForwardVar_Is_Sprint() const;

	/** ForwardVar → Run：步态等于 Run 或 None */
	bool EnterForwardVar_Is_Run() const;

	// ---- Enter Back 脚分派 ----

	/** Enter Back → 右脚子机：当前支撑脚为右脚 */
	bool EnterBack_L_To_R() const;

	/** Enter Back → 左脚子机：当前支撑脚为左脚 */
	bool EnterBack_R_To_L() const;

	// ---- BackLeft 子机内部过渡（AutoRule equivalents）----

	/** BackLeft: Start → B */
	bool BackLeft_Start_To_B() const;

	/** BackLeft: Start → B AutoRule guard */
	bool BackLeft_Start_To_B_Auto() const;

	/** BackLeft: B → Exit */
	bool BackLeft_B_To_Exit() const;

	// ---- BackRight 子机内部过渡（AutoRule equivalents）----

	/** BackRight: Start → B */
	bool BackRight_Start_To_B() const;

	/** BackRight: Start → B AutoRule guard */
	bool BackRight_Start_To_B_Auto() const;

	/** BackRight: B → Exit */
	bool BackRight_B_To_Exit() const;

private:
	/** 当前帧快照指针（游戏线程写入，worker 线程只读） */
	const FAnimSnapshot* Snap = nullptr;
};
