/** @file GGYGOAnimNotifyState_GameplayEventWindow.cpp */
#include "Animation/Notifies/GGYGOAnimNotifyState_GameplayEventWindow.h"

#include "AbilitySystemBlueprintLibrary.h"
#include "Components/SkeletalMeshComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOAnimNotifyState_GameplayEventWindow)

void UGGYGOAnimNotifyState_GameplayEventWindow::NotifyBegin(
	USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration,
	const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyBegin(MeshComp, Animation, TotalDuration, EventReference);
	SendEvent(MeshComp, Animation, BeginEventTag);
}

void UGGYGOAnimNotifyState_GameplayEventWindow::NotifyEnd(
	USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
	const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyEnd(MeshComp, Animation, EventReference);
	SendEvent(MeshComp, Animation, EndEventTag);
}

void UGGYGOAnimNotifyState_GameplayEventWindow::SendEvent(
	USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, FGameplayTag EventTag) const
{
	AActor* Owner = MeshComp ? MeshComp->GetOwner() : nullptr;
	if (!Owner || !EventTag.IsValid())
	{
		return;
	}

	FGameplayEventData Payload;
	Payload.EventTag = EventTag;
	Payload.Instigator = Owner;
	Payload.Target = Owner;
	Payload.OptionalObject = Animation;
	UAbilitySystemBlueprintLibrary::SendGameplayEventToActor(Owner, EventTag, Payload);
}
