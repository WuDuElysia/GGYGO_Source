#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "UObject/WeakObjectPtrTemplates.h"

class UObject;
class UGGYGOPlayerInput;
struct FGGYGOMovementInputEvaluationProof;

/** 原 PlayerInput 发行的会话身份值；序列 0 未分配，弱引用不延长来源生命期。 */
struct FGGYGOMovementInputSessionIdentity
{
	TWeakObjectPtr<UObject> Producer;
	uint64 SessionSerial = 0;

	/** 比较原对象身份和发行序列，不证明会话当前有效。 */
	bool operator==(const FGGYGOMovementInputSessionIdentity& Other) const
	{
		return Producer.HasSameIndexAndSerialNumber(Other.Producer)
			&& SessionSerial == Other.SessionSerial;
	}

	bool operator!=(const FGGYGOMovementInputSessionIdentity& Other) const
	{
		return !(*this == Other);
	}
};

/** PlayerInput 发行的真实输入请求身份；与 CMC 执行请求编号分开。 */
struct FGGYGOMovementInputRequestIdentity
{
	FGGYGOMovementInputSessionIdentity Session;
	uint64 RequestSerial = 0;

	bool operator==(const FGGYGOMovementInputRequestIdentity& Other) const
	{
		return Session == Other.Session && RequestSerial == Other.RequestSerial;
	}

	bool operator!=(const FGGYGOMovementInputRequestIdentity& Other) const
	{
		return !(*this == Other);
	}
};

/** Read-only qualification of the original Source request; never CMC admission or execution. */
enum class EGGYGOMovementInputRequestQueryResult : uint8
{
	/** The original session, route or attached receiver is unavailable. */
	Unavailable = 0,
	/** An allocated original request is still proven physically Held. */
	Held,
	/** Real Neutral with no active or unresolved request. */
	NotHeld,
	/** Original assembly is live, but observation/press/release proof is not sufficient. */
	AwaitingPhysicalProof
};

/** 原评价是否真正完成；完成只证明 edge 来源，不授移动或能力执行许可。 */
enum class EGGYGOMovementInputEvaluationState : uint8
{
	Invalidated = 0,
	Waiting,
	Completed
};

/** 原 edge 持有的只读完成凭据；内容由 Input 私有，副本延长凭据而非 UObject 的寿命。 */
struct GGYGO_API FGGYGOMovementInputEvaluationToken
{
	bool IsSet() const { return Proof.IsValid(); }
	/** 弱来源身份不授有效性；仍须查询原 Proof，并核消费方的 Avatar/PlayerInput。 */
	FGGYGOMovementInputSessionIdentity GetSourceSession() const;
	bool operator==(const FGGYGOMovementInputEvaluationToken& Other) const { return Proof == Other.Proof; }
	bool operator!=(const FGGYGOMovementInputEvaluationToken& Other) const { return !(*this == Other); }

private:
	friend class UGGYGOPlayerInput;
	TSharedPtr<const FGGYGOMovementInputEvaluationProof> Proof;
};

/** 一次真实段首的输入值；原 edge 与段首评价分开，网络不传本地 Proof 或弱 UObject。 */
struct GGYGO_API FGGYGOMovementInputStepFact
{
	uint64 SourceSessionSerial = 0;
	uint64 OriginalEdgeEvaluationSerial = 0;
	uint64 StepEvaluationSerial = 0;
	EGGYGOMovementInputRequestQueryResult InputState = EGGYGOMovementInputRequestQueryResult::Unavailable;
	FVector2D ActionValue = FVector2D::ZeroVector;
	float ControlYaw = 0.0f;
};

/** 只通知原自然评价完成或来源退役；消费者在自己的段首资源中重新查询事实。 */
DECLARE_DELEGATE_TwoParams(FGGYGOMovementInputEvaluationDelegate,
	EGGYGOMovementInputEvaluationState, const FString&);

/** CMC 发行的消费绑定身份；原来源会话也是绑定身份的一部分。 */
struct FGGYGOMovementInputConsumerBindingId
{
	TWeakObjectPtr<UObject> Consumer;
	uint64 ConsumerBindingSerial = 0;
	FGGYGOMovementInputSessionIdentity SourceSession;

	bool operator==(const FGGYGOMovementInputConsumerBindingId& Other) const
	{
		return Consumer.HasSameIndexAndSerialNumber(Other.Consumer)
			&& ConsumerBindingSerial == Other.ConsumerBindingSerial
			&& SourceSession == Other.SourceSession;
	}

	bool operator!=(const FGGYGOMovementInputConsumerBindingId& Other) const
	{
		return !(*this == Other);
	}
};

/** 来源事实种类；会话失效和不可证明不能冒充真实释放。 */
enum class EGGYGOMovementInputFactKind : uint8
{
	Invalid = 0,
	SessionOpened,
	NeutralConfirmed,
	RequestStarted,
	RequestReleased,
	SessionInvalidated,
	SourceUnresolved
};

/** SessionOpened的原资格模式值；Cold不是已发行请求，Rearm不证明真实释放。 */
enum class EGGYGOMovementInputSessionMode : uint8
{
	Invalid = 0,
	Cold,
	Rearm
};

/** Source为实际RequestStarted提供的开始证明；不授移动执行或补造Neutral。 */
enum class EGGYGOMovementInputStartProof : uint8
{
	Invalid = 0,
	ColdPhysicalPress,
	ReleasedThenPhysicalPress
};

/** 中性事实值副本；不持有按键集合、neutral 状态或移动执行/失败状态。 */
struct FGGYGOMovementInputFact
{
	/** 会话级事实允许 RequestSerial 为 0；请求级事实需要已发行的请求身份。 */
	FGGYGOMovementInputRequestIdentity Request;
	/** PlayerInput 发行的观察顺序；0 未分配，不能以 World time 替代。 */
	uint64 EventSerial = 0;
	EGGYGOMovementInputFactKind Kind = EGGYGOMovementInputFactKind::Invalid;
	/** 失效或不可证明事实须有可定位原因；具体原因由生产者提供。 */
	FName Reason = NAME_None;
	/** 仅SessionOpened允许Cold/Rearm；其它Kind必须Invalid，缺值不能选默认模式。 */
	EGGYGOMovementInputSessionMode SessionMode = EGGYGOMovementInputSessionMode::Invalid;
	/** 仅RequestStarted允许有效证明；其它Kind必须Invalid，同Event不得改换证明。 */
	EGGYGOMovementInputStartProof StartProof = EGGYGOMovementInputStartProof::Invalid;
};

/**
 * 注册和在途回调携带原绑定值；旧回调不得查询新绑定并替换自身身份。
 * CMC 消费结果由消费者定义，记录事实不表示移动准入或执行成功。
 */
DECLARE_DELEGATE_TwoParams(FGGYGOMovementInputFactDelegate,
	const FGGYGOMovementInputConsumerBindingId&, const FGGYGOMovementInputFact&);
