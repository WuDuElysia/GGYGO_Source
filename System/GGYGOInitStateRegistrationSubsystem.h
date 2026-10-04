#pragma once

#include "Subsystems/GameInstanceSubsystem.h"

#include "GGYGOInitStateRegistrationSubsystem.generated.h"

/**
 * Registers the project's global feature-state order during native GI initialization.
 * The original GameFrameworkComponentManager owns the order and every actor feature state.
 * No retained manager, readiness state, callbacks or resources require separate cleanup.
 */
UCLASS()
class GGYGO_API UGGYGOInitStateRegistrationSubsystem final : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
};
