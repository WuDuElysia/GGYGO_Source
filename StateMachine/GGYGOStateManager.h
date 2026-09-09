/**
 * @file GGYGOStateManager.h
 * @brief 并行叠加状态管理器（阶段 6 核心）— 纯 C++ 实现
 *
 * 替代原来的单一状态机 FCharacterStateMachine。
 * 参考 NTE 的 UHTCharacterStateManagerComponent 设计，但用纯 C++ 实现（零 UObject 开销）。
 *
 * 核心能力：
 * - 多个状态可同时活跃（按组分层：Locomotion / Action / Overlay / System）
 * - DataTable 驱动的 N×N 关系矩阵决定转换规则
 * - Locomotion 组的活跃状态 = PrimaryState（供 AnimBP / UI 读取）
 * - GAS 联动钩子（Phase 7 接线，当前为空操作）
 *
 * 使用方式：
 *   BaseCharacter 构造函数 MakeUnique<FGYGOStateManager>()
 *   BeginPlay 时调 Init(RuntimeData) 注册所有状态 + 加载关系矩阵
 *   Tick 第5步调 Update(DeltaTime) 驱动状态更新
 */
#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "StateMachine/CharacterStateType.h"
#include "Contracts/State/StateUpdateResult.h"
#include "StateMachine/Data/FStateRelationRow.h"

// 前向声明
struct FRuntimeData;
class FCharacterState;
class UAbilitySystemComponent;
class UDataTable;
class UGameplayEffect;

/**
 * 关系检查结果
 * 由 CheckRelations() 返回，告诉 RequestState 该怎么做
 */
struct FRelationCheckResult
{
	/** 是否允许激活新状态 */
	bool bAllowed = true;
	/** 需要中断（Deactivate）的旧状态列表 */
	TArray<FCharacterState*> StatesToInterrupt;
};

/**
 * 并行叠加状态管理器（纯 C++ 类）
 *
 * 生命周期：
 *   MakeUnique()     → 创建实例
 *   Init(RD)         → 注册所有状态实例 + 加载关系矩阵 + 激活 Idle
 *   Update(DT)       → 每帧更新所有 ActiveStates（由 BaseCharacter::Tick 显式调用）
 *   RequestState()   → 外部请求激活新状态（查矩阵 → 激活/中断/拒绝）
 *   ReleaseState()   → 外部请求停用某状态
 *
 * 状态所有权：
 *   AllStates (TUniquePtr) 拥有所有 FCharacterState 实例
 *   ActiveStates (TSet) 存储裸指针，指向 AllStates 中的实例
 *   FGYGOStateManager 销毁时 AllStates 自动回收，ActiveStates 指针随之失效
 *
 * 配置来源（优先级从高到低）:
 *   1. 外部传入的 DataTable 引用（Init 时指定）
 *   2. 内置硬编码默认矩阵（DT 为空时的 fallback）
 */
class FGYGOStateManager
{
public:

	FGYGOStateManager();
	~FGYGOStateManager();

	// ====== 初始化 ======

	/**
	 * 初始化状态管理器
	 * 必须在 BeginPlay 中调用（依赖 RuntimeData 已就绪）
	 * @param InRuntimeData 运行时黑板引用（生命周期由 BaseCharacter 管理）
	 * @param InRelationTable 可选的关系矩阵 DT（null 则使用内置默认矩阵）
	 */
	void Init(FRuntimeData& InRuntimeData, UDataTable* InRelationTable = nullptr);

	/**
	 * 注入 ASC（GAS 联动必须，Phase 7）
	 * 必须在 Init() 之后、状态切换之前调用
	 * @param InASC AbilitySystemComponent（不拥有所有权，由 BaseCharacter 持有）
	 */
	void InitASC(UAbilitySystemComponent* InASC);

	// ====== 帧更新 ======

	/**
	 * 手动触发状态更新（由 BaseCharacter::Tick 第5步调用）。
	 * 兼容旧调用方，结果写入 GetLastUpdateResult()。
	 */
	void Update(float DeltaTime);

	/**
	 * 手动触发状态更新并输出本次状态操作结果。
	 * StateManager 仍在此调用内完成状态规则、生命周期和状态侧副作用；
	 * OutResult 只向 Pipeline 报告已完成的结果，不供 Pipeline 重放状态操作。
	 */
	void Update(float DeltaTime, FStateUpdateResult& OutResult);

	/** 返回最近一次 Update 产生的状态结果。 */
	const FStateUpdateResult& GetLastUpdateResult() const { return LastUpdateResult; }

	// ====== 核心接口 ======

	/**
	 * 请求激活新状态
	 *
	 * 流程：
	 *   1. 查关系矩阵 → 遍历当前 ActiveStates 判断每个 [New, Old] 的关系
	 *   2. 如果有 Interrupted → 先 Deactivate 被中断的状态
	 *   3. 如果有 Blocked → 直接拒绝返回 false
	 *   4. 全部 Independent/Interrupted → ActivateState(New)
	 *
	 * @param NewStateType 要激活的状态类型
	 * @return 是否成功激活（Blocked 时返回 false）
	 */
	bool RequestState(ECharacterStateType NewStateType);

	/**
	 * 请求停用一个状态
	 * @param StateType 要停用的状态类型
	 * @return 是否成功停用（该状态当前未活跃时返回 false）
	 */
	bool ReleaseState(ECharacterStateType StateType);

	// ====== 查询接口 ======

	/** 指定状态类型是否当前活跃 */
	bool IsInState(ECharacterStateType StateType) const;

