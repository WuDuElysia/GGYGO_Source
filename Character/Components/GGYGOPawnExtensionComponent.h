/**
 * @file GGYGOPawnExtensionComponent.h
 * @brief Pawn 初始化协调者
 *
 * 本组件**不实现任何玩法**。它只做两件事：
 *   1. 持有 PawnData 与 ASC 的引用，作为其它组件获取这两者的唯一入口
 *   2. 驱动 InitState 状态机，让各组件不必互相知道彼此的初始化顺序
 *
 * ## 为什么需要一个专门的协调者
 * 角色初始化的依赖是网状的：HealthComponent 要等 ASC 就位才能绑定 AttributeSet 委托；
 * ASC 要等 PawnData 才知道授予哪些能力；PawnData 在客户端要等复制到达；
 * 而 Controller 的到达时机在服务器/客户端/本地控制三种情况下各不相同。
 *
 * 手工排序的写法（在 BeginPlay 里按固定顺序调用）会在联机下崩掉，因为顺序不再固定。
 * 常见的补救是各组件自己轮询"我依赖的东西到了吗"，那等于把时序判断抄 N 份。
 *
 * InitState 的做法是反过来的：每个组件声明**自己进入某状态的前置条件**
 * （`CanChangeInitState`），由 `UGameFrameworkComponentManager` 统一推进。
 * 谁先谁后不再由代码顺序决定，而是由条件是否满足决定。
 *
 * ## 四个阶段的含义
 * | 状态 | 含义 | 本组件的前置条件 |
 * |---|---|---|
 * | `Spawned`         | Pawn 实体存在 | 挂在一个合法 Pawn 上 |
 * | `DataAvailable`   | 配置数据齐了 | 有 PawnData，且（有权威或本地控制时）已被 Controller 附身 |
 * | `DataInitialized` | 数据已生效 | **所有** feature 都到达了 `DataAvailable` |
 * | `GameplayReady`   | 可以开始玩 | 无条件 |
 *
 * `DataInitialized` 那一条是整个机制的核心：它把"等别人就绪"从各组件的手工判断
 * 变成向 Manager 的一次查询（`HaveAllFeaturesReachedInitState`）。
 *
 * ## 与 Lyra 的三处差异
 * 1. **ASC 挂在 Pawn 自己身上**（决策 D1），不是 PlayerState。Lyra 一个 PlayerState
 *    一个 ASC，而本项目一名玩家带三个角色，共用 ASC 会让三角色共享 HP 与冷却。
 *    `InitializeAbilitySystem` 仍保留 `InOwnerActor` 参数，因为队伍级 ASC
 *    （挂 PlayerState、管队伍共享资源）将来仍走这条路。
 * 2. **额外注入 `AbilityGroupConfig`**，这是本项目的组并发规则表，Lyra 没有对应物。
 * 3. **AbilitySets 在这里授予**。Lyra 在 `ALyraPlayerState::SetPawnData` 里授予，
 *    因为 ASC 归 PlayerState；我们的 ASC 归 Pawn，授予方自然也跟着下来。
 */
#pragma once

#include "AbilitySystem/GGYGOAbilitySet.h"
#include "Components/GameFrameworkInitStateInterface.h"
#include "Components/PawnComponent.h"

#include "GGYGOPawnExtensionComponent.generated.h"

namespace EEndPlayReason { enum Type : int; }

class AActor;
class APawn;
class UGameFrameworkComponentManager;
class UGGYGOAbilitySystemComponent;
class UGGYGOPawnData;
class UObject;
struct FActorInitStateChangedParams;
struct FFrame;
struct FGameplayTag;

UCLASS(meta = (BlueprintSpawnableComponent))
class GGYGO_API UGGYGOPawnExtensionComponent : public UPawnComponent, public IGameFrameworkInitStateInterface
{
	GENERATED_BODY()

public:
	UGGYGOPawnExtensionComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/**
	 * 本 feature 在 InitState 系统里的名字。
	 *
	 * 每个参与协调的组件注册一个唯一 feature 名，Manager 靠它区分"谁到了哪一步"。
	 * 名字重复会让 `HaveAllFeaturesReachedInitState` 的结果不可预料。
	 */
	static const FName NAME_ActorFeatureName;

	//~IGameFrameworkInitStateInterface interface
	virtual FName GetFeatureName() const override { return NAME_ActorFeatureName; }
	virtual bool CanChangeInitState(UGameFrameworkComponentManager* Manager, FGameplayTag CurrentState, FGameplayTag DesiredState) const override;
	virtual void HandleChangeInitState(UGameFrameworkComponentManager* Manager, FGameplayTag CurrentState, FGameplayTag DesiredState) override;
	virtual void OnActorInitStateChanged(const FActorInitStateChangedParams& Params) override;
	virtual void CheckDefaultInitialization() override;
	//~End of IGameFrameworkInitStateInterface interface

