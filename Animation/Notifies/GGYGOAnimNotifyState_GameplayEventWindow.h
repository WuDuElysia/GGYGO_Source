/** @file GGYGOAnimNotifyState_GameplayEventWindow.h @brief Montage 时窗到 GameplayEvent 的通用桥 */
#pragma once

#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "GameplayTagContainer.h"

#include "GGYGOAnimNotifyState_GameplayEventWindow.generated.h"

class AActor;
struct FGameplayEventData;
struct FGGYGOMontageWindowSourceData;

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

	/** @return Authored Begin tag copied by value; not a runtime window occurrence. */
	FGameplayTag GetBeginEventTag() const { return BeginEventTag; }

	/** @return Authored End tag copied by value; not a runtime window occurrence. */
	FGameplayTag GetEndEventTag() const { return EndEventTag; }

	/**
	 * 仅在本桥同步发送 GameplayEvent 的调用栈内复制原窗口值。
	 * 引擎按值复制 Payload，因此核原字段而非地址；返回身份只供公共 Task 去重，
	 * 不认证窗口、不授予播放或工作许可，调用栈退出后不可继续读取。
	 */
	static bool TryReadWindowDispatch(const FGameplayEventData* Payload,
		FGGYGOMontageWindowSourceData& OutSource);

	virtual void NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
		float TotalDuration, const FAnimNotifyEventReference& EventReference) override;
	virtual void NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
		const FAnimNotifyEventReference& EventReference) override;
	virtual void BranchingPointNotifyBegin(FBranchingPointNotifyPayload& BranchingPointPayload) override;
	virtual void BranchingPointNotifyEnd(FBranchingPointNotifyPayload& BranchingPointPayload) override;

protected:
	void SendEvent(FGameplayTag EventTag, const FGGYGOMontageWindowSourceData& Source,
		const TWeakObjectPtr<AActor>& OriginalOwner, float LegacyEventMagnitude) const;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gameplay Event")
	FGameplayTag BeginEventTag;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gameplay Event")
	FGameplayTag EndEventTag;
};
