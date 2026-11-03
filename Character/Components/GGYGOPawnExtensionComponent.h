/**
 * @file GGYGOPawnExtensionComponent.h
 * @brief Pawn 初始化协调者
 *
 * 本组件**不实现任何玩法**。它只做两件事：
 *   1. 持有 PawnData 与本 Pawn 当前的 ASC 绑定资源，向本地消费方广播就绪／释放
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
 * 1. **ASC 来自外部持久战斗状态宿主**。玩家是 `AGGYGOCharacterSlot`，可换形态
 *    Boss 是 `AGGYGOBossState`。宿主执行 InitAbilityActorInfo 后调用
 *    `HandleAbilitySystemBound`，释放时调用 `HandleAbilitySystemReleased`；
 *    本组件不创建也不拥有 ASC，也不自己写 ActorInfo（Controller 变化时的 Refresh 除外）。
 * 2. **本组件不授予 AbilitySet**。Lyra 在 `ALyraPlayerState::SetPawnData` 里授予，
 *    因为 ASC 归 PlayerState；本项目归位置，所以授予方也在位置上。
 *    本组件只分发 PawnData 里属于 Pawn 的部分：移动参数与 Cue 预热。
 */
#pragma once

#include "AbilitySystem/GGYGOAbilitySet.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
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

class UGGYGOPawnExtensionComponent;

/** 资源记录的弱引用副本；只描述端点，不授予执行权。 */
struct GGYGO_API FGGYGOPawnASCResourceIdentity
{
	TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC{};
	TWeakObjectPtr<APawn> Pawn{};
};

/**
 * 一次 ASC 绑定在本 Pawn 上的不透明身份。
 * 每次 HandleAbilitySystemBound 新建一份共享记录；同一 ASC 解绑后重绑也得到不同资源，
 * 消费方据此区分"同一次绑定"。可复制，只有 PawnExtension 能创建。
 */
class GGYGO_API FGGYGOPawnASCResourceHandle final
{
public:
	FGGYGOPawnASCResourceHandle() = default;
	bool HasResource() const { return Resource.IsValid(); }
	bool HasSameResource(const FGGYGOPawnASCResourceHandle& Other) const
	{
		return Resource.IsValid() && Resource == Other.Resource;
	}
	FGGYGOPawnASCResourceIdentity GetIdentity() const
	{
		return Resource.IsValid() ? *Resource : FGGYGOPawnASCResourceIdentity{};
	}

private:
	TSharedPtr<const FGGYGOPawnASCResourceIdentity> Resource{};
	friend class UGGYGOPawnExtensionComponent;
};

enum class EGGYGOPawnASCNoticeKind : uint8
{
	/** 宿主已对本 Pawn 执行 InitAbilityActorInfo。 */
	Ready,
	/** 宿主已取消能力、清输入并移除 Cue，ActorInfo 即将清除或改绑。 */
	Released
};

struct GGYGO_API FGGYGOPawnASCNotice
{
	EGGYGOPawnASCNoticeKind Kind = EGGYGOPawnASCNoticeKind::Ready;
	FGGYGOPawnASCResourceHandle Resource{};
};

/**
 * 本 Pawn 的 ASC 就绪／释放订阅，消费方作为成员持有，析构时退订。仅游戏线程。
 *
 * - Subscribe 时若已就绪，立即回放一次 Ready；
 * - 与已消费资源相同的 Ready 不再回调；Released 只在资源与已消费的相同时回调，
 *   回调前已清空已消费资源；
 * - 回调中退订或重新订阅是安全的：分发前复制订阅者列表，跳过已退订者；新订阅自行回放。
 */
class GGYGO_API FGGYGOLocalASCSubscription final
{
public:
	using FCallback = TFunction<void(const FGGYGOPawnASCNotice&)>;

	FGGYGOLocalASCSubscription() = default;
	~FGGYGOLocalASCSubscription() { Unsubscribe(); }
	FGGYGOLocalASCSubscription(const FGGYGOLocalASCSubscription&) = delete;
	FGGYGOLocalASCSubscription& operator=(const FGGYGOLocalASCSubscription&) = delete;

