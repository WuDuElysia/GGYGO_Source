/** @file GGYGOHealthAcceptanceTest.cpp @brief 生产 PIE 的生命、破韧与原死者双端验收。 */
#include "AbilitySystem/Tests/GGYGOHealthAcceptanceTestTypes.h"

#include "AbilitySystem/Attributes/GGYGOHealthSet.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Character/Components/GGYGOHealthComponent.h"
#include "System/GGYGOGameplayTags.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOHealthAcceptanceTestTypes)

void UGGYGOHealthAcceptanceObserver::Observe(UGGYGOHealthComponent* InHealth,
	UGGYGOAbilitySystemComponent* InASC, const FString& InSide)
{
	Release();
	Health = InHealth;
	ASC = InASC;
	Side = InSide;
	Events.Reset();
	InHealth->OnHealthChanged.AddDynamic(this, &ThisClass::HealthChanged);
	InHealth->OnPoiseChanged.AddDynamic(this, &ThisClass::PoiseChanged);
	InHealth->OnPoiseBroken.AddDynamic(this, &ThisClass::PoiseBroken);
	InHealth->OnDeathStarted.AddDynamic(this, &ThisClass::DeathStarted);
	InHealth->OnDeathFinished.AddDynamic(this, &ThisClass::DeathFinished);
	Set = InASC->GetSet<UGGYGOHealthSet>();
	if (Set.IsValid())
	{
		OutOfHealthHandle = Set->OnOutOfHealth.AddWeakLambda(this,
			[this](AActor*, AActor*, const FGameplayEffectSpec*, float, float OldValue, float NewValue)
			{ Record(TEXT("OutOfHealth"), OldValue, NewValue, Health.IsValid() ? Health->GetOwner() : nullptr); });
	}
}

void UGGYGOHealthAcceptanceObserver::Release()
{
	if (Health.IsValid())
	{
		Health->OnHealthChanged.RemoveDynamic(this, &ThisClass::HealthChanged);
		Health->OnPoiseChanged.RemoveDynamic(this, &ThisClass::PoiseChanged);
		Health->OnPoiseBroken.RemoveDynamic(this, &ThisClass::PoiseBroken);
		Health->OnDeathStarted.RemoveDynamic(this, &ThisClass::DeathStarted);
		Health->OnDeathFinished.RemoveDynamic(this, &ThisClass::DeathFinished);
	}
	if (Set.IsValid()) { Set->OnOutOfHealth.Remove(OutOfHealthHandle); }
	OutOfHealthHandle.Reset();
	Health.Reset(); ASC.Reset(); Set.Reset();
}

int32 UGGYGOHealthAcceptanceObserver::Count(const TCHAR* Kind) const
{
	return Events.FilterByPredicate([Kind](const auto& Event) { return Event.Kind == Kind; }).Num();
}

const FGGYGOHealthAcceptanceEvent* UGGYGOHealthAcceptanceObserver::Find(const TCHAR* Kind) const
{
	return Events.FindByPredicate([Kind](const auto& Event) { return Event.Kind == Kind; });
}

void UGGYGOHealthAcceptanceObserver::Record(const TCHAR* Kind, float OldValue, float NewValue, AActor* Actor)
{
	FGGYGOHealthAcceptanceEvent& Event = Events.AddDefaulted_GetRef();
	Event.Kind = Kind; Event.OldValue = OldValue; Event.NewValue = NewValue;
	Event.ASCHealth = ASC.IsValid() ? ASC->GetNumericAttribute(UGGYGOHealthSet::GetHealthAttribute()) : -1.0f;
	Event.ComponentHealth = Health.IsValid() ? Health->GetHealth() : -1.0f;
	Event.DeathState = Health.IsValid() ? uint8(Health->GetDeathState()) : 255;
	Event.bOriginalActor = Actor && Health.IsValid() && Actor == Health->GetOwner();
	UE_LOG(LogTemp, Display, TEXT("[Health.Acceptance.Event] Side=%s OriginalPawn=%s Kind=%s Old=%.3f New=%.3f ASCHealth=%.3f ComponentHealth=%.3f DeathState=%u Original=%d Frame=%llu"),
		*Side, *GetPathNameSafe(Actor), Kind, OldValue, NewValue, Event.ASCHealth, Event.ComponentHealth,
		Event.DeathState, Event.bOriginalActor, static_cast<unsigned long long>(GFrameCounter));
}

