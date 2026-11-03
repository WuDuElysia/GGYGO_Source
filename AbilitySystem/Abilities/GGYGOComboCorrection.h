/** @file GGYGOComboCorrection.h @brief PlayerCombo 的强类型 TargetData 纠正负载。 */
#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbilityTargetTypes.h"
#include "GGYGOComboCorrection.generated.h"

/** 与 ASC 的预测 Spec/ActivationKey 一起传输的连段状态快照。 */
USTRUCT()
struct FGGYGOComboCorrectionData : public FGameplayAbilityTargetData
{
	GENERATED_BODY()

	FGGYGOComboCorrectionData() = default;
	FGGYGOComboCorrectionData(int32 InRevision, int32 InRequestId, int32 InServerStep, float InPosition,
		bool bInWindowOpen, bool bInWindowClosed, bool bInAccepted,
		int32 InServerStepRequestId = INDEX_NONE, const FQuat* InEntryActorRotation = nullptr)
		: Revision(InRevision)
		, RequestId(InRequestId)
		, ServerStep(InServerStep)
		, Position(InPosition)
		, bWindowOpen(bInWindowOpen)
		, bWindowClosed(bInWindowClosed)
		, bAccepted(bInAccepted)
		, ServerStepRequestId(InServerStepRequestId)
		, bHasEntryActorRotation(InEntryActorRotation != nullptr)
		, EntryActorRotation(InEntryActorRotation ? *InEntryActorRotation : FQuat::Identity)
	{
	}

	UPROPERTY()
	int32 Revision = 0;

	UPROPERTY()
	int32 RequestId = 0;

	UPROPERTY()
	int32 ServerStep = INDEX_NONE;

	UPROPERTY()
	float Position = 0.0f;

	UPROPERTY()
	bool bWindowOpen = false;

	UPROPERTY()
	bool bWindowClosed = false;

	UPROPERTY()
	bool bAccepted = false;

	/** 当前服务器段的真实起段请求；RequestId 是本消息确认／拒绝的请求，二者不等价。 */
	UPROPERTY()
	int32 ServerStepRequestId = INDEX_NONE;

	/** 旧 CurrentFacing 调用可不带 basis；QualifiedInput 的重放必须有原实际接受值。 */
	UPROPERTY()
	bool bHasEntryActorRotation = false;

	UPROPERTY()
	FQuat EntryActorRotation = FQuat::Identity;

	virtual UScriptStruct* GetScriptStruct() const override
	{
		return FGGYGOComboCorrectionData::StaticStruct();
	}

	bool NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess);

	/** 验证独立于具体 ComboSteps 配置的字段约束。 */
	bool HasValidFields() const;
};

template<>
struct TStructOpsTypeTraits<FGGYGOComboCorrectionData> : public TStructOpsTypeTraitsBase2<FGGYGOComboCorrectionData>
{
	enum
	{
		WithNetSerializer = true,
		WithCopy = true
	};
};
