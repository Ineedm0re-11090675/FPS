// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Core/ToolRegistry.h"
#include "PythonAPI/UAssetDiscoveryService.h"
#include "PythonAPI/UWidgetService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "IAssetTools.h"
#include "Json.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/UObjectIterator.h"
#include "Blueprint/WidgetTree.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"
#include "Misc/PackageName.h"

namespace
{
	static const FTopLevelAssetPath WidgetBlueprintClassPath(TEXT("/Script/UMGEditor.WidgetBlueprint"));

	static FString ExtractWidgetParam(const TMap<FString, FString>& Params, const FString& FieldName)
	{
		if (const FString* DirectParam = Params.Find(FieldName))
		{
			return *DirectParam;
		}

		FString CapitalizedField = FieldName;
		if (CapitalizedField.Len() > 0)
		{
			CapitalizedField[0] = FChar::ToUpper(CapitalizedField[0]);
		}

		if (const FString* CapitalizedParam = Params.Find(CapitalizedField))
		{
			return *CapitalizedParam;
		}

		const FString* ParamsJsonStr = Params.Find(TEXT("ParamsJson"));
		if (!ParamsJsonStr)
		{
			return FString();
		}

		TSharedPtr<FJsonObject> JsonObj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(*ParamsJsonStr);
		if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
		{
			return FString();
		}

		FString Value;
		if (JsonObj->TryGetStringField(FieldName, Value))
		{
			return Value;
		}

		return FString();
	}

