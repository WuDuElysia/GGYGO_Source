/**
 * @file GGYGOHeroCharacter.h
 * @brief 被玩家操控的角色
 *
 * 在 `AGGYGOCharacterBase` 之上只多一件事：挂 `UGGYGOHeroComponent`。
 *
 * ## 为什么单独一个类
 * 输入处理只对被玩家操控的单位有意义。把 HeroComponent 挂在基类上，
 * 每个 AI 敌人都会带一个永远等不到 PlayerController 的组件；
 * 而那个组件是 InitState 的一个 feature，它卡住会让整个角色的初始化链条停住。
 *
 * 分成两个类之后，"这个单位要不要响应输入"由类型回答，
 * 不需要在运行时判断 —— AI 角色直接用基类。
 *
 * ## 队伍换角色
 * 队伍里的三个角色都应当是本类，换人时被操控的那一个获得 Controller，
 * 其余两个虽然挂着 HeroComponent 但没有 Controller，输入自然不会到达它们。
 */
#pragma once

#include "Character/GGYGOCharacterBase.h"
#include "CoreMinimal.h"

#include "GGYGOHeroCharacter.generated.h"

class UGGYGOCameraComponent;
class UGGYGOCameraMode;
class UGGYGOHeroComponent;
class UObject;

UCLASS(Config = Game, meta = (ShortTooltip = "被玩家操控的角色"))
class GGYGO_API AGGYGOHeroCharacter : public AGGYGOCharacterBase
{
	GENERATED_BODY()

public:
	AGGYGOHeroCharacter(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/** 输入组件。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Character")
	UGGYGOHeroComponent* GetHeroComponent() const { return HeroComponent; }

	/** 相机组件。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Character")
	UGGYGOCameraComponent* GetCameraComponent() const { return CameraComponent; }

protected:
	virtual void PostInitializeComponents() override;

	/**
	 * 决定默认相机模式。绑到相机组件的委托上。
	 *
	 * 取自 PawnData，这样每个角色可以有不同的默认视角
	 * （大剑角色需要更远的镜头，双刀角色可以更近）。
	 */
	TSubclassOf<UGGYGOCameraMode> DetermineCameraMode() const;

private:
	/** 输入处理。IMC 与优先级在这个组件的细节面板里配。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GGYGO|Character", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UGGYGOHeroComponent> HeroComponent;

	/**
	 * 相机。
	 *
	 * 挂在角色而不是 Controller 上：队伍换人时镜头参数应当跟着角色走，
	 * 而 Controller 是跨角色复用的。
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GGYGO|Character", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UGGYGOCameraComponent> CameraComponent;
};
