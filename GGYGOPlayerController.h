/**
 * @file GGYGOPlayerController.h
 * @brief 玩家控制器 - IMC 注册（在蓝图 BP_PlayerController 里完成）
 * 
 * 输入响应在 PlayerCharacter 里处理，不在这里。
 * 这个类目前是空壳，后续可能加 IMC 切换逻辑。
 */

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "GGYGOPlayerController.generated.h"

UCLASS()
class GGYGO_API AGGYGOPlayerController : public APlayerController
{
	GENERATED_BODY()
};
