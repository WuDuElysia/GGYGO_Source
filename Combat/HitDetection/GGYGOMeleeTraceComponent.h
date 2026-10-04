/**
 * @file GGYGOMeleeTraceComponent.h
 * @brief 近战判定：在判定窗口内沿武器扫掠检测命中
 *
 * ## 为什么不用碰撞体重叠
 * 用武器上的碰撞体做 Overlap 有两个硬伤：快速挥砍时武器在两帧之间可能
 * 完全穿过敌人（隧穿），以及无法控制"哪一段动画才算有效判定"。
 *
 * 本组件在两个 Socket 之间按半径分段取点，每帧对各点做球形扫掠，
 * 用上一帧到本帧的位置做连续检测，降低跨帧漏判；而窗口的开关由
 * 能力（配合 AnimNotifyState）控制，判定时机与动画严格对齐。
 *
 * ## 每次攻击只命中一次
 * 一次挥砍会跨越多帧，同一个敌人会被反复扫到。组件内记录本次窗口已命中的
 * Actor，重复命中直接跳过 —— 否则一刀会造成多次伤害，而且伤害量随帧率变化。
 */
#pragma once

#include "Components/ActorComponent.h"
#include "Delegates/Delegate.h"
#include "GameplayTagContainer.h"

#include "GGYGOMeleeTraceComponent.generated.h"

class AActor;
class UObject;
class USkeletalMeshComponent;
class UGGYGOMeleeTraceComponent;
struct FHitResult;

/** 原生窗口资源身份。只有签发组件能构造；不持有组件，也不代表窗口仍活动。 */
struct GGYGO_API FGGYGOMeleeTraceWindowHandle
{
public:
	FGGYGOMeleeTraceWindowHandle() = default;

	/** 历史上曾取得窗口；当前有效性必须向签发组件查询。 */
	bool HasWindow() const { return Serial != 0; }
	bool operator==(const FGGYGOMeleeTraceWindowHandle& Other) const
	{
		return Serial == Other.Serial && Issuer.HasSameIndexAndSerialNumber(Other.Issuer);
	}
	bool operator!=(const FGGYGOMeleeTraceWindowHandle& Other) const { return !(*this == Other); }

private:
	TWeakObjectPtr<UGGYGOMeleeTraceComponent> Issuer;
	uint64 Serial = 0;

	friend class UGGYGOMeleeTraceComponent;
};

enum class EGGYGOMeleeTraceWindowOpenResult : uint8
{
	Opened,
	InvalidConfiguration,
	InvalidExpectedHandle,
	WindowConflict,
	Unavailable,
	SerialExhausted
};

enum class EGGYGOMeleeTraceWindowCloseResult : uint8
{
	Closed,
	AlreadyInactive,
	InvalidHandle,
	WrongComponent
};

enum class EGGYGOMeleeTraceWindowQueryResult : uint8
{
	Active,
	Inactive,
	InvalidHandle,
	WrongComponent
};

enum class EGGYGOMeleeTraceWindowSubscribeResult : uint8
{
	Subscribed,
	InvalidHandle,
	WrongComponent,
	WindowInactive,
	InvalidCallback
};

/** Owned 窗口只报告原窗口身份和碰撞候选；调用方在注册时捕获自己的原 Activation。 */
DECLARE_DELEGATE_ThreeParams(FGGYGOMeleeTraceWindowHitDelegate,
	const FGGYGOMeleeTraceWindowHandle&, AActor*, const FHitResult&);