	/** 取某个 Actor 上的本组件。没有则返回 nullptr。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Pawn")
	static UGGYGOPawnExtensionComponent* FindPawnExtensionComponent(const AActor* Actor)
	{
		return Actor ? Actor->FindComponentByClass<UGGYGOPawnExtensionComponent>() : nullptr;
	}

	/** 按目标类型取 PawnData。类型不匹配返回 nullptr。 */
	template <class T>
	const T* GetPawnData() const { return Cast<T>(PawnData); }

	/**
	 * 设置 PawnData。**仅服务器有效**，客户端靠复制拿到。
	 *
	 * 重复设置会被拒绝并记录错误：PawnData 决定授予了哪些能力，
	 * 中途换一份会留下一批已授予但配置里已不存在的能力。
	 * 队伍换角色应该换 Pawn，而不是给同一个 Pawn 换 PawnData。
	 */
	void SetPawnData(const UGGYGOPawnData* InPawnData);

	/** 当前 ASC。可能为 nullptr（初始化未完成或已反初始化）。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Pawn")
	UGGYGOAbilitySystemComponent* GetGGYGOAbilitySystemComponent() const { return AbilitySystemComponent; }

	/**
	 * 让本 Pawn 成为 ASC 的 Avatar。由拥有者 Pawn 调用。
	 *
	 * @param InASC        目标 ASC。本项目通常是 Pawn 自己持有的那个。
	 * @param InOwnerActor ASC 的逻辑拥有者。挂 Pawn 时传 Pawn 自己；
	 *                     队伍级 ASC 场景下传 PlayerState。
	 *
	 * 内部会处理"该 ASC 已有别的 Avatar"的情况 —— 客户端网络延迟时，
	 * 新 Pawn 可能在旧 Pawn 销毁前就被附身，此时要先把旧的踢下来。
	 */
	void InitializeAbilitySystem(UGGYGOAbilitySystemComponent* InASC, AActor* InOwnerActor);

	/** 解除本 Pawn 与 ASC 的关联，回收授予的能力。由拥有者 Pawn 或 EndPlay 调用。 */
	void UninitializeAbilitySystem();

	/** Controller 变更时由拥有者 Pawn 调用。 */
	void HandleControllerChanged();

	/** PlayerState 复制到达时由拥有者 Pawn 调用。 */
	void HandlePlayerStateReplicated();

	/** 输入组件建立后由拥有者 Pawn 调用。 */
	void SetupPlayerInputComponent();

	/**
	 * 订阅 ASC 就绪事件，**且如果已经就绪就立刻回调一次**。
	 *
	 * 这个"注册即可能立即触发"的语义是必需的：订阅方（如 HealthComponent）
	 * 的 BeginPlay 与本组件的 InitializeAbilitySystem 谁先执行是不确定的，
	 * 只用普通订阅会漏掉已经发生的那次广播。
	 */
	void OnAbilitySystemInitialized_RegisterAndCall(FSimpleMulticastDelegate::FDelegate Delegate);

	/** 订阅 ASC 反初始化事件。这个不需要补发，因为反初始化必然发生在订阅之后。 */
	void OnAbilitySystemUninitialized_Register(FSimpleMulticastDelegate::FDelegate Delegate);

protected:
	virtual void OnRegister() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** PawnData 复制到达。客户端由此推进 `DataAvailable`。 */
	UFUNCTION()
	void OnRep_PawnData();

	/** 授予 PawnData 里配置的所有 AbilitySet。仅服务器，且只执行一次。 */
	void GrantAbilitySets();

	/** ASC 就绪（本 Pawn 成为 Avatar）后广播。 */
	FSimpleMulticastDelegate OnAbilitySystemInitialized;

	/** ASC 解除关联后广播。 */
	FSimpleMulticastDelegate OnAbilitySystemUninitialized;

	/**
	 * 本单位的静态配置。
	 *
	 * `EditInstanceOnly`：放置在关卡里的实例可以直接配，但蓝图默认值不行 ——
	 * PawnData 与 Pawn 类是多对一关系，配在类默认值上就失去了这层间接的意义。
	 */
	UPROPERTY(EditInstanceOnly, ReplicatedUsing = OnRep_PawnData, Category = "GGYGO|Pawn")
	TObjectPtr<const UGGYGOPawnData> PawnData;

	/** ASC 缓存。`Transient` 因为它由 `InitializeAbilitySystem` 在运行时填。 */
	UPROPERTY(Transient)
	TObjectPtr<UGGYGOAbilitySystemComponent> AbilitySystemComponent;

	/**
	 * 授予记录，用于反初始化时回收。
	 *
	 * 不回收的后果不是内存泄漏（ASC 销毁时一起走），而是**换 Avatar 时能力重复授予**：
	 * 同一个 ASC 换到新 Pawn 后再授予一遍，两套句柄同时存在。
	 */
	UPROPERTY(Transient)
	FGGYGOAbilitySet_GrantedHandles GrantedHandles;

	/** AbilitySets 是否已授予。防止 `DataInitialized` 被重复进入时授予两遍。 */
	bool bAbilitySetsGranted = false;
};
