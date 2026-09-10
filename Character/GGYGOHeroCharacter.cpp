/**
 * @file GGYGOHeroCharacter.cpp
 * @brief 玩家操控角色实现
 */
#include "Character/GGYGOHeroCharacter.h"

#include "Camera/GGYGOCameraComponent.h"
#include "Character/Components/GGYGOHeroComponent.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Character/Data/GGYGOPawnData.h"

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

void AGGYGOHeroCharacter::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	if (CameraComponent)
	{
		CameraComponent->DetermineCameraModeDelegate.BindUObject(this, &ThisClass::DetermineCameraMode);
	}
}

TSubclassOf<UGGYGOCameraMode> AGGYGOHeroCharacter::DetermineCameraMode() const
{
	const UGGYGOPawnExtensionComponent* PawnExtComp = GetPawnExtensionComponent();
	if (!PawnExtComp)
	{
		return nullptr;
	}

	const UGGYGOPawnData* PawnData = PawnExtComp->GetPawnData<UGGYGOPawnData>();

	// 返回空是合法的：PawnData 还没到达时相机保持上一次的模式，
	// 而不是塞一个硬编码的兜底模式 —— 那会在初始化完成时产生一次可见的视角跳变。
	return PawnData ? PawnData->DefaultCameraMode : nullptr;
}
