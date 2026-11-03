#pragma once

#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Combatants/GGYGOCombatantState.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "UObject/Package.h"

#include "GGYGOCombatantBindingLifecycleTestTypes.generated.h"

/** 绑定流程测试用的具体宿主。 */
UCLASS(Transient)
class AGGYGOCombatantBindingTestState : public AGGYGOCombatantState
{
	GENERATED_BODY()

public:
	AGGYGOCombatantBindingTestState(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/** 测试世界不 BeginPlay，显式调用真实的 EndPlay 覆盖。 */
	void InvokeEndPlayForTest() { EndPlay(EEndPlayReason::Destroyed); }
};

/** 只带生产 PawnExtension 的最小 Pawn。 */
UCLASS(Transient)
class AGGYGOCombatantBindingTestPawn : public APawn
{
	GENERATED_BODY()

public:
	AGGYGOCombatantBindingTestPawn(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	UGGYGOPawnExtensionComponent* GetPawnExtensionForTest() const { return PawnExtension; }

private:
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneComponent> TestRoot;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UGGYGOPawnExtensionComponent> PawnExtension;
};

#if WITH_DEV_AUTOMATION_TESTS
/** 已初始化 Actor、未 BeginPlay 的私有 Game 世界；析构时销毁世界与上下文。
 *  需要初始化 Actor：生成时才走 PostInitializeComponents，Actor 的动态委托（OnDestroyed）才会执行。 */
struct FGGYGOCombatantBindingTestWorld
{
	UEngine* Engine = GEngine;
	UWorld* World = nullptr;

	FGGYGOCombatantBindingTestWorld()
	{
		UWorld::InitializationValues Init;
		Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(false)
			.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
		World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
			ERHIFeatureLevel::Num, &Init);
		if (World && Engine)
		{
			Engine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
		}
	}

	~FGGYGOCombatantBindingTestWorld()
	{
		if (World)
		{
			World->DestroyWorld(false);
			if (Engine)
			{
				Engine->DestroyWorldContext(World);
			}
			if (UPackage* Package = World->GetPackage())
			{
				Package->SetDirtyFlag(false);
			}
		}
	}

	FGGYGOCombatantBindingTestWorld(const FGGYGOCombatantBindingTestWorld&) = delete;
	FGGYGOCombatantBindingTestWorld& operator=(const FGGYGOCombatantBindingTestWorld&) = delete;

	AGGYGOCombatantBindingTestState* SpawnHost() const
	{
		return World ? World->SpawnActor<AGGYGOCombatantBindingTestState>() : nullptr;
	}

	AGGYGOCombatantBindingTestPawn* SpawnPawn() const
	{
		return World ? World->SpawnActor<AGGYGOCombatantBindingTestPawn>() : nullptr;
	}
};

/** 记录一个 PawnExtension 的就绪／释放通知。 */
struct FGGYGOPawnASCNoticeLog
{
	TArray<FGGYGOPawnASCNotice> Notices;

	int32 Count(EGGYGOPawnASCNoticeKind Kind) const
	{
		int32 Result = 0;
		for (const FGGYGOPawnASCNotice& Notice : Notices) { Result += Notice.Kind == Kind ? 1 : 0; }
		return Result;
	}

	void Observe(FGGYGOLocalASCSubscription& Subscription, UGGYGOPawnExtensionComponent* Extension)
	{
		Subscription.Subscribe(Extension, [this](const FGGYGOPawnASCNotice& Notice) { Notices.Add(Notice); });
	}
};
#endif
