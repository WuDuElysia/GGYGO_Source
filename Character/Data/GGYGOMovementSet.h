/**
 * @file GGYGOMovementSet.h
 * @brief 一个角色的移动参数
 *
 * 做成独立 DataAsset 而不是内联在角色配置里，理由与 `UGGYGOPawnData` 相同：
 * 移动手感需要能跨角色复用，也需要能给同一角色换一套（受伤状态、水下、载具）。
 *
 * ValidateMovementSet 只校验本资产的移动参数。曲线模式由 Animation 发布的
 * 原始动画源绑定提供运动曲线；显式固定模式使用 WalkSpeed / RunSpeed。
 * 源绑定的资格与生命周期由 CMC 请求入口检查，不由复制的 Profile 授权。
 */
#pragma once

#include "Character/Data/GGYGOMovementTypes.h"
#include "Engine/DataAsset.h"

#include "GGYGOMovementSet.generated.h"

class UObject;
class UGGYGOLocomotionMotionProfile;

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
	float WalkToRunHoldSeconds = 1.5f;

	/** WalkRun BlendSpace 的权重推进速率（1/秒）。0 表示立即吸附；非法值由统一校验拒绝。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Gait", meta = (DisplayName = "Walk Run Blend Interp Speed", ClampMin = "0.0", ClampMax = "50.0", UIMin = "0.0", UIMax = "50.0"))
	float WalkRunBlendInterpSpeed = 6.0f;

	/** 起步后在该时间内松手时选择 StartStop。该计时只由 CMC 推进。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stop", meta = (ClampMin = "0.0", UIMin = "0.0", ForceUnits = "s"))
	float StartStopSelectionSeconds = 0.3f;

	// ===== 旋转与加减速 =====

	/**
	 * 是否让角色朝向自动对齐移动方向。
	 *
	 * 动作游戏通常要 true（角色面向跑动方向），锁定目标时由能力临时关掉。
	 *
	 * 转身的 `Turning` / `Braking` 两段不受这个开关影响：那两段 `PhysicsRotation`
	 * 直接绕过基类，朝向完全由曲线的转角增量驱动，不需要先关掉再恢复。
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
	 * 是否让原始动画源曲线接管速度。
	 *
	 * 开启后速度由源动画的 `RootMotion_Speed` 曲线给出。
	 * 关闭则一直用上面的 `WalkSpeed` / `RunSpeed`。
	 *
	 * 曲线模式必须有有效的 Animation 源绑定；缺失或非法源不会回落为固定速度。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Curve Driven")
	bool bUseCurveDrivenSpeed = true;

	/** 源动画曲线速度的缩放系数，不缩放 Yaw。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Curve Driven", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float RootMotionScale = 1.0f;

	/** 旧服务器放宽上界，仅保留资产序列化兼容；Profile 权威后不再读取。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Deprecated", meta = (DeprecatedProperty, DeprecationMessage = "Locomotion profiles are evaluated on the server; this bound is no longer used."))
	float MaxCurveDrivenSpeed = 20000.0f;

	// ===== 历史 Locomotion Motion Profiles =====
	// 仅保留旧资产序列化与迁移记录，不参与源绑定资格或运行时求值。
	// CMC 消费 Animation 发布的不可变源绑定，不反查 AnimInstance 当前姿态。

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Deprecated|Locomotion Profiles", meta = (DeprecatedProperty, DeprecationMessage = "Motion curves come from the original Animation source binding."))
	TObjectPtr<const UGGYGOLocomotionMotionProfile> WalkStartProfile;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Deprecated|Locomotion Profiles", meta = (DeprecatedProperty, DeprecationMessage = "Motion curves come from the original Animation source binding."))
	TObjectPtr<const UGGYGOLocomotionMotionProfile> WalkLoopProfile;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Deprecated|Locomotion Profiles", meta = (DeprecatedProperty, DeprecationMessage = "Motion curves come from the original Animation source binding."))
	TObjectPtr<const UGGYGOLocomotionMotionProfile> RunLoopProfile;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Deprecated|Locomotion Profiles", meta = (DeprecatedProperty, DeprecationMessage = "Motion curves come from the original Animation source binding."))
	TObjectPtr<const UGGYGOLocomotionMotionProfile> StartStopProfile;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Deprecated|Locomotion Profiles", meta = (DeprecatedProperty, DeprecationMessage = "Motion curves come from the original Animation source binding."))
	TObjectPtr<const UGGYGOLocomotionMotionProfile> WalkStopProfile;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Deprecated|Locomotion Profiles", meta = (DeprecatedProperty, DeprecationMessage = "Motion curves come from the original Animation source binding."))
	TObjectPtr<const UGGYGOLocomotionMotionProfile> RunStopProfile;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Deprecated|Locomotion Profiles", meta = (DeprecatedProperty, DeprecationMessage = "Motion curves come from the original Animation source binding."))
	TObjectPtr<const UGGYGOLocomotionMotionProfile> TurnBackProfile;

	// ===== TurnBack（急停转身）=====
	// 由 UGGYGOCharacterMovementComponent 的相位机读取。
	// 相位边界从动画曲线判定，下面三个阈值是判定的容差，不是时间点。

	/**
	 * 反向输入判定阈值：移动输入与角色前向的点积小于等于此值才算"要转身"。
	 *
	 * 默认 -0.95 约等于 162 度以上的反向。
	 * 阈值放在移动层的配置资产上而不是动画层的常量里，是为了保持依赖方向 ——
	 * 转身的触发判定属于移动层，动画层只读相位结果。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "TurnBack", meta = (ClampMin = "-1.0", ClampMax = "1.0", UIMin = "-1.0", UIMax = "1.0"))
	float TurnBackReverseInputDotThreshold = -0.95f;

	/**
	 * `Turning` → `Braking` 的门槛：累计转角至少要达到这个绝对值（度）。
	 *
	 * 单独用"本帧转角为 0"判断转身结束是不够的：动画段刚切换、采样基线尚未建立的
	 * 那一帧差分恒为 0，会被误判成"已经转完"，于是转身第一帧就直接进 Braking。
	 * 这个门槛把那一帧挡掉。设为明显小于动画实际转角（Pyrios 是 180 度）即可。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "TurnBack", meta = (ClampMin = "0.0", ClampMax = "180.0", UIMin = "0.0", UIMax = "180.0"))
	float TurnBackMinYawDegrees = 90.0f;

	/**
	 * `Turning` → `Braking` 的判据：本帧转角增量绝对值不超过此值（度）就算转角已到位。
	 *
	 * 不是"每秒多少度"而是"每帧多少度"，因为它比对的是曲线差分。
	 * 用固定值可行的原因是转身完成后曲线转角是**精确的常量**，差分恰好为 0，
	 * 不存在缓慢收敛的尾巴；这个值只用来吸收浮点误差。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "TurnBack", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float TurnBackYawSettleDegrees = 0.05f;

	/**
	 * `Braking` → `RunOut` 的判据：曲线方向的前向分量低于此值的负数时，认为位移方向已翻转。
	 *
	 * 曲线方向在动画段起点坐标系里，前向分量由正转负就意味着"不再沿进入方向滑行，
	 * 开始朝反方向跑出"。取 0.5 而不是 0，是为了避开方向正在过渡时的中间值。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "TurnBack", meta = (ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float TurnBackRunOutForwardThreshold = 0.5f;

	/**
	 * `Braking` → `RunOut` 的速度门限（cm/s）：低于此速度不采信曲线方向。
	 *
	 * 刹车段末速会掉到很低（Pyrios 是 52 cm/s），那时单帧位移不足 1cm，
	 * 归一化出的方向容易抖动。加这个门限避免在速度谷底被一帧噪声提前推进相位。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "TurnBack", meta = (ClampMin = "0.0", UIMin = "0.0", ForceUnits = "cm/s"))
	float TurnBackRunOutMinSpeed = 150.0f;

	/**
	 * 转身总时长上限（秒）。超过即强制结束。
	 * 0 是合法的显式立即截止配置，统一校验不会替换为默认上限。
	 *
	 * 这是兜底而非正常出口：正常情况下 `RunOut` 段由松手结束。
	 * 需要它是因为相位推进依赖曲线，而动画可能被别的状态打断、
	 * 或者压根没配曲线，那时相位会卡住，进而永久阻止下一次转身触发。
	 * 应当设为大于最长转身动画的时长。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "TurnBack", meta = (ClampMin = "0.0", UIMin = "0.0", ForceUnits = "s"))
	float TurnBackDurationSeconds = 3.0f;

	/**
	 * 取指定步态对应的速度。
	 *
	 * `None` 返回 0，这是有意的：被禁止移动和静止都映射到 `None`，
	 * 让 `GetMaxSpeed()` 只需查这一张表，不必再写分支。
	 * 返回已编写的原值；调用者须先通过配置校验。
	 */
	float GetSpeedForGait(EGGYGOGait Gait) const;

	/** 旧名称兼容入口，返回已编写的原值；调用者须先通过配置校验。 */
	float GetSanitizedWalkToRunHoldSeconds() const;

	/** 仅供历史 Profile 迁移工具查询，运行时不据此选择或替代动画源。 */
	const UGGYGOLocomotionMotionProfile* GetProfileForMotion(EGGYGOLocomotionMotionType MotionType) const;

	/**
	 * 纯校验本资产的非废弃数值，不修改配置或持有运行状态。
	 * 原始动画源、曲线和生命周期资格由源绑定消费入口独立校验。
	 * 合法零值保持原语义；成功清空旧错误，失败含资产路径、字段与原因。
	 * 编辑器与 CMC 配置接纳使用同一数值规则。
	 */
	bool ValidateMovementSet(FString& OutError) const;

#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
#endif
};
