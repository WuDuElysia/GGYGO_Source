/**
 * @file GGYGOMovementSet.h
 * @brief 一个角色的移动参数
 *
 * 取代旧的 `FMovementConfig`（内联在 `UCharConfigData` 里的 USTRUCT）。
 * 改成独立 DataAsset 的理由与 `UGGYGOPawnData` 相同：移动手感需要能跨角色复用，
 * 也需要能给同一角色换一套（受伤状态、水下、载具），内联结构做不到。
 *
 * ## 迁移时丢弃的字段
 * 旧 `FMovementConfig` 有 11 个字段，其中 7 个在整个 `Source/GGYGO` 里
 * **没有任何读取方**：`SprintMultiplier`、`SprintSpeed`、`AirControlFactor`、
 * `DodgeSpeed`、`DodgeDuration`、`KnockbackDecay`、`TurnBackSecondSegmentTimeSeconds`、
 * `bDebugMotion`。它们不是"预留"，是配了没人用 —— 闪避速度实际由动画曲线决定，
 * 空中控制从未被写进 CMC。这次不迁移，需要时再按真实需求加。
 *
 * ## 新增的两个字段
 * `WalkSpeed` / `RunSpeed` 是**新增的**，旧实现里不存在。
 * 旧移动完全由 `RM_Speed` 动画曲线驱动速度（`MaxWalkSpeed` 每帧被覆写成曲线值），
 * 所以根本没有固定速度配置。曲线驱动会在阶段 6 重新接上，
 * 这两个值届时降为"曲线缺失时的兜底速度"，现在则是唯一速度来源。
 *
 * 保留兜底而不是照搬旧行为（无曲线就 `StopMovementImmediately`）是有意的：
 * 那个行为让"动画没配曲线"表现为角色完全不动，极难排查。有兜底速度时
 * 表现为"能动但没有防滑步"，问题明显但不阻塞。
 */
#pragma once

#include "Character/Data/GGYGOMovementTypes.h"
#include "Engine/DataAsset.h"

#include "GGYGOMovementSet.generated.h"

class UObject;

UCLASS(BlueprintType, Const, meta = (DisplayName = "GGYGO Movement Set", ShortTooltip = "一个角色的移动参数"))
class GGYGO_API UGGYGOMovementSet : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UGGYGOMovementSet(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	// ===== 速度 =====

	/** 行走速度（cm/s）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Speed", meta = (ClampMin = "0.0", UIMin = "0.0", ForceUnits = "cm/s"))
	float WalkSpeed = 200.0f;

	/** 跑步速度（cm/s）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Speed", meta = (ClampMin = "0.0", UIMin = "0.0", ForceUnits = "cm/s"))
	float RunSpeed = 450.0f;

	// ===== 步态切换 =====

	/**
	 * 持续行走多久自动升为跑步（秒）。
	 *
	 * 这是 ZZZ 的走跑切换方式：不看摇杆幅度，只看持续时间。
	 * 计时只在"有移动输入 + 在地面移动 + 当前是 Walk"时累加，
	 * 任何中断（松手、被禁止移动、离地）都归零重来。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Gait", meta = (ClampMin = "0.1", ClampMax = "60.0", UIMin = "0.1", UIMax = "60.0", ForceUnits = "s"))
	float WalkToRunHoldSeconds = 5.0f;

	// ===== 旋转与加减速 =====

	/**
	 * 是否让角色朝向自动对齐移动方向。
	 *
	 * 动作游戏通常要 true（角色面向跑动方向），锁定目标时由能力临时关掉。
	 * TurnBack 期间也会被临时关掉，因为那段的朝向由动画曲线驱动（阶段 6）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rotation")
	bool bOrientRotationToMovement = true;

	/** 朝向对齐的角速度（度/秒）。只有 Yaw 有意义，Pitch/Roll 由动画负责。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rotation", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float RotationYawRate = 720.0f;

	/** 最大加速度（cm/s²）。越大越"贴手"，越小越有惯性。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Acceleration", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float MaxAcceleration = 2048.0f;

	/** 地面制动减速度（cm/s²）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Acceleration", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float BrakingDecelerationWalking = 2048.0f;

	/** 地面摩擦力。影响转向的粘滞感，与制动减速度共同决定停止手感。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Acceleration", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float GroundFriction = 8.0f;

	// ===== 曲线驱动（阶段 6 消费） =====

	/**
	 * 动画曲线速度的缩放系数。
	 *
	 * 从旧 `FMovementConfig::RootMotionScale` 迁移而来。只缩放速度，不缩放位移量。
	 * 阶段 6 曲线驱动接入后生效，当前无读取方 —— 保留是因为它是既有配置，
	 * 删掉会让阶段 6 重建时丢失已调好的数值。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Curve Driven", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float RootMotionScale = 1.0f;

	// ===== TurnBack（阶段 6 消费） =====

	/**
	 * 反向输入判定阈值：移动输入与角色前向的点积小于等于此值才算"要转身"。
	 *
	 * 默认 -0.95 约等于 162 度以上的反向。
	 * 这个值原本是动画层的常量 `ZZZLocomotionRules::DefaultTurnBackReverseInputDotThreshold`，
	 * 却被逻辑层的 `FTurnBackPhaseProcessor` 直接引用 —— 那是逻辑层反向依赖表现层的唯一处。
	 * 提到配置资产上一并解决了依赖方向和可调性两件事。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "TurnBack", meta = (ClampMin = "-1.0", ClampMax = "1.0", UIMin = "-1.0", UIMax = "1.0"))
	float TurnBackReverseInputDotThreshold = -0.95f;

	/** 转身第一段（不可打断）持续多久后进入可释放阶段（秒）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "TurnBack", meta = (ClampMin = "0.0", UIMin = "0.0", ForceUnits = "s"))
	float TurnBackReleaseTimeSeconds = 0.17f;

	/** 转身动作总时长（秒）。超过即自然结束。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "TurnBack", meta = (ClampMin = "0.0", UIMin = "0.0", ForceUnits = "s"))
	float TurnBackDurationSeconds = 2.40f;

	/**
	 * 取指定步态对应的速度。
	 *
	 * `None` 返回 0，这是有意的：被禁止移动和静止都映射到 `None`，
	 * 让 `GetMaxSpeed()` 只需查这一张表，不必再写分支。
	 */
	float GetSpeedForGait(EGGYGOGait Gait) const;

	/** 取经过合法性校验的走跑切换阈值。非有限或越界时返回钳制后的安全值。 */
	float GetSanitizedWalkToRunHoldSeconds() const;
};
