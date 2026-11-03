#pragma once

#include "AbilitySystem/Abilities/GGYGOGameplayAbility.h"
#include "AbilitySystemInterface.h"
#include "Character/Components/GGYGOHeroComponent.h"
#include "Combatants/GGYGOCombatantState.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/Character.h"
#include "GameFramework/GameModeBase.h"
#include "InputMappingContext.h"
#include "InputTriggers.h"
#include "Player/GGYGOPlayerController.h"

#include "GGYGOInputTestTypes.generated.h"

class UEnhancedInputLocalPlayerSubsystem;
class UEnhancedPlayerInput;
class UGGYGOAbilitySystemComponent;
class UGGYGOInputComponent;
class UGGYGOPawnExtensionComponent;
class UInputAction;
class ULocalPlayer;
class UWorld;

/** 默认只持有独立 WorldContext 与 LocalPlayer，不启动 online／GI 级服务；显式选择后才走原生 Init／Shutdown。 */
UCLASS(Transient)
class UGGYGOInputTestGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	virtual void Init() override;
	virtual void Shutdown() override;
	/** 在 InitializeStandalone 前选择走原生 Init／Shutdown，以启用 GI 子系统（如 MessageRouter）。 */
	void UseNativeLifecycleForTest() { bNativeLifecycle = true; }

private:
	bool bNativeLifecycle = false;
};

/** Runs viewport Init; explicit native mode owns a real hidden window. Player focus/layout notifications remain suppressed. */
UCLASS(Transient)
class UGGYGOInputTestViewport : public UGameViewportClient
{
	GENERATED_BODY()

public:
	virtual void NotifyPlayerAdded(int32 PlayerIndex, ULocalPlayer* AddedPlayer) override {}
	virtual void NotifyPlayerRemoved(int32 PlayerIndex, ULocalPlayer* RemovedPlayer) override {}
};

/** Uses the project's native input birth, without changing project defaults. */
UCLASS(Transient)
class AGGYGOInputTestController : public AGGYGOPlayerController
{
	GENERATED_BODY()

public:
	virtual void InitInputSystem() override;
	/** Records the fixture's native initialization choice before SetPlayer. */
	bool EnableNativeInputInitializationForTest();
	/** Explicit test consumption of the real PC entry; never installs another frame scheduler. */
	void ConsumeInputForTest(float DeltaTime = 0.0f);
	/** 走一次 PC 原生输入帧（PlayerInput Tick → 评价 → PostProcessInput），不另设帧调度。 */
	void TickInputForTest(float DeltaTime);

private:
	bool bUseNativeInputInitialization = false;
};

/** 原生 PIE 宿主使用项目 PC 的输入出生链及 CameraManager 配置。 */
UCLASS(Transient)
class AGGYGOInputTestGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AGGYGOInputTestGameMode(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};

/** Configures the test component's own IMC list; no private session state is exposed. */
UCLASS(Transient)
class UGGYGOInputTestHeroComponent : public UGGYGOHeroComponent
{
	GENERATED_BODY()

public:
	void ConfigureMappingForTest(const UInputMappingContext* Mapping, int32 Priority);
	/** Owns the real typed notice subscription while this fixture deliberately stays before BeginPlay. */
	bool ObserveLocalAbilitySystemForTest(UGGYGOPawnExtensionComponent* Extension, FString& OutError);
	void StopObservingLocalAbilitySystemForTest();
	/** Queries the real original H/input association without exposing its records. */
	bool HasAbilityInputAssociationForTest(const UGGYGOAbilitySystemComponent* ExpectedASC) const;
	bool ObserveNativeMappingRebuildForTest(UEnhancedInputLocalPlayerSubsystem* Subsystem);
	void StopObservingNativeMappingRebuildForTest();
	int32 GetNativeMappingRebuildCountForTest() const { return NativeMappingRebuildCount; }
	double GetFirstNativeMappingRebuildTimeForTest() const { return FirstNativeMappingRebuildTime; }
	bool IsObservingNativeMappingRebuildForTest() const { return OriginalNativeMappingSubsystem.IsValid(); }

private:
	UFUNCTION()
	void OnNativeMappingRebuiltForTest();
	TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> OriginalNativeMappingSubsystem;
	int32 NativeMappingRebuildCount = 0;
	double FirstNativeMappingRebuildTime = 0.0;
};

