/** @file GGYGOMontageFXPublicationTest.cpp @brief 派生关闭两序、资源归还与注册后继隔离。 */
#include "FX/GGYGOMontageFXPublication.h"

#include "AbilitySystemComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "NiagaraSystem.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
struct FPublicationWorld
{
	UWorld* World = nullptr;
	AActor* Avatar = nullptr;
	FGGYGOMontageFXState Snapshot;
	TStrongObjectPtr<UAbilitySystemComponent> Issuer;
	TStrongObjectPtr<USkeletalMeshComponent> Mesh;
	TStrongObjectPtr<UNiagaraSystem> System;
	TStrongObjectPtr<UGGYGOMontageFXPublicationOwner> Owner;

	explicit FPublicationWorld(FGGYGOMontageFXSendTerminal Send = {})
	{
		UWorld::InitializationValues Init;
		Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(false)
			.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
		World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
		if (!World) { return; }
		if (GEngine) { GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World); }
		Avatar = World->SpawnActor<AActor>();
		if (!Avatar) { return; }
		Issuer.Reset(NewObject<UAbilitySystemComponent>(Avatar));
		Issuer->RegisterComponentWithWorld(World);
		Mesh.Reset(NewObject<USkeletalMeshComponent>(Avatar));
		System.Reset(NewObject<UNiagaraSystem>());
		Owner.Reset(UGGYGOMontageFXPublicationOwner::RegisterHost(Issuer.Get(), Snapshot, MoveTemp(Send)));
	}
	~FPublicationWorld()
	{
		if (Owner.IsValid()) { Owner->Shutdown(); }
		if (World)
		{
			World->DestroyWorld(false);
			if (GEngine) { GEngine->DestroyWorldContext(World); }
			World->GetPackage()->SetDirtyFlag(false);
		}
	}
	FGGYGOMontageFXRecord Request() const
	{
		FGGYGOMontageFXRecord Result;
		Result.Avatar = Avatar;
		Result.Mesh = Mesh.Get();
		Result.System = System.Get();
		Result.Space = EGGYGONiagaraEffectSpace::World;
		Result.NormalStopMode = EGGYGONiagaraEffectStopMode::DeactivateDetachKeepWorld;
		Result.AbortStopMode = EGGYGONiagaraEffectStopMode::DestroyImmediately;
		Result.RemoteSeekDeltaSeconds = .01f;
		Result.RemoteMaxSimTimeSeconds = .002f;
		return Result;
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOMontageFXClosedKeyTest,
	"GGYGO.FX.MontagePublication.ClosedKeyOrders",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOMontageFXClosedKeyTest::RunTest(const FString&)
{
	using FOwner = UGGYGOMontageFXPublicationOwner;
	TestFalse(TEXT("Terminal 先到但 state 尚旧，不能回收"), FOwner::CanCollectClosedKey(3, 4, true, false));
	TestFalse(TEXT("仅 revision 已到不证明实际成员消失"), FOwner::CanCollectClosedKey(4, 4, true, false));
	TestFalse(TEXT("snapshot 先删 key，原资源仍在返栈不能回收"), FOwner::CanCollectClosedKey(4, 4, false, true));
	TestTrue(TEXT("三项实际满足才可回收"), FOwner::CanCollectClosedKey(4, 4, false, false));
	FGGYGOMontageFXState State;
	State.StateRevision = 9;
	State.ActiveRecords.AddDefaulted_GetRef().Occurrence = 7;
	TestNotNull(TEXT("未映射对象引用仍保留 key 成员"), State.Find(7));
	TestNull(TEXT("旧 End 不匹配后继 key"), State.Find(8));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOMontageFXLeaseTest,
	"GGYGO.FX.MontagePublication.ExactLeaseAndPureClose",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOMontageFXLeaseTest::RunTest(const FString&)
{
	TArray<FGGYGOMontageFXTerminal> Sent;
	FPublicationWorld Fixture(FGGYGOMontageFXSendTerminal::CreateLambda(
		[&Sent](const FGGYGOMontageFXTerminal& Terminal) { Sent.Add(Terminal); }));
	if (!TestTrue(TEXT("独立 World 与 owner 有效"), Fixture.World && Fixture.Owner.IsValid())) { return false; }
	const FGGYGOFXSourceIdentity Source = FGGYGOFXSourceIdentity::FromAbilityActivation(MakeShared<int32>(1));
	FGGYGOMontageFXLease First, Next;
	FString Error;
	TestTrue(TEXT("第一次实际发布"), Fixture.Owner->Begin(Source, Fixture.Request(), First, Error));
	TestTrue(TEXT("同来源后继有不同 occurrence"), Fixture.Owner->Begin(Source, Fixture.Request(), Next, Error));
	TestNotEqual(TEXT("occurrence 不复用"), First.GetOccurrence(), Next.GetOccurrence());
	FGGYGOMontageFXCloseObligation Close;
	TestTrue(TEXT("纯 Prepare 接受原 lease"), Fixture.Owner->PrepareClose(First,
		EGGYGOMontageFXCloseReason::Normal, Close) == EGGYGOMontageFXCloseResult::Closed);
	TestEqual(TEXT("Prepare 不发送"), Sent.Num(), 0);
	TestNull(TEXT("只移除原 active"), Fixture.Snapshot.Find(First.GetOccurrence()));
	TestNotNull(TEXT("后继保留"), Fixture.Snapshot.Find(Next.GetOccurrence()));
	FGGYGOMontageFXCloseObligation Moved = MoveTemp(Close);
	TestFalse(TEXT("move 后原义务不再可发"), Close.IsSet());
	TestTrue(TEXT("义务携原 issuer"), Moved.GetIssuer() == Fixture.Owner.Get());
	TestTrue(TEXT("安全出口显式发送"), Fixture.Owner->DispatchClose(MoveTemp(Moved)));
	TestEqual(TEXT("只发一次"), Sent.Num(), 1);
	if (Sent.Num()) { TestTrue(TEXT("正常退出原因保留"), Sent[0].Reason == EGGYGOMontageFXCloseReason::Normal); }
	FGGYGOMontageFXCloseObligation Again;
	TestTrue(TEXT("后续 Abort 不升级已 Normal"), Fixture.Owner->PrepareClose(First,
		EGGYGOMontageFXCloseReason::Abort, Again) == EGGYGOMontageFXCloseResult::AlreadyClosed);
	TestFalse(TEXT("没有第二个发送义务"), Again.IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOMontageFXResourceFactsTest,
	"GGYGO.FX.MontagePublication.LocalReleaseFacts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOMontageFXResourceFactsTest::RunTest(const FString&)
{
	FPublicationWorld Fixture;
	if (!Fixture.World || !Fixture.Owner.IsValid()) { AddError(TEXT("Resource fixture could not initialize.")); return false; }
	TStrongObjectPtr<UGGYGOAbilityTask_PlayNiagaraEffect> Task(NewObject<UGGYGOAbilityTask_PlayNiagaraEffect>());
	FString Error;
	auto Cap = Fixture.Owner->RegisterLocalTask(Task.Get(), Fixture.Avatar, Fixture.World, Error);
	TestTrue(TEXT("真实工厂能力登记可用"), Cap.IsValid());
	TestTrue(TEXT("登记 nonce 不等于已经生产"), Fixture.Owner->ReadLocalResourceFact(Fixture.Avatar)
		== EGGYGOMontageFXLocalResourceFact::NoProductionNow);
	FGGYGOMontageFXLocalStartTicket Ticket;
	TestTrue(TEXT("没有旧 remote 时可申请启动"), Fixture.Owner->RequestLocalStart(Cap, Ticket, Error)
		== EGGYGOMontageFXLocalStartResult::ReadyToActivate);
	TestTrue(TEXT("未消费许可仍是无生产事实"), Fixture.Owner->ReadLocalResourceFact(Fixture.Avatar)
		== EGGYGOMontageFXLocalResourceFact::NoProductionNow);
	Fixture.Owner->CancelLocalStart(Ticket);
	TestTrue(TEXT("取消未生成票不冒称最终 Released"), Fixture.Owner->ReadLocalResourceFact(Fixture.Avatar)
		== EGGYGOMontageFXLocalResourceFact::NoProductionNow);
	TestTrue(TEXT("取消票不能裸启动"), Fixture.Owner->ConsumeLocalStart(Ticket) == EGGYGOMontageFXPermitResult::Closed);
	Fixture.Owner->SealLocalProducer(Cap);
	Fixture.Owner->FlushResourceChanges();
	TestTrue(TEXT("封闭且实际无资源后才最终 Released"), Fixture.Owner->ReadLocalResourceFact(Fixture.Avatar)
		== EGGYGOMontageFXLocalResourceFact::Released);

	TStrongObjectPtr<UGGYGOAbilityTask_PlayNiagaraEffect> NextTask(NewObject<UGGYGOAbilityTask_PlayNiagaraEffect>());
	auto NextCap = Fixture.Owner->RegisterLocalTask(NextTask.Get(), Fixture.Avatar, Fixture.World, Error);
	FGGYGOMontageFXLocalStartTicket NextTicket;
	Fixture.Owner->RequestLocalStart(NextCap, NextTicket, Error);
	TestFalse(TEXT("后继 nonce 不同"), NextTicket == Ticket);
	TestTrue(TEXT("当前许可单次消费"), Fixture.Owner->ConsumeLocalStart(NextTicket) == EGGYGOMontageFXPermitResult::Granted);
	TestTrue(TEXT("旧 NoProduction 证不能跨消费使用"), Fixture.Owner->ReadLocalResourceFact(Fixture.Avatar)
		== EGGYGOMontageFXLocalResourceFact::Pending);
	Fixture.Owner->BeginLocalOperation(NextCap);
	Fixture.Owner->SetLocalOwnership(NextCap, true);
	Fixture.Owner->SealLocalProducer(NextCap);
	Fixture.Owner->SetLocalOwnership(NextCap, false);
	Fixture.Owner->FlushResourceChanges();
	TestTrue(TEXT("OnDestroy 已封口/摘空但 startup 栈未归还仍 Pending"), Fixture.Owner->ReadLocalResourceFact(Fixture.Avatar)
		== EGGYGOMontageFXLocalResourceFact::Pending);
	Fixture.Owner->CancelLocalStart(Ticket);
	TestTrue(TEXT("旧 nonce 不能释放后继在途资源"), Fixture.Owner->ReadLocalResourceFact(Fixture.Avatar)
		== EGGYGOMontageFXLocalResourceFact::Pending);
	Fixture.Owner->EndLocalOperation(NextCap);
	Fixture.Owner->FlushResourceChanges();
	TestTrue(TEXT("临时 native 栈归还后才能发布 Released"), Fixture.Owner->ReadLocalResourceFact(Fixture.Avatar)
		== EGGYGOMontageFXLocalResourceFact::Released);
	UGGYGOAbilityTask_PlayNiagaraEffect* LostTask = NewObject<UGGYGOAbilityTask_PlayNiagaraEffect>();
	auto LostCap = Fixture.Owner->RegisterLocalTask(LostTask, Fixture.Avatar, Fixture.World, Error);
	LostTask->MarkAsGarbage();
	TestTrue(TEXT("弱 Task 丢失且没有原 owner 封口，零资源字段也不是完整无生产证明"),
		Fixture.Owner->ReadLocalResourceFact(Fixture.Avatar) == EGGYGOMontageFXLocalResourceFact::Unknown);
	Fixture.Owner->SealLocalProducer(LostCap);
	Fixture.Owner->FlushResourceChanges();
	TestTrue(TEXT("必须得到原生产者的封口及在途结算才最终归还"),
		Fixture.Owner->ReadLocalResourceFact(Fixture.Avatar) == EGGYGOMontageFXLocalResourceFact::Released);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOMontageFXRegistrationTest,
	"GGYGO.FX.MontagePublication.RegistrationSuccessor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOMontageFXRegistrationTest::RunTest(const FString&)
{
	FPublicationWorld Fixture;
	if (!Fixture.World || !Fixture.Owner.IsValid()) { return false; }
	const FGGYGOFXSourceIdentity OldSource = FGGYGOFXSourceIdentity::FromAbilityActivation(MakeShared<int32>(1));
	const FGGYGOFXSourceIdentity NewSource = FGGYGOFXSourceIdentity::FromAbilityActivation(MakeShared<int32>(2));
	FGGYGOMontageFXLease Old, Current;
	FString Error;
	TestTrue(TEXT("原注册发布"), Fixture.Owner->Begin(OldSource, Fixture.Request(), Old, Error));
	const uint64 OldRegistration = Fixture.Owner->GetRegistrationSerial();
	auto* Registered = UGGYGOMontageFXPublicationOwner::RegisterHost(Fixture.Issuer.Get(), Fixture.Snapshot, {});
	TestTrue(TEXT("同 ASC 保唯一资源 view"), Registered == Fixture.Owner.Get());
	TestTrue(TEXT("重注册推进实际代次"), Registered->GetRegistrationSerial() > OldRegistration);
	TestTrue(TEXT("新注册发布"), Registered->Begin(NewSource, Fixture.Request(), Current, Error));
	TestTrue(TEXT("原 occurrence 不复用"), Current.GetOccurrence() > Old.GetOccurrence());
	Fixture.Owner->Shutdown(OldRegistration);
	TestNull(TEXT("旧清理只移除旧注册发布"), Fixture.Snapshot.Find(Old.GetOccurrence()));
	TestNotNull(TEXT("旧 Shutdown 不清新注册共享 state"), Fixture.Snapshot.Find(Current.GetOccurrence()));
	TArray<FGGYGOFXSourceIdentity> Sources;
	Fixture.Owner->GetSourceIdentities(Sources);
	TestEqual(TEXT("只返回仍发布的 source"), Sources.Num(), 1);
	if (Sources.Num()) { TestTrue(TEXT("新原 source 身份保留"), Sources[0] == NewSource); }
	return true;
}
#endif
