// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Layout/Margin.h"
#include "Widgets/Layout/Anchors.h"
#include "UObject/NoExportTypes.h"
#include "UWidgetDesignerService.generated.h"

/**
 * Live designer preview and persisted design settings for a Widget Blueprint.
 * Preview fields are only populated when a Widget Blueprint editor + designer tab are available.
 */
USTRUCT(BlueprintType)
struct FWidgetDesignerPreviewInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	FString WidgetPath;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	bool bWidgetBlueprintLoaded = false;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	bool bEditorOpen = false;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	bool bDesignerReady = false;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	int32 PreviewAreaWidth = 0;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	int32 PreviewAreaHeight = 0;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	int32 PreviewWidgetWidth = 0;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	int32 PreviewWidgetHeight = 0;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	float PreviewDPIScale = 1.0f;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	FString DesignSizeMode;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	FVector2D DesignTimeSize = FVector2D::ZeroVector;
};

/**
 * Structured slot layout for common UMG panel slot types.
 * FieldsToApply is used by SetSlotLayout to support partial updates.
 */
USTRUCT(BlueprintType)
struct FWidgetDesignerSlotLayout
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	bool bHasSlot = false;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	FString WidgetPath;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	FString ComponentName;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	FString ParentWidget;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	FString SlotClass;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	TArray<FString> SupportedFields;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	TArray<FString> FieldsToApply;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	FMargin Padding = FMargin(0.0f);

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	FString HorizontalAlignment = TEXT("Fill");

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	FString VerticalAlignment = TEXT("Fill");

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	FAnchors Anchors = FAnchors(0.0f, 0.0f, 0.0f, 0.0f);

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	FMargin Offsets = FMargin(0.0f);

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	FVector2D Alignment = FVector2D::ZeroVector;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	bool bAutoSize = false;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	int32 ZOrder = 0;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	FString SizeRule = TEXT("Automatic");

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	float SizeValue = 0.0f;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	int32 Row = 0;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	int32 Column = 0;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	int32 RowSpan = 1;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	int32 ColumnSpan = 1;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	int32 Layer = 0;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	FVector2D Nudge = FVector2D::ZeroVector;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	TMap<FString, FString> RawProperties;
};

/**
 * Current widget/component selection in the Widget Designer editor.
 */
USTRUCT(BlueprintType)
struct FWidgetDesignerSelectionInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	FString WidgetPath;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	bool bWidgetBlueprintLoaded = false;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	bool bEditorOpen = false;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	bool bDesignerReady = false;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	int32 SelectedCount = 0;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	FString PrimaryComponent;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	TArray<FString> SelectedComponents;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	bool bHasNamedSlotSelection = false;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	FString NamedSlotHostComponent;

	UPROPERTY(BlueprintReadWrite, Category = "Widget Designer")
	FString NamedSlotName;
};

/**
 * Designer-focused service for Widget Blueprint preview sizing and explicit slot layout editing.
 */
UCLASS(BlueprintType)
class VIBEUE_API UWidgetDesignerService : public UObject
{
	GENERATED_BODY()

public:
	/** Get live preview info from the Widget Designer, plus persisted design size settings. */
	UFUNCTION(BlueprintCallable, Category = "VibeUE|WidgetDesigner")
	static FWidgetDesignerPreviewInfo GetPreviewInfo(
		const FString& WidgetPath,
		bool bOpenIfNeeded = true,
		bool bSwitchToDesigner = true);

	/** Set the current live designer preview area size. */
	UFUNCTION(BlueprintCallable, Category = "VibeUE|WidgetDesigner")
	static bool SetPreviewArea(
		const FString& WidgetPath,
		int32 PreviewWidth,
		int32 PreviewHeight,
		bool bOpenIfNeeded = true,
		bool bSwitchToDesigner = true);

	/** Get persisted design settings from the Widget Blueprint default object. */
	UFUNCTION(BlueprintCallable, Category = "VibeUE|WidgetDesigner")
	static FWidgetDesignerPreviewInfo GetDesignSettings(const FString& WidgetPath);

	/** Set persisted design settings on the Widget Blueprint default object. */
	UFUNCTION(BlueprintCallable, Category = "VibeUE|WidgetDesigner")
	static bool SetDesignSettings(
		const FString& WidgetPath,
		const FString& SizeMode,
		FVector2D CustomSize = FVector2D(1920.0f, 1080.0f));

	/** Read the current widget/component selection in the Widget Designer editor. */
	UFUNCTION(BlueprintCallable, Category = "VibeUE|WidgetDesigner")
	static FWidgetDesignerSelectionInfo GetSelectionInfo(
		const FString& WidgetPath,
		bool bOpenIfNeeded = true,
		bool bSwitchToDesigner = true);

	/** Select one or more widget components in the Widget Designer editor. */
	UFUNCTION(BlueprintCallable, Category = "VibeUE|WidgetDesigner")
	static bool SelectComponents(
		const FString& WidgetPath,
		const TArray<FString>& ComponentNames,
		bool bAppendToSelection = false,
		bool bOpenIfNeeded = true,
		bool bSwitchToDesigner = true);

	/** Clear the current widget/component selection in the Widget Designer editor. */
	UFUNCTION(BlueprintCallable, Category = "VibeUE|WidgetDesigner")
	static bool ClearSelection(
		const FString& WidgetPath,
		bool bOpenIfNeeded = true,
		bool bSwitchToDesigner = true);

	/** Get structured slot layout info for a widget component. */
	UFUNCTION(BlueprintCallable, Category = "VibeUE|WidgetDesigner")
	static FWidgetDesignerSlotLayout GetSlotLayout(
		const FString& WidgetPath,
		const FString& ComponentName);

	/** Set structured slot layout info for a widget component. */
	UFUNCTION(BlueprintCallable, Category = "VibeUE|WidgetDesigner")
	static bool SetSlotLayout(
		const FString& WidgetPath,
		const FString& ComponentName,
		const FWidgetDesignerSlotLayout& SlotLayout);
};
