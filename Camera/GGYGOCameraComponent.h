/**
 * @file GGYGOCameraComponent.h
 * @brief 由相机模式栈驱动的相机组件
 *
 * 取代直接用 `USpringArmComponent` + `UCameraComponent` 的常见做法。
 * 差别在于视角来源：这里的视角由 `UGGYGOCameraModeStack` 求值，
 * 因此能力可以临时推入模式并自动获得平滑过渡，而不必去改弹簧臂的参数
 * 再想办法恢复。
 *
 * ## 默认模式的来源
 * 栈底必须始终有一个模式，否则没有视角可算。默认模式通过
 * `DetermineCameraModeDelegate` 由外部提供 —— 通常是 HeroComponent，
 * 它知道当前角色的 PawnData 里配了哪个模式。
 *
 * 做成委托而不是组件上的一个字段，是因为默认模式会随状态变化：
 * 载具、潜行、锁定各有默认视角，而这些状态归各自的系统管，相机组件不该知道。
 */
#pragma once

#include "Camera/CameraComponent.h"

#include "GGYGOCameraComponent.generated.h"

class AActor;
class UGGYGOCameraMode;
class UGGYGOCameraModeStack;
class UObject;

/** 由外部决定当前该用哪个相机模式。返回空表示保持现状。 */
DECLARE_DELEGATE_RetVal(TSubclassOf<UGGYGOCameraMode>, FGGYGODetermineCameraModeSignature);

UCLASS(meta = (BlueprintSpawnableComponent))
class GGYGO_API UGGYGOCameraComponent : public UCameraComponent
{
	GENERATED_BODY()

public:
	UGGYGOCameraComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/** 取某个 Actor 上的本组件。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Camera")
	static UGGYGOCameraComponent* FindCameraComponent(const AActor* Actor)
	{
		return Actor ? Actor->FindComponentByClass<UGGYGOCameraComponent>() : nullptr;
	}

	/** 视角跟随的目标。默认是组件拥有者。 */
	virtual AActor* GetTargetActor() const { return GetOwner(); }

	/** 由外部提供默认（栈底）相机模式。 */
	FGGYGODetermineCameraModeSignature DetermineCameraModeDelegate;

	/**
	 * 临时推入一个相机模式。
	 *
	 * 能力激活期间调用，能力结束时不需要显式弹出 —— 停止推入后，
	 * 默认模式会在下一帧重新被推到栈顶并混合回去。
	 */
	void PushCameraMode(TSubclassOf<UGGYGOCameraMode> CameraModeClass);

	/** 清空模式栈。切换 Avatar 时调用。 */
	void ClearCameraModeStack();

protected:
	virtual void OnRegister() override;

	/** 引擎每帧取视角的入口。在这里求值模式栈。 */
	virtual void GetCameraView(float DeltaTime, FMinimalViewInfo& DesiredView) override;

	/** 把默认模式推到栈底。 */
	virtual void UpdateCameraModes();

	/** 相机模式栈。 */
	UPROPERTY()
	TObjectPtr<UGGYGOCameraModeStack> CameraModeStack;
};
