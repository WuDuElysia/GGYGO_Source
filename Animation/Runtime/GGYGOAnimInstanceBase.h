/**
 * @file GGYGOAnimInstanceBase.h
 * @brief 向 AnimBP 发布只读语义帧的项目通用薄基类
 */
#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/Debug/GGYGOAnimationDebugFrame.h"
#include "Animation/Runtime/GGYGOAnimationStateCapture.h"
#include "Animation/Runtime/GGYGOAnimationStateFrame.h"

#include "GGYGOAnimInstanceBase.generated.h"

class ACharacter;

/**
 * 不知道任何 AnimBP 状态名或过渡拓扑，只负责游戏线程抓取与只读发布。
 */
UCLASS(Abstract, BlueprintType, Blueprintable)
class GGYGO_API UGGYGOAnimInstanceBase : public UAnimInstance
{
	GENERATED_BODY()

public:
	virtual void NativeInitializeAnimation() override;
	virtual void NativeUpdateAnimation(float DeltaSeconds) override;

	/** 线程安全地查询本帧是否具有某个 ASC 状态 Tag。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Animation", meta = (BlueprintThreadSafe))
	bool HasAnimationStateTag(FGameplayTag Tag) const;

	const FGGYGOAnimationStateFrame& GetAnimationStateFrame() const { return AnimationState; }

protected:
	/** C++ 权威层发布的一帧只读语义事实。 */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "GGYGO|Animation")
	FGGYGOAnimationStateFrame AnimationState;

	const FGGYGOAnimationDebugFrame& GetAnimationDebugFrame() const { return AnimationDebug; }

private:
	void RefreshAnimationStateFrame();

	TWeakObjectPtr<ACharacter> CharacterOwner;
	FGGYGOAnimationStateCapture StateCapture;
	FGGYGOAnimationDebugFrame AnimationDebug;
};
