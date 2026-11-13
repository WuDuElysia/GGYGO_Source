/** @file GGYGOCharacterLandingTestTypes.h @brief 记录真实 Landed 的测试观察者。 */
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "GGYGOCharacterLandingTestTypes.generated.h"

struct FHitResult;
class AActor;

UCLASS()
class UGGYGOCharacterLandingTestObserver : public UObject
{
	GENERATED_BODY()
public:
	UFUNCTION()
	void RecordLanded(const FHitResult& Hit);
	UFUNCTION()
	void RecordMovement(float DeltaSeconds, FVector OldLocation, FVector OldVelocity);
	UFUNCTION()
	void RecordHit(AActor* SelfActor, AActor* OtherActor, FVector NormalImpulse, const FHitResult& Hit);
	int32 CharacterImpacts = 0;
	TArray<FVector> CompletedLocations;
	int32 CharacterLandings = 0;
	int32 EnvironmentLandings = 0;
};
