/**
 * @file InteractingState.cpp
 * @brief 交互状态实现
 */
#include "StateMachine/State/InteractingState.h"
#include "StateMachine/GGYGOStateManager.h"
#include "Data/RuntimeData.h"

FInteractingState::FInteractingState()
	: FCharacterState(ECharacterStateType::Interacting, EStateGroup::System)
{
}

void FInteractingState::Enter(FRuntimeData& RuntimeData)
{
	// TODO: Phase 7 接入后同步 GameplayTag + 应用限制 GE
}

void FInteractingState::Update(float DeltaTime, FRuntimeData& RuntimeData, FGYGOStateManager& SM)
{
	// 交互期间保持当前状态
	// 由外部调用 ReleaseState(Interacting) 来退出
}

void FInteractingState::Exit(FRuntimeData& RuntimeData)
{
}
