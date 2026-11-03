/**
 * @file GGYGOAbilityInputBuffer.h
 * @brief ASC 私有协作者：能力输入缓冲。
 */
#pragma once

#include "AbilitySystem/GGYGOAbilityInputRequestTypes.h"
#include "GameplayAbilitySpecHandle.h"
#include "GameplayTagContainer.h"
#include "UObject/WeakObjectPtrTemplates.h"

class AActor;
class UGameplayAbility;
class UGGYGOAbilitySystemComponent;

/**
 * ASC 的能力输入缓冲。
 *
 * ASC 发行输入请求身份，首次精确匹配 InputTag 并固定 Spec 集合、原截止与来源会话；
 * 请求只靠"Revision 递增＋清空记录"失效（ActorInfo 端点变化、ClearActorInfo 与输入阻断都走 Clear）。
 * 同 Spec 任一原来源按住即保持；只缓存首按／末真实释放边沿，Invalidated 不伪造释放。
 * ProcessAbilityInput 沿边沿顺序消费，再处理精确重试与按住，每帧每 Spec 最多一次 Try。
 *
 * 普通 C++ 对象，由 ASC 持有，只在 ASC 调用时执行：没有 Tick、计时器或帧调度。
 * 通过 ASC 的 friend 访问原生 Spec 表与激活入口，不复制原生状态。
 */
class FGGYGOAbilityInputBuffer final
{
public:
	explicit FGGYGOAbilityInputBuffer(UGGYGOAbilitySystemComponent& InASC);
	FGGYGOAbilityInputBuffer(const FGGYGOAbilityInputBuffer&) = delete;
	FGGYGOAbilityInputBuffer& operator=(const FGGYGOAbilityInputBuffer&) = delete;

	FGGYGOAbilityInputRequestResult ReceiveAbilityInputRequest(const FGameplayTag& InputTag,
		const FGGYGOAbilityInputRequestIdentity& PreviousIdentity, double OriginalDeadline);
	FGGYGOAbilityInputRequestResult ReceiveAbilityInputRequest(const FGameplayTag& InputTag,
		const FGGYGOAbilityInputRequestIdentity& PreviousIdentity, double OriginalDeadline,
		const FGGYGOMovementInputEvaluationToken& OriginalEvaluationToken, const FGGYGOAbilityInputSourceKey& Source);
	FGGYGOAbilityInputRequestResult EndAbilityInputRequest(const FGGYGOAbilityInputRequestIdentity& Identity,
		EGGYGOAbilityInputRequestEndKind EndKind);
	void EndAbilityInputRequestsFromSource(const FGGYGOAbilityInputSourceKey& Source);
	void ProcessAbilityInput(float DeltaTime, bool bGamePaused);
	/** 同步纯容器写入：清空请求、边沿、重试、按住并递增 Revision；不广播、不调用 GA／BP。 */
	void ClearAbilityInput();
	/** 组释放：把已有的真实 Queued 许可排入下一次 Process；不在广播栈中激活。 */
	void WakeQueuedAbilityInputRequests();
	/** 失去控制：对按住的活动 Spec 派发一次真实释放，再 ClearAbilityInput。见 ASC 同名入口。 */
	void ReleaseHeldInputForControlLoss();
	/** Handle 是当前输入 Try 的 Spec，且本次结果尚未被激活或失败通知认领。 */
	bool IsInputActivationAttempt(FGameplayAbilitySpecHandle Handle) const
	{
		return CurrentInputAttempt && !CurrentInputAttempt->bClaimed && CurrentInputAttempt->Origin.Handle == Handle;
	}

	enum class EAbilityActivationInputOrigin : uint8
	{
		None,
		InitialPress,
		FiniteRetry
	};

	struct FAbilityActivationFailureOrigin
	{
		EAbilityActivationInputOrigin Kind = EAbilityActivationInputOrigin::None;
		FGameplayAbilitySpecHandle Handle;
		TArray<FGGYGOAbilityInputRetryRequest> Requests;
		TWeakObjectPtr<AActor> OwnerActor;
	};

	/** 一次失败通知认领的输入来源，以及在 Super 之前已登记重试许可的请求。 */
	struct FQueuedFailure
	{
		FAbilityActivationFailureOrigin Origin;
		TArray<FGGYGOAbilityInputRetryRequest> Notices;
	};

	/** NotifyAbilityActivated：本次输入尝试的 Spec 已激活，之后同 Spec 的失败不再归入本输入来源。 */
	void NotifyAbilityActivated(FGameplayAbilitySpecHandle Handle);
	/** NotifyAbilityFailed 在 Super 之前：认领输入来源，真实组排队或换人回场待定失败时登记重试许可。 */
	FQueuedFailure ClaimQueuedFailure(FGameplayAbilitySpecHandle Handle, const FGameplayTagContainer& FailureTags,
		const TWeakObjectPtr<UGameplayAbility>& FailedAbility);
	/** NotifyAbilityFailed 在 Super 之后：许可仍有效的请求广播 OnAbilityInputRetryable。 */
	void BroadcastQueuedFailure(FGameplayAbilitySpecHandle Handle, const FQueuedFailure& Failure,
		const TWeakObjectPtr<UGameplayAbility>& FailedAbility);

