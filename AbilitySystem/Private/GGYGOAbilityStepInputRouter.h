/**
 * @file GGYGOAbilityStepInputRouter.h
 * @brief ASC 私有协作者：分段输入路由。
 */
#pragma once

#include "Abilities/GameplayAbilityTargetTypes.h"
#include "AbilitySystem/GGYGOAbilityInputRequestTypes.h"
#include "GameplayAbilitySpecHandle.h"
#include "GameplayPrediction.h"

class UGameplayAbility;
class UGGYGOAbilitySystemComponent;
class UGGYGOAbilityTask_WaitComboInput;
class UGGYGOGameplayAbility;
class FGGYGOAbilityActivationHandle;

/**
 * ASC 的分段输入路由。
 *
 * 认证步骤输入请求的原来源（原 Process 边沿、激活、Avatar、Controller、CMC），维护原激活的
 * Initial 运输终态，并处理本项目分段输入 TargetData 的服务器 RPC。原生 RPC 覆盖留在 ASC，
 * 转到这里；需要原生处理时经 ASC 的 ForwardNative* 调回原生实现。
 * 普通 C++ 对象，只在 ASC 调用时执行；没有 Tick、计时器或帧调度。
 */
class FGGYGOAbilityStepInputRouter final
{
public:
	explicit FGGYGOAbilityStepInputRouter(UGGYGOAbilitySystemComponent& InASC);
	FGGYGOAbilityStepInputRouter(const FGGYGOAbilityStepInputRouter&) = delete;
	FGGYGOAbilityStepInputRouter& operator=(const FGGYGOAbilityStepInputRouter&) = delete;

	/** 仅原 Process 的同步调用栈可借用的一次真实边沿；不会保存到等待中的请求。 */
	struct FDispatch
	{
		FGameplayAbilitySpecHandle Spec;
		FGGYGOMovementInputEvaluationToken OriginalEvaluationToken;
		bool bInitial = false;
		uint64 InputRevision = 0;
		bool bDelivering = false;
		/** 只在 ASC 自身 Invoke 窗口认领一次；Super/raw 重入不借原边沿。 */
		mutable bool bClaimed = false;
		mutable bool bNativePressEntered = false;
	};

	/** 在原生按下／激活调用期间登记当前边沿，析构时恢复上一层。 */
	class FScopedDispatch final
	{
	public:
		FScopedDispatch(FGGYGOAbilityStepInputRouter& InRouter, const FDispatch& InDispatch);
		~FScopedDispatch();
		FScopedDispatch(const FScopedDispatch&) = delete;
		FScopedDispatch& operator=(const FScopedDispatch&) = delete;
	private:
		FGGYGOAbilityStepInputRouter& Router;
		const FDispatch& Dispatch;
		const FDispatch* Previous = nullptr;
	};

	const FDispatch* GetActiveDispatch() const { return ActiveDispatch; }

	FGGYGOAbilityInitialInputRoute CaptureInitialInputRoute(UGGYGOGameplayAbility* Ability,
		const FGGYGOAbilityActivationHandle& Original, FString& OutError);
	FGGYGOAbilityStepInputRequest CaptureStepInputRequest(UGGYGOAbilityTask_WaitComboInput* Task,
		UGGYGOGameplayAbility* Ability, EGGYGOAbilityStepInputKind Kind, int32 SourceStep, int32 RequestId,
		FString& OutError);
	bool CheckStepInputRequestCurrent(const FGGYGOAbilityStepInputRequest& Request, FString& OutError) const;
	bool CheckStepInputRequestForTask(const FGGYGOAbilityStepInputRequest& Request,
		const UGameplayAbility* Ability, const FGGYGOAbilityActivationHandle& Activation, FString& OutError) const;
	bool CommitStepInputFact(UGGYGOAbilityTask_WaitComboInput* Task,
		const FGGYGOAbilityStepInputRequest& Request, const FGGYGOMovementInputStepFact& Fact,
		const FGGYGOAbilityInitialInputRoute& Route, FString& OutError);
	void NotifyInitialInputUnavailable(const FGGYGOAbilityInitialInputRoute& Route);
	bool ConsumeInitialInputUnavailable(const FGGYGOAbilityInitialInputRoute& Route,
		const FGGYGOAbilityActivationHandle& Original);

	/** ServerSetReplicatedTargetData：本项目分段输入载荷认证后带原请求转原生；其它载荷直接转原生。 */
	void HandleServerTargetData(FGameplayAbilitySpecHandle AbilityHandle, FPredictionKey AbilityOriginalPredictionKey,
		const FGameplayAbilityTargetDataHandle& TargetData, FGameplayTag ApplicationTag, FPredictionKey CurrentPredictionKey);
	/** ServerSetReplicatedTargetDataCancelled：有原 Initial 路由时由路由处理，否则转原生。 */
	void HandleServerTargetDataCancelled(FGameplayAbilitySpecHandle AbilityHandle,
		FPredictionKey AbilityOriginalPredictionKey, FPredictionKey CurrentPredictionKey);

private:
	FGGYGOAbilityStepInputRequest MakeStepInputRequest(UGGYGOGameplayAbility* Ability,
		UGGYGOAbilityTask_WaitComboInput* Task, EGGYGOAbilityStepInputKind Kind,
		int32 SourceStep, int32 RequestId, bool bRemote, FString& OutError);
	bool CheckInitialInputRoute(const FGGYGOAbilityInitialInputRoute& Route, FString& OutError) const;
	FGGYGOAbilityInitialInputRoute FindInitialInputRoute(FGameplayAbilitySpecHandle Spec, FPredictionKey Key) const;
	void DispatchInitialInputUnavailable(const FGGYGOAbilityInitialInputRoute& Route,
		FPredictionKey CurrentPredictionKey, const TCHAR* Boundary);
	FGGYGOAbilityStepInputRequest WrapRemoteStepInputRequest(FGameplayAbilitySpecHandle Spec,
		FPredictionKey ActivationKey, EGGYGOAbilityStepInputKind Kind, int32 SourceStep, int32 RequestId,
		const FGGYGOMovementInputStepFact& Fact, FString& OutError);

	UGGYGOAbilitySystemComponent& ASC;
	TArray<TWeakPtr<FGGYGOAbilityInitialInputRoute::FRouteProof>> InitialInputRoutes;
	const FDispatch* ActiveDispatch = nullptr;
};
