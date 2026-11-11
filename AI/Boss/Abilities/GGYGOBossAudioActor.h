/** @file GGYGOBossAudioActor.h @brief 原 GA 持有的持续声复制资源 */
#pragma once
#include "GameFramework/Actor.h"
#include "GGYGOBossAudioActor.generated.h"

class UAudioComponent;
class USoundBase;
UCLASS(NotBlueprintable)
class GGYGO_API AGGYGOBossAudioActor : public AActor
{
	GENERATED_BODY()
public:
	AGGYGOBossAudioActor();
	void Configure(USoundBase* InSound, float InVolume, float InPitch);
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
	UFUNCTION()
	void OnRep_Playback();
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UAudioComponent> Audio;
	UPROPERTY(ReplicatedUsing = OnRep_Playback)
	TObjectPtr<USoundBase> Sound;
	UPROPERTY(ReplicatedUsing = OnRep_Playback)
	float Volume = 1.0f;
	UPROPERTY(ReplicatedUsing = OnRep_Playback)
	float Pitch = 1.0f;
	UPROPERTY(ReplicatedUsing = OnRep_Playback)
	float StartServerTime = 0.0f;
};
