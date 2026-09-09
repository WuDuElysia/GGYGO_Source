/**
 * @file GGYGOAbilityFailureMessages.h
 * @brief 能力激活失败的反馈消息
 *
 * 能力激活失败时，玩家需要知道"为什么没打出来"。这里定义两条通过
 * `UGameplayMessageSubsystem` 广播的消息载荷：
 *   - 文本提示：耐力不足、还在冷却
 *   - 失败 Montage：例如无耐力时播一个"力竭"的小动作
 *
 * 广播方是 `UGGYGOGameplayAbility::NativeOnAbilityFailedToActivate`，
 * 接收方是 UI 与角色表现层，都是只读消费。
 */
#pragma once

#include "GameplayTagContainer.h"
#include "NativeGameplayTags.h"

#include "GGYGOAbilityFailureMessages.generated.h"

class APlayerController;
class UAnimMontage;

/** 文本提示消息的通道。 */
GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_GGYGO_Ability_SimpleFailureMessage);
/** 失败 Montage 消息的通道。 */
GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_GGYGO_Ability_PlayMontageFailureMessage);

/** 用文本告知玩家失败原因。 */
USTRUCT(BlueprintType)
struct FGGYGOAbilitySimpleFailureMessage
{
	GENERATED_BODY()

	/** 请求激活的玩家控制器。ASC 不属于玩家时为空。 */
	UPROPERTY(BlueprintReadWrite, Category = "Ability")
	TObjectPtr<APlayerController> PlayerController = nullptr;

	/** 本次失败的**全部**原因 Tag，不只是匹配到文本的那一个。 */
	UPROPERTY(BlueprintReadWrite, Category = "Ability")
	FGameplayTagContainer FailureTags;

	/** 要显示给玩家的文本。 */
	UPROPERTY(BlueprintReadWrite, Category = "Ability")
	FText UserFacingReason;
};

/** 用一段动画表达失败。 */
USTRUCT(BlueprintType)
struct FGGYGOAbilityMontageFailureMessage
{
	GENERATED_BODY()

	/** 请求激活的玩家控制器，可为空。 */
	UPROPERTY(BlueprintReadWrite, Category = "Ability")
	TObjectPtr<APlayerController> PlayerController = nullptr;

	/** 要播放动画的 Avatar。 */
	UPROPERTY(BlueprintReadWrite, Category = "Ability")
	TObjectPtr<AActor> AvatarActor = nullptr;

	/** 本次失败的全部原因 Tag。 */
	UPROPERTY(BlueprintReadWrite, Category = "Ability")
	FGameplayTagContainer FailureTags;

	/** 匹配到的失败动画。 */
	UPROPERTY(BlueprintReadWrite, Category = "Ability")
	TObjectPtr<UAnimMontage> FailureMontage = nullptr;
};
