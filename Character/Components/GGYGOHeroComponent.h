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
 * 输入侧持有原绑定、IMC 与 Action 观察，只向原 ASC 接收/结束请求；
 * 请求等待、Queued 许可、held 聚合和重试执行由 ASC 唯一持有。
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

#include "AbilitySystem/GGYGOAbilityInputRequestTypes.h"

#include "Components/GameFrameworkInitStateInterface.h"
#include "Components/PawnComponent.h"
#include "GameplayAbilitySpecHandle.h"
#include "Templates/SharedPointer.h"

#include "GGYGOHeroComponent.generated.h"

namespace EEndPlayReason { enum Type : int; }

class APawn;
class UGameFrameworkComponentManager;
class UEnhancedInputLocalPlayerSubsystem;
class UEnhancedPlayerInput;
class UGGYGOAbilitySystemComponent;
class UGGYGOCameraMode;
class UGGYGOCharacterMovementComponent;
class UGGYGOInputComponent;
class UGGYGOHeroMovementMappingObserver;
struct FGGYGOHeroMovementInputScope;
class UGGYGOInputConfig;
class UInputAction;
class UInputComponent;
class UInputMappingContext;
class UObject;
class UGGYGOPawnExtensionComponent;
struct FGGYGOPawnASCLocalNotice;
struct FActorInitStateChangedParams;
struct FGameplayTag;
struct FInputActionInstance;
struct FInputActionValue;

/** 一条仍处于激活状态的能力相机覆盖；数组顺序就是覆盖先后顺序。 */
USTRUCT()
struct FGGYGOAbilityCameraModeOverride
{
	GENERATED_BODY()

	UPROPERTY()
	TSubclassOf<UGGYGOCameraMode> CameraMode;

	FGameplayAbilitySpecHandle OwningSpecHandle;
	UPROPERTY()
	uint64 RequestGeneration = 0;
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

	/** 幂等退出本组件输入会话；只归还原组件上的绑定和自己增加的 IMC 注册。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Input")
	void ReleasePlayerInput();

	/** 返回当前有效的相机模式：能力覆盖优先，否则使用 PawnData 默认模式。 */
	TSubclassOf<UGGYGOCameraMode> DetermineCameraMode() const;

	/** 由能力登记临时相机模式；返回本次请求的单调代次。 */
	uint64 SetAbilityCameraMode(TSubclassOf<UGGYGOCameraMode> CameraMode, const FGameplayAbilitySpecHandle& OwningSpecHandle);

	/** 仅移除 Spec 与请求代次都匹配的覆盖。 */
	bool ClearAbilityCameraMode(const FGameplayAbilitySpecHandle& OwningSpecHandle, uint64 RequestGeneration);

	/**
	 * 输入缓冲的有效时长（秒）。
	 *
	 * 从原 Action 首次 Triggered 起算；请求被组占用拒绝后，只在原窗口内等待组释放重试。
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

	/** 强制步行按下/释放；输入状态由 CMC 参与预测。 */
	void Input_ForceWalkPressed();
	void Input_ForceWalkReleased();

	/** 鼠标视角。鼠标已经是像素增量，不乘 DeltaTime。 */
	void Input_LookMouse(const FInputActionValue& InputActionValue);

	/** 手柄视角。摇杆是持续量，需要乘 DeltaTime 才能与帧率无关。 */
	void Input_LookStick(const FInputActionValue& InputActionValue);

	// ===== Ability 输入处理 =====

	// Ability 回调只由下方记录原来源的实例委托进入，不提供无绑定身份的 Tag 入口。

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

private:
	/** Continues the native entry after its camera-provider boundary was qualified on this stack. */
	void InitializePlayerInputBindings(UInputComponent* PlayerInputComponent);
	/** Consumes actual local provider readiness; returns whether this original native init may continue.
	 *  Camera failure is reported separately and does not become an input readiness gate. */
	bool ConsumeLocalCameraProviderReady(APawn* ExpectedPawn, const TCHAR* NativeEntry);

	/** 仍处于激活状态的能力相机覆盖，最后一项优先级最高。 */
	UPROPERTY(Transient)
	TArray<FGGYGOAbilityCameraModeOverride> AbilityCameraModeOverrides;

	/** 全生命周期单调递增；不随解绑或 EndPlay 重置。0 表示无有效请求。 */
	uint64 LastAbilityCameraModeRequestGeneration = 0;

