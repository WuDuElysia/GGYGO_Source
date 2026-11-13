/** @file GGYGOAvatarSwitchObserverTest.cpp @brief 客户端换人后服务器与另一客户端的绑定及实际视口验收。 */
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Camera/GGYGOPlayerCameraManager.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Character/Data/GGYGOPawnData.h"
#include "GameModes/GGYGOExperienceDefinition.h"
#include "GameModes/GGYGOGameMode.h"
#include "Player/GGYGOPlayerState.h"
#include "Teams/GGYGOCharacterSlot.h"
#include "Teams/GGYGOSquadComponent.h"

#include "Camera/CameraActor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Editor.h"
#include "Engine/ActorChannel.h"
#include "Engine/GameViewportClient.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/PackageMapClient.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameStateBase.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PlayInEditorDataTypes.h"
#include "SceneInterface.h"
#include "SceneView.h"
#include "SceneViewExtension.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "UnrealClient.h"
#include "UObject/StrongObjectPtr.h"

namespace GGYGOAvatarSwitchObserver
{
	FNetworkGUID NetGuid(const AActor* Actor)
	{
		const UNetDriver* Driver = Actor ? Actor->GetNetDriver() : nullptr;
		return Driver && Driver->GetNetGuidCache().IsValid() ? Driver->GetNetGuidCache()->GetNetGUID(Actor) : FNetworkGUID();
	}

	APawn* LivePawn(const AGGYGOCharacterSlot* Slot)
	{
		APawn* Pawn = Slot ? Slot->GetAvatarPawn() : nullptr;
		return IsValid(Pawn) && !Pawn->IsActorBeingDestroyed() ? Pawn : nullptr;
	}

	/** 只读真实 Draw 的活动角色与机位；不求值镜头，不手动更新姿态。 */
	class FSwitchView final : public FSceneViewExtensionBase
	{
	public:
		FSwitchView(const FAutoRegister& Register, UGameViewportClient* InViewport, APlayerController* InViewer,
			ACameraActor* InCamera, UGGYGOSquadComponent* InSquad)
			: FSceneViewExtensionBase(Register), Viewport(InViewport), Viewer(InViewer), Camera(InCamera), Squad(InSquad) {}
		virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override
		{ return IsInGameThread() && Viewport.IsValid() && Context.Viewport == Viewport->Viewport && Context.GetWorld() == Viewport->GetWorld(); }
		virtual void SetupView(FSceneViewFamily& Family, FSceneView& View) override
		{
			if (!Viewport.IsValid() || !Viewer.IsValid() || !Camera.IsValid() || !Squad.IsValid()
				|| Family.RenderTarget != Viewport->Viewport || !Family.Scene || Family.Scene->GetWorld() != Squad->GetWorld()
				|| Viewer->GetViewTarget() != Camera.Get() || !View.ViewLocation.Equals(Camera->GetActorLocation(), 1.0)
				|| !View.ViewRotation.Equals(Camera->GetActorRotation(), 0.5)) { return; }
			APawn* Pawn = LivePawn(Squad->GetActiveSlot());
			const ACharacter* Character = Cast<ACharacter>(Pawn);
			const auto* Mesh = Character ? Character->GetMesh() : nullptr;
			if (!Pawn || Pawn->IsHidden() || !Mesh || !Mesh->IsVisible()
				|| !View.ViewFrustum.IntersectBox(Mesh->Bounds.Origin, Mesh->Bounds.BoxExtent)
				|| Squad->GetWorld()->GetTimeSeconds() - Mesh->GetLastRenderTimeOnScreen() > 0.5f) { return; }
			Frame = GFrameCounter; ActivePawn = Pawn;
			Evidence = FString::Printf(TEXT("Frame=%llu Pawn=%s PawnNetGUID=%s Mesh=%s Hidden=0 MeshVisible=1 LastOnScreen=%.3f View=%s/%s"),
				static_cast<unsigned long long>(Frame), *Pawn->GetPathName(), *NetGuid(Pawn).ToString(), *Mesh->GetPathName(),
				Mesh->GetLastRenderTimeOnScreen(), *View.ViewLocation.ToString(), *View.ViewRotation.ToString());
		}
		TWeakObjectPtr<UGameViewportClient> Viewport;
		TWeakObjectPtr<APlayerController> Viewer;
		TWeakObjectPtr<ACameraActor> Camera;
		TWeakObjectPtr<UGGYGOSquadComponent> Squad;
		TWeakObjectPtr<APawn> ActivePawn;
		uint64 Frame = 0;
		FString Evidence;
	};

