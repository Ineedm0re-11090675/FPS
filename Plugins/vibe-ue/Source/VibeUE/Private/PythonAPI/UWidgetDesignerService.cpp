// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "PythonAPI/UWidgetDesignerService.h"

#include "PythonAPI/UAssetDiscoveryService.h"

#include "AssetRegistry/AssetData.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "BlueprintModes/WidgetBlueprintApplicationModes.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/GridSlot.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/OverlaySlot.h"
#include "Components/PanelSlot.h"
#include "Components/UniformGridSlot.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/Widget.h"
#include "Components/SlateWrapperTypes.h"
#include "Designer/SDesignerView.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Framework/Docking/TabManager.h"
#include "JsonObjectConverter.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/PackageName.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Containers/Ticker.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditor.h"
#include "WidgetBlueprintEditorUtils.h"
#include "Widgets/Docking/SDockTab.h"

namespace
{
	static const FName DesignerTabId(TEXT("SlatePreview"));
	static TSet<FString> PendingWidgetPreviewRefreshes;

	static FString NormalizeToken(const FString& InValue)
	{
		FString Result;
		Result.Reserve(InValue.Len());

		for (const TCHAR Char : InValue)
		{
			if (FChar::IsAlnum(Char))
			{
				Result.AppendChar(FChar::ToLower(Char));
			}
		}

		return Result;
	}

	static FString NormalizeFieldName(const FString& InValue)
	{
		FString Result = InValue.TrimStartAndEnd().ToLower();
		Result.ReplaceInline(TEXT(" "), TEXT("_"));
		Result.ReplaceInline(TEXT("-"), TEXT("_"));
		return Result;
	}

	static FString ShortEnumName(const FString& InValue)
	{
		FString Result = InValue;
		int32 ScopeIndex = INDEX_NONE;
		if (Result.FindLastChar(TEXT(':'), ScopeIndex) && ScopeIndex + 1 < Result.Len())
		{
			Result = Result.Mid(ScopeIndex + 1);
		}

		if (Result.StartsWith(TEXT("HAlign_")))
		{
			return Result.RightChop(7);
		}

		if (Result.StartsWith(TEXT("VAlign_")))
		{
			return Result.RightChop(7);
		}

		return Result;
	}

	template<typename TEnum>
	static FString EnumToShortString(TEnum Value)
	{
		const UEnum* Enum = StaticEnum<TEnum>();
		if (!Enum)
		{
			return FString();
		}

		return ShortEnumName(Enum->GetNameStringByValue(static_cast<int64>(Value)));
	}

	static bool ParseHorizontalAlignment(const FString& InValue, EHorizontalAlignment& OutAlignment)
	{
		const FString Token = NormalizeToken(InValue);
		if (Token == TEXT("fill") || Token == TEXT("halignfill"))
		{
			OutAlignment = HAlign_Fill;
			return true;
		}
		if (Token == TEXT("left") || Token == TEXT("halignleft"))
		{
			OutAlignment = HAlign_Left;
			return true;
		}
		if (Token == TEXT("center") || Token == TEXT("haligncenter"))
		{
			OutAlignment = HAlign_Center;
			return true;
		}
		if (Token == TEXT("right") || Token == TEXT("halignright"))
		{
			OutAlignment = HAlign_Right;
			return true;
		}
		return false;
	}

	static bool ParseVerticalAlignment(const FString& InValue, EVerticalAlignment& OutAlignment)
	{
		const FString Token = NormalizeToken(InValue);
		if (Token == TEXT("fill") || Token == TEXT("valignfill"))
		{
			OutAlignment = VAlign_Fill;
			return true;
		}
		if (Token == TEXT("top") || Token == TEXT("valigntop"))
		{
			OutAlignment = VAlign_Top;
			return true;
		}
		if (Token == TEXT("center") || Token == TEXT("valigncenter"))
		{
			OutAlignment = VAlign_Center;
			return true;
		}
		if (Token == TEXT("bottom") || Token == TEXT("valignbottom"))
		{
			OutAlignment = VAlign_Bottom;
			return true;
		}
		return false;
	}

	static bool ParseSizeRule(const FString& InValue, ESlateSizeRule::Type& OutRule)
	{
		const FString Token = NormalizeToken(InValue);
		if (Token == TEXT("automatic") || Token == TEXT("auto") || Token == TEXT("size_rule_automatic"))
		{
			OutRule = ESlateSizeRule::Automatic;
			return true;
		}
		if (Token == TEXT("fill") || Token == TEXT("size_rule_fill"))
		{
			OutRule = ESlateSizeRule::Fill;
			return true;
		}
		return false;
	}

	static FString SizeRuleToString(ESlateSizeRule::Type Rule)
	{
		switch (Rule)
		{
		case ESlateSizeRule::Automatic:
			return TEXT("Automatic");
		case ESlateSizeRule::Fill:
			return TEXT("Fill");
		default:
			return TEXT("Automatic");
		}
	}

	static bool ParseDesignSizeMode(const FString& InValue, EDesignPreviewSizeMode& OutMode)
	{
		const FString Token = NormalizeToken(InValue);
		if (Token == TEXT("fillscreen"))
		{
			OutMode = EDesignPreviewSizeMode::FillScreen;
			return true;
		}
		if (Token == TEXT("custom"))
		{
			OutMode = EDesignPreviewSizeMode::Custom;
			return true;
		}
		if (Token == TEXT("customonscreen"))
		{
			OutMode = EDesignPreviewSizeMode::CustomOnScreen;
			return true;
		}
		if (Token == TEXT("desired"))
		{
			OutMode = EDesignPreviewSizeMode::Desired;
			return true;
		}
		if (Token == TEXT("desiredonscreen"))
		{
			OutMode = EDesignPreviewSizeMode::DesiredOnScreen;
			return true;
		}
		return false;
	}

	static FString DesignSizeModeToString(EDesignPreviewSizeMode Mode)
	{
		const UEnum* Enum = StaticEnum<EDesignPreviewSizeMode>();
		if (!Enum)
		{
			return TEXT("FillScreen");
		}
		return Enum->GetNameStringByValue(static_cast<int64>(Mode));
	}

