/**
 * @file GGYGOHeroCharacter.cpp
 * @brief 玩家操控角色实现
 */
#include "Character/GGYGOHeroCharacter.h"

#include "Camera/GGYGOCameraComponent.h"
#include "Character/Components/GGYGOHeroComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOHeroCharacter)

AGGYGOHeroCharacter::AGGYGOHeroCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	HeroComponent = CreateDefaultSubobject<UGGYGOHeroComponent>(TEXT("HeroComponent"));

	CameraComponent = CreateDefaultSubobject<UGGYGOCameraComponent>(TEXT("CameraComponent"));

	// 附着到角色根而不是 Mesh：附到 Mesh 会让镜头跟着动画的位移抖动，
	// 而相机模式已经自己处理了跟随与平滑。
	CameraComponent->SetupAttachment(RootComponent);
}