	static bool ExtractWidgetBoolParam(const TMap<FString, FString>& Params, const FString& FieldName, bool bDefaultValue)
	{
		const FString Value = ExtractWidgetParam(Params, FieldName);
		if (Value.IsEmpty())
		{
			return bDefaultValue;
		}

		return Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("1"));
	}

	static int32 ExtractWidgetIntParam(const TMap<FString, FString>& Params, const FString& FieldName, int32 DefaultValue, int32 MinValue = 0, int32 MaxValue = 200)
	{
		const FString Value = ExtractWidgetParam(Params, FieldName);
		if (Value.IsEmpty())
		{
			return FMath::Clamp(DefaultValue, MinValue, MaxValue);
		}

		return FMath::Clamp(FCString::Atoi(*Value), MinValue, MaxValue);
	}

	static FString SerializeWidgetJson(const TSharedPtr<FJsonObject>& JsonObj)
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

	static FString GetBlueprintStatusString(EBlueprintStatus Status)
	{
		switch (Status)
		{
		case BS_Unknown:
			return TEXT("Unknown");
		case BS_Dirty:
			return TEXT("Dirty");
		case BS_Error:
			return TEXT("Error");
		case BS_UpToDate:
			return TEXT("UpToDate");
		case BS_BeingCreated:
			return TEXT("BeingCreated");
		case BS_UpToDateWithWarnings:
			return TEXT("UpToDateWithWarnings");
		default:
			return TEXT("Unknown");
		}
	}

	static TSharedPtr<FJsonObject> BuildAssetJson(const FAssetData& AssetData)
	{
		const TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
		AssetObj->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
		AssetObj->SetStringField(TEXT("asset_class"), AssetData.AssetClassPath.GetAssetName().ToString());
		AssetObj->SetStringField(TEXT("asset_class_path"), AssetData.AssetClassPath.ToString());
		AssetObj->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
		AssetObj->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
		AssetObj->SetStringField(TEXT("object_path"), AssetData.GetObjectPathString());
		return AssetObj;
	}

	static TSharedPtr<FJsonObject> WidgetInfoToJson(const FWidgetInfo& Info)
	{
		const TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
		JsonObj->SetStringField(TEXT("widget_name"), Info.WidgetName);
		JsonObj->SetStringField(TEXT("widget_class"), Info.WidgetClass);
		JsonObj->SetStringField(TEXT("parent_widget"), Info.ParentWidget);
		JsonObj->SetBoolField(TEXT("is_root_widget"), Info.bIsRootWidget);
		JsonObj->SetBoolField(TEXT("is_variable"), Info.bIsVariable);
		AddStringArrayField(JsonObj, TEXT("children"), Info.Children);
		return JsonObj;
	}

	static TSharedPtr<FJsonObject> WidgetPropertyToJson(const FWidgetPropertyInfo& Info)
	{
		const TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
		JsonObj->SetStringField(TEXT("property_name"), Info.PropertyName);
		JsonObj->SetStringField(TEXT("property_type"), Info.PropertyType);
		JsonObj->SetStringField(TEXT("category"), Info.Category);
		JsonObj->SetStringField(TEXT("current_value"), Info.CurrentValue);
		JsonObj->SetBoolField(TEXT("is_editable"), Info.bIsEditable);
		JsonObj->SetBoolField(TEXT("is_blueprint_visible"), Info.bIsBlueprintVisible);
		return JsonObj;
	}

	static TSharedPtr<FJsonObject> WidgetEventToJson(const FWidgetEventInfo& Info)
	{
		const TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
		JsonObj->SetStringField(TEXT("event_name"), Info.EventName);
		JsonObj->SetStringField(TEXT("event_type"), Info.EventType);
		JsonObj->SetStringField(TEXT("description"), Info.Description);
		return JsonObj;
	}

	static TSharedPtr<FJsonObject> WidgetValidationToJson(const FWidgetValidationResult& Result)
	{
		const TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
		JsonObj->SetBoolField(TEXT("is_valid"), Result.bIsValid);
		JsonObj->SetStringField(TEXT("validation_message"), Result.ValidationMessage);
		AddStringArrayField(JsonObj, TEXT("errors"), Result.Errors);
		return JsonObj;
	}

	static TSharedPtr<FJsonObject> WidgetAddComponentToJson(const FWidgetAddComponentResult& Result)
	{
		const TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
		JsonObj->SetBoolField(TEXT("success"), Result.bSuccess);
		JsonObj->SetStringField(TEXT("component_name"), Result.ComponentName);
		JsonObj->SetStringField(TEXT("component_type"), Result.ComponentType);
		JsonObj->SetStringField(TEXT("parent_name"), Result.ParentName);
		JsonObj->SetBoolField(TEXT("is_variable"), Result.bIsVariable);
		JsonObj->SetStringField(TEXT("error_message"), Result.ErrorMessage);
		return JsonObj;
	}

	static TSharedPtr<FJsonObject> WidgetRemoveComponentToJson(const FWidgetRemoveComponentResult& Result)
	{
		const TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
		JsonObj->SetBoolField(TEXT("success"), Result.bSuccess);
		AddStringArrayField(JsonObj, TEXT("removed_components"), Result.RemovedComponents);
		AddStringArrayField(JsonObj, TEXT("orphaned_children"), Result.OrphanedChildren);
		JsonObj->SetStringField(TEXT("error_message"), Result.ErrorMessage);
		return JsonObj;
	}

	static TSharedPtr<FJsonObject> WidgetViewModelToJson(const FWidgetViewModelInfo& Info)
	{
		const TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
		JsonObj->SetStringField(TEXT("view_model_name"), Info.ViewModelName);
		JsonObj->SetStringField(TEXT("view_model_class_name"), Info.ViewModelClassName);
		JsonObj->SetStringField(TEXT("creation_type"), Info.CreationType);
		JsonObj->SetStringField(TEXT("view_model_id"), Info.ViewModelId);
		return JsonObj;
	}

	static TSharedPtr<FJsonObject> WidgetBindingToJson(const FWidgetViewModelBindingInfo& Info)
	{
		const TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
		JsonObj->SetNumberField(TEXT("binding_index"), Info.BindingIndex);
		JsonObj->SetStringField(TEXT("source_path"), Info.SourcePath);
		JsonObj->SetStringField(TEXT("destination_path"), Info.DestinationPath);
		JsonObj->SetStringField(TEXT("binding_mode"), Info.BindingMode);
		JsonObj->SetBoolField(TEXT("enabled"), Info.bEnabled);
		JsonObj->SetStringField(TEXT("binding_id"), Info.BindingId);
		return JsonObj;
	}

	static TArray<TSharedPtr<FJsonValue>> WidgetInfoArrayToJson(const TArray<FWidgetInfo>& Infos)
	{
		TArray<TSharedPtr<FJsonValue>> JsonArray;
		JsonArray.Reserve(Infos.Num());
		for (const FWidgetInfo& Info : Infos)
		{
			JsonArray.Add(MakeShared<FJsonValueObject>(WidgetInfoToJson(Info)));
		}
		return JsonArray;
	}

	static TArray<TSharedPtr<FJsonValue>> WidgetPropertyArrayToJson(const TArray<FWidgetPropertyInfo>& Infos)
	{
		TArray<TSharedPtr<FJsonValue>> JsonArray;
		JsonArray.Reserve(Infos.Num());
		for (const FWidgetPropertyInfo& Info : Infos)
		{
			JsonArray.Add(MakeShared<FJsonValueObject>(WidgetPropertyToJson(Info)));
		}
		return JsonArray;
	}

	static TArray<TSharedPtr<FJsonValue>> WidgetEventArrayToJson(const TArray<FWidgetEventInfo>& Infos)
	{
		TArray<TSharedPtr<FJsonValue>> JsonArray;
		JsonArray.Reserve(Infos.Num());
		for (const FWidgetEventInfo& Info : Infos)
		{
			JsonArray.Add(MakeShared<FJsonValueObject>(WidgetEventToJson(Info)));
		}
		return JsonArray;
	}

	static TArray<TSharedPtr<FJsonValue>> WidgetViewModelArrayToJson(const TArray<FWidgetViewModelInfo>& Infos)
	{
		TArray<TSharedPtr<FJsonValue>> JsonArray;
		JsonArray.Reserve(Infos.Num());
		for (const FWidgetViewModelInfo& Info : Infos)
		{
			JsonArray.Add(MakeShared<FJsonValueObject>(WidgetViewModelToJson(Info)));
		}
		return JsonArray;
	}

	static TArray<TSharedPtr<FJsonValue>> WidgetBindingArrayToJson(const TArray<FWidgetViewModelBindingInfo>& Infos)
	{
		TArray<TSharedPtr<FJsonValue>> JsonArray;
		JsonArray.Reserve(Infos.Num());
		for (const FWidgetViewModelBindingInfo& Info : Infos)
		{
			JsonArray.Add(MakeShared<FJsonValueObject>(WidgetBindingToJson(Info)));
		}
		return JsonArray;
	}

	static TSharedPtr<FJsonObject> BuildWidgetError(
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

	static TSharedPtr<FJsonObject> BuildWidgetSuccess(
		const FString& Action,
		const FString& Summary,
		const FString& WidgetPath = TEXT(""),
		TSharedPtr<FJsonObject> Data = nullptr,
		const TArray<FString>& ChangedItems = {},
		const TArray<FString>& VerificationHints = {},
		const TArray<FString>& Warnings = {},
		const TArray<FString>& AffectedAssets = {})
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
		AddStringArrayField(RootObj, TEXT("affected_assets"), AffectedAssets);
		return RootObj;
	}

	static FString NormalizeWidgetAction(const FString& RawAction)
	{
		const FString Action = RawAction.ToLower();
		if (Action == TEXT("create_asset"))
		{
			return TEXT("create");
		}
		if (Action == TEXT("list_widget_blueprints"))
		{
			return TEXT("list_assets");
		}
		if (Action == TEXT("widget_blueprint_exists"))
		{
			return TEXT("exists");
		}
		if (Action == TEXT("widget_exists"))
		{
			return TEXT("component_exists");
		}
		if (Action == TEXT("get_component_properties"))
		{
			return TEXT("list_properties");
		}
		if (Action == TEXT("list_view_model_bindings"))
		{
			return TEXT("list_bindings");
		}
		if (Action == TEXT("add_view_model_binding"))
		{
			return TEXT("add_binding");
		}
		if (Action == TEXT("remove_view_model_binding"))
		{
			return TEXT("remove_binding");
		}
		return Action;
	}

	static FString NormalizeComponentTypeInput(const FString& ComponentType)
	{
		FString Result = ComponentType.TrimStartAndEnd();
		if (Result.StartsWith(TEXT("[WBP] ")))
		{
			Result.RightChopInline(6);
			const int32 PathStartIndex = Result.Find(TEXT(" ("));
			if (PathStartIndex != INDEX_NONE)
			{
				Result = Result.Left(PathStartIndex);
			}
			Result.TrimEndInline();
		}
		return Result;
	}

	static bool ResolveWidgetAssetData(const FString& WidgetPath, FAssetData& OutAssetData)
	{
		if (WidgetPath.IsEmpty())
		{
			return false;
		}

		if (!UAssetDiscoveryService::FindAssetByPath(WidgetPath, OutAssetData))
		{
			return false;
		}

		return OutAssetData.AssetClassPath == WidgetBlueprintClassPath;
	}

	static bool LoadWidgetBlueprintAsset(const FString& WidgetPath, UWidgetBlueprint*& OutWidgetBlueprint, FString& OutCanonicalPath, TSharedPtr<FJsonObject>& OutError, const FString& Action)
	{
		FAssetData AssetData;
		if (!ResolveWidgetAssetData(WidgetPath, AssetData))
		{
			OutError = BuildWidgetError(
				Action,
				TEXT("WIDGET_BLUEPRINT_NOT_FOUND"),
				FString::Printf(TEXT("Widget Blueprint '%s' was not found."), *WidgetPath),
				WidgetPath,
				{ TEXT("Use action=list_assets to inspect available Widget Blueprints.") }
			);
			return false;
		}

		OutCanonicalPath = AssetData.GetObjectPathString();
		OutWidgetBlueprint = Cast<UWidgetBlueprint>(AssetData.GetAsset());
		if (!OutWidgetBlueprint)
		{
			OutWidgetBlueprint = Cast<UWidgetBlueprint>(UEditorAssetLibrary::LoadAsset(OutCanonicalPath));
		}
		if (!OutWidgetBlueprint)
		{
			const FString PackagePath = FPackageName::ObjectPathToPackageName(OutCanonicalPath);
			if (!PackagePath.IsEmpty())
			{
				OutWidgetBlueprint = Cast<UWidgetBlueprint>(UEditorAssetLibrary::LoadAsset(PackagePath));
			}
		}
		if (!OutWidgetBlueprint)
		{
			OutError = BuildWidgetError(
				Action,
				TEXT("WIDGET_BLUEPRINT_LOAD_FAILED"),
				FString::Printf(TEXT("Failed to load Widget Blueprint '%s'."), *OutCanonicalPath),
				OutCanonicalPath,
				{ TEXT("Open the asset in the editor and confirm it is not corrupted.") }
			);
			return false;
		}

		return true;
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

	static FProperty* FindWidgetProperty(UWidget* Widget, const FString& PropertyName)
	{
		if (!Widget || PropertyName.IsEmpty())
		{
			return nullptr;
		}

		FProperty* Property = Widget->GetClass()->FindPropertyByName(FName(*PropertyName));
		if (Property)
		{
			return Property;
		}

		for (TFieldIterator<FProperty> It(Widget->GetClass()); It; ++It)
		{
			if (It->GetName().Equals(PropertyName, ESearchCase::IgnoreCase))
			{
				return *It;
			}
		}

		return nullptr;
	}

	static FString NormalizePackagePath(const FString& InPath, const FString& DefaultPath = TEXT("/Game/UI"))
	{
		FString PackagePath = InPath;
		PackagePath.ReplaceInline(TEXT("\\"), TEXT("/"));
		PackagePath.TrimStartAndEndInline();
		while (PackagePath.EndsWith(TEXT("/")))
		{
			PackagePath.LeftChopInline(1);
		}
		if (PackagePath.IsEmpty())
		{
			PackagePath = DefaultPath;
		}
		return PackagePath;
	}

	static UClass* ResolveWidgetParentClass(const FString& ParentClassInput)
	{
		if (ParentClassInput.IsEmpty() || ParentClassInput.Equals(TEXT("UserWidget"), ESearchCase::IgnoreCase))
		{
			return UUserWidget::StaticClass();
		}

		if (GEditor)
		{
			if (UEditorAssetSubsystem* EditorAssetSubsystem = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>())
			{
				if (UClass* BlueprintClass = EditorAssetSubsystem->LoadBlueprintClass(ParentClassInput))
				{
					if (BlueprintClass->IsChildOf(UUserWidget::StaticClass()))
					{
						return BlueprintClass;
					}
				}
			}
		}

		if (UClass* DirectClass = FindObject<UClass>(nullptr, *ParentClassInput))
		{
			if (DirectClass->IsChildOf(UUserWidget::StaticClass()))
			{
				return DirectClass;
			}
		}

		if (UClass* LoadedClass = LoadClass<UUserWidget>(nullptr, *ParentClassInput))
		{
			if (LoadedClass->IsChildOf(UUserWidget::StaticClass()))
			{
				return LoadedClass;
			}
		}

		const TArray<FString> CandidatePaths = {
			FString::Printf(TEXT("/Script/UMG.%s"), *ParentClassInput),
			FString::Printf(TEXT("/Script/CommonUI.%s"), *ParentClassInput)
		};

		for (const FString& CandidatePath : CandidatePaths)
		{
			if (UClass* CandidateClass = FindObject<UClass>(nullptr, *CandidatePath))
			{
				if (CandidateClass->IsChildOf(UUserWidget::StaticClass()))
				{
					return CandidateClass;
				}
			}
		}

		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* TestClass = *It;
			if (!TestClass || !TestClass->IsChildOf(UUserWidget::StaticClass()))
			{
				continue;
			}

			if (TestClass->GetName().Equals(ParentClassInput, ESearchCase::IgnoreCase) ||
				TestClass->GetPathName().Equals(ParentClassInput, ESearchCase::IgnoreCase))
			{
				return TestClass;
			}
		}

		return nullptr;
	}

	static bool OpenWidgetAsset(const FString& WidgetPath)
	{
		if (!GEditor)
		{
			return false;
		}

		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			UObject* Asset = UEditorAssetLibrary::LoadAsset(WidgetPath);
			if (!Asset)
			{
				const FString PackagePath = FPackageName::ObjectPathToPackageName(WidgetPath);
				if (!PackagePath.IsEmpty())
				{
					Asset = UEditorAssetLibrary::LoadAsset(PackagePath);
				}
			}
			if (!Asset)
			{
				FAssetData AssetData;
				if (ResolveWidgetAssetData(WidgetPath, AssetData))
				{
					Asset = AssetData.GetAsset();
				}
			}
			return Asset != nullptr && AssetEditorSubsystem->OpenEditorForAsset(Asset);
		}

		return false;
	}

	static void ApplyPostWidgetActions(UWidgetBlueprint* WidgetBlueprint, const FString& WidgetPath, const TMap<FString, FString>& Params, TArray<FString>& InOutChangedItems, TArray<FString>& InOutWarnings)
	{
		if (!WidgetBlueprint)
		{
			return;
		}

		if (ExtractWidgetBoolParam(Params, TEXT("compile_after"), false))
		{
			FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
			InOutChangedItems.Add(TEXT("compiled widget blueprint"));
			if (WidgetBlueprint->Status == BS_Error)
			{
				InOutWarnings.Add(TEXT("Compile completed with errors. Open the Widget Blueprint and inspect the compiler results."));
			}
			else if (WidgetBlueprint->Status == BS_UpToDateWithWarnings)
			{
				InOutWarnings.Add(TEXT("Compile completed with warnings. Open the Widget Blueprint and inspect the compiler results."));
			}
		}

		if (ExtractWidgetBoolParam(Params, TEXT("save_after"), false))
		{
			if (UEditorAssetLibrary::SaveAsset(WidgetPath, false))
			{
				InOutChangedItems.Add(TEXT("saved widget blueprint"));
			}
			else
			{
				InOutWarnings.Add(TEXT("Failed to save the Widget Blueprint after applying changes."));
			}
		}

		if (ExtractWidgetBoolParam(Params, TEXT("open_after"), false))
		{
			if (OpenWidgetAsset(WidgetPath))
			{
				InOutChangedItems.Add(TEXT("opened widget blueprint editor"));
			}
			else
			{
				InOutWarnings.Add(TEXT("Failed to open the Widget Blueprint editor tab."));
			}
		}
	}

	static TSharedPtr<FJsonObject> BuildWidgetAssetMeta(UWidgetBlueprint* WidgetBlueprint, const FString& WidgetPath)
	{
		const TSharedPtr<FJsonObject> MetaObj = MakeShared<FJsonObject>();
		MetaObj->SetStringField(TEXT("widget_path"), WidgetPath);
		MetaObj->SetStringField(TEXT("widget_name"), WidgetBlueprint ? WidgetBlueprint->GetName() : TEXT(""));
		MetaObj->SetStringField(TEXT("blueprint_status"), WidgetBlueprint ? GetBlueprintStatusString(WidgetBlueprint->Status) : TEXT("Unknown"));
		MetaObj->SetBoolField(TEXT("is_up_to_date"), WidgetBlueprint ? WidgetBlueprint->IsUpToDate() : false);
		MetaObj->SetStringField(TEXT("root_widget"), UWidgetService::GetRootWidget(WidgetPath));
		return MetaObj;
	}

	static FString BuildWidgetHelpResponse()
	{
		const TSharedPtr<FJsonObject> HelpObj = MakeShared<FJsonObject>();
		HelpObj->SetBoolField(TEXT("success"), true);
		HelpObj->SetStringField(TEXT("tool"), TEXT("widget_blueprint"));
		HelpObj->SetStringField(TEXT("description"), TEXT("Structured Widget Blueprint editing and inspection. Supports discovery, hierarchy inspection, component/property updates, MVVM operations, and basic asset lifecycle actions."));

		TArray<TSharedPtr<FJsonValue>> ActionsArray;
		auto AddAction = [&ActionsArray](const FString& Name, const FString& Desc)
		{
			const TSharedPtr<FJsonObject> ActionObj = MakeShared<FJsonObject>();
			ActionObj->SetStringField(TEXT("action"), Name);
			ActionObj->SetStringField(TEXT("description"), Desc);
			ActionsArray.Add(MakeShared<FJsonValueObject>(ActionObj));
		};

		AddAction(TEXT("help"), TEXT("Show this help payload."));
		AddAction(TEXT("list_assets"), TEXT("List Widget Blueprint assets under a path filter."));
		AddAction(TEXT("search_types"), TEXT("List native widget types plus available custom Widget Blueprints."));
		AddAction(TEXT("create"), TEXT("Create a new Widget Blueprint asset."));
		AddAction(TEXT("describe"), TEXT("Inspect a Widget Blueprint: root widget, hierarchy, validation, MVVM state."));
		AddAction(TEXT("get_hierarchy"), TEXT("Return only the widget hierarchy tree."));
		AddAction(TEXT("get_root_widget"), TEXT("Return only the root widget name."));
		AddAction(TEXT("list_components"), TEXT("Return the flat widget component list."));
		AddAction(TEXT("exists"), TEXT("Check whether a Widget Blueprint asset exists."));
		AddAction(TEXT("component_exists"), TEXT("Check whether a widget component exists in a Widget Blueprint."));
		AddAction(TEXT("open"), TEXT("Open a Widget Blueprint in the editor."));
		AddAction(TEXT("save"), TEXT("Save a Widget Blueprint asset."));
		AddAction(TEXT("compile"), TEXT("Compile a Widget Blueprint and report status."));
		AddAction(TEXT("validate"), TEXT("Run hierarchy validation."));
		AddAction(TEXT("list_properties"), TEXT("List component properties."));
		AddAction(TEXT("get_property"), TEXT("Read one component property."));
		AddAction(TEXT("set_property"), TEXT("Update one component property."));
		AddAction(TEXT("get_available_events"), TEXT("List event hooks for a component or widget type."));
		AddAction(TEXT("bind_event"), TEXT("Record an event/delegate binding request for a specific component. Current implementation must still be verified manually in the graph."));
		AddAction(TEXT("add_component"), TEXT("Add a widget component to the hierarchy."));
		AddAction(TEXT("remove_component"), TEXT("Remove a widget component."));
		AddAction(TEXT("list_view_models"), TEXT("List MVVM view models."));
		AddAction(TEXT("add_view_model"), TEXT("Add an MVVM view model."));
		AddAction(TEXT("remove_view_model"), TEXT("Remove an MVVM view model."));
		AddAction(TEXT("list_bindings"), TEXT("List MVVM bindings."));
		AddAction(TEXT("add_binding"), TEXT("Add an MVVM binding."));
		AddAction(TEXT("remove_binding"), TEXT("Remove an MVVM binding by index."));

		HelpObj->SetArrayField(TEXT("actions"), ActionsArray);

		TArray<FString> Notes = {
			TEXT("Most mutating actions support compile_after, save_after, and open_after booleans."),
			TEXT("Use describe before edits to ground Codex/Cursor in the current widget hierarchy and MVVM state."),
			TEXT("bind_event currently records a component-scoped binding request and marks the asset modified, but it does not build full Blueprint graph wiring automatically.")
		};
		AddStringArrayField(HelpObj, TEXT("notes"), Notes);

		TArray<FString> Examples = {
			TEXT("action=create, asset_name=WBP_InventoryPanel, destination_path=/Game/UI"),
			TEXT("action=describe, widget_path=/Game/UI/WBP_InventoryPanel"),
			TEXT("action=add_component, widget_path=/Game/UI/WBP_InventoryPanel, component_type=Button, component_name=EquipButton, parent_name=CanvasPanel_0, compile_after=true"),
			TEXT("action=set_property, widget_path=/Game/UI/WBP_InventoryPanel, component_name=EquipButton, property_name=Visibility, property_value=Visible"),
			TEXT("action=add_view_model, widget_path=/Game/UI/WBP_InventoryPanel, view_model_class=VM_Inventory, view_model_name=InventoryVM"),
			TEXT("action=add_binding, widget_path=/Game/UI/WBP_InventoryPanel, view_model_name=InventoryVM, view_model_property=CurrentWeight, widget_name=WeightText, widget_property=Text")
		};
		AddStringArrayField(HelpObj, TEXT("examples"), Examples);

		return SerializeWidgetJson(HelpObj);
	}
}

