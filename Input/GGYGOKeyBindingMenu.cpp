/**
 * @file GGYGOKeyBindingMenu.cpp
 * @brief 原用户设置事务和 Slate 真实选键转接。
 */
#include "Input/GGYGOKeyBindingMenu.h"

#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "EnhancedInputSubsystems.h"
#include "Framework/Commands/InputChord.h"
#include "GameFramework/PlayerController.h"
#include "Input/Events.h"
#include "Input/GGYGOPlayerInput.h"
#include "Player/GGYGOLocalPlayer.h"
#include "UserSettings/EnhancedInputUserSettings.h"
#include "Widgets/Input/SInputKeySelector.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOKeyBindingMenu)

namespace
{
	DECLARE_DELEGATE_TwoParams(FOnBindingKeySelected, const FInputChord&, const FKeyEvent*);

	/** 保留 SInputKeySelector 同栈消费的实际 KeyUp，供改键成功后登记原释放。 */
	class SGGYGOKeySelector final : public SInputKeySelector
	{
	public:
		SLATE_BEGIN_ARGS(SGGYGOKeySelector) {}
			SLATE_ATTRIBUTE(FInputChord, SelectedKey)
			SLATE_ARGUMENT(FText, KeySelectionText)
			SLATE_ARGUMENT(FText, NoKeySpecifiedText)
			SLATE_EVENT(FOnBindingKeySelected, OnKeySelected)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			OnBindingKeySelected = InArgs._OnKeySelected;
			SInputKeySelector::FArguments Args;
			Args
				.SelectedKey(InArgs._SelectedKey)
				.OnKeySelected(this, &SGGYGOKeySelector::ForwardSelection)
				.AllowModifierKeys(false)
				.AllowGamepadKeys(false)
				.EscapeCancelsSelection(true);
			if (!InArgs._KeySelectionText.IsEmpty()) { Args.KeySelectionText(InArgs._KeySelectionText); }
			if (!InArgs._NoKeySpecifiedText.IsEmpty()) { Args.NoKeySpecifiedText(InArgs._NoKeySpecifiedText); }
			SInputKeySelector::Construct(Args);
		}

		virtual FReply OnKeyUp(const FGeometry& Geometry, const FKeyEvent& Event) override
		{
			TGuardValue<const FKeyEvent*> OriginalKeyUp(SelectionKeyUp, &Event);
			return SInputKeySelector::OnKeyUp(Geometry, Event);
		}

	private:
		void ForwardSelection(const FInputChord& Chord)
		{
			// 鼠标按下选键照常改键，只在原键盘 KeyUp 同栈选择相同键时传递释放。
			const FKeyEvent* KeyUp = SelectionKeyUp && SelectionKeyUp->GetKey() == Chord.Key ? SelectionKeyUp : nullptr;
			OnBindingKeySelected.ExecuteIfBound(Chord, KeyUp);
		}

		FOnBindingKeySelected OnBindingKeySelected;
		const FKeyEvent* SelectionKeyUp = nullptr;
	};

	const FPlayerKeyMapping* FindFirstSlotMapping(const FKeyMappingRow& Row)
	{
		for (const FPlayerKeyMapping& Mapping : Row.Mappings)
		{
			if (Mapping.GetSlot() == EPlayerMappableKeySlot::First) { return &Mapping; }
		}
		return nullptr;
	}

	void ApplyAndSave(UEnhancedInputUserSettings& Settings)
	{
		const TWeakObjectPtr<UEnhancedInputUserSettings> Original(&Settings);
		Settings.ApplySettings();
		if (UEnhancedInputUserSettings* StillOriginal = Original.Get()) { StillOriginal->AsyncSaveSettings(); }
	}
}

void UGGYGOKeySettingsPage::InitializeBindingContext(UEnhancedInputUserSettings& Settings,
	UGGYGOPlayerInput& PlayerInput, FSimpleDelegate CloseRequested)
{
	OriginalSettings = &Settings;
	OriginalPlayerInput = &PlayerInput;
	OnCloseRequested = MoveTemp(CloseRequested);
}

bool UGGYGOKeySettingsPage::BeginBindingObservation(const TSharedRef<SWidget>& OriginalRoot)
{
	UEnhancedInputUserSettings* Settings = FindOriginalSettings();
	if (!Settings)
	{
		return false;
	}
	OriginalPlayerInput->BeginKeyBindingMenuObservation(*Settings, OriginalRoot);
	return true;
}

