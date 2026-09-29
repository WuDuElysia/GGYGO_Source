/** @file GGYGOAbilityTask_WaitComboInput.h @brief 持续接收带来源段序的 GAS 输入事件 */
#pragma once

#include "Abilities/Tasks/AbilityTask.h"
#include "GGYGOAbilityTask_WaitComboInput.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FGGYGOComboInputDelegate, int32, SourceStep, int32, RequestId);

/** InputPressed 通过 GAS 原始预测键定位激活；负载防止延迟按键误用于另一段。 */
UCLASS()
class GGYGO_API UGGYGOAbilityTask_WaitComboInput : public UAbilityTask
{
	GENERATED_BODY()
public:
	static UGGYGOAbilityTask_WaitComboInput* WaitComboInput(UGameplayAbility* OwningAbility);
	virtual void Activate() override;
	virtual void OnDestroy(bool AbilityEnded) override;
	void SetSourceStep(int32 InStep) { SourceStep = InStep; }
	UPROPERTY(BlueprintAssignable)
	FGGYGOComboInputDelegate OnPress;
private:
	void HandlePress();
	FDelegateHandle InputHandle;
	int32 SourceStep = 0;
	int32 LocalRequestId = 0;
};
