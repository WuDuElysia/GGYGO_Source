/**
 * @file TurnBackPhaseProcessor.h
 * @brief TurnBack 逻辑时间轴处理器（急停转身的唯一真相）
 *
 * 逻辑层单点维护 RuntimeData.Movement.TurnBack：
 *   None → Frozen    Run 且当前输入接近角色前向的反方向时触发
 *   Frozen → Released 到达配置的释放时间点；第一段不可被输入打断
 *   CanYaw AnimNotify 到达后先进入事件闩；下一次参数阶段消费后立即设置 bCanYaw 和 bSecondSegment，进入 d1 输入接管
 *   Released → None  到达总时长，或 d1 开始后没有移动输入
 *
 * 第二段开始由逻辑时间轴标记，不读取动画旋转曲线推进生命周期。
 * 反向输入在一次 TurnBack 完成后需要先离开反向阈值，避免持续按住反向输入立即重触发。
 */
#pragma once

#include "CoreMinimal.h"
#include "Pipeline/Interfaces/IParameterProcessor.h"

class ACharacter;

class FTurnBackPhaseProcessor : public IParameterProcessor
{
public:
	/** @param InOwner 角色指针（读取当前 Actor 前向和 MovementConfig） */
	void Init(ACharacter* InOwner);

	/**
	 * 接收新版 ZZZAnim 的 CanYaw AnimNotify；只设置待消费事件闩，不直接写 RuntimeData。
	 * 下一次参数阶段在 TurnBack 活动时消费；None 阶段会丢弃该闩。
	 */
	void NotifyCanYaw();

	/**
	 * 前置条件：本帧 LocomotionIntentProcessor 已写 DesiredWorldMoveDir，
	 * 步态阶段已写 ResolvedGait；本函数使用 DeltaTime 推进 TurnBack 逻辑时间轴。
	 */
	virtual void Process(FRuntimeData& RuntimeData, float DeltaTime) override;

	ACharacter* Owner = nullptr;

	/** Frozen → Released 的时间点（秒）。 */
	float ReleaseTimeSeconds = 0.17f;

	/** CanYaw Notify 待消费事件闩；重复 Notify 合并为一次，None 阶段由 Process 丢弃。 */
	bool bCanYawNotified = false;

	/** TurnBack 自然结束的总时长（秒）。 */
	float DurationSeconds = 2.40f;

	/** 一次反向输入触发已经消费；离开反向阈值后才允许再次触发。 */
	bool bTurnBackInputLatched = false;
};
