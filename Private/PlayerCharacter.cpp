/**
 * @file PlayerCharacter.cpp
 * @brief 玩家角色实现 - 输入绑定和摄像机配置
 */
#include "PlayerCharacter.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/CharacterMovementComponent.h"

APlayerCharacter::APlayerCharacter()
{
}

void APlayerCharacter::BeginPlay()
{
	Super::BeginPlay();

	// 设置摄像机俯仰角限制
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (PC && PC->PlayerCameraManager)
	{
		PC->PlayerCameraManager->ViewPitchMin = PitchMin;
		PC->PlayerCameraManager->ViewPitchMax = PitchMax;
	}
}

void APlayerCharacter::SetupPlayerInputComponent(
	UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	// 转换为 Enhanced Input 组件
	UEnhancedInputComponent* EIC =
		Cast<UEnhancedInputComponent>(PlayerInputComponent);

#if !UE_BUILD_SHIPPING
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Orange,
			FString::Printf(TEXT("SetupInput: EIC=%s IA_Move=%s IA_Look=%s"),
				EIC ? TEXT("OK") : TEXT("NULL"),
				IA_Move ? TEXT("OK") : TEXT("NULL"),
				IA_Look ? TEXT("OK") : TEXT("NULL")));
	}
#endif

	if (!EIC) return;

	// 绑定移动输入
	if (IA_Move)
	{
		// 按住时每帧触发
		EIC->BindAction(IA_Move, ETriggerEvent::Triggered,
			this, &APlayerCharacter::OnMoveInput);
		// 松开时触发，通知 InputPipeline 清零
		EIC->BindAction(IA_Move, ETriggerEvent::Completed,
			this, &APlayerCharacter::OnMoveCompleted);
	}

	// 绑定视角输入
	if (IA_Look)
	{
		EIC->BindAction(IA_Look, ETriggerEvent::Triggered,
			this, &APlayerCharacter::OnLookInput);
	}
}

void APlayerCharacter::OnMoveInput(const FInputActionValue& Value)
{
	FVector2D Input = Value.Get<FVector2D>();
	InputPipeline->SetMoveInput(Input);

#if !UE_BUILD_SHIPPING
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 0.f, FColor::Red,
			FString::Printf(TEXT("OnMoveInput: %s"), *Input.ToString()));
	}
#endif
}

void APlayerCharacter::OnMoveCompleted(const FInputActionValue& Value)
{
	InputPipeline->ClearMoveInput();
}

void APlayerCharacter::OnLookInput(const FInputActionValue& Value)
{
	FVector2D Input = Value.Get<FVector2D>();
	InputPipeline->SetLookInput(Input);
}
