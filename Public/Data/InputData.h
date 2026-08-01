/**
 * @file InputData.h
 * @brief 输入数据容器 - 输入管线的唯一输出
 * 
 * InputPipeline 是唯一写入 InputData 的系统。
 * IntentProcessors 只读 InputData 来生成意图。
 * 双缓冲设计：CurrentFrame 和 LastFrame。
 */

#pragma once

#include "CoreMinimal.h"

/**
 * 后处理输入数据
 * 经过防抖缓冲和动作按键缓冲后的输入状态
 */
struct 
FProcessedInput
{
	/** 移动输入（后处理，防抖后） */
	FVector2D Move = FVector2D::ZeroVector;

	/** 视角输入（后处理） */
	FVector2D Look = FVector2D::ZeroVector;

	/** 攻击键持续按住 */
	bool bAttackHeld = false;

	/** 闪避键持续按住 */
	bool bDodgeHeld = false;

	/** 冲刺键持续按住 */
	bool bSprintHeld = false;

	/** 强制步行键持续按住（Ctrl） */
	bool bForceWalkHeld = false;

	// ============================================================
	// 缓冲计时器
	// 按下时设为缓冲时间（如 0.2 秒），每帧递减
	// 窗口期内视为有效按下，解决输入丢失问题
	// ============================================================

	/** 攻击缓冲计时器 */
	float AttackBufferTimer = 0.f;

	/** 闪避缓冲计时器 */
	float DodgeBufferTimer = 0.f;

	/** 缓冲窗口内视为有效按下 */
	bool IsAttackPressed() const { return AttackBufferTimer > 0.f; }

	/** 缓冲窗口内视为有效按下 */
	bool IsDodgePressed() const { return DodgeBufferTimer > 0.f; }

	/** 消费攻击输入 */
	void ConsumeAttack() { AttackBufferTimer = 0.f; }

	/** 消费闪避输入 */
	void ConsumeDodge() { DodgeBufferTimer = 0.f; }
};

/**
 * 输入数据容器
 * 双缓冲设计，支持上一帧/当前帧对比
 * InputPipeline 是唯一写入者
 */
class FInputData
{
public:
	/** 当前帧输入数据 */
	FProcessedInput CurrentFrame;

	/** 上一帧输入数据 */
	FProcessedInput LastFrame;

	/**
	 * 推进帧缓冲
	 * 每帧开头调用，将当前帧数据存为上一帧
	 */
	void AdvanceFrame()
	{
		LastFrame = CurrentFrame;
	}
};
