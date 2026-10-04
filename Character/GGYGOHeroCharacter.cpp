/**
 * @file GGYGOHeroCharacter.cpp
 * @brief 玩家操控角色实现
 */
#include "Character/GGYGOHeroCharacter.h"

#include "Camera/GGYGOCameraComponent.h"
#include "Character/Components/GGYGOHeroComponent.h"
#include "Character/Components/GGYGOPyriosRenderComponent.h"
#include "Combat/HitDetection/GGYGOMeleeTraceComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOHeroCharacter)

AGGYGOHeroCharacter::AGGYGOHeroCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	HeroComponent = CreateDefaultSubobject<UGGYGOHeroComponent>(TEXT("HeroComponent"));
	MeleeTraceComponent = CreateDefaultSubobject<UGGYGOMeleeTraceComponent>(TEXT("MeleeTraceComponent"));

	CameraComponent = CreateDefaultSubobject<UGGYGOCameraComponent>(TEXT("CameraComponent"));

	// 附着到角色根而不是 Mesh：附到 Mesh 会让镜头跟着动画的位移抖动，
	// 而相机模式已经自己处理了跟随与平滑。
	CameraComponent->SetupAttachment(RootComponent);

	// Preserve the serialized subobject class/name of existing Blueprints. This
	// compatibility subclass contains no character selection or asset defaults;
	// its reusable base only renders the bindings authored on the character BP.
	StylizedRenderComponent = CreateDefaultSubobject<UGGYGOPyriosRenderComponent>(TEXT("StylizedRenderComponent"));
}
