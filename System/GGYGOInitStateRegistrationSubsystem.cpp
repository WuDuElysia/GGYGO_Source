#include "System/GGYGOInitStateRegistrationSubsystem.h"

#include "Components/GameFrameworkComponentManager.h"
#include "CoreGlobals.h"
#include "Engine/GameInstance.h"
#include "Logging/LogMacros.h"
#include "Subsystems/SubsystemCollection.h"
#include "System/GGYGOGameplayTags.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOInitStateRegistrationSubsystem)

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOInitStateRegistration, Log, All);

void UGGYGOInitStateRegistrationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	if (!IsInGameThread())
	{
		UE_LOG(LogGGYGOInitStateRegistration, Error,
			TEXT("[System.InitStateRegistration] Subsystem=%p Reason=InitializeRequiresGameThread"),
			static_cast<const void*>(this));
		return;
	}
	Super::Initialize(Collection);

	UGameInstance* const GameInstance = GetGameInstance();
	if (!IsValid(GameInstance) || GameInstance->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		UE_LOG(LogGGYGOInitStateRegistration, Error,
			TEXT("[System.InitStateRegistration] Subsystem=%s GameInstance=%s Reason=InvalidOriginalGameInstance"),
			*GetPathName(), *GetPathNameSafe(GameInstance));
		return;
	}
	UGameFrameworkComponentManager* const Manager = Collection.InitializeDependency<UGameFrameworkComponentManager>();
	if (!IsValid(Manager) || Manager->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		|| Manager->GetGameInstance() != GameInstance)
	{
		UE_LOG(LogGGYGOInitStateRegistration, Error,
			TEXT("[System.InitStateRegistration] Subsystem=%s GameInstance=%s Manager=%s ExpectedManagerClass=%s Reason=InvalidOriginalManager"),
			*GetPathName(), *GetPathNameSafe(GameInstance), *GetPathNameSafe(Manager),
			*GetPathNameSafe(UGameFrameworkComponentManager::StaticClass()));
		return;
	}

	// Call-local registration inputs, not a retained order or actor-state mirror.
	const FGameplayTag StateChain[] = {
		GGYGOGameplayTags::InitState_Spawned.GetTag(),
		GGYGOGameplayTags::InitState_DataAvailable.GetTag(),
		GGYGOGameplayTags::InitState_DataInitialized.GetTag(),
		GGYGOGameplayTags::InitState_GameplayReady.GetTag()
	};
	constexpr int32 StateCount = UE_ARRAY_COUNT(StateChain);
	for (int32 Index = 0; Index < StateCount; ++Index)
	{
		if (!StateChain[Index].IsValid())
		{
			UE_LOG(LogGGYGOInitStateRegistration, Error,
				TEXT("[System.InitStateRegistration] Subsystem=%s GameInstance=%s Manager=%s TagSource=System/GGYGOGameplayTags.cpp StateIndex=%d Reason=InvalidRequiredNativeTag"),
				*GetPathName(), *GetPathNameSafe(GameInstance), *GetPathNameSafe(Manager), Index);
			return;
		}
	}
	// Native registration preserves existing entries. Reject conflicting order before adding anything.
	for (int32 Earlier = 0; Earlier < StateCount - 1; ++Earlier)
	{
		for (int32 Later = Earlier + 1; Later < StateCount; ++Later)
		{
			if (Manager->IsInitStateAfterOrEqual(StateChain[Earlier], StateChain[Later]))
			{
				UE_LOG(LogGGYGOInitStateRegistration, Error,
					TEXT("[System.InitStateRegistration] Subsystem=%s GameInstance=%s Manager=%s ExpectedEarlier=%s ExpectedLater=%s Reason=ExistingOrderConflict NoStatesRegistered"),
					*GetPathName(), *GetPathNameSafe(GameInstance), *GetPathNameSafe(Manager),
					*StateChain[Earlier].ToString(), *StateChain[Later].ToString());
				return;
			}
		}
	}

	FGameplayTag PreviousState;
	for (const FGameplayTag& State : StateChain)
	{
		// The first tag is appended; each subsequent tag is anchored after its predecessor.
		Manager->RegisterInitState(State, false, PreviousState);
		PreviousState = State;
	}
	for (int32 Index = 1; Index < StateCount; ++Index)
	{
		const bool bForward = Manager->IsInitStateAfterOrEqual(StateChain[Index], StateChain[Index - 1]);
		const bool bReverse = Manager->IsInitStateAfterOrEqual(StateChain[Index - 1], StateChain[Index]);
		if (!bForward || bReverse)
		{
			UE_LOG(LogGGYGOInitStateRegistration, Error,
				TEXT("[System.InitStateRegistration] Subsystem=%s GameInstance=%s Manager=%s ExpectedEarlier=%s ExpectedLater=%s Forward=%d Reverse=%d Reason=RegisteredOrderValidationFailed"),
				*GetPathName(), *GetPathNameSafe(GameInstance), *GetPathNameSafe(Manager),
				*StateChain[Index - 1].ToString(), *StateChain[Index].ToString(), bForward, bReverse);
			return;
		}
	}
	UE_LOG(LogGGYGOInitStateRegistration, Log,
		TEXT("[System.InitStateRegistration] GameInstance=%s Manager=%s RegisteredOrder=%s -> %s -> %s -> %s"),
		*GetPathNameSafe(GameInstance), *GetPathNameSafe(Manager),
		*StateChain[0].ToString(), *StateChain[1].ToString(), *StateChain[2].ToString(), *StateChain[3].ToString());
}
