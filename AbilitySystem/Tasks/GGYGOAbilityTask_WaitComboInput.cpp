/** @file GGYGOAbilityTask_WaitComboInput.cpp */
#include "AbilitySystem/Tasks/GGYGOAbilityTask_WaitComboInput.h"
#include "AbilitySystemComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOAbilityTask_WaitComboInput)

UGGYGOAbilityTask_WaitComboInput* UGGYGOAbilityTask_WaitComboInput::WaitComboInput(UGameplayAbility* OwningAbility)
{
	return NewAbilityTask<UGGYGOAbilityTask_WaitComboInput>(OwningAbility);
}

void UGGYGOAbilityTask_WaitComboInput::Activate()
{
	Super::Activate();
	if (UAbilitySystemComponent* ASC = AbilitySystemComponent.Get())
	{
		InputHandle = ASC->AbilityReplicatedEventDelegate(EAbilityGenericReplicatedEvent::InputPressed,
			GetAbilitySpecHandle(), GetActivationPredictionKey()).AddUObject(this, &ThisClass::HandlePress);
		// 不测试 Spec.InputPressed，首次激活的按下不算下一段请求。
		if (IsForRemoteClient() && !ASC->CallReplicatedEventDelegateIfSet(
			EAbilityGenericReplicatedEvent::InputPressed, GetAbilitySpecHandle(), GetActivationPredictionKey()))
		{
			SetWaitingOnRemotePlayerData();
		}
	}
}

void UGGYGOAbilityTask_WaitComboInput::HandlePress()
{
	UAbilitySystemComponent* ASC = AbilitySystemComponent.Get();
	if (!ASC || !ShouldBroadcastAbilityTaskDelegates()) { return; }
	FScopedPredictionWindow Prediction(ASC, IsPredictingClient());
	int32 RequestStep = SourceStep;
	int32 RequestId = 0;
	if (IsForRemoteClient())
	{
		const FAbilityReplicatedData Data = ASC->GetReplicatedDataOfGenericReplicatedEvent(
			EAbilityGenericReplicatedEvent::InputPressed, GetAbilitySpecHandle(), GetActivationPredictionKey());
		// GAS 通用事件提供 VectorPayload；这里只传精确的小整数 X=段序、Y=请求序号。
		const FVector& Payload = Data.VectorPayload;
		if (!FMath::IsFinite(Payload.X) || !FMath::IsFinite(Payload.Y)
			|| Payload.X < 0.0 || Payload.X > 65535.0 || Payload.Y < 1.0 || Payload.Y > 65535.0
			|| Payload.X != FMath::FloorToDouble(Payload.X) || Payload.Y != FMath::FloorToDouble(Payload.Y))
		{
			ASC->ConsumeGenericReplicatedEvent(EAbilityGenericReplicatedEvent::InputPressed,
				GetAbilitySpecHandle(), GetActivationPredictionKey());
			return;
		}
		RequestStep = FMath::RoundToInt(Data.VectorPayload.X);
		RequestId = FMath::RoundToInt(Data.VectorPayload.Y);
	}
	else
	{
		RequestId = ++LocalRequestId;
		if (IsPredictingClient())
		{
			ASC->ServerSetReplicatedEventWithPayload(EAbilityGenericReplicatedEvent::InputPressed,
				GetAbilitySpecHandle(), GetActivationPredictionKey(), ASC->ScopedPredictionKey,
				FVector(RequestStep, RequestId, 0.0));
		}
	}
	// 先消费再通知 GA，防止换段或结束中的重入重复消费同一事件。
	ASC->ConsumeGenericReplicatedEvent(EAbilityGenericReplicatedEvent::InputPressed,
		GetAbilitySpecHandle(), GetActivationPredictionKey());
	OnPress.Broadcast(RequestStep, RequestId);
}

void UGGYGOAbilityTask_WaitComboInput::OnDestroy(bool AbilityEnded)
{
	if (UAbilitySystemComponent* ASC = AbilitySystemComponent.Get())
	{
		ASC->AbilityReplicatedEventDelegate(EAbilityGenericReplicatedEvent::InputPressed,
			GetAbilitySpecHandle(), GetActivationPredictionKey()).Remove(InputHandle);
	}
	Super::OnDestroy(AbilityEnded);
}