void UGGYGOHealthAcceptanceObserver::HealthChanged(UGGYGOHealthComponent* Sender, float OldValue, float NewValue, AActor*)
{ Record(TEXT("HealthChanged"), OldValue, NewValue, Sender ? Sender->GetOwner() : nullptr); }
void UGGYGOHealthAcceptanceObserver::PoiseChanged(UGGYGOHealthComponent* Sender, float OldValue, float NewValue, AActor*)
{ Record(TEXT("PoiseChanged"), OldValue, NewValue, Sender ? Sender->GetOwner() : nullptr); }
void UGGYGOHealthAcceptanceObserver::PoiseBroken(AActor* Actor) { Record(TEXT("PoiseBroken"), 0, 0, Actor); }
void UGGYGOHealthAcceptanceObserver::DeathStarted(AActor* Actor) { Record(TEXT("DeathStarted"), 0, 0, Actor); }
void UGGYGOHealthAcceptanceObserver::DeathFinished(AActor* Actor) { Record(TEXT("DeathFinished"), 0, 0, Actor); }

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Camera/CameraActor.h"
#include "Camera/GGYGOPlayerCameraManager.h"
#include "Character/Data/GGYGOPawnData.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/GameViewportClient.h"
#include "Engine/NetDriver.h"
#include "Engine/PackageMapClient.h"
#include "Editor.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameModes/GGYGOExperienceDefinition.h"
#include "GameModes/GGYGOGameMode.h"
#include "HAL/FileManager.h"
#include "Player/GGYGOPlayerState.h"
#include "System/GGYGOGameData.h"
#include "Teams/GGYGOCharacterSlot.h"
#include "Teams/GGYGOSquadComponent.h"
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

namespace GGYGOHealthAcceptance
{
	/** 只记录自然 Draw 的原角色状态；不重算相机、不手动驱动动画或渲染。 */
	class FHealthView final : public FSceneViewExtensionBase
	{
	public:
		FHealthView(const FAutoRegister& AutoRegister, UGameViewportClient* InViewport,
			UGGYGOHealthComponent* InHealth, APlayerController* InViewer, ACameraActor* InCamera)
			: FSceneViewExtensionBase(AutoRegister), Viewport(InViewport), Health(InHealth), Viewer(InViewer), Camera(InCamera) {}
		virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override
		{ return IsInGameThread() && Viewport.IsValid() && Context.Viewport == Viewport->Viewport && Context.GetWorld() == Viewport->GetWorld(); }
		virtual void SetupView(FSceneViewFamily& Family, FSceneView& View) override
		{
			if (!Viewport.IsValid() || !Health.IsValid() || Family.RenderTarget != Viewport->Viewport
				|| !Family.Scene || Family.Scene->GetWorld() != Health->GetWorld()) { return; }
			if (!Viewer.IsValid() || !Camera.IsValid() || Viewer->GetViewTarget() != Camera.Get()
				|| !View.ViewLocation.Equals(Camera->GetActorLocation(), 1.0)
				|| !View.ViewRotation.Equals(Camera->GetActorRotation(), 0.5)) { return; }
			const ACharacter* Character = Cast<ACharacter>(Health->GetOwner());
			const USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
			const UAnimInstance* Anim = Mesh ? Mesh->GetAnimInstance() : nullptr;
			const UAnimMontage* Montage = Anim ? Anim->GetCurrentActiveMontage() : nullptr;
			Frame = GFrameCounter;
			DeathState = uint8(Health->GetDeathState());
			Position = Montage ? Anim->Montage_GetPosition(Montage) : -1.0f;
			const float Length = Montage ? Montage->GetPlayLength() : 0.0f;
			Progress = Length > 0.0f ? Position / Length : -1.0f;
			Evidence = FString::Printf(TEXT("Frame=%llu Pawn=%s Health=%.3f Poise=%.3f DeathState=%u Hidden=%d MeshVisible=%d Montage=%s Position=%.3f Length=%.3f Progress=%.3f DiagnosticCamera=1 View=%s/%s"),
				static_cast<unsigned long long>(Frame), *GetPathNameSafe(Character), Health->GetHealth(), Health->GetPoise(), DeathState,
				Character ? Character->IsHidden() : true, Mesh && Mesh->IsVisible(), *GetPathNameSafe(Montage), Position,
				Length, Progress,
				*View.ViewLocation.ToString(), *View.ViewRotation.ToString());
		}
		TWeakObjectPtr<UGameViewportClient> Viewport;
		TWeakObjectPtr<UGGYGOHealthComponent> Health;
		TWeakObjectPtr<APlayerController> Viewer;
		TWeakObjectPtr<ACameraActor> Camera;
		uint64 Frame = 0;
		uint8 DeathState = 255;
		float Position = -1;
		float Progress = -1;
		FString Evidence;
	};

