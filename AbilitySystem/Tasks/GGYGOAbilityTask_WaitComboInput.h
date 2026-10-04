/** @file GGYGOAbilityTask_WaitComboInput.h @brief 持续接收带来源段序的 GAS 输入事件 */
#pragma once

#include "Abilities/Tasks/AbilityTask.h"
#include "GGYGOAbilityTask_WaitComboInput.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FGGYGOComboInputDelegate, int32, SourceStep, int32, RequestId);
DECLARE_DELEGATE_TwoParams(FGGYGOComboInputNativeDelegate, int32, int32);

/** InputPressed 使用原 ASC/Spec/预测键定位事件桶；桶不是能力激活身份，原激活校验由调用方闭包负责。 */
UCLASS()
class GGYGO_API UGGYGOAbilityTask_WaitComboInput : public UAbilityTask
{
	GENERATED_BODY()
public:
	static UGGYGOAbilityTask_WaitComboInput* WaitComboInput(UGameplayAbility* OwningAbility);
	virtual void Activate() override;
	virtual void OnDestroy(bool AbilityEnded) override;
	void SetSourceStep(int32 InStep) { SourceStep = InStep; }
	/** 可选单播，只允许原 pre-Ready Awaiting 阶段安装；返回精确注销 token。 */
	FDelegateHandle RegisterNativeCallback(FGGYGOComboInputNativeDelegate Callback);
	bool UnregisterNativeCallback(FDelegateHandle Registration);
	UPROPERTY(BlueprintAssignable)
	FGGYGOComboInputDelegate OnPress;
private:
	struct FNativeCallbackRegistration;
	struct FOriginalInputSubscription;
	static UGGYGOAbilityTask_WaitComboInput* RecheckOriginalSubscription(
		const TWeakObjectPtr<UGGYGOAbilityTask_WaitComboInput>& OriginalTask,
		const TSharedPtr<FOriginalInputSubscription>& Subscription);
	void RejectAndEndTask(const TCHAR* Reason, const TSharedPtr<FOriginalInputSubscription>& Subscription);
	void HandlePress();
	TSharedPtr<FNativeCallbackRegistration> NativeCallbackRegistration;
	TSharedPtr<FOriginalInputSubscription> OriginalInputSubscription;
	bool bActivationStarted = false;
	bool bEndingTask = false;
	int32 SourceStep = 0;
	int32 LocalRequestId = 0;
};