	uint64 GetInputRevision() const { return AbilityInputRevision; }
	/** 分段输入：仍存在以该评价 Token 首按、且仍匹配 Spec 的原请求。 */
	bool HasLiveRequestForEdge(const FGGYGOMovementInputEvaluationToken& Edge, FGameplayAbilitySpecHandle Spec) const;

private:
	/** 唯一请求／按住权威；Spec 在首次接受时固定。不保存 Owner/Avatar/World 快照。 */
	struct FAbilityInputRequestRecord
	{
		FGGYGOAbilityInputRetryRequest Request;
		FGGYGOAbilityInputSourceKey Source;
		TArray<FGameplayAbilitySpecHandle> SpecHandles;
		TArray<FGameplayAbilitySpecHandle> RetryableSpecHandles;
		/** 首次失败通知的顺序；本次等待被一次组释放取走后归零。 */
		uint64 QueuedWaitOrder = 0;
		bool bHeld = true;
	};

	/** 有序的聚合边沿事实，不是另一份按住权威。 */
	struct FAbilityInputEdge
	{
		FGameplayAbilitySpecHandle Handle;
		uint64 InputRevision = 0;
		bool bPressed = false;
		TArray<FGGYGOAbilityInputRequestIdentity> Sources;
		FGGYGOMovementInputEvaluationToken OriginalEvaluationToken;
	};

	/** ProcessAbilityInput 一次 Try 的输入来源；只活在该 Try 的同步调用栈内。 */
	struct FAbilityInputActivationAttempt
	{
		FAbilityActivationFailureOrigin Origin{};
		/** 同 Spec 的第一次激活或失败通知认领后，嵌套 Try 的结果不再归入本输入来源。 */
		bool bClaimed = false;
	};

	class FScopedAbilityInputActivation final
	{
	public:
		FScopedAbilityInputActivation(FGGYGOAbilityInputBuffer& InBuffer, const FAbilityActivationFailureOrigin& Origin);
		~FScopedAbilityInputActivation();
		FScopedAbilityInputActivation(const FScopedAbilityInputActivation&) = delete;
		FScopedAbilityInputActivation& operator=(const FScopedAbilityInputActivation&) = delete;
	private:
		FGGYGOAbilityInputBuffer& Buffer;
		FAbilityInputActivationAttempt* Previous = nullptr;
		FAbilityInputActivationAttempt Attempt{};
	};

	FGGYGOAbilityInputRequestResult MakeAbilityInputRequestFailure(
		EGGYGOAbilityInputRequestOutcome Outcome, EGGYGOAbilityInputRequestReason Reason,
		const FGGYGOAbilityInputRequestIdentity& Identity, FGameplayTag Tag, double Deadline) const;
	/** 请求是否仍有效：本 ASC、当前 Revision、记录存在且身份相等。 */
	const FAbilityInputRequestRecord* FindAbilityInputRequest(const FGGYGOAbilityInputRequestIdentity& Identity) const;
	/** 当前 ActorInfo 的 Owner/Avatar/World 是否可用于输入；Receive 与 Process 入口各查一次。 */
	bool IsAbilityInputActorInfoUsable() const;
	/** 记录冻结的 Spec 仍存在、未待移除，且 Spec 动态 Tag 仍含输入 Tag。 */
	bool RequestMatchesSpec(const FAbilityInputRequestRecord& Record, FGameplayAbilitySpecHandle Handle) const;
	/** 截止时间有限且当前世界时间未到截止。 */
	bool IsBeforeDeadline(const FAbilityInputRequestRecord& Record) const;
	bool IsAbilityInputEdgeCurrent(const FAbilityInputEdge& Edge) const;
	void RebuildAbilityInputHeldHandles();
	void PruneAbilityInputRequests();
	/** 组释放唤醒与失败重排共用的精确入队；失败只作返回值，不记日志。 */
	FGGYGOAbilityInputRequestResult QueueAbilityInputRetry(const FGGYGOAbilityInputRetryRequest& Request);
	void RecordAbilityInputQueuedWait(FAbilityInputRequestRecord& Record);
	void ConsumeSuccessfulAbilityInputRequests(const TArray<FGGYGOAbilityInputRetryRequest>& Requests);
	bool IsAbilityActivationFailureOriginCurrent(const FAbilityActivationFailureOrigin& Origin) const;

	UGGYGOAbilitySystemComponent& ASC;
	/** 由请求记录派生；截止时间到达不释放按住。 */
	TArray<FGameplayAbilitySpecHandle> InputHeldSpecHandles;
	TMap<uint64, FAbilityInputRequestRecord> AbilityInputRequests;
	uint64 LastAbilityInputRequestSerial = 0; // 不重置，也不借用评价序号。
	uint64 LastAbilityInputWaitOrder = 0; // 只表达顺序；等待集合被取走或清空时归零。
	TArray<FAbilityInputEdge> PendingAbilityInputEdges;
	TArray<FGGYGOAbilityInputRetryRequest> PendingAbilityInputRetries;
	FAbilityInputActivationAttempt* CurrentInputAttempt = nullptr;
	uint64 AbilityInputRevision = 0;
	bool bProcessingAbilityInput = false;
	/**
	 * 有请求因 State.SwitchReturnPending 登记了重试许可。标签可能由复制移除，没有同步回调；
	 * ProcessAbilityInput 入口发现标签已移除时唤醒一次。ClearAbilityInput 清零。
	 */
	bool bAwaitingSwitchReturn = false;
};
