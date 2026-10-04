/** @file GGYGOBossActionSet.cpp */
#include "AI/Boss/GGYGOBossActionSet.h"

#include "AbilitySystem/Abilities/GGYGOCombatActionAbility.h"
#include "Misc/DataValidation.h"
#include "System/GGYGOGameplayTags.h"

#include <cmath>

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOBossActionSet)

bool UGGYGOBossActionSet::ValidateConfiguration(FString& OutError) const
{
	OutError.Reset();
	TArray<FString> Errors;
	TSet<FGameplayTag> SeenTags;
	const FGameplayTag BossActionRoot = GGYGOGameplayTags::BossAction;

	for (int32 Index = 0; Index < Actions.Num(); ++Index)
	{
		const FGGYGOBossActionDefinition& Action = Actions[Index];
		const FString Row = FString::Printf(TEXT("Actions[%d]"), Index);

		if (!Action.ActionTag.IsValid())
		{
			Errors.Add(FString::Printf(TEXT("%s: ActionTag 无效。"), *Row));
		}
		else
		{
			if (SeenTags.Contains(Action.ActionTag))
			{
				Errors.Add(FString::Printf(TEXT("%s: ActionTag [%s] 重复。"),
					*Row, *Action.ActionTag.ToString()));
			}
			else
			{
				SeenTags.Add(Action.ActionTag);
			}

			if (Action.ActionTag == BossActionRoot || !Action.ActionTag.MatchesTag(BossActionRoot))
			{
				Errors.Add(FString::Printf(TEXT("%s: ActionTag [%s] 必须是 BossAction 下的具体语义动作。"),
					*Row, *Action.ActionTag.ToString()));
			}
		}

		UClass* AbilityClass = Action.AbilityClass.Get();
		if (!IsValid(AbilityClass))
		{
			Errors.Add(FString::Printf(TEXT("%s: AbilityClass 为空或无效。"), *Row));
		}
		else if (AbilityClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			Errors.Add(FString::Printf(TEXT("%s: AbilityClass [%s] 是抽象类或无效类。"),
				*Row, *GetNameSafe(AbilityClass)));
		}
		else if (!AbilityClass->IsChildOf(UGGYGOCombatActionAbility::StaticClass()))
		{
			Errors.Add(FString::Printf(TEXT("%s: AbilityClass [%s] 不是 CombatActionAbility。"),
				*Row, *GetNameSafe(AbilityClass)));
		}
		else
		{
			const UGGYGOCombatActionAbility* AbilityCDO =
				AbilityClass->GetDefaultObject<UGGYGOCombatActionAbility>();
			if (!AbilityCDO || !Action.ActionTag.IsValid() || AbilityCDO->GetActionTag() != Action.ActionTag)
			{
				Errors.Add(FString::Printf(TEXT("%s: ActionTag 与 AbilityClass [%s] 的 CDO 标签不一致。"),
					*Row, *GetNameSafe(AbilityClass)));
			}
		}

		const auto CheckNonNegativeFinite = [&Errors, &Row](const TCHAR* Name, float Value)
		{
			if (!std::isfinite(static_cast<double>(Value)) || Value < 0.0f)
			{
				Errors.Add(FString::Printf(TEXT("%s: %s 必须是有限非负数。"), *Row, Name));
			}
		};

		CheckNonNegativeFinite(TEXT("BaseWeight"), Action.BaseWeight);
		CheckNonNegativeFinite(TEXT("MinDistance"), Action.MinDistance);
		CheckNonNegativeFinite(TEXT("MaxDistance"), Action.MaxDistance);
		CheckNonNegativeFinite(TEXT("UnusedWeightGain"), Action.UnusedWeightGain);
		CheckNonNegativeFinite(TEXT("MaxWeight"), Action.MaxWeight);

		if (!std::isfinite(static_cast<double>(Action.MaxFacingAngle)) ||
			Action.MaxFacingAngle < 0.0f || Action.MaxFacingAngle > 180.0f)
		{
			Errors.Add(FString::Printf(TEXT("%s: MaxFacingAngle 必须是 0 到 180 度的有限数。"), *Row));
		}

		if (!std::isfinite(static_cast<double>(Action.RepeatPenalty)) ||
			Action.RepeatPenalty < 0.0f || Action.RepeatPenalty > 1.0f)
		{
			Errors.Add(FString::Printf(TEXT("%s: RepeatPenalty 必须是 0 到 1 的有限数。"), *Row));
		}

		if (std::isfinite(static_cast<double>(Action.MinDistance)) &&
			std::isfinite(static_cast<double>(Action.MaxDistance)) &&
			Action.MaxDistance > 0.0f && Action.MaxDistance < Action.MinDistance)
		{
			Errors.Add(FString::Printf(TEXT("%s: MaxDistance 必须为 0 或不小于 MinDistance。"), *Row));
		}
	}

	if (Errors.IsEmpty())
	{
		return true;
	}

	OutError = FString::Join(Errors, TEXT("\n"));
	return false;
}

#if WITH_EDITOR
EDataValidationResult UGGYGOBossActionSet::IsDataValid(FDataValidationContext& Context) const
{
	const EDataValidationResult SuperResult = Super::IsDataValid(Context);
	FString Error;
	if (!ValidateConfiguration(Error))
	{
		Context.AddError(FText::FromString(Error));
		return EDataValidationResult::Invalid;
	}
	return SuperResult == EDataValidationResult::Invalid
		? EDataValidationResult::Invalid
		: EDataValidationResult::Valid;
}
#endif

const FGGYGOBossActionDefinition* UGGYGOBossActionSet::FindAction(FGameplayTag ActionTag) const
{
	const FGGYGOBossActionDefinition* Match = nullptr;
	for (const FGGYGOBossActionDefinition& Action : Actions)
	{
		if (Action.ActionTag != ActionTag)
		{
			continue;
		}

		if (Match)
		{
			return nullptr;
		}
		Match = &Action;
	}
	return Match;
}
