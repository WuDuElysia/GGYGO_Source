/** @file GGYGOComboCorrection.h @brief PlayerCombo 的强类型 TargetData 纠正负载。 */
#pragma once

#include "Abilities/GameplayAbilityTargetTypes.h"
#include "GGYGOComboCorrection.generated.h"

/** 与 ASC 的预测 Spec/ActivationKey 一起传输的连段状态快照。 */
USTRUCT()
struct FGGYGOComboCorrectionData : public FGameplayAbilityTargetData
{
	GENERATED_BODY()

	FGGYGOComboCorrectionData() = default;
	FGGYGOComboCorrectionData(int32 InRevision, int32 InRequestId, int32 InServerStep, float InPosition,
		bool bInWindowOpen, bool bInWindowClosed, bool bInAccepted)
		: Revision(InRevision)
		, RequestId(InRequestId)
		, ServerStep(InServerStep)
		, Position(InPosition)
		, bWindowOpen(bInWindowOpen)
		, bWindowClosed(bInWindowClosed)
		, bAccepted(bInAccepted)
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
