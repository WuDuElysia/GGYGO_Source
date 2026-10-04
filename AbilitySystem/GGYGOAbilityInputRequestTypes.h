#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "UObject/WeakObjectPtrTemplates.h"

class UGGYGOAbilitySystemComponent;

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
};

/** Cache-operation outcome only; never an ability activation result. */
enum class EGGYGOAbilityInputRequestOutcome : uint8
{
	Accepted,
	AlreadyApplied,
	Rejected,
	Stale
};

enum class EGGYGOAbilityInputRequestReason : uint8
{
	None,
	InvalidRequest,
	InvalidTag,
	InvalidDeadline,
	InvalidPreviousIdentity,
	InvalidEndKind,
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
	NotHeld,
	SerialExhausted,
	RevisionExhausted
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
