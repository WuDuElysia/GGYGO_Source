/** @file GGYGOAbilityInputRequestTypes.h @brief 原输入请求、步骤运输身份与受验事实资源。 */
#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Input/GGYGOMovementInputTypes.h"
#include "UObject/WeakObjectPtrTemplates.h"

class UGGYGOAbilitySystemComponent;
class UGGYGOAbilityTask_WaitComboInput;
class UGGYGOCharacterMovementComponent;
class UGGYGOGameplayAbility;
struct FGGYGOMovementOwnerSyncScopeId;

/** 首次激活和真实续击共享运输通路，但不是同一种输入请求。 */
enum class EGGYGOAbilityStepInputKind : uint8 { Initial, Continue };

/** 原激活的 Initial 运输终态；Task 尚未创建也能归还，不能取消已经提交的招式。 */
class GGYGO_API FGGYGOAbilityInitialInputRoute
{
public:
	bool HasRoute() const { return Proof.IsValid(); }
	bool HasSubmittedInitial() const;
	bool HasSameRoute(const FGGYGOAbilityInitialInputRoute& Other) const
	{ return Proof.IsValid() && Proof == Other.Proof; }
	/** 仅原预测 owner 的未提交 Initial 发一次原生取消；其它终态正常不发送。 */
	void NotifyInitialUnavailable() const;
private:
	struct FRouteProof;
	TSharedPtr<FRouteProof> Proof;
	friend class FGGYGOAbilityStepInputRouter;
	friend class UGGYGOAbilityTask_WaitComboInput;
};

/**
 * 原能力步骤的输入请求资源。ASC 独占来源认证；Task 独占该请求的一次发送。
 * 本地请求保留原 edge，真入场时才取当前评价；远端只读原 owned RPC 封装的事实。
 * 准入查询仅在 GameThread 使用。默认无来源；复制不授移动或重放许可，
 * 也不延长 Actor/Task/输入会话生命期。
 */
class GGYGO_API FGGYGOAbilityStepInputRequest
{
public:
	bool HasRequest() const { return Proof.IsValid(); }
	bool HasSubmittedEntryFact() const;
	bool IsRemote() const;
	EGGYGOAbilityStepInputKind GetKind() const;
	int32 GetSourceStep() const;
	int32 GetRequestId() const;
	FGGYGOMovementInputEvaluationToken GetOriginalEdgeToken() const;
	bool HasSameRequest(const FGGYGOAbilityStepInputRequest& Other) const
	{ return Proof.IsValid() && Proof == Other.Proof; }
	/** 只核原激活/Avatar/CMC/scope，不授步骤或移动许可。 */
	bool IsCurrentFor(const UGGYGOCharacterMovementComponent* OriginalMovement,
		const FGGYGOMovementOwnerSyncScopeId& OriginalScope, FString& OutError) const;
	/** 不读取 Axis/Yaw；远端和已封存请求只核原 owned 资源。 */
	EGGYGOMovementInputEvaluationState QueryStepStartInputReadiness(FString& OutError) const;
	/** 本地未提交时查询真段首；已提交/远端只读同一事实，不重采替换。 */
	EGGYGOMovementInputEvaluationState QueryStepStartInputFact(
		FGGYGOMovementInputStepFact& OutFact, FString& OutError) const;
	/** 本地原 Task 可靠提交一次；远端和重复提交明确拒绝。 */
	bool SubmitStepStartInputFact(const FGGYGOMovementInputStepFact& Fact, FString& OutError) const;
	/** 入场前核同一事实/来源；成功后的动作不逐帧借此重授资格。 */
	bool ValidateStepStartInputFact(const FGGYGOMovementInputStepFact& Fact, FString& OutError) const;

private:
	struct FRequestProof;
	TSharedPtr<FRequestProof> Proof;
	friend class FGGYGOAbilityStepInputRouter;
	friend class UGGYGOAbilityTask_WaitComboInput;
};