UGGYGOPlayerInput* UGGYGOKeySettingsPage::GetOriginalPlayerInput() const
{
	return OriginalPlayerInput.Get();
}

bool UGGYGOKeySettingsPage::PrepareReturnToGame(const SWidget& OriginalRoot)
{
	UEnhancedInputUserSettings* Settings = FindOriginalSettings();
	if (!Settings)
	{
		return false;
	}
	OriginalPlayerInput->BeginKeyBindingMenuClose(*Settings, OriginalRoot);
	return true;
}

void UGGYGOKeySettingsPage::ReleaseBindingContext()
{
	OnCloseRequested.Unbind();
	OriginalSettings.Reset();
	OriginalPlayerInput.Reset();
}

UEnhancedInputUserSettings* UGGYGOKeySettingsPage::FindOriginalSettings() const
{
	UEnhancedInputUserSettings* Settings = OriginalSettings.Get();
	const UGGYGOPlayerInput* Input = OriginalPlayerInput.Get();
	const UGGYGOLocalPlayer* LocalPlayer = GetOwningLocalPlayer<UGGYGOLocalPlayer>();
	const APlayerController* Controller = GetOwningPlayer();
	if (!Settings || !Input || !LocalPlayer || LocalPlayer->IsInputMappingRemovalStarted()
		|| Settings->GetLocalPlayer() != LocalPlayer || !IsValid(Controller) || Controller->IsActorBeingDestroyed()
		|| LocalPlayer->PlayerController != Controller || Controller->PlayerInput != Input)
	{
		return nullptr;
	}
	const UEnhancedInputLocalPlayerSubsystem* Subsystem = LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
	return Subsystem && Subsystem->GetUserSettings() == Settings && Subsystem->GetPlayerInput() == Input ? Settings : nullptr;
}

bool UGGYGOKeySettingsPage::ReadKeyBindingRows(TArray<FGGYGOKeyBindingRow>& Rows) const
{
	Rows.Reset();
	const UEnhancedInputUserSettings* Settings = FindOriginalSettings();
	const UEnhancedPlayerMappableKeyProfile* Profile = Settings ? Settings->GetActiveKeyProfile() : nullptr;
	if (!Profile)
	{
		return false;
	}
	for (const TPair<FName, FKeyMappingRow>& Pair : Profile->GetPlayerMappingRows())
	{
		if (const FPlayerKeyMapping* Mapping = FindFirstSlotMapping(Pair.Value))
		{
			FGGYGOKeyBindingRow& Row = Rows.AddDefaulted_GetRef();
			Row.MappingName = Pair.Key;
			Row.Category = Mapping->GetDisplayCategory();
			Row.DisplayName = Mapping->GetDisplayName();
		}
	}
	Rows.Sort([](const FGGYGOKeyBindingRow& A, const FGGYGOKeyBindingRow& B)
	{
		const int32 ByCategory = A.Category.CompareTo(B.Category);
		return ByCategory != 0 ? ByCategory < 0 : A.MappingName.LexicalLess(B.MappingName);
	});
	return true;
}

FInputChord UGGYGOKeySettingsPage::ReadCurrentChord(FName MappingName) const
{
	const UEnhancedInputUserSettings* Settings = FindOriginalSettings();
	const FPlayerKeyMapping* Mapping = Settings
		? Settings->FindCurrentMappingForSlot(MappingName, EPlayerMappableKeySlot::First) : nullptr;
	return Mapping ? FInputChord(Mapping->GetCurrentKey()) : FInputChord();
}