	class FHealthCommand final : public IAutomationLatentCommand
	{
	public:
		FHealthCommand(FAutomationTestBase& InTest, bool bInNetwork) : Test(InTest), bNetwork(bInNetwork) {}
		virtual ~FHealthCommand() override { ReleaseObservers(); EndOwnedPIE(); }
		virtual bool Update() override
		{
			if (Stage == EStage::Done) { return true; }
			if (Stage == EStage::Start) { Start(); return false; }
			if (Stage == EStage::Ending)
			{
				if (GEditor->IsPlaySessionInProgress() && FPlatformTime::Seconds() < Deadline) { return false; }
				Test.TestFalse(TEXT("释放自有 PIE"), GEditor->IsPlaySessionInProgress()); Stage = EStage::Done; return true;
			}
			if (bFailed || FPlatformTime::Seconds() > Deadline)
			{
				if (!bFailed) { Fail(FString::Printf(TEXT("阶段 %u 等待超时：%s"), uint8(Stage), *Waiting)); }
				End(); return false;
			}
			if (Stage == EStage::Ready) { Ready(); return false; }
			if (Stage == EStage::Baseline && CapturedByAll(TEXT("baseline")))
			{ Apply(10.0f, 0.0f); Advance(EStage::Damage); }
			else if (Stage == EStage::Damage && Values(InitialHealth - 10.0f, InitialPoise)
				&& Notices(TEXT("HealthChanged"), 1) && CapturedByAll(TEXT("damage")))
			{ Apply(0.0f, InitialPoise); Advance(EStage::Poise); }
			else if (Stage == EStage::Poise && Values(InitialHealth - 10.0f, 0.0f)
				&& Notices(TEXT("PoiseBroken"), 1) && CapturedByAll(TEXT("poise")))
			{ Apply(InitialHealth + 10.0f, 0.0f); Advance(EStage::Death); }
			else if (Stage == EStage::Death) { Death(); }
			return false;
		}

