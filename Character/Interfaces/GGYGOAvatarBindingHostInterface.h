#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"

#include "GGYGOAvatarBindingHostInterface.generated.h"

class APawn;

UINTERFACE(meta = (CannotImplementInterfaceInBlueprint))
class GGYGO_API UGGYGOAvatarBindingHostInterface : public UInterface
{
    GENERATED_UINTERFACE_BODY()
};

/**
 * ASC 宿主（ASC 的组件 Owner）向 Pawn 一侧暴露的唯一入口。
 *
 * 绑定由宿主主动发起，Pawn 不请求绑定；Pawn 只在需要立即解除时（死亡后销毁前）请求释放。
 * 读取宿主的 ASC 用 IAbilitySystemInterface。
 */
class GGYGO_API IGGYGOAvatarBindingHostInterface
{
    GENERATED_IINTERFACE_BODY()

public:
    /** 只在 ExpectedAvatar 仍是宿主当前 Avatar 时释放；已被替换的旧 Pawn 不处理。 */
    virtual void ReleaseAvatarBinding(APawn* ExpectedAvatar) = 0;
};
