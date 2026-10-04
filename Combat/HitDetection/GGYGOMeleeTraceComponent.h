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
#include "GameplayTagContainer.h"

#include "GGYGOMeleeTraceComponent.generated.h"

class AActor;
class UObject;
class USkeletalMeshComponent;
struct FHitResult;

/** 一次判定命中的结果。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FGGYGOMeleeHitSignature, AActor*, HitActor, const FHitResult&, HitResult);

UCLASS(meta = (BlueprintSpawnableComponent))
class GGYGO_API UGGYGOMeleeTraceComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UGGYGOMeleeTraceComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/**
	 * 开启判定窗口。
	 *
	 * @param InStartSocket 判定线段的起点骨骼（通常是武器根）。
	 * @param InEndSocket   终点骨骼（通常是武器尖）。
	 * @param InTraceRadius 扫掠球半径。
	 *
	 * 每次开启都会清空已命中记录，所以连招的每一段都能重新命中同一个敌人。
	 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Combat")
	void BeginTraceWindow(FName InStartSocket, FName InEndSocket, float InTraceRadius);

	/** 关闭判定窗口。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Combat")
	void EndTraceWindow();

	/** 判定窗口是否开启。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Combat")
	bool IsTracing() const { return bIsTracing; }

	/** 命中时广播。开窗调用者负责权限，组件只报告碰撞候选。 */
	UPROPERTY(BlueprintAssignable, Category = "GGYGO|Combat")
	FGGYGOMeleeHitSignature OnMeleeHit;

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

	/** 回调中关闭或重开窗口时改变，旧扫掠不能继续广播或写回采样基线。 */
	uint32 WindowSerial = 0;

	friend class FGGYGOMeleeTraceSafetyTest;
};
