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

#if WITH_EDITOR
EDataValidationResult UGGYGOExperienceDefinition::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = CombineDataValidationResults(Super::IsDataValid(Context), EDataValidationResult::Valid);

	if (SquadMembers.Num() > GGYGO_MAX_SQUAD_SIZE)
	{
		// 运行时也会截断并报错，但那要等到实际进关才发现。
		// 每个成员带一个 ASC，超配的代价是成倍的复制开销，值得在编辑期就拦住。
		Context.AddError(FText::Format(
			LOCTEXT("SquadTooLarge", "默认编队有 {0} 个成员，超过上限 {1}。多余的成员在装配时会被丢弃。"),
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