	class FSwitchCommand final : public IAutomationLatentCommand
	{
	public:
		explicit FSwitchCommand(FAutomationTestBase& InTest) : Test(InTest) {}
		virtual ~FSwitchCommand() override { Release(); EndOwnedPIE(); }
		virtual bool Update() override
		{
			if (Stage == EStage::Done) { return true; }
			if (Stage == EStage::Start) { Start(); return false; }
			if (Stage == EStage::Ending)
			{
				if (GEditor->IsPlaySessionInProgress() && FPlatformTime::Seconds() < Deadline) { return false; }
				Test.TestFalse(TEXT("自有三端 PIE 已退出"), GEditor->IsPlaySessionInProgress()); Stage = EStage::Done; return true;
			}
			if (bFailed || FPlatformTime::Seconds() > Deadline)
			{
				if (!bFailed) { Fail(TEXT("等待超时：") + Waiting); }
				Release(); EndOwnedPIE(); Advance(EStage::Ending, 15.0); return false;
			}
			if (Stage == EStage::Ready) { Ready(); return false; }
			if (!RetainedBindings()) { return false; }
			const int32 Index = Stage == EStage::After ? 1 : 0;
			bConverged = Matches(Index);
			if (!bConverged || !CapturedByViews()) { return false; }
			LogState(Index);
			if (Stage == EStage::Returned)
			{
				Test.AddInfo(TEXT("[Avatar.Observer] 客户端 0→1→0 经真实 RPC／复制完成；两观察视口实拍完成，原持久绑定未替换。"));
				Release(); EndOwnedPIE(); Advance(EStage::Ending, 15.0); return false;
			}
			const int32 NextIndex = Stage == EStage::Before ? 1 : 0;
			// 仅调用所属客户端的生产 RPC；不从测试直接调用服务器切换或客户端表现写入。
			Sides[1].Squad->ServerRequestSwitchToSlot(NextIndex);
			Test.AddInfo(FString::Printf(TEXT("[Avatar.Observer.Request] ClientPlayerId=%d Slot=%d"), PlayerId, NextIndex));
			Advance(NextIndex == 1 ? EStage::After : EStage::Returned);
			return false;
		}

