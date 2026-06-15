/**
 * @file ViewRotationProcessor.cpp
 * @brief 视角旋转处理器实现
 */
#include "Pipeline/Intents/ViewRotationProcessor.h"
#include "Data/InputData.h"
#include "Data/RuntimeData.h"
#include "GameFramework/Character.h"

void FViewRotationProcessor::Init(ACharacter* InOwner)
{
	Owner = InOwner;
}

void FViewRotationProcessor::Process(const FInputData& InputData, FRuntimeData& RuntimeData)
{
	if (!Owner) return;

	const FVector2D& Look = InputData.CurrentFrame.Look;

	// 将鼠标增量应用到控制器旋转
	Owner->AddControllerYawInput(Look.X);
	Owner->AddControllerPitchInput(Look.Y);

	// 从控制器读取最终旋转（经过 PlayerCameraManager Clamp 后的值）
	FRotator ControlRot = Owner->GetControlRotation();
	RuntimeData.ControlRotation = ControlRot;
	RuntimeData.ViewYaw = ControlRot.Yaw;
	RuntimeData.ViewPitch = ControlRot.Pitch;
}
