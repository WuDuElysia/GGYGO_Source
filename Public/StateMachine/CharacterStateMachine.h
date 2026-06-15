/**
 * @file CharacterStateMachine.h
 * @brief 纯 C++ 状态机
 *
 * 只读 RuntimeData 的意图和仲裁标记，不直接读输入、不直接调 GAS。
 * 状态切换时同步 GameplayTag（通过回调）。
 *
 * 生命周期说明：
 * - States(TUniquePtr) 拥有所有状态实例的所有权
 * - CurrentState 是裸指针，指向 States 中的某个实例，不负责 delete
 * - FCharacterStateMachine 销毁时 States 自动回收，CurrentState 随之失效
 */
#pragma once

#include "CoreMinimal.h"

struct FRuntimeData;
class FCharacterState;
enum class ECharacterStateType : uint8;

class FCharacterStateMachine
{
public:
	FCharacterStateMachine();
	~FCharacterStateMachine();

	/** 初始化，创建所有状态实例并进入 Idle */
	void Init();

	/**
	 * 每帧更新（Tick 第 5 步）
	 * 前置条件: IntentPipeline 和 ArbiterPipeline 已执行
	 */
	void Update(float DeltaTime, FRuntimeData& RuntimeData);

	/**
	 * 尝试切换到目标状态（检查转换规则表）
	 * @param NewState    目标状态类型
	 * @param RuntimeData 运行时黑板
	 * @return 是否切换成功
	 */
	bool TryTransitionTo(ECharacterStateType NewState, FRuntimeData& RuntimeData);

	/**
	 * 强制切换状态（不检查转换规则表）
	 * 用于 HitStun / Stunned / Dead 等仲裁层强制打断
	 */
	void ForceTransitionTo(ECharacterStateType NewState, FRuntimeData& RuntimeData);

	/** 获取当前状态类型 */
	ECharacterStateType GetCurrentStateType() const;

private:
	/** 执行状态切换的内部方法（调用 Exit → 切换 → Enter） */
	void PerformTransition(ECharacterStateType NewState, FRuntimeData& RuntimeData);

	/**
	 * 检查转换规则表
	 * @return 当前状态是否允许切换到目标状态
	 */
	bool IsTransitionAllowed(ECharacterStateType From, ECharacterStateType To) const;

	/**
	 * 全局打断检查（每帧在状态自身逻辑之前执行）
	 * 检查仲裁标记（死亡/眩晕/受击）→ 匹配则强制切换
	 * TODO: 阶段六仲裁管线完成后，根据 RuntimeData 的仲裁标记驱动
	 */
	void CheckGlobalInterrupts(FRuntimeData& RuntimeData);

	/**
	 * 所有状态实例（拥有所有权）
	 * TUniquePtr 保证 FCharacterStateMachine 销毁时自动回收
	 */
	TMap<ECharacterStateType, TUniquePtr<FCharacterState>> States;

	/**
	 * 当前活跃状态（非拥有型指针）
	 * 指向 States 中某个 TUniquePtr 管理的状态实例。
	 * 不负责 delete —— States 随 FCharacterStateMachine 销毁时自动回收。
	 */
	FCharacterState* CurrentState = nullptr;

};
