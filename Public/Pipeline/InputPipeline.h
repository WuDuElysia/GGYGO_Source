/**
* @file InputPipeline.h
 * @brief 输入管线 - 输入加工和缓冲
 *
 * 唯一写入 InputData 的系统。
 * 负责防抖缓冲、动作按键缓冲、双缓冲推进。
 */
#pragma once

#include "CoreMinimal.h"
#include "Data/InputData.h"

class FInputPipeline
{
public:
	/**
	 * 构造函数，绑定输入数据容器
	 * @param InInputData 输入数据容器（由 BaseCharacter 持有，生命周期由调用方管理）
	 */
	explicit FInputPipeline(FInputData& InInputData);

	/**
	 * 每帧调用，处理输入
	 * 在 BaseCharacter::Tick 的第 2 步调用
	 * @param DeltaTime 帧间隔
	 */
	void Process(float DeltaTime);

	// ============================================================
	// 原始输入写入接口（由 PlayerCharacter 的回调调用）
	// InputPipeline 是 PlayerCharacter 和 InputData 之间的桥梁
	// ============================================================

	/** 写入移动输入（PlayerCharacter::OnMoveInput 调用） */
	void SetMoveInput(const FVector2D& Value);

	/** 写入移动输入结束（PlayerCharacter::OnMoveCompleted 调用） */
	void ClearMoveInput();

	/** 写入视角输入（PlayerCharacter::OnLookInput 调用） */
	void SetLookInput(const FVector2D& Value);

	/** 写入攻击按下 */
	void SetAttackPressed();

	/** 写入闪避按下 */
	void SetDodgePressed();

	/** 写入冲刺状态 */
	void SetSprintHeld(bool bHeld);

	/** 写入强制步行状态（Ctrl 键） */
	void SetForceWalkHeld(bool bHeld);

private:
	/** 输入数据容器（不拥有，由 BaseCharacter 管理生命周期） */
	FInputData& InputData;

	/** 动作按键缓冲时间（秒） */
	static constexpr float ActionBufferTime = 0.15f;

	/** 移动防抖缓冲时间（秒） */
	static constexpr float MoveFlickerBuffer = 0.05f;

	// 临时存储本帧的原始输入（Process 时写入 InputData）
	FVector2D PendingMoveInput = FVector2D::ZeroVector;
	FVector2D PendingLookInput = FVector2D::ZeroVector;
	bool bPendingAttack = false;
	bool bPendingDodge = false;
	bool bPendingSprint = false;
	bool bPendingForceWalk = false;

	/** 移动防抖计时器 */
	float MoveFlickerTimer = 0.f;

	/** 上一帧有效的移动方向（防抖用） */
	FVector2D LastValidMoveDir = FVector2D::ZeroVector;
};