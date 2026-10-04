/** @file GGYGOBossActionSet.h @brief Boss 决策层的语义动作集 */
#pragma once

#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"

#include "GGYGOBossActionSet.generated.h"

class UGGYGOCombatActionAbility;

/** 一个可供 AI 选择的语义动作；不保存冷却、伤害或 Montage 实现。 */
USTRUCT(BlueprintType)
struct FGGYGOBossActionDefinition
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta = (Categories = "BossAction"))
	FGameplayTag ActionTag;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	TSubclassOf<UGGYGOCombatActionAbility> AbilityClass;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta = (ClampMin = "0.0"))
	float BaseWeight = 1.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta = (ClampMin = "0.0"))
	float MinDistance = 0.0f;

	/** 0 表示不限制最远距离。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta = (ClampMin = "0.0"))
	float MaxDistance = 0.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta = (ClampMin = "0.0", ClampMax = "180.0"))
	float MaxFacingAngle = 180.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	FGameplayTagContainer RequiredTags;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	FGameplayTagContainer BlockedTags;

	/** 选中后的权重乘数；0 优先避免重复，合法候选全部耗尽时回退基础权重。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float RepeatPenalty = 0.35f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta = (ClampMin = "0.0"))
	float UnusedWeightGain = 0.25f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta = (ClampMin = "0.0"))
	float MaxWeight = 3.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	bool bRequiresLineOfSight = false;
};

UCLASS(BlueprintType, Const, meta = (DisplayName = "GGYGO Boss Action Set"))
class GGYGO_API UGGYGOBossActionSet : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** 全集校验；空集/全零基础权重合法。运行时与编辑器共享同一套规则。 */
	bool ValidateConfiguration(FString& OutError) const;
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif

	/** 精确唯一匹配；重复 Tag 返回 nullptr。 */
	const FGGYGOBossActionDefinition* FindAction(FGameplayTag ActionTag) const;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta = (TitleProperty = "ActionTag"))
	TArray<FGGYGOBossActionDefinition> Actions;
};