	static bool LoadWidgetBlueprintAsset(const FString& WidgetPath, UWidgetBlueprint*& OutWidgetBlueprint)
	{
		OutWidgetBlueprint = nullptr;
		if (WidgetPath.IsEmpty())
		{
			return false;
		}

		auto TryLoadPath = [&OutWidgetBlueprint](const FString& CandidatePath) -> bool
		{
			if (CandidatePath.IsEmpty())
			{
				return false;
			}

			if (UObject* DirectObject = UEditorAssetLibrary::LoadAsset(CandidatePath))
			{
				OutWidgetBlueprint = Cast<UWidgetBlueprint>(DirectObject);
				if (OutWidgetBlueprint)
				{
					return true;
				}
			}

			FAssetData AssetData;
			if (UAssetDiscoveryService::FindAssetByPath(CandidatePath, AssetData))
			{
				OutWidgetBlueprint = Cast<UWidgetBlueprint>(UEditorAssetLibrary::LoadAsset(AssetData.GetObjectPathString()));
				if (OutWidgetBlueprint)
				{
					return true;
				}
			}

			return false;
		};

		if (TryLoadPath(WidgetPath))
		{
			return true;
		}

		if (!WidgetPath.Contains(TEXT(".")))
		{
			const FString AssetName = FPackageName::GetLongPackageAssetName(WidgetPath);
			if (!AssetName.IsEmpty() && TryLoadPath(WidgetPath + TEXT(".") + AssetName))
			{
				return true;
			}
		}

		return false;
	}

	static UWidget* FindWidgetByName(UWidgetBlueprint* WidgetBlueprint, const FString& ComponentName)
	{
		if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree || ComponentName.IsEmpty())
		{
			return nullptr;
		}

		TArray<UWidget*> AllWidgets;
		WidgetBlueprint->WidgetTree->GetAllWidgets(AllWidgets);
		for (UWidget* Widget : AllWidgets)
		{
			if (Widget && Widget->GetName().Equals(ComponentName, ESearchCase::IgnoreCase))
			{
				return Widget;
			}
		}

