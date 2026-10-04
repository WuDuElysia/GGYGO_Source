/**
 * @file GGYGOPawnExtensionComponent.h
 * @brief Pawn 初始化协调者
 *
 * 本组件**不实现任何玩法**。它只做两件事：
 *   1. 持有 PawnData 与本地 ASC 资源，提供经过真实 Ready 校验的读取入口
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
 * ## 与 Lyra 的差异
 * 1. **ASC 可以来自外部持久战斗状态宿主**。玩家是 `AGGYGOCharacterSlot`，可换形态
 *    Boss 是 `AGGYGOBossState`；本组件只接收 `ExternalASC + OwnerActor`，不依赖宿主类型，
 *    自己不创建也不拥有 ASC。
 * 2. **本组件不授予 AbilitySet**。Lyra 在 `ALyraPlayerState::SetPawnData` 里授予，
 *    因为 ASC 归 PlayerState；本项目归位置，所以授予方也在位置上。
 *    本组件只分发 PawnData 里属于 Pawn 的部分：移动参数与 Cue 预热。
 */
#pragma once

#include "AbilitySystem/GGYGOAbilitySet.h"
// K4-Character-L1 includes begin.
#include "AbilitySystem/GGYGOAvatarBindingTypes.h"
#include "Templates/SharedPointer.h"
// K4-Character-L1 includes end.
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

// K4-Character-L1 native resource types begin.
class UGGYGOPawnExtensionComponent;
class FGGYGOAvatarBindingPublicationReceipt;

/** Retained weak identities and the ASC-issued Binding; never an execution permission. */
struct GGYGO_API FGGYGOPawnASCResourceIdentity
{
	TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC{};
	TWeakObjectPtr<APawn> Pawn{};
	FGGYGOAvatarBindingIdentity Binding{};

	bool HasSameIdentity(const FGGYGOPawnASCResourceIdentity& Other) const;
};

/** Copyable original local resource. Only Extension can create or mutate its private record. */
class GGYGO_API FGGYGOPawnASCResourceHandle final
{
public:
	FGGYGOPawnASCResourceHandle() = default;
	/** History exists, not liveness, installation or Ready. */
	bool HasResource() const;
	bool HasSameResource(const FGGYGOPawnASCResourceHandle& Other) const;
	/** Writable copy; changing it cannot change the retained original resource. */
	FGGYGOPawnASCResourceIdentity GetIdentity() const;

private:
	struct FLocalResource;
	TSharedPtr<FLocalResource> Resource{};
	friend class UGGYGOPawnExtensionComponent;
};

enum class EGGYGOPawnASCLocalOutcome : uint8
{
	Rejected = 0,
	Succeeded,
	Stale,
	Failed
};

enum class EGGYGOPawnASCLocalReason : uint8
{
	None = 0,
	InvalidArguments,
	InvalidASC,
	InvalidPawn,
	WrongExtension,
	LifecycleClosed,
	InvalidBinding,
	ContextMismatch,
	ResourceConflict,
	ResourceNotInstalled,
	ResourceNotWithdrawn,
	InvalidPublication,
	PublicationNotDispatching,
	ReadyNotEstablished,
	CallbackInvalidated
};

/** Caller-stack historical local result. No ActorInfo commit or global readiness claim. */
struct GGYGO_API FGGYGOPawnASCLocalResult
{
	EGGYGOPawnASCLocalOutcome Outcome = EGGYGOPawnASCLocalOutcome::Rejected;
	EGGYGOPawnASCLocalReason Reason = EGGYGOPawnASCLocalReason::InvalidArguments;
	FGGYGOPawnASCResourceHandle Resource{};
	bool bLocalChanged = false;
};

enum class EGGYGOPawnASCLocalNoticeKind : uint8
{
	Invalid = 0,
	Ready,
	Released,
	Refreshed
};

/** Released carries an empty PublishedContext and proves only local withdrawal. */
struct GGYGO_API FGGYGOPawnASCLocalNotice
{
	EGGYGOPawnASCLocalNoticeKind Kind = EGGYGOPawnASCLocalNoticeKind::Invalid;
	FGGYGOPawnASCResourceHandle Resource{};
	FGGYGOAvatarBindingContext PublishedContext{};
};

DECLARE_MULTICAST_DELEGATE_OneParam(FGGYGOPawnASCLocalNoticeDelegate, const FGGYGOPawnASCLocalNotice&);
// K4-Character-L1 native resource types end.

