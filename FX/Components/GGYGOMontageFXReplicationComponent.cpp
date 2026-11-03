/** @file GGYGOMontageFXReplicationComponent.cpp @brief FX 发布宿主的注册寿命、复制与发布入口认证。 */
#include "FX/Components/GGYGOMontageFXReplicationComponent.h"

#include "AbilitySystem/Abilities/GGYGOGameplayAbility.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/Tasks/GGYGOAbilityTask_PlayMontageAndWaitForEvent.h"
#include "AbilitySystemInterface.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Character/Data/GGYGOMotionExecutionTypes.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"
#include "UObject/StrongObjectPtr.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOMontageFXReplicationComponent)

UGGYGOMontageFXReplicationComponent::UGGYGOMontageFXReplicationComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

UGGYGOMontageFXReplicationComponent* UGGYGOMontageFXReplicationComponent::FindForASC(const UAbilitySystemComponent* ASC)
{
	const AActor* Owner = ASC ? ASC->GetOwner() : nullptr;
	return Owner ? Owner->FindComponentByClass<ThisClass>() : nullptr;
}

void UGGYGOMontageFXReplicationComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION_NOTIFY(ThisClass, State, COND_None, REPNOTIFY_Always);
}

void UGGYGOMontageFXReplicationComponent::OnRegister()
{
	Super::OnRegister();
	if (!PublicationOwner)
	{
		// 同一 State 跨重注册复用原 LocalOwner；Initialize 只推进注册代次。
		PublicationOwner = UGGYGOMontageFXPublicationOwner::RegisterHost(this, State,
			FGGYGOMontageFXSendTerminal::CreateUObject(this, &ThisClass::SendTerminal));
	}
	PublicationOwner->ReceiveState();
}

void UGGYGOMontageFXReplicationComponent::OnUnregister()
{
	check(IsInGameThread());
	TStrongObjectPtr<UGGYGOMontageFXPublicationOwner> OriginalOwner(State.LocalOwner.Get());
	const uint64 OriginalRegistration = OriginalOwner.IsValid() ? OriginalOwner->GetRegistrationSerial() : 0;
	PublicationOwner = nullptr;
	Super::OnUnregister();
	if (OriginalOwner.IsValid()) { OriginalOwner->Shutdown(OriginalRegistration); }
}

void UGGYGOMontageFXReplicationComponent::OnRep_State()
{
	if (UGGYGOMontageFXPublicationOwner* Owner = PublicationOwner) { Owner->ReceiveState(); }
}

void UGGYGOMontageFXReplicationComponent::SendTerminal(const FGGYGOMontageFXTerminal& Terminal)
{
	const AActor* Owner = GetOwner();
	if (Owner && Owner->HasAuthority()) { MulticastTerminal(Terminal); }
}

void UGGYGOMontageFXReplicationComponent::MulticastTerminal_Implementation(const FGGYGOMontageFXTerminal& Terminal)
{
	if (UGGYGOMontageFXPublicationOwner* Owner = State.LocalOwner) { Owner->ReceiveTerminal(Terminal); }
}

UGGYGOAbilitySystemComponent* UGGYGOMontageFXReplicationComponent::GetOwnerAbilitySystem() const
{
	const IAbilitySystemInterface* Host = Cast<IAbilitySystemInterface>(GetOwner());
	UGGYGOAbilitySystemComponent* ASC = Host ? Cast<UGGYGOAbilitySystemComponent>(Host->GetAbilitySystemComponent()) : nullptr;
	return ASC && ASC->GetOwner() == GetOwner() ? ASC : nullptr;
}

bool UGGYGOMontageFXReplicationComponent::PublishMontageFX(UGGYGOGameplayAbility* OriginalAbility,
	const FGGYGOAbilityActivationHandle& OriginalActivation,
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* OriginalTask, const FGGYGOMontageWindowFact& BeginFact,
	const FGGYGOMontageFXRecord& Request, FGGYGOMontageFXLease& OutLease, FString& OutDiagnostic)
{
	OutLease = {};
	OutDiagnostic.Reset();
	const AActor* Owner = GetOwner();
	const UGGYGOAbilitySystemComponent* AbilityASC = IsValid(OriginalAbility)
		? OriginalAbility->GetGGYGOAbilitySystemComponentFromActorInfo() : nullptr;
	if (!IsInGameThread() || !IsRegistered() || !Owner || !Owner->HasAuthority() || !PublicationOwner
		|| !AbilityASC || AbilityASC->GetOwner() != Owner
		|| !OriginalAbility->CaptureCurrentActivation().HasSameActivation(OriginalActivation)
		|| !IsValid(OriginalTask) || OriginalTask->GetTaskOwner() != OriginalAbility
		|| !OriginalTask->IsOriginalMontageWindowFact(BeginFact)
		|| BeginFact.Phase != EGGYGOMontageWindowPhase::Begin
		|| BeginFact.Mesh != Request.Mesh || !IsValid(BeginFact.Mesh) || !IsValid(Request.Avatar)
		|| BeginFact.Mesh->GetOwner() != Request.Avatar
		|| OriginalAbility->GetAvatarActorFromActorInfo() != Request.Avatar)
	{
		OutDiagnostic = FString::Printf(TEXT("Montage FX 发布缺少权威原宿主/GA/Task 的同步 Begin 来源：Host=%s Ability=%s。"),
			*GetPathName(), *GetPathNameSafe(OriginalAbility));
		return false;
	}
	FGGYGOMontageFXRecord Record = Request;
	Record.BeginServerTime = BeginFact.DispatchWorldTimeSeconds;
	return PublicationOwner->Begin(
		FGGYGOFXSourceIdentity::FromAbilityActivation(OriginalActivation.GetResourceIdentity()),
		Record, OutLease, OutDiagnostic);
}