	struct FPlayerInputSession;
	struct FAbilityInputAssociation;
	struct FAbilityActionBinding;
	struct FAbilityInputObservation;
	struct FLocalAbilitySystemSubscription;
	/** Owns this original input component's bindings, IMC registrations and Action associations. */
	TSharedPtr<FPlayerInputSession> PlayerInputSession;
	/** 只用于使同步回调中的旧初始化失效；不保存物理输入事实。 */
	uint64 InputSessionGeneration = 0;
	bool bEndingPlay = false;

	friend class UGGYGOHeroMovementMappingObserver;
	/** Own registration/session/binding resources only; Source and CMC remain their authorities. */
	TSharedPtr<FGGYGOHeroMovementInputScope> MovementInputScope;
	UPROPERTY(Transient)
	TObjectPtr<UGGYGOHeroMovementMappingObserver> MovementMappingObserver;
	bool IsMovementInputScopeCurrent(const TSharedPtr<FGGYGOHeroMovementInputScope>& Scope) const;
	void HandleMovementMappingsRebuilt(const TSharedPtr<FGGYGOHeroMovementInputScope>& OriginalScope);
	void Input_Move(const FInputActionInstance& Instance, const TSharedPtr<FGGYGOHeroMovementInputScope>& OriginalScope);

	bool IsAbilityInputAssociationCurrent(const TSharedPtr<FAbilityInputAssociation>& Association) const;
	bool HasValidPlayerInputSession() const;
	void AssociateReadyAbilitySystemWithInput();
	/** Retires the original association locally; returned IDs still require exact ASC cleanup. */
	TArray<FGGYGOAbilityInputRequestIdentity> RetireAbilityInputAssociation(const TSharedPtr<FPlayerInputSession>& Session);
	void PruneEndedAbilityInputObservations(double Now);
	bool IsAbilityActionBindingCurrent(const TSharedPtr<FAbilityActionBinding>& Binding) const;
	void Input_AbilityActionTriggered(const FInputActionInstance& ActionInstance,
		const TSharedPtr<FAbilityActionBinding>& Binding);
	void Input_AbilityActionReleased(const FInputActionInstance& ActionInstance,
		const TSharedPtr<FAbilityActionBinding>& Binding);
	/** Seals records and removes only their own session membership before any cleanup callout. */
	static TArray<FGGYGOAbilityInputRequestIdentity> RetireAbilityInputObservations(
		const TArray<TSharedPtr<FAbilityInputObservation>>& Observations);
	void InvalidateAbilityInputObservations(const TArray<TSharedPtr<FAbilityInputObservation>>& Observations);
	void InvalidateAbilityActionBinding(const TSharedPtr<FAbilityActionBinding>& Binding);

protected:
	/** Register before replay; Ready associates only an already established input session. */
	bool PrepareLocalAbilitySystemSubscription(UGGYGOPawnExtensionComponent* Extension, FString& OutError);
	/** Derived query of the exact consumed resource; no input or movement permission. */
	UGGYGOAbilitySystemComponent* GetReadyLocalAbilitySystemComponent() const;
	/** @return ASC of this input session's exact Ready H association, or nullptr when unavailable.
	 *  Read-only: never follows a successor ASC or changes input/held state. */
	UGGYGOAbilitySystemComponent* GetInputSessionAbilitySystem() const;
	/** Retire this original notice record and its own accepted input requests. */
	void ReleaseLocalAbilitySystemSubscription();

private:
	void ConsumeLocalAbilitySystemNotice(
		const TSharedPtr<FLocalAbilitySystemSubscription>& ExpectedSubscription,
		const FGGYGOPawnASCLocalNotice& Notice);
	TSharedPtr<FLocalAbilitySystemSubscription> LocalAbilitySystemSubscription;
};

/** Original context for the native parameterless dynamic notification; never a movement source. */
UCLASS(Transient)
class UGGYGOHeroMovementMappingObserver : public UObject
{
	GENERATED_BODY()

public:
	void Initialize(UGGYGOHeroComponent* Hero, UEnhancedInputLocalPlayerSubsystem* Subsystem,
		const TSharedPtr<FGGYGOHeroMovementInputScope>& Scope);
	void Detach();
	virtual void BeginDestroy() override;

private:
	UFUNCTION()
	void OnMappingsRebuilt();

	TWeakObjectPtr<UGGYGOHeroComponent> OriginalHero;
	TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> OriginalSubsystem;
	TSharedPtr<FGGYGOHeroMovementInputScope> OriginalScope;
};
