#pragma once

#include "CoreMinimal.h"

class AActor;
class APawn;
class UAbilitySystemComponent;
class UGGYGOAbilitySystemComponent;
struct FGameplayAbilityActorInfo;

#if WITH_DEV_AUTOMATION_TESTS
/** Read-only observations from public binding interfaces; never a second binding state. */
struct FGGYGOCombatantBindingSuccessorSnapshot
{
	bool bEndpointsLive = false;
	APawn* HostAvatar = nullptr;
	AActor* CachedOwner = nullptr;
	AActor* CachedAvatar = nullptr;
	const FGameplayAbilityActorInfo* ActorInfoAllocation = nullptr;
	AActor* ActorInfoOwner = nullptr;
	AActor* ActorInfoAvatar = nullptr;
	UAbilitySystemComponent* ActorInfoASC = nullptr;
	UGGYGOAbilitySystemComponent* OldCachedASC = nullptr;
	UGGYGOAbilitySystemComponent* CandidateCachedASC = nullptr;
	bool bOldDestroyedSubscription = false;
	bool bCandidateDestroyedSubscription = false;
};
#endif // WITH_DEV_AUTOMATION_TESTS
