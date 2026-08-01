/**
 * @file MotionDriver.h
 * @brief 运动驱动器
 *
 * 负责将意图管线产生的移动方向转化为实际位移。
 * 防滑步架构：输入方向 × 动画速度 = RequestDirectMove（帧级同步）。
 */
#pragma once

#include "CoreMinimal.h"
#include "Data/Logic/RuntimeData.h"

class ACharacter;
class UCharacterMovementComponent;
class USkeletalMeshComponent;

/**
 * 运动驱动器
 *
 * 负责将意图管线产生的移动方向转化为实际位移。
 * 防滑步架构：输入方向 × 动画速度 = RequestDirectMove（帧级同步）。
 *
 * 两条移动路径：
 *   - ProcessLocomotion：常规移动，AnimSpeed > 0 时使用 RequestDirectMove 直设速度
 *   - ProcessRootMotionMovement：Montage/技能动画专用，使用 RootMotionDelta/DeltaTime
 */
class FMotionDriver
{
public:
	void Init(ACharacter* InOwner);

	/** 每帧由 BaseCharacter::Tick 调用，根据 RuntimeData 驱动角色位移 */
	void Process(float DeltaTime, FRuntimeData& RuntimeData);

private:
	/** Root Motion 路径：Montage/技能动画的帧位移 → 瞬时速度 */
	void ProcessRootMotionMovement(const FVector& Delta, float DeltaTime);

	/** 常规移动路径：输入方向 × 动画速度 → RequestDirectMove（防滑步核心） */
	void ProcessLocomotion(float DeltaTime, const FVector& WorldDir, float InAnimSpeed, const FRuntimeData& RuntimeData);

	/** 回写 RuntimeData 中的 CurrentSpeed/bIsMoving/MoveAngle */
	void UpdateRuntimeData(FRuntimeData& RuntimeData);

	/** 所属角色 */
	ACharacter* Owner = nullptr;

	/** 角色移动组件（缓存，避免每帧 GetCharacterMovement） */
	UCharacterMovementComponent* Movement = nullptr;

	/** 骨骼网格组件（缓存，Init 时设置 RootMotionMode） */
	USkeletalMeshComponent* Mesh = nullptr;

	/** 默认最大行走速度（Init 时备份，供 AnimSpeed=0 时降级使用） */
	float DefaultMaxWalkSpeed = 600.f;
};
