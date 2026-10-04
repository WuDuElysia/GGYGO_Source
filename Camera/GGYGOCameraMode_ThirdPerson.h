/**
 * @file GGYGOCameraMode_ThirdPerson.h
 * @brief 第三人称跟随相机
 *
 * 从角色枢轴沿视线反方向计算期望位置，并输出穿墙查询请求。
 * 最终碰撞与恢复由 CameraComponent 在模式混合和 Offset 之后统一处理。
 *
 * 不用 `USpringArmComponent` 而自己算，是因为弹簧臂是场景组件、
 * 它的长度是组件状态：多个相机模式要同时存在并混合时，
 * 它们没法各自持有一份不同的臂长。模式栈需要的是纯函数式的求值。
 */
#pragma once

#include "Camera/GGYGOCameraMode.h"

#include "GGYGOCameraMode_ThirdPerson.generated.h"

class UCurveVector;
class UObject;

UCLASS(Blueprintable)
class GGYGO_API UGGYGOCameraMode_ThirdPerson : public UGGYGOCameraMode
{
	GENERATED_BODY()

public:
	UGGYGOCameraMode_ThirdPerson();

protected:
	virtual void UpdateView(float DeltaTime) override;
	virtual FGGYGOCameraEvaluationResult ValidateModeConfiguration() const override;

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
};
