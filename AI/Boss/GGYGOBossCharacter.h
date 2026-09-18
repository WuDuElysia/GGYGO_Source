/** @file GGYGOBossCharacter.h @brief 不持有 ASC 的 Boss Avatar */
#pragma once

#include "Character/GGYGOCharacterBase.h"

#include "GGYGOBossCharacter.generated.h"

class UGGYGOMeleeTraceComponent;

/** Boss 的通用 Avatar；战斗状态由 AGGYGOBossState 持有。 */
UCLASS(Blueprintable)
class GGYGO_API AGGYGOBossCharacter : public AGGYGOCharacterBase
{
	GENERATED_BODY()

public:
	AGGYGOBossCharacter(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	UFUNCTION(BlueprintPure, Category = "GGYGO|Boss")
	UGGYGOMeleeTraceComponent* GetMeleeTraceComponent() const { return MeleeTraceComponent; }

protected:
	/** 只在判定窗口内 Tick，由 CombatActionAbility 控制。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GGYGO|Boss")
	TObjectPtr<UGGYGOMeleeTraceComponent> MeleeTraceComponent;
};
