/**
 * @file GGYGOCharacterDeathPolicyTest.cpp
 * @brief 验证死亡广播之后的蓝图选择、客户端销毁权限及生产蓝图入口。
 */
#include "Character/Tests/GGYGOCharacterDeathPolicyTestTypes.h"

#include "Character/Components/GGYGOHealthComponent.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "TimerManager.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOCharacterDeathPolicyTestTypes)

bool AGGYGOCharacterDeathPolicyTestPawn::K2_HandleDeathFinished_Implementation()
{
	++PolicyCalls;
	if (!bKeepPawn) { UninitAndDestroy(); }
	return true;
}

void AGGYGOCharacterDeathPolicyTestPawn::ObserveDeathFinished(AActor* OwningActor)
{
	bPawnIntactDuringBroadcast = IsValid(OwningActor) && !OwningActor->IsHidden()
		&& OwningActor->GetLifeSpan() == 0.0f && PolicyCalls == 0;
}

#if WITH_DEV_AUTOMATION_TESTS
namespace GGYGOCharacterDeathPolicyTests
{
	UWorld* CreateWorld()
	{
		UWorld::InitializationValues Init;
		Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(true)
			.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
		// AActor::ProcessEvent 只在已初始化世界执行动态委托及蓝图函数。
		if (World) { World->InitializeActorsForPlay(FURL()); }
		return World;
	}

	void FinishDeath(AGGYGOCharacterBase* Pawn, AGGYGOCharacterDeathPolicyTestPawn* Observer)
	{
		Pawn->GetHealthComponent()->OnDeathFinished.AddDynamic(Observer, &AGGYGOCharacterDeathPolicyTestPawn::ObserveDeathFinished);
		Pawn->GetHealthComponent()->StartDeath();
		Pawn->GetHealthComponent()->FinishDeath();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOCharacterDeathPolicyChoice,
	"GGYGO.Character.DeathPolicy.DeferredChoiceAndAuthority", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGGYGOCharacterDeathPolicyChoice::RunTest(const FString& Parameters)
{
	UWorld* World = GGYGOCharacterDeathPolicyTests::CreateWorld();
	if (!TestNotNull(TEXT("死亡处理测试世界"), World)) { return false; }
	TStrongObjectPtr<UPackage> Package(World->GetPackage());
	ON_SCOPE_EXIT { World->DestroyWorld(false); Package->SetDirtyFlag(false); };
	auto* DestroyPawn = World->SpawnActor<AGGYGOCharacterDeathPolicyTestPawn>();
	auto* KeepPawn = World->SpawnActor<AGGYGOCharacterDeathPolicyTestPawn>();
	auto* ClientPawn = World->SpawnActor<AGGYGOCharacterDeathPolicyTestPawn>();
	auto* UnconfiguredPawn = World->SpawnActor<AGGYGOCharacterBase>();
	if (!DestroyPawn || !KeepPawn || !ClientPawn || !UnconfiguredPawn) { AddError(TEXT("测试 Pawn 生成失败")); return false; }
	KeepPawn->bKeepPawn = true;
	ClientPawn->UseSimulatedRole();
	for (auto* Pawn : {DestroyPawn, KeepPawn, ClientPawn})
	{
		GGYGOCharacterDeathPolicyTests::FinishDeath(Pawn, Pawn);
		TestTrue(TEXT("死亡广播期间 Pawn 仍完整，尚未运行处理策略"), Pawn->bPawnIntactDuringBroadcast);
	}
	GGYGOCharacterDeathPolicyTests::FinishDeath(UnconfiguredPawn, DestroyPawn);
	AddExpectedError(TEXT("Handle Death Finished 未配置或未确认处理"), EAutomationExpectedErrorFlags::Contains, 1);
	World->GetTimerManager().Tick(0.016f);
	TestTrue(TEXT("权威端按策略隐藏并设置旧有销毁时限"), DestroyPawn->IsHidden() && DestroyPawn->GetLifeSpan() > 0.0f);
	TestTrue(TEXT("显式保留策略不隐藏、不安排销毁"), !KeepPawn->IsHidden() && KeepPawn->GetLifeSpan() == 0.0f);
	TestTrue(TEXT("客户端仅本地清理，不发起销毁"), ClientPawn->IsHidden() && ClientPawn->GetLifeSpan() == 0.0f);
	TestTrue(TEXT("未配置策略保留 Pawn，不偷换默认销毁"), !UnconfiguredPawn->IsHidden() && UnconfiguredPawn->GetLifeSpan() == 0.0f);
	for (auto* Pawn : {DestroyPawn, KeepPawn, ClientPawn})
	{
		TestEqual(TEXT("每个死亡完成只处理一次"), Pawn->PolicyCalls, 1);
		Pawn->GetHealthComponent()->FinishDeath();
	}
	TestEqual(TEXT("重复 FinishDeath 不重复处理"), DestroyPawn->PolicyCalls, 1);
	ClientPawn->Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOCharacterDeathPolicyProduction,
	"GGYGO.Character.DeathPolicy.ProductionBlueprint", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGGYGOCharacterDeathPolicyProduction::RunTest(const FString& Parameters)
{
	UClass* ProductionClass = LoadClass<AGGYGOCharacterBase>(nullptr,
		TEXT("/Game/BP/Character/Player/BP_PC_Pyrios.BP_PC_Pyrios_C"));
	if (!TestNotNull(TEXT("生产角色蓝图"), ProductionClass)) { return false; }
	UWorld* World = GGYGOCharacterDeathPolicyTests::CreateWorld();
	if (!TestNotNull(TEXT("生产蓝图死亡处理测试世界"), World)) { return false; }
	TStrongObjectPtr<UPackage> Package(World->GetPackage());
	ON_SCOPE_EXIT { World->DestroyWorld(false); Package->SetDirtyFlag(false); };
	auto* Pawn = World->SpawnActor<AGGYGOCharacterBase>(ProductionClass);
	auto* Observer = World->SpawnActor<AGGYGOCharacterDeathPolicyTestPawn>();
	if (!Pawn || !Observer) { AddError(TEXT("生产蓝图测试 Pawn 生成失败")); return false; }
	GGYGOCharacterDeathPolicyTests::FinishDeath(Pawn, Observer);
	TestTrue(TEXT("生产角色在同次死亡广播期间尚未清理"), Observer->bPawnIntactDuringBroadcast);
	World->GetTimerManager().Tick(0.016f);
	TestTrue(TEXT("生产蓝图下一帧选择等价清理"), Pawn->IsHidden() && Pawn->GetLifeSpan() > 0.0f);
	return true;
}
#endif