	/** 指定状态组是否有任何活跃状态 */
	bool IsInGroup(EStateGroup Group) const;

	/**
	 * 获取主状态（PrimaryState）
	 * 定义：Locomotion 组中当前唯一活跃的状态。无则回退 Idle。
	 */
	ECharacterStateType GetPrimaryState() const { return CachedPrimaryState; }

	/** 获取所有当前活跃状态的只读引用 */
	const TSet<FCharacterState*>& GetActiveStates() const { return ActiveStates; }

	/**
	 * 强制切换主状态（不检查规则表）
	 * 仅用于 Death / Stunned 等仲裁层强制打断场景。
	 * 会先 Deactivate 当前 PrimaryState 所在组的全部状态。
	 */
	void ForceSetPrimaryState(ECharacterStateType NewState);

	/**
	 * 获取两个状态之间的关系类型
	 * 优先从缓存读取，未命中时从默认矩阵读取。
	 */
	EStateRelationType GetRelation(ECharacterStateType From, ECharacterStateType To) const;

	/**
	 * 查询指定动作是否被 Tag 允许（Phase 7 输入过滤）
	 * 检查 ASC 中是否存在对应的 Restriction.* Tag
	 *
	 * @param ActionTag 要检查的限制 Tag（如 Restriction.CantAttack）
	 * @return true = 允许执行（没有该限制 Tag），false = 被禁止
	 */
	bool IsActionAllowed(const FGameplayTag& ActionRestrictionTag) const;

	/**
	 * 预判：当前能否切换到目标状态（查关系矩阵，不真正执行）
	 * 用于 ActionArbiter 在 GA 激活前做第二道预判
	 * @param NewState 目标状态类型
	 * @return true = 可以切换（不会被 Blocked / Interrupted 拒绝）
	 */
	bool CanEnterState(ECharacterStateType NewState) const;

private:

	// ====== 内部数据 ======

	/** 运行时黑板引用（不拥有所有权，BaseCharacter 持有）*/
	FRuntimeData* RuntimeData = nullptr;

	/** ASC 引用（不拥有所有权，GAS 联动核心，Phase 7）*/
	UAbilitySystemComponent* ASC = nullptr;

	/** 所有状态实例（拥有所有权）*/
	TMap<ECharacterStateType, TUniquePtr<FCharacterState>> AllStates;

	/** 当前活跃状态集合（裸指针，不拥有所有权）*/
	TSet<FCharacterState*> ActiveStates;

	/** 关系矩阵缓存（From, To）→ RelationType */
	TMap<TPair<ECharacterStateType, ECharacterStateType>, EStateRelationType> RelationMatrixCache;

	/** 缓存的主状态（Locomotion 组的唯一活跃状态）*/
	ECharacterStateType CachedPrimaryState = ECharacterStateType::Idle;

	/** 最近一次状态更新产生的结果；用于兼容无输出参数的 Update 调用。 */
	FStateUpdateResult LastUpdateResult;

	/** 当前正在执行 Update 时的结果接收地址；状态内部请求通过它告知 Pipeline。 */
	FStateUpdateResult* ActiveUpdateResult = nullptr;

	// ====== 内部方法 ======

	/** 注册一个状态实例到 AllStates */
	template<typename TStateClass>
	void RegisterState(ECharacterStateType Type);

	/** 从 DataTable 加载关系到 RelationMatrixCache，或使用内置默认矩阵 */
	void LoadRelationMatrix(UDataTable* Table);

	/** 构建内置硬编码默认关系矩阵 */
	void BuildDefaultRelationMatrix();

	/** 检查新状态与所有当前活跃状态的关系 */
	FRelationCheckResult CheckRelations(ECharacterStateType NewStateType);

	/** 开始收集一次 Update 的状态结果，并让状态请求写入 OutResult。 */
	void BeginUpdateResult(FStateUpdateResult& OutResult);

	/** 完成一次 Update 的状态结果，刷新最终 PrimaryState 并解除结果收集。 */
	void FinishUpdateResult(FStateUpdateResult& OutResult);

	/** 将已完成的状态操作写入当前 Update 结果；不改变状态规则或副作用。 */
	void RecordTransitionEvent(
		EStateTransitionEventType EventType,
		ECharacterStateType RequestedState,
		ECharacterStateType PreviousPrimaryState,
		ECharacterStateType CurrentPrimaryState,
		bool bAccepted,
		const TArray<ECharacterStateType>& InterruptedStates);

	/** 激活一个状态（Enter + 设标记 + 加集合 + 更新 PrimaryState）*/
	void ActivateState(FCharacterState* State);

	/** 停用一个状态（Exit + 清标记 + 移集合 + 更新 PrimaryState）*/
	void DeactivateState(FCharacterState* State);

	/** 根据 ActiveStates 重新计算 CachedPrimaryState */
	void UpdatePrimaryState();

	/** 应用功能限制位掩码到 RuntimeData */
	void ApplyLimitFlags(int32 LimitFlags);

	// ====== ★ GAS 联动内部方法（Phase 7）=====

	/**
	 * 应用状态进入时的 GE 列表
	 * 遍历 State->GetEnterGameplayEffects()，逐个通过 ASC 施加
	 */
	void ApplyEnterGameplayEffects(FCharacterState* State);

	/**
	 * 按标签移除匹配的活跃 GE
	 * 遍历 State->GetRemoveGEsWithTag()，从 ASC 的 ActiveGameplayEffects 中移除匹配项
	 */
	void RemoveGameplayEffectsByTag(FCharacterState* State);
};
