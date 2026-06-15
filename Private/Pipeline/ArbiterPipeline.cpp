/**
 * @file ArbiterPipeline.cpp
 * @brief 仲裁管线实现
 */
#include "Pipeline/ArbiterPipeline.h"
#include "Pipeline/Arbiters/GASArbiter.h"
#include "Pipeline/Arbiters/ActionArbiter.h"
#include "Pipeline/Arbiters/HealthArbiter.h"
#include "Pipeline/Arbiters/StaminaArbiter.h"
#include "Data/RuntimeData.h"

void FArbiterPipeline::Init(UAbilitySystemComponent* InASC)
{
	// 按固定顺序注册：GAS → Action → Health → Stamina
	auto GAS = MakeUnique<FGASArbiter>();
	GAS->Init(InASC);
	Arbiters.Add(MoveTemp(GAS));

	auto Action = MakeUnique<FActionArbiter>();
	Action->Init(InASC);
	Arbiters.Add(MoveTemp(Action));

	auto Health = MakeUnique<FHealthArbiter>();
	HealthArbiterPtr = Health.Get(); // 保存裸指针给外部调用 RequestDamage
	Health->Init(InASC);
	Arbiters.Add(MoveTemp(Health));

	auto Stamina = MakeUnique<FStaminaArbiter>();
	Stamina->Init(InASC);
	Arbiters.Add(MoveTemp(Stamina));
}

void FArbiterPipeline::Process(FRuntimeData& RuntimeData, float DeltaTime)
{
	// 每帧先重置仲裁标记，由各仲裁器重新写入
	// 帧末只需 ResetFrameIntents 清零意图，仲裁标记由这里的仲裁器维护
	RuntimeData.bBlockMove   = false;
	RuntimeData.bBlockAttack = false;
	RuntimeData.bBlockDodge  = false;
	RuntimeData.bBlockInput  = false;
	RuntimeData.ActionGranted = ECharacterStateType::Idle;

	for (auto& Arbiter : Arbiters)
	{
		Arbiter->Arbitrate(RuntimeData, DeltaTime);
	}
}

FHealthArbiter* FArbiterPipeline::GetHealthArbiter() const
{
	return HealthArbiterPtr;
}