void UGGYGOKeySettingsPage::CommitNativeSelection(FName MappingName, const FInputChord& Chord, const FKeyEvent* KeyUp)
{
	UEnhancedInputUserSettings* Settings = FindOriginalSettings();
	const UEnhancedPlayerMappableKeyProfile* Profile = Settings ? Settings->GetActiveKeyProfile() : nullptr;
	if (!Profile || !Chord.Key.IsValid() || !Settings->FindCurrentMappingForSlot(MappingName, EPlayerMappableKeySlot::First))
	{
		return;
	}

	FGGYGOKeyBindingResult Result;
	Result.Key = Chord.Key;
	TArray<FName> Users;
	Profile->GetMappingNamesForKey(Chord.Key, Users);
	Users.Remove(MappingName);
	if (!Users.IsEmpty())
	{
		const FPlayerKeyMapping* Other = Settings->FindCurrentMappingForSlot(Users[0], EPlayerMappableKeySlot::First);
		Result.Status = EGGYGOKeyBindingResult::Conflict;
		Result.ConflictingDisplayName = Other ? Other->GetDisplayName() : FText::FromName(Users[0]);
		OnKeyBindingResult(Result);
		return;
	}

	const bool bChangedKey = ReadCurrentChord(MappingName).Key != Chord.Key;
	const TWeakObjectPtr<UEnhancedInputUserSettings> WriteTarget(Settings);
	FMapPlayerKeyArgs Args;
	Args.MappingName = MappingName;
	Args.Slot = EPlayerMappableKeySlot::First;
	Args.NewKey = Chord.Key;
	Settings->MapPlayerKey(Args, Result.Failure);
	if (!Result.Failure.IsEmpty())
	{
		Result.Status = EGGYGOKeyBindingResult::Rejected;
		UE_LOG(LogGGYGOAbilitySystem, Warning, TEXT("[Input/KeyBinding] Mapping=%s Key=%s rejected: %s"),
			*MappingName.ToString(), *Chord.Key.ToString(), *Result.Failure.ToStringSimple());
	}
	else if (UEnhancedInputUserSettings* Original = WriteTarget.Get())
	{
		if (KeyUp && bChangedKey && FindOriginalSettings() == Original)
		{
			OriginalPlayerInput->ObserveKeyBindingMenuRelease(*Original, *KeyUp);
		}
		ApplyAndSave(*Original);
		Result.Status = EGGYGOKeyBindingResult::Applied;
	}
	if (FindOriginalSettings())
	{
		OnKeyBindingResult(Result);
	}
}

FGGYGOKeyBindingResult UGGYGOKeySettingsPage::ResetKeyBindings()
{
	FGGYGOKeyBindingResult Result;
	UEnhancedInputUserSettings* Settings = FindOriginalSettings();
	if (!Settings)
	{
		return Result;
	}
	const FString ProfileId = Settings->GetActiveKeyProfileId();
	const TWeakObjectPtr<UEnhancedInputUserSettings> WriteTarget(Settings);
	Settings->ResetKeyProfileIdToDefault(ProfileId, Result.Failure);
	if (!Result.Failure.IsEmpty())
	{
		Result.Status = EGGYGOKeyBindingResult::Rejected;
		UE_LOG(LogGGYGOAbilitySystem, Warning, TEXT("[Input/KeyBinding] Reset rejected: %s"), *Result.Failure.ToStringSimple());
	}
	else if (UEnhancedInputUserSettings* Original = WriteTarget.Get())
	{
		ApplyAndSave(*Original);
		Result.Status = EGGYGOKeyBindingResult::Applied;
	}
	return Result;
}

void UGGYGOKeySettingsPage::RequestKeySettingsClose()
{
	// 关闭可能撤销成员委托；先保留本次原请求。
	const FSimpleDelegate CloseRequested = OnCloseRequested;
	CloseRequested.ExecuteIfBound();
}

void UGGYGOKeyBindingSelector::BindKeyBindingRow(UGGYGOKeySettingsPage* Page, FName MappingName)
{
	BindingPage = Page;
	BoundMappingName = MappingName;
}

TSharedRef<SWidget> UGGYGOKeyBindingSelector::RebuildWidget()
{
	return SNew(SGGYGOKeySelector)
		.SelectedKey(TAttribute<FInputChord>::Create(
			TAttribute<FInputChord>::FGetter::CreateUObject(this, &ThisClass::ReadCurrentChord)))
		.KeySelectionText(KeySelectionText)
		.NoKeySpecifiedText(NoKeySpecifiedText)
		.OnKeySelected(FOnBindingKeySelected::CreateUObject(this, &ThisClass::HandleNativeSelection));
}

FInputChord UGGYGOKeyBindingSelector::ReadCurrentChord() const
{
	const UGGYGOKeySettingsPage* Page = BindingPage.Get();
	return Page ? Page->ReadCurrentChord(BoundMappingName) : FInputChord();
}

void UGGYGOKeyBindingSelector::HandleNativeSelection(const FInputChord& Chord, const FKeyEvent* KeyUp)
{
	if (UGGYGOKeySettingsPage* Page = BindingPage.Get())
	{
		Page->CommitNativeSelection(BoundMappingName, Chord, KeyUp);
	}
}
