/**
 * @file MotionDriver.h
 * @brief 运动驱动器
 *
 * 负责将意图管线产生的世界移动方向转化为实际位移，并在 TurnBack 解冻信号到达时
 * 根据 RootMotion 位移 XY 与目标输入方向的一致性选择直接方向或反向方向，一次性提交 Actor 转身。
 * 移动方向和 Actor 旋转都由本类作为唯一运行时写入方。
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
 * 负责将意图管线产生的世界移动方向转化为实际位移。
 * DesiredWorldMoveDir 是摄像机相对输入生成的世界主方向；TurnBack Frozen 期间优先使用进入转身前锁定的方向，
 * Released 首帧根据 RootMotion 位移 XY 与目标输入方向的一致性选择直接或 180° 反向方向，随后使用修正后的方向提交移动；None 使用当前输入方向。
 * RM_Speed 是曲线速度主值（cm/s），决定移动最终速度；RootMotionScale 只缩放该曲线速度。
 * RM_Dist 仅保留为距离差分诊断，RM_PosX/RM_PosY 通过 RootMotionDelta 提供 TurnBack 解冻方向依据。
 *
 * 两条移动路径：
 *   - ProcessLocomotion：常规移动，使用有效 RM_Speed × RootMotionScale 后 RequestDirectMove(..., true)
 *   - ProcessRootMotionMovement：解析后的世界方向 × RM_Speed × RootMotionScale → RequestDirectMove(..., true)
 */
class FMotionDriver
{
public:
	void Init(ACharacter* InOwner);

	/** 每帧由 Pipeline 消费移动命令并提交角色位移或 TurnBack Actor 旋转。 */
	void Process(
		float DeltaTime,
		const FCharacterMovementCommand& Command,
		FRuntimeData& RuntimeData);

private:
	/**
	 * 返回当前帧实际用于位移的世界方向。
	 * TurnBackPhase==Frozen 时使用进入转身前锁定的方向；Released 首帧优先使用刚修正的方向，之后使用当前输入方向。
	 */
	FVector ResolveWorldMoveDirection(const FCharacterMovementCommand& Command) const;

	enum class ETurnBackReleaseDirectionSource : uint8
	{
		RootMotionXY,
		RootMotionXYReversed,
		DesiredWorldMoveDir,
		EntryDirectionFallback
	};

	/**
	 * 解析解冻瞬间的正确方向：RootMotion 局部 XY 转世界后与目标输入比较；仅在相反时取 180°反向，
	 * 无位移时回退到输入/入口方向，并可返回本次选择的来源供诊断日志使用。
	 */
	FVector ResolveTurnBackReleaseDirection(
		const FCharacterMovementCommand& Command,
		ETurnBackReleaseDirectionSource* OutSource = nullptr) const;

	/** 仅在 Frozen→Released 的首个 Commit 阶段设置 Actor 朝向并锁存本帧修正方向。 */
	void ApplyTurnBackReleaseRotation(const FCharacterMovementCommand& Command);

	/**
	 * 曲线驱动移动：解析后的世界方向（普通输入或 TurnBack 锁定方向）决定水平世界方向，
	 * RM_Speed × RootMotionScale 决定最终速度；RM_Dist 仅用于距离诊断。
	 * 无输入时仅回退到 Actor 的水平前向，不旋转曲线局部轴。
	 */
	void ProcessRootMotionMovement(
		float DeltaTime,
		const FCharacterMovementCommand& Command);

	/** 常规移动路径：输入方向 × RM_Speed × RootMotionScale → RequestDirectMove(..., true)（防滑步核心） */
	void ProcessLocomotion(
		const FVector& WorldDir,
		const FCharacterMovementCommand& Command);

	/** 回写 RuntimeData 中的 CurrentSpeed/bIsMoving/MoveAngle */
	void UpdateRuntimeData(FRuntimeData& RuntimeData);

	/** 所属角色 */
	ACharacter* Owner = nullptr;

	/** 角色移动组件（缓存，避免每帧 GetCharacterMovement） */
	UCharacterMovementComponent* Movement = nullptr;

	/** 骨骼网格组件（缓存，Init 时设置 RootMotionMode） */
	USkeletalMeshComponent* Mesh = nullptr;

	/** 曲线速度缩放（Init 时从 MovementConfig.RootMotionScale 读取）；仅缩放 RM_Speed，不是固定速度来源。 */
	float RootMotionScale = 1.f;

	/** 防止同一段 TurnBack Released 阶段重复设置 Actor 朝向。 */
	bool bTurnBackReleaseRotationApplied = false;

	/** 解冻首帧由 RootMotion XY 与目标输入选择出的移动方向，仅存活到当前 Process 调用。 */
	FVector FrameTurnBackReleaseDirection = FVector::ZeroVector;
};
