/**
 * @file TurnBackPhaseProcessor.h
 * @brief TurnBack 相位处理器（急停转身的唯一真相）
 *
 * 逻辑层单点维护 RuntimeData.Movement.TurnBack.Phase：
 *   None → Frozen    Run 且当前输入接近角色前向的反方向时触发；记录进入前的移动方向
 *   Frozen → Released sig_turnback 曲线越过阈值（动画告知"可解冻"）
 *   Released → None  角色朝向已对齐当前输入，或已无移动输入
 *
 * 反向输入判定只在此处进行一次；MotionDriver 与动画快照都只读该相位，
 * 不再各自重复检测，消除双探测器不一致。
 */
#pragma once

#include "CoreMinimal.h"
#include "Pipeline/Interfaces/IParameterProcessor.h"

class ACharacter;

class FTurnBackPhaseProcessor : public IParameterProcessor
{
public:
	/** @param InOwner 角色指针（读取当前 Actor 前向用于反向判定与进入方向记录） */
	void Init(ACharacter* InOwner);

	/**
	 * 前置条件：本帧 LocomotionIntentProcessor 已写 DesiredWorldMoveDir、
	 * 步态阶段已写 ResolvedGait、FAnimSignalParameterProcessor 已采样 sig_turnback。
	 */
	virtual void Process(FRuntimeData& RuntimeData, float DeltaTime) override;

private:
	ACharacter* Owner = nullptr;
};
