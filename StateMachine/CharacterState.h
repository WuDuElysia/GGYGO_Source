/**
 * @file CharacterState.h
 * @brief 角色状态基类（阶段 6 重构版）
 *
 * 每个状态是独立的纯 C++ 类。
 * 只读 RuntimeData 做判断，不直接读输入或调 GAS。
 *
 * 阶段 6 改动：
 * - 新增 StateGroup 分组字段（Locomotion/Action/Overlay/System）
 * - 新增 GAS 联动钩子（Phase 7 接线，当前返回空默认值）
 * - 新增 MovementDataAsset 钩子（Phase 8 接线）
 * - 新增 bIsActive 标记（并行状态机需要知道每个状态的激活状态）
 */
#pragma once

#include "CoreMinimal.h"
#include "StateMachine/CharacterStateType.h"
#include "GameplayTagContainer.h" // FGameplayTagContainer 完整定义（Phase 7 GAS 接入需要）

// 前向声明：避免 Phase 6 引入 GameplayAbilities 模块依赖
// Phase 7 接入 GAS 时这些类型会自动解析
class UGameplayEffect;
class UDataAsset; // Phase 8 DataAsset 前向声明

struct FRuntimeData;
class FGYGOStateManager; // ★ 阶段6：纯C++ 并行状态管理器

/**
 * 角色状态基类
 *
 * 生命周期：Enter → Update(每帧) → Exit
 * 并行模式下多个 FCharacterState 可同时处于 Active 状态，
 * 通过 StateGroup 区分所属层级。
 */
class FCharacterState
{
public:
	explicit FCharacterState(ECharacterStateType InType, EStateGroup InGroup = EStateGroup::Locomotion);
	virtual ~FCharacterState();

	// ====== 生命周期钩子 ======

	/** 进入状态时调用 */
	virtual void Enter(FRuntimeData& RuntimeData) {}

	/**
	 * 每帧更新
	 * @param DeltaTime   帧间隔
	 * @param RuntimeData 运行时黑板（只读意图、仲裁标记）
	 * @param SM          状态管理器引用（调用 RequestState / ReleaseState）
	 */
	virtual void Update(float DeltaTime, FRuntimeData& RuntimeData, FGYGOStateManager& SM) {}

	/** 退出状态时调用 */
	virtual void Exit(FRuntimeData& RuntimeData) {}

	// ====== 查询接口 ======

	/** 获取状态类型枚举值 */
	ECharacterStateType GetStateType() const { return StateType; }

	/** 获取所属状态组 */
	EStateGroup GetStateGroup() const { return StateGroup; }

	/** 获取状态名称（用于日志/调试）*/
	FName GetStateName() const { return StateName; }

	/** 当前是否处于激活状态（在 ActiveStates 集合中）*/
	bool IsActive() const { return bIsActive; }

	// ====== ★ GAS 联动钩子（Phase 7 接线，当前返回空默认值）=====

	/**
	 * 进入此状态时应施加的 GameplayEffect 类列表
	 * Phase 7: StateManager::ActivateState() 会遍历并调 ASC->ApplyGameplayEffectToSelf()
	 * 示例: HitStun 返回 { GE_BlockCombat }, Dodging 返回 { GE_Invincible, GE_CooldownEvade }
	 */
	virtual TArray<TSubclassOf<UGameplayEffect>> GetEnterGameplayEffects() const { return {}; }

	/**
	 * 进入此状态时应移除的 GE 的 Tag
	 * Phase 7: ActivateState() 会用此 Tag 查询并移除已激活的匹配 GE
	 * 典型用途: 退出闪避时移除 Invincible GE（通过 Tag 匹配而非硬编码 GE 引用）
	 */
	virtual FGameplayTagContainer GetRemoveGEsWithTag() const { return FGameplayTagContainer(); }

	/**
	 * 此状态的功能限制位掩码（NTE 的 LimitGamePlayFun 对应物）
	 * Phase 6~7 过渡方案: 直接写入 RuntimeData.bBlock* 字段
	 * Phase 7 完整方案: 由 GE 的 OwnedTag 驱动，此处仅作为 fallback
	 *
	 * 位定义（可按需扩展）:
	 *   Bit 0: CantMove     Bit 1: CantAttack    Bit 2: CantDodge
	 *   Bit 3: CantJump     Bit 4: CantInput     Bit 5: CantLookInput
	 *   Bit 6: CantInteract Bit 7: ImmuneDamage
	 */
	virtual int32 GetLimitFlags() const { return 0; }

	// ====== ★ 运动参数钩子（Phase 8 接线，当前返回 null）=====

	/**
	 * 此状态下应使用的运动参数 DataAsset
	 * Phase 8: ActivateState() 会调 DA->ApplyToMovementComponent()
	 * 返回 null 表示不改变当前运动参数
	 */
	virtual TSoftObjectPtr<UDataAsset> GetMovementDA() const { return nullptr; }

protected:
	/** 状态类型标识 */
	ECharacterStateType StateType = ECharacterStateType::None;

	/** 所属状态组（决定排他性行为）*/
	EStateGroup StateGroup = EStateGroup::Locomotion;

	/** 状态调试名称 */
	FName StateName = NAME_None;

	/** 是否当前激活（由 StateManager 在 Activate/Deactivate 时设置）*/
	bool bIsActive = false;

	/** 允许 StateManager 访问 protected 成员（设置 bIsActive 等）*/
	friend class FGYGOStateManager;
};