UCLASS(Transient)
class UGGYGOInputTestMappingContext : public UInputMappingContext
{
	GENERATED_BODY()

public:
	UGGYGOInputTestMappingContext(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};

/** Ordinary input-only probe: no group, retry policy, cost, or business state. */
UCLASS(Transient)
class UGGYGOInputTestAbility : public UGGYGOGameplayAbility
{
	GENERATED_BODY()

public:
	UGGYGOInputTestAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	int32 GetActivationCountForTest() const { return ActivationCount; }

protected:
	virtual void ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
		FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;

private:
	/** Per-instance observation, discarded with the fixture's ASC/ability instance. */
	int32 ActivationCount = 0;
};

/** Concrete fixture configuration; ASC ownership and binding lifecycle remain inherited from the production Host. */
UCLASS(Transient)
class AGGYGOInputTestAbilitySystemHost : public AGGYGOCombatantState
{
	GENERATED_BODY()

public:
	AGGYGOInputTestAbilitySystemHost(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get())
		: Super(ObjectInitializer) {}
};

/** Synthetic world does not begin play; initialization/release use the real public entries. */
UCLASS(Transient)
class AGGYGOInputTestPawn : public ACharacter, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	AGGYGOInputTestPawn(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	/** 与生产 AGGYGOCharacterBase 相同：经 PawnExtension 取 Host 注入的 ASC，注入前为 nullptr。 */
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	UGGYGOInputTestHeroComponent* GetHeroForTest() const { return Hero; }
	UGGYGOPawnExtensionComponent* GetPawnExtensionForTest() const { return PawnExtension; }
	UGGYGOAbilitySystemComponent* GetASCForTest() const;
	UGGYGOInputComponent* GetInputComponentForTest() const;
	void SetAbilitySystemHostForTest(AGGYGOCombatantState* Host);
	/** 首次输入安装前选择 Source 探针；普通夹具仍由完整 Hero 持有输入会话。 */
	bool EnableSourceMovementProbeForTest();

protected:
	virtual UInputComponent* CreatePlayerInputComponent() override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

private:
	UPROPERTY()
	TObjectPtr<UGGYGOPawnExtensionComponent> PawnExtension;
	UPROPERTY()
	TObjectPtr<UGGYGOInputTestHeroComponent> Hero;
	TWeakObjectPtr<AGGYGOCombatantState> AbilitySystemHost;
	bool bSourceMovementProbe = false;
};

#if WITH_DEV_AUTOMATION_TESTS
class FAutomationTestBase;

/** RAII owner of a private context. Accessors never substitute for Hero validity checks.
 * GI-wide services and BeginPlay/InitState are deliberately outside this source gate.
 */
class FGGYGOInputTestFixture
{
public:
	FGGYGOInputTestFixture();
	~FGGYGOInputTestFixture();
	FGGYGOInputTestFixture(const FGGYGOInputTestFixture&) = delete;
	FGGYGOInputTestFixture& operator=(const FGGYGOInputTestFixture&) = delete;

	bool Initialize(FAutomationTestBase& Test);
	/** Also owns the real native window and observes the first actual mapping rebuild. */
	bool InitializeNativeMovementOrigin(FAutomationTestBase& Test);
	void Shutdown();
	/** Initialize 前选择：GI 走原生 Init 以启用子系统。 */
	void RequestGameInstanceSubsystemsForTest();
	/** Initialize 前选择：把能力输入动作映射到一个真实按键。 */
	void MapAbilityKeyForTest(const FKey& Key);
	/**
	 * Initialize 前选择：Pawn 生成后、Possess／Host 绑定前组装 Avatar（Mesh、AnimInstance 等）。
	 * Host 提交的 ActorInfo 快照包含实际 AnimInstance，绑定后再换会使绑定失效，与生产先组装后绑定一致。
	 */
	void PreparePawnBeforeBindingForTest(TFunction<bool(AGGYGOInputTestPawn&)> Preparation);
	/**
	 * 编辑器非 PIE 时 EnhancedInput 模块不逐帧 Tick；同步完成它的帧末两步：立即重建映射，再广播重建通知，
	 * 由 Hero 按生产流程建立移动输入会话。只用于不推进引擎帧的同步夹具。
	 */
	bool CompleteNativeMappingRebuildForTest(FAutomationTestBase& Test);
	/** 经 PlayerInput 注入能力键按下／释放，各走一次 PC 原生输入帧。 */
	bool PressAbilityKeyForTest(FAutomationTestBase& Test);
	bool ReleaseAbilityKeyForTest(FAutomationTestBase& Test);
	UWorld* GetWorld() const;
	UGGYGOInputTestGameInstance* GetGameInstance() const;
	UGameViewportClient* GetViewport() const;
	ULocalPlayer* GetLocalPlayer() const;
	UEnhancedInputLocalPlayerSubsystem* GetInputSubsystem() const;
	UEnhancedPlayerInput* GetPlayerInput() const;
	AGGYGOInputTestController* GetController() const;
	AGGYGOInputTestPawn* GetPawn() const;
	const UInputAction* GetAbilityAction() const;
	const UInputAction* GetMoveAction() const;
	const UInputMappingContext* GetMappingContext() const;

	/** Clones real matching bindings before execution so synchronous removal is safe.
	 * This injects an event at the binding boundary, not device/trigger evaluation.
	 * Clones and FInputActionInstance live only for this call; returns executed binding count.
	 */
	int32 ExecuteAbilityBinding(ETriggerEvent Event);

private:
	bool RequireNativeMovementCold(FAutomationTestBase& Test, const TCHAR* Stage) const;
	bool SendAbilityKeyForTest(FAutomationTestBase& Test, EInputEvent Event);
	struct FState;
	TUniquePtr<FState> State;
};
#endif
