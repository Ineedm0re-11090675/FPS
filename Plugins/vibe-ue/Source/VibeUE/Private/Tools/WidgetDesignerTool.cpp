// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Core/ToolRegistry.h"
#include "PythonAPI/UAssetDiscoveryService.h"
#include "PythonAPI/UWidgetDesignerService.h"

#include "AssetRegistry/AssetData.h"
#include "EditorAssetLibrary.h"
#include "Json.h"
#include "JsonObjectConverter.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "WidgetBlueprint.h"

namespace
{
	static bool TryGetDesignerJsonValue(const TMap<FString, FString>& Params, const FString& FieldName, TSharedPtr<FJsonValue>& OutValue)
	{
		const FString* ParamsJsonStr = Params.Find(TEXT("ParamsJson"));
		if (!ParamsJsonStr)
		{
			return false;
		}

		TSharedPtr<FJsonObject> JsonObj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(*ParamsJsonStr);
		if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
		{
			return false;
		}

		if (const TSharedPtr<FJsonValue>* JsonValue = JsonObj->Values.Find(FieldName))
		{
			OutValue = *JsonValue;
			return OutValue.IsValid();
		}

		FString CapitalizedField = FieldName;
		if (!CapitalizedField.IsEmpty())
		{
			CapitalizedField[0] = FChar::ToUpper(CapitalizedField[0]);
		}

		if (const TSharedPtr<FJsonValue>* JsonValue = JsonObj->Values.Find(CapitalizedField))
		{
			OutValue = *JsonValue;
			return OutValue.IsValid();
		}

		return false;
	}

