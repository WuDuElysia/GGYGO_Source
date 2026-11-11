/**
 * @file GGYGOCharacterDeathPolicyTestTypes.h
 * @brief 死亡结束处理的选择与广播顺序测试观察者。
 */
#pragma once

#include "Character/GGYGOCharacterBase.h"
#include "GGYGOCharacterDeathPolicyTestTypes.generated.h"

UCLASS(Transient)
class AGGYGOCharacterDeathPolicyTestPawn : public AGGYGOCharacterBase
{
	GENERATED_BODY()

public:
	bool bKeepPawn = false;
	int32 PolicyCalls = 0;
	bool bPawnIntactDuringBroadcast = false;
	void UseSimulatedRole() { SetRole(ROLE_SimulatedProxy); }
	UFUNCTION()
	void ObserveDeathFinished(AActor* OwningActor);

protected:
	virtual bool K2_HandleDeathFinished_Implementation() override;
};