/** Legacy 窗口的命中结果。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FGGYGOMeleeHitSignature, AActor*, HitActor, const FHitResult&, HitResult);

UCLASS(meta = (BlueprintSpawnableComponent))
class GGYGO_API UGGYGOMeleeTraceComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UGGYGOMeleeTraceComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/**
	 * 开启 Legacy 判定窗口；活动 Owned 窗口存在时拒绝，不得无身份接管。
	 *
	 * @param InStartSocket 判定线段的起点骨骼（通常是武器根）。
	 * @param InEndSocket   终点骨骼（通常是武器尖）。
	 * @param InTraceRadius 扫掠球半径。
	 *
	 * 每次开启都会清空已命中记录，所以连招的每一段都能重新命中同一个敌人。
	 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Combat")
	void BeginTraceWindow(FName InStartSocket, FName InEndSocket, float InTraceRadius);

	/** 关闭 Legacy 判定窗口；不得关闭活动 Owned 窗口。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Combat")
	void EndTraceWindow();

	/** 判定窗口是否开启。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Combat")
	bool IsTracing() const { return bIsTracing; }

	/** Legacy 命中时广播；Owned 窗口不广播此无身份事件。开窗调用者负责权限。 */
	UPROPERTY(BlueprintAssignable, Category = "GGYGO|Combat")
	FGGYGOMeleeHitSignature OnMeleeHit;

	/**
	 * GameThread 原生开窗，不同步发出命中。
	 * 空 ExpectedWindow 只取得空闲窗口；非空只替换本组件活动的同一 Owned 窗口。
	 * 身份冲突不改窗口。获准替换后配置失败会关闭原窗口，不继续旧业务。
	 * 先快照 ExpectedWindow，再重置 OutWindow；允许两参数引用同一变量。失败输出为空。
	 */
	EGGYGOMeleeTraceWindowOpenResult TryOpenOwnedTraceWindow(
		FName InStartSocket, FName InEndSocket, float InTraceRadius,
		const FGGYGOMeleeTraceWindowHandle& ExpectedWindow, FGGYGOMeleeTraceWindowHandle& OutWindow);

	/** GameThread：仅关闭确切原窗口。已结束/被替换返回 AlreadyInactive，不影响后继窗口。 */
	EGGYGOMeleeTraceWindowCloseResult CloseOwnedTraceWindow(const FGGYGOMeleeTraceWindowHandle& Window);

	/** GameThread：Inactive 包括已结束/被替换；不维护历史结束原因。 */
	EGGYGOMeleeTraceWindowQueryResult QueryOwnedTraceWindow(const FGGYGOMeleeTraceWindowHandle& Window) const;

	/**
	 * GameThread：订阅确切活动 Owned 窗口，失败令牌为空。窗口结束自动移除其订阅。
	 * 每次命中快照订阅；分发中新增的订阅从后续命中开始，已退订的快照成员不再调用。
	 */
	EGGYGOMeleeTraceWindowSubscribeResult SubscribeWindowHit(
		const FGGYGOMeleeTraceWindowHandle& Window, FGGYGOMeleeTraceWindowHitDelegate Callback,
		FDelegateHandle& OutSubscription);

	/** GameThread：只移除确切令牌；旧令牌不能移除后继窗口订阅。 */
	bool UnsubscribeWindowHit(FDelegateHandle Subscription);

	/**
	 * 判定使用的碰撞通道。
	 *
	 * 应当用一个专用通道而不是复用 `ECC_Pawn`：判定要能穿过队友、
	 * 忽略触发器体积，而那些行为无法在复用通道时表达。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Combat")
	TEnumAsByte<ECollisionChannel> TraceChannel = ECC_Pawn;

	/** 每帧武器线段最多分段数；所需密度超过此值时拒绝窗口并告警，避免静默产生空洞。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Combat", meta = (ClampMin = "1", ClampMax = "512"))
	int32 MaxTraceSegments = 64;

protected:
	virtual void OnUnregister() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Deactivate() override;

	/** 执行一帧扫掠。 */
	void PerformTrace();

	/** 仅使用当前角色主 Mesh；缺失 Socket 必须关闭窗口，不能退回组件原点。 */
	USkeletalMeshComponent* GetTraceMesh() const;

	/** 判定线段起点骨骼。 */
	FName StartSocket;

	/** 判定线段终点骨骼。 */
	FName EndSocket;

	/** 扫掠半径。 */
	float TraceRadius = 20.0f;

	/** 窗口是否开启。 */
	bool bIsTracing = false;

	/**
	 * 上一帧的线段端点。
	 *
	 * 连续检测的关键：本帧扫掠的是"上一帧位置到本帧位置"这段路径，
	 * 只用本帧位置会在武器移动快于胶囊直径时漏过敌人。
	 */
	FVector PreviousStart = FVector::ZeroVector;
	FVector PreviousEnd = FVector::ZeroVector;

	/** 上一帧端点是否有效。窗口开启的第一帧没有可比对的上一帧。 */
	bool bHasPreviousTransform = false;

	/**
	 * 本次窗口已命中的 Actor。
	 *
	 * 用弱引用：命中后目标可能被销毁（一击必杀），持强引用会阻止 GC。
	 */
	TArray<TWeakObjectPtr<AActor>> HitActorsThisWindow;

	/** 每次成功开窗递增，永不回绕/复用；与 bIsTracing 共同校验原窗口。 */
	uint64 WindowSerial = 0;

private:
	/** 同一窗口的来源，不另建窗口状态或执行链。 */
	enum class EWindowSource : uint8 { None, Legacy, Owned };
	EWindowSource WindowSource = EWindowSource::None;
	/** 仅生命周期清理栈内拒绝重入开窗，不是另一份窗口活动状态。 */
	bool bClosingForLifecycle = false;

	/** 原窗口持有的订阅资源；仅活动 Owned 窗口持有，结束统一释放。 */
	struct FWindowHitSubscription
	{
		FGGYGOMeleeTraceWindowHandle Window;
		FDelegateHandle Handle;
		FGGYGOMeleeTraceWindowHitDelegate Callback;
	};
	TArray<FWindowHitSubscription> WindowHitSubscriptions;

	EGGYGOMeleeTraceWindowOpenResult StartTraceWindow(
		FName InStartSocket, FName InEndSocket, float InTraceRadius, EWindowSource Source);
	/** 生命周期/内部失败的唯一清理入口；公共 Legacy End 不具备此权限。 */
	void CloseCurrentTraceWindow();
	bool IsActiveWindow(uint64 Serial) const;
	void DispatchOwnedWindowHit(const FGGYGOMeleeTraceWindowHandle& Window, AActor* HitActor, const FHitResult& Hit);

	friend class FGGYGOMeleeTraceSafetyTest;
};