bool UGGYGOMontageFXReplicationComponent::IsLocomotionAvatarCurrent(const UGGYGOAbilitySystemComponent* ASC,
	const UGGYGOCharacterMovementComponent* OriginalMovement,
	const AActor* OriginalAvatar, const USkeletalMeshComponent* OriginalMesh) const
{
	const UWorld* World = GetWorld();
	if (!IsInGameThread() || !ASC || !ASC->IsRegistered() || !ASC->IsOwnerActorAuthoritative()
		|| !World || World->bIsTearingDown || !IsValid(OriginalMovement) || OriginalMovement->IsBeingDestroyed()
		|| !IsValid(OriginalAvatar) || OriginalAvatar->IsActorBeingDestroyed()
		|| !IsValid(OriginalMesh) || OriginalMesh->IsBeingDestroyed()
		|| OriginalMovement->GetOwner() != OriginalAvatar || OriginalMesh->GetOwner() != OriginalAvatar
		|| OriginalMovement->GetWorld() != World || OriginalMesh->GetWorld() != World)
	{
		return false;
	}
	const FGameplayAbilityActorInfo* ActorInfo = ASC->AbilityActorInfo.Get();
	return ActorInfo && ActorInfo->AbilitySystemComponent.Get() == ASC
		&& ActorInfo->AvatarActor.Get() == OriginalAvatar
		&& ActorInfo->MovementComponent.Get() == OriginalMovement
		&& ActorInfo->SkeletalMeshComponent.Get() == OriginalMesh;
}

bool UGGYGOMontageFXReplicationComponent::PublishLocomotionFX(
	UGGYGOCharacterMovementComponent* OriginalMovement,
	const FGGYGOMotionExecutionSnapshot& CompletedInterval, const FGGYGOMontageFXRecord& Request,
	FGGYGOMontageFXLease& OutLease, FString& OutDiagnostic)
{
	OutLease = {};
	OutDiagnostic.Reset();
	const AActor* Owner = GetOwner();
	UGGYGOAbilitySystemComponent* ASC = GetOwnerAbilitySystem();
	if (!IsInGameThread() || !IsRegistered() || !Owner || !Owner->HasAuthority() || !ASC || !PublicationOwner
		|| !IsValid(OriginalMovement) || OriginalMovement->IsBeingDestroyed())
	{
		OutDiagnostic = FString::Printf(TEXT("Locomotion FX 发布缺少已注册的权威原宿主/ASC/CMC/发行者：Host=%s。"), *GetPathName());
		return false;
	}
	const TWeakObjectPtr<UGGYGOMontageFXPublicationOwner> OriginalPublisher(PublicationOwner.Get());
	FGGYGOLocomotionSourceProof Proof;
	if (!OriginalMovement->TryGetLocomotionSourceProof(CompletedInterval, Proof, OutDiagnostic))
	{
		OutDiagnostic = FString::Printf(TEXT("Locomotion FX 无法认证原 CMC [%s] 的完成区间：%s"),
			*GetPathNameSafe(OriginalMovement), *OutDiagnostic);
		return false;
	}
	UWorld* CompletionWorld = nullptr;
	double CompletionWorldTime = 0.;
	if (!OriginalMovement->TryGetLocomotionCompletionWorldTime(Proof,
		CompletedInterval.CompletedIntervalSerial, CompletedInterval.CompletedFrame,
		CompletionWorld, CompletionWorldTime, OutDiagnostic))
	{
		OutDiagnostic = FString::Printf(TEXT("Locomotion FX 无法认证原 CMC [%s] 的 producer 事件时间：%s"),
			*GetPathNameSafe(OriginalMovement), *OutDiagnostic);
		return false;
	}
	if (!IsValid(CompletionWorld) || CompletionWorld != GetWorld()
		|| !FMath::IsFinite(CompletionWorldTime) || CompletionWorldTime < 0.
		|| CompletionWorldTime > CompletionWorld->GetTimeSeconds())
	{
		OutDiagnostic = FString::Printf(
			TEXT("Locomotion FX 原 producer 事件 World/时间无效：Host=%s World=%s Begin=%.17g。"),
			*GetPathName(), *GetPathNameSafe(CompletionWorld), CompletionWorldTime);
		return false;
	}
	if (!IsLocomotionAvatarCurrent(ASC, OriginalMovement, Request.Avatar.Get(), Request.Mesh.Get())
		|| !OriginalPublisher.IsValid() || PublicationOwner.Get() != OriginalPublisher.Get())
	{
		OutDiagnostic = FString::Printf(
			TEXT("Locomotion FX 原绑定或 Avatar/Movement/Mesh/发行者已变化：Host=%s CMC=%s Avatar=%s Mesh=%s。"),
			*GetPathName(), *GetPathNameSafe(OriginalMovement),
			*GetPathNameSafe(Request.Avatar.Get()), *GetPathNameSafe(Request.Mesh.Get()));
		return false;
	}
	FGGYGOMontageFXRecord Record = Request;
	// 原生 producer 完成事件时间；不是连续 cue 边界映射，也不取本调用的接收时刻。
	Record.BeginServerTime = CompletionWorldTime;
	return OriginalPublisher->Begin(FGGYGOFXSourceIdentity::FromLocomotion(Proof, ASC),
		Record, OutLease, OutDiagnostic);
}
