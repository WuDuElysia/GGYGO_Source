/**
 * @file CharacterStateMachine.cpp
 * @brief 状态机实现
 */
#include "StateMachine/CharacterStateMachine.h"
#include "StateMachine/CharacterState.h"
#include "StateMachine/State/IdleState.h"
#include "StateMachine/State/RunStartState.h"
#include "StateMachine/State/RunLoopState.h"
#include "StateMachine/State/RunEndState.h"
#include "StateMachine/State/InAirState.h"
#include "StateMachine/State/AttackingState.h"
#include "StateMachine/State/DodgingState.h"
#include "StateMachine/State/HitStunState.h"
#include "StateMachine/State/StunnedState.h"
#include "StateMachine/State/DeadState.h"
#include "Data/RuntimeData.h"

FCharacterStateMachine::FCharacterStateMachine()
{
}

FCharacterStateMachine::~FCharacterStateMachine()
{
}

void FCharacterStateMachine::Init()
{
	// 创建所有状态实例（TUniquePtr 持有所有权）
	// 注意：Init() 可重复调用（角色出池时），TMap::Add 不检查重复
	States.Add(ECharacterStateType::Idle,         MakeUnique<FIdleState>());
	States.Add(ECharacterStateType::RunStart,     MakeUnique<FRunStartState>());
	States.Add(ECharacterStateType::RunLoop,      MakeUnique<FRunLoopState>());
	States.Add(ECharacterStateType::RunEnd,       MakeUnique<FRunEndState>());
	States.Add(ECharacterStateType::InAir,        MakeUnique<FInAirState>());
	States.Add(ECharacterStateType::Attacking,    MakeUnique<FAttackingState>());
	States.Add(ECharacterStateType::Dodging,      MakeUnique<FDodgingState>());
	States.Add(ECharacterStateType::HitStun,      MakeUnique<FHitStunState>());
	States.Add(ECharacterStateType::Stunned,      MakeUnique<FStunnedState>());
	States.Add(ECharacterStateType::Dead,         MakeUnique<FDeadState>());

	// 默认进入 Idle
	// CurrentState 是裸指针，指向 States 里 TUniquePtr 管理的实例
	// 不负责 delete，States 销毁时自动回收
	CurrentState = States[ECharacterStateType::Idle].Get();
}

void FCharacterStateMachine::Update(float DeltaTime, FRuntimeData& RuntimeData)
{
	if (!CurrentState) return;

	// 1. 全局打断检查（仲裁标记驱动，优先于状态自身逻辑）
	// 由 ArbiterPipeline 写入的 bBlockInput / bBlockMove 触发强制切换
	CheckGlobalInterrupts(RuntimeData);

	// 2. 当前状态自身逻辑（读 RuntimeData 意图 + 仲裁标记，尝试转换）
	CurrentState->Update(DeltaTime, RuntimeData, *this);
}

void FCharacterStateMachine::CheckGlobalInterrupts(FRuntimeData& RuntimeData)
{
	const ECharacterStateType Current = GetCurrentStateType();

	// 死亡最高优先级：全面阻断（bBlockInput=true）→ 强制切到 Dead
	if (RuntimeData.bBlockInput && Current != ECharacterStateType::Dead)
	{
		ForceTransitionTo(ECharacterStateType::Dead, RuntimeData);
		return;
	}

	// 眩晕：阻止移动和攻击（bBlockMove=true）→ 强制切到 Stunned
	// 注意：Dead 不再被眩晕覆盖（上面已拦截），避免 Dead → Stunned 回退
	if (RuntimeData.bBlockMove && Current != ECharacterStateType::Stunned
	    && Current != ECharacterStateType::Dead)
	{
		ForceTransitionTo(ECharacterStateType::Stunned, RuntimeData);
		return;
	}
}

bool FCharacterStateMachine::TryTransitionTo(ECharacterStateType NewState, FRuntimeData& RuntimeData)
{
	if (!CurrentState) return false;

	// 不允许切换到相同状态
	if (CurrentState->GetType() == NewState) return false;

	// 检查转换规则表
	if (!IsTransitionAllowed(CurrentState->GetType(), NewState)) return false;

	PerformTransition(NewState, RuntimeData);
	
	return true;
}

void FCharacterStateMachine::ForceTransitionTo(ECharacterStateType NewState, FRuntimeData& RuntimeData)
{
	if (CurrentState && CurrentState->GetType() == NewState) return;
	PerformTransition(NewState, RuntimeData);
}

void FCharacterStateMachine::PerformTransition(ECharacterStateType NewState, FRuntimeData& RuntimeData)
{
	// 退出当前状态
	if (CurrentState)
	{
		CurrentState->Exit(RuntimeData);
	}

	// 进入新状态
	auto* Found = States.Find(NewState);
	if (Found)
	{
		CurrentState = Found->Get();
		CurrentState->Enter(RuntimeData);

		// 写回 RuntimeData，供动画蓝图、UI 等只读
		RuntimeData.CurrentState = NewState;
	}
}

ECharacterStateType FCharacterStateMachine::GetCurrentStateType() const
{
	return CurrentState ? CurrentState->GetType() : ECharacterStateType::Idle;
}

bool FCharacterStateMachine::IsTransitionAllowed(ECharacterStateType From, ECharacterStateType To) const
{
	// 转换规则表（参考 Architecture.md 第五节）
	// HitStun / Stunned / Dead 走 ForceTransition，不经过这里
	switch (From)
	{
	case ECharacterStateType::Idle:
		return To == ECharacterStateType::RunStart
			|| To == ECharacterStateType::InAir
			|| To == ECharacterStateType::Attacking
			|| To == ECharacterStateType::Dodging
			|| To == ECharacterStateType::Interacting;

	case ECharacterStateType::RunStart:
		return To == ECharacterStateType::RunLoop
			|| To == ECharacterStateType::RunEnd
			|| To == ECharacterStateType::InAir
			|| To == ECharacterStateType::Attacking
			|| To == ECharacterStateType::Dodging;

	case ECharacterStateType::RunLoop:
		return To == ECharacterStateType::RunEnd
			|| To == ECharacterStateType::InAir
			|| To == ECharacterStateType::Attacking
			|| To == ECharacterStateType::Dodging;

	case ECharacterStateType::RunEnd:
		return To == ECharacterStateType::RunStart
			|| To == ECharacterStateType::Idle;

	case ECharacterStateType::InAir:
		return To == ECharacterStateType::Idle
			|| To == ECharacterStateType::RunStart
			|| To == ECharacterStateType::Attacking;

	case ECharacterStateType::Attacking:
		return To == ECharacterStateType::Idle
			|| To == ECharacterStateType::RunStart
			|| To == ECharacterStateType::Dodging
			|| To == ECharacterStateType::Attacking;

	case ECharacterStateType::Dodging:
		return To == ECharacterStateType::Idle
			|| To == ECharacterStateType::RunStart;

	case ECharacterStateType::HitStun:
		return To == ECharacterStateType::Idle
			|| To == ECharacterStateType::HitStun
			|| To == ECharacterStateType::Stunned;

	case ECharacterStateType::Stunned:
		return To == ECharacterStateType::Idle;

	case ECharacterStateType::Dead:
		return false; // 终态，不允许任何转换

	default:
		return false;
	}
}
