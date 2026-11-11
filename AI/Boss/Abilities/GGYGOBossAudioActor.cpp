#include "AI/Boss/Abilities/GGYGOBossAudioActor.h"
#include "Components/AudioComponent.h"
#include "Net/UnrealNetwork.h"
#include "Sound/SoundBase.h"
#include "GameFramework/GameStateBase.h"
#include "Engine/World.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOBossAudioActor)

AGGYGOBossAudioActor::AGGYGOBossAudioActor()
{
	bReplicates = true;
	SetReplicateMovement(true);
	bNetUseOwnerRelevancy = true;
	PrimaryActorTick.bCanEverTick = false;
	Audio = CreateDefaultSubobject<UAudioComponent>(TEXT("OwnedAudio"));
	SetRootComponent(Audio);
	Audio->bAutoActivate = false;
	Audio->bAutoDestroy = false;
	Audio->bStopWhenOwnerDestroyed = true;
}

void AGGYGOBossAudioActor::Configure(USoundBase* InSound, float InVolume, float InPitch)
{
	check(HasAuthority() && !HasActorBegunPlay());
	Sound = InSound;
	Volume = InVolume;
	Pitch = InPitch;
	StartServerTime = GetWorld()->GetTimeSeconds();
}

void AGGYGOBossAudioActor::BeginPlay()
{
	Super::BeginPlay();
	OnRep_Playback();
}

void AGGYGOBossAudioActor::OnRep_Playback()
{
	if (!HasActorBegunPlay() || !Sound || GetNetMode() == NM_DedicatedServer) { return; }
	Audio->SetVolumeMultiplier(Volume);
	Audio->SetPitchMultiplier(Pitch);
	if (Audio->Sound != Sound) { Audio->SetSound(Sound); }
	if (!Audio->IsPlaying())
	{
		const AGameStateBase* State = GetWorld()->GetGameState();
		if (!State && !HasAuthority())
		{ UE_LOG(LogTemp, Error, TEXT("BossAudio 缺少真实服务器时钟，拒绝以客户端本地时间起播。")); return; }
		const float Now = HasAuthority() ? GetWorld()->GetTimeSeconds() : State->GetServerWorldTimeSeconds();
		const float Offset = FMath::Max(0.0f, (Now - StartServerTime) * Pitch);
		if (Sound->IsLooping() || Offset < Sound->GetDuration()) { Audio->Play(Offset); }
	}
}

void AGGYGOBossAudioActor::EndPlay(const EEndPlayReason::Type Reason)
{
	Audio->Stop();
	Super::EndPlay(Reason);
}

void AGGYGOBossAudioActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION(AGGYGOBossAudioActor, Sound, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AGGYGOBossAudioActor, Volume, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AGGYGOBossAudioActor, Pitch, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AGGYGOBossAudioActor, StartServerTime, COND_InitialOnly);
}
