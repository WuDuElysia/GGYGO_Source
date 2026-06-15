/**
 * @file ViewRotationProcessor.h
 * @brief 视角旋转处理器
 *
 * 鼠标/摇杆增量 → AddControllerYawInput/PitchInput → 写入 ControlRotation。
 * 必须在 LocomotionIntentProcessor 之前执行（后者依赖 ControlRotation）。
 * 需要 ACharacter 指针，通过 Init 注入，UObject 生命周期由 GC 管理所以用指针。
 */
#pragma once

#include "Pipeline/Interfaces/IIntentProcessor.h"

class ACharacter;

class FViewRotationProcessor : public IIntentProcessor
{
public:
	/** 注入角色指针（BeginPlay 时调用） */
	void Init(ACharacter* InOwner);

	virtual void Process(const FInputData& InputData, FRuntimeData& RuntimeData) override;

private:
	/** 角色指针（不拥有，UObject 由 GC 管理） */
	ACharacter* Owner = nullptr;
};
