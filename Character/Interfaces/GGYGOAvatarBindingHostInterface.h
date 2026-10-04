#pragma once

#include "CoreMinimal.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Misc/Optional.h"
#include "UObject/Interface.h"

#include "GGYGOAvatarBindingHostInterface.generated.h"

enum class EGGYGOAvatarBindingHostOperation : uint8
{
    Invalid = 0,
    Initialize,
    Release,
    Refresh
};

enum class EGGYGOAvatarBindingHostReason : uint8
{
    None = 0,
    InvalidRequest,
    InvalidHost,
    UnsupportedHost,
    InvalidASC,
    InvalidPawn,
    InvalidExtension,
    AuthorityDenied,
    EndpointMismatch,
    ContextMismatch,
    ResourceMismatch,
    ResourceConflict,
    LifecycleClosed,
    CallerInvalidated,
    NativeWriteBusy,
    NativeStepFailed,
    LocalStepFailed,
    PublicationFailed,
    ReadyNotEstablished
};

struct GGYGO_API FGGYGOAvatarBindingHostRequest
{
    EGGYGOAvatarBindingHostOperation Operation =
        EGGYGOAvatarBindingHostOperation::Invalid;

    TWeakObjectPtr<AActor> ExpectedHost{};
    TWeakObjectPtr<UGGYGOAbilitySystemComponent> ExpectedASC{};
    TWeakObjectPtr<APawn> ExpectedPawn{};
    TWeakObjectPtr<UGGYGOPawnExtensionComponent> ExpectedExtension{};

    FGGYGOAvatarBindingContext ExpectedContext{};
    FGGYGOPawnASCResourceHandle ExpectedResource{};
};

enum class EGGYGOAvatarBindingHostStep : uint8
{
    Invalid = 0,
    ActorInfoInit,
    ActorInfoClear,
    ActorInfoRefresh,
    CancelAbilities,
    ClearAbilityInput,
    RemoveGameplayCues,
    InstallLocalResources,
    WithdrawLocalResources,
    NotifyLocalReady,
    NotifyLocalReleased,
    PublishNotice
};

/** Only records an actual ClearAbilityInput return; never grants permission. */
struct GGYGO_API FGGYGOAvatarBindingHostInputClearHistory
{
    FGGYGOAvatarBindingContext OriginalContext{};
    FGGYGOPawnASCResourceHandle OriginalResource{};
};

/** One actual returned step. Exactly the matching payload is populated. */
struct GGYGO_API FGGYGOAvatarBindingHostStepResult
{
    EGGYGOAvatarBindingHostStep Step =
        EGGYGOAvatarBindingHostStep::Invalid;

    TOptional<FGGYGOAvatarBindingResult> ASCResult{};
    TOptional<FGGYGOPawnASCLocalResult> LocalResult{};
    TOptional<FGGYGOAvatarBindingHostInputClearHistory> InputClear{};
};

/** Request-stack history, not a current binding/Ready authority. */
struct GGYGO_API FGGYGOAvatarBindingHostResult
{
    EGGYGOAvatarBindingOutcome Outcome =
        EGGYGOAvatarBindingOutcome::Rejected;
    EGGYGOAvatarBindingHostReason Reason =
        EGGYGOAvatarBindingHostReason::InvalidRequest;

    TArray<FGGYGOAvatarBindingHostStepResult> Steps{};
};

UINTERFACE(meta = (CannotImplementInterfaceInBlueprint))
class GGYGO_API UGGYGOAvatarBindingHostInterface : public UInterface
{
    GENERATED_UINTERFACE_BODY()
};

class GGYGO_API IGGYGOAvatarBindingHostInterface
{
    GENERATED_IINTERFACE_BODY()

public:
    virtual FGGYGOAvatarBindingHostResult RequestAvatarBinding(
        const FGGYGOAvatarBindingHostRequest& Request) = 0;
};
