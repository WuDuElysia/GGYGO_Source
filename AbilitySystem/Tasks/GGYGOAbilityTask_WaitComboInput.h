/** @file GGYGOAbilityTask_WaitComboInput.h @brief 原步骤请求和真段首事实的单一 owned 运输。 */
#pragma once

#include "Abilities/Tasks/AbilityTask.h"
#include "Abilities/GameplayAbilityTargetTypes.h"
#include "AbilitySystem/GGYGOAbilityInputRequestTypes.h"
#include "GGYGOAbilityTask_WaitComboInput.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FGGYGOComboInputDelegate, int32, SourceStep, int32, RequestId);
DECLARE_DELEGATE_OneParam(FGGYGOComboInputNativeDelegate, const FGGYGOAbilityStepInputRequest&);

/** 网络只运输纯值；本地 owned 封装只由 ASC 接收入口生成，不进入序列化。 */
USTRUCT()
struct GGYGO_API FGGYGOAbilityStepInputTargetData : public FGameplayAbilityTargetData
{
	GENERATED_BODY()
	EGGYGOAbilityStepInputKind Kind = EGGYGOAbilityStepInputKind::Initial;
	int32 SourceStep = INDEX_NONE;
	int32 RequestId = 0;
	FGGYGOMovementInputStepFact StepFact;
	virtual UScriptStruct* GetScriptStruct() const override { return StaticStruct(); }
	bool NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess);
	bool HasValidFields() const;
private:
	FGGYGOAbilityStepInputRequest OwnedRequest;
	friend class UGGYGOAbilitySystemComponent;
	friend class UGGYGOAbilityTask_WaitComboInput;
};

template<> struct TStructOpsTypeTraits<FGGYGOAbilityStepInputTargetData>
	: TStructOpsTypeTraitsBase2<FGGYGOAbilityStepInputTargetData>
{ enum { WithNetSerializer = true, WithCopy = true }; };

/** 本地 InputPressed 形成原意图；远端只消费原 Spec/激活键的 owned TargetData。GA 管步骤裁决与清理。 */
UCLASS()
class GGYGO_API UGGYGOAbilityTask_WaitComboInput : public UAbilityTask
{
	GENERATED_BODY()
public:
	static UGGYGOAbilityTask_WaitComboInput* WaitComboInput(UGameplayAbility* OwningAbility);
	static UGGYGOAbilityTask_WaitComboInput* WaitComboInput(UGameplayAbility* OwningAbility,
		const FGGYGOAbilityInitialInputRoute& InitialRoute);
	virtual void Activate() override;
	virtual void OnDestroy(bool AbilityEnded) override;
	void SetSourceStep(int32 InStep) { SourceStep = InStep; }
	/** owned 请求的必需 native 消费者，只允许原 pre-Ready 安装，返回精确注销 token。 */
	FDelegateHandle RegisterNativeCallback(FGGYGOComboInputNativeDelegate Callback,
		FSimpleDelegate InitialUnavailable);
	bool UnregisterNativeCallback(FDelegateHandle Registration);
	/** 真段首事实已就绪后由请求资源调用；不提前采样或按本地 Gate 筛发送。 */
	bool SubmitEntryFact(const FGGYGOAbilityStepInputRequest& Request,
		const FGGYGOMovementInputStepFact& Fact, FString& OutError);
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
	void HandleTargetData(const FGameplayAbilityTargetDataHandle& Data, FGameplayTag ApplicationTag);
	void HandleInitialUnavailable();
	void DispatchRequest(const FGGYGOAbilityStepInputRequest& Request,
		const TSharedPtr<FOriginalInputSubscription>& Subscription);
	TSharedPtr<FNativeCallbackRegistration> NativeCallbackRegistration;
	TSharedPtr<FOriginalInputSubscription> OriginalInputSubscription;
	TSharedPtr<const void> OriginalActivationIdentity;
	FGGYGOAbilityInitialInputRoute InitialInputRoute;
	bool bActivationStarted = false;
	bool bEndingTask = false;
	int32 SourceStep = 0;
	int32 LocalRequestId = 0;
	/** 仅保证该原 Task 的运输顺序；步骤许可与初始化资源仍由 GA 管理。 */
	int32 LastRemoteRequestId = 0;
	bool bInitialReceived = false;
};