		return nullptr;
	}

	static UUserWidget* GetDefaultUserWidget(UWidgetBlueprint* WidgetBlueprint)
	{
		if (!WidgetBlueprint || !WidgetBlueprint->GeneratedClass)
		{
			return nullptr;
		}

		return WidgetBlueprint->GeneratedClass->GetDefaultObject<UUserWidget>();
	}

	static bool IsWidgetBlueprintEditorInstance(IAssetEditorInstance* EditorInstance)
	{
		return EditorInstance && EditorInstance->GetEditorName() == TEXT("WidgetBlueprintEditor");
	}

	static FWidgetBlueprintEditor* GetWidgetBlueprintEditor(UWidgetBlueprint* WidgetBlueprint, bool bOpenIfNeeded)
	{
		if (!WidgetBlueprint || !GEditor)
		{
			return nullptr;
		}

		UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (!AssetEditorSubsystem)
		{
			return nullptr;
		}

		IAssetEditorInstance* EditorInstance = AssetEditorSubsystem->FindEditorForAsset(WidgetBlueprint, false);
		if (!EditorInstance && bOpenIfNeeded)
		{
			if (!AssetEditorSubsystem->OpenEditorForAsset(WidgetBlueprint))
			{
				return nullptr;
			}

			EditorInstance = AssetEditorSubsystem->FindEditorForAsset(WidgetBlueprint, false);
		}

		if (!IsWidgetBlueprintEditorInstance(EditorInstance))
		{
			return nullptr;
		}

		return static_cast<FWidgetBlueprintEditor*>(EditorInstance);
	}

	static TSharedPtr<SDesignerView> GetDesignerView(FWidgetBlueprintEditor* WidgetEditor, bool bSwitchToDesigner)
	{
		if (!WidgetEditor)
		{
			return nullptr;
		}

		if (bSwitchToDesigner)
		{
			WidgetEditor->SetCurrentMode(FWidgetBlueprintApplicationModes::DesignerMode);
		}

		const TSharedPtr<FTabManager> TabManager = WidgetEditor->GetTabManager();
		if (!TabManager.IsValid())
		{
			return nullptr;
		}

		TSharedPtr<SDockTab> DesignerTab = TabManager->FindExistingLiveTab(FTabId(DesignerTabId));
		if (!DesignerTab.IsValid())
		{
			DesignerTab = TabManager->TryInvokeTab(FTabId(DesignerTabId));
		}

		if (!DesignerTab.IsValid())
		{
			return nullptr;
		}

		return StaticCastSharedRef<SDesignerView>(DesignerTab->GetContent());
	}

	static void PopulateSelectionInfo(
		FWidgetBlueprintEditor* WidgetEditor,
		bool bSwitchToDesigner,
		FWidgetDesignerSelectionInfo& OutInfo)
	{
		if (!WidgetEditor)
		{
			return;
		}

		OutInfo.bEditorOpen = true;
		OutInfo.bDesignerReady = GetDesignerView(WidgetEditor, bSwitchToDesigner).IsValid();

		const TSet<FWidgetReference>& SelectedWidgets = WidgetEditor->GetSelectedWidgets();
		for (const FWidgetReference& WidgetRef : SelectedWidgets)
		{
			if (UWidget* TemplateWidget = WidgetRef.GetTemplate())
			{
				OutInfo.SelectedComponents.Add(TemplateWidget->GetName());
			}
		}

		OutInfo.SelectedComponents.Sort();
		OutInfo.SelectedCount = OutInfo.SelectedComponents.Num();
		if (OutInfo.SelectedCount > 0)
		{
			OutInfo.PrimaryComponent = OutInfo.SelectedComponents[0];
		}

		const TOptional<FNamedSlotSelection> NamedSlotSelection = WidgetEditor->GetSelectedNamedSlot();
		if (NamedSlotSelection.IsSet())
		{
			OutInfo.bHasNamedSlotSelection = true;
			OutInfo.NamedSlotName = NamedSlotSelection->SlotName.ToString();
			if (UWidget* HostWidget = NamedSlotSelection->NamedSlotHostWidget.GetTemplate())
			{
				OutInfo.NamedSlotHostComponent = HostWidget->GetName();
			}
		}
	}

	static int32 AddComponentReferencesToSelection(
		UWidgetBlueprint* WidgetBlueprint,
		FWidgetBlueprintEditor* WidgetEditor,
		const TArray<FString>& ComponentNames,
		TSet<FWidgetReference>& InOutSelection)
	{
		int32 AddedCount = 0;
		for (const FString& ComponentName : ComponentNames)
		{
			if (UWidget* Widget = FindWidgetByName(WidgetBlueprint, ComponentName))
			{
				const FWidgetReference WidgetRef = WidgetEditor->GetReferenceFromTemplate(Widget);
				if (WidgetRef.IsValid() && !InOutSelection.Contains(WidgetRef))
				{
					InOutSelection.Add(WidgetRef);
					++AddedCount;
				}
			}
		}

		return AddedCount;
	}

	static bool ReadRawSlotProperties(UPanelSlot* Slot, TMap<FString, FString>& OutProperties)
	{
		if (!Slot)
		{
			return false;
		}

		TMap<FName, FString> ExportedProperties;
		FWidgetBlueprintEditorUtils::ExportPropertiesToText(Slot, ExportedProperties);
		for (const TPair<FName, FString>& Entry : ExportedProperties)
		{
			OutProperties.Add(Entry.Key.ToString(), Entry.Value);
		}

		return true;
	}

	static void AddSupportedField(FWidgetDesignerSlotLayout& Layout, const TCHAR* FieldName)
	{
		const FString Field = FieldName;
		if (!Layout.SupportedFields.Contains(Field))
		{
			Layout.SupportedFields.Add(Field);
		}
	}

	static void PopulateCommonAlignmentFields(UPanelSlot* Slot, FWidgetDesignerSlotLayout& Layout)
	{
		if (UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(Slot))
		{
			Layout.Padding = OverlaySlot->GetPadding();
			Layout.HorizontalAlignment = EnumToShortString(OverlaySlot->GetHorizontalAlignment());
			Layout.VerticalAlignment = EnumToShortString(OverlaySlot->GetVerticalAlignment());
			AddSupportedField(Layout, TEXT("padding"));
			AddSupportedField(Layout, TEXT("horizontal_alignment"));
			AddSupportedField(Layout, TEXT("vertical_alignment"));
			return;
		}

		if (UHorizontalBoxSlot* HorizontalBoxSlot = Cast<UHorizontalBoxSlot>(Slot))
		{
			Layout.Padding = HorizontalBoxSlot->GetPadding();
			Layout.HorizontalAlignment = EnumToShortString(HorizontalBoxSlot->GetHorizontalAlignment());
			Layout.VerticalAlignment = EnumToShortString(HorizontalBoxSlot->GetVerticalAlignment());
			Layout.SizeRule = SizeRuleToString(HorizontalBoxSlot->GetSize().SizeRule);
			Layout.SizeValue = HorizontalBoxSlot->GetSize().Value;
			AddSupportedField(Layout, TEXT("padding"));
			AddSupportedField(Layout, TEXT("horizontal_alignment"));
			AddSupportedField(Layout, TEXT("vertical_alignment"));
			AddSupportedField(Layout, TEXT("size_rule"));
			AddSupportedField(Layout, TEXT("size_value"));
			return;
		}

		if (UVerticalBoxSlot* VerticalBoxSlot = Cast<UVerticalBoxSlot>(Slot))
		{
			Layout.Padding = VerticalBoxSlot->GetPadding();
			Layout.HorizontalAlignment = EnumToShortString(VerticalBoxSlot->GetHorizontalAlignment());
			Layout.VerticalAlignment = EnumToShortString(VerticalBoxSlot->GetVerticalAlignment());
			Layout.SizeRule = SizeRuleToString(VerticalBoxSlot->GetSize().SizeRule);
			Layout.SizeValue = VerticalBoxSlot->GetSize().Value;
			AddSupportedField(Layout, TEXT("padding"));
			AddSupportedField(Layout, TEXT("horizontal_alignment"));
			AddSupportedField(Layout, TEXT("vertical_alignment"));
			AddSupportedField(Layout, TEXT("size_rule"));
			AddSupportedField(Layout, TEXT("size_value"));
			return;
		}

		if (UGridSlot* GridSlot = Cast<UGridSlot>(Slot))
		{
			Layout.Padding = GridSlot->GetPadding();
			Layout.HorizontalAlignment = EnumToShortString(GridSlot->GetHorizontalAlignment());
			Layout.VerticalAlignment = EnumToShortString(GridSlot->GetVerticalAlignment());
			Layout.Row = GridSlot->GetRow();
			Layout.RowSpan = GridSlot->GetRowSpan();
			Layout.Column = GridSlot->GetColumn();
			Layout.ColumnSpan = GridSlot->GetColumnSpan();
			Layout.Layer = GridSlot->GetLayer();
			Layout.Nudge = GridSlot->GetNudge();
			AddSupportedField(Layout, TEXT("padding"));
			AddSupportedField(Layout, TEXT("horizontal_alignment"));
			AddSupportedField(Layout, TEXT("vertical_alignment"));
			AddSupportedField(Layout, TEXT("row"));
			AddSupportedField(Layout, TEXT("row_span"));
			AddSupportedField(Layout, TEXT("column"));
			AddSupportedField(Layout, TEXT("column_span"));
			AddSupportedField(Layout, TEXT("layer"));
			AddSupportedField(Layout, TEXT("nudge"));
			return;
		}

		if (UUniformGridSlot* UniformGridSlot = Cast<UUniformGridSlot>(Slot))
		{
			Layout.HorizontalAlignment = EnumToShortString(UniformGridSlot->GetHorizontalAlignment());
			Layout.VerticalAlignment = EnumToShortString(UniformGridSlot->GetVerticalAlignment());
			Layout.Row = UniformGridSlot->GetRow();
			Layout.Column = UniformGridSlot->GetColumn();
			AddSupportedField(Layout, TEXT("horizontal_alignment"));
			AddSupportedField(Layout, TEXT("vertical_alignment"));
			AddSupportedField(Layout, TEXT("row"));
			AddSupportedField(Layout, TEXT("column"));
		}
	}

	static void PopulateCanvasFields(UPanelSlot* Slot, FWidgetDesignerSlotLayout& Layout)
	{
		if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Slot))
		{
			Layout.Anchors = CanvasSlot->GetAnchors();
			Layout.Offsets = CanvasSlot->GetOffsets();
			Layout.Alignment = CanvasSlot->GetAlignment();
			Layout.bAutoSize = CanvasSlot->GetAutoSize();
			Layout.ZOrder = CanvasSlot->GetZOrder();
			AddSupportedField(Layout, TEXT("anchors"));
			AddSupportedField(Layout, TEXT("offsets"));
			AddSupportedField(Layout, TEXT("alignment"));
			AddSupportedField(Layout, TEXT("auto_size"));
			AddSupportedField(Layout, TEXT("z_order"));
		}
	}

	static bool ShouldApplyField(const TSet<FString>& FieldsToApply, const TCHAR* FieldName)
	{
		return FieldsToApply.Num() == 0 || FieldsToApply.Contains(NormalizeFieldName(FieldName));
	}

	static FString BoolToPropertyText(const bool bValue)
	{
		return bValue ? TEXT("True") : TEXT("False");
	}

	static FString MarginToPropertyText(const FMargin& Margin)
	{
		return FString::Printf(
			TEXT("(Left=%0.6f,Top=%0.6f,Right=%0.6f,Bottom=%0.6f)"),
			Margin.Left,
			Margin.Top,
			Margin.Right,
			Margin.Bottom);
	}

	static FString Vector2DToPropertyText(const FVector2D& Value)
	{
		return FString::Printf(TEXT("(X=%0.6f,Y=%0.6f)"), Value.X, Value.Y);
	}

	static FString AnchorsToPropertyText(const FAnchors& Anchors)
	{
		return FString::Printf(
			TEXT("(Minimum=(X=%0.6f,Y=%0.6f),Maximum=(X=%0.6f,Y=%0.6f))"),
			Anchors.Minimum.X,
			Anchors.Minimum.Y,
			Anchors.Maximum.X,
			Anchors.Maximum.Y);
	}

	static FString LayoutDataToPropertyText(const FWidgetDesignerSlotLayout& Layout)
	{
		return FString::Printf(
			TEXT("(Offsets=%s,Anchors=%s,Alignment=%s)"),
			*MarginToPropertyText(Layout.Offsets),
			*AnchorsToPropertyText(Layout.Anchors),
			*Vector2DToPropertyText(Layout.Alignment));
	}

	static FString HorizontalAlignmentToPropertyText(const FString& Value)
	{
		const FString Normalized = NormalizeFieldName(Value);
		if (Normalized == TEXT("left"))
		{
			return TEXT("HAlign_Left");
		}
		if (Normalized == TEXT("center"))
		{
			return TEXT("HAlign_Center");
		}
		if (Normalized == TEXT("right"))
		{
			return TEXT("HAlign_Right");
		}
		return TEXT("HAlign_Fill");
	}

	static FString VerticalAlignmentToPropertyText(const FString& Value)
	{
		const FString Normalized = NormalizeFieldName(Value);
		if (Normalized == TEXT("top"))
		{
			return TEXT("VAlign_Top");
		}
		if (Normalized == TEXT("center"))
		{
			return TEXT("VAlign_Center");
		}
		if (Normalized == TEXT("bottom"))
		{
			return TEXT("VAlign_Bottom");
		}
		return TEXT("VAlign_Fill");
	}

	static FString SizeToPropertyText(const FWidgetDesignerSlotLayout& Layout)
	{
		const FString Rule = NormalizeFieldName(Layout.SizeRule) == TEXT("fill") ? TEXT("Fill") : TEXT("Automatic");
		return FString::Printf(TEXT("(Value=%0.6f,SizeRule=%s)"), Layout.SizeValue, *Rule);
	}

	static void SyncStructuredFieldsToRawProperties(
		UPanelSlot* Slot,
		const FWidgetDesignerSlotLayout& Layout,
		const TSet<FString>& FieldsToApply,
		TMap<FString, FString>& InOutRawProperties)
	{
		if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Slot))
		{
			if (ShouldApplyField(FieldsToApply, TEXT("anchors")) ||
				ShouldApplyField(FieldsToApply, TEXT("offsets")) ||
				ShouldApplyField(FieldsToApply, TEXT("alignment")))
			{
				InOutRawProperties.Add(TEXT("LayoutData"), LayoutDataToPropertyText(Layout));
			}
			if (ShouldApplyField(FieldsToApply, TEXT("auto_size")))
			{
				InOutRawProperties.Add(TEXT("bAutoSize"), BoolToPropertyText(Layout.bAutoSize));
			}
			if (ShouldApplyField(FieldsToApply, TEXT("z_order")))
			{
				InOutRawProperties.Add(TEXT("ZOrder"), FString::FromInt(Layout.ZOrder));
			}
			return;
		}

		if (UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(Slot))
		{
			if (ShouldApplyField(FieldsToApply, TEXT("padding")))
			{
				InOutRawProperties.Add(TEXT("Padding"), MarginToPropertyText(Layout.Padding));
			}
			if (ShouldApplyField(FieldsToApply, TEXT("horizontal_alignment")))
			{
				InOutRawProperties.Add(TEXT("HorizontalAlignment"), HorizontalAlignmentToPropertyText(Layout.HorizontalAlignment));
			}
			if (ShouldApplyField(FieldsToApply, TEXT("vertical_alignment")))
			{
				InOutRawProperties.Add(TEXT("VerticalAlignment"), VerticalAlignmentToPropertyText(Layout.VerticalAlignment));
			}
			return;
		}

		if (UHorizontalBoxSlot* HorizontalBoxSlot = Cast<UHorizontalBoxSlot>(Slot))
		{
			if (ShouldApplyField(FieldsToApply, TEXT("padding")))
			{
				InOutRawProperties.Add(TEXT("Padding"), MarginToPropertyText(Layout.Padding));
			}
			if (ShouldApplyField(FieldsToApply, TEXT("horizontal_alignment")))
			{
				InOutRawProperties.Add(TEXT("HorizontalAlignment"), HorizontalAlignmentToPropertyText(Layout.HorizontalAlignment));
			}
			if (ShouldApplyField(FieldsToApply, TEXT("vertical_alignment")))
			{
				InOutRawProperties.Add(TEXT("VerticalAlignment"), VerticalAlignmentToPropertyText(Layout.VerticalAlignment));
			}
			if (ShouldApplyField(FieldsToApply, TEXT("size_rule")) || ShouldApplyField(FieldsToApply, TEXT("size_value")))
			{
				InOutRawProperties.Add(TEXT("Size"), SizeToPropertyText(Layout));
			}
			return;
		}

		if (UVerticalBoxSlot* VerticalBoxSlot = Cast<UVerticalBoxSlot>(Slot))
		{
			if (ShouldApplyField(FieldsToApply, TEXT("padding")))
			{
				InOutRawProperties.Add(TEXT("Padding"), MarginToPropertyText(Layout.Padding));
			}
			if (ShouldApplyField(FieldsToApply, TEXT("horizontal_alignment")))
			{
				InOutRawProperties.Add(TEXT("HorizontalAlignment"), HorizontalAlignmentToPropertyText(Layout.HorizontalAlignment));
			}
			if (ShouldApplyField(FieldsToApply, TEXT("vertical_alignment")))
			{
				InOutRawProperties.Add(TEXT("VerticalAlignment"), VerticalAlignmentToPropertyText(Layout.VerticalAlignment));
			}
			if (ShouldApplyField(FieldsToApply, TEXT("size_rule")) || ShouldApplyField(FieldsToApply, TEXT("size_value")))
			{
				InOutRawProperties.Add(TEXT("Size"), SizeToPropertyText(Layout));
			}
			return;
		}

		if (UGridSlot* GridSlot = Cast<UGridSlot>(Slot))
		{
			if (ShouldApplyField(FieldsToApply, TEXT("padding")))
			{
				InOutRawProperties.Add(TEXT("Padding"), MarginToPropertyText(Layout.Padding));
			}
			if (ShouldApplyField(FieldsToApply, TEXT("horizontal_alignment")))
			{
				InOutRawProperties.Add(TEXT("HorizontalAlignment"), HorizontalAlignmentToPropertyText(Layout.HorizontalAlignment));
			}
			if (ShouldApplyField(FieldsToApply, TEXT("vertical_alignment")))
			{
				InOutRawProperties.Add(TEXT("VerticalAlignment"), VerticalAlignmentToPropertyText(Layout.VerticalAlignment));
			}
			if (ShouldApplyField(FieldsToApply, TEXT("row")))
			{
				InOutRawProperties.Add(TEXT("Row"), FString::FromInt(Layout.Row));
			}
			if (ShouldApplyField(FieldsToApply, TEXT("row_span")))
			{
				InOutRawProperties.Add(TEXT("RowSpan"), FString::FromInt(Layout.RowSpan));
			}
			if (ShouldApplyField(FieldsToApply, TEXT("column")))
			{
				InOutRawProperties.Add(TEXT("Column"), FString::FromInt(Layout.Column));
			}
			if (ShouldApplyField(FieldsToApply, TEXT("column_span")))
			{
				InOutRawProperties.Add(TEXT("ColumnSpan"), FString::FromInt(Layout.ColumnSpan));
			}
			if (ShouldApplyField(FieldsToApply, TEXT("layer")))
			{
				InOutRawProperties.Add(TEXT("Layer"), FString::FromInt(Layout.Layer));
			}
			if (ShouldApplyField(FieldsToApply, TEXT("nudge")))
			{
				InOutRawProperties.Add(TEXT("Nudge"), Vector2DToPropertyText(Layout.Nudge));
			}
			return;
		}

		if (UUniformGridSlot* UniformGridSlot = Cast<UUniformGridSlot>(Slot))
		{
			if (ShouldApplyField(FieldsToApply, TEXT("horizontal_alignment")))
			{
				InOutRawProperties.Add(TEXT("HorizontalAlignment"), HorizontalAlignmentToPropertyText(Layout.HorizontalAlignment));
			}
			if (ShouldApplyField(FieldsToApply, TEXT("vertical_alignment")))
			{
				InOutRawProperties.Add(TEXT("VerticalAlignment"), VerticalAlignmentToPropertyText(Layout.VerticalAlignment));
			}
			if (ShouldApplyField(FieldsToApply, TEXT("row")))
			{
				InOutRawProperties.Add(TEXT("Row"), FString::FromInt(Layout.Row));
			}
			if (ShouldApplyField(FieldsToApply, TEXT("column")))
			{
				InOutRawProperties.Add(TEXT("Column"), FString::FromInt(Layout.Column));
			}
		}
	}

	static void RefreshWidgetEditor(UWidgetBlueprint* WidgetBlueprint)
	{
		if (!WidgetBlueprint)
		{
			return;
		}

		if (!GetWidgetBlueprintEditor(WidgetBlueprint, false))
		{
			return;
		}

		const FString WidgetBlueprintPath = WidgetBlueprint->GetPathName();
		if (PendingWidgetPreviewRefreshes.Contains(WidgetBlueprintPath))
		{
			return;
		}

		PendingWidgetPreviewRefreshes.Add(WidgetBlueprintPath);

		const TWeakObjectPtr<UWidgetBlueprint> WeakWidgetBlueprint(WidgetBlueprint);
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([WeakWidgetBlueprint, WidgetBlueprintPath](float)
			{
				PendingWidgetPreviewRefreshes.Remove(WidgetBlueprintPath);

				if (UWidgetBlueprint* ResolvedWidgetBlueprint = WeakWidgetBlueprint.Get())
				{
					if (FWidgetBlueprintEditor* WidgetEditor = GetWidgetBlueprintEditor(ResolvedWidgetBlueprint, false))
					{
						WidgetEditor->RefreshPreview();
					}
				}

				return false;
			}),
			0.0f);
	}
}

