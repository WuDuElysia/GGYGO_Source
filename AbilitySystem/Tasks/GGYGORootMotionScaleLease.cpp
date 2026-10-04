/** @file GGYGORootMotionScaleLease.cpp @brief Montage Task root motion scale token lease. */
#include "AbilitySystem/Tasks/GGYGORootMotionScaleLease.h"

#include "GameFramework/Character.h"

namespace
{
	struct FRootMotionScaleLeaseRecord
	{
		TWeakObjectPtr<ACharacter> Character;
		TWeakObjectPtr<UObject> Owner;
		FGGYGORootMotionScaleLease::FToken Token = 0;
		float BaselineScale = 1.0f;
	};

	using FRootMotionScaleLeaseMap = TMap<TWeakObjectPtr<ACharacter>, FRootMotionScaleLeaseRecord>;

	FRootMotionScaleLeaseMap& GetRootMotionScaleLeases()
	{
		static FRootMotionScaleLeaseMap Leases;
		return Leases;
	}

	FGGYGORootMotionScaleLease::FToken& GetNextRootMotionScaleLeaseToken()
	{
		static FGGYGORootMotionScaleLease::FToken NextToken = 1;
		return NextToken;
	}

	void PruneInvalidCharacters(FRootMotionScaleLeaseMap& Leases)
	{
		for (auto It = Leases.CreateIterator(); It; ++It)
		{
			if (!It.Key().IsValid())
			{
				It.RemoveCurrent();
			}
		}
	}
}

FGGYGORootMotionScaleLease::FToken FGGYGORootMotionScaleLease::Acquire(
	ACharacter* Character, UObject* Owner, float DesiredScale)
{
	if (!IsValid(Character) || !IsValid(Owner) || !FMath::IsFinite(DesiredScale))
	{
		return 0;
	}

	FRootMotionScaleLeaseMap& Leases = GetRootMotionScaleLeases();
	PruneInvalidCharacters(Leases);

	const TWeakObjectPtr<ACharacter> CharacterKey(Character);
	const FRootMotionScaleLeaseRecord* ExistingRecord = Leases.Find(CharacterKey);
	const float BaselineScale = ExistingRecord
		? ExistingRecord->BaselineScale
		: Character->GetAnimRootMotionTranslationScale();

	FGGYGORootMotionScaleLease::FToken& NextToken = GetNextRootMotionScaleLeaseToken();
	FGGYGORootMotionScaleLease::FToken Token = NextToken++;
	if (Token == 0)
	{
		Token = NextToken++;
	}
	if (NextToken == 0)
	{
		NextToken = 1;
	}

	FRootMotionScaleLeaseRecord& Record = Leases.Add(CharacterKey);
	Record.Character = CharacterKey;
	Record.Owner = Owner;
	Record.Token = Token;
	Record.BaselineScale = BaselineScale;
	Character->SetAnimRootMotionTranslationScale(DesiredScale);
	return Token;
}

bool FGGYGORootMotionScaleLease::Release(
	const TWeakObjectPtr<ACharacter>& Character, const UObject* Owner, FToken Token)
{
	if (Token == 0)
	{
		return false;
	}

	FRootMotionScaleLeaseMap& Leases = GetRootMotionScaleLeases();
	PruneInvalidCharacters(Leases);

	FRootMotionScaleLeaseRecord* Record = Leases.Find(Character);
	if (!Record || Record->Token != Token || Record->Owner.Get() != Owner)
	{
		return false;
	}

	const float BaselineScale = Record->BaselineScale;
	ACharacter* LiveCharacter = Character.Get();
	Leases.Remove(Character);
	if (IsValid(LiveCharacter))
	{
		LiveCharacter->SetAnimRootMotionTranslationScale(BaselineScale);
	}
	return true;
}