/**
 * 输入请求身份的值副本；身份分配和真实输入来源归原 ASC 所有。
 * 弱引用不延长 ASC 生命周期；本类型不维护 held 或运行有效性。
 */
struct FGGYGOAbilityInputRequestIdentity
{
	TWeakObjectPtr<UGGYGOAbilitySystemComponent> SourceASC;
	uint64 InputRevision = 0;
	uint64 RequestSerial = 0;

	/** 仅表示 serial 已分配；revision 0 合法，不证明来源或请求当前有效。 */
	bool IsAssigned() const
	{
		return RequestSerial != 0;
	}

	bool operator==(const FGGYGOAbilityInputRequestIdentity& Other) const
	{
		// 保留失效弱引用的原对象身份，不能把两个 Get() == nullptr 当同一来源。
		return SourceASC.HasSameIndexAndSerialNumber(Other.SourceASC)
			&& InputRevision == Other.InputRevision
			&& RequestSerial == Other.RequestSerial;
	}

	bool operator!=(const FGGYGOAbilityInputRequestIdentity& Other) const
	{
		return !(*this == Other);
	}
};

/**
 * 签发输入请求的来源会话（如 Hero 的一次输入绑定）。ASC 把它记在每条请求上，
 * 来源退役时按它一次结束该会话签发的全部请求。Owner 为空表示没有来源会话；
 * 比较保留失效弱引用的原对象身份。
 */
struct FGGYGOAbilityInputSourceKey
{
	TWeakObjectPtr<const UObject> Owner;
	uint64 Generation = 0;

	bool IsSet() const { return !Owner.IsExplicitlyNull(); }

	bool operator==(const FGGYGOAbilityInputSourceKey& Other) const
	{
		return Owner.HasSameIndexAndSerialNumber(Other.Owner) && Generation == Other.Generation;
	}
};

/** 仅携带原请求与原截止的值副本，不执行入队、激活或期限解析。 */
struct FGGYGOAbilityInputRetryRequest
{
	FGameplayTag InputTag;
	FGGYGOAbilityInputRequestIdentity Identity;

	/**
	 * 原请求在 World::GetTimeSeconds 时间域的绝对截止。
	 * 默认 -1.0 表示无效截止；无效值不得借用近期同 Tag 截止或重新起算。
	 */
	double OriginalDeadline = -1.0;
	/** 首次真实边沿的来源；重试和 Deferred 只复制，不改签当前评价。 */
	FGGYGOMovementInputEvaluationToken OriginalEvaluationToken;
};

/** Cache-operation outcome only; never an ability activation result. */
enum class EGGYGOAbilityInputRequestOutcome : uint8
{
	Accepted,
	AlreadyApplied,
	Rejected,
	Stale
};

/** 仅用于诊断，调用方按 Outcome 分支；只列仍可能产生的原因。 */
enum class EGGYGOAbilityInputRequestReason : uint8
{
	None,
	InvalidRequest,
	InvalidTag,
	InvalidDeadline,
	InvalidPreviousIdentity,
	InvalidASC,
	WrongASC,
	WrongRevision,
	UnknownRequest,
	RequestEnded,
	RequestMismatch,
	ActorInfoUnavailable,
	InputBlocked,
	NoMatchingSpec,
	Expired,
	NoQueuedAdmission,
	NotHeld
};

/** Invalidated is resource retirement, not a physical release. */
enum class EGGYGOAbilityInputRequestEndKind : uint8
{
	Released,
	Invalidated
};

struct FGGYGOAbilityInputRequestResult
{
	EGGYGOAbilityInputRequestOutcome Outcome = EGGYGOAbilityInputRequestOutcome::Rejected;
	EGGYGOAbilityInputRequestReason Reason = EGGYGOAbilityInputRequestReason::InvalidRequest;
	FGGYGOAbilityInputRequestIdentity Identity;
};
