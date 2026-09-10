/**
 * @file GGYGOMovementSet.h
 * @brief 一个角色的移动参数
 *
 * 做成独立 DataAsset 而不是内联在角色配置里，理由与 `UGGYGOPawnData` 相同：
 * 移动手感需要能跨角色复用，也需要能给同一角色换一套（受伤状态、水下、载具）。
 *
 * ## 速度来源
 * `WalkSpeed` / `RunSpeed` 是当前唯一的速度来源。
 * 动画曲线驱动（`RM_Speed` 决定每帧速度，用于消除脚滑）接入后，
 * 这两个值会降为曲线缺失时的兜底速度 —— 保留兜底而不是让角色停住，
 * 是因为"动画没配曲线"若表现为角色完全不动，极难定位；
 * 表现为"能动但有脚滑"则问题明显且不阻塞。
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
	 * 转身第一段期间也会被临时关掉，因为那段的朝向由动画曲线驱动。
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

	// ===== 曲线驱动 =====

	/**
	 * 是否让动画曲线接管速度。
	 *
	 * 开启后速度由 `RM_Speed` 曲线逐帧给出，脚步与位移严格对齐（不打滑）。
	 * 关闭则一直用上面的 `WalkSpeed` / `RunSpeed`。
	 *
	 * 动画没有烘焙曲线时会自动回退到固定速度，所以开启它对未处理的动画无害。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Curve Driven")
	bool bUseCurveDrivenSpeed = true;

	/** 动画曲线速度的缩放系数。只缩放速度，不缩放位移量。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Curve Driven", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float RootMotionScale = 1.0f;

	/**
	 * 曲线速度的上界（cm/s），用于非本地控制端的速度校验。
	 *
	 * 曲线值是本地动画状态，服务器若没有评估动画就采不到曲线。
	 * 那种情况下服务器用固定速度会低于客户端的曲线速度，
	 * 导致位置校正持续触发（角色被反复拉回）。本字段给服务器一个足够宽松的
	 * 上界，代价是这个上界内客户端的速度不受精确约束。
	 *
	 * 应当设为大于所有移动动画 `RM_Speed` 峰值的值。设得过小会拉回角色，
	 * 过大则放宽了作弊空间 —— 但位置本身仍受服务器校验，收益上限有限。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Curve Driven", meta = (ClampMin = "0.0", UIMin = "0.0", ForceUnits = "cm/s"))
	float MaxCurveDrivenSpeed = 1500.0f;

	// ===== TurnBack =====
	// 相位机尚未在 CMC 内实现，以下三项目前没有读取方。

	/**
	 * 反向输入判定阈值：移动输入与角色前向的点积小于等于此值才算"要转身"。
	 *
	 * 默认 -0.95 约等于 162 度以上的反向。
	 * 阈值放在移动层的配置资产上而不是动画层的常量里，是为了保持依赖方向 ——
	 * 转身的触发判定属于移动层，动画层只读相位结果。
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
