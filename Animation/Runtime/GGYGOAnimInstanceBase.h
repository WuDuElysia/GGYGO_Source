/**
 * @file GGYGOAnimInstanceBase.h
 * @brief 向 AnimBP 发布只读语义帧的项目通用薄基类
 */
#pragma once

#include "CoreMinimal.h"
#include "Animation/Data/GGYGOLocomotionSourceBinding.h"
#include "Animation/Runtime/GGYGOMontageGuardAnimInstance.h"
#include "Animation/Debug/GGYGOAnimationDebugFrame.h"
#include "Animation/Runtime/GGYGOAnimationStateCapture.h"
#include "Animation/Runtime/GGYGOAnimationStateFrame.h"

#include "GGYGOAnimInstanceBase.generated.h"

class ACharacter;
class UGGYGOCharacterMovementComponent;

/**
 * 不知道任何 AnimBP 状态名或过渡拓扑，只负责游戏线程抓取与只读发布。
 */
UCLASS(Abstract, BlueprintType, Blueprintable)
class GGYGO_API UGGYGOAnimInstanceBase : public UGGYGOMontageGuardAnimInstance
{
	GENERATED_BODY()

public:
	virtual void NativeInitializeAnimation() override;
	virtual void NativeUninitializeAnimation() override;
	virtual void NativeUpdateAnimation(float DeltaSeconds) override;

	/** 线程安全地查询本帧是否具有某个 ASC 状态 Tag。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Animation", meta = (BlueprintThreadSafe))
	bool HasAnimationStateTag(FGameplayTag Tag) const;

	/** 当前权威步态是否为 Run。只描述语义事实，不知道 AnimBP 的状态拓扑。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Animation", meta = (BlueprintThreadSafe))
	bool IsAnimationRunGait() const;

	/** 转身是否处于 Turning / Braking 的曲线接管段（明确排除 RunOut）。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Animation", meta = (BlueprintThreadSafe))
	bool IsTurnBackCurveDriven() const;

	const FGGYGOAnimationStateFrame& GetAnimationStateFrame() const { return AnimationState; }

protected:
	/** C++ 权威层发布的一帧只读语义事实。 */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "GGYGO|Animation")
	FGGYGOAnimationStateFrame AnimationState;

	/** 生命周期复位时清理派生 AnimInstance 自己持有的运行时表现状态。 */
	virtual void OnAnimationLifecycleReset();

	/** Resolve configured assets only. Curve evaluation and all motion clocks belong to Movement. */
	virtual void ResolveLocomotionSourceBinding(FGGYGOLocomotionSourceBinding& OutBinding) const;
	/** Compare current configuration against the last immutable publication without allocating asset holders. */
	virtual bool IsLocomotionSourceConfigurationCurrent(const FGGYGOLocomotionSourceBinding& Binding) const;

	const FGGYGOAnimationDebugFrame& GetAnimationDebugFrame() const { return AnimationDebug; }

private:
	/** 幂等地清理基类运行时状态并通知派生表现层。 */
	void ResetAnimationLifecycleState();
	void RefreshAnimationStateFrame();
	void RefreshLocomotionSourceBinding();
	void RetireLocomotionSourceBinding();

	TWeakObjectPtr<ACharacter> CharacterOwner;
	/** 记录上次绑定过有效 Character，避免弱引用失效后无法识别 nullptr -> nullptr。 */
	bool bHadCharacterOwner = false;
	FGGYGOAnimationStateCapture StateCapture;
	FGGYGOAnimationDebugFrame AnimationDebug;
	/** Only the publication/resource handle is owned here; no movement state is mirrored. */
	FGGYGOLocomotionSourceBindingPtr PublishedLocomotionSource;
	TWeakObjectPtr<UGGYGOCharacterMovementComponent> LocomotionSourceConsumer;
	uint64 AnimationLifecycleGeneration = 0;
	/** Never reset across reinitialization: an old retirement cannot match a later publication. */
	uint64 LocomotionSourceConfigurationGeneration = 0;
	bool bSourceGenerationExhausted = false;
};
