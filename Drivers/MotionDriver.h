/**
 * @file MotionDriver.h
 * @brief 运动驱动器
 *
 * 负责把动画烘焙曲线转换为 Actor 的实际位移和 D0 朝向。RM_Speed 是速度主值，RM_Yaw 是累计角度曲线，
 * 参数处理器将其相邻采样值差分为 D0 每帧角度增量。普通 walkrun 无 dir 曲线，移动方向 = 玩家输入方向 DesiredWorldMoveDir。
 * TurnBack D0 使用进入时捕获的 Bone_Root 世界前向基准，并按当前 Bone_Root 相对入口的 yaw
 * 逆变换原始方向曲线，保证 Actor 转身后世界位移仍沿原直线；D0 临时关闭 CharacterMovement/Controller 自动朝向，
 * CanYaw 后进入 d1，像 walkrun 一样由输入接管并恢复原有旋转配置。
 * 动画实例不提取 Root Motion，AnimBP 只消费快照，不承担 Actor 移动或朝向。
 * 原始 dir 曲线分量约定 X=左右、Y=前后；MotionDriver 映射到 UE 局部 X=前、Y=右。
 */
#pragma once

#include "CoreMinimal.h"
#include "Data/Runtime/RuntimeData.h"
#include "Contracts/Pipeline/CharacterFrameCommands.h"

class ACharacter;
class UCharacterMovementComponent;
class USkeletalMeshComponent;

/**
 * 运动驱动器
 *
 * RM_Speed 是曲线速度主值（cm/s），AnimCurveYawDelta 是由累计 RM_Yaw 相邻采样差分得到的 D0 当前帧角度增量（度）。
 * 普通 walkrun 无 dir 曲线，移动方向 = 玩家输入方向 DesiredWorldMoveDir。
 * TurnBack D0：按 CanYaw 之前将 AnimCurveYawDelta（由累计 RM_Yaw 相邻采样差分得到）给 Actor 累加 yaw；位移方向使用进入时捕获的
 * Bone_Root 入口前向 yaw 基准，并用当前 Bone_Root 相对入口的逆 yaw 修正原始方向曲线，保持世界直线。
 * TurnBack d1：CanYaw 到达后像 walkrun 一样朝玩家输入方向移动并允许现有旋转配置接管朝向。
 */
class FMotionDriver
{
public:
	void Init(ACharacter* InOwner);

	/** 每帧由 Pipeline 消费移动命令并提交角色位移或 TurnBack 状态更新。 */
	void Process(
		float DeltaTime,
		const FCharacterMovementCommand& Command,
		FRuntimeData& RuntimeData);

private:
	/** 返回当前帧实际用于位移的世界方向；有效根轨迹曲线优先于输入和 TurnBack 兼容方向。 */
	FVector ResolveWorldMoveDirection(const FCharacterMovementCommand& Command) const;

	/** 进入 TurnBack 首帧捕获 d0 使用的 Bone_Root 世界水平前向；右向每帧按当前骨骼重新读取。 */
	void UpdateTurnBackDirectionIntent(const FCharacterMovementCommand& Command);

	/** 在 CanYaw 之前把由累计 RM_Yaw 相邻采样差分得到的 AnimCurveYawDelta 累加到 Actor 的水平 yaw。 */
	void ApplyTurnBackYaw(const FCharacterMovementCommand& Command);

	/** D0 期间关闭 CharacterMovement/Controller 自动朝向，进入 d1 或退出时恢复原配置。 */
	void UpdateTurnBackRotationMode(const FCharacterMovementCommand& Command);

	/** 读取当前 Bone_Root 的世界水平前向，并可选输出右向；缺失时回退 Actor 基准。 */
	void ResolveTurnBackBoneRootBasis(
		FVector& OutForward,
		FVector* OutRight = nullptr) const;

	/** 解析 TurnBack 世界移动方向：d0 用 Bone_Root 逆 yaw 修正 dir，d1 用玩家输入方向。 */
	FVector ResolveTurnBackWorldMoveDirection(
		const FCharacterMovementCommand& Command) const;

	/** 更新 TurnBack 动画计时和完成诊断。 */
	void UpdateTurnBackReleaseState(
		float DeltaTime,
		const FCharacterMovementCommand& Command);

	/** 在 Init 和 TurnBack 活动期间确保曲线驱动不会被引擎 Root Motion 叠加。 */
	void EnsureRootMotionIgnored();

	/** 退出 TurnBack 时清理进入锁定状态。 */
	void ResetTurnBackDirection();

	/** 曲线驱动移动：方向由 ResolveWorldMoveDirection 决定，RM_Speed × RootMotionScale 决定速度。 */
	void ProcessRootMotionMovement(
		float DeltaTime,
		const FCharacterMovementCommand& Command);

	/** 常规移动路径：世界方向 × RM_Speed × RootMotionScale。 */
	void ProcessLocomotion(
		const FVector& WorldDir,
		const FCharacterMovementCommand& Command);

	/** 回写 RuntimeData 中的 CurrentSpeed/bIsMoving/MoveAngle。 */
	void UpdateRuntimeData(FRuntimeData& RuntimeData);

	/** 所属角色 */
	ACharacter* Owner = nullptr;

	/** 角色移动组件（缓存，避免每帧 GetCharacterMovement） */
	UCharacterMovementComponent* Movement = nullptr;

	/** 骨骼网格组件（缓存，Init 时设置 RootMotionMode 并读取 TurnBack 序列） */
	USkeletalMeshComponent* Mesh = nullptr;

	/** 曲线速度缩放（Init 时从 MovementConfig.RootMotionScale 读取）。 */
	float RootMotionScale = 1.f;

	/** 是否已锁定本次 TurnBack 的进入方向基准。 */
	bool bTurnBackDirectionActive = false;

	/** 本实例是否拥有 D0 的临时自动朝向覆盖；只在首次进入 D0 时保存原配置，离开 D0 时恢复。 */
	bool bTurnBackRotationOverrideActive = false;
	bool bSavedOrientRotationToMovement = false;
	bool bSavedUseControllerDesiredRotation = false;
	bool bSavedUseControllerRotationYaw = false;

	/** TurnBack D0 进入时保存的 Bone_Root 世界水平前向；只需保留其 yaw 作为世界直线基准。 */
	FVector TurnBackDirectionBasisForward = FVector(1.f, 0.f, 0.f);

	/** TurnBack 动画是否已完成；只用于避免重复输出完成诊断。 */
	bool bTurnBackAnimationComplete = false;

	/** TurnBack 动画从 Frozen 入口开始累计的播放时间。 */
	float TurnBackAnimationElapsed = 0.f;

	/** 从 UZZZAnimInstance 的 TurnBack AnimSequence 读取的播放时长。 */
	float TurnBackAnimationLength = 0.f;

	/** 是否已经进入一段需要等待动画完成诊断的 TurnBack。 */
	bool bTurnBackAnimationActive = false;
};
