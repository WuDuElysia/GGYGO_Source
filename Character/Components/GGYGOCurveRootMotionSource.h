/**
 * @file GGYGOCurveRootMotionSource.h
 * @brief 由烘焙曲线驱动位移的 root motion source
 *
 * ZZZ 的移动动画是 in-place 的，真实位移被烘焙成 `RootMotion_*` 曲线。
 * 这个 source 把曲线的速度与方向提交给 CMC 的 root motion 仲裁，让动画自己决定位移。
 *
 * ## 为什么走 root motion source 而不是改 Acceleration
 * `Acceleration` 是 CMC 唯一的水平输入通道，也是玩家输入的通道。用它来喂曲线速度
 * 会有两个后果：一是必须伪造一个"假输入"才能让 `CalcVelocity` 动起来；二是玩家松手
 * 时 `Acceleration` 归零，`CalcVelocity` 会按 `BrakingDecelerationWalking` 主动制动，
 * 曲线和刹车互相打，刹停动画的滑行距离全部丢失。
 *
 * Override 模式的 root motion source 让 `PhysWalking` 整段跳过 `CalcVelocity`
 * （引擎自己的判据是 `CurrentRootMotion.HasOverrideVelocity()`），于是制动不发生，
 * 玩家输入彻底失去对位移的影响 —— 这正是"动画接管位移"要的语义。
 * 位移仍然走 `MoveAlongFloor`，碰撞、斜坡、台阶一个不丢。
 *
 * ## 谁来提交
 * 由 `UGGYGOCharacterMovementComponent` 按 `InstanceName` 挂载与摘除。
 * 每个语义段一个名字（刹停、转身），同一时刻只应有一个在生效。
 */

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/RootMotionSource.h"
#include "GGYGOCurveRootMotionSource.generated.h"

/**
 * 曲线驱动的位移源。
 *
 * 速度与方向每帧从 `UGGYGOCharacterMovementComponent::GetCurveMotion()` 现取，
 * 不在本结构里缓存。这样曲线只有一个真值来源；移动回放时
 * `FSavedMove_GGYGO::PrepMoveFor` 已经把 `CurveMotion` 还原成当时的值，
 * 所以现取反而比自己存一份更不容易与回放失配。
 */
USTRUCT()
struct FRootMotionSource_GGYGOCurve : public FRootMotionSource
{
	GENERATED_USTRUCT_BODY()

	GGYGO_API FRootMotionSource_GGYGOCurve();

	virtual ~FRootMotionSource_GGYGOCurve() = default;

	/**
	 * 位移方向的基准朝向（度，世界空间 yaw）。
	 *
	 * 曲线的方向分量以**动画段起点**为基准，所以这里必须是进入该段时的 Actor 朝向，
	 * 不能是当前朝向。用当前朝向会把角色已经转过的角度重复计入一次，
	 * 直线滑行会被拖成弧线 —— 急停转身要的恰恰是"一边转身一边沿原方向滑",
	 * 世界方向必须保持不变。
	 *
	 * 这也是不能用基类 `bInLocalSpace` 的原因：那个开关用的是**当前**组件旋转。
	 */
	UPROPERTY()
	float BaseYaw = 0.0f;

	/** 曲线速度的缩放系数。取自 `UGGYGOMovementSet::RootMotionScale`。 */
	UPROPERTY()
	float SpeedScale = 1.0f;

	/**
	 * 曲线速度归零时自我终结。
	 *
	 * 刹停用 true：曲线衰减到 0 就是滑行结束，让 source 自己退场，
	 * `CalcVelocity` 下一帧恢复接管，玩家输入自然生效 —— 不需要外部状态机盯着。
	 *
	 * 转身用 false：转身段中间可能有若干帧曲线速度为 0（动画那几帧确实原地不动），
	 * 自我终结会让 `CalcVelocity` 在那几帧恢复，而玩家此时正按着反方向键，
	 * 角色会被推着走。false 时那几帧输出零速度把角色压住，
	 * 生命周期交给相位机管。
	 */
	UPROPERTY()
	bool bEndOnZeroSpeed = true;

	GGYGO_API virtual FRootMotionSource* Clone() const override;

	GGYGO_API virtual bool Matches(const FRootMotionSource* Other) const override;

	GGYGO_API virtual bool MatchesAndHasSameState(const FRootMotionSource* Other) const override;

	GGYGO_API virtual bool UpdateStateFrom(const FRootMotionSource* SourceToTakeStateFrom, bool bMarkForSimulatedCatchup = false) override;

	GGYGO_API virtual void PrepareRootMotion(
		float SimulationTime,
		float MovementTickTime,
		const ACharacter& Character,
		const UCharacterMovementComponent& MoveComponent
		) override;

	GGYGO_API virtual bool NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess) override;

	GGYGO_API virtual UScriptStruct* GetScriptStruct() const override;

	GGYGO_API virtual FString ToSimpleString() const override;
};

template<>
struct TStructOpsTypeTraits<FRootMotionSource_GGYGOCurve> : public TStructOpsTypeTraitsBase2<FRootMotionSource_GGYGOCurve>
{
	enum
	{
		WithNetSerializer = true,
		WithCopy = true
	};
};