REGISTER_VIBEUE_TOOL(widget_blueprint,
	"Structured Widget Blueprint editing and inspection. Actions: help, list_assets, search_types, create, describe, get_hierarchy, get_root_widget, list_components, exists, component_exists, open, save, compile, validate, list_properties, get_property, set_property, get_available_events, bind_event, add_component, remove_component, list_view_models, add_view_model, remove_view_model, list_bindings, add_binding, remove_binding. Mutating actions also support compile_after, save_after, and open_after booleans.",
	"Widget",
	TOOL_PARAMS(
		TOOL_PARAM("action", "Widget Blueprint operation to perform. Use help for documentation.", "string", true),
		TOOL_PARAM("widget_path", "Widget Blueprint asset path (package or object path). Required for most actions except list/search/create/help.", "string", false),
		TOOL_PARAM("path_filter", "[list_assets] Package path filter such as /Game/UI", "string", false),
		TOOL_PARAM("filter_text", "[search_types] Filter native/custom widget types by name substring", "string", false),
		TOOL_PARAM("asset_name", "[create] New Widget Blueprint asset name", "string", false),
		TOOL_PARAM("destination_path", "[create] Destination package path such as /Game/UI", "string", false),
		TOOL_PARAM("parent_class", "[create] Optional parent widget class name or asset path; defaults to UserWidget", "string", false),
		TOOL_PARAM("component_name", "[component/property actions] Widget component name", "string", false),
		TOOL_PARAM("component_type", "[add_component] Native widget type or custom Widget Blueprint name", "string", false),
		TOOL_PARAM("parent_name", "[add_component] Parent panel widget name; empty uses root panel", "string", false),
		TOOL_PARAM("remove_children", "[remove_component] Whether to also remove children", "boolean", false),
		TOOL_PARAM("is_variable", "[add_component] Whether the new component should be a variable", "boolean", false),
		TOOL_PARAM("property_name", "[property actions] Property name", "string", false),
		TOOL_PARAM("property_value", "[set_property] Property value to import as text", "string", false),
		TOOL_PARAM("editable_only", "[list_properties] Only return editable properties", "boolean", false),
		TOOL_PARAM("widget_type", "[get_available_events] Widget type name if component_name is not supplied", "string", false),
		TOOL_PARAM("event_name", "[bind_event] Event name to bind", "string", false),
		TOOL_PARAM("function_name", "[bind_event] Target function name", "string", false),
		TOOL_PARAM("view_model_class", "[add_view_model] ViewModel class name or asset path", "string", false),
		TOOL_PARAM("view_model_name", "[view model actions] Registered ViewModel name", "string", false),
		TOOL_PARAM("creation_type", "[add_view_model] CreateInstance, Manual, GlobalViewModelCollection, PropertyPath, or Resolver", "string", false),
		TOOL_PARAM("view_model_property", "[add_binding] Source ViewModel property name", "string", false),
		TOOL_PARAM("widget_name", "[add_binding] Destination widget/component name (alias of component_name)", "string", false),
		TOOL_PARAM("widget_property", "[add_binding] Destination widget property name", "string", false),
		TOOL_PARAM("binding_mode", "[add_binding] OneWayToDestination, TwoWay, OneTimeToDestination, OneWayToSource, or OneTimeToSource", "string", false),
		TOOL_PARAM("binding_index", "[remove_binding] Binding index from list_bindings", "number", false),
		TOOL_PARAM("max_results", "[list_assets] Maximum assets to return (default 100)", "number", false),
		TOOL_PARAM("compile_after", "[mutating actions] Compile the Widget Blueprint after the operation", "boolean", false),
		TOOL_PARAM("save_after", "[mutating actions] Save the Widget Blueprint after the operation", "boolean", false),
		TOOL_PARAM("open_after", "[create/mutating actions] Open the Widget Blueprint editor after the operation", "boolean", false)
	),
	{
		const FString Action = NormalizeWidgetAction(ExtractWidgetParam(Params, TEXT("action")));
		if (Action.IsEmpty())
		{
			return SerializeWidgetJson(BuildWidgetError(
				TEXT("unknown"),
				TEXT("MISSING_ACTION"),
				TEXT("The 'action' parameter is required. Use action=help for documentation.")
			));
		}

		if (Action == TEXT("help"))
		{
			return BuildWidgetHelpResponse();
		}

		if (Action == TEXT("list_assets"))
		{
			const FString PathFilter = ExtractWidgetParam(Params, TEXT("path_filter"));
			const int32 MaxResults = ExtractWidgetIntParam(Params, TEXT("max_results"), 100, 1, 500);

			IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
			FARFilter Filter;
			Filter.ClassPaths.Add(WidgetBlueprintClassPath);
			if (!PathFilter.IsEmpty())
			{
				Filter.PackagePaths.Add(FName(*PathFilter));
				Filter.bRecursivePaths = true;
			}

			TArray<FAssetData> AssetDataList;
			AssetRegistry.GetAssets(Filter, AssetDataList);

			TArray<TSharedPtr<FJsonValue>> AssetsArray;
			AssetsArray.Reserve(FMath::Min(MaxResults, AssetDataList.Num()));
			for (int32 Index = 0; Index < AssetDataList.Num() && Index < MaxResults; ++Index)
			{
				AssetsArray.Add(MakeShared<FJsonValueObject>(BuildAssetJson(AssetDataList[Index])));
			}

			const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			DataObj->SetNumberField(TEXT("asset_count"), AssetDataList.Num());
			DataObj->SetNumberField(TEXT("result_limit"), MaxResults);
			DataObj->SetNumberField(TEXT("truncated_count"), FMath::Max(0, AssetDataList.Num() - MaxResults));
			DataObj->SetArrayField(TEXT("assets"), AssetsArray);

			return SerializeWidgetJson(BuildWidgetSuccess(
				Action,
				FString::Printf(TEXT("Found %d Widget Blueprint asset(s)."), AssetDataList.Num()),
				TEXT(""),
				DataObj
			));
		}

		if (Action == TEXT("search_types"))
		{
			const FString FilterText = ExtractWidgetParam(Params, TEXT("filter_text"));
			const TArray<FString> RawTypes = UWidgetService::SearchTypes(FilterText);
			TArray<TSharedPtr<FJsonValue>> TypeArray;
			TypeArray.Reserve(RawTypes.Num());

			int32 NativeCount = 0;
			int32 CustomCount = 0;
			for (const FString& RawType : RawTypes)
			{
				const TSharedPtr<FJsonObject> TypeObj = MakeShared<FJsonObject>();
				if (RawType.StartsWith(TEXT("[WBP] ")))
				{
					FString AssetName = RawType.RightChop(6);
					FString AssetPath;
					const int32 PathStart = AssetName.Find(TEXT(" ("));
					const int32 PathEnd = AssetName.EndsWith(TEXT(")")) ? AssetName.Len() - 1 : INDEX_NONE;
					if (PathStart != INDEX_NONE && PathEnd != INDEX_NONE && PathEnd > PathStart + 2)
					{
						AssetPath = AssetName.Mid(PathStart + 2, PathEnd - PathStart - 2);
						AssetName = AssetName.Left(PathStart);
					}

					TypeObj->SetStringField(TEXT("kind"), TEXT("widget_blueprint"));
					TypeObj->SetStringField(TEXT("type_name"), AssetName);
					TypeObj->SetStringField(TEXT("widget_path"), AssetPath);
					++CustomCount;
				}
				else
				{
					TypeObj->SetStringField(TEXT("kind"), TEXT("native"));
					TypeObj->SetStringField(TEXT("type_name"), RawType);
					++NativeCount;
				}

				TypeArray.Add(MakeShared<FJsonValueObject>(TypeObj));
			}

			const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			DataObj->SetStringField(TEXT("filter_text"), FilterText);
			DataObj->SetNumberField(TEXT("native_count"), NativeCount);
			DataObj->SetNumberField(TEXT("custom_widget_blueprint_count"), CustomCount);
			DataObj->SetArrayField(TEXT("types"), TypeArray);

			return SerializeWidgetJson(BuildWidgetSuccess(
				Action,
				FString::Printf(TEXT("Resolved %d widget type option(s)."), RawTypes.Num()),
				TEXT(""),
				DataObj
			));
		}

		if (Action == TEXT("create"))
		{
			const FString AssetName = ExtractWidgetParam(Params, TEXT("asset_name"));
			const FString DestinationPath = NormalizePackagePath(ExtractWidgetParam(Params, TEXT("destination_path")));
			const FString ParentClassInput = ExtractWidgetParam(Params, TEXT("parent_class"));

			if (AssetName.IsEmpty())
			{
				return SerializeWidgetJson(BuildWidgetError(Action, TEXT("MISSING_ASSET_NAME"), TEXT("The 'asset_name' parameter is required for create.")));
			}

			const FString FullAssetPath = DestinationPath / AssetName;
			if (UEditorAssetLibrary::DoesAssetExist(FullAssetPath))
			{
				return SerializeWidgetJson(BuildWidgetError(
					Action,
					TEXT("ASSET_ALREADY_EXISTS"),
					FString::Printf(TEXT("Widget Blueprint already exists at '%s'."), *FullAssetPath),
					FullAssetPath,
					{ TEXT("Use a different asset_name or remove the existing asset first.") }
				));
			}

			UClass* ParentClass = ResolveWidgetParentClass(ParentClassInput);
			if (!ParentClass)
			{
				return SerializeWidgetJson(BuildWidgetError(
					Action,
					TEXT("PARENT_CLASS_NOT_FOUND"),
					FString::Printf(TEXT("Parent widget class '%s' could not be resolved."), *ParentClassInput),
					TEXT(""),
					{ TEXT("Use a UUserWidget subclass name, class path, or Widget Blueprint asset path.") }
				));
			}

			IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
			UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
			Factory->BlueprintType = BPTYPE_Normal;
			Factory->ParentClass = ParentClass;

			UObject* NewAsset = AssetTools.CreateAsset(AssetName, DestinationPath, UWidgetBlueprint::StaticClass(), Factory);
			UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(NewAsset);
			if (!WidgetBlueprint)
			{
				return SerializeWidgetJson(BuildWidgetError(
					Action,
					TEXT("CREATE_FAILED"),
					FString::Printf(TEXT("Failed to create Widget Blueprint '%s' in '%s'."), *AssetName, *DestinationPath)
				));
			}

			const FString CanonicalPath = WidgetBlueprint->GetPathName();
			UEditorAssetLibrary::SaveAsset(CanonicalPath, false);

			TArray<FString> ChangedItems = { FString::Printf(TEXT("created widget blueprint %s"), *CanonicalPath) };
			TArray<FString> Warnings;
			ApplyPostWidgetActions(WidgetBlueprint, CanonicalPath, Params, ChangedItems, Warnings);

			const TSharedPtr<FJsonObject> DataObj = BuildWidgetAssetMeta(WidgetBlueprint, CanonicalPath);
			DataObj->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());

			return SerializeWidgetJson(BuildWidgetSuccess(
				Action,
				FString::Printf(TEXT("Created Widget Blueprint %s."), *CanonicalPath),
				CanonicalPath,
				DataObj,
				ChangedItems,
				{
					TEXT("Open the Widget Blueprint designer and verify the default root widget/layout."),
					TEXT("If you plan to edit MVVM bindings next, run describe to inspect the initial state.")
				},
				Warnings,
				{ CanonicalPath }
			));
		}

		const FString RequestedWidgetPath = ExtractWidgetParam(Params, TEXT("widget_path"));
		UWidgetBlueprint* WidgetBlueprint = nullptr;
		FString CanonicalWidgetPath;
		TSharedPtr<FJsonObject> LoadError;

		const bool bActionNeedsWidget =
			Action != TEXT("search_types") &&
			Action != TEXT("list_assets") &&
			Action != TEXT("exists");

		if (bActionNeedsWidget)
		{
			if (RequestedWidgetPath.IsEmpty())
			{
				return SerializeWidgetJson(BuildWidgetError(
					Action,
					TEXT("MISSING_WIDGET_PATH"),
					TEXT("The 'widget_path' parameter is required for this action.")
				));
			}

			if (!LoadWidgetBlueprintAsset(RequestedWidgetPath, WidgetBlueprint, CanonicalWidgetPath, LoadError, Action))
			{
				return SerializeWidgetJson(LoadError);
			}
		}

		if (Action == TEXT("exists"))
		{
			FAssetData AssetData;
			const bool bExists = ResolveWidgetAssetData(RequestedWidgetPath, AssetData);
			if (bExists)
			{
				CanonicalWidgetPath = AssetData.GetObjectPathString();
			}
			const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			DataObj->SetBoolField(TEXT("exists"), bExists);
			DataObj->SetStringField(TEXT("resolved_widget_path"), CanonicalWidgetPath.IsEmpty() ? RequestedWidgetPath : CanonicalWidgetPath);
			return SerializeWidgetJson(BuildWidgetSuccess(
				Action,
				bExists
					? FString::Printf(TEXT("Widget Blueprint %s exists."), *CanonicalWidgetPath)
					: FString::Printf(TEXT("Widget Blueprint %s does not exist."), *RequestedWidgetPath),
				CanonicalWidgetPath,
				DataObj,
				{},
				{},
				{},
				bExists ? TArray<FString>{ CanonicalWidgetPath } : TArray<FString>{}
			));
		}

		if (Action == TEXT("component_exists"))
		{
			const FString ComponentName = ExtractWidgetParam(Params, TEXT("component_name"));
			if (ComponentName.IsEmpty())
			{
				return SerializeWidgetJson(BuildWidgetError(Action, TEXT("MISSING_COMPONENT_NAME"), TEXT("The 'component_name' parameter is required for component_exists."), CanonicalWidgetPath));
			}

			const bool bExists = UWidgetService::WidgetExists(CanonicalWidgetPath, ComponentName);
			const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			DataObj->SetBoolField(TEXT("exists"), bExists);
			DataObj->SetStringField(TEXT("component_name"), ComponentName);

			return SerializeWidgetJson(BuildWidgetSuccess(
				Action,
				bExists
					? FString::Printf(TEXT("Component %s exists in %s."), *ComponentName, *CanonicalWidgetPath)
					: FString::Printf(TEXT("Component %s was not found in %s."), *ComponentName, *CanonicalWidgetPath),
				CanonicalWidgetPath,
				DataObj,
				{},
				bExists ? TArray<FString>{} : TArray<FString>{ TEXT("Use action=describe or action=list_components to inspect the live widget hierarchy.") },
				{},
				{ CanonicalWidgetPath }
			));
		}

		if (Action == TEXT("open"))
		{
			const bool bOpened = OpenWidgetAsset(CanonicalWidgetPath);
			if (!bOpened)
			{
				return SerializeWidgetJson(BuildWidgetError(
					Action,
					TEXT("OPEN_FAILED"),
					FString::Printf(TEXT("Failed to open Widget Blueprint '%s'."), *CanonicalWidgetPath),
					CanonicalWidgetPath
				));
			}

			return SerializeWidgetJson(BuildWidgetSuccess(
				Action,
				FString::Printf(TEXT("Opened Widget Blueprint %s."), *CanonicalWidgetPath),
				CanonicalWidgetPath,
				BuildWidgetAssetMeta(WidgetBlueprint, CanonicalWidgetPath),
				{ TEXT("opened widget blueprint editor") },
				{},
				{},
				{ CanonicalWidgetPath }
			));
		}

		if (Action == TEXT("save"))
		{
			const bool bSaved = UAssetDiscoveryService::SaveAsset(CanonicalWidgetPath);
			if (!bSaved)
			{
				return SerializeWidgetJson(BuildWidgetError(
					Action,
					TEXT("SAVE_FAILED"),
					FString::Printf(TEXT("Failed to save Widget Blueprint '%s'."), *CanonicalWidgetPath),
					CanonicalWidgetPath
				));
			}

			return SerializeWidgetJson(BuildWidgetSuccess(
				Action,
				FString::Printf(TEXT("Saved Widget Blueprint %s."), *CanonicalWidgetPath),
				CanonicalWidgetPath,
				BuildWidgetAssetMeta(WidgetBlueprint, CanonicalWidgetPath),
				{ TEXT("saved widget blueprint") },
				{},
				{},
				{ CanonicalWidgetPath }
			));
		}

		if (Action == TEXT("compile"))
		{
			FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);

			TArray<FString> ChangedItems = { TEXT("compiled widget blueprint") };
			TArray<FString> Warnings;
			if (ExtractWidgetBoolParam(Params, TEXT("save_after"), false))
			{
				if (UAssetDiscoveryService::SaveAsset(CanonicalWidgetPath))
				{
					ChangedItems.Add(TEXT("saved widget blueprint"));
				}
				else
				{
					Warnings.Add(TEXT("Compile succeeded but saving the asset failed."));
				}
			}

			const TSharedPtr<FJsonObject> DataObj = BuildWidgetAssetMeta(WidgetBlueprint, CanonicalWidgetPath);

			TArray<FString> VerificationHints;
			VerificationHints.Add(TEXT("Open the Widget Blueprint compiler results and confirm there are no warnings/errors."));
			VerificationHints.Add(TEXT("Preview the widget in the designer to verify layout and MVVM binding behavior."));

			return SerializeWidgetJson(BuildWidgetSuccess(
				Action,
				FString::Printf(TEXT("Compiled Widget Blueprint %s (%s)."), *CanonicalWidgetPath, *GetBlueprintStatusString(WidgetBlueprint->Status)),
				CanonicalWidgetPath,
				DataObj,
				ChangedItems,
				VerificationHints,
				Warnings,
				{ CanonicalWidgetPath }
			));
		}

		if (Action == TEXT("validate"))
		{
			const FWidgetValidationResult Validation = UWidgetService::Validate(CanonicalWidgetPath);
			const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			DataObj->SetObjectField(TEXT("validation"), WidgetValidationToJson(Validation));
			DataObj->SetObjectField(TEXT("asset"), BuildWidgetAssetMeta(WidgetBlueprint, CanonicalWidgetPath));

			return SerializeWidgetJson(BuildWidgetSuccess(
				Action,
				Validation.bIsValid
					? FString::Printf(TEXT("Widget Blueprint %s passed validation."), *CanonicalWidgetPath)
					: FString::Printf(TEXT("Widget Blueprint %s has %d validation issue(s)."), *CanonicalWidgetPath, Validation.Errors.Num()),
				CanonicalWidgetPath,
				DataObj,
				{},
				Validation.bIsValid
					? TArray<FString>{ TEXT("Open the widget in designer/preview to verify layout and data flow visually.") }
					: TArray<FString>{ TEXT("Open the Widget Blueprint and fix the reported hierarchy/validation issues before further edits.") },
				{},
				{ CanonicalWidgetPath }
			));
		}

		if (Action == TEXT("get_root_widget"))
		{
			const FString RootWidget = UWidgetService::GetRootWidget(CanonicalWidgetPath);
			const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			DataObj->SetStringField(TEXT("root_widget"), RootWidget);
			DataObj->SetObjectField(TEXT("asset"), BuildWidgetAssetMeta(WidgetBlueprint, CanonicalWidgetPath));
			return SerializeWidgetJson(BuildWidgetSuccess(
				Action,
				RootWidget.IsEmpty()
					? FString::Printf(TEXT("Widget Blueprint %s has no root widget."), *CanonicalWidgetPath)
					: FString::Printf(TEXT("Root widget for %s is %s."), *CanonicalWidgetPath, *RootWidget),
				CanonicalWidgetPath,
				DataObj,
				{},
				{},
				{},
				{ CanonicalWidgetPath }
			));
		}

		if (Action == TEXT("get_hierarchy") || Action == TEXT("list_components") || Action == TEXT("describe"))
		{
			const TArray<FWidgetInfo> Hierarchy = UWidgetService::GetHierarchy(CanonicalWidgetPath);
			const TArray<FWidgetInfo> Components = UWidgetService::ListComponents(CanonicalWidgetPath);
			const TArray<FWidgetViewModelInfo> ViewModels = UWidgetService::ListViewModels(CanonicalWidgetPath);
			const TArray<FWidgetViewModelBindingInfo> Bindings = UWidgetService::ListViewModelBindings(CanonicalWidgetPath);
			const FWidgetValidationResult Validation = UWidgetService::Validate(CanonicalWidgetPath);

			const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			DataObj->SetObjectField(TEXT("asset"), BuildWidgetAssetMeta(WidgetBlueprint, CanonicalWidgetPath));
			DataObj->SetNumberField(TEXT("component_count"), Components.Num());

			if (Action == TEXT("get_hierarchy") || Action == TEXT("describe"))
			{
				DataObj->SetArrayField(TEXT("hierarchy"), WidgetInfoArrayToJson(Hierarchy));
			}

			if (Action == TEXT("list_components") || Action == TEXT("describe"))
			{
				DataObj->SetArrayField(TEXT("components"), WidgetInfoArrayToJson(Components));
			}

			if (Action == TEXT("describe"))
			{
				DataObj->SetObjectField(TEXT("validation"), WidgetValidationToJson(Validation));
				DataObj->SetArrayField(TEXT("view_models"), WidgetViewModelArrayToJson(ViewModels));
				DataObj->SetArrayField(TEXT("bindings"), WidgetBindingArrayToJson(Bindings));

				const FString ComponentName = ExtractWidgetParam(Params, TEXT("component_name"));
				if (!ComponentName.IsEmpty() && UWidgetService::WidgetExists(CanonicalWidgetPath, ComponentName))
				{
					DataObj->SetStringField(TEXT("focused_component"), ComponentName);
					DataObj->SetArrayField(TEXT("component_properties"), WidgetPropertyArrayToJson(UWidgetService::ListProperties(CanonicalWidgetPath, ComponentName, false)));
					DataObj->SetArrayField(TEXT("available_events"), WidgetEventArrayToJson(UWidgetService::GetAvailableEvents(CanonicalWidgetPath, ComponentName, TEXT(""))));
				}
			}

			return SerializeWidgetJson(BuildWidgetSuccess(
				Action,
				FString::Printf(TEXT("Inspected %s (%d component(s), %d ViewModel(s), %d binding(s))."), *CanonicalWidgetPath, Components.Num(), ViewModels.Num(), Bindings.Num()),
				CanonicalWidgetPath,
				DataObj,
				{},
				{
					TEXT("Open the Widget Blueprint designer to verify hierarchy, slot layout, and preview rendering."),
					TEXT("If MVVM bindings are present, inspect the View Bindings panel and validate runtime data flow.")
				},
				{},
				{ CanonicalWidgetPath }
			));
		}

		if (Action == TEXT("list_properties"))
		{
			const FString ComponentName = ExtractWidgetParam(Params, TEXT("component_name"));
			if (ComponentName.IsEmpty())
			{
				return SerializeWidgetJson(BuildWidgetError(Action, TEXT("MISSING_COMPONENT_NAME"), TEXT("The 'component_name' parameter is required for list_properties."), CanonicalWidgetPath));
			}

			if (!UWidgetService::WidgetExists(CanonicalWidgetPath, ComponentName))
			{
				return SerializeWidgetJson(BuildWidgetError(
					Action,
					TEXT("COMPONENT_NOT_FOUND"),
					FString::Printf(TEXT("Widget component '%s' was not found."), *ComponentName),
					CanonicalWidgetPath,
					{ TEXT("Use action=describe or action=list_components to inspect available component names.") }
				));
			}

			const bool bEditableOnly = ExtractWidgetBoolParam(Params, TEXT("editable_only"), false);
			const TArray<FWidgetPropertyInfo> Properties = UWidgetService::ListProperties(CanonicalWidgetPath, ComponentName, bEditableOnly);
			const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			DataObj->SetStringField(TEXT("component_name"), ComponentName);
			DataObj->SetBoolField(TEXT("editable_only"), bEditableOnly);
			DataObj->SetNumberField(TEXT("property_count"), Properties.Num());
			DataObj->SetArrayField(TEXT("properties"), WidgetPropertyArrayToJson(Properties));

			return SerializeWidgetJson(BuildWidgetSuccess(
				Action,
				FString::Printf(TEXT("Listed %d propertie(s) for %s."), Properties.Num(), *ComponentName),
				CanonicalWidgetPath,
				DataObj,
				{},
				{},
				{},
				{ CanonicalWidgetPath }
			));
		}

		if (Action == TEXT("get_property"))
		{
			const FString ComponentName = ExtractWidgetParam(Params, TEXT("component_name"));
			const FString PropertyName = ExtractWidgetParam(Params, TEXT("property_name"));
			if (ComponentName.IsEmpty() || PropertyName.IsEmpty())
			{
				return SerializeWidgetJson(BuildWidgetError(Action, TEXT("MISSING_PARAMETERS"), TEXT("Both 'component_name' and 'property_name' are required for get_property."), CanonicalWidgetPath));
			}

			UWidget* Widget = FindWidgetByName(WidgetBlueprint, ComponentName);
			if (!Widget)
			{
				return SerializeWidgetJson(BuildWidgetError(Action, TEXT("COMPONENT_NOT_FOUND"), FString::Printf(TEXT("Widget component '%s' was not found."), *ComponentName), CanonicalWidgetPath));
			}

			if (PropertyName.Equals(TEXT("bIsVariable"), ESearchCase::IgnoreCase))
			{
				const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
				DataObj->SetStringField(TEXT("component_name"), ComponentName);
				DataObj->SetStringField(TEXT("property_name"), TEXT("bIsVariable"));
				DataObj->SetStringField(TEXT("property_type"), TEXT("bool"));
				DataObj->SetStringField(TEXT("property_value"), Widget->bIsVariable ? TEXT("True") : TEXT("False"));

				return SerializeWidgetJson(BuildWidgetSuccess(
					Action,
					FString::Printf(TEXT("Read %s.%s."), *ComponentName, TEXT("bIsVariable")),
					CanonicalWidgetPath,
					DataObj,
					{},
					{},
					{},
					{ CanonicalWidgetPath }
				));
			}

			FProperty* Property = FindWidgetProperty(Widget, PropertyName);
			if (!Property)
			{
				return SerializeWidgetJson(BuildWidgetError(Action, TEXT("PROPERTY_NOT_FOUND"), FString::Printf(TEXT("Property '%s' was not found on component '%s'."), *PropertyName, *ComponentName), CanonicalWidgetPath));
			}

			const FString PropertyValue = UWidgetService::GetProperty(CanonicalWidgetPath, ComponentName, PropertyName);
			const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			DataObj->SetStringField(TEXT("component_name"), ComponentName);
			DataObj->SetStringField(TEXT("property_name"), Property->GetName());
			DataObj->SetStringField(TEXT("property_type"), Property->GetCPPType());
			DataObj->SetStringField(TEXT("property_value"), PropertyValue);

			return SerializeWidgetJson(BuildWidgetSuccess(
				Action,
				FString::Printf(TEXT("Read %s.%s."), *ComponentName, *Property->GetName()),
				CanonicalWidgetPath,
				DataObj,
				{},
				{},
				{},
				{ CanonicalWidgetPath }
			));
		}

		if (Action == TEXT("set_property"))
		{
			const FString ComponentName = ExtractWidgetParam(Params, TEXT("component_name"));
			const FString PropertyName = ExtractWidgetParam(Params, TEXT("property_name"));
			const FString PropertyValue = ExtractWidgetParam(Params, TEXT("property_value"));
			if (ComponentName.IsEmpty() || PropertyName.IsEmpty())
			{
				return SerializeWidgetJson(BuildWidgetError(Action, TEXT("MISSING_PARAMETERS"), TEXT("Both 'component_name' and 'property_name' are required for set_property."), CanonicalWidgetPath));
			}

			UWidget* Widget = FindWidgetByName(WidgetBlueprint, ComponentName);
			if (!Widget)
			{
				return SerializeWidgetJson(BuildWidgetError(Action, TEXT("COMPONENT_NOT_FOUND"), FString::Printf(TEXT("Widget component '%s' was not found."), *ComponentName), CanonicalWidgetPath));
			}

			if (PropertyName.Equals(TEXT("bIsVariable"), ESearchCase::IgnoreCase))
			{
				const bool bIsVariable = PropertyValue.Equals(TEXT("true"), ESearchCase::IgnoreCase)
					|| PropertyValue.Equals(TEXT("1"))
					|| PropertyValue.Equals(TEXT("yes"), ESearchCase::IgnoreCase);

				WidgetBlueprint->Modify();
				Widget->Modify();
				Widget->bIsVariable = bIsVariable;
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

				TArray<FString> ChangedItems = {
					FString::Printf(TEXT("set %s.%s = %s"), *ComponentName, TEXT("bIsVariable"), bIsVariable ? TEXT("True") : TEXT("False"))
				};
				TArray<FString> Warnings;
				ApplyPostWidgetActions(WidgetBlueprint, CanonicalWidgetPath, Params, ChangedItems, Warnings);

				const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
				DataObj->SetStringField(TEXT("component_name"), ComponentName);
				DataObj->SetStringField(TEXT("property_name"), TEXT("bIsVariable"));
				DataObj->SetStringField(TEXT("property_value"), Widget->bIsVariable ? TEXT("True") : TEXT("False"));

				return SerializeWidgetJson(BuildWidgetSuccess(
					Action,
					FString::Printf(TEXT("Updated %s.%s in %s."), *ComponentName, TEXT("bIsVariable"), *CanonicalWidgetPath),
					CanonicalWidgetPath,
					DataObj,
					ChangedItems,
					{},
					Warnings,
					{ CanonicalWidgetPath }
				));
			}

			FProperty* Property = FindWidgetProperty(Widget, PropertyName);
			if (!Property)
			{
				return SerializeWidgetJson(BuildWidgetError(Action, TEXT("PROPERTY_NOT_FOUND"), FString::Printf(TEXT("Property '%s' was not found on component '%s'."), *PropertyName, *ComponentName), CanonicalWidgetPath));
			}

			if (!Property->HasAnyPropertyFlags(CPF_Edit))
			{
				return SerializeWidgetJson(BuildWidgetError(Action, TEXT("PROPERTY_NOT_EDITABLE"), FString::Printf(TEXT("Property '%s' is not editable on component '%s'."), *Property->GetName(), *ComponentName), CanonicalWidgetPath));
			}

			if (!UWidgetService::SetProperty(CanonicalWidgetPath, ComponentName, PropertyName, PropertyValue))
			{
				return SerializeWidgetJson(BuildWidgetError(
					Action,
					TEXT("SET_PROPERTY_FAILED"),
					FString::Printf(TEXT("Failed to set %s.%s to '%s'."), *ComponentName, *Property->GetName(), *PropertyValue),
					CanonicalWidgetPath,
					{ TEXT("Use action=list_properties to inspect the expected property type and current value format.") }
				));
			}

			TArray<FString> ChangedItems = {
				FString::Printf(TEXT("set %s.%s = %s"), *ComponentName, *Property->GetName(), *PropertyValue)
			};
			TArray<FString> Warnings;
			ApplyPostWidgetActions(WidgetBlueprint, CanonicalWidgetPath, Params, ChangedItems, Warnings);

			const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			DataObj->SetStringField(TEXT("component_name"), ComponentName);
			DataObj->SetStringField(TEXT("property_name"), Property->GetName());
			DataObj->SetStringField(TEXT("property_value"), UWidgetService::GetProperty(CanonicalWidgetPath, ComponentName, Property->GetName()));

			return SerializeWidgetJson(BuildWidgetSuccess(
				Action,
				FString::Printf(TEXT("Updated %s.%s in %s."), *ComponentName, *Property->GetName(), *CanonicalWidgetPath),
				CanonicalWidgetPath,
				DataObj,
				ChangedItems,
				{
					TEXT("Open the Widget Blueprint designer/details panel and confirm the component property value."),
					TEXT("If the property affects runtime appearance, preview the widget or run the screen it belongs to.")
				},
				Warnings,
				{ CanonicalWidgetPath }
			));
		}

		if (Action == TEXT("get_available_events"))
		{
			const FString ComponentName = ExtractWidgetParam(Params, TEXT("component_name"));
			const FString WidgetType = ExtractWidgetParam(Params, TEXT("widget_type"));
			const TArray<FWidgetEventInfo> Events = UWidgetService::GetAvailableEvents(CanonicalWidgetPath, ComponentName, WidgetType);
			const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			DataObj->SetStringField(TEXT("component_name"), ComponentName);
			DataObj->SetStringField(TEXT("widget_type"), WidgetType);
			DataObj->SetNumberField(TEXT("event_count"), Events.Num());
			DataObj->SetArrayField(TEXT("events"), WidgetEventArrayToJson(Events));

			return SerializeWidgetJson(BuildWidgetSuccess(
				Action,
				FString::Printf(TEXT("Resolved %d event option(s) for %s."), Events.Num(), ComponentName.IsEmpty() ? *CanonicalWidgetPath : *ComponentName),
				CanonicalWidgetPath,
				DataObj,
				{},
				{},
				{},
				{ CanonicalWidgetPath }
			));
		}

		if (Action == TEXT("bind_event"))
		{
			const FString ComponentName = ExtractWidgetParam(Params, TEXT("component_name"));
			const FString EventName = ExtractWidgetParam(Params, TEXT("event_name"));
			const FString FunctionName = ExtractWidgetParam(Params, TEXT("function_name"));
			if (EventName.IsEmpty() || FunctionName.IsEmpty())
			{
				return SerializeWidgetJson(BuildWidgetError(Action, TEXT("MISSING_PARAMETERS"), TEXT("Both 'event_name' and 'function_name' are required for bind_event."), CanonicalWidgetPath));
			}

			if (!UWidgetService::BindEvent(CanonicalWidgetPath, ComponentName, EventName, FunctionName))
			{
				return SerializeWidgetJson(BuildWidgetError(Action, TEXT("BIND_EVENT_FAILED"), FString::Printf(TEXT("Failed to bind event '%s' to function '%s'."), *EventName, *FunctionName), CanonicalWidgetPath));
			}

			TArray<FString> ChangedItems = {
				FString::Printf(TEXT("requested binding %s.%s -> %s"), ComponentName.IsEmpty() ? TEXT("(widget)") : *ComponentName, *EventName, *FunctionName)
			};
			TArray<FString> Warnings = {
				TEXT("Underlying bind_event support is currently a placeholder; it marks the Widget Blueprint modified but does not author full Blueprint graph wiring automatically.")
			};
			ApplyPostWidgetActions(WidgetBlueprint, CanonicalWidgetPath, Params, ChangedItems, Warnings);

			const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			DataObj->SetStringField(TEXT("component_name"), ComponentName);
			DataObj->SetStringField(TEXT("event_name"), EventName);
			DataObj->SetStringField(TEXT("function_name"), FunctionName);

			return SerializeWidgetJson(BuildWidgetSuccess(
				Action,
				FString::Printf(TEXT("Recorded bind_event request %s.%s -> %s for %s."), ComponentName.IsEmpty() ? TEXT("(widget)") : *ComponentName, *EventName, *FunctionName, *CanonicalWidgetPath),
				CanonicalWidgetPath,
				DataObj,
				ChangedItems,
				{
					TEXT("Open the Widget Blueprint event graph and verify or manually author the actual event node wiring."),
					TEXT("Recompile the Widget Blueprint and test the target interaction in-game.")
				},
				Warnings,
				{ CanonicalWidgetPath }
			));
		}

		if (Action == TEXT("add_component"))
		{
			const FString ComponentType = NormalizeComponentTypeInput(ExtractWidgetParam(Params, TEXT("component_type")));
			const FString ComponentName = ExtractWidgetParam(Params, TEXT("component_name"));
			const FString ParentName = ExtractWidgetParam(Params, TEXT("parent_name"));
			const bool bIsVariable = ExtractWidgetBoolParam(Params, TEXT("is_variable"), true);
			if (ComponentType.IsEmpty() || ComponentName.IsEmpty())
			{
				return SerializeWidgetJson(BuildWidgetError(Action, TEXT("MISSING_PARAMETERS"), TEXT("Both 'component_type' and 'component_name' are required for add_component."), CanonicalWidgetPath));
			}

			const FWidgetAddComponentResult Result = UWidgetService::AddComponent(CanonicalWidgetPath, ComponentType, ComponentName, ParentName, bIsVariable);
			if (!Result.bSuccess)
			{
				return SerializeWidgetJson(BuildWidgetError(
					Action,
					TEXT("ADD_COMPONENT_FAILED"),
					Result.ErrorMessage.IsEmpty() ? FString::Printf(TEXT("Failed to add component '%s'."), *ComponentName) : Result.ErrorMessage,
					CanonicalWidgetPath,
					{ TEXT("Use action=search_types to inspect valid component types and action=describe to inspect parent panel names.") }
				));
			}

			TArray<FString> ChangedItems = {
				FString::Printf(TEXT("added component %s (%s) under %s"), *Result.ComponentName, *Result.ComponentType, Result.ParentName.IsEmpty() ? TEXT("(root)") : *Result.ParentName)
			};
			TArray<FString> Warnings;
			ApplyPostWidgetActions(WidgetBlueprint, CanonicalWidgetPath, Params, ChangedItems, Warnings);

			return SerializeWidgetJson(BuildWidgetSuccess(
				Action,
				FString::Printf(TEXT("Added component %s to %s."), *Result.ComponentName, *CanonicalWidgetPath),
				CanonicalWidgetPath,
				WidgetAddComponentToJson(Result),
				ChangedItems,
				{
					TEXT("Open the Widget Blueprint designer and confirm the hierarchy placement and slot layout."),
					TEXT("If the new component is interactive, preview the widget and verify focus/input behavior.")
				},
				Warnings,
				{ CanonicalWidgetPath }
			));
		}

		if (Action == TEXT("remove_component"))
		{
			const FString ComponentName = ExtractWidgetParam(Params, TEXT("component_name"));
			const bool bRemoveChildren = ExtractWidgetBoolParam(Params, TEXT("remove_children"), false);
			if (ComponentName.IsEmpty())
			{
				return SerializeWidgetJson(BuildWidgetError(Action, TEXT("MISSING_COMPONENT_NAME"), TEXT("The 'component_name' parameter is required for remove_component."), CanonicalWidgetPath));
			}

			const FWidgetRemoveComponentResult Result = UWidgetService::RemoveComponent(CanonicalWidgetPath, ComponentName, bRemoveChildren);
			if (!Result.bSuccess)
			{
				return SerializeWidgetJson(BuildWidgetError(
					Action,
					TEXT("REMOVE_COMPONENT_FAILED"),
					Result.ErrorMessage.IsEmpty() ? FString::Printf(TEXT("Failed to remove component '%s'."), *ComponentName) : Result.ErrorMessage,
					CanonicalWidgetPath,
					{ TEXT("Use action=describe to verify the component name and review any child widgets that would be orphaned.") }
				));
			}

			TArray<FString> ChangedItems;
			for (const FString& RemovedComponent : Result.RemovedComponents)
			{
				ChangedItems.Add(FString::Printf(TEXT("removed component %s"), *RemovedComponent));
			}
			for (const FString& OrphanedChild : Result.OrphanedChildren)
			{
				ChangedItems.Add(FString::Printf(TEXT("orphaned child %s"), *OrphanedChild));
			}
			TArray<FString> Warnings;
			ApplyPostWidgetActions(WidgetBlueprint, CanonicalWidgetPath, Params, ChangedItems, Warnings);

			return SerializeWidgetJson(BuildWidgetSuccess(
				Action,
				FString::Printf(TEXT("Removed component %s from %s."), *ComponentName, *CanonicalWidgetPath),
				CanonicalWidgetPath,
				WidgetRemoveComponentToJson(Result),
				ChangedItems,
				{
					TEXT("Open the Widget Blueprint designer and confirm the hierarchy/layout after removal."),
					TEXT("If child widgets were orphaned, verify whether they should be reattached or deleted.")
				},
				Warnings,
				{ CanonicalWidgetPath }
			));
		}

		if (Action == TEXT("list_view_models"))
		{
			const TArray<FWidgetViewModelInfo> ViewModels = UWidgetService::ListViewModels(CanonicalWidgetPath);
			const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			DataObj->SetNumberField(TEXT("view_model_count"), ViewModels.Num());
			DataObj->SetArrayField(TEXT("view_models"), WidgetViewModelArrayToJson(ViewModels));
			return SerializeWidgetJson(BuildWidgetSuccess(
				Action,
				FString::Printf(TEXT("Listed %d ViewModel(s) for %s."), ViewModels.Num(), *CanonicalWidgetPath),
				CanonicalWidgetPath,
				DataObj,
				{},
				{},
				{},
				{ CanonicalWidgetPath }
			));
		}

		if (Action == TEXT("add_view_model"))
		{
			const FString ViewModelClass = ExtractWidgetParam(Params, TEXT("view_model_class"));
			const FString ViewModelName = ExtractWidgetParam(Params, TEXT("view_model_name"));
			const FString CreationType = ExtractWidgetParam(Params, TEXT("creation_type"));
			if (ViewModelClass.IsEmpty() || ViewModelName.IsEmpty())
			{
				return SerializeWidgetJson(BuildWidgetError(Action, TEXT("MISSING_PARAMETERS"), TEXT("Both 'view_model_class' and 'view_model_name' are required for add_view_model."), CanonicalWidgetPath));
			}

			if (!UWidgetService::AddViewModel(CanonicalWidgetPath, ViewModelClass, ViewModelName, CreationType.IsEmpty() ? TEXT("CreateInstance") : CreationType))
			{
				return SerializeWidgetJson(BuildWidgetError(
					Action,
					TEXT("ADD_VIEW_MODEL_FAILED"),
					FString::Printf(TEXT("Failed to add ViewModel '%s' (%s)."), *ViewModelName, *ViewModelClass),
					CanonicalWidgetPath,
					{ TEXT("Ensure the ViewModel class inherits from MVVMViewModelBase and is compiled.") }
				));
			}

			TArray<FString> ChangedItems = {
				FString::Printf(TEXT("added ViewModel %s (%s)"), *ViewModelName, *ViewModelClass)
			};
			TArray<FString> Warnings;
			ApplyPostWidgetActions(WidgetBlueprint, CanonicalWidgetPath, Params, ChangedItems, Warnings);

			const TArray<FWidgetViewModelInfo> ViewModels = UWidgetService::ListViewModels(CanonicalWidgetPath);
			const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			DataObj->SetArrayField(TEXT("view_models"), WidgetViewModelArrayToJson(ViewModels));

			return SerializeWidgetJson(BuildWidgetSuccess(
				Action,
				FString::Printf(TEXT("Added ViewModel %s to %s."), *ViewModelName, *CanonicalWidgetPath),
				CanonicalWidgetPath,
				DataObj,
				ChangedItems,
				{
					TEXT("Open the View Bindings panel and verify the new ViewModel registration and creation mode."),
					TEXT("If the widget uses runtime initialization, test the screen in PIE to confirm the ViewModel resolves correctly.")
				},
				Warnings,
				{ CanonicalWidgetPath }
			));
		}

		if (Action == TEXT("remove_view_model"))
		{
			const FString ViewModelName = ExtractWidgetParam(Params, TEXT("view_model_name"));
			if (ViewModelName.IsEmpty())
			{
				return SerializeWidgetJson(BuildWidgetError(Action, TEXT("MISSING_VIEW_MODEL_NAME"), TEXT("The 'view_model_name' parameter is required for remove_view_model."), CanonicalWidgetPath));
			}

			if (!UWidgetService::RemoveViewModel(CanonicalWidgetPath, ViewModelName))
			{
				return SerializeWidgetJson(BuildWidgetError(
					Action,
					TEXT("REMOVE_VIEW_MODEL_FAILED"),
					FString::Printf(TEXT("Failed to remove ViewModel '%s'."), *ViewModelName),
					CanonicalWidgetPath
				));
			}

			TArray<FString> ChangedItems = {
				FString::Printf(TEXT("removed ViewModel %s"), *ViewModelName)
			};
			TArray<FString> Warnings;
			ApplyPostWidgetActions(WidgetBlueprint, CanonicalWidgetPath, Params, ChangedItems, Warnings);

			return SerializeWidgetJson(BuildWidgetSuccess(
				Action,
				FString::Printf(TEXT("Removed ViewModel %s from %s."), *ViewModelName, *CanonicalWidgetPath),
				CanonicalWidgetPath,
				nullptr,
				ChangedItems,
				{
					TEXT("Open the View Bindings panel and verify any bindings that referenced the removed ViewModel are cleaned up."),
					TEXT("Test the widget in PIE to ensure no runtime ViewModel resolution errors remain.")
				},
				Warnings,
				{ CanonicalWidgetPath }
			));
		}

		if (Action == TEXT("list_bindings"))
		{
			const TArray<FWidgetViewModelBindingInfo> Bindings = UWidgetService::ListViewModelBindings(CanonicalWidgetPath);
			const TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
			DataObj->SetNumberField(TEXT("binding_count"), Bindings.Num());
			DataObj->SetArrayField(TEXT("bindings"), WidgetBindingArrayToJson(Bindings));

			return SerializeWidgetJson(BuildWidgetSuccess(
				Action,
				FString::Printf(TEXT("Listed %d binding(s) for %s."), Bindings.Num(), *CanonicalWidgetPath),
				CanonicalWidgetPath,
				DataObj,
				{},
				{},
				{},
				{ CanonicalWidgetPath }
			));
		}

		if (Action == TEXT("add_binding"))
		{
			const FString ViewModelName = ExtractWidgetParam(Params, TEXT("view_model_name"));
			const FString ViewModelProperty = ExtractWidgetParam(Params, TEXT("view_model_property"));
			const FString WidgetName = ExtractWidgetParam(Params, TEXT("component_name")).IsEmpty()
				? ExtractWidgetParam(Params, TEXT("widget_name"))
				: ExtractWidgetParam(Params, TEXT("component_name"));
			const FString WidgetProperty = ExtractWidgetParam(Params, TEXT("widget_property"));
			const FString BindingMode = ExtractWidgetParam(Params, TEXT("binding_mode"));

			if (ViewModelName.IsEmpty() || ViewModelProperty.IsEmpty() || WidgetName.IsEmpty() || WidgetProperty.IsEmpty())
			{
				return SerializeWidgetJson(BuildWidgetError(Action, TEXT("MISSING_PARAMETERS"), TEXT("'view_model_name', 'view_model_property', 'widget_name'/'component_name', and 'widget_property' are required for add_binding."), CanonicalWidgetPath));
			}

			if (!UWidgetService::AddViewModelBinding(CanonicalWidgetPath, ViewModelName, ViewModelProperty, WidgetName, WidgetProperty, BindingMode.IsEmpty() ? TEXT("OneWayToDestination") : BindingMode))
			{
				return SerializeWidgetJson(BuildWidgetError(
					Action,
					TEXT("ADD_BINDING_FAILED"),
					FString::Printf(TEXT("Failed to add binding %s.%s -> %s.%s."), *ViewModelName, *ViewModelProperty, *WidgetName, *WidgetProperty),
					CanonicalWidgetPath,
					{ TEXT("Ensure the ViewModel exists, both source/destination properties are valid, and the binding mode is supported.") }
				));
			}

			TArray<FString> ChangedItems = {
				FString::Printf(TEXT("added binding %s.%s -> %s.%s"), *ViewModelName, *ViewModelProperty, *WidgetName, *WidgetProperty)
			};
			TArray<FString> Warnings;
			ApplyPostWidgetActions(WidgetBlueprint, CanonicalWidgetPath, Params, ChangedItems, Warnings);

			return SerializeWidgetJson(BuildWidgetSuccess(
				Action,
				FString::Printf(TEXT("Added MVVM binding %s.%s -> %s.%s."), *ViewModelName, *ViewModelProperty, *WidgetName, *WidgetProperty),
				CanonicalWidgetPath,
				nullptr,
				ChangedItems,
				{
					TEXT("Open the View Bindings panel and verify the binding direction and field paths."),
					TEXT("Preview the widget or run PIE to confirm the destination property updates as expected.")
				},
				Warnings,
				{ CanonicalWidgetPath }
			));
		}

		if (Action == TEXT("remove_binding"))
		{
			const int32 BindingIndex = ExtractWidgetIntParam(Params, TEXT("binding_index"), INDEX_NONE, INDEX_NONE, 100000);
			if (BindingIndex == INDEX_NONE)
			{
				return SerializeWidgetJson(BuildWidgetError(Action, TEXT("MISSING_BINDING_INDEX"), TEXT("The 'binding_index' parameter is required for remove_binding."), CanonicalWidgetPath));
			}

			if (!UWidgetService::RemoveViewModelBinding(CanonicalWidgetPath, BindingIndex))
			{
				return SerializeWidgetJson(BuildWidgetError(
					Action,
					TEXT("REMOVE_BINDING_FAILED"),
					FString::Printf(TEXT("Failed to remove binding at index %d."), BindingIndex),
					CanonicalWidgetPath,
					{ TEXT("Use action=list_bindings to inspect valid binding indices.") }
				));
			}

			TArray<FString> ChangedItems = {
				FString::Printf(TEXT("removed binding index %d"), BindingIndex)
			};
			TArray<FString> Warnings;
			ApplyPostWidgetActions(WidgetBlueprint, CanonicalWidgetPath, Params, ChangedItems, Warnings);

			return SerializeWidgetJson(BuildWidgetSuccess(
				Action,
				FString::Printf(TEXT("Removed binding index %d from %s."), BindingIndex, *CanonicalWidgetPath),
				CanonicalWidgetPath,
				nullptr,
				ChangedItems,
				{
					TEXT("Open the View Bindings panel and confirm the correct binding row was removed."),
					TEXT("Run PIE or preview the widget to confirm the target property no longer updates from the removed binding.")
				},
				Warnings,
				{ CanonicalWidgetPath }
			));
		}

		return SerializeWidgetJson(BuildWidgetError(
			Action,
			TEXT("UNKNOWN_ACTION"),
			FString::Printf(TEXT("Unknown widget_blueprint action '%s'. Use action=help for documentation."), *Action)
		));
	}
);
