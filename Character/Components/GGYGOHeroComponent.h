/**
 * @file GGYGOHeroComponent.h
 * @brief 被玩家操控的单位才挂的组件 —— 负责输入与相机模式仲裁
 *
 * "Hero" 指的是**被玩家操控**，不是"英雄角色"。AI 控制的敌人不挂它，
 * 因此不必为"这个单位需不需要读手柄"写运行时判断 —— 它根本没有这个组件。
 *
 * 队伍换角色时正好利用这一点：只有当前出战的角色挂着能响应输入的组件。
 *
 * ## 职责边界
 * 输入侧注册 IMC、把输入绑到 Native 函数或翻译成 InputTag；
 * 相机侧只仲裁“能力覆盖 / PawnData 默认模式”，不自己 Tick、不计算视角。
 * 它**不消费** ASC 的输入缓存 —— 那需要 `PostProcessInput` 的时机，
 * 只有 PlayerController 有（见 `AGGYGOPlayerController`）。
 *
 * ## 为什么它是一个 InitState feature
 * 输入初始化依赖两件事都就位：PawnData（里面有 InputConfig）与 PlayerController
 * （IMC 要注册到它的 LocalPlayer 上）。这两者的到达顺序在联机下不固定，
 * 所以本组件注册成 feature，由 `UGameFrameworkComponentManager` 协调，
 * 而不是在 BeginPlay 里假定某个顺序。
 */
#pragma once

#include "Components/GameFrameworkInitStateInterface.h"
#include "Components/PawnComponent.h"
#include "GameplayAbilitySpecHandle.h"

#include "GGYGOHeroComponent.generated.h"

namespace EEndPlayReason { enum Type : int; }

class UGameFrameworkComponentManager;
class UGGYGOCameraMode;
class UGGYGOInputConfig;
class UInputComponent;
class UObject;
struct FActorInitStateChangedParams;
struct FGameplayTag;
struct FInputActionValue;

/** 一条仍处于激活状态的能力相机覆盖；数组顺序就是覆盖先后顺序。 */
USTRUCT()
struct FGGYGOAbilityCameraModeOverride
{
	GENERATED_BODY()

	UPROPERTY()
	TSubclassOf<UGGYGOCameraMode> CameraMode;

	FGameplayAbilitySpecHandle OwningSpecHandle;
};

UCLASS(meta = (BlueprintSpawnableComponent))
class GGYGO_API UGGYGOHeroComponent : public UPawnComponent, public IGameFrameworkInitStateInterface
{
	GENERATED_BODY()

public:
	UGGYGOHeroComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/** 本 feature 在 InitState 系统里的名字。 */
	static const FName NAME_ActorFeatureName;

	//~IGameFrameworkInitStateInterface interface
	virtual FName GetFeatureName() const override { return NAME_ActorFeatureName; }
	virtual bool CanChangeInitState(UGameFrameworkComponentManager* Manager, FGameplayTag CurrentState, FGameplayTag DesiredState) const override;
	virtual void HandleChangeInitState(UGameFrameworkComponentManager* Manager, FGameplayTag CurrentState, FGameplayTag DesiredState) override;
	virtual void OnActorInitStateChanged(const FActorInitStateChangedParams& Params) override;
	virtual void CheckDefaultInitialization() override;
	//~End of IGameFrameworkInitStateInterface interface

	/** 取某个 Actor 上的本组件。没有则返回 nullptr。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Hero")
	static UGGYGOHeroComponent* FindHeroComponent(const AActor* Actor)
	{
		return Actor ? Actor->FindComponentByClass<UGGYGOHeroComponent>() : nullptr;
	}

	/** 由拥有者 Pawn 在 `SetupPlayerInputComponent` 里调用。 */
	void InitializePlayerInput(UInputComponent* PlayerInputComponent);

	/** 返回当前有效的相机模式：能力覆盖优先，否则使用 PawnData 默认模式。 */
	TSubclassOf<UGGYGOCameraMode> DetermineCameraMode() const;