FWidgetDesignerPreviewInfo UWidgetDesignerService::GetPreviewInfo(
	const FString& WidgetPath,
	bool bOpenIfNeeded,
	bool bSwitchToDesigner)
{
	FWidgetDesignerPreviewInfo Result;
	Result.WidgetPath = WidgetPath;

	UWidgetBlueprint* WidgetBlueprint = nullptr;
	if (!LoadWidgetBlueprintAsset(WidgetPath, WidgetBlueprint))
	{
		return Result;
	}

	Result.bWidgetBlueprintLoaded = true;

	if (UUserWidget* DefaultWidget = GetDefaultUserWidget(WidgetBlueprint))
	{
		Result.DesignSizeMode = DesignSizeModeToString(DefaultWidget->DesignSizeMode);
		Result.DesignTimeSize = DefaultWidget->DesignTimeSize;
	}

	FWidgetBlueprintEditor* WidgetEditor = GetWidgetBlueprintEditor(WidgetBlueprint, bOpenIfNeeded);
	Result.bEditorOpen = WidgetEditor != nullptr;
	if (!WidgetEditor)
	{
		return Result;
	}

	const TSharedPtr<SDesignerView> DesignerView = GetDesignerView(WidgetEditor, bSwitchToDesigner);
	Result.bDesignerReady = DesignerView.IsValid();
	if (!DesignerView.IsValid())
	{
		return Result;
	}

	Result.PreviewAreaWidth = FMath::RoundToInt(DesignerView->GetPreviewAreaWidth().Get());
	Result.PreviewAreaHeight = FMath::RoundToInt(DesignerView->GetPreviewAreaHeight().Get());
	Result.PreviewWidgetWidth = FMath::RoundToInt(DesignerView->GetPreviewSizeWidth().Get());
	Result.PreviewWidgetHeight = FMath::RoundToInt(DesignerView->GetPreviewSizeHeight().Get());
	Result.PreviewDPIScale = DesignerView->GetPreviewDPIScale();

	return Result;
}

