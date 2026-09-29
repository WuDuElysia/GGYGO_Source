/** @file GGYGOAnimNotifyState_GameplayEventWindow.cpp */
#include "Animation/Notifies/GGYGOAnimNotifyState_GameplayEventWindow.h"

#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "Animation/ActiveMontageInstanceScope.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOAnimNotifyState_GameplayEventWindow)

void UGGYGOAnimNotifyState_GameplayEventWindow::NotifyBegin(
	USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration,
	const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyBegin(MeshComp, Animation, TotalDuration, EventReference);
	SendEvent(MeshComp, Animation, BeginEventTag, EventReference);
}

void UGGYGOAnimNotifyState_GameplayEventWindow::NotifyEnd(
	USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
	const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyEnd(MeshComp, Animation, EventReference);
	SendEvent(MeshComp, Animation, EndEventTag, EventReference);
}

void UGGYGOAnimNotifyState_GameplayEventWindow::SendEvent(
	USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, FGameplayTag EventTag,
	const FAnimNotifyEventReference& EventReference) const
{
	AActor* Owner = MeshComp ? MeshComp->GetOwner() : nullptr;
	if (!Owner || !EventTag.IsValid())
	{
		return;
	}
	if (!UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Owner))
	{
		// Persona 等预览 Actor 可以没有 ASC；游戏世界缺少接收者仍需要诊断。
		const UWorld* World = Owner->GetWorld();
		if (World && World->IsGameWorld())
		{
			UE_LOG(LogGGYGOAbilitySystem, Warning,
				TEXT("GameplayEventWindow：游戏 Actor [%s] 没有 ASC，无法发送 [%s]，动画 [%s]。"),
				*Owner->GetPathName(), *EventTag.ToString(), *GetNameSafe(Animation));
		}
		return;
	}

	FGameplayEventData Payload;
	Payload.EventTag = EventTag;
	Payload.Instigator = Owner;
	Payload.Target = Owner;
	Payload.OptionalObject = Animation;
	Payload.OptionalObject2 = MeshComp;
	if (const UE::Anim::FAnimNotifyMontageInstanceContext* Context = EventReference.GetContextData<UE::Anim::FAnimNotifyMontageInstanceContext>())
	{
		Payload.EventMagnitude = static_cast<float>(Context->MontageInstanceID + 1);
	}
	UAbilitySystemBlueprintLibrary::SendGameplayEventToActor(Owner, EventTag, Payload);
}