	/** 替换现有订阅。Extension 为空或 Callback 未绑定时返回 false 且不保留订阅。 */
	bool Subscribe(UGGYGOPawnExtensionComponent* Extension, FCallback Callback);
	/** 幂等；不回调 Released，消费方自行清理已消费资源对应的状态。 */
	void Unsubscribe();
	bool IsSubscribed() const;
	UGGYGOPawnExtensionComponent* GetExtension() const;
	/** 已消费资源仍是 Extension 当前就绪资源时返回其 ASC，否则 nullptr。 */
	UGGYGOAbilitySystemComponent* GetReadyASC() const;
	/** 最近一次 Ready 消费的资源；Released 或退订后为空。 */
	FGGYGOPawnASCResourceHandle GetConsumedResource() const;

private:
	struct FState;
	TSharedPtr<FState> State;
	friend class UGGYGOPawnExtensionComponent;
};

UCLASS(meta = (BlueprintSpawnableComponent))
class GGYGO_API UGGYGOPawnExtensionComponent : public UPawnComponent, public IGameFrameworkInitStateInterface
{
	GENERATED_BODY()

public:
	UGGYGOPawnExtensionComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/**
	 * 宿主对本 Pawn 执行 InitAbilityActorInfo 后调用：新建资源并广播 Ready。
	 * 已有同一 ASC 的资源时不重复广播；已有其它 ASC 的资源时拒绝并诊断（跨宿主须先释放）。
	 */
	bool HandleAbilitySystemBound(UGGYGOAbilitySystemComponent* ASC);
	/** 宿主释放流程中调用：先清空当前资源再广播 Released。没有资源时无操作。 */
	void HandleAbilitySystemReleased();
	/** 当前绑定资源；未绑定时为空。 */
	FGGYGOPawnASCResourceHandle GetCurrentLocalAbilitySystemResource() const;
	/** ExpectedResource 仍是当前资源，且其 ASC 的 Avatar 仍是本 Pawn。 */
	bool IsLocalAbilitySystemResourceReady(const FGGYGOPawnASCResourceHandle& ExpectedResource) const;

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

	/** Game thread only: an original synchronous initialization call is on the stack, not configuration acceptance or Ready. */
	bool IsPawnDataInitializationInProgress() const;

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

	/** 当前就绪资源的 ASC；未绑定或 Avatar 已不是本 Pawn 时为 nullptr。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Pawn")
	UGGYGOAbilitySystemComponent* GetGGYGOAbilitySystemComponent() const;

	/**
	 * 请求当前宿主立即释放本 Pawn 的绑定（死亡后销毁前，避免 GE 落到正在销毁的 Avatar）。
	 * 宿主即 ASC 的组件 Owner，须实现 IGGYGOAvatarBindingHostInterface；不支持时诊断。
	 */
	void UninitializeAbilitySystem();

	/** Controller 变更：刷新当前 ASC 的 ActorInfo，再推进 InitState。不经过宿主。 */
	void HandleControllerChanged();

	/** PlayerState 复制到达时由拥有者 Pawn 调用。 */
	void HandlePlayerStateReplicated();

	/** 输入组件建立后由拥有者 Pawn 调用。 */
	void SetupPlayerInputComponent();

protected:
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
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

	/**
	 * 本单位的静态配置。
	 *
	 * `EditInstanceOnly`：放置在关卡里的实例可以直接配，但蓝图默认值不行 ——
	 * PawnData 与 Pawn 类是多对一关系，配在类默认值上就失去了这层间接的意义。
	 */
	UPROPERTY(EditInstanceOnly, ReplicatedUsing = OnRep_PawnData, Category = "GGYGO|Pawn")
	TObjectPtr<const UGGYGOPawnData> PawnData;

private:
	struct FPawnDataInitializationScope;
	/** Borrowed only while the original stack scope exists; unregister/EndPlay invalidate it immediately. */
	const FPawnDataInitializationScope* PawnDataInitializationScope = nullptr;
	/** EndPlay closes scope publication; only a real subsequent BeginPlay reopens it. */
	bool bPawnDataInitializationClosed = false;

	void BroadcastLocalASCNotice(const FGGYGOPawnASCNotice& Notice);
	/** 当前绑定资源；只由宿主经 HandleAbilitySystemBound／Released 改写。 */
	FGGYGOPawnASCResourceHandle LocalAbilitySystemResource{};
	TArray<TWeakPtr<FGGYGOLocalASCSubscription::FState>> LocalASCSubscribers;
	friend class FGGYGOLocalASCSubscription;

};