bool UWidgetDesignerService::SetPreviewArea(
	const FString& WidgetPath,
	int32 PreviewWidth,
	int32 PreviewHeight,
	bool bOpenIfNeeded,
	bool bSwitchToDesigner)
{
	UWidgetBlueprint* WidgetBlueprint = nullptr;
	if (!LoadWidgetBlueprintAsset(WidgetPath, WidgetBlueprint))
	{
		return false;
	}

	FWidgetBlueprintEditor* WidgetEditor = GetWidgetBlueprintEditor(WidgetBlueprint, bOpenIfNeeded);
	if (!WidgetEditor)
	{
		return false;
	}

	const TSharedPtr<SDesignerView> DesignerView = GetDesignerView(WidgetEditor, bSwitchToDesigner);
	if (!DesignerView.IsValid())
	{
		return false;
	}

	UUserWidget* DefaultWidget = GetDefaultUserWidget(WidgetBlueprint);
	const bool bPersistedThroughAsset =
		DefaultWidget &&
		(DefaultWidget->DesignSizeMode == EDesignPreviewSizeMode::Custom ||
		 DefaultWidget->DesignSizeMode == EDesignPreviewSizeMode::CustomOnScreen);

	if (DefaultWidget)
	{
		DefaultWidget->Modify();
	}
	WidgetBlueprint->Modify();

	DesignerView->SetPreviewAreaSize(PreviewWidth, PreviewHeight);

	if (bPersistedThroughAsset)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBlueprint);
	}

	RefreshWidgetEditor(WidgetBlueprint);
	return true;
}

