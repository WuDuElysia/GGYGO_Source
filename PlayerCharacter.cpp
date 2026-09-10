/**
 * @file PlayerCharacter.cpp
 * @brief 旧玩家角色过渡壳实现
 */
#include "PlayerCharacter.h"

#include "EnhancedInputComponent.h"
#include "GameFramework/PlayerController.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PlayerCharacter)

APlayerCharacter::APlayerCharacter()
{
}

void APlayerCharacter::BeginPlay()
{
	Super::BeginPlay();

	if (const APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		if (PC->PlayerCameraManager)
		{
			PC->PlayerCameraManager->ViewPitchMin = PitchMin;
			PC->PlayerCameraManager->ViewPitchMax = PitchMax;
		}
	}
}

void APlayerCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!EIC)
	{
		return;
	}

	if (IA_Move)
	{
		// 只绑 Triggered，不再绑 Completed。
		//
		// 旧实现需要 Completed 去调 ClearMoveInput()，因为输入被存在
		// FInputPipeline 自己的双缓冲里，不清就会一直保持上一次的方向。
		// AddMovementInput 没有这个问题：CMC 每帧消费完 Acceleration 就自动归零，
		// 松手后不再调用 AddMovementInput 即等于零输入。
		EIC->BindAction(IA_Move, ETriggerEvent::Triggered, this, &APlayerCharacter::OnMoveInput);
	}

	if (IA_Look)
	{
		EIC->BindAction(IA_Look, ETriggerEvent::Triggered, this, &APlayerCharacter::OnLookInput);
	}

	if (IA_Sprint)
	{
		EIC->BindAction(IA_Sprint, ETriggerEvent::Started, this, &APlayerCharacter::OnSprintInput);
		EIC->BindAction(IA_Sprint, ETriggerEvent::Completed, this, &APlayerCharacter::OnSprintInput);
	}

	if (IA_ForceWalk)
	{
		EIC->BindAction(IA_ForceWalk, ETriggerEvent::Started, this, &APlayerCharacter::OnForceWalkInput);
		EIC->BindAction(IA_ForceWalk, ETriggerEvent::Completed, this, &APlayerCharacter::OnForceWalkInput);
	}
}

void APlayerCharacter::OnMoveInput(const FInputActionValue& Value)
{
	const FVector2D Input = Value.Get<FVector2D>();
	if (Input.IsNearlyZero())
	{
		return;
	}

	// 摄像机相对移动：摇杆的"上"是摄像机朝向的前方，不是角色的前方。
	// 只取 Yaw 构造基准 —— 带上 Pitch 会让俯视时的前向带有向下的分量，
	// 角色会试图往地里走。
	const FRotator YawOnlyRotation(0.0f, GetControlRotation().Yaw, 0.0f);
	const FRotationMatrix RotationBasis(YawOnlyRotation);

	const FVector Forward = RotationBasis.GetUnitAxis(EAxis::X);
	const FVector Right = RotationBasis.GetUnitAxis(EAxis::Y);

	// AddMovementInput 会累加进 CMC 的 Acceleration，于是自动获得
	// SavedMove 保存与网络预测。旧链路把输入存在 Pipeline 私有缓冲里，CMC 看不到。
	AddMovementInput(Forward * Input.Y + Right * Input.X);
}

void APlayerCharacter::OnLookInput(const FInputActionValue& Value)
{
	const FVector2D Input = Value.Get<FVector2D>();

	AddControllerYawInput(Input.X);
	AddControllerPitchInput(Input.Y);
}

void APlayerCharacter::OnSprintInput(const FInputActionValue& Value)
{
	// 空实现。新步态体系没有冲刺档位，见头文件对 IA_Sprint 的说明。
}

void APlayerCharacter::OnForceWalkInput(const FInputActionValue& Value)
{
	// 空实现。见头文件对 IA_ForceWalk 的说明。
}
