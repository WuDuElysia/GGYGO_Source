/**
 * @file GroundedDecisions.h
 * @brief Layer 2 Grounded 决策模块
 *
 * 封装 Grounded 层级（MainGroundedStatesMachine）的全部过渡判定函数。
 * 纯 C++ 类，依赖 FAnimSnapshot 只读指针、FLocomotionTuning 配置与
 * FAnimTimeRemainingDelegate 动画剩余时间代理，不依赖 UObject 运行时。
 */
#pragma once

#include "CoreMinimal.h"
#include "Animation/Decisions/DecisionTypes.h"

// 前向声明（编译隔离：头文件不需要完整定义）
struct FAnimSnapshot;
struct FLocomotionTuning;

/**
 * Layer 2 Grounded 决策类
 *
 * 负责落地层状态机（MainGroundedStatesMachine）的过渡判定：
 *   - Grounded_Just_Landed（既有）— 判定是否为重着地（冲击速度超过阈值）
 *   - Entry 分派 / FromRoll / LandedStationary / LandedMobile / StatToMove /
 *     RollToRun / Landed / VaultContinue / VinesOver / RunOnWallsOver /
 *     Conduit / SprintVinesOver 等 33 个新增过渡判定（#468–#500）
 *
 * 所有决策函数为 const，只读 Snap（与注入的 AnimTime 代理），线程安全。
 */
class FGroundedDecisions
{
public:
	/**
	 * 初始化 Tuning 配置与 AnimTime 代理
	 * @param InTuning 配置指针；若为 nullptr 则降级到 DefaultTuning
	 * @param InAnimTimeDelegate 动画剩余时间查询代理（可为空，空则降级见 GetAnimTimeRemainingSafe）
	 */
	void Init(const FLocomotionTuning* InTuning,
	          FAnimTimeRemainingDelegate InAnimTimeDelegate = nullptr);

	/** 每帧由 AnimInstance 调用，设置快照指针 */
	void SetSnap(const FAnimSnapshot* InSnap);

	// ---- 既有（1 个，保留不动）----
	/** 刚落地：标记刚落地且落地冲击速度大于 Tuning.JumpLandedThreshold */
	bool Grounded_Just_Landed() const;

	// ---- Entry 分派（#468–#477）----
	bool MG_Entry_To_VaultContinue() const;    // #468 (GroundAnimState==0x8)&&(VaultSubState!=0x0)&&bCanStayingTheGround
	bool MG_Entry_To_Conduit() const;          // #469 true
	bool MG_Entry_To_FromRoll() const;         // #470 GroundAnimState==0x7
	bool MG_Entry_To_LandedMobile() const;     // #471 (GroundAnimState==0x1)&&bShouldMove
	bool MG_Entry_To_Landed() const;           // #472 (GroundAnimState==0x1)&&!bShouldMove&&!bIsPlayingAnyMontage&&!bIsGlidingLanded
	bool MG_Entry_To_MainGS() const;           // #473 true (fallback ×10)
	bool MG_Entry_To_VinesOver() const;        // #474 GroundAnimState==0xA
	bool MG_Entry_To_RunOnWallsOver() const;   // #475 GroundAnimState==0xB
	bool MG_Entry_To_LandedStationary() const; // #476 bIsGlidingLanded&&(Velocity2DLength<10)&&(GroundAnimState==0x1)
	bool MG_Entry_To_SprintVinesOver() const;  // #477 GroundAnimState==0xC

	// ---- FromRoll（#478–#479）----
	bool MG_FromRoll_To_MainGS() const;        // #478 true
	bool MG_FromRoll_To_RollToRun() const;     // #479 GetAnimTimeRemainingSafe(410,2)==0

	// ---- LandedStationary（#480–#483）----
	bool MG_LandedStat_To_MainGS_Auto() const; // #480 true
	bool MG_LandedStat_To_StatToMove() const;  // #481 Velocity2DLength>=10
	bool MG_LandedStat_To_LandedMob() const;   // #482 (GroundAnimState==0x1)&&(Velocity2DLength>10)&&!bIsGlidingLanded
	bool MG_LandedStat_To_MainGS() const;      // #483 (GroundAnimState==0x1)&&(Velocity2DLength>10)&&bIsGlidingLanded

	// ---- LandedMobile（#484–#485）----
	bool MG_LandedMob_To_MainGS_Auto() const;  // #484 true
	bool MG_LandedMob_To_MainGS() const;       // #485 true

	// ---- StatToMove（#486–#487）----
	bool MG_StatToMove_To_MainGS_Auto() const; // #486 true
	bool MG_StatToMove_To_MainGS() const;      // #487 !bShouldMove && (GetAnimTimeRemainingSafe(410,5)<0.2)

	// ---- RollToRun（#488–#489）----
	bool MG_RollToRun_To_MainGS_Auto() const;  // #488 true
	bool MG_RollToRun_To_MainGS() const;       // #489 VaultEndToStopL || VaultEndToStopR

	// ---- Landed（#490–#491）----
	bool MG_Landed_To_LandedStat() const;      // #490 (GetAnimTimeRemainingSafe(410,7)==0) && !bShouldMove
	bool MG_Landed_To_StatToMove() const;      // #491 true

	// ---- VaultContinue（#492）----
	bool MG_VaultCont_To_MainGS() const;       // #492 (GroundAnimState==0x0) || (VaultSubState==0x0)

	// ---- VinesOver（#493–#494）----
	bool MG_VinesOver_To_MainGS_Auto() const;  // #493 true
	bool MG_VinesOver_To_MainGS() const;       // #494 GroundAnimState==0x0

	// ---- RunOnWallsOver（#495–#496）----
	bool MG_RunWalls_To_MainGS_Auto() const;   // #495 true
	bool MG_RunWalls_To_MainGS() const;        // #496 (GroundAnimState==0x0) || (GroundAnimState==0x1)

	// ---- Conduit（#497–#498）----
	bool MG_Conduit_To_LandedMob() const;      // #497 true
	bool MG_Conduit_To_Landed() const;         // #498 true

	// ---- SprintVinesOver（#499–#500）----
	bool MG_SprintVines_To_MainGS() const;     // #499 GroundAnimState==0x0
	bool MG_SprintVines_To_MainGS_Auto() const;// #500 true

private:
	/** 安全包装：AnimTimeDelegate 为空时返回大哨兵值（TNumericLimits<float>::Max()）
	    使 `==0` 与 `<0.2` 条件均为 false（阻断过渡，安全降级） */
	float GetAnimTimeRemainingSafe(int32 MachineIndex, int32 StateIndex) const;

	/** 当前帧快照指针（游戏线程写入，worker 线程只读） */
	const FAnimSnapshot* Snap = nullptr;

	/** 移动参数配置（Init 时绑定，生命周期由 AnimInstance 保证） */
	const FLocomotionTuning* Tuning = nullptr;

	/** 动画剩余时间查询代理（Init 时注入，可为空） */
	FAnimTimeRemainingDelegate AnimTimeDelegate;

	/** 降级默认配置（Tuning 传入 nullptr 时使用） */
	static const FLocomotionTuning DefaultTuning;
};