FWidgetDesignerPreviewInfo UWidgetDesignerService::GetDesignSettings(const FString& WidgetPath)
{
	FWidgetDesignerPreviewInfo Result;
	Result.WidgetPath = WidgetPath;

	UWidgetBlueprint* WidgetBlueprint = nullptr;
	if (!LoadWidgetBlueprintAsset(WidgetPath, WidgetBlueprint))
	{
		return Result;
	}

	Result.bWidgetBlueprintLoaded = true;
	Result.bEditorOpen = GetWidgetBlueprintEditor(WidgetBlueprint, false) != nullptr;

	if (UUserWidget* DefaultWidget = GetDefaultUserWidget(WidgetBlueprint))
	{
		Result.DesignSizeMode = DesignSizeModeToString(DefaultWidget->DesignSizeMode);
		Result.DesignTimeSize = DefaultWidget->DesignTimeSize;
	}

	return Result;
}

FWidgetDesignerSelectionInfo UWidgetDesignerService::GetSelectionInfo(
	const FString& WidgetPath,
	bool bOpenIfNeeded,
	bool bSwitchToDesigner)
{
	FWidgetDesignerSelectionInfo Result;
	Result.WidgetPath = WidgetPath;

	UWidgetBlueprint* WidgetBlueprint = nullptr;
	if (!LoadWidgetBlueprintAsset(WidgetPath, WidgetBlueprint))
	{
		return Result;
	}

	Result.bWidgetBlueprintLoaded = true;
	PopulateSelectionInfo(GetWidgetBlueprintEditor(WidgetBlueprint, bOpenIfNeeded), bSwitchToDesigner, Result);
	return Result;
}

bool UWidgetDesignerService::SelectComponents(
	const FString& WidgetPath,
	const TArray<FString>& ComponentNames,
	bool bAppendToSelection,
	bool bOpenIfNeeded,
	bool bSwitchToDesigner)
{
	UWidgetBlueprint* WidgetBlueprint = nullptr;
	if (!LoadWidgetBlueprintAsset(WidgetPath, WidgetBlueprint))
	{
		return false;
	}

	FWidgetBlueprintEditor* WidgetEditor = GetWidgetBlueprintEditor(WidgetBlueprint, bOpenIfNeeded);
	if (!WidgetEditor)
	{
		return false;
	}

	if (bSwitchToDesigner)
	{
		GetDesignerView(WidgetEditor, true);
	}

	TSet<FWidgetReference> NewSelection;
	if (bAppendToSelection)
	{
		NewSelection = WidgetEditor->GetSelectedWidgets();
	}

	const int32 AddedCount = AddComponentReferencesToSelection(WidgetBlueprint, WidgetEditor, ComponentNames, NewSelection);
	if (AddedCount == 0)
	{
		return false;
	}

	WidgetEditor->SelectWidgets(NewSelection, false);
	return true;
}