	/** 由能力登记临时相机模式；同一 Spec 再登记会更新并移到覆盖栈顶。 */
	void SetAbilityCameraMode(TSubclassOf<UGGYGOCameraMode> CameraMode, const FGameplayAbilitySpecHandle& OwningSpecHandle);

	/** 移除指定能力的覆盖；若它在栈顶，下一条仍激活的覆盖会自然恢复。 */
	void ClearAbilityCameraMode(const FGameplayAbilitySpecHandle& OwningSpecHandle);

	/**
	 * 输入缓冲的有效时长（秒）。
	 *
	 * 请求被"组内已有实例"拒绝后会被缓冲这么久，期间一旦该组空出就立刻重试。
	 * 这是连段手感的核心参数：太短会让玩家必须精确卡在动画末尾按键，
	 * 太长会让早按的键在很久之后突然生效，玩家已经不预期它了。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input", meta = (ClampMin = "0.0", ForceUnits = "s"))
	float InputBufferWindow = 0.35f;

protected:
	virtual void OnRegister() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// ===== Native 输入处理 =====

	/** 移动。把摇杆的 2D 值按摄像机水平朝向解析成世界方向。 */
	void Input_Move(const FInputActionValue& InputActionValue);

	/** 鼠标视角。鼠标已经是像素增量，不乘 DeltaTime。 */
	void Input_LookMouse(const FInputActionValue& InputActionValue);

	/** 手柄视角。摇杆是持续量，需要乘 DeltaTime 才能与帧率无关。 */
	void Input_LookStick(const FInputActionValue& InputActionValue);

	// ===== Ability 输入处理 =====

	/** 转交给 ASC 的输入缓存，不在这里激活能力。 */
	void Input_AbilityInputTagPressed(FGameplayTag InputTag);

	/** 同上。 */
	void Input_AbilityInputTagReleased(FGameplayTag InputTag);

	/**
	 * 输入映射上下文（IMC）。
	 *
	 * 配在组件上而不是 PawnData 里，因为 IMC 属于"这个单位怎么被操控"，
	 * 而 PawnData 描述的是"这个单位是什么"。同一份 PawnData 在玩家操控与
	 * AI 操控下应当是同一份，区别只在有没有挂本组件。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TArray<TObjectPtr<const class UInputMappingContext>> DefaultInputMappings;

	/** IMC 的优先级。多个上下文同时激活时数值大的先匹配。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	int32 InputMappingPriority = 0;

	/** 订阅 ASC 的重试通知与组空出通知。ASC 就绪后调用。 */
	void BindAbilityRetryDelegates();

	/** 缓冲一个被拒的请求。同一个 InputTag 重复缓冲只刷新时间戳。 */
	void BufferAbilityInput(FGameplayTag InputTag);

	/** 某个能力组空出，重试缓冲中的请求。 */
	void HandleAbilityGroupFreed(FGameplayTag GroupTag);

private:
	/** 仍处于激活状态的能力相机覆盖，最后一项优先级最高。 */
	UPROPERTY(Transient)
	TArray<FGGYGOAbilityCameraModeOverride> AbilityCameraModeOverrides;

	/** 本组件产生的 Ability 输入绑定句柄，用于整批解绑。 */
	TArray<uint32> AbilityInputBindHandles;

	/** 一条被缓冲的输入请求。 */
	struct FBufferedInput
	{
		FGameplayTag InputTag;

		/** 缓冲开始的世界时间。用绝对时间而非倒计时，避免每帧递减。 */
		float BufferedAtTime = 0.0f;
	};

	/**
	 * 被缓冲的输入请求。
	 *
	 * 用数组而不是单个槽位：玩家可能在一段攻击播放期间先按普攻再按闪避，
	 * 两者属于不同的能力组，各自的空出时机也不同。只留一个槽位会丢掉其中一个。
	 */
	TArray<FBufferedInput> BufferedInputs;

	/** 委托是否已订阅。ASC 可能多次就绪（换 Avatar），避免重复订阅。 */
	bool bAbilityRetryDelegatesBound = false;
};
