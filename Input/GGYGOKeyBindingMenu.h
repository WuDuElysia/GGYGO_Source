/**
 * @file GGYGOKeyBindingMenu.h
 * @brief 共享 UMG 改键页使用的原设置事务和真实选键控件。
 */
#pragma once

#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "GameplayTagContainer.h"
#include "InputCoreTypes.h"

#include "GGYGOKeyBindingMenu.generated.h"

class UEnhancedInputUserSettings;
class UGGYGOKeyBindingSelector;
class UGGYGOPlayerInput;
struct FInputChord;
struct FKeyEvent;

UENUM(BlueprintType)
enum class EGGYGOKeyBindingResult : uint8
{
	Unavailable,
	Applied,
	Conflict,
	Rejected
};

USTRUCT(BlueprintType)
struct FGGYGOKeyBindingRow
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "GGYGO|Input")
	FName MappingName;

	UPROPERTY(BlueprintReadOnly, Category = "GGYGO|Input")
	FText Category;

	UPROPERTY(BlueprintReadOnly, Category = "GGYGO|Input")
	FText DisplayName;
};

USTRUCT(BlueprintType)
struct FGGYGOKeyBindingResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "GGYGO|Input")
	EGGYGOKeyBindingResult Status = EGGYGOKeyBindingResult::Unavailable;

	UPROPERTY(BlueprintReadOnly, Category = "GGYGO|Input")
	FKey Key;

	UPROPERTY(BlueprintReadOnly, Category = "GGYGO|Input")
	FText ConflictingDisplayName;

	UPROPERTY(BlueprintReadOnly, Category = "GGYGO|Input")
	FGameplayTagContainer Failure;
};

UCLASS(Abstract, Blueprintable)
class GGYGO_API UGGYGOKeySettingsPage : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Controller 在构造页面的 Slate 根之前注入原输入上下文。 */
	void InitializeBindingContext(UEnhancedInputUserSettings& Settings, UGGYGOPlayerInput& PlayerInput,
		FSimpleDelegate CloseRequested);
	bool BeginBindingObservation(const TSharedRef<SWidget>& OriginalRoot);
	UGGYGOPlayerInput* GetOriginalPlayerInput() const;
	bool PrepareReturnToGame(const SWidget& OriginalRoot);
	void ReleaseBindingContext();

	UFUNCTION(BlueprintCallable, Category = "GGYGO|Input")
	bool ReadKeyBindingRows(TArray<FGGYGOKeyBindingRow>& Rows) const;

	UFUNCTION(BlueprintCallable, Category = "GGYGO|Input")
	FGGYGOKeyBindingResult ResetKeyBindings();

	UFUNCTION(BlueprintCallable, Category = "GGYGO|Input")
	void RequestKeySettingsClose();

protected:
	UFUNCTION(BlueprintImplementableEvent, Category = "GGYGO|Input")
	void OnKeyBindingResult(const FGGYGOKeyBindingResult& Result);

private:
	friend class UGGYGOKeyBindingSelector;

	UEnhancedInputUserSettings* FindOriginalSettings() const;
	FInputChord ReadCurrentChord(FName MappingName) const;
	void CommitNativeSelection(FName MappingName, const FInputChord& Chord, const FKeyEvent* KeyUp);

	// 只借用打开本页时的原设置和原输入对象，关页时撤销访问。
	TWeakObjectPtr<UEnhancedInputUserSettings> OriginalSettings;
	TWeakObjectPtr<UGGYGOPlayerInput> OriginalPlayerInput;
	FSimpleDelegate OnCloseRequested;
};

UCLASS()
class GGYGO_API UGGYGOKeyBindingSelector : public UWidget
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Input")
	void BindKeyBindingRow(UGGYGOKeySettingsPage* Page, FName MappingName);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Appearance)
	FText KeySelectionText;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Appearance)
	FText NoKeySpecifiedText;

private:
	FInputChord ReadCurrentChord() const;
	void HandleNativeSelection(const FInputChord& Chord, const FKeyEvent* KeyUp);

	TWeakObjectPtr<UGGYGOKeySettingsPage> BindingPage;
	FName BoundMappingName;
};