	private:
		enum class EStage : uint8 { Start, Ready, Baseline, Damage, Poise, Death, Ending, Done };
		struct FSide
		{
			FString Name;
			TWeakObjectPtr<APlayerController> Controller;
			TWeakObjectPtr<UGGYGOSquadComponent> Squad;
			TWeakObjectPtr<APawn> Pawn;
			TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC;
			TWeakObjectPtr<UGGYGOHealthComponent> Health;
			TStrongObjectPtr<UGGYGOHealthAcceptanceObserver> Observer;
			TSharedPtr<FHealthView, ESPMode::ThreadSafe> View;
			TSet<FString> Captures;
		};
		void Advance(EStage Next, double Timeout = 20.0)
		{ Stage = Next; Entered = FPlatformTime::Seconds(); Deadline = Entered + Timeout; Waiting.Reset(); }
		void Fail(const FString& Reason)
		{ if (!bFailed) { Test.AddError(TEXT("[Health.Acceptance] ") + Reason); } bFailed = true; }
		bool Require(bool Condition, const TCHAR* Reason)
		{ if (!Condition) { Waiting = Reason; } return Condition; }
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
		void ReleaseObservers()
		{
			FGameModeEvents::GameModePostLoginEvent.Remove(PostLogin);
			UGameViewportClient::OnViewportRendered().Remove(Rendered);
			for (FSide& Side : Sides) { if (Side.Observer.IsValid()) { Side.Observer->Release(); } Side.View.Reset(); }
		}
		void End()
		{ ReleaseObservers(); EndOwnedPIE(); Advance(EStage::Ending, 15.0); }
		void Start()
		{
			Advance(EStage::Ready, 120.0);
			if (!GEditor || GEditor->IsPlaySessionInProgress() || GEditor->GetPlaySessionRequest().IsSet())
			{ Fail(TEXT("需要空闲 Editor；不接管已有 PIE。")); return; }
			if (!FApp::CanEverRender() || !FParse::Value(FCommandLine::Get(), TEXT("GGYGOHealthCaptureDir="), Output))
			{ Fail(TEXT("本验收要求真实 RHI 与 GGYGOHealthCaptureDir，不以 NullRHI 代替画面。")); return; }
			Output /= bNetwork ? TEXT("network") : TEXT("standalone");
			IFileManager::Get().MakeDirectory(*Output, true);
			PostLogin = FGameModeEvents::GameModePostLoginEvent.AddLambda([this](AGameModeBase* Mode, APlayerController* PC)
			{
				if (!Mode || Mode->GetWorld()->WorldType != EWorldType::PIE) { return; }
				const AGGYGOGameMode* ProjectMode = Cast<AGGYGOGameMode>(Mode);
				const auto* Experience = ProjectMode ? ProjectMode->GetExperience() : nullptr;
				const auto* PS = PC ? PC->GetPlayerState<AGGYGOPlayerState>() : nullptr;
				auto* Squad = PS ? PS->GetSquadComponent() : nullptr;
				UGGYGOPawnData* Member = Experience && !Experience->SquadMembers.IsEmpty()
					? const_cast<UGGYGOPawnData*>(Experience->SquadMembers[0].Get()) : nullptr;
				if (!Member || !Squad || !Squad->SetRoster({Member, Member})) { Fail(TEXT("生产 Experience 无法建立两成员测试编队。")); }
			});
			Settings.Reset(NewObject<ULevelEditorPlaySettings>());
			Settings->SetPlayNetMode(bNetwork ? EPlayNetMode::PIE_ListenServer : EPlayNetMode::PIE_Standalone);
			Settings->SetRunUnderOneProcess(true);
			Settings->SetPlayNumberOfClients(bNetwork ? 2 : 1);
			Settings->bLaunchSeparateServer = false;
			Settings->GameGetsMouseControl = false;
			Settings->bShouldMinimizeEditorOnNonVRPIE = false;
			FRequestPlaySessionParams Params;
			Params.EditorPlaySettings = Settings.Get(); Params.bAllowOnlineSubsystem = false;
			GEditor->RequestPlaySession(Params);
			const auto Queued = GEditor->GetPlaySessionRequest();
			if (!Queued.IsSet() || !Queued->EditorPlaySettings) { Fail(TEXT("原生 PIE 请求未保留设置。")); return; }
			Settings.Reset(Queued->EditorPlaySettings.Get());
		}
		bool AddSide(UWorld* World, bool bRemotePlayer, const TCHAR* Name)
		{
			APlayerController* PC = nullptr;
			for (auto It = World->GetPlayerControllerIterator(); It; ++It)
			{ if (It->Get() && It->Get()->IsLocalController() != bRemotePlayer) { PC = It->Get(); break; } }
			const auto* PS = PC ? PC->GetPlayerState<AGGYGOPlayerState>() : nullptr;
			auto* Squad = PS ? PS->GetSquadComponent() : nullptr;
			const auto* Slot = Squad ? Squad->GetSlot(0) : nullptr;
			APawn* Pawn = Slot ? Slot->GetAvatarPawn() : nullptr;
			auto* ASC = Slot ? Slot->GetGGYGOAbilitySystemComponent() : nullptr;
			auto* Health = UGGYGOHealthComponent::FindHealthComponent(Pawn);
			if (!Require(Pawn && PC->GetPawn() == Pawn && ASC && Health && Health->GetHealth() > 10.0f
				&& Health->GetPoise() > 0.0f && Health->GetDeathState() == EGGYGODeathState::NotDead,
				TEXT("原玩家 Pawn、ASC 与 Health 就绪"))) { return false; }
			FSide& Side = Sides.AddDefaulted_GetRef();
			Side.Name = Name; Side.Controller = PC; Side.Squad = Squad; Side.Pawn = Pawn; Side.ASC = ASC; Side.Health = Health;
			return true;
		}
		void Ready()
		{
			Sides.Reset();
			UWorld* Authority = nullptr; UWorld* Client = nullptr;
			for (const auto& Context : GEditor->GetWorldContexts())
			{
				UWorld* World = Context.WorldType == EWorldType::PIE ? Context.World() : nullptr;
				if (!World || !World->HasBegunPlay()) { continue; }
				if (World->GetNetMode() == (bNetwork ? NM_ListenServer : NM_Standalone)) { Authority = World; }
				if (World->GetNetMode() == NM_Client) { Client = World; }
			}
			if (!Require(Authority && (!bNetwork || Client), TEXT("所需的生产 PIE 世界"))
				|| !AddSide(Authority, bNetwork, TEXT("authority")) || (bNetwork && !AddSide(Client, false, TEXT("client"))))
			{ ReadySince.Reset(); return; }
			// 移开测试中另一名玩家，让两端都能看到原死者；位置经正常复制抵达客户端。
			if (bNetwork && !bHostSeparated)
			{
				for (auto It = Authority->GetPlayerControllerIterator(); It; ++It)
				{
					APlayerController* PC = It->Get();
					APawn* HostPawn = PC && PC->IsLocalController() ? PC->GetPawn() : nullptr;
					if (!HostPawn || HostPawn == Sides[0].Pawn.Get()) { continue; }
					if (!HostPawn->TeleportTo(Sides[0].Pawn->GetActorLocation() + FVector(0, 1500, 0), HostPawn->GetActorRotation()))
					{ Fail(TEXT("无法移开遮挡原角色的测试主机玩家。")); return; }
					HostPawn->ForceNetUpdate(); bHostSeparated = true; break;
				}
				if (!Require(bHostSeparated, TEXT("另一名测试玩家就绪并移开"))) { ReadySince.Reset(); return; }
			}
			for (const FSide& Side : Sides)
			{
				const ACharacter* Character = Cast<ACharacter>(Side.Pawn.Get());
				const auto* Movement = Character ? Character->GetCharacterMovement() : nullptr;
				const auto* Viewer = Side.Pawn->GetWorld()->GetFirstPlayerController();
				const auto* Manager = Viewer ? Cast<AGGYGOPlayerCameraManager>(Viewer->PlayerCameraManager) : nullptr;
				if (!Require(Movement && Movement->IsMovingOnGround() && Character->GetVelocity().IsNearlyZero(1.0)
					&& Manager && Manager->GetCameraPublicationState() == EGGYGOCameraPublicationState::Running,
					TEXT("原角色落地静止且本地镜头完成准入"))) { ReadySince.Reset(); return; }
			}
			// 给初始附身和落地复制稳定时间，避免诊断取景被迟到的 PawnClientRestart 覆盖。
			if (!ReadySince.IsSet()) { ReadySince = FPlatformTime::Seconds(); }
			if (FPlatformTime::Seconds() - *ReadySince < 1.0) { return; }
			if (bNetwork)
			{
				const auto Guid = [](const FSide& Side)
				{ const auto* Driver = Side.Pawn->GetNetDriver(); return Driver && Driver->GetNetGuidCache().IsValid()
					? Driver->GetNetGuidCache()->GetNetGUID(Side.Pawn.Get()) : FNetworkGUID(); };
				if (!Require(Guid(Sides[0]).IsValid() && Guid(Sides[0]) == Guid(Sides[1])
					&& Sides[0].Controller->PlayerState->GetPlayerId() == Sides[1].Controller->PlayerState->GetPlayerId(),
					TEXT("两端原 Pawn NetGUID 与 PlayerId 配对"))) { return; }
				Test.AddInfo(FString::Printf(TEXT("[Health.Acceptance.Pair] PawnNetGUID=%s PlayerId=%d Authority=%s Client=%s"),
					*Guid(Sides[0]).ToString(), Sides[0].Controller->PlayerState->GetPlayerId(),
					*Sides[0].Pawn->GetPathName(), *Sides[1].Pawn->GetPathName()));
			}
			InitialHealth = Sides[0].Health->GetHealth(); InitialPoise = Sides[0].Health->GetPoise();
			for (FSide& Side : Sides)
			{
				Side.Observer.Reset(NewObject<UGGYGOHealthAcceptanceObserver>());
				Side.Observer->Observe(Side.Health.Get(), Side.ASC.Get(), Side.Name);
				UWorld* World = Side.Pawn->GetWorld();
				APlayerController* Viewer = nullptr;
				for (auto It = World->GetPlayerControllerIterator(); It; ++It)
				{ if (It->Get() && It->Get()->IsLocalController()) { Viewer = It->Get(); break; } }
				UGameViewportClient* Viewport = World->GetGameViewport();
				if (!Viewer || !Viewport || !Viewport->Viewport) { Fail(TEXT("缺少该端实际视口。")); return; }
				// 同一原角色的侧面诊断机位，仅存在于自有 PIE；不保存或更改生产镜头资产。
				ACameraActor* Camera = World->SpawnActor<ACameraActor>();
				if (!Camera) { Fail(TEXT("无法建立只用于实拍的相机。")); return; }
				const FVector Focus = Side.Pawn->GetActorLocation() + FVector(0, 0, -20);
				const FVector Position = Focus + Side.Pawn->GetActorForwardVector() * 370.0f + Side.Pawn->GetActorRightVector() * 270.0f + FVector(0, 0, 200);
				Camera->SetActorLocation(Position); Camera->SetActorRotation((Focus - Position).Rotation()); Viewer->SetViewTarget(Camera);
				if (Viewer->GetViewTarget() != Camera) { Fail(TEXT("诊断相机未获原生 ViewTarget 准入。")); return; }
				Side.View = FSceneViewExtensions::NewExtension<FHealthView>(Viewport, Side.Health.Get(), Viewer, Camera);
			}
			Rendered = UGameViewportClient::OnViewportRendered().AddRaw(this, &FHealthCommand::Capture);
			Advance(EStage::Baseline);
		}
		bool Values(float Health, float Poise)
		{
			for (const FSide& Side : Sides)
			{
				if (!Require(Side.ASC.IsValid() && Side.Health.IsValid()
					&& FMath::IsNearlyEqual(Side.ASC->GetNumericAttribute(UGGYGOHealthSet::GetHealthAttribute()), Health, 0.01f)
					&& FMath::IsNearlyEqual(Side.Health->GetHealth(), Health, 0.01f)
					&& FMath::IsNearlyEqual(Side.Health->GetPoise(), Poise, 0.01f), TEXT("原角色两端 ASC／HealthComponent 数值收敛"))) { return false; }
			}
			return true;
		}
		bool Notices(const TCHAR* Kind, int32 Expected)
		{
			for (const FSide& Side : Sides)
			{
				const int32 Actual = Side.Observer->Count(Kind);
				if (Actual > Expected) { Fail(Side.Name + TEXT(" 重复广播：") + Kind); return false; }
				if (!Require(Actual == Expected, Kind)) { return false; }
			}
			return true;
		}
		void Apply(float Damage, float PoiseDamage)
		{
			auto* ASC = Sides[0].ASC.Get();
			const auto Effect = UGGYGOGameData::ResolveDamageGameplayEffect(nullptr, true);
			if (!ASC || !Effect) { Fail(TEXT("共享生产伤害 GE 未就绪。")); return; }
			auto Context = ASC->MakeEffectContext();
			Context.AddInstigator(Sides[0].Pawn.Get(), Sides[0].Pawn.Get());
			auto Spec = ASC->MakeOutgoingSpec(Effect, 1.0f, Context);
			if (!Spec.IsValid()) { Fail(TEXT("无法创建生产伤害 Spec。")); return; }
			Spec.Data->SetSetByCallerMagnitude(GGYGOGameplayTags::SetByCaller_Damage, Damage);
			Spec.Data->SetSetByCallerMagnitude(GGYGOGameplayTags::SetByCaller_PoiseDamage, PoiseDamage);
			ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
			Test.AddInfo(FString::Printf(TEXT("[Health.Acceptance.Apply] GE=%s Damage=%.3f PoiseDamage=%.3f"), *Effect->GetPathName(), Damage, PoiseDamage));
		}
		bool CapturedByAll(const TCHAR* Name)
		{
			for (const FSide& Side : Sides) { if (!Require(Side.Captures.Contains(Name), Name)) { return false; } }
			return true;
		}
		void Capture(FViewport* Viewport)
		{
			for (FSide& Side : Sides)
			{
				const auto View = Side.View;
				if (!View.IsValid() || !View->Viewport.IsValid() || View->Viewport->Viewport != Viewport || View->Frame != GFrameCounter) { continue; }
				FString Label;
				if (FPlatformTime::Seconds() - Entered < (Stage == EStage::Baseline ? 1.0 : 0.35)) { continue; }
				if (Stage == EStage::Baseline) { Label = TEXT("baseline"); }
				else if (Stage == EStage::Damage && Values(InitialHealth - 10, InitialPoise)) { Label = TEXT("damage"); }
				else if (Stage == EStage::Poise && Values(InitialHealth - 10, 0)) { Label = TEXT("poise"); }
				else if (Stage == EStage::Death && View->DeathState == uint8(EGGYGODeathState::DeathStarted) && View->Progress >= 0.15f)
				{ Label = View->Progress >= 0.75f ? TEXT("dying_late") : TEXT("dying"); }
				if (Label.IsEmpty() || Side.Captures.Contains(Label)) { continue; }
				TArray<FColor> Pixels; const FIntPoint Size = Viewport->GetSizeXY();
				if (Size.X <= 0 || Size.Y <= 0 || !Viewport->ReadPixels(Pixels) || Pixels.Num() != Size.X * Size.Y)
				{ Fail(TEXT("真实视口像素读取失败。")); return; }
				for (FColor& Pixel : Pixels) { Pixel.A = 255; }
				TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
				const FString File = Output / (Side.Name + TEXT("_") + Label + TEXT(".png"));
				if (!FFileHelper::SaveArrayToFile(Png, *File)) { Fail(TEXT("截图保存失败：") + File); return; }
				Side.Captures.Add(Label);
				Test.AddInfo(FString::Printf(TEXT("[Health.Acceptance.Draw] %s %s %s"), *Side.Name, *File, *View->Evidence));
			}
		}
		void Death()
		{
			if (!bDyingMatched && Values(0, 0))
			{
				bDyingMatched = true;
				for (const auto& Side : Sides) { bDyingMatched &= Side.Health->GetDeathState() == EGGYGODeathState::DeathStarted; }
			}
			if (!Notices(TEXT("DeathFinished"), 1)) { return; }
			if (!Notices(TEXT("HealthChanged"), 2) || !Notices(TEXT("PoiseChanged"), 1)
				|| !Notices(TEXT("OutOfHealth"), 1) || !Notices(TEXT("PoiseBroken"), 1) || !Notices(TEXT("DeathStarted"), 1)) { return; }
			// 等原下一帧蓝图清理和复制尾包通过，避免在首个完成通知处结束 PIE，漏掉迟到重复事件。
			if (!FinishedAt.IsSet()) { FinishedAt = FPlatformTime::Seconds(); }
			if (FPlatformTime::Seconds() - *FinishedAt < 0.5) { return; }
			Test.TestTrue(TEXT("原角色在两端均观察到 Health=0、Poise=0、DeathStarted"), bDyingMatched);
			for (const auto& Side : Sides)
			{
				const auto* StartEvent = Side.Observer->Find(TEXT("DeathStarted"));
				const auto* EndEvent = Side.Observer->Find(TEXT("DeathFinished"));
				Test.TestTrue(Side.Name + TEXT(" 原死者开始／结束各一次且阶段单调"), StartEvent && EndEvent
					&& StartEvent->bOriginalActor && EndEvent->bOriginalActor
					&& StartEvent->DeathState == 1 && EndEvent->DeathState == 2
					&& Side.Observer->Events.IndexOfByPredicate([](const auto& E) { return E.Kind == TEXT("DeathStarted"); })
					< Side.Observer->Events.IndexOfByPredicate([](const auto& E) { return E.Kind == TEXT("DeathFinished"); }));
				Test.TestTrue(Side.Name + TEXT(" 原死者完成广播时 Health 与 ASC 都为零"), EndEvent
					&& FMath::IsNearlyZero(EndEvent->ASCHealth) && FMath::IsNearlyZero(EndEvent->ComponentHealth));
				Test.TestTrue(Side.Name + TEXT(" 实际诊断机位自然 Draw 已捕获原死亡 Montage 前后段"),
					Side.Captures.Contains(TEXT("dying")) && Side.Captures.Contains(TEXT("dying_late")));
				Test.TestEqual(Side.Name + TEXT(" 原持久 ASC 死亡后仍为零"), Side.ASC->GetNumericAttribute(UGGYGOHealthSet::GetHealthAttribute()), 0.0f);
			}
			Test.AddInfo(TEXT("[Health.Acceptance] 已核原角色扣血、破韧、零血、DeathStarted/DeathFinished 及自然 Draw；原日志不屏蔽。"));
			End();
		}
		FAutomationTestBase& Test;
		bool bNetwork = false, bFailed = false, bDyingMatched = false, bHostSeparated = false;
		EStage Stage = EStage::Start;
		double Entered = 0, Deadline = 0;
		TOptional<double> FinishedAt, ReadySince;
		float InitialHealth = 0, InitialPoise = 0;
		FString Output, Waiting;
		TArray<FSide> Sides;
		TStrongObjectPtr<ULevelEditorPlaySettings> Settings;
		FDelegateHandle PostLogin, Rendered;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOHealthStandaloneAcceptance, "GGYGO.AbilitySystem.HealthAcceptance.Standalone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGGYGOHealthStandaloneAcceptance::RunTest(const FString& Parameters)
{ ADD_LATENT_AUTOMATION_COMMAND(GGYGOHealthAcceptance::FHealthCommand(*this, false)); return true; }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOHealthNetworkAcceptance, "GGYGO.AbilitySystem.HealthAcceptance.ListenServer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGGYGOHealthNetworkAcceptance::RunTest(const FString& Parameters)
{ ADD_LATENT_AUTOMATION_COMMAND(GGYGOHealthAcceptance::FHealthCommand(*this, true)); return true; }
#endif
