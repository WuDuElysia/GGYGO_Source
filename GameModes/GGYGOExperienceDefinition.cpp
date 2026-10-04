/**
 * @file GGYGOExperienceDefinition.cpp
 * @brief 玩法定义实现
 */
#include "GameModes/GGYGOExperienceDefinition.h"

#include "Character/Data/GGYGOPawnData.h"
#include "Teams/GGYGOSquadTypes.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOExperienceDefinition)

#define LOCTEXT_NAMESPACE "GGYGOExperience"

UGGYGOExperienceDefinition::UGGYGOExperienceDefinition(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

bool UGGYGOExperienceDefinition::TryBuildGameFeatureInput(
	FGGYGOGameFeatureClosureResolver::FInput& OutInput,
	TArray<FString>& OutErrors) const
{
	OutInput = {};
	OutErrors.Reset();

	FGGYGOGameFeatureClosureResolver::FInput Candidate;
	const FString ExperiencePath = GetPathName();
	const auto AddError = [&OutErrors, &ExperiencePath](const TCHAR* Field, int32 Index, const FString& Reason)
	{
		OutErrors.Add(FString::Printf(TEXT("[GameFeature] Experience '%s': %s[%d]: %s"),
			*ExperiencePath, Field, Index, *Reason));
	};
	const auto IsValidPluginName = [](const FString& PluginName)
	{
		return !PluginName.IsEmpty() && PluginName.TrimStartAndEnd() == PluginName;
	};

	if (GameFeaturesToEnable.IsEmpty() && !GameFeatureSources.IsEmpty())
	{
		AddError(TEXT("GameFeatureSources"), 0,
			TEXT("Source declarations require at least one GameFeaturesToEnable root; no-GF mode requires both arrays empty."));
	}

	for (int32 Index = 0; Index < GameFeaturesToEnable.Num(); ++Index)
	{
		const FString& PluginName = GameFeaturesToEnable[Index];
		if (!IsValidPluginName(PluginName))
		{
			AddError(TEXT("GameFeaturesToEnable"), Index,
				TEXT("Plugin name must be nonempty and contain no leading or trailing whitespace."));
			continue;
		}
		Candidate.RootPluginNames.AddUnique(PluginName);
	}

	// Original configuration indices are temporary diagnostics, never runtime/plugin state.
	TMap<FString, int32> DeclarationIndices;
	for (int32 Index = 0; Index < GameFeatureSources.Num(); ++Index)
	{
		const FGGYGOGameFeatureSourceDeclaration& Declaration = GameFeatureSources[Index];
		bool bValid = true;
		if (!IsValidPluginName(Declaration.PluginName))
		{
			AddError(TEXT("GameFeatureSources.PluginName"), Index,
				TEXT("Plugin name must be nonempty and contain no leading or trailing whitespace."));
			bValid = false;
		}

		FGGYGOGameFeatureClosureResolver::FSourceDeclaration NativeDeclaration;
		switch (Declaration.Source)
		{
		case EGGYGOGameFeatureSource::ProjectNativeManaged:
			NativeDeclaration.Source = FGGYGOGameFeatureClosureResolver::ESource::ProjectNativeManaged;
			if (!Declaration.ExternalOwnerLabel.IsEmpty())
			{
				AddError(TEXT("GameFeatureSources.ExternalOwnerLabel"), Index,
					TEXT("ProjectNativeManaged requires an empty external owner label."));
				bValid = false;
			}
			break;
		case EGGYGOGameFeatureSource::ExplicitExternalBorrow:
			NativeDeclaration.Source = FGGYGOGameFeatureClosureResolver::ESource::ExplicitExternalBorrow;
			if (Declaration.ExternalOwnerLabel.IsEmpty()
				|| Declaration.ExternalOwnerLabel.TrimStartAndEnd() != Declaration.ExternalOwnerLabel)
			{
				AddError(TEXT("GameFeatureSources.ExternalOwnerLabel"), Index,
					TEXT("ExplicitExternalBorrow requires a nonempty external owner label without leading or trailing whitespace; this does not grant admission."));
				bValid = false;
			}
			break;
		case EGGYGOGameFeatureSource::Unspecified:
			AddError(TEXT("GameFeatureSources.Source"), Index, TEXT("An explicit source is required; Unspecified is invalid."));
			bValid = false;
			break;
		default:
			AddError(TEXT("GameFeatureSources.Source"), Index,
				FString::Printf(TEXT("Unknown source enum value %d."), static_cast<int32>(Declaration.Source)));
			bValid = false;
			break;
		}

		if (!bValid)
		{
			continue;
		}

		if (const int32* PreviousIndex = DeclarationIndices.Find(Declaration.PluginName))
		{
			const FGGYGOGameFeatureSourceDeclaration& Previous = GameFeatureSources[*PreviousIndex];
			if (Previous.Source != Declaration.Source || Previous.ExternalOwnerLabel != Declaration.ExternalOwnerLabel)
			{
				AddError(TEXT("GameFeatureSources"), Index,
					FString::Printf(TEXT("Plugin '%s' conflicts with the declaration at GameFeatureSources[%d]."),
						*Declaration.PluginName, *PreviousIndex));
			}
			continue;
		}

		DeclarationIndices.Add(Declaration.PluginName, Index);
		NativeDeclaration.PluginName = Declaration.PluginName;
		NativeDeclaration.ExternalOwnerLabel = Declaration.ExternalOwnerLabel;
		Candidate.DeclaredSources.Add(MoveTemp(NativeDeclaration));
	}

	for (int32 Index = 0; Index < GameFeaturesToEnable.Num(); ++Index)
	{
		const FString& PluginName = GameFeaturesToEnable[Index];
		if (IsValidPluginName(PluginName) && !DeclarationIndices.Contains(PluginName))
		{
			AddError(TEXT("GameFeaturesToEnable"), Index,
				FString::Printf(TEXT("Plugin '%s' has no valid explicit GameFeatureSources declaration."), *PluginName));
		}
	}

	if (!OutErrors.IsEmpty())
	{
		return false;
	}

	OutInput = MoveTemp(Candidate);
	return true;
}

#if WITH_EDITOR
EDataValidationResult UGGYGOExperienceDefinition::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = CombineDataValidationResults(Super::IsDataValid(Context), EDataValidationResult::Valid);

	FGGYGOGameFeatureClosureResolver::FInput GameFeatureInput;
	TArray<FString> GameFeatureErrors;
	if (!TryBuildGameFeatureInput(GameFeatureInput, GameFeatureErrors))
	{
		for (const FString& Error : GameFeatureErrors)
		{
			Context.AddError(FText::FromString(Error));
		}
		Result = EDataValidationResult::Invalid;
	}

	if (!GGYGOSquad::IsMemberCountWithinCapacity(SquadMembers.Num()))
	{
		// 运行时 SetRoster 同样按原始数量拒绝超限名单，不截断。
		// 每个成员带一个 ASC，超配的代价是成倍的复制开销，值得在编辑期就拦住。
		Context.AddError(FText::Format(
			LOCTEXT("SquadTooLarge", "默认编队有 {0} 个成员，超过上限 {1}，该名单将被拒绝。请减少成员数量。"),
			FText::AsNumber(SquadMembers.Num()),
			FText::AsNumber(GGYGO_MAX_SQUAD_SIZE)));
		Result = EDataValidationResult::Invalid;
	}

	int32 MemberIndex = 0;
	for (const TObjectPtr<const UGGYGOPawnData>& Member : SquadMembers)
	{
		if (!Member)
		{
			Context.AddError(FText::Format(
				LOCTEXT("NullSquadMember", "队伍成员 {0} 是空的。空槽位会让队伍序号与实际角色错位。"),
				FText::AsNumber(MemberIndex)));
			Result = EDataValidationResult::Invalid;
		}
		else if (!Member->PawnClass)
		{
			// PawnData 没配 PawnClass 时无法生成角色，而这个错误在运行时
			// 表现为"角色没出现"，很难追到是哪份资产的问题。
			Context.AddError(FText::Format(
				LOCTEXT("NoPawnClass", "队伍成员 {0} 的 PawnData [{1}] 没有配置 PawnClass。"),
				FText::AsNumber(MemberIndex),
				FText::FromString(GetNameSafe(Member))));
			Result = EDataValidationResult::Invalid;
		}

		++MemberIndex;
	}

	return Result;
}
#endif

#undef LOCTEXT_NAMESPACE