bool UWidgetDesignerService::ClearSelection(
	const FString& WidgetPath,
	bool bOpenIfNeeded,
	bool bSwitchToDesigner)
{
	UWidgetBlueprint* WidgetBlueprint = nullptr;
	if (!LoadWidgetBlueprintAsset(WidgetPath, WidgetBlueprint))
	{
		return false;
	}

	FWidgetBlueprintEditor* WidgetEditor = GetWidgetBlueprintEditor(WidgetBlueprint, bOpenIfNeeded);
	if (!WidgetEditor)
	{
		return false;
	}

	if (bSwitchToDesigner)
	{
		GetDesignerView(WidgetEditor, true);
	}

	WidgetEditor->SelectWidgets(TSet<FWidgetReference>(), false);
	return true;
}

bool UWidgetDesignerService::SetDesignSettings(
	const FString& WidgetPath,
	const FString& SizeMode,
	FVector2D CustomSize)
{
	UWidgetBlueprint* WidgetBlueprint = nullptr;
	if (!LoadWidgetBlueprintAsset(WidgetPath, WidgetBlueprint))
	{
		return false;
	}

	UUserWidget* DefaultWidget = GetDefaultUserWidget(WidgetBlueprint);
	if (!DefaultWidget)
	{
		return false;
	}

	EDesignPreviewSizeMode ParsedMode = EDesignPreviewSizeMode::FillScreen;
	if (!ParseDesignSizeMode(SizeMode, ParsedMode))
	{
		return false;
	}

	DefaultWidget->Modify();
	WidgetBlueprint->Modify();

	DefaultWidget->DesignSizeMode = ParsedMode;
	if (ParsedMode == EDesignPreviewSizeMode::Custom ||
		ParsedMode == EDesignPreviewSizeMode::CustomOnScreen ||
		ParsedMode == EDesignPreviewSizeMode::Desired ||
		ParsedMode == EDesignPreviewSizeMode::DesiredOnScreen)
	{
		DefaultWidget->DesignTimeSize = FVector2D(
			FMath::Max(CustomSize.X, 1.0f),
			FMath::Max(CustomSize.Y, 1.0f));
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBlueprint);
	RefreshWidgetEditor(WidgetBlueprint);
	return true;
}

FWidgetDesignerSlotLayout UWidgetDesignerService::GetSlotLayout(
	const FString& WidgetPath,
	const FString& ComponentName)
{
	FWidgetDesignerSlotLayout Result;
	Result.WidgetPath = WidgetPath;
	Result.ComponentName = ComponentName;

	UWidgetBlueprint* WidgetBlueprint = nullptr;
	if (!LoadWidgetBlueprintAsset(WidgetPath, WidgetBlueprint))
	{
		return Result;
	}

	UWidget* Widget = FindWidgetByName(WidgetBlueprint, ComponentName);
	if (!Widget || !Widget->Slot)
	{
		return Result;
	}

	UPanelSlot* Slot = Widget->Slot;
	Result.bHasSlot = true;
	Result.SlotClass = Slot->GetClass()->GetName();
	Result.ParentWidget = Slot->Parent ? Slot->Parent->GetName() : FString();

	PopulateCanvasFields(Slot, Result);
	PopulateCommonAlignmentFields(Slot, Result);
	ReadRawSlotProperties(Slot, Result.RawProperties);

	return Result;
}