	private:
		enum class EStage : uint8 { Start, Ready, Before, After, Returned, Ending, Done };
		struct FMember
		{
			TWeakObjectPtr<AGGYGOCharacterSlot> Slot;
			TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC;
			TWeakObjectPtr<APawn> Pawn;
			FGGYGOPawnASCResourceHandle Resource;
		};
		struct FSide
		{
			FString Name;
			TWeakObjectPtr<UWorld> World;
			TWeakObjectPtr<APlayerController> Viewer;
			TWeakObjectPtr<AGGYGOPlayerState> PlayerState;
			TWeakObjectPtr<UGGYGOSquadComponent> Squad;
			FMember Members[2];
			TSharedPtr<FSwitchView, ESPMode::ThreadSafe> View;
			TSet<uint8> Captures;
		};
		void Advance(EStage Next, double Timeout = 25.0)
		{ Stage = Next; Entered = FPlatformTime::Seconds(); Deadline = Entered + Timeout; bConverged = false; Waiting.Reset(); }
		bool Require(bool Condition, const TCHAR* Reason)
		{ if (!Condition) { Waiting = Reason; } return Condition; }
		void Fail(const FString& Reason)
		{ if (!bFailed) { Test.AddError(TEXT("[Avatar.Observer] ") + Reason); } bFailed = true; }
		bool OwnsPIE() const
		{
			const auto Info = GEditor ? GEditor->GetPlayInEditorSessionInfo() : TOptional<FPlayInEditorSessionInfo>();
			return Info.IsSet() && Settings.IsValid() && Info->OriginalRequestParams.EditorPlaySettings == Settings.Get();
		}
		void EndOwnedPIE()
		{
			if (!GEditor) { return; }
			if (OwnsPIE()) { GEditor->EndPlayMap(); }
			else if (const auto Request = GEditor->GetPlaySessionRequest(); Request.IsSet() && Settings.IsValid()
				&& Request->EditorPlaySettings == Settings.Get()) { GEditor->CancelRequestPlaySession(); }
		}
		void Release()
		{
			FGameModeEvents::GameModePostLoginEvent.Remove(PostLogin);
			UGameViewportClient::OnViewportRendered().Remove(Rendered);
			for (auto& Side : Sides) { Side.View.Reset(); }
		}
		void Start()
		{
			Advance(EStage::Ready, 120.0);
			if (!GEditor || GEditor->IsPlaySessionInProgress() || GEditor->GetPlaySessionRequest().IsSet())
			{ Fail(TEXT("需要空闲 Editor，不接管现有 PIE。")); return; }
			if (!FApp::CanEverRender() || !FParse::Value(FCommandLine::Get(), TEXT("GGYGOAvatarCaptureDir="), Output))
			{ Fail(TEXT("需要真实 RHI 与 GGYGOAvatarCaptureDir。")); return; }
			IFileManager::Get().MakeDirectory(*Output, true);
			PostLogin = FGameModeEvents::GameModePostLoginEvent.AddLambda([this](AGameModeBase* Mode, APlayerController* PC)
			{
				if (!Mode || Mode->GetWorld()->WorldType != EWorldType::PIE) { return; }
				const auto* ProjectMode = Cast<AGGYGOGameMode>(Mode);
				const auto* Experience = ProjectMode ? ProjectMode->GetExperience() : nullptr;
				const auto* PS = PC ? PC->GetPlayerState<AGGYGOPlayerState>() : nullptr;
				auto* Squad = PS ? PS->GetSquadComponent() : nullptr;
				UGGYGOPawnData* Member = Experience && !Experience->SquadMembers.IsEmpty()
					? const_cast<UGGYGOPawnData*>(Experience->SquadMembers[0].Get()) : nullptr;
				if (!Member || !Squad || !Squad->SetRoster({Member, Member})) { Fail(TEXT("生产 Experience 无法建立临时两成员编队。")); }
			});
			Settings.Reset(NewObject<ULevelEditorPlaySettings>());
			Settings->SetPlayNetMode(EPlayNetMode::PIE_ListenServer); Settings->SetPlayNumberOfClients(3);
			Settings->SetRunUnderOneProcess(true); Settings->bLaunchSeparateServer = false;
			Settings->GameGetsMouseControl = false; Settings->bShouldMinimizeEditorOnNonVRPIE = false;
			FRequestPlaySessionParams Params; Params.EditorPlaySettings = Settings.Get(); Params.bAllowOnlineSubsystem = false;
			GEditor->RequestPlaySession(Params);
			const auto Request = GEditor->GetPlaySessionRequest();
			if (!Request.IsSet() || !Request->EditorPlaySettings) { Fail(TEXT("原生 PIE 请求未建立。")); return; }
			Settings.Reset(Request->EditorPlaySettings.Get());
		}
		bool ResolveSides()
		{
			UWorld* Server = nullptr; TArray<UWorld*> Clients;
			for (const auto& Context : GEditor->GetWorldContexts())
			{
				UWorld* World = Context.WorldType == EWorldType::PIE ? Context.World() : nullptr;
				if (!World || !World->HasBegunPlay()) { continue; }
				if (World->GetNetMode() == NM_ListenServer) { Server = World; }
				else if (World->GetNetMode() == NM_Client) { Clients.Add(World); }
			}
			if (!Require(Server && Clients.Num() == 2, TEXT("监听服务器与两个独立客户端世界"))) { return false; }
			int32 TargetId = MAX_int32;
			for (auto It = Server->GetPlayerControllerIterator(); It; ++It)
			{
				const auto* PC = It->Get(); const auto* PS = PC ? PC->GetPlayerState<AGGYGOPlayerState>() : nullptr;
				if (PS && !PC->IsLocalController()) { TargetId = FMath::Min(TargetId, PS->GetPlayerId()); }
			}
			if (!Require(TargetId != MAX_int32, TEXT("远端换人玩家"))) { return false; }
			PlayerId = TargetId;
			UWorld* Owning = nullptr; UWorld* Observing = nullptr;
			for (UWorld* World : Clients)
			{
				const auto* PC = World->GetFirstPlayerController(); const auto* PS = PC ? PC->GetPlayerState<AGGYGOPlayerState>() : nullptr;
				if (!Require(PS && PC->IsLocalController(), TEXT("两个客户端的本地 Controller／PlayerState"))) { return false; }
				if (PS->GetPlayerId() == PlayerId) { Owning = World; } else { Observing = World; }
			}
			if (!Require(Owning && Observing, TEXT("所属客户端和不同玩家的观察客户端"))) { return false; }
			UWorld* Worlds[] = {Server, Owning, Observing}; const TCHAR* Names[] = {TEXT("server"), TEXT("owner"), TEXT("observer")};
			Sides.Reset();
			for (int32 SideIndex = 0; SideIndex < 3; ++SideIndex)
			{
				UWorld* World = Worlds[SideIndex]; const auto* GS = World->GetGameState(); AGGYGOPlayerState* PS = nullptr;
				if (GS) { for (const auto& Candidate : GS->PlayerArray)
					{ if (Candidate && Candidate->GetPlayerId() == PlayerId) { PS = Cast<AGGYGOPlayerState>(Candidate.Get()); break; } } }
				auto* Squad = PS ? PS->GetSquadComponent() : nullptr;
				if (!Require(Squad && Squad->GetSlotCount() == 2, TEXT("同一玩家的复制编队与两 Slot"))) { return false; }
				FSide& Side = Sides.AddDefaulted_GetRef(); Side.Name = Names[SideIndex]; Side.World = World;
				Side.PlayerState = PS; Side.Squad = Squad; Side.Viewer = World->GetFirstPlayerController();
				for (int32 Index = 0; Index < 2; ++Index)
				{
					auto* Slot = Squad->GetSlot(Index); auto* ASC = Slot ? Slot->GetGGYGOAbilitySystemComponent() : nullptr;
					APawn* Pawn = LivePawn(Slot); auto* Extension = Pawn ? Pawn->FindComponentByClass<UGGYGOPawnExtensionComponent>() : nullptr;
					if (!Require(Slot && ASC && (SideIndex == 2 || (Extension && ASC->GetOwnerActor() == Slot
						&& ASC->GetAvatarActor() == Pawn && Extension->IsLocalAbilitySystemResourceReady(Extension->GetCurrentLocalAbilitySystemResource()))),
						TEXT("持久 Slot／ASC 与服务器、所属端的原 Pawn 绑定就绪"))) { return false; }
					Side.Members[Index] = {Slot, ASC, Pawn, Extension ? Extension->GetCurrentLocalAbilitySystemResource() : FGGYGOPawnASCResourceHandle()};
				}
			}
			return Matches(0);
		}
		void Ready()
		{
			if (!ResolveSides()) { ReadySince.Reset(); return; }
			if (!bSeparated)
			{
				const FVector Focus = LivePawn(Sides[0].Squad->GetSlot(0))->GetActorLocation(); int32 OtherIndex = 0;
				for (auto It = Sides[0].World->GetPlayerControllerIterator(); It; ++It)
				{
					auto* PC = It->Get(); const auto* PS = PC ? PC->GetPlayerState<AGGYGOPlayerState>() : nullptr;
					if (!PS || PS->GetPlayerId() == PlayerId) { continue; }
					APawn* Pawn = PC->GetPawn();
					if (!Pawn || !Pawn->TeleportTo(Focus + FVector(-1000, 1200 + 500 * OtherIndex++, 0), Pawn->GetActorRotation()))
					{ Fail(TEXT("无法移开其它测试玩家以免遮挡。")); return; }
					Pawn->ForceNetUpdate();
				}
				bSeparated = true;
			}
			for (const auto& Side : Sides)
			{
				const auto* Pawn = Cast<ACharacter>(LivePawn(Side.Squad->GetSlot(0)));
				const auto* Movement = Pawn ? Pawn->GetCharacterMovement() : nullptr;
				const auto* Manager = Side.Viewer.IsValid() ? Cast<AGGYGOPlayerCameraManager>(Side.Viewer->PlayerCameraManager) : nullptr;
				if (!Require(Movement && Movement->IsMovingOnGround() && Pawn->GetVelocity().IsNearlyZero(1.0)
					&& Manager && Manager->GetCameraPublicationState() == EGGYGOCameraPublicationState::Running,
					TEXT("角色落地静止与本地镜头准入"))) { ReadySince.Reset(); return; }
			}
			if (!ReadySince.IsSet()) { ReadySince = FPlatformTime::Seconds(); }
			if (FPlatformTime::Seconds() - *ReadySince < 1.0) { return; }
			for (const int32 SideIndex : {0, 2})
			{
				FSide& Side = Sides[SideIndex]; auto* Viewport = Side.World->GetGameViewport();
				if (!Viewport || !Viewport->Viewport) { Fail(TEXT("缺少实际观察视口。")); return; }
				auto* Camera = Side.World->SpawnActor<ACameraActor>();
				if (!Camera) { Fail(TEXT("诊断机位创建失败。")); return; }
				const FVector Focus = LivePawn(Side.Squad->GetSlot(0))->GetActorLocation() + FVector(0, 0, -20);
				const FVector Location = Focus + FVector(370, 270, 200);
				Camera->SetActorLocation(Location); Camera->SetActorRotation((Focus - Location).Rotation()); Side.Viewer->SetViewTarget(Camera);
				Side.View = FSceneViewExtensions::NewExtension<FSwitchView>(Viewport, Side.Viewer.Get(), Camera, Side.Squad.Get());
			}
			Rendered = UGameViewportClient::OnViewportRendered().AddRaw(this, &FSwitchCommand::Capture);
			Advance(EStage::Before);
		}
		bool RetainedBindings()
		{
			for (int32 SideIndex = 0; SideIndex < Sides.Num(); ++SideIndex)
			{
				const FSide& Side = Sides[SideIndex];
				for (int32 Index = 0; Index < 2; ++Index)
				{
					const auto& Original = Side.Members[Index];
					if (!Side.Squad.IsValid() || !Original.Slot.IsValid() || !Original.ASC.IsValid()
						|| Side.Squad->GetSlot(Index) != Original.Slot.Get() || Original.Slot->GetGGYGOAbilitySystemComponent() != Original.ASC.Get())
					{ Fail(Side.Name + TEXT(" 换人替换了原 Slot／ASC。")); return false; }
					// 观察端的隐藏 Pawn 可因原生相关性消失；它的本地绑定不冒充服务器的永久 Avatar。
					if (SideIndex == 2) { continue; }
					APawn* Pawn = LivePawn(Original.Slot.Get()); auto* Extension = Pawn ? Pawn->FindComponentByClass<UGGYGOPawnExtensionComponent>() : nullptr;
					if (!Original.Pawn.IsValid() || Pawn != Original.Pawn.Get() || Original.ASC->GetOwnerActor() != Original.Slot.Get()
						|| Original.ASC->GetAvatarActor() != Pawn || !Extension || !Extension->IsLocalAbilitySystemResourceReady(Original.Resource))
					{ Fail(Side.Name + TEXT(" 原 Pawn／ASC 绑定资源被换人释放或替换。")); return false; }
				}
			}
			return true;
		}
		bool Matches(int32 Index)
		{
			if (Sides.Num() != 3) { return false; }
			APawn* ServerPawn = LivePawn(Sides[0].Squad->GetSlot(Index)); const auto Guid = NetGuid(ServerPawn);
			for (int32 SideIndex = 0; SideIndex < 3; ++SideIndex)
			{
				const auto& Side = Sides[SideIndex]; auto* Slot = Side.Squad->GetSlot(Index);
				APawn* Pawn = LivePawn(Slot); APawn* Other = LivePawn(Side.Squad->GetSlot(1 - Index));
				auto* ASC = Slot ? Slot->GetGGYGOAbilitySystemComponent() : nullptr;
				auto* Extension = Pawn ? Pawn->FindComponentByClass<UGGYGOPawnExtensionComponent>() : nullptr;
				if (!Require(Side.Squad->GetActiveSlot() == Slot && Pawn && Guid.IsValid() && NetGuid(Pawn) == Guid
					&& !Pawn->IsHidden() && Pawn->GetActorEnableCollision() && (!Other || (Other->IsHidden() && !Other->GetActorEnableCollision()))
					&& ASC && ASC->GetOwnerActor() == Slot && ASC->GetAvatarActor() == Pawn && Extension
					&& Extension->IsLocalAbilitySystemResourceReady(Extension->GetCurrentLocalAbilitySystemResource()),
					TEXT("三端活动 Pawn 配对、可见性与当前 ASC 绑定收敛"))) { return false; }
				for (int32 Member = 0; Member < 2; ++Member)
				{
					if (!Require(NetGuid(Side.Squad->GetSlot(Member)).IsValid()
						&& NetGuid(Side.Squad->GetSlot(Member)) == NetGuid(Sides[0].Squad->GetSlot(Member)), TEXT("三端 Slot NetGUID 对应"))) { return false; }
				}
				if (SideIndex == 0 || SideIndex == 1)
				{
					const auto* PC = Cast<APlayerController>(Side.PlayerState->GetOwner());
					if (!Require(PC && PC->GetPawn() == Pawn && Pawn->GetController() == PC
						&& (SideIndex == 0 || PC->GetViewTarget() == Pawn), TEXT("服务器和所属客户端控制／所属端镜头切到原目标 Pawn"))) { return false; }
				}
				else if (!Require(Pawn->GetLocalRole() == ROLE_SimulatedProxy && Side.Viewer->GetPlayerState<AGGYGOPlayerState>()->GetPlayerId() != PlayerId,
					TEXT("第三端确为另一玩家的 SimulatedProxy 观察"))) { return false; }
				if (SideIndex > 0)
				{
					const UNetDriver* Driver = Side.World->GetNetDriver(); auto* Connection = Driver ? Driver->ServerConnection.Get() : nullptr;
					if (!Require(Connection && Connection->GetConnectionState() == USOCK_Open && Connection->FindActorChannelRef(Pawn),
						TEXT("客户端真实连接和活动 Pawn ActorChannel"))) { return false; }
				}
			}
			return true;
		}
		void LogState(int32 Index)
		{
			for (const auto& Side : Sides)
			{
				auto* Slot = Side.Squad->GetSlot(Index); APawn* Pawn = LivePawn(Slot); APawn* Other = LivePawn(Side.Squad->GetSlot(1 - Index));
				Test.AddInfo(FString::Printf(TEXT("[Avatar.Observer.State] Phase=%u Side=%s PlayerId=%d ActiveSlot=%d SlotNetGUID=%s PawnNetGUID=%s Pawn=%s ASC=%s ASCActorInfo=Matched OtherPawn=%s OtherHidden=%d OtherCollision=%d ViewerPlayerId=%d NetDriver=%s"),
					uint8(Stage), *Side.Name, PlayerId, Index, *NetGuid(Slot).ToString(), *NetGuid(Pawn).ToString(), *GetPathNameSafe(Pawn),
					*GetPathNameSafe(Slot->GetGGYGOAbilitySystemComponent()), *GetPathNameSafe(Other), Other ? Other->IsHidden() : true,
					Other && Other->GetActorEnableCollision(), Side.Viewer->PlayerState->GetPlayerId(), *GetPathNameSafe(Side.World->GetNetDriver())));
			}
		}
		bool CapturedByViews() const
		{ return Sides[0].Captures.Contains(uint8(Stage)) && Sides[2].Captures.Contains(uint8(Stage)); }
		void Capture(FViewport* Viewport)
		{
			if (!bConverged || FPlatformTime::Seconds() - Entered < 1.0) { return; }
			for (auto& Side : Sides)
			{
				const auto View = Side.View;
				if (!View.IsValid() || !View->Viewport.IsValid() || View->Viewport->Viewport != Viewport || View->Frame != GFrameCounter
					|| View->ActivePawn.Get() != LivePawn(Side.Squad->GetActiveSlot()) || Side.Captures.Contains(uint8(Stage))) { continue; }
				TArray<FColor> Pixels; const FIntPoint Size = Viewport->GetSizeXY();
				if (Size.X <= 0 || Size.Y <= 0 || !Viewport->ReadPixels(Pixels) || Pixels.Num() != Size.X * Size.Y)
				{ Fail(TEXT("实际视口像素读取失败。")); return; }
				for (FColor& Pixel : Pixels) { Pixel.A = 255; }
				TArray64<uint8> PNG; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, PNG);
				const TCHAR* Label = Stage == EStage::Before ? TEXT("before_slot0") : Stage == EStage::After ? TEXT("after_slot1") : TEXT("returned_slot0");
				const FString File = Output / (Side.Name + TEXT("_") + Label + TEXT(".png"));
				if (!FFileHelper::SaveArrayToFile(PNG, *File)) { Fail(TEXT("截图写入失败：") + File); return; }
				Side.Captures.Add(uint8(Stage));
				Test.AddInfo(FString::Printf(TEXT("[Avatar.Observer.Draw] Side=%s Phase=%s File=%s %s"), *Side.Name, Label, *File, *View->Evidence));
			}
		}
		FAutomationTestBase& Test;
		EStage Stage = EStage::Start;
		bool bFailed = false, bSeparated = false, bConverged = false;
		int32 PlayerId = INDEX_NONE;
		double Entered = 0, Deadline = 0;
		TOptional<double> ReadySince;
		FString Output, Waiting;
		TArray<FSide> Sides;
		TStrongObjectPtr<ULevelEditorPlaySettings> Settings;
		FDelegateHandle PostLogin, Rendered;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOAvatarSwitchObserverTest, "GGYGO.Combatants.AvatarSwitch.ThirdClientObserver",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGGYGOAvatarSwitchObserverTest::RunTest(const FString& Parameters)
{ ADD_LATENT_AUTOMATION_COMMAND(GGYGOAvatarSwitchObserver::FSwitchCommand(*this)); return true; }
#endif
