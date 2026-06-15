/**
 * @file CharacterState.h
 * @brief 角色状态基类
 *
 * 每个状态是独立的纯 C++ 类。
 * 只读 RuntimeData 做判断，不直接读输入或调 GAS。
 * Enter / Update / Exit 是状态的生命周期钩子。
 */
#pragma once

#include "CoreMinimal.h"
#include "CharacterStateType.h"

struct FRuntimeData;
class FCharacterStateMachine;

class FCharacterState
{
public:
	explicit FCharacterState(ECharacterStateType InType);
	virtual ~FCharacterState();

	/**
	 * 进入状态时调用
	 * @param RuntimeData 运行时黑板（写仲裁标记等）
	 */ 
	virtual void Enter(FRuntimeData& RuntimeData) {}

	/**
	 * 每帧更新，读取 RuntimeData，通过 SM 发起状态转换
	 * @param DeltaTime   帧间隔
	 * @param RuntimeData 运行时黑板（只读意图、仲裁标记）
	 * @param SM          状态机引用（调用 TryTransitionTo）
	 */
	virtual void Update(float DeltaTime, FRuntimeData& RuntimeData, FCharacterStateMachine& SM) {}

	/**
	 * 退出状态时调用
	 * @param RuntimeData 运行时黑板
	 */
	virtual void Exit(FRuntimeData& RuntimeData) {}

	/** 获取状态类型 */
	ECharacterStateType GetType() const { return StateType; }

protected:
	/** 当前状态类型标识 */
	ECharacterStateType StateType;
};