	static FString ExtractDesignerParam(const TMap<FString, FString>& Params, const FString& FieldName)
	{
		if (const FString* DirectParam = Params.Find(FieldName))
		{
			return *DirectParam;
		}

		FString CapitalizedField = FieldName;
		if (!CapitalizedField.IsEmpty())
		{
			CapitalizedField[0] = FChar::ToUpper(CapitalizedField[0]);
		}

		if (const FString* CapitalizedParam = Params.Find(CapitalizedField))
		{
			return *CapitalizedParam;
		}

		TSharedPtr<FJsonValue> JsonValue;
		if (!TryGetDesignerJsonValue(Params, FieldName, JsonValue) || !JsonValue.IsValid())
		{
			return FString();
		}

		switch (JsonValue->Type)
		{
		case EJson::String:
			return JsonValue->AsString();
		case EJson::Number:
			return FString::Printf(TEXT("%.10g"), JsonValue->AsNumber());
		case EJson::Boolean:
			return JsonValue->AsBool() ? TEXT("true") : TEXT("false");
		case EJson::Object:
		{
			FString JsonString;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonString);
			FJsonSerializer::Serialize(JsonValue->AsObject().ToSharedRef(), Writer);
			return JsonString;
		}
		case EJson::Array:
		{
			FString JsonString;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonString);
			FJsonSerializer::Serialize(JsonValue->AsArray(), Writer);
			return JsonString;
		}
		default:
			return FString();
		}
	}

	static bool HasDesignerParam(const TMap<FString, FString>& Params, const FString& FieldName)
	{
		if (Params.Contains(FieldName))
		{
			return true;
		}

		FString CapitalizedField = FieldName;
		if (!CapitalizedField.IsEmpty())
		{
			CapitalizedField[0] = FChar::ToUpper(CapitalizedField[0]);
		}
		if (Params.Contains(CapitalizedField))
		{
			return true;
		}

		TSharedPtr<FJsonValue> JsonValue;
		return TryGetDesignerJsonValue(Params, FieldName, JsonValue);
	}

	static bool ExtractDesignerBoolParam(const TMap<FString, FString>& Params, const FString& FieldName, bool bDefaultValue)
	{
		const FString Value = ExtractDesignerParam(Params, FieldName);
		if (Value.IsEmpty())
		{
			return bDefaultValue;
		}

		return Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("1"));
	}

	static int32 ExtractDesignerIntParam(const TMap<FString, FString>& Params, const FString& FieldName, int32 DefaultValue)
	{
		const FString Value = ExtractDesignerParam(Params, FieldName);
		return Value.IsEmpty() ? DefaultValue : FCString::Atoi(*Value);
	}

	static float ExtractDesignerFloatParam(const TMap<FString, FString>& Params, const FString& FieldName, float DefaultValue)
	{
		const FString Value = ExtractDesignerParam(Params, FieldName);
		return Value.IsEmpty() ? DefaultValue : FCString::Atof(*Value);
	}

	static FString SerializeDesignerJson(const TSharedPtr<FJsonObject>& JsonObj)
	{
		FString JsonString;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
		FJsonSerializer::Serialize(JsonObj.ToSharedRef(), Writer);
		return JsonString;
	}

	static void AddStringArrayField(TSharedPtr<FJsonObject> JsonObj, const FString& FieldName, const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		JsonObj->SetArrayField(FieldName, JsonValues);
	}

	static TSharedPtr<FJsonObject> BuildDesignerError(
		const FString& Action,
		const FString& ErrorCode,
		const FString& ErrorMessage,
		const FString& WidgetPath = TEXT(""),
		const TArray<FString>& VerificationHints = {})
	{
		const TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
		ErrorObj->SetBoolField(TEXT("success"), false);
		ErrorObj->SetStringField(TEXT("action"), Action);
		ErrorObj->SetStringField(TEXT("error_code"), ErrorCode);
		ErrorObj->SetStringField(TEXT("error"), ErrorMessage);
		ErrorObj->SetStringField(TEXT("error_message"), ErrorMessage);
		if (!WidgetPath.IsEmpty())
		{
			ErrorObj->SetStringField(TEXT("widget_path"), WidgetPath);
		}
		AddStringArrayField(ErrorObj, TEXT("verification_hints"), VerificationHints);
		return ErrorObj;
	}

	static TSharedPtr<FJsonObject> BuildDesignerSuccess(
		const FString& Action,
		const FString& Summary,
		const FString& WidgetPath = TEXT(""),
		TSharedPtr<FJsonObject> Data = nullptr,
		const TArray<FString>& ChangedItems = {},
		const TArray<FString>& VerificationHints = {},
		const TArray<FString>& Warnings = {})
	{
		const TSharedPtr<FJsonObject> RootObj = MakeShared<FJsonObject>();
		RootObj->SetBoolField(TEXT("success"), true);
		RootObj->SetStringField(TEXT("action"), Action);
		RootObj->SetStringField(TEXT("summary"), Summary);
		if (!WidgetPath.IsEmpty())
		{
			RootObj->SetStringField(TEXT("widget_path"), WidgetPath);
		}
		if (Data.IsValid())
		{
			RootObj->SetObjectField(TEXT("data"), Data);
		}
		AddStringArrayField(RootObj, TEXT("changed_items"), ChangedItems);
		AddStringArrayField(RootObj, TEXT("verification_hints"), VerificationHints);
		AddStringArrayField(RootObj, TEXT("warnings"), Warnings);
		return RootObj;
	}

	static bool ParseVector2DString(const FString& InValue, FVector2D& OutValue)
	{
		TArray<FString> Parts;
		InValue.ParseIntoArray(Parts, TEXT(","), true);
		if (Parts.Num() != 2)
		{
			return false;
		}

		OutValue.X = FCString::Atof(*Parts[0]);
		OutValue.Y = FCString::Atof(*Parts[1]);
		return true;
	}

	static bool ParseMarginString(const FString& InValue, FMargin& OutValue)
	{
		TArray<FString> Parts;
		InValue.ParseIntoArray(Parts, TEXT(","), true);
		if (Parts.Num() != 4)
		{
			return false;
		}

		OutValue.Left = FCString::Atof(*Parts[0]);
		OutValue.Top = FCString::Atof(*Parts[1]);
		OutValue.Right = FCString::Atof(*Parts[2]);
		OutValue.Bottom = FCString::Atof(*Parts[3]);
		return true;
	}

	static bool ParseAnchorsString(const FString& InValue, FAnchors& OutAnchors)
	{
		TArray<FString> Parts;
		InValue.ParseIntoArray(Parts, TEXT(","), true);
		if (Parts.Num() != 4)
		{
			return false;
		}

		OutAnchors.Minimum.X = FCString::Atof(*Parts[0]);
		OutAnchors.Minimum.Y = FCString::Atof(*Parts[1]);
		OutAnchors.Maximum.X = FCString::Atof(*Parts[2]);
		OutAnchors.Maximum.Y = FCString::Atof(*Parts[3]);
		return true;
	}

	static FString NormalizeDesignerAction(const FString& RawAction)
	{
		const FString Action = RawAction.ToLower();
		if (Action == TEXT("get_preview_info"))
		{
			return TEXT("get_preview");
		}
		if (Action == TEXT("set_preview_area"))
		{
			return TEXT("set_preview");
		}
		if (Action == TEXT("get_design_info"))
		{
			return TEXT("get_design_settings");
		}
		if (Action == TEXT("get_selected_components"))
		{
			return TEXT("get_selection");
		}
		if (Action == TEXT("select_component"))
		{
			return TEXT("select_components");
		}
		if (Action == TEXT("clear_selected_components"))
		{
			return TEXT("clear_selection");
		}
		return Action;
	}

	static FString NormalizeDesignerToken(const FString& InValue)
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

	static bool IsExplicitDesignSizeModeToken(const FString& InValue)
	{
		const FString Token = NormalizeDesignerToken(InValue);
		return Token == TEXT("custom") ||
			Token == TEXT("customonscreen") ||
			Token == TEXT("desired") ||
			Token == TEXT("desiredonscreen");
	}

	static TSharedPtr<FJsonObject> PreviewInfoToJson(const FWidgetDesignerPreviewInfo& Info)
	{
		const TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
		JsonObj->SetStringField(TEXT("widget_path"), Info.WidgetPath);
		JsonObj->SetBoolField(TEXT("widget_blueprint_loaded"), Info.bWidgetBlueprintLoaded);
		JsonObj->SetBoolField(TEXT("editor_open"), Info.bEditorOpen);
		JsonObj->SetBoolField(TEXT("designer_ready"), Info.bDesignerReady);
		JsonObj->SetNumberField(TEXT("preview_area_width"), Info.PreviewAreaWidth);
		JsonObj->SetNumberField(TEXT("preview_area_height"), Info.PreviewAreaHeight);
		JsonObj->SetNumberField(TEXT("preview_widget_width"), Info.PreviewWidgetWidth);
		JsonObj->SetNumberField(TEXT("preview_widget_height"), Info.PreviewWidgetHeight);
		JsonObj->SetNumberField(TEXT("preview_dpi_scale"), Info.PreviewDPIScale);
		JsonObj->SetStringField(TEXT("design_size_mode"), Info.DesignSizeMode);
		JsonObj->SetObjectField(TEXT("design_time_size"), MakeShared<FJsonObject>());
		JsonObj->GetObjectField(TEXT("design_time_size"))->SetNumberField(TEXT("x"), Info.DesignTimeSize.X);
		JsonObj->GetObjectField(TEXT("design_time_size"))->SetNumberField(TEXT("y"), Info.DesignTimeSize.Y);
		return JsonObj;
	}

	static TSharedPtr<FJsonObject> SlotLayoutToJson(const FWidgetDesignerSlotLayout& Layout)
	{
		const TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
		JsonObj->SetBoolField(TEXT("has_slot"), Layout.bHasSlot);
		JsonObj->SetStringField(TEXT("widget_path"), Layout.WidgetPath);
		JsonObj->SetStringField(TEXT("component_name"), Layout.ComponentName);
		JsonObj->SetStringField(TEXT("parent_widget"), Layout.ParentWidget);
		JsonObj->SetStringField(TEXT("slot_class"), Layout.SlotClass);
		AddStringArrayField(JsonObj, TEXT("supported_fields"), Layout.SupportedFields);
		AddStringArrayField(JsonObj, TEXT("fields_to_apply"), Layout.FieldsToApply);

		const TSharedPtr<FJsonObject> PaddingObj = MakeShared<FJsonObject>();
		PaddingObj->SetNumberField(TEXT("left"), Layout.Padding.Left);
		PaddingObj->SetNumberField(TEXT("top"), Layout.Padding.Top);
		PaddingObj->SetNumberField(TEXT("right"), Layout.Padding.Right);
		PaddingObj->SetNumberField(TEXT("bottom"), Layout.Padding.Bottom);
		JsonObj->SetObjectField(TEXT("padding"), PaddingObj);

		JsonObj->SetStringField(TEXT("horizontal_alignment"), Layout.HorizontalAlignment);
		JsonObj->SetStringField(TEXT("vertical_alignment"), Layout.VerticalAlignment);

		const TSharedPtr<FJsonObject> AnchorsObj = MakeShared<FJsonObject>();
		AnchorsObj->SetNumberField(TEXT("minimum_x"), Layout.Anchors.Minimum.X);
		AnchorsObj->SetNumberField(TEXT("minimum_y"), Layout.Anchors.Minimum.Y);
		AnchorsObj->SetNumberField(TEXT("maximum_x"), Layout.Anchors.Maximum.X);
		AnchorsObj->SetNumberField(TEXT("maximum_y"), Layout.Anchors.Maximum.Y);
		JsonObj->SetObjectField(TEXT("anchors"), AnchorsObj);

		const TSharedPtr<FJsonObject> OffsetsObj = MakeShared<FJsonObject>();
		OffsetsObj->SetNumberField(TEXT("left"), Layout.Offsets.Left);
		OffsetsObj->SetNumberField(TEXT("top"), Layout.Offsets.Top);
		OffsetsObj->SetNumberField(TEXT("right"), Layout.Offsets.Right);
		OffsetsObj->SetNumberField(TEXT("bottom"), Layout.Offsets.Bottom);
		JsonObj->SetObjectField(TEXT("offsets"), OffsetsObj);

		const TSharedPtr<FJsonObject> AlignmentObj = MakeShared<FJsonObject>();
		AlignmentObj->SetNumberField(TEXT("x"), Layout.Alignment.X);
		AlignmentObj->SetNumberField(TEXT("y"), Layout.Alignment.Y);
		JsonObj->SetObjectField(TEXT("alignment"), AlignmentObj);

		JsonObj->SetBoolField(TEXT("auto_size"), Layout.bAutoSize);
		JsonObj->SetNumberField(TEXT("z_order"), Layout.ZOrder);
		JsonObj->SetStringField(TEXT("size_rule"), Layout.SizeRule);
		JsonObj->SetNumberField(TEXT("size_value"), Layout.SizeValue);
		JsonObj->SetNumberField(TEXT("row"), Layout.Row);
		JsonObj->SetNumberField(TEXT("column"), Layout.Column);
		JsonObj->SetNumberField(TEXT("row_span"), Layout.RowSpan);
		JsonObj->SetNumberField(TEXT("column_span"), Layout.ColumnSpan);
		JsonObj->SetNumberField(TEXT("layer"), Layout.Layer);

		const TSharedPtr<FJsonObject> NudgeObj = MakeShared<FJsonObject>();
		NudgeObj->SetNumberField(TEXT("x"), Layout.Nudge.X);
		NudgeObj->SetNumberField(TEXT("y"), Layout.Nudge.Y);
		JsonObj->SetObjectField(TEXT("nudge"), NudgeObj);

		const TSharedPtr<FJsonObject> RawPropertiesObj = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& Entry : Layout.RawProperties)
		{
			RawPropertiesObj->SetStringField(Entry.Key, Entry.Value);
		}
		JsonObj->SetObjectField(TEXT("raw_properties"), RawPropertiesObj);

		return JsonObj;
	}

	static TSharedPtr<FJsonObject> SelectionInfoToJson(const FWidgetDesignerSelectionInfo& Info)
	{
		const TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
		JsonObj->SetStringField(TEXT("widget_path"), Info.WidgetPath);
		JsonObj->SetBoolField(TEXT("widget_blueprint_loaded"), Info.bWidgetBlueprintLoaded);
		JsonObj->SetBoolField(TEXT("editor_open"), Info.bEditorOpen);
		JsonObj->SetBoolField(TEXT("designer_ready"), Info.bDesignerReady);
		JsonObj->SetNumberField(TEXT("selected_count"), Info.SelectedCount);
		JsonObj->SetStringField(TEXT("primary_component"), Info.PrimaryComponent);
		AddStringArrayField(JsonObj, TEXT("selected_components"), Info.SelectedComponents);
		JsonObj->SetBoolField(TEXT("has_named_slot_selection"), Info.bHasNamedSlotSelection);
		JsonObj->SetStringField(TEXT("named_slot_host_component"), Info.NamedSlotHostComponent);
		JsonObj->SetStringField(TEXT("named_slot_name"), Info.NamedSlotName);
		return JsonObj;
	}

	static TSharedPtr<FJsonObject> BuildDesignerStateJson(
		const FString& WidgetPath,
		bool bOpenIfNeeded,
		bool bSwitchToDesigner,
		const FString& ComponentName = FString())
	{
		const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
		DataObj->SetObjectField(TEXT("preview"), PreviewInfoToJson(UWidgetDesignerService::GetPreviewInfo(WidgetPath, bOpenIfNeeded, bSwitchToDesigner)));
		DataObj->SetObjectField(TEXT("design_settings"), PreviewInfoToJson(UWidgetDesignerService::GetDesignSettings(WidgetPath)));
		DataObj->SetObjectField(TEXT("selection"), SelectionInfoToJson(UWidgetDesignerService::GetSelectionInfo(WidgetPath, bOpenIfNeeded, bSwitchToDesigner)));
		if (!ComponentName.IsEmpty())
		{
			DataObj->SetObjectField(TEXT("slot_layout"), SlotLayoutToJson(UWidgetDesignerService::GetSlotLayout(WidgetPath, ComponentName)));
		}
		return DataObj;
	}

	static TArray<FString> ExtractDesignerStringListParam(const TMap<FString, FString>& Params, const FString& FieldName)
	{
		TArray<FString> Result;
		const FString RawValue = ExtractDesignerParam(Params, FieldName).TrimStartAndEnd();
		if (RawValue.IsEmpty())
		{
			return Result;
		}

		if (RawValue.StartsWith(TEXT("[")))
		{
			TArray<TSharedPtr<FJsonValue>> JsonArray;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RawValue);
			if (FJsonSerializer::Deserialize(Reader, JsonArray))
			{
				for (const TSharedPtr<FJsonValue>& Value : JsonArray)
				{
					if (!Value.IsValid())
					{
						continue;
					}

					const FString Item = Value->Type == EJson::String
						? Value->AsString().TrimStartAndEnd()
						: Value->AsString();
					if (!Item.IsEmpty())
					{
						Result.AddUnique(Item);
					}
				}
				return Result;
			}
		}

		TArray<FString> Parts;
		RawValue.ParseIntoArray(Parts, TEXT(","), true);
		for (FString& Part : Parts)
		{
			Part = Part.TrimStartAndEnd();
			if (!Part.IsEmpty())
			{
				Result.AddUnique(Part);
			}
		}

		return Result;
	}

	static bool ContainsStringIgnoreCase(const TArray<FString>& Values, const FString& Candidate)
	{
		for (const FString& Value : Values)
		{
			if (Value.Equals(Candidate, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	static bool ApplyMarginAliases(const TMap<FString, FString>& Params, const FString& Prefix, FMargin& InOutMargin)
	{
		bool bApplied = false;
		auto ApplySide = [&Params, &bApplied](const FString& Key, float& Target)
		{
			if (HasDesignerParam(Params, Key))
			{
				Target = ExtractDesignerFloatParam(Params, Key, Target);
				bApplied = true;
			}
		};

		ApplySide(Prefix + TEXT("_left"), InOutMargin.Left);
		ApplySide(Prefix + TEXT("_top"), InOutMargin.Top);
		ApplySide(Prefix + TEXT("_right"), InOutMargin.Right);
		ApplySide(Prefix + TEXT("_bottom"), InOutMargin.Bottom);
		return bApplied;
	}

	static bool ApplyVectorAliases(const TMap<FString, FString>& Params, const FString& Prefix, FVector2D& InOutVector)
	{
		bool bApplied = false;
		const FString XKey = Prefix + TEXT("_x");
		const FString YKey = Prefix + TEXT("_y");
		if (HasDesignerParam(Params, XKey))
		{
			InOutVector.X = ExtractDesignerFloatParam(Params, XKey, InOutVector.X);
			bApplied = true;
		}
		if (HasDesignerParam(Params, YKey))
		{
			InOutVector.Y = ExtractDesignerFloatParam(Params, YKey, InOutVector.Y);
			bApplied = true;
		}
		return bApplied;
	}

	static bool ApplyAnchorAliases(const TMap<FString, FString>& Params, FAnchors& InOutAnchors)
	{
		bool bApplied = false;
		auto ApplyAnchor = [&Params, &bApplied](const FString& Key, double& Target)
		{
			if (HasDesignerParam(Params, Key))
			{
				Target = ExtractDesignerFloatParam(Params, Key, Target);
				bApplied = true;
			}
		};

		ApplyAnchor(TEXT("anchor_min_x"), InOutAnchors.Minimum.X);
		ApplyAnchor(TEXT("anchor_min_y"), InOutAnchors.Minimum.Y);
		ApplyAnchor(TEXT("anchor_max_x"), InOutAnchors.Maximum.X);
		ApplyAnchor(TEXT("anchor_max_y"), InOutAnchors.Maximum.Y);
		ApplyAnchor(TEXT("anchors_min_x"), InOutAnchors.Minimum.X);
		ApplyAnchor(TEXT("anchors_min_y"), InOutAnchors.Minimum.Y);
		ApplyAnchor(TEXT("anchors_max_x"), InOutAnchors.Maximum.X);
		ApplyAnchor(TEXT("anchors_max_y"), InOutAnchors.Maximum.Y);
		return bApplied;
	}

	static bool LoadWidgetBlueprintForMutation(const FString& WidgetPath, UWidgetBlueprint*& OutWidgetBlueprint)
	{
		OutWidgetBlueprint = nullptr;

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

	static void ApplyPostWidgetActions(UWidgetBlueprint* WidgetBlueprint, const TMap<FString, FString>& Params, TArray<FString>& ChangedItems, TArray<FString>& Warnings)
	{
		if (!WidgetBlueprint)
		{
			return;
		}

		if (ExtractDesignerBoolParam(Params, TEXT("compile_after"), false))
		{
			FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
			ChangedItems.Add(TEXT("compiled widget blueprint"));
		}

		if (ExtractDesignerBoolParam(Params, TEXT("save_after"), false))
		{
			if (UEditorAssetLibrary::SaveLoadedAsset(WidgetBlueprint))
			{
				ChangedItems.Add(TEXT("saved widget blueprint"));
			}
			else
			{
				Warnings.Add(TEXT("Failed to save Widget Blueprint asset."));
			}
		}
	}

	static FString BuildWidgetDesignerHelpResponse()
	{
		const TSharedPtr<FJsonObject> HelpObj = MakeShared<FJsonObject>();
		HelpObj->SetBoolField(TEXT("success"), true);
		HelpObj->SetStringField(TEXT("tool"), TEXT("widget_designer"));
		HelpObj->SetStringField(TEXT("description"), TEXT("Designer-focused Widget Blueprint tooling. Supports live preview sizing, persisted design settings, designer selection workflows, and explicit slot layout editing."));

		TArray<TSharedPtr<FJsonValue>> ActionsArray;
		auto AddAction = [&ActionsArray](const FString& Name, const FString& Desc)
		{
			const TSharedPtr<FJsonObject> ActionObj = MakeShared<FJsonObject>();
			ActionObj->SetStringField(TEXT("action"), Name);
			ActionObj->SetStringField(TEXT("description"), Desc);
			ActionsArray.Add(MakeShared<FJsonValueObject>(ActionObj));
		};

		AddAction(TEXT("help"), TEXT("Show this help payload."));
		AddAction(TEXT("describe"), TEXT("Return persisted design settings, live preview info, and current designer selection. If component_name is provided, include slot layout."));
		AddAction(TEXT("get_preview"), TEXT("Read the current live preview area from the Widget Designer tab."));
		AddAction(TEXT("set_preview"), TEXT("Set the current live preview area size in the Widget Designer tab."));
		AddAction(TEXT("get_design_settings"), TEXT("Read persisted design-size mode and custom design-time size from the Widget Blueprint."));
		AddAction(TEXT("set_design_settings"), TEXT("Set persisted design-size mode and optional custom design-time size."));
		AddAction(TEXT("get_selection"), TEXT("Read the current widget/component selection in the Widget Designer editor."));
		AddAction(TEXT("select_components"), TEXT("Select one or more components in the Widget Designer editor."));
		AddAction(TEXT("clear_selection"), TEXT("Clear the current Widget Designer selection."));
		AddAction(TEXT("capture_preview_to_design_settings"), TEXT("Persist the current live preview size back onto the Widget Blueprint design settings."));
		AddAction(TEXT("apply_design_settings_to_preview"), TEXT("Apply persisted design settings back onto the live preview area."));
		AddAction(TEXT("get_slot_layout"), TEXT("Read explicit panel slot layout for a widget component."));
		AddAction(TEXT("set_slot_layout"), TEXT("Update explicit panel slot layout for a widget component."));

		HelpObj->SetArrayField(TEXT("actions"), ActionsArray);

		TArray<FString> Notes = {
			TEXT("Preview and design settings are different layers: preview is the live designer tab state, design settings are persisted on the widget default object."),
			TEXT("Selection actions operate on the live Widget Blueprint editor and are useful when you want Codex to target the same component you are inspecting in Designer."),
			TEXT("set_slot_layout supports Canvas, Overlay, HorizontalBox, VerticalBox, Grid, and UniformGrid slots in structured form."),
			TEXT("set_slot_layout accepts both packed values like padding=1,2,3,4 and flat aliases like padding_left=1, padding_top=2, padding_right=3, padding_bottom=4."),
			TEXT("Mutating actions also support compile_after and save_after booleans.")
		};
		AddStringArrayField(HelpObj, TEXT("notes"), Notes);

		TArray<FString> Examples = {
			TEXT("action=set_design_settings, widget_path=/Game/UI/WBP_MyWidget, size_mode=Custom, custom_width=195, custom_height=15"),
			TEXT("action=set_preview, widget_path=/Game/UI/WBP_MyWidget, width=195, height=15"),
			TEXT("action=select_components, widget_path=/Game/UI/WBP_MyWidget, component_names=RootSizeBox,CurrentFill"),
			TEXT("action=capture_preview_to_design_settings, widget_path=/Game/UI/WBP_MyWidget"),
			TEXT("action=get_slot_layout, widget_path=/Game/UI/WBP_MyWidget, component_name=RootSizeBox"),
			TEXT("action=set_slot_layout, widget_path=/Game/UI/WBP_MyWidget, component_name=RootSizeBox, horizontal_alignment=Fill, vertical_alignment=Center"),
			TEXT("action=set_slot_layout, widget_path=/Game/UI/WBP_MyWidget, component_name=CurrentFill, offset_right=83.3, offset_bottom=15, z_order=3"),
			TEXT("action=set_slot_layout, widget_path=/Game/UI/WBP_MyWidget, component_name=GridCell, padding_left=2, padding_right=2, nudge_x=1, nudge_y=2")
		};
		AddStringArrayField(HelpObj, TEXT("examples"), Examples);

		return SerializeDesignerJson(HelpObj);
	}
}

REGISTER_VIBEUE_TOOL(widget_designer,
	"Designer-focused Widget Blueprint tooling. Actions: help, describe, get_preview, set_preview, get_design_settings, set_design_settings, get_selection, select_components, clear_selection, capture_preview_to_design_settings, apply_design_settings_to_preview, get_slot_layout, set_slot_layout.",
	"Widget",
	TOOL_PARAMS(
		TOOL_PARAM("action", "Widget Designer operation to perform. Use help for documentation.", "string", true),
		TOOL_PARAM("widget_path", "Widget Blueprint asset path (package or object path). Required for most actions.", "string", false),
		TOOL_PARAM("component_name", "[slot actions] Widget component name.", "string", false),
		TOOL_PARAM("component_names", "[select_components] Comma-separated component names or JSON string array.", "string", false),
		TOOL_PARAM("append_to_selection", "[select_components] Keep the current selection and add the requested components. Default false.", "boolean", false),
		TOOL_PARAM("width", "[set_preview] Preview width.", "number", false),
		TOOL_PARAM("height", "[set_preview] Preview height.", "number", false),
		TOOL_PARAM("size_mode", "[set_design_settings] FillScreen, Custom, CustomOnScreen, Desired, or DesiredOnScreen.", "string", false),
		TOOL_PARAM("custom_width", "[set_design_settings] Custom design-time width.", "number", false),
		TOOL_PARAM("custom_height", "[set_design_settings] Custom design-time height.", "number", false),
		TOOL_PARAM("open_if_needed", "[preview actions] Open the Widget Blueprint editor if needed. Default true.", "boolean", false),
		TOOL_PARAM("switch_to_designer", "[preview actions] Switch the editor to Designer mode if needed. Default true.", "boolean", false),
		TOOL_PARAM("fields_to_apply", "[set_slot_layout] Comma-separated field list to update.", "string", false),
		TOOL_PARAM("padding", "[set_slot_layout] Margin as left,top,right,bottom.", "string", false),
		TOOL_PARAM("padding_left", "[set_slot_layout] Left margin alias.", "number", false),
		TOOL_PARAM("padding_top", "[set_slot_layout] Top margin alias.", "number", false),
		TOOL_PARAM("padding_right", "[set_slot_layout] Right margin alias.", "number", false),
		TOOL_PARAM("padding_bottom", "[set_slot_layout] Bottom margin alias.", "number", false),
		TOOL_PARAM("horizontal_alignment", "[set_slot_layout] Fill, Left, Center, Right.", "string", false),
		TOOL_PARAM("vertical_alignment", "[set_slot_layout] Fill, Top, Center, Bottom.", "string", false),
		TOOL_PARAM("anchors", "[set_slot_layout] Canvas anchors as min_x,min_y,max_x,max_y.", "string", false),
		TOOL_PARAM("anchor_min_x", "[set_slot_layout] Canvas anchor minimum X alias.", "number", false),
		TOOL_PARAM("anchor_min_y", "[set_slot_layout] Canvas anchor minimum Y alias.", "number", false),
		TOOL_PARAM("anchor_max_x", "[set_slot_layout] Canvas anchor maximum X alias.", "number", false),
		TOOL_PARAM("anchor_max_y", "[set_slot_layout] Canvas anchor maximum Y alias.", "number", false),
		TOOL_PARAM("offsets", "[set_slot_layout] Canvas offsets as left,top,right,bottom.", "string", false),
		TOOL_PARAM("offset_left", "[set_slot_layout] Canvas offset left alias.", "number", false),
		TOOL_PARAM("offset_top", "[set_slot_layout] Canvas offset top alias.", "number", false),
		TOOL_PARAM("offset_right", "[set_slot_layout] Canvas offset right alias.", "number", false),
		TOOL_PARAM("offset_bottom", "[set_slot_layout] Canvas offset bottom alias.", "number", false),
		TOOL_PARAM("alignment", "[set_slot_layout] Pivot/alignment as x,y.", "string", false),
		TOOL_PARAM("alignment_x", "[set_slot_layout] Pivot/alignment X alias.", "number", false),
		TOOL_PARAM("alignment_y", "[set_slot_layout] Pivot/alignment Y alias.", "number", false),
		TOOL_PARAM("auto_size", "[set_slot_layout] Canvas auto-size flag.", "boolean", false),
		TOOL_PARAM("z_order", "[set_slot_layout] Canvas z-order.", "number", false),
		TOOL_PARAM("size_rule", "[set_slot_layout] Automatic or Fill for box slots.", "string", false),
		TOOL_PARAM("size_value", "[set_slot_layout] Fill value for box slots.", "number", false),
		TOOL_PARAM("row", "[set_slot_layout] Grid/UniformGrid row.", "number", false),
		TOOL_PARAM("column", "[set_slot_layout] Grid/UniformGrid column.", "number", false),
		TOOL_PARAM("row_span", "[set_slot_layout] Grid row span.", "number", false),
		TOOL_PARAM("column_span", "[set_slot_layout] Grid column span.", "number", false),
		TOOL_PARAM("layer", "[set_slot_layout] Grid layer.", "number", false),
		TOOL_PARAM("nudge", "[set_slot_layout] Grid nudge as x,y.", "string", false),
		TOOL_PARAM("nudge_x", "[set_slot_layout] Grid nudge X alias.", "number", false),
		TOOL_PARAM("nudge_y", "[set_slot_layout] Grid nudge Y alias.", "number", false),
		TOOL_PARAM("layout_json", "[set_slot_layout] Optional JSON payload for structured slot layout input.", "string", false),
		TOOL_PARAM("compile_after", "[mutating actions] Compile the Widget Blueprint after the operation.", "boolean", false),
		TOOL_PARAM("save_after", "[mutating actions] Save the Widget Blueprint after the operation.", "boolean", false)
	),
	{
		const FString Action = NormalizeDesignerAction(ExtractDesignerParam(Params, TEXT("action")));
		if (Action.IsEmpty())
		{
			return SerializeDesignerJson(BuildDesignerError(
				TEXT("unknown"),
				TEXT("MISSING_ACTION"),
				TEXT("The 'action' parameter is required. Use action=help for documentation.")
			));
		}

		if (Action == TEXT("help"))
		{
			return BuildWidgetDesignerHelpResponse();
		}

		const FString WidgetPath = ExtractDesignerParam(Params, TEXT("widget_path"));
		if (WidgetPath.IsEmpty())
		{
			return SerializeDesignerJson(BuildDesignerError(
				Action,
				TEXT("MISSING_WIDGET_PATH"),
				TEXT("The 'widget_path' parameter is required for this action.")
			));
		}

		if (Action == TEXT("describe"))
		{
			const bool bOpenIfNeeded = ExtractDesignerBoolParam(Params, TEXT("open_if_needed"), true);
			const bool bSwitchToDesigner = ExtractDesignerBoolParam(Params, TEXT("switch_to_designer"), true);
			const FString ComponentName = ExtractDesignerParam(Params, TEXT("component_name"));

			return SerializeDesignerJson(BuildDesignerSuccess(
				Action,
				TEXT("Collected widget designer state."),
				WidgetPath,
				BuildDesignerStateJson(WidgetPath, bOpenIfNeeded, bSwitchToDesigner, ComponentName),
				{},
				{
					TEXT("Open the Widget Blueprint in Designer mode to inspect or adjust the live preview area."),
					TEXT("selection reflects the current live Designer editor selection, which is useful when coordinating manual inspection with Codex-driven edits.")
				}
			));
		}

		if (Action == TEXT("get_preview"))
		{
			const bool bOpenIfNeeded = ExtractDesignerBoolParam(Params, TEXT("open_if_needed"), true);
			const bool bSwitchToDesigner = ExtractDesignerBoolParam(Params, TEXT("switch_to_designer"), true);
			const FWidgetDesignerPreviewInfo Info = UWidgetDesignerService::GetPreviewInfo(WidgetPath, bOpenIfNeeded, bSwitchToDesigner);

			if (!Info.bWidgetBlueprintLoaded)
			{
				return SerializeDesignerJson(BuildDesignerError(
					Action,
					TEXT("WIDGET_BLUEPRINT_NOT_FOUND"),
					FString::Printf(TEXT("Widget Blueprint '%s' was not found."), *WidgetPath),
					WidgetPath
				));
			}

			return SerializeDesignerJson(BuildDesignerSuccess(
				Action,
				Info.bDesignerReady ? TEXT("Read live designer preview state.") : TEXT("Widget Blueprint loaded, but the live designer tab was not available."),
				WidgetPath,
				PreviewInfoToJson(Info),
				{},
				{
					TEXT("designer_ready=true means the preview numbers came from the live Widget Designer tab."),
					TEXT("design_size_mode and design_time_size are persisted asset settings even when the live designer is not available.")
				}
			));
		}

		if (Action == TEXT("set_preview"))
		{
			const bool bOpenIfNeeded = ExtractDesignerBoolParam(Params, TEXT("open_if_needed"), true);
			const bool bSwitchToDesigner = ExtractDesignerBoolParam(Params, TEXT("switch_to_designer"), true);
			const int32 Width = FMath::Max(ExtractDesignerIntParam(Params, TEXT("width"), 0), 1);
			const int32 Height = FMath::Max(ExtractDesignerIntParam(Params, TEXT("height"), 0), 1);

			if (!UWidgetDesignerService::SetPreviewArea(WidgetPath, Width, Height, bOpenIfNeeded, bSwitchToDesigner))
			{
				return SerializeDesignerJson(BuildDesignerError(
					Action,
					TEXT("SET_PREVIEW_FAILED"),
					TEXT("Failed to set the live designer preview area. Ensure the Widget Blueprint can be opened in Designer mode."),
					WidgetPath,
					{ TEXT("Try opening the Widget Blueprint editor manually, then rerun set_preview.") }
				));
			}

			UWidgetBlueprint* WidgetBlueprint = nullptr;
			LoadWidgetBlueprintForMutation(WidgetPath, WidgetBlueprint);
			TArray<FString> ChangedItems = {
				FString::Printf(TEXT("set live preview area to %dx%d"), Width, Height)
			};
			TArray<FString> Warnings;
			ApplyPostWidgetActions(WidgetBlueprint, Params, ChangedItems, Warnings);

			return SerializeDesignerJson(BuildDesignerSuccess(
				Action,
				FString::Printf(TEXT("Updated live designer preview area to %dx%d."), Width, Height),
				WidgetPath,
				PreviewInfoToJson(UWidgetDesignerService::GetPreviewInfo(WidgetPath, bOpenIfNeeded, bSwitchToDesigner)),
				ChangedItems,
				{
					TEXT("In Custom/CustomOnScreen design modes, preview resizing also updates the widget's persisted DesignTimeSize."),
					TEXT("In screen-preview modes, preview resizing affects the live designer state rather than the widget asset default size.")
				},
				Warnings
			));
		}

		if (Action == TEXT("get_design_settings"))
		{
			const FWidgetDesignerPreviewInfo Info = UWidgetDesignerService::GetDesignSettings(WidgetPath);
			if (!Info.bWidgetBlueprintLoaded)
			{
				return SerializeDesignerJson(BuildDesignerError(
					Action,
					TEXT("WIDGET_BLUEPRINT_NOT_FOUND"),
					FString::Printf(TEXT("Widget Blueprint '%s' was not found."), *WidgetPath),
					WidgetPath
				));
			}

			return SerializeDesignerJson(BuildDesignerSuccess(
				Action,
				TEXT("Read persisted widget design settings."),
				WidgetPath,
				PreviewInfoToJson(Info),
				{},
				{
					TEXT("Design settings are persisted on the Widget Blueprint default object and define the initial designer sizing behavior for this asset.")
				}
			));
		}

		if (Action == TEXT("get_selection"))
		{
			const bool bOpenIfNeeded = ExtractDesignerBoolParam(Params, TEXT("open_if_needed"), true);
			const bool bSwitchToDesigner = ExtractDesignerBoolParam(Params, TEXT("switch_to_designer"), true);
			const FWidgetDesignerSelectionInfo Info = UWidgetDesignerService::GetSelectionInfo(WidgetPath, bOpenIfNeeded, bSwitchToDesigner);
			if (!Info.bWidgetBlueprintLoaded)
			{
				return SerializeDesignerJson(BuildDesignerError(
					Action,
					TEXT("WIDGET_BLUEPRINT_NOT_FOUND"),
					FString::Printf(TEXT("Widget Blueprint '%s' was not found."), *WidgetPath),
					WidgetPath
				));
			}

			return SerializeDesignerJson(BuildDesignerSuccess(
				Action,
				Info.bEditorOpen ? TEXT("Read current designer selection.") : TEXT("Widget Blueprint loaded, but the Widget Designer editor is not open."),
				WidgetPath,
				SelectionInfoToJson(Info),
				{},
				{
					TEXT("selected_components comes from the live Widget Blueprint editor selection, not from the hierarchy tree export."),
					TEXT("Use select_components and clear_selection to coordinate Codex with the component you are currently inspecting.")
				}
			));
		}

		if (Action == TEXT("select_components"))
		{
			const bool bOpenIfNeeded = ExtractDesignerBoolParam(Params, TEXT("open_if_needed"), true);
			const bool bSwitchToDesigner = ExtractDesignerBoolParam(Params, TEXT("switch_to_designer"), true);
			const bool bAppendToSelection = ExtractDesignerBoolParam(Params, TEXT("append_to_selection"), false);
			TArray<FString> ComponentNames = ExtractDesignerStringListParam(Params, TEXT("component_names"));
			if (ComponentNames.Num() == 0)
			{
				const FString SingleComponent = ExtractDesignerParam(Params, TEXT("component_name"));
				if (!SingleComponent.IsEmpty())
				{
					ComponentNames.Add(SingleComponent);
				}
			}

			if (ComponentNames.Num() == 0)
			{
				return SerializeDesignerJson(BuildDesignerError(
					Action,
					TEXT("MISSING_COMPONENT_NAME"),
					TEXT("Provide component_name or component_names for select_components."),
					WidgetPath
				));
			}

			if (!UWidgetDesignerService::SelectComponents(WidgetPath, ComponentNames, bAppendToSelection, bOpenIfNeeded, bSwitchToDesigner))
			{
				return SerializeDesignerJson(BuildDesignerError(
					Action,
					TEXT("SELECT_COMPONENTS_FAILED"),
					TEXT("Failed to select the requested components. Check the component names and ensure the Widget Blueprint editor can be opened."),
					WidgetPath,
					{ TEXT("Use widget_blueprint describe to inspect valid component names before selecting them.") }
				));
			}

			const FWidgetDesignerSelectionInfo Selection = UWidgetDesignerService::GetSelectionInfo(WidgetPath, bOpenIfNeeded, bSwitchToDesigner);
			TArray<FString> MissingComponents;
			for (const FString& RequestedComponent : ComponentNames)
			{
				if (!ContainsStringIgnoreCase(Selection.SelectedComponents, RequestedComponent))
				{
					MissingComponents.Add(RequestedComponent);
				}
			}

			TArray<FString> ChangedItems;
			for (const FString& RequestedComponent : ComponentNames)
			{
				ChangedItems.Add(FString::Printf(TEXT("selected component %s"), *RequestedComponent));
			}

			TArray<FString> Warnings;
			if (MissingComponents.Num() > 0)
			{
				Warnings.Add(FString::Printf(TEXT("Some requested components were not resolved into the live selection: %s"), *FString::Join(MissingComponents, TEXT(", "))));
			}

			return SerializeDesignerJson(BuildDesignerSuccess(
				Action,
				FString::Printf(TEXT("Updated designer selection (%d component(s) selected)."), Selection.SelectedCount),
				WidgetPath,
				SelectionInfoToJson(Selection),
				ChangedItems,
				{
					TEXT("append_to_selection=true keeps the current selection and adds the requested components."),
					TEXT("Use get_selection after clicking in Designer if you want Codex to target the same component you manually highlighted.")
				},
				Warnings
			));
		}

		if (Action == TEXT("clear_selection"))
		{
			const bool bOpenIfNeeded = ExtractDesignerBoolParam(Params, TEXT("open_if_needed"), true);
			const bool bSwitchToDesigner = ExtractDesignerBoolParam(Params, TEXT("switch_to_designer"), true);
			if (!UWidgetDesignerService::ClearSelection(WidgetPath, bOpenIfNeeded, bSwitchToDesigner))
			{
				return SerializeDesignerJson(BuildDesignerError(
					Action,
					TEXT("CLEAR_SELECTION_FAILED"),
					TEXT("Failed to clear the designer selection. Ensure the Widget Blueprint editor can be opened."),
					WidgetPath
				));
			}

			return SerializeDesignerJson(BuildDesignerSuccess(
				Action,
				TEXT("Cleared designer selection."),
				WidgetPath,
				SelectionInfoToJson(UWidgetDesignerService::GetSelectionInfo(WidgetPath, bOpenIfNeeded, bSwitchToDesigner)),
				{ TEXT("cleared designer selection") },
				{
					TEXT("Use clear_selection before select_components when you want a deterministic single-target editing flow.")
				}
			));
		}

		if (Action == TEXT("set_design_settings"))
		{
			const FString SizeMode = ExtractDesignerParam(Params, TEXT("size_mode"));
			const FVector2D CustomSize(
				static_cast<double>(ExtractDesignerIntParam(Params, TEXT("custom_width"), 1920)),
				static_cast<double>(ExtractDesignerIntParam(Params, TEXT("custom_height"), 1080))
			);

			if (SizeMode.IsEmpty())
			{
				return SerializeDesignerJson(BuildDesignerError(
					Action,
					TEXT("MISSING_SIZE_MODE"),
					TEXT("The 'size_mode' parameter is required for set_design_settings."),
					WidgetPath
				));
			}

			if (!UWidgetDesignerService::SetDesignSettings(WidgetPath, SizeMode, CustomSize))
			{
				return SerializeDesignerJson(BuildDesignerError(
					Action,
					TEXT("SET_DESIGN_SETTINGS_FAILED"),
					TEXT("Failed to update persisted design settings. Check size_mode and custom size values."),
					WidgetPath,
					{ TEXT("Valid size_mode values: FillScreen, Custom, CustomOnScreen, Desired, DesiredOnScreen.") }
				));
			}

			UWidgetBlueprint* WidgetBlueprint = nullptr;
			LoadWidgetBlueprintForMutation(WidgetPath, WidgetBlueprint);
			TArray<FString> ChangedItems = {
				FString::Printf(TEXT("set design size mode to %s"), *SizeMode)
			};
			FString NormalizedSizeMode = SizeMode;
			NormalizedSizeMode.ReplaceInline(TEXT(" "), TEXT(""));
			NormalizedSizeMode = NormalizedSizeMode.ToLower();
			if (NormalizedSizeMode == TEXT("custom") || NormalizedSizeMode == TEXT("customonscreen") ||
				NormalizedSizeMode == TEXT("desired") || NormalizedSizeMode == TEXT("desiredonscreen"))
			{
				ChangedItems.Add(FString::Printf(TEXT("set design time size to %.0fx%.0f"), CustomSize.X, CustomSize.Y));
			}
			TArray<FString> Warnings;
			ApplyPostWidgetActions(WidgetBlueprint, Params, ChangedItems, Warnings);

			return SerializeDesignerJson(BuildDesignerSuccess(
				Action,
				TEXT("Updated persisted widget design settings."),
				WidgetPath,
				PreviewInfoToJson(UWidgetDesignerService::GetDesignSettings(WidgetPath)),
				ChangedItems,
				{
					TEXT("Reopen the Widget Blueprint or switch back to Designer mode if you want to visually confirm the new initial preview range.")
				},
				Warnings
			));
		}

		if (Action == TEXT("capture_preview_to_design_settings"))
		{
			const bool bOpenIfNeeded = ExtractDesignerBoolParam(Params, TEXT("open_if_needed"), true);
			const bool bSwitchToDesigner = ExtractDesignerBoolParam(Params, TEXT("switch_to_designer"), true);
			const FWidgetDesignerPreviewInfo PreviewInfo = UWidgetDesignerService::GetPreviewInfo(WidgetPath, bOpenIfNeeded, bSwitchToDesigner);
			if (!PreviewInfo.bWidgetBlueprintLoaded)
			{
				return SerializeDesignerJson(BuildDesignerError(
					Action,
					TEXT("WIDGET_BLUEPRINT_NOT_FOUND"),
					FString::Printf(TEXT("Widget Blueprint '%s' was not found."), *WidgetPath),
					WidgetPath
				));
			}
			if (!PreviewInfo.bDesignerReady)
			{
				return SerializeDesignerJson(BuildDesignerError(
					Action,
					TEXT("DESIGNER_NOT_READY"),
					TEXT("The live Widget Designer tab is not ready, so the current preview size cannot be captured."),
					WidgetPath,
					{ TEXT("Open the Widget Blueprint in Designer mode, then rerun capture_preview_to_design_settings.") }
				));
			}

			FString SizeMode = ExtractDesignerParam(Params, TEXT("size_mode"));
			if (SizeMode.IsEmpty())
			{
				SizeMode = IsExplicitDesignSizeModeToken(PreviewInfo.DesignSizeMode) ? PreviewInfo.DesignSizeMode : TEXT("Custom");
			}

			if (!UWidgetDesignerService::SetDesignSettings(
				WidgetPath,
				SizeMode,
				FVector2D(PreviewInfo.PreviewAreaWidth, PreviewInfo.PreviewAreaHeight)))
			{
				return SerializeDesignerJson(BuildDesignerError(
					Action,
					TEXT("CAPTURE_PREVIEW_FAILED"),
					TEXT("Failed to persist the current live preview size back into the widget design settings."),
					WidgetPath
				));
			}

			UWidgetBlueprint* WidgetBlueprint = nullptr;
			LoadWidgetBlueprintForMutation(WidgetPath, WidgetBlueprint);
			TArray<FString> ChangedItems;
			ChangedItems.Add(FString::Printf(TEXT("captured preview size %dx%d into design settings"), PreviewInfo.PreviewAreaWidth, PreviewInfo.PreviewAreaHeight));
			ChangedItems.Add(FString::Printf(TEXT("set design size mode to %s"), *SizeMode));
			TArray<FString> Warnings;
			ApplyPostWidgetActions(WidgetBlueprint, Params, ChangedItems, Warnings);

			return SerializeDesignerJson(BuildDesignerSuccess(
				Action,
				TEXT("Captured live preview size into persisted design settings."),
				WidgetPath,
				BuildDesignerStateJson(WidgetPath, bOpenIfNeeded, bSwitchToDesigner),
				ChangedItems,
				{
					TEXT("This is the fastest way to turn a manually adjusted Designer preview range into the widget's persisted initial design size."),
					TEXT("If size_mode is omitted, the current explicit design mode is preserved; otherwise Custom is used.")
				},
				Warnings
			));
		}

		if (Action == TEXT("apply_design_settings_to_preview"))
		{
			const bool bOpenIfNeeded = ExtractDesignerBoolParam(Params, TEXT("open_if_needed"), true);
			const bool bSwitchToDesigner = ExtractDesignerBoolParam(Params, TEXT("switch_to_designer"), true);
			const FWidgetDesignerPreviewInfo DesignInfo = UWidgetDesignerService::GetDesignSettings(WidgetPath);
			if (!DesignInfo.bWidgetBlueprintLoaded)
			{
				return SerializeDesignerJson(BuildDesignerError(
					Action,
					TEXT("WIDGET_BLUEPRINT_NOT_FOUND"),
					FString::Printf(TEXT("Widget Blueprint '%s' was not found."), *WidgetPath),
					WidgetPath
				));
			}
			if (!IsExplicitDesignSizeModeToken(DesignInfo.DesignSizeMode))
			{
				return SerializeDesignerJson(BuildDesignerError(
					Action,
					TEXT("DESIGN_SETTINGS_NOT_EXPLICIT"),
					TEXT("apply_design_settings_to_preview requires an explicit design size mode such as Custom, CustomOnScreen, Desired, or DesiredOnScreen."),
					WidgetPath,
					{ TEXT("Use set_design_settings or capture_preview_to_design_settings first if you want the live preview to follow a concrete authored size.") }
				));
			}

			const int32 Width = FMath::Max(FMath::RoundToInt(DesignInfo.DesignTimeSize.X), 1);
			const int32 Height = FMath::Max(FMath::RoundToInt(DesignInfo.DesignTimeSize.Y), 1);
			if (!UWidgetDesignerService::SetPreviewArea(WidgetPath, Width, Height, bOpenIfNeeded, bSwitchToDesigner))
			{
				return SerializeDesignerJson(BuildDesignerError(
					Action,
					TEXT("APPLY_DESIGN_SETTINGS_FAILED"),
					TEXT("Failed to apply persisted design settings back onto the live preview area."),
					WidgetPath
				));
			}

			return SerializeDesignerJson(BuildDesignerSuccess(
				Action,
				FString::Printf(TEXT("Applied persisted design settings to live preview (%dx%d)."), Width, Height),
				WidgetPath,
				BuildDesignerStateJson(WidgetPath, bOpenIfNeeded, bSwitchToDesigner),
				{ FString::Printf(TEXT("applied design settings size %dx%d to live preview"), Width, Height) },
				{
					TEXT("This is the reverse of capture_preview_to_design_settings and is useful when you want the live Designer tab to snap back to the authored initial size.")
				}
			));
		}

		if (Action == TEXT("get_slot_layout"))
		{
			const FString ComponentName = ExtractDesignerParam(Params, TEXT("component_name"));
			if (ComponentName.IsEmpty())
			{
				return SerializeDesignerJson(BuildDesignerError(
					Action,
					TEXT("MISSING_COMPONENT_NAME"),
					TEXT("The 'component_name' parameter is required for slot layout actions."),
					WidgetPath
				));
			}

			const FWidgetDesignerSlotLayout Layout = UWidgetDesignerService::GetSlotLayout(WidgetPath, ComponentName);
			if (!Layout.bHasSlot)
			{
				return SerializeDesignerJson(BuildDesignerError(
					Action,
					TEXT("SLOT_NOT_FOUND"),
					FString::Printf(TEXT("Component '%s' does not have a panel slot in '%s'."), *ComponentName, *WidgetPath),
					WidgetPath
				));
			}

			return SerializeDesignerJson(BuildDesignerSuccess(
				Action,
				TEXT("Read widget slot layout."),
				WidgetPath,
				SlotLayoutToJson(Layout),
				{},
				{
					TEXT("supported_fields lists the structured fields that can be safely edited for this slot type."),
					TEXT("raw_properties contains the direct slot property export for advanced cases.")
				}
			));
		}

		if (Action == TEXT("set_slot_layout"))
		{
			const FString ComponentName = ExtractDesignerParam(Params, TEXT("component_name"));
			if (ComponentName.IsEmpty())
			{
				return SerializeDesignerJson(BuildDesignerError(
					Action,
					TEXT("MISSING_COMPONENT_NAME"),
					TEXT("The 'component_name' parameter is required for slot layout actions."),
					WidgetPath
				));
			}

			FWidgetDesignerSlotLayout Layout = UWidgetDesignerService::GetSlotLayout(WidgetPath, ComponentName);
			if (!Layout.bHasSlot)
			{
				return SerializeDesignerJson(BuildDesignerError(
					Action,
					TEXT("SLOT_NOT_FOUND"),
					FString::Printf(TEXT("Component '%s' does not have a panel slot in '%s'."), *ComponentName, *WidgetPath),
					WidgetPath
				));
			}

			const FString LayoutJson = ExtractDesignerParam(Params, TEXT("layout_json"));
			if (!LayoutJson.IsEmpty())
			{
				FJsonObjectConverter::JsonObjectStringToUStruct(LayoutJson, &Layout, 0, 0);
			}

			const FString FieldsToApply = ExtractDesignerParam(Params, TEXT("fields_to_apply"));
			if (!FieldsToApply.IsEmpty())
			{
				FieldsToApply.ParseIntoArray(Layout.FieldsToApply, TEXT(","), true);
				for (FString& FieldName : Layout.FieldsToApply)
				{
					FieldName = FieldName.TrimStartAndEnd();
				}
			}

			const FString Padding = ExtractDesignerParam(Params, TEXT("padding"));
			if (!Padding.IsEmpty())
			{
				if (!ParseMarginString(Padding, Layout.Padding))
				{
					return SerializeDesignerJson(BuildDesignerError(
						Action,
						TEXT("INVALID_PADDING"),
						TEXT("padding must be left,top,right,bottom"),
						WidgetPath
					));
				}
				Layout.FieldsToApply.AddUnique(TEXT("padding"));
			}
			else if (ApplyMarginAliases(Params, TEXT("padding"), Layout.Padding))
			{
				Layout.FieldsToApply.AddUnique(TEXT("padding"));
			}

			const FString HorizontalAlignment = ExtractDesignerParam(Params, TEXT("horizontal_alignment"));
			if (!HorizontalAlignment.IsEmpty())
			{
				Layout.HorizontalAlignment = HorizontalAlignment;
				Layout.FieldsToApply.AddUnique(TEXT("horizontal_alignment"));
			}

			const FString VerticalAlignment = ExtractDesignerParam(Params, TEXT("vertical_alignment"));
			if (!VerticalAlignment.IsEmpty())
			{
				Layout.VerticalAlignment = VerticalAlignment;
				Layout.FieldsToApply.AddUnique(TEXT("vertical_alignment"));
			}

			const FString Anchors = ExtractDesignerParam(Params, TEXT("anchors"));
			if (!Anchors.IsEmpty())
			{
				if (!ParseAnchorsString(Anchors, Layout.Anchors))
				{
					return SerializeDesignerJson(BuildDesignerError(
						Action,
						TEXT("INVALID_ANCHORS"),
						TEXT("anchors must be min_x,min_y,max_x,max_y"),
						WidgetPath
					));
				}
				Layout.FieldsToApply.AddUnique(TEXT("anchors"));
			}
			else if (ApplyAnchorAliases(Params, Layout.Anchors))
			{
				Layout.FieldsToApply.AddUnique(TEXT("anchors"));
			}

			const FString Offsets = ExtractDesignerParam(Params, TEXT("offsets"));
			if (!Offsets.IsEmpty())
			{
				if (!ParseMarginString(Offsets, Layout.Offsets))
				{
					return SerializeDesignerJson(BuildDesignerError(
						Action,
						TEXT("INVALID_OFFSETS"),
						TEXT("offsets must be left,top,right,bottom"),
						WidgetPath
					));
				}
				Layout.FieldsToApply.AddUnique(TEXT("offsets"));
			}
			else if (ApplyMarginAliases(Params, TEXT("offset"), Layout.Offsets))
			{
				Layout.FieldsToApply.AddUnique(TEXT("offsets"));
			}

			const FString Alignment = ExtractDesignerParam(Params, TEXT("alignment"));
			if (!Alignment.IsEmpty())
			{
				if (!ParseVector2DString(Alignment, Layout.Alignment))
				{
					return SerializeDesignerJson(BuildDesignerError(
						Action,
						TEXT("INVALID_ALIGNMENT"),
						TEXT("alignment must be x,y"),
						WidgetPath
					));
				}
				Layout.FieldsToApply.AddUnique(TEXT("alignment"));
			}
			else if (ApplyVectorAliases(Params, TEXT("alignment"), Layout.Alignment))
			{
				Layout.FieldsToApply.AddUnique(TEXT("alignment"));
			}

			if (HasDesignerParam(Params, TEXT("auto_size")))
			{
				Layout.bAutoSize = ExtractDesignerBoolParam(Params, TEXT("auto_size"), false);
				Layout.FieldsToApply.AddUnique(TEXT("auto_size"));
			}

			if (HasDesignerParam(Params, TEXT("z_order")))
			{
				Layout.ZOrder = ExtractDesignerIntParam(Params, TEXT("z_order"), 0);
				Layout.FieldsToApply.AddUnique(TEXT("z_order"));
			}

			const FString SizeRule = ExtractDesignerParam(Params, TEXT("size_rule"));
			if (!SizeRule.IsEmpty())
			{
				Layout.SizeRule = SizeRule;
				Layout.FieldsToApply.AddUnique(TEXT("size_rule"));
			}

			if (HasDesignerParam(Params, TEXT("size_value")))
			{
				Layout.SizeValue = ExtractDesignerFloatParam(Params, TEXT("size_value"), 0.0f);
				Layout.FieldsToApply.AddUnique(TEXT("size_value"));
			}

			if (HasDesignerParam(Params, TEXT("row")))
			{
				Layout.Row = ExtractDesignerIntParam(Params, TEXT("row"), 0);
				Layout.FieldsToApply.AddUnique(TEXT("row"));
			}

			if (HasDesignerParam(Params, TEXT("column")))
			{
				Layout.Column = ExtractDesignerIntParam(Params, TEXT("column"), 0);
				Layout.FieldsToApply.AddUnique(TEXT("column"));
			}

			if (HasDesignerParam(Params, TEXT("row_span")))
			{
				Layout.RowSpan = ExtractDesignerIntParam(Params, TEXT("row_span"), 1);
				Layout.FieldsToApply.AddUnique(TEXT("row_span"));
			}

			if (HasDesignerParam(Params, TEXT("column_span")))
			{
				Layout.ColumnSpan = ExtractDesignerIntParam(Params, TEXT("column_span"), 1);
				Layout.FieldsToApply.AddUnique(TEXT("column_span"));
			}

			if (HasDesignerParam(Params, TEXT("layer")))
			{
				Layout.Layer = ExtractDesignerIntParam(Params, TEXT("layer"), 0);
				Layout.FieldsToApply.AddUnique(TEXT("layer"));
			}

			const FString Nudge = ExtractDesignerParam(Params, TEXT("nudge"));
			if (!Nudge.IsEmpty())
			{
				if (!ParseVector2DString(Nudge, Layout.Nudge))
				{
					return SerializeDesignerJson(BuildDesignerError(
						Action,
						TEXT("INVALID_NUDGE"),
						TEXT("nudge must be x,y"),
						WidgetPath
					));
				}
				Layout.FieldsToApply.AddUnique(TEXT("nudge"));
			}
			else if (ApplyVectorAliases(Params, TEXT("nudge"), Layout.Nudge))
			{
				Layout.FieldsToApply.AddUnique(TEXT("nudge"));
			}

			if (!UWidgetDesignerService::SetSlotLayout(WidgetPath, ComponentName, Layout))
			{
				return SerializeDesignerJson(BuildDesignerError(
					Action,
					TEXT("SET_SLOT_LAYOUT_FAILED"),
					TEXT("Failed to update the widget slot layout. Check slot type and field compatibility."),
					WidgetPath
				));
			}

			UWidgetBlueprint* WidgetBlueprint = nullptr;
			LoadWidgetBlueprintForMutation(WidgetPath, WidgetBlueprint);
			TArray<FString> ChangedItems;
			for (const FString& FieldName : Layout.FieldsToApply)
			{
				ChangedItems.Add(FString::Printf(TEXT("updated slot field %s"), *FieldName));
			}
			if (ChangedItems.Num() == 0)
			{
				ChangedItems.Add(TEXT("updated slot layout"));
			}
			TArray<FString> Warnings;
			ApplyPostWidgetActions(WidgetBlueprint, Params, ChangedItems, Warnings);

			return SerializeDesignerJson(BuildDesignerSuccess(
				Action,
				TEXT("Updated widget slot layout."),
				WidgetPath,
				SlotLayoutToJson(UWidgetDesignerService::GetSlotLayout(WidgetPath, ComponentName)),
				ChangedItems,
				{
					TEXT("If the component is in a container with a different slot class than expected, unsupported fields are ignored."),
					TEXT("Use get_slot_layout first to inspect supported_fields before applying a narrow patch.")
				},
				Warnings
			));
		}

		return SerializeDesignerJson(BuildDesignerError(
			Action,
			TEXT("UNSUPPORTED_ACTION"),
			FString::Printf(TEXT("Unsupported widget_designer action '%s'. Use action=help for documentation."), *Action),
			WidgetPath
		));
	}
);
