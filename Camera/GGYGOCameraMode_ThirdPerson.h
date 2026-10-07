/**
 * @file GGYGOCameraMode_ThirdPerson.h
 * @brief 第三人称跟随相机
 *
 * 从角色枢轴沿视线反方向计算期望位置，并输出穿墙查询请求。
 * 最终碰撞与恢复由 CameraComponent 在模式混合和 Offset 之后统一处理。
 *
 * 不用 `USpringArmComponent` 而自己算，是因为弹簧臂是场景组件、
 * 它的长度是组件状态：多个相机模式要同时存在并混合时，
 * 它们没法各自持有一份不同的臂长。每个模式只保存自己的构图表现状态。
 */
#pragma once

#include "Camera/GGYGOCameraMode.h"
#include "Curves/CurveFloat.h"

#include "GGYGOCameraMode_ThirdPerson.generated.h"

class UGGYGOCharacterMovementComponent;

UCLASS(Blueprintable)
class GGYGO_API UGGYGOCameraMode_ThirdPerson : public UGGYGOCameraMode
{
	GENERATED_BODY()

public:
	UGGYGOCameraMode_ThirdPerson();
	virtual void OnActivation() override;
	virtual void OnDeactivation() override;

protected:
	virtual FGGYGOCameraEvaluationResult UpdateView(float DeltaTime) override;
	virtual FGGYGOCameraEvaluationResult ValidateModeConfiguration() const override;

	/** 只作构图几何计算；真实轨迹转向率、实际速度方向和资格由 Movement 提供。 */
	FGGYGOCameraEvaluationResult ComputeSteeringLateralTarget(float ActualSignedTrajectoryYawRate,
		const FVector& WorldVelocityDirection, float WalkRunBlendAlpha,
		const FRotator& CameraRotation, float& OutLateralTarget) const;
	FGGYGOCameraEvaluationResult UpdateSteeringPresentation(float DeltaTime, float LateralTarget);
	void ResetSteeringPresentation();

	/** 显式关闭时不消费 Movement 来源，也不要求转向构图配置。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Third Person|Steering")
	bool bEnableWalkRunSteeringOffset = false;

	/** X=真实轨迹转向率绝对值（度/秒），Y=构图强度[0,1]；须从(0,0)起，至少两个键。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Third Person|Steering", meta = (EditCondition = "bEnableWalkRunSteeringOffset"))
	FRuntimeFloatCurve SteeringOffsetResponse;

	/** 满强度的世界弯道外侧偏移幅度（厘米），随后投影为纯 camera-right 横移。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Third Person|Steering", meta = (ClampMin = "0.0", EditCondition = "bEnableWalkRunSteeringOffset"))
	float SteeringOffsetAmplitude = 40.0f;

	/** 世界外侧幅度上限（厘米），在投影前限制；侧视的横移贡献自然减弱。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Third Person|Steering", meta = (ClampMin = "0.0", EditCondition = "bEnableWalkRunSteeringOffset"))
	float SteeringOffsetMaxDistance = 60.0f;

	/** 随 Movement 原 WalkRunBlendAlpha 连续混合的 Walk / Run 构图增益。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Third Person|Steering", meta = (ClampMin = "0.0", EditCondition = "bEnableWalkRunSteeringOffset"))
	float WalkSteeringOffsetScale = 0.35f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Third Person|Steering", meta = (ClampMin = "0.0", EditCondition = "bEnableWalkRunSteeringOffset"))
	float RunSteeringOffsetScale = 1.0f;

	/** 展示量进入和回收速度（每秒）；显式0表示立即到目标，不是非法配置的替代值。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Third Person|Steering", meta = (ClampMin = "0.0", EditCondition = "bEnableWalkRunSteeringOffset"))
	float SteeringOffsetEnterSpeed = 6.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Third Person|Steering", meta = (ClampMin = "0.0", EditCondition = "bEnableWalkRunSteeringOffset"))
	float SteeringOffsetReturnSpeed = 4.0f;

	/** 仅当前模式的展示滤波量；不是移动方向或镜头 POV 副本。 */
	float SteeringLateralOffset = 0.0f;

	/**
	 * 相对枢轴、按枢轴朝向旋转的偏移（视线局部空间）。
	 *
	 * X 为前后（负值把镜头拉到身后），Y 为左右（正值把角色推到画面左侧，
	 * 动作游戏常用来给主手武器留出视野），Z 为高低。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Third Person")
	FVector TargetOffset = FVector(-350.0f, 0.0f, 60.0f);

	/**
	 * 是否做穿墙规避。
	 *
	 * 关闭时镜头会穿进墙里看到背面。开启后镜头遇到遮挡会被拉近到遮挡点前，
	 * 代价是贴墙时镜头会明显推近。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Third Person")
	bool bPreventPenetration = true;

	/** 规避时镜头与遮挡面保持的距离。太小会导致近裁剪面切进墙体。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Third Person", meta = (ClampMin = "0.0", EditCondition = "bPreventPenetration"))
	float PenetrationProbeRadius = 14.0f;

	/**
	 * 遮挡解除后镜头回到原距离的速度（每秒比例）。
	 *
	 * 拉近是立即的（否则镜头会短暂穿墙），推远则要平滑 ——
	 * 立即推远会在经过门框、柱子时产生剧烈的前后抽动。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Third Person", meta = (ClampMin = "0.0", EditCondition = "bPreventPenetration"))
	float PenetrationRecoverySpeed = 4.0f;

private:
	FGGYGOCameraEvaluationResult UpdateSteeringFromMovement(float DeltaTime, const FRotator& CameraRotation);
	FGGYGOCameraEvaluationResult SteeringResponseFailure(const TCHAR* Reason) const;
	/** 只记原来源身份以清理展示量，不认证或推进 Movement 的代次。 */
	TWeakObjectPtr<UGGYGOCharacterMovementComponent> ObservedSteeringMovement;
	uint64 ObservedSteeringSourceEpoch = 0;
};
