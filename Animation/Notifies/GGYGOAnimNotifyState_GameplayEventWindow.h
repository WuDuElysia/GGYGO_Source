/** @file GGYGOAnimNotifyState_GameplayEventWindow.h @brief Montage 时窗到 GameplayEvent 的通用桥 */
#pragma once

#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "GameplayTagContainer.h"

#include "GGYGOAnimNotifyState_GameplayEventWindow.generated.h"

/** NotifyState 只发事件；Trace、伤害与能力结束均由当前 GA 决定。 */
UCLASS(meta = (DisplayName = "GGYGO Gameplay Event Window"))
class GGYGO_API UGGYGOAnimNotifyState_GameplayEventWindow : public UAnimNotifyState
{
	GENERATED_BODY()

public:
	void InitializeEventTags(FGameplayTag InBeginEventTag, FGameplayTag InEndEventTag)
	{
		BeginEventTag = InBeginEventTag;
		EndEventTag = InEndEventTag;
	}

	virtual void NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
		float TotalDuration, const FAnimNotifyEventReference& EventReference) override;
	virtual void NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
		const FAnimNotifyEventReference& EventReference) override;

protected:
	void SendEvent(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, FGameplayTag EventTag) const;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gameplay Event")
	FGameplayTag BeginEventTag;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gameplay Event")
	FGameplayTag EndEventTag;
};