bool UWidgetDesignerService::SetSlotLayout(
	const FString& WidgetPath,
	const FString& ComponentName,
	const FWidgetDesignerSlotLayout& SlotLayout)
{
	UWidgetBlueprint* WidgetBlueprint = nullptr;
	if (!LoadWidgetBlueprintAsset(WidgetPath, WidgetBlueprint))
	{
		return false;
	}

	UWidget* Widget = FindWidgetByName(WidgetBlueprint, ComponentName);
	if (!Widget || !Widget->Slot)
	{
		return false;
	}

	UPanelSlot* Slot = Widget->Slot;
	Slot->Modify();
	WidgetBlueprint->Modify();

	TSet<FString> FieldsToApply;
	for (const FString& FieldName : SlotLayout.FieldsToApply)
	{
		FieldsToApply.Add(NormalizeFieldName(FieldName));
	}

	if (ShouldApplyField(FieldsToApply, TEXT("anchors")))
	{
		if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Slot))
		{
			CanvasSlot->SetAnchors(SlotLayout.Anchors);
		}
	}

	if (ShouldApplyField(FieldsToApply, TEXT("offsets")))
	{
		if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Slot))
		{
			CanvasSlot->SetOffsets(SlotLayout.Offsets);
		}
	}

	if (ShouldApplyField(FieldsToApply, TEXT("alignment")))
	{
		if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Slot))
		{
			CanvasSlot->SetAlignment(SlotLayout.Alignment);
		}
	}

	if (ShouldApplyField(FieldsToApply, TEXT("auto_size")))
	{
		if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Slot))
		{
			CanvasSlot->SetAutoSize(SlotLayout.bAutoSize);
		}
	}

	if (ShouldApplyField(FieldsToApply, TEXT("z_order")))
	{
		if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Slot))
		{
			CanvasSlot->SetZOrder(SlotLayout.ZOrder);
		}
	}

	if (ShouldApplyField(FieldsToApply, TEXT("padding")))
	{
		if (UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(Slot))
		{
			OverlaySlot->SetPadding(SlotLayout.Padding);
		}
		else if (UHorizontalBoxSlot* HorizontalBoxSlot = Cast<UHorizontalBoxSlot>(Slot))
		{
			HorizontalBoxSlot->SetPadding(SlotLayout.Padding);
		}
		else if (UVerticalBoxSlot* VerticalBoxSlot = Cast<UVerticalBoxSlot>(Slot))
		{
			VerticalBoxSlot->SetPadding(SlotLayout.Padding);
		}
		else if (UGridSlot* GridSlot = Cast<UGridSlot>(Slot))
		{
			GridSlot->SetPadding(SlotLayout.Padding);
		}
	}

	if (ShouldApplyField(FieldsToApply, TEXT("horizontal_alignment")))
	{
		EHorizontalAlignment HorizontalAlignment = HAlign_Fill;
		if (ParseHorizontalAlignment(SlotLayout.HorizontalAlignment, HorizontalAlignment))
		{
			if (UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(Slot))
			{
				OverlaySlot->SetHorizontalAlignment(HorizontalAlignment);
			}
			else if (UHorizontalBoxSlot* HorizontalBoxSlot = Cast<UHorizontalBoxSlot>(Slot))
			{
				HorizontalBoxSlot->SetHorizontalAlignment(HorizontalAlignment);
			}
			else if (UVerticalBoxSlot* VerticalBoxSlot = Cast<UVerticalBoxSlot>(Slot))
			{
				VerticalBoxSlot->SetHorizontalAlignment(HorizontalAlignment);
			}
			else if (UGridSlot* GridSlot = Cast<UGridSlot>(Slot))
			{
				GridSlot->SetHorizontalAlignment(HorizontalAlignment);
			}
			else if (UUniformGridSlot* UniformGridSlot = Cast<UUniformGridSlot>(Slot))
			{
				UniformGridSlot->SetHorizontalAlignment(HorizontalAlignment);
			}
		}
	}

	if (ShouldApplyField(FieldsToApply, TEXT("vertical_alignment")))
	{
		EVerticalAlignment VerticalAlignment = VAlign_Fill;
		if (ParseVerticalAlignment(SlotLayout.VerticalAlignment, VerticalAlignment))
		{
			if (UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(Slot))
			{
				OverlaySlot->SetVerticalAlignment(VerticalAlignment);
			}
			else if (UHorizontalBoxSlot* HorizontalBoxSlot = Cast<UHorizontalBoxSlot>(Slot))
			{
				HorizontalBoxSlot->SetVerticalAlignment(VerticalAlignment);
			}
			else if (UVerticalBoxSlot* VerticalBoxSlot = Cast<UVerticalBoxSlot>(Slot))
			{
				VerticalBoxSlot->SetVerticalAlignment(VerticalAlignment);
			}
			else if (UGridSlot* GridSlot = Cast<UGridSlot>(Slot))
			{
				GridSlot->SetVerticalAlignment(VerticalAlignment);
			}
			else if (UUniformGridSlot* UniformGridSlot = Cast<UUniformGridSlot>(Slot))
			{
				UniformGridSlot->SetVerticalAlignment(VerticalAlignment);
			}
		}
	}

	if (UHorizontalBoxSlot* HorizontalBoxSlot = Cast<UHorizontalBoxSlot>(Slot))
	{
		FSlateChildSize NewSize = HorizontalBoxSlot->GetSize();
		if (ShouldApplyField(FieldsToApply, TEXT("size_rule")))
		{
			ESlateSizeRule::Type ParsedSizeRule = ESlateSizeRule::Automatic;
			if (ParseSizeRule(SlotLayout.SizeRule, ParsedSizeRule))
			{
				NewSize.SizeRule = ParsedSizeRule;
			}
		}
		if (ShouldApplyField(FieldsToApply, TEXT("size_value")))
		{
			NewSize.Value = SlotLayout.SizeValue;
		}
		if (ShouldApplyField(FieldsToApply, TEXT("size_rule")) || ShouldApplyField(FieldsToApply, TEXT("size_value")))
		{
			HorizontalBoxSlot->SetSize(NewSize);
		}
	}
	else if (UVerticalBoxSlot* VerticalBoxSlot = Cast<UVerticalBoxSlot>(Slot))
	{
		FSlateChildSize NewSize = VerticalBoxSlot->GetSize();
		if (ShouldApplyField(FieldsToApply, TEXT("size_rule")))
		{
			ESlateSizeRule::Type ParsedSizeRule = ESlateSizeRule::Automatic;
			if (ParseSizeRule(SlotLayout.SizeRule, ParsedSizeRule))
			{
				NewSize.SizeRule = ParsedSizeRule;
			}
		}
		if (ShouldApplyField(FieldsToApply, TEXT("size_value")))
		{
			NewSize.Value = SlotLayout.SizeValue;
		}
		if (ShouldApplyField(FieldsToApply, TEXT("size_rule")) || ShouldApplyField(FieldsToApply, TEXT("size_value")))
		{
			VerticalBoxSlot->SetSize(NewSize);
		}
	}

	if (UGridSlot* GridSlot = Cast<UGridSlot>(Slot))
	{
		if (ShouldApplyField(FieldsToApply, TEXT("row")))
		{
			GridSlot->SetRow(SlotLayout.Row);
		}
		if (ShouldApplyField(FieldsToApply, TEXT("row_span")))
		{
			GridSlot->SetRowSpan(SlotLayout.RowSpan);
		}
		if (ShouldApplyField(FieldsToApply, TEXT("column")))
		{
			GridSlot->SetColumn(SlotLayout.Column);
		}
		if (ShouldApplyField(FieldsToApply, TEXT("column_span")))
		{
			GridSlot->SetColumnSpan(SlotLayout.ColumnSpan);
		}
		if (ShouldApplyField(FieldsToApply, TEXT("layer")))
		{
			GridSlot->SetLayer(SlotLayout.Layer);
		}
		if (ShouldApplyField(FieldsToApply, TEXT("nudge")))
		{
			GridSlot->SetNudge(SlotLayout.Nudge);
		}
	}

	if (UUniformGridSlot* UniformGridSlot = Cast<UUniformGridSlot>(Slot))
	{
		if (ShouldApplyField(FieldsToApply, TEXT("row")))
		{
			UniformGridSlot->SetRow(SlotLayout.Row);
		}
		if (ShouldApplyField(FieldsToApply, TEXT("column")))
		{
			UniformGridSlot->SetColumn(SlotLayout.Column);
		}
	}

	if (SlotLayout.RawProperties.Num() > 0)
	{
		TMap<FString, FString> RawPropertiesToImport = SlotLayout.RawProperties;
		SyncStructuredFieldsToRawProperties(Slot, SlotLayout, FieldsToApply, RawPropertiesToImport);

		TMap<FName, FString> ImportedProperties;
		for (const TPair<FString, FString>& Entry : RawPropertiesToImport)
		{
			ImportedProperties.Add(FName(*Entry.Key), Entry.Value);
		}
		FWidgetBlueprintEditorUtils::ImportPropertiesFromText(Slot, ImportedProperties);
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBlueprint);
	RefreshWidgetEditor(WidgetBlueprint);
	return true;
}