UCLASS(meta = (BlueprintSpawnableComponent))
class GGYGO_API UGGYGOPawnExtensionComponent : public UPawnComponent, public IGameFrameworkInitStateInterface
{
	GENERATED_BODY()

public:
	UGGYGOPawnExtensionComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

// K4-Character-L1 native APIs begin.
	/** Game thread only. New local path is not connected to the legacy production cache. */
	FGGYGOPawnASCLocalResult InstallLocalAbilitySystemResources(
		UGGYGOAbilitySystemComponent* ExpectedASC, APawn* ExpectedPawn,
		const FGGYGOAvatarBindingContext& CommittedContext);
	/** Local-only exact cleanup, including an expired/transferred ASC. No callbacks. */
	FGGYGOPawnASCLocalResult WithdrawLocalAbilitySystemResources(
		const FGGYGOPawnASCResourceHandle& ExpectedResource);
	/** Consume before callbacks. Never clear a successor or claim an ASC Clear commit. */
	FGGYGOPawnASCLocalResult NotifyLocalResourcesReleased(
		const FGGYGOPawnASCResourceHandle& ReleasedResource);
	/** Authenticate real ASC Dispatching before history. Refresh requires prior real Ready. */
	FGGYGOPawnASCLocalResult NotifyLocalResourcesReady(
		const FGGYGOPawnASCResourceHandle& ExpectedResource,
		const FGGYGOAvatarBindingPublicationReceipt& Publication);
	/** Game-thread current local slot copy, including stale ASC; no Installed/Ready permission. */
	FGGYGOPawnASCResourceHandle GetCurrentLocalAbilitySystemResource() const;
	/** Pure assembly/publication precondition; never requires Ready or grants ASC execution. */
	bool IsLocalAbilitySystemResourceInstalled(const FGGYGOPawnASCResourceHandle& ExpectedResource) const;
	/** Pure local/ASC publication gate; installation and historical receipts are insufficient. */
	bool IsLocalAbilitySystemResourceReady(const FGGYGOPawnASCResourceHandle& ExpectedResource) const;
	/** Identity-aware subscriptions. Replay only a currently authenticated local Ready resource. */
	FDelegateHandle RegisterLocalAbilitySystemNoticeAndCall(FGGYGOPawnASCLocalNoticeDelegate::FDelegate Delegate);
	void UnregisterLocalAbilitySystemNotice(FDelegateHandle Handle);
// K4-Character-L1 native APIs end.

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

	/** 仅返回原本地资源通过真实 Ready 校验的 ASC；安装或历史提交不代表就绪。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Pawn")
	UGGYGOAbilitySystemComponent* GetGGYGOAbilitySystemComponent() const;

	/**
	 * 同步请求明确的原 Host 将本 Pawn 绑定到 InASC；自身不执行 ActorInfo 写入。
	 * InOwnerActor 必须实现 native Host 接口并拥有 InASC。
	 * 跨 Host 由外层显式释放旧资源、核对后再调用；失败不自动回滚或替换其它装配。
	 */
	void InitializeAbilitySystem(UGGYGOAbilitySystemComponent* InASC, AActor* InOwnerActor);

	/**
	 * 一次捕获原本地 H/Context，向该 H 的 ASC 组件 Owner 请求释放。
	 * ExpectedASC 不匹配时不执行；端口失效时仅清理原本地资源并明确诊断失败。
	 * EndPlay 关闭新准入后仍可处理原释放义务，不读取后继继续清理。
	 */
	void UninitializeAbilitySystem(UGGYGOAbilitySystemComponent* ExpectedASC = nullptr);

	/** Controller 变更时仅请求刷新原 Ready 资源，再推进原作用域的配置初始化。 */
	void HandleControllerChanged();

	/** PlayerState 复制到达时由拥有者 Pawn 调用。 */
	void HandlePlayerStateReplicated();

	/** 输入组件建立后由拥有者 Pawn 调用。 */
	void SetupPlayerInputComponent();

	/**
	 * 订阅 ASC 就绪事件，**且如果已经就绪就立刻回调一次**。
	 *
	 * 这个"注册即可能立即触发"的语义是必需的：订阅方（如 HealthComponent）
	 * 的 BeginPlay 与真实 Ready 谁先发生是不确定的；
	 * 回放只认捕获的原 Ready H/Context，回调后不收养后继。
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

	/**
	 * 把 PawnData 里的配置分发给各消费者（ASC 的两张表、CMC 的移动参数）。
	 *
	 * 幂等，可以重复调用 —— 各 Setter 都是单纯赋值。
	 * 主调用点在 `HandleChangeInitState(DataInitialized)`，那里能保证 PawnData 非空。
	 */
	void ApplyPawnDataToConsumers();

	// 能力授予不在本组件：AbilitySet 由队伍位置 `AGGYGOCharacterSlot` 在装配时授予，
	// 因为 ASC 与属性集都归它持有。本组件只负责把 PawnData 里**属于 Pawn 的**部分
	// （移动参数、Cue 预热）分发下去。

	/** 原资源通过真实 ASC 发布认证成为 Ready 后广播；由本地通知接口唯一发出。 */
	FSimpleMulticastDelegate OnAbilitySystemInitialized;

	/** 本地 ASC 绑定解除后广播。 */
	FSimpleMulticastDelegate OnAbilitySystemUninitialized;

	/**
	 * 本单位的静态配置。
	 *
	 * `EditInstanceOnly`：放置在关卡里的实例可以直接配，但蓝图默认值不行 ——
	 * PawnData 与 Pawn 类是多对一关系，配在类默认值上就失去了这层间接的意义。
	 */
	UPROPERTY(EditInstanceOnly, ReplicatedUsing = OnRep_PawnData, Category = "GGYGO|Pawn")
	TObjectPtr<const UGGYGOPawnData> PawnData;

// K4-Character-L1 private local resources begin.
private:
	bool OwnsLocalAbilitySystemResource(const FGGYGOPawnASCResourceHandle& ExpectedResource) const;
	/** 本组件生命周期准入：EndPlay 关闭，真实下一次 BeginPlay 重开；不发行 Binding/Ready。 */
	bool bLocalAbilitySystemAdmissionClosed = false;
	FGGYGOPawnASCResourceHandle LocalAbilitySystemResource{};
	FGGYGOPawnASCLocalNoticeDelegate LocalAbilitySystemNotice;
// K4-Character-L1 private local resources end.

};
