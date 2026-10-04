/** @file GGYGOComboCorrection.cpp */
#include "AbilitySystem/Abilities/GGYGOComboCorrection.h"

#include "Serialization/Archive.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOComboCorrection)

bool FGGYGOComboCorrectionData::NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
{
	(void)Map;
	Ar << Revision;
	Ar << RequestId;
	Ar << ServerStep;
	Ar << Position;
	Ar << bWindowOpen;
	Ar << bWindowClosed;
	Ar << bAccepted;
	bOutSuccess = !Ar.IsError();
	return bOutSuccess;
}

bool FGGYGOComboCorrectionData::HasValidFields() const
{
	return Revision > 0
		&& RequestId > 0 && RequestId <= 65535
		&& ServerStep >= 0
		&& FMath::IsFinite(Position) && Position >= 0.0f
		&& !(bWindowOpen && bWindowClosed);
}
