// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Core/ToolRegistry.h"
#include "PythonAPI/UAssetDiscoveryService.h"
#include "PythonAPI/UWidgetService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Blueprint/UserWidget.h"
#include "Components/BorderSlot.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/GridSlot.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/OverlaySlot.h"
#include "Components/PanelWidget.h"
#include "Components/ScaleBoxSlot.h"
#include "Components/ScrollBoxSlot.h"
#include "Components/SizeBoxSlot.h"
#include "Components/UniformGridSlot.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/Widget.h"
#include "Components/WrapBoxSlot.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "Json.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/Optional.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/PropertyIterator.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"

namespace
{
	static const FTopLevelAssetPath WidgetBlueprintClassPath(TEXT("/Script/UMGEditor.WidgetBlueprint"));

	struct FWidgetSpecNodeData
	{
		FString Name;
		FString Type;
		FString Parent;
		bool bIsVariable = false;
	};

	static FString ExtractParam(const TMap<FString, FString>& Params, const FString& FieldName)
	{
		if (const FString* DirectValue = Params.Find(FieldName))
		{
			return *DirectValue;
		}

		FString CapitalizedField = FieldName;
		if (!CapitalizedField.IsEmpty())
		{
			CapitalizedField[0] = FChar::ToUpper(CapitalizedField[0]);
		}

		if (const FString* CapitalizedValue = Params.Find(CapitalizedField))
		{
			return *CapitalizedValue;
		}

		const FString* ParamsJsonString = Params.Find(TEXT("ParamsJson"));
		if (!ParamsJsonString)
		{
			return FString();
		}

		TSharedPtr<FJsonObject> JsonObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(*ParamsJsonString);
		if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
		{
			return FString();
		}

		FString Value;
		if (JsonObject->TryGetStringField(FieldName, Value))
		{
			return Value;
		}

		return FString();
	}

	static bool ExtractBoolParam(const TMap<FString, FString>& Params, const FString& FieldName, bool bDefaultValue)
	{
		const FString Value = ExtractParam(Params, FieldName);
		if (Value.IsEmpty())
		{
			return bDefaultValue;
		}

		return Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("1"));
	}

	static TOptional<bool> TryExtractBoolParam(const TMap<FString, FString>& Params, const FString& FieldName)
	{
		const FString Value = ExtractParam(Params, FieldName);
		if (Value.IsEmpty())
		{
			return TOptional<bool>();
		}

		return Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("1"));
	}

	static FString SerializeJson(TSharedPtr<FJsonObject> JsonObject, bool bPretty = false)
	{
		FString Result;
		if (bPretty)
		{
			const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Result);
			FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
		}
		else
		{
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Result);
			FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
		}
		return Result;
	}

	static void AddStringArrayField(TSharedPtr<FJsonObject> JsonObject, const FString& FieldName, const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		JsonObject->SetArrayField(FieldName, JsonValues);
	}

	static void AddJsonObjectArrayField(TSharedPtr<FJsonObject> JsonObject, const FString& FieldName, const TArray<TSharedPtr<FJsonObject>>& Objects)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(Objects.Num());
		for (const TSharedPtr<FJsonObject>& Object : Objects)
		{
			JsonValues.Add(MakeShared<FJsonValueObject>(Object));
		}
		JsonObject->SetArrayField(FieldName, JsonValues);
	}

	static TSharedPtr<FJsonObject> BuildSpecError(
		const FString& Action,
		const FString& ErrorCode,
		const FString& ErrorMessage,
		const TArray<FString>& VerificationHints = {},
		const TArray<FString>& Warnings = {})
	{
		const TSharedPtr<FJsonObject> ErrorObject = MakeShared<FJsonObject>();
		ErrorObject->SetBoolField(TEXT("success"), false);
		ErrorObject->SetStringField(TEXT("action"), Action);
		ErrorObject->SetStringField(TEXT("error_code"), ErrorCode);
		ErrorObject->SetStringField(TEXT("error"), ErrorMessage);
		ErrorObject->SetStringField(TEXT("error_message"), ErrorMessage);
		AddStringArrayField(ErrorObject, TEXT("verification_hints"), VerificationHints);
		AddStringArrayField(ErrorObject, TEXT("warnings"), Warnings);
		return ErrorObject;
	}

	static TSharedPtr<FJsonObject> BuildSpecSuccess(
		const FString& Action,
		const FString& Summary,
		TSharedPtr<FJsonObject> Data = nullptr,
		const TArray<FString>& ChangedItems = {},
		const TArray<FString>& VerificationHints = {},
		const TArray<FString>& Warnings = {},
		const TArray<FString>& AffectedAssets = {})
	{
		const TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
		ResultObject->SetBoolField(TEXT("success"), true);
		ResultObject->SetStringField(TEXT("action"), Action);
		ResultObject->SetStringField(TEXT("summary"), Summary);
		if (Data.IsValid())
		{
			ResultObject->SetObjectField(TEXT("data"), Data);
		}
		AddStringArrayField(ResultObject, TEXT("changed_items"), ChangedItems);
		AddStringArrayField(ResultObject, TEXT("verification_hints"), VerificationHints);
		AddStringArrayField(ResultObject, TEXT("warnings"), Warnings);
		AddStringArrayField(ResultObject, TEXT("affected_assets"), AffectedAssets);
		return ResultObject;
	}

	static FString NormalizeSpecAction(const FString& RawAction)
	{
		const FString Action = RawAction.ToLower();
		if (Action == TEXT("export_spec"))
		{
			return TEXT("export_widget_spec");
		}
		if (Action == TEXT("validate_spec"))
		{
			return TEXT("validate_widget_spec");
		}
		if (Action == TEXT("apply_spec"))
		{
			return TEXT("apply_widget_spec");
		}
		if (Action == TEXT("export_review"))
		{
			return TEXT("export_widget_review");
		}
		return Action;
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
			OutError = BuildSpecError(
				Action,
				TEXT("WIDGET_BLUEPRINT_NOT_FOUND"),
				FString::Printf(TEXT("Widget Blueprint '%s' was not found."), *WidgetPath),
				{ TEXT("Use widget_blueprint action=list_assets to inspect available Widget Blueprints.") }
			);
			return false;
		}

		OutCanonicalPath = AssetData.GetObjectPathString();
		OutWidgetBlueprint = Cast<UWidgetBlueprint>(UEditorAssetLibrary::LoadAsset(OutCanonicalPath));
		if (!OutWidgetBlueprint)
		{
			OutError = BuildSpecError(
				Action,
				TEXT("WIDGET_BLUEPRINT_LOAD_FAILED"),
				FString::Printf(TEXT("Failed to load Widget Blueprint '%s'."), *OutCanonicalPath),
				{ TEXT("Open the asset in the editor and confirm it is valid.") }
			);
			return false;
		}

		return true;
	}

	static UClass* ResolveWidgetParentClass(const FString& ParentClassInput)
	{
		if (ParentClassInput.IsEmpty())
		{
			return UUserWidget::StaticClass();
		}

		if (UClass* DirectClass = FindObject<UClass>(nullptr, *ParentClassInput))
		{
			if (DirectClass->IsChildOf(UUserWidget::StaticClass()))
			{
				return DirectClass;
			}
		}

		if (UClass* LoadedClass = LoadObject<UClass>(nullptr, *ParentClassInput))
		{
			if (LoadedClass->IsChildOf(UUserWidget::StaticClass()))
			{
				return LoadedClass;
			}
		}

		FAssetData AssetData;
		if (ResolveWidgetAssetData(ParentClassInput, AssetData))
		{
			if (UWidgetBlueprint* ParentWidgetBlueprint = Cast<UWidgetBlueprint>(AssetData.GetAsset()))
			{
				if (ParentWidgetBlueprint->GeneratedClass && ParentWidgetBlueprint->GeneratedClass->IsChildOf(UUserWidget::StaticClass()))
				{
					return ParentWidgetBlueprint->GeneratedClass;
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
			return Asset != nullptr && AssetEditorSubsystem->OpenEditorForAsset(Asset);
		}

		return false;
	}

	static FString GetPackagePathFromAnyWidgetPath(const FString& WidgetPath)
	{
		const FString PackagePath = FPackageName::ObjectPathToPackageName(WidgetPath);
		return PackagePath.IsEmpty() ? WidgetPath : PackagePath;
	}

	static FString GetWidgetSpecDirectory()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("VibeUE"), TEXT("WidgetSpecs"));
	}

	static FString MakeWidgetSpecBaseName(const FString& WidgetPath)
	{
		FString PackagePath = GetPackagePathFromAnyWidgetPath(WidgetPath);
		PackagePath.ReplaceInline(TEXT("/"), TEXT("__"));
		PackagePath.ReplaceInline(TEXT("."), TEXT("__"));
		return FPaths::MakeValidFileName(PackagePath);
	}

	static FString ResolveOutputPath(const FString& RequestedPath, const FString& DefaultPath)
	{
		if (RequestedPath.IsEmpty())
		{
			return DefaultPath;
		}

		if (FPaths::IsRelative(RequestedPath))
		{
			return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), RequestedPath);
		}

		return RequestedPath;
	}

	static FString BuildDefaultSpecPath(const FString& WidgetPath)
	{
		return FPaths::Combine(GetWidgetSpecDirectory(), MakeWidgetSpecBaseName(WidgetPath) + TEXT(".widget.json"));
	}

	static FString BuildDefaultReviewPathFromWidgetPath(const FString& WidgetPath)
	{
		return FPaths::Combine(GetWidgetSpecDirectory(), MakeWidgetSpecBaseName(WidgetPath) + TEXT(".widget.review.md"));
	}

	static FString BuildDefaultReviewPathFromSpecPath(const FString& SpecPath)
	{
		const FString FullSpecPath = FPaths::ConvertRelativePathToFull(SpecPath);
		return FPaths::ChangeExtension(FullSpecPath, TEXT(".review.md"));
	}

	static bool SaveTextFile(const FString& OutputPath, const FString& Content)
	{
		const FString Directory = FPaths::GetPath(OutputPath);
		IFileManager::Get().MakeDirectory(*Directory, true);
		return FFileHelper::SaveStringToFile(Content, *OutputPath);
	}

	static bool LoadTextFile(const FString& InputPath, FString& OutContent)
	{
		return FFileHelper::LoadFileToString(OutContent, *InputPath);
	}

	static UWidget* FindWidgetByName(UWidgetBlueprint* WidgetBlueprint, const FString& WidgetName)
	{
		if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree || WidgetName.IsEmpty())
		{
			return nullptr;
		}

		TArray<UWidget*> AllWidgets;
		WidgetBlueprint->WidgetTree->GetAllWidgets(AllWidgets);
		for (UWidget* Widget : AllWidgets)
		{
			if (Widget && Widget->GetName().Equals(WidgetName, ESearchCase::IgnoreCase))
			{
				return Widget;
			}
		}

		return nullptr;
	}

	static bool IsMeaningfulStringValue(const FString& Value)
	{
		return !Value.IsEmpty() && Value != TEXT("None") && Value != TEXT("()");
	}

	static bool ShouldIncludePropertyInSpec(UWidget* Widget, FProperty* Property, bool bIncludeAllProperties, FString& OutValue, FString& OutCategory)
	{
		if (!Widget || !Property)
		{
			return false;
		}

		if (!Property->HasAnyPropertyFlags(CPF_Edit))
		{
			return false;
		}

		if (Property->GetName().Equals(TEXT("Slot"), ESearchCase::IgnoreCase))
		{
			return false;
		}

		OutCategory = Property->GetMetaData(TEXT("Category"));
		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Widget);
		Property->ExportTextItem_Direct(OutValue, ValuePtr, nullptr, Widget, PPF_None);

		if (!bIncludeAllProperties)
		{
			UObject* DefaultObject = Widget->GetClass()->GetDefaultObject();
			FString DefaultValue;
			if (DefaultObject)
			{
				void* DefaultValuePtr = Property->ContainerPtrToValuePtr<void>(DefaultObject);
				Property->ExportTextItem_Direct(DefaultValue, DefaultValuePtr, nullptr, DefaultObject, PPF_None);
				if (OutValue == DefaultValue)
				{
					return false;
				}
			}

			if (!IsMeaningfulStringValue(OutValue))
			{
				return false;
			}
		}

		return true;
	}

	static FString GetPropertySectionName(const FString& PropertyName, const FString& Category)
	{
		if (Category.Contains(TEXT("Accessibility"), ESearchCase::IgnoreCase))
		{
			return TEXT("accessibility");
		}
		if (Category.Contains(TEXT("Behavior"), ESearchCase::IgnoreCase) || Category.Contains(TEXT("Navigation"), ESearchCase::IgnoreCase))
		{
			return TEXT("behavior");
		}
		if (Category.Contains(TEXT("Content"), ESearchCase::IgnoreCase) || Category.Contains(TEXT("Localization"), ESearchCase::IgnoreCase))
		{
			return TEXT("content");
		}
		if (Category.Contains(TEXT("Appearance"), ESearchCase::IgnoreCase) ||
			Category.Contains(TEXT("Render"), ESearchCase::IgnoreCase) ||
			Category.Contains(TEXT("Wrapping"), ESearchCase::IgnoreCase) ||
			Category.Contains(TEXT("Clipping"), ESearchCase::IgnoreCase))
		{
			return TEXT("appearance");
		}
		if (PropertyName.Equals(TEXT("Visibility"), ESearchCase::IgnoreCase) || PropertyName.Equals(TEXT("bIsEnabled"), ESearchCase::IgnoreCase))
		{
			return TEXT("behavior");
		}
		return TEXT("advanced");
	}

	static void SetStringFieldOnSection(TSharedPtr<FJsonObject> WidgetObject, const FString& SectionName, const FString& PropertyName, const FString& PropertyValue)
	{
		const TSharedPtr<FJsonObject>* ExistingSection = nullptr;
		if (!WidgetObject->TryGetObjectField(SectionName, ExistingSection))
		{
			WidgetObject->SetObjectField(SectionName, MakeShared<FJsonObject>());
			WidgetObject->TryGetObjectField(SectionName, ExistingSection);
		}

		(*ExistingSection)->SetStringField(PropertyName, PropertyValue);
	}

	static TArray<TSharedPtr<FJsonValue>> MakeVectorArray(double X, double Y)
	{
		return {
			MakeShared<FJsonValueNumber>(X),
			MakeShared<FJsonValueNumber>(Y)
		};
	}

	static TSharedPtr<FJsonObject> MakeOffsetsObject(const FMargin& Margin)
	{
		const TSharedPtr<FJsonObject> OffsetsObject = MakeShared<FJsonObject>();
		OffsetsObject->SetNumberField(TEXT("left"), Margin.Left);
		OffsetsObject->SetNumberField(TEXT("top"), Margin.Top);
		OffsetsObject->SetNumberField(TEXT("right"), Margin.Right);
		OffsetsObject->SetNumberField(TEXT("bottom"), Margin.Bottom);
		return OffsetsObject;
	}

	static TSharedPtr<FJsonObject> MakeMarginObject(const FMargin& Margin)
	{
		return MakeOffsetsObject(Margin);
	}

	static FString HorizontalAlignmentToString(EHorizontalAlignment Alignment)
	{
		switch (Alignment)
		{
		case HAlign_Left:
			return TEXT("Left");
		case HAlign_Center:
			return TEXT("Center");
		case HAlign_Right:
			return TEXT("Right");
		case HAlign_Fill:
			return TEXT("Fill");
		default:
			return TEXT("Fill");
		}
	}

	static FString VerticalAlignmentToString(EVerticalAlignment Alignment)
	{
		switch (Alignment)
		{
		case VAlign_Top:
			return TEXT("Top");
		case VAlign_Center:
			return TEXT("Center");
		case VAlign_Bottom:
			return TEXT("Bottom");
		case VAlign_Fill:
			return TEXT("Fill");
		default:
			return TEXT("Fill");
		}
	}

	static FString SlateSizeRuleToString(ESlateSizeRule::Type SizeRule)
	{
		switch (SizeRule)
		{
		case ESlateSizeRule::Automatic:
			return TEXT("Automatic");
		case ESlateSizeRule::Fill:
			return TEXT("Fill");
		default:
			return TEXT("Fill");
		}
	}

	static bool TryParseHorizontalAlignment(const FString& RawValue, EHorizontalAlignment& OutAlignment)
	{
		if (RawValue.Equals(TEXT("Left"), ESearchCase::IgnoreCase) || RawValue.Equals(TEXT("HAlign_Left"), ESearchCase::IgnoreCase))
		{
			OutAlignment = HAlign_Left;
			return true;
		}
		if (RawValue.Equals(TEXT("Center"), ESearchCase::IgnoreCase) || RawValue.Equals(TEXT("HAlign_Center"), ESearchCase::IgnoreCase))
		{
			OutAlignment = HAlign_Center;
			return true;
		}
		if (RawValue.Equals(TEXT("Right"), ESearchCase::IgnoreCase) || RawValue.Equals(TEXT("HAlign_Right"), ESearchCase::IgnoreCase))
		{
			OutAlignment = HAlign_Right;
			return true;
		}
		if (RawValue.Equals(TEXT("Fill"), ESearchCase::IgnoreCase) || RawValue.Equals(TEXT("HAlign_Fill"), ESearchCase::IgnoreCase))
		{
			OutAlignment = HAlign_Fill;
			return true;
		}
		return false;
	}

	static bool TryParseVerticalAlignment(const FString& RawValue, EVerticalAlignment& OutAlignment)
	{
		if (RawValue.Equals(TEXT("Top"), ESearchCase::IgnoreCase) || RawValue.Equals(TEXT("VAlign_Top"), ESearchCase::IgnoreCase))
		{
			OutAlignment = VAlign_Top;
			return true;
		}
		if (RawValue.Equals(TEXT("Center"), ESearchCase::IgnoreCase) || RawValue.Equals(TEXT("VAlign_Center"), ESearchCase::IgnoreCase))
		{
			OutAlignment = VAlign_Center;
			return true;
		}
		if (RawValue.Equals(TEXT("Bottom"), ESearchCase::IgnoreCase) || RawValue.Equals(TEXT("VAlign_Bottom"), ESearchCase::IgnoreCase))
		{
			OutAlignment = VAlign_Bottom;
			return true;
		}
		if (RawValue.Equals(TEXT("Fill"), ESearchCase::IgnoreCase) || RawValue.Equals(TEXT("VAlign_Fill"), ESearchCase::IgnoreCase))
		{
			OutAlignment = VAlign_Fill;
			return true;
		}
		return false;
	}

	static bool TryParseSlateSizeRule(const FString& RawValue, ESlateSizeRule::Type& OutSizeRule)
	{
		if (RawValue.Equals(TEXT("Automatic"), ESearchCase::IgnoreCase) || RawValue.Equals(TEXT("Auto"), ESearchCase::IgnoreCase))
		{
			OutSizeRule = ESlateSizeRule::Automatic;
			return true;
		}
		if (RawValue.Equals(TEXT("Fill"), ESearchCase::IgnoreCase))
		{
			OutSizeRule = ESlateSizeRule::Fill;
			return true;
		}
		return false;
	}

	static bool TryGetNumberField(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, double& OutNumber)
	{
		return JsonObject.IsValid() && JsonObject->TryGetNumberField(FieldName, OutNumber);
	}

	static bool TryGetIntField(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, int32& OutValue)
	{
		double Number = 0.0;
		if (!TryGetNumberField(JsonObject, FieldName, Number))
		{
			return false;
		}

		OutValue = static_cast<int32>(Number);
		return true;
	}

	static bool TryGetStringField(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, FString& OutValue)
	{
		return JsonObject.IsValid() && JsonObject->TryGetStringField(FieldName, OutValue);
	}

	static bool TryGetMarginObject(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, FMargin& OutMargin)
	{
		const TSharedPtr<FJsonObject>* MarginObject = nullptr;
		if (!JsonObject.IsValid() || !JsonObject->TryGetObjectField(FieldName, MarginObject) || !MarginObject || !(*MarginObject).IsValid())
		{
			return false;
		}

		double Left = 0.0;
		double Top = 0.0;
		double Right = 0.0;
		double Bottom = 0.0;
		if (!(*MarginObject)->TryGetNumberField(TEXT("left"), Left) ||
			!(*MarginObject)->TryGetNumberField(TEXT("top"), Top) ||
			!(*MarginObject)->TryGetNumberField(TEXT("right"), Right) ||
			!(*MarginObject)->TryGetNumberField(TEXT("bottom"), Bottom))
		{
			return false;
		}

		OutMargin = FMargin(Left, Top, Right, Bottom);
		return true;
	}

	static TSharedPtr<FJsonObject> MakeChildSizeObject(const FSlateChildSize& ChildSize)
	{
		const TSharedPtr<FJsonObject> SizeObject = MakeShared<FJsonObject>();
		SizeObject->SetStringField(TEXT("rule"), SlateSizeRuleToString(ChildSize.SizeRule));
		SizeObject->SetNumberField(TEXT("value"), ChildSize.Value);
		return SizeObject;
	}

	static bool TryGetChildSizeObject(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, FSlateChildSize& OutChildSize)
	{
		const TSharedPtr<FJsonObject>* SizeObject = nullptr;
		if (!JsonObject.IsValid() || !JsonObject->TryGetObjectField(FieldName, SizeObject) || !SizeObject || !(*SizeObject).IsValid())
		{
			return false;
		}

		FString Rule;
		ESlateSizeRule::Type ParsedSizeRule = ESlateSizeRule::Fill;
		if (!(*SizeObject)->TryGetStringField(TEXT("rule"), Rule) || !TryParseSlateSizeRule(Rule, ParsedSizeRule))
		{
			return false;
		}
		OutChildSize.SizeRule = ParsedSizeRule;

		double Value = 1.0;
		(*SizeObject)->TryGetNumberField(TEXT("value"), Value);
		OutChildSize.Value = static_cast<float>(Value);
		return true;
	}

	static TSharedPtr<FJsonObject> BuildSlotSpecObject(UWidget* Widget)
	{
		if (!Widget || !Widget->Slot)
		{
			return nullptr;
		}

		if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Widget->Slot))
		{
			const TSharedPtr<FJsonObject> SlotObject = MakeShared<FJsonObject>();
			SlotObject->SetStringField(TEXT("slot_type"), TEXT("CanvasPanelSlot"));

			const TSharedPtr<FJsonObject> RequiredObject = MakeShared<FJsonObject>();
			const FAnchors Anchors = CanvasSlot->GetAnchors();
			const TSharedPtr<FJsonObject> AnchorsObject = MakeShared<FJsonObject>();
			AnchorsObject->SetArrayField(TEXT("minimum"), MakeVectorArray(Anchors.Minimum.X, Anchors.Minimum.Y));
			AnchorsObject->SetArrayField(TEXT("maximum"), MakeVectorArray(Anchors.Maximum.X, Anchors.Maximum.Y));
			RequiredObject->SetObjectField(TEXT("anchors"), AnchorsObject);

			const FVector2D Alignment = CanvasSlot->GetAlignment();
			RequiredObject->SetArrayField(TEXT("alignment"), MakeVectorArray(Alignment.X, Alignment.Y));

			const FVector2D Position = CanvasSlot->GetPosition();
			RequiredObject->SetArrayField(TEXT("position"), MakeVectorArray(Position.X, Position.Y));
			RequiredObject->SetBoolField(TEXT("auto_size"), CanvasSlot->GetAutoSize());
			SlotObject->SetObjectField(TEXT("required"), RequiredObject);

			const TSharedPtr<FJsonObject> OptionalObject = MakeShared<FJsonObject>();
			if (!CanvasSlot->GetAutoSize())
			{
				const FVector2D Size = CanvasSlot->GetSize();
				OptionalObject->SetArrayField(TEXT("size"), MakeVectorArray(Size.X, Size.Y));
				OptionalObject->SetObjectField(TEXT("offsets"), MakeOffsetsObject(CanvasSlot->GetOffsets()));
			}
			if (CanvasSlot->GetZOrder() != 0)
			{
				OptionalObject->SetNumberField(TEXT("z_order"), CanvasSlot->GetZOrder());
			}
			SlotObject->SetObjectField(TEXT("optional"), OptionalObject);
			return SlotObject;
		}

		const TSharedPtr<FJsonObject> SlotObject = MakeShared<FJsonObject>();
		const TSharedPtr<FJsonObject> OptionalObject = MakeShared<FJsonObject>();

		if (UHorizontalBoxSlot* HorizontalSlot = Cast<UHorizontalBoxSlot>(Widget->Slot))
		{
			SlotObject->SetStringField(TEXT("slot_type"), TEXT("HorizontalBoxSlot"));
			OptionalObject->SetObjectField(TEXT("padding"), MakeMarginObject(HorizontalSlot->GetPadding()));
			OptionalObject->SetObjectField(TEXT("size"), MakeChildSizeObject(HorizontalSlot->GetSize()));
			OptionalObject->SetStringField(TEXT("horizontal_alignment"), HorizontalAlignmentToString(HorizontalSlot->GetHorizontalAlignment()));
			OptionalObject->SetStringField(TEXT("vertical_alignment"), VerticalAlignmentToString(HorizontalSlot->GetVerticalAlignment()));
		}
		else if (UVerticalBoxSlot* VerticalSlot = Cast<UVerticalBoxSlot>(Widget->Slot))
		{
			SlotObject->SetStringField(TEXT("slot_type"), TEXT("VerticalBoxSlot"));
			OptionalObject->SetObjectField(TEXT("padding"), MakeMarginObject(VerticalSlot->GetPadding()));
			OptionalObject->SetObjectField(TEXT("size"), MakeChildSizeObject(VerticalSlot->GetSize()));
			OptionalObject->SetStringField(TEXT("horizontal_alignment"), HorizontalAlignmentToString(VerticalSlot->GetHorizontalAlignment()));
			OptionalObject->SetStringField(TEXT("vertical_alignment"), VerticalAlignmentToString(VerticalSlot->GetVerticalAlignment()));
		}
		else if (UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(Widget->Slot))
		{
			SlotObject->SetStringField(TEXT("slot_type"), TEXT("OverlaySlot"));
			OptionalObject->SetObjectField(TEXT("padding"), MakeMarginObject(OverlaySlot->GetPadding()));
			OptionalObject->SetStringField(TEXT("horizontal_alignment"), HorizontalAlignmentToString(OverlaySlot->GetHorizontalAlignment()));
			OptionalObject->SetStringField(TEXT("vertical_alignment"), VerticalAlignmentToString(OverlaySlot->GetVerticalAlignment()));
		}
		else if (UScrollBoxSlot* ScrollBoxSlot = Cast<UScrollBoxSlot>(Widget->Slot))
		{
			SlotObject->SetStringField(TEXT("slot_type"), TEXT("ScrollBoxSlot"));
			OptionalObject->SetObjectField(TEXT("padding"), MakeMarginObject(ScrollBoxSlot->GetPadding()));
			OptionalObject->SetObjectField(TEXT("size"), MakeChildSizeObject(ScrollBoxSlot->GetSize()));
			OptionalObject->SetStringField(TEXT("horizontal_alignment"), HorizontalAlignmentToString(ScrollBoxSlot->GetHorizontalAlignment()));
			OptionalObject->SetStringField(TEXT("vertical_alignment"), VerticalAlignmentToString(ScrollBoxSlot->GetVerticalAlignment()));
		}
		else if (UWrapBoxSlot* WrapBoxSlot = Cast<UWrapBoxSlot>(Widget->Slot))
		{
			SlotObject->SetStringField(TEXT("slot_type"), TEXT("WrapBoxSlot"));
			OptionalObject->SetObjectField(TEXT("padding"), MakeMarginObject(WrapBoxSlot->GetPadding()));
			OptionalObject->SetBoolField(TEXT("fill_empty_space"), WrapBoxSlot->DoesFillEmptySpace());
			OptionalObject->SetNumberField(TEXT("fill_span_when_less_than"), WrapBoxSlot->GetFillSpanWhenLessThan());
			OptionalObject->SetStringField(TEXT("horizontal_alignment"), HorizontalAlignmentToString(WrapBoxSlot->GetHorizontalAlignment()));
			OptionalObject->SetStringField(TEXT("vertical_alignment"), VerticalAlignmentToString(WrapBoxSlot->GetVerticalAlignment()));
			OptionalObject->SetBoolField(TEXT("new_line"), WrapBoxSlot->DoesForceNewLine());
		}
		else if (UGridSlot* GridSlot = Cast<UGridSlot>(Widget->Slot))
		{
			SlotObject->SetStringField(TEXT("slot_type"), TEXT("GridSlot"));
			OptionalObject->SetObjectField(TEXT("padding"), MakeMarginObject(GridSlot->GetPadding()));
			OptionalObject->SetStringField(TEXT("horizontal_alignment"), HorizontalAlignmentToString(GridSlot->GetHorizontalAlignment()));
			OptionalObject->SetStringField(TEXT("vertical_alignment"), VerticalAlignmentToString(GridSlot->GetVerticalAlignment()));
			OptionalObject->SetNumberField(TEXT("row"), GridSlot->GetRow());
			OptionalObject->SetNumberField(TEXT("row_span"), GridSlot->GetRowSpan());
			OptionalObject->SetNumberField(TEXT("column"), GridSlot->GetColumn());
			OptionalObject->SetNumberField(TEXT("column_span"), GridSlot->GetColumnSpan());
			OptionalObject->SetNumberField(TEXT("layer"), GridSlot->GetLayer());
			const FVector2D Nudge = GridSlot->GetNudge();
			OptionalObject->SetArrayField(TEXT("nudge"), MakeVectorArray(Nudge.X, Nudge.Y));
		}
		else if (UUniformGridSlot* UniformGridSlot = Cast<UUniformGridSlot>(Widget->Slot))
		{
			SlotObject->SetStringField(TEXT("slot_type"), TEXT("UniformGridSlot"));
			OptionalObject->SetStringField(TEXT("horizontal_alignment"), HorizontalAlignmentToString(UniformGridSlot->GetHorizontalAlignment()));
			OptionalObject->SetStringField(TEXT("vertical_alignment"), VerticalAlignmentToString(UniformGridSlot->GetVerticalAlignment()));
			OptionalObject->SetNumberField(TEXT("row"), UniformGridSlot->GetRow());
			OptionalObject->SetNumberField(TEXT("column"), UniformGridSlot->GetColumn());
		}
		else if (UBorderSlot* BorderSlot = Cast<UBorderSlot>(Widget->Slot))
		{
			SlotObject->SetStringField(TEXT("slot_type"), TEXT("BorderSlot"));
			OptionalObject->SetObjectField(TEXT("padding"), MakeMarginObject(BorderSlot->GetPadding()));
			OptionalObject->SetStringField(TEXT("horizontal_alignment"), HorizontalAlignmentToString(BorderSlot->GetHorizontalAlignment()));
			OptionalObject->SetStringField(TEXT("vertical_alignment"), VerticalAlignmentToString(BorderSlot->GetVerticalAlignment()));
		}
		else if (USizeBoxSlot* SizeBoxSlot = Cast<USizeBoxSlot>(Widget->Slot))
		{
			SlotObject->SetStringField(TEXT("slot_type"), TEXT("SizeBoxSlot"));
			OptionalObject->SetObjectField(TEXT("padding"), MakeMarginObject(SizeBoxSlot->GetPadding()));
			OptionalObject->SetStringField(TEXT("horizontal_alignment"), HorizontalAlignmentToString(SizeBoxSlot->GetHorizontalAlignment()));
			OptionalObject->SetStringField(TEXT("vertical_alignment"), VerticalAlignmentToString(SizeBoxSlot->GetVerticalAlignment()));
		}
		else if (UScaleBoxSlot* ScaleBoxSlot = Cast<UScaleBoxSlot>(Widget->Slot))
		{
			SlotObject->SetStringField(TEXT("slot_type"), TEXT("ScaleBoxSlot"));
			OptionalObject->SetStringField(TEXT("horizontal_alignment"), HorizontalAlignmentToString(ScaleBoxSlot->GetHorizontalAlignment()));
			OptionalObject->SetStringField(TEXT("vertical_alignment"), VerticalAlignmentToString(ScaleBoxSlot->GetVerticalAlignment()));
		}
		else
		{
			SlotObject->SetStringField(TEXT("slot_type"), Widget->Slot->GetClass()->GetName());
			return SlotObject;
		}

		SlotObject->SetObjectField(TEXT("optional"), OptionalObject);
		return SlotObject;
	}

	static TSharedPtr<FJsonObject> BuildSlotSpec(UWidget* Widget)
	{
		return Widget ? BuildSlotSpecObject(Widget) : nullptr;
	}

	static void ParseBindingPath(const FString& Path, FString& OutOwner, FString& OutProperty)
	{
		const int32 DotIndex = Path.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (DotIndex == INDEX_NONE)
		{
			OutOwner = Path;
			OutProperty.Reset();
			return;
		}

		OutOwner = Path.Left(DotIndex);
		OutProperty = Path.Mid(DotIndex + 1);
	}

	static TSharedPtr<FJsonObject> BuildBindingsSpec(const FString& WidgetPath)
	{
		const TArray<FWidgetViewModelInfo> ViewModels = UWidgetService::ListViewModels(WidgetPath);
		const TArray<FWidgetViewModelBindingInfo> RawBindings = UWidgetService::ListViewModelBindings(WidgetPath);

		const TSharedPtr<FJsonObject> BindingsObject = MakeShared<FJsonObject>();
		const TSharedPtr<FJsonObject> MVVMObject = MakeShared<FJsonObject>();

		TArray<TSharedPtr<FJsonObject>> ViewModelObjects;
		for (const FWidgetViewModelInfo& ViewModelInfo : ViewModels)
		{
			const TSharedPtr<FJsonObject> ViewModelObject = MakeShared<FJsonObject>();
			ViewModelObject->SetStringField(TEXT("name"), ViewModelInfo.ViewModelName);
			ViewModelObject->SetStringField(TEXT("class"), ViewModelInfo.ViewModelClassName);
			ViewModelObject->SetStringField(TEXT("creation_type"), ViewModelInfo.CreationType);
			ViewModelObject->SetStringField(TEXT("view_model_id"), ViewModelInfo.ViewModelId);
			ViewModelObjects.Add(ViewModelObject);
		}
		AddJsonObjectArrayField(MVVMObject, TEXT("view_models"), ViewModelObjects);

		TArray<TSharedPtr<FJsonObject>> BindingObjects;
		for (const FWidgetViewModelBindingInfo& BindingInfo : RawBindings)
		{
			FString ViewModelName;
			FString ViewModelProperty;
			ParseBindingPath(BindingInfo.SourcePath, ViewModelName, ViewModelProperty);

			FString WidgetName;
			FString WidgetProperty;
			ParseBindingPath(BindingInfo.DestinationPath, WidgetName, WidgetProperty);

			const TSharedPtr<FJsonObject> BindingObject = MakeShared<FJsonObject>();
			BindingObject->SetStringField(TEXT("raw_source_path"), BindingInfo.SourcePath);
			BindingObject->SetStringField(TEXT("raw_destination_path"), BindingInfo.DestinationPath);
			BindingObject->SetStringField(TEXT("binding_mode"), BindingInfo.BindingMode);
			BindingObject->SetBoolField(TEXT("enabled"), BindingInfo.bEnabled);
			BindingObject->SetStringField(TEXT("binding_id"), BindingInfo.BindingId);
			BindingObject->SetStringField(TEXT("view_model_name"), ViewModelName);
			BindingObject->SetStringField(TEXT("view_model_property"), ViewModelProperty);
			BindingObject->SetStringField(TEXT("widget_name"), WidgetName);
			BindingObject->SetStringField(TEXT("widget_property"), WidgetProperty);
			BindingObjects.Add(BindingObject);
		}
		AddJsonObjectArrayField(MVVMObject, TEXT("property_bindings"), BindingObjects);

		BindingsObject->SetObjectField(TEXT("mvvm"), MVVMObject);
		BindingsObject->SetArrayField(TEXT("events"), TArray<TSharedPtr<FJsonValue>>());
		BindingsObject->SetArrayField(TEXT("delegates"), TArray<TSharedPtr<FJsonValue>>());
		return BindingsObject;
	}

	static TSharedPtr<FJsonObject> BuildWidgetSpecObject(UWidgetBlueprint* WidgetBlueprint, const FString& WidgetObjectPath, bool bIncludeAllProperties)
	{
		const FString WidgetPackagePath = GetPackagePathFromAnyWidgetPath(WidgetObjectPath);
		const TArray<FWidgetInfo> Hierarchy = UWidgetService::GetHierarchy(WidgetObjectPath);

		const TSharedPtr<FJsonObject> RootObject = MakeShared<FJsonObject>();
		RootObject->SetNumberField(TEXT("spec_version"), 1);
		RootObject->SetStringField(TEXT("kind"), TEXT("widget_blueprint_spec"));

		const TSharedPtr<FJsonObject> AssetObject = MakeShared<FJsonObject>();
		AssetObject->SetStringField(TEXT("widget_path"), WidgetPackagePath);
		AssetObject->SetStringField(TEXT("object_path"), WidgetObjectPath);
		AssetObject->SetStringField(TEXT("widget_name"), WidgetBlueprint ? WidgetBlueprint->GetName() : FPackageName::GetLongPackageAssetName(WidgetPackagePath));
		AssetObject->SetStringField(TEXT("parent_class"), WidgetBlueprint && WidgetBlueprint->ParentClass ? WidgetBlueprint->ParentClass->GetPathName() : TEXT("/Script/UMG.UserWidget"));
		RootObject->SetObjectField(TEXT("asset"), AssetObject);

		const TSharedPtr<FJsonObject> TreeObject = MakeShared<FJsonObject>();
		TreeObject->SetStringField(TEXT("root"), UWidgetService::GetRootWidget(WidgetObjectPath));

		TArray<TSharedPtr<FJsonObject>> NodeObjects;
		TSharedPtr<FJsonObject> WidgetsObject = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> SlotsObject = MakeShared<FJsonObject>();

		for (const FWidgetInfo& WidgetInfo : Hierarchy)
		{
			const TSharedPtr<FJsonObject> NodeObject = MakeShared<FJsonObject>();
			NodeObject->SetStringField(TEXT("name"), WidgetInfo.WidgetName);
			NodeObject->SetStringField(TEXT("type"), WidgetInfo.WidgetClass);
			if (!WidgetInfo.ParentWidget.IsEmpty())
			{
				NodeObject->SetStringField(TEXT("parent"), WidgetInfo.ParentWidget);
			}
			else
			{
				NodeObject->SetField(TEXT("parent"), MakeShared<FJsonValueNull>());
			}
			NodeObject->SetBoolField(TEXT("is_variable"), WidgetInfo.bIsVariable);
			NodeObjects.Add(NodeObject);

			const TSharedPtr<FJsonObject> WidgetObject = MakeShared<FJsonObject>();
			const TSharedPtr<FJsonObject> IdentityObject = MakeShared<FJsonObject>();
			IdentityObject->SetStringField(TEXT("type"), WidgetInfo.WidgetClass);
			IdentityObject->SetBoolField(TEXT("is_variable"), WidgetInfo.bIsVariable);
			WidgetObject->SetObjectField(TEXT("identity"), IdentityObject);
			WidgetObject->SetObjectField(TEXT("content"), MakeShared<FJsonObject>());
			WidgetObject->SetObjectField(TEXT("appearance"), MakeShared<FJsonObject>());
			WidgetObject->SetObjectField(TEXT("behavior"), MakeShared<FJsonObject>());
			WidgetObject->SetObjectField(TEXT("accessibility"), MakeShared<FJsonObject>());
			WidgetObject->SetObjectField(TEXT("advanced"), MakeShared<FJsonObject>());

			if (UWidget* Widget = FindWidgetByName(WidgetBlueprint, WidgetInfo.WidgetName))
			{
				for (TFieldIterator<FProperty> It(Widget->GetClass()); It; ++It)
				{
					FProperty* Property = *It;
					FString PropertyValue;
					FString PropertyCategory;
					if (!ShouldIncludePropertyInSpec(Widget, Property, bIncludeAllProperties, PropertyValue, PropertyCategory))
					{
						continue;
					}

					SetStringFieldOnSection(WidgetObject, GetPropertySectionName(Property->GetName(), PropertyCategory), Property->GetName(), PropertyValue);
				}

				if (!WidgetInfo.bIsRootWidget)
				{
					if (TSharedPtr<FJsonObject> SlotSpec = BuildSlotSpec(Widget))
					{
						SlotsObject->SetObjectField(WidgetInfo.WidgetName, SlotSpec);
					}
				}
			}

			WidgetsObject->SetObjectField(WidgetInfo.WidgetName, WidgetObject);
		}

		AddJsonObjectArrayField(TreeObject, TEXT("nodes"), NodeObjects);
		RootObject->SetObjectField(TEXT("tree"), TreeObject);
		RootObject->SetObjectField(TEXT("widgets"), WidgetsObject);
		RootObject->SetObjectField(TEXT("slots"), SlotsObject);
		RootObject->SetObjectField(TEXT("bindings"), BuildBindingsSpec(WidgetObjectPath));

		const TSharedPtr<FJsonObject> ApplyObject = MakeShared<FJsonObject>();
		ApplyObject->SetBoolField(TEXT("compile"), true);
		ApplyObject->SetBoolField(TEXT("save"), true);
		ApplyObject->SetBoolField(TEXT("open"), false);
		ApplyObject->SetBoolField(TEXT("remove_missing_widgets"), false);
		RootObject->SetObjectField(TEXT("apply"), ApplyObject);

		return RootObject;
	}

	static FString JsonValueToString(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid() || Value->IsNull())
		{
			return TEXT("null");
		}

		switch (Value->Type)
		{
		case EJson::String:
			return Value->AsString();
		case EJson::Number:
			return FString::SanitizeFloat(Value->AsNumber());
		case EJson::Boolean:
			return Value->AsBool() ? TEXT("true") : TEXT("false");
		default:
			return Value->AsString();
		}
	}

	static bool TryGetVector2FromJsonValue(const TSharedPtr<FJsonValue>& Value, FVector2D& OutVector)
	{
		if (!Value.IsValid() || Value->Type != EJson::Array)
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>& Values = Value->AsArray();
		if (Values.Num() != 2 || !Values[0].IsValid() || !Values[1].IsValid())
		{
			return false;
		}

		OutVector.X = Values[0]->AsNumber();
		OutVector.Y = Values[1]->AsNumber();
		return true;
	}

	static bool TryGetVector2Field(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, FVector2D& OutVector)
	{
		if (!JsonObject.IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonValue> Value = JsonObject->TryGetField(FieldName);
		return Value.IsValid() && TryGetVector2FromJsonValue(Value, OutVector);
	}

	static bool TryGetBoolField(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, bool& OutBool)
	{
		if (!JsonObject.IsValid())
		{
			return false;
		}

		return JsonObject->TryGetBoolField(FieldName, OutBool);
	}

	static FString GetWidgetSpecSourceDescription(const FString& SpecPath, bool bInline)
	{
		return bInline ? TEXT("inline spec_content") : SpecPath;
	}

	static bool LoadSpecObject(const TMap<FString, FString>& Params, const FString& Action, TSharedPtr<FJsonObject>& OutSpecObject, FString& OutResolvedSpecPath, TSharedPtr<FJsonObject>& OutError)
	{
		FString SpecContent = ExtractParam(Params, TEXT("spec_content"));
		const FString RequestedSpecPath = ExtractParam(Params, TEXT("spec_path"));
		const bool bInline = !SpecContent.IsEmpty();

		if (!bInline)
		{
			if (RequestedSpecPath.IsEmpty())
			{
				OutError = BuildSpecError(
					Action,
					TEXT("MISSING_SPEC_INPUT"),
					TEXT("Provide either 'spec_path' or 'spec_content'.")
				);
				return false;
			}

			OutResolvedSpecPath = ResolveOutputPath(RequestedSpecPath, RequestedSpecPath);
			if (!LoadTextFile(OutResolvedSpecPath, SpecContent))
			{
				OutError = BuildSpecError(
					Action,
					TEXT("SPEC_FILE_READ_FAILED"),
					FString::Printf(TEXT("Failed to read widget spec file '%s'."), *OutResolvedSpecPath)
				);
				return false;
			}
		}
		else
		{
			OutResolvedSpecPath.Reset();
		}

		const FString TrimmedContent = SpecContent.TrimStartAndEnd();
		if (!TrimmedContent.StartsWith(TEXT("{")))
		{
			OutError = BuildSpecError(
				Action,
				TEXT("UNSUPPORTED_SPEC_FORMAT"),
				FString::Printf(TEXT("Widget spec input from '%s' is not JSON. The initial implementation uses the exported .widget.json file as the canonical spec."), *GetWidgetSpecSourceDescription(OutResolvedSpecPath, bInline)),
				{ TEXT("Use export_widget_spec to generate a canonical .widget.json file, then review/edit that file before validate/apply.") }
			);
			return false;
		}

		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(TrimmedContent);
		if (!FJsonSerializer::Deserialize(Reader, OutSpecObject) || !OutSpecObject.IsValid())
		{
			OutError = BuildSpecError(
				Action,
				TEXT("SPEC_PARSE_FAILED"),
				FString::Printf(TEXT("Failed to parse widget spec JSON from '%s'."), *GetWidgetSpecSourceDescription(OutResolvedSpecPath, bInline))
			);
			return false;
		}

		return true;
	}

	static bool GetRequiredObjectField(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, const TSharedPtr<FJsonObject>*& OutObject)
	{
		return JsonObject.IsValid() && JsonObject->TryGetObjectField(FieldName, OutObject) && OutObject && (*OutObject).IsValid();
	}

	static bool GetRequiredArrayField(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, const TArray<TSharedPtr<FJsonValue>>*& OutArray)
	{
		return JsonObject.IsValid() && JsonObject->TryGetArrayField(FieldName, OutArray) && OutArray;
	}

	static TArray<FWidgetSpecNodeData> ParseSpecNodes(const TSharedPtr<FJsonObject>& TreeObject)
	{
		TArray<FWidgetSpecNodeData> Nodes;
		const TArray<TSharedPtr<FJsonValue>>* NodeValues = nullptr;
		if (!GetRequiredArrayField(TreeObject, TEXT("nodes"), NodeValues))
		{
			return Nodes;
		}

		for (const TSharedPtr<FJsonValue>& NodeValue : *NodeValues)
		{
			if (!NodeValue.IsValid() || NodeValue->Type != EJson::Object)
			{
				continue;
			}

			const TSharedPtr<FJsonObject> NodeObject = NodeValue->AsObject();
			FWidgetSpecNodeData Node;
			NodeObject->TryGetStringField(TEXT("name"), Node.Name);
			NodeObject->TryGetStringField(TEXT("type"), Node.Type);
			NodeObject->TryGetStringField(TEXT("parent"), Node.Parent);
			NodeObject->TryGetBoolField(TEXT("is_variable"), Node.bIsVariable);
			if (!Node.Name.IsEmpty() && !Node.Type.IsEmpty())
			{
				Nodes.Add(Node);
			}
		}

		return Nodes;
	}

	static bool SpecContainsNodeName(const TArray<FWidgetSpecNodeData>& Nodes, const FString& WidgetName)
	{
		for (const FWidgetSpecNodeData& Node : Nodes)
		{
			if (Node.Name.Equals(WidgetName, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}

		return false;
	}

	static void ValidateOptionalPaddingField(const TSharedPtr<FJsonObject>& OptionalObject, const FString& Prefix, TArray<FString>& OutErrors)
	{
		if (!OptionalObject.IsValid() || !OptionalObject->HasField(TEXT("padding")))
		{
			return;
		}

		FMargin Padding;
		if (!TryGetMarginObject(OptionalObject, TEXT("padding"), Padding))
		{
			OutErrors.Add(FString::Printf(TEXT("%s.padding must be an object with left/top/right/bottom numbers."), *Prefix));
		}
	}

	static void ValidateOptionalHorizontalAlignmentField(const TSharedPtr<FJsonObject>& OptionalObject, const FString& Prefix, TArray<FString>& OutErrors)
	{
		if (!OptionalObject.IsValid() || !OptionalObject->HasField(TEXT("horizontal_alignment")))
		{
			return;
		}

		FString HorizontalAlignment;
		EHorizontalAlignment ParsedAlignment = HAlign_Fill;
		if (!TryGetStringField(OptionalObject, TEXT("horizontal_alignment"), HorizontalAlignment) || !TryParseHorizontalAlignment(HorizontalAlignment, ParsedAlignment))
		{
			OutErrors.Add(FString::Printf(TEXT("%s.horizontal_alignment must be one of Left/Center/Right/Fill."), *Prefix));
		}
	}

	static void ValidateOptionalVerticalAlignmentField(const TSharedPtr<FJsonObject>& OptionalObject, const FString& Prefix, TArray<FString>& OutErrors)
	{
		if (!OptionalObject.IsValid() || !OptionalObject->HasField(TEXT("vertical_alignment")))
		{
			return;
		}

		FString VerticalAlignment;
		EVerticalAlignment ParsedAlignment = VAlign_Fill;
		if (!TryGetStringField(OptionalObject, TEXT("vertical_alignment"), VerticalAlignment) || !TryParseVerticalAlignment(VerticalAlignment, ParsedAlignment))
		{
			OutErrors.Add(FString::Printf(TEXT("%s.vertical_alignment must be one of Top/Center/Bottom/Fill."), *Prefix));
		}
	}

	static void ValidateOptionalChildSizeField(const TSharedPtr<FJsonObject>& OptionalObject, const FString& Prefix, TArray<FString>& OutErrors)
	{
		if (!OptionalObject.IsValid() || !OptionalObject->HasField(TEXT("size")))
		{
			return;
		}

		FSlateChildSize ChildSize;
		if (!TryGetChildSizeObject(OptionalObject, TEXT("size"), ChildSize))
		{
			OutErrors.Add(FString::Printf(TEXT("%s.size must be an object with rule=Automatic|Fill and numeric value."), *Prefix));
		}
	}

	static void ValidateSlotSpecObject(const FString& WidgetName, const TSharedPtr<FJsonObject>& SlotObject, TArray<FString>& OutErrors, TArray<FString>& OutWarnings)
	{
		FString SlotType;
		SlotObject->TryGetStringField(TEXT("slot_type"), SlotType);
		if (SlotType.IsEmpty())
		{
			OutErrors.Add(FString::Printf(TEXT("slots.%s.slot_type is required."), *WidgetName));
			return;
		}

		const FString SlotPrefix = FString::Printf(TEXT("slots.%s"), *WidgetName);

		if (SlotType.Equals(TEXT("CanvasPanelSlot"), ESearchCase::IgnoreCase))
		{
			const TSharedPtr<FJsonObject>* RequiredObject = nullptr;
			if (!SlotObject->TryGetObjectField(TEXT("required"), RequiredObject) || !RequiredObject || !(*RequiredObject).IsValid())
			{
				OutErrors.Add(FString::Printf(TEXT("%s.required is missing."), *SlotPrefix));
				return;
			}

			const TSharedPtr<FJsonObject>* AnchorsObject = nullptr;
			if (!(*RequiredObject)->TryGetObjectField(TEXT("anchors"), AnchorsObject) || !AnchorsObject || !(*AnchorsObject).IsValid())
			{
				OutErrors.Add(FString::Printf(TEXT("%s.required.anchors is missing."), *SlotPrefix));
			}
			else
			{
				FVector2D Minimum;
				FVector2D Maximum;
				if (!TryGetVector2Field(*AnchorsObject, TEXT("minimum"), Minimum) || !TryGetVector2Field(*AnchorsObject, TEXT("maximum"), Maximum))
				{
					OutErrors.Add(FString::Printf(TEXT("%s.required.anchors minimum/maximum must be [x, y] arrays."), *SlotPrefix));
				}
			}

			FVector2D Alignment;
			if (!TryGetVector2Field(*RequiredObject, TEXT("alignment"), Alignment))
			{
				OutErrors.Add(FString::Printf(TEXT("%s.required.alignment must be a [x, y] array."), *SlotPrefix));
			}

			FVector2D Position;
			if (!TryGetVector2Field(*RequiredObject, TEXT("position"), Position))
			{
				OutErrors.Add(FString::Printf(TEXT("%s.required.position must be a [x, y] array."), *SlotPrefix));
			}

			bool bAutoSize = false;
			if (!TryGetBoolField(*RequiredObject, TEXT("auto_size"), bAutoSize))
			{
				OutErrors.Add(FString::Printf(TEXT("%s.required.auto_size must be a boolean."), *SlotPrefix));
			}
			return;
		}

		const TSharedPtr<FJsonObject>* OptionalObject = nullptr;
		if (!SlotObject->TryGetObjectField(TEXT("optional"), OptionalObject) || !OptionalObject || !(*OptionalObject).IsValid())
		{
			return;
		}

		const TSharedPtr<FJsonObject> OptionalValue = *OptionalObject;
		if (SlotType.Equals(TEXT("HorizontalBoxSlot"), ESearchCase::IgnoreCase) ||
			SlotType.Equals(TEXT("VerticalBoxSlot"), ESearchCase::IgnoreCase) ||
			SlotType.Equals(TEXT("ScrollBoxSlot"), ESearchCase::IgnoreCase))
		{
			ValidateOptionalPaddingField(OptionalValue, SlotPrefix + TEXT(".optional"), OutErrors);
			ValidateOptionalChildSizeField(OptionalValue, SlotPrefix + TEXT(".optional"), OutErrors);
			ValidateOptionalHorizontalAlignmentField(OptionalValue, SlotPrefix + TEXT(".optional"), OutErrors);
			ValidateOptionalVerticalAlignmentField(OptionalValue, SlotPrefix + TEXT(".optional"), OutErrors);
			return;
		}

		if (SlotType.Equals(TEXT("OverlaySlot"), ESearchCase::IgnoreCase) ||
			SlotType.Equals(TEXT("BorderSlot"), ESearchCase::IgnoreCase) ||
			SlotType.Equals(TEXT("SizeBoxSlot"), ESearchCase::IgnoreCase) ||
			SlotType.Equals(TEXT("ScaleBoxSlot"), ESearchCase::IgnoreCase))
		{
			ValidateOptionalPaddingField(OptionalValue, SlotPrefix + TEXT(".optional"), OutErrors);
			ValidateOptionalHorizontalAlignmentField(OptionalValue, SlotPrefix + TEXT(".optional"), OutErrors);
			ValidateOptionalVerticalAlignmentField(OptionalValue, SlotPrefix + TEXT(".optional"), OutErrors);
			return;
		}

		if (SlotType.Equals(TEXT("WrapBoxSlot"), ESearchCase::IgnoreCase))
		{
			ValidateOptionalPaddingField(OptionalValue, SlotPrefix + TEXT(".optional"), OutErrors);
			ValidateOptionalHorizontalAlignmentField(OptionalValue, SlotPrefix + TEXT(".optional"), OutErrors);
			ValidateOptionalVerticalAlignmentField(OptionalValue, SlotPrefix + TEXT(".optional"), OutErrors);
			if (OptionalValue->HasField(TEXT("fill_span_when_less_than")))
			{
				double FillSpan = 0.0;
				if (!TryGetNumberField(OptionalValue, TEXT("fill_span_when_less_than"), FillSpan))
				{
					OutErrors.Add(FString::Printf(TEXT("%s.optional.fill_span_when_less_than must be a number."), *SlotPrefix));
				}
			}
			if (OptionalValue->HasField(TEXT("fill_empty_space")))
			{
				bool bFillEmptySpace = false;
				if (!TryGetBoolField(OptionalValue, TEXT("fill_empty_space"), bFillEmptySpace))
				{
					OutErrors.Add(FString::Printf(TEXT("%s.optional.fill_empty_space must be a boolean."), *SlotPrefix));
				}
			}
			if (OptionalValue->HasField(TEXT("new_line")))
			{
				bool bNewLine = false;
				if (!TryGetBoolField(OptionalValue, TEXT("new_line"), bNewLine))
				{
					OutErrors.Add(FString::Printf(TEXT("%s.optional.new_line must be a boolean."), *SlotPrefix));
				}
			}
			return;
		}

		if (SlotType.Equals(TEXT("GridSlot"), ESearchCase::IgnoreCase))
		{
			ValidateOptionalPaddingField(OptionalValue, SlotPrefix + TEXT(".optional"), OutErrors);
			ValidateOptionalHorizontalAlignmentField(OptionalValue, SlotPrefix + TEXT(".optional"), OutErrors);
			ValidateOptionalVerticalAlignmentField(OptionalValue, SlotPrefix + TEXT(".optional"), OutErrors);
			static const TArray<FString> GridIntegerFields = {
				TEXT("row"),
				TEXT("row_span"),
				TEXT("column"),
				TEXT("column_span"),
				TEXT("layer")
			};
			for (const FString& IntField : GridIntegerFields)
			{
				if (OptionalValue->HasField(IntField))
				{
					int32 Number = 0;
					if (!TryGetIntField(OptionalValue, IntField, Number))
					{
						OutErrors.Add(FString::Printf(TEXT("%s.optional.%s must be a number."), *SlotPrefix, *IntField));
					}
				}
			}
			if (OptionalValue->HasField(TEXT("nudge")))
			{
				FVector2D Nudge;
				if (!TryGetVector2Field(OptionalValue, TEXT("nudge"), Nudge))
				{
					OutErrors.Add(FString::Printf(TEXT("%s.optional.nudge must be a [x, y] array."), *SlotPrefix));
				}
			}
			return;
		}

		if (SlotType.Equals(TEXT("UniformGridSlot"), ESearchCase::IgnoreCase))
		{
			ValidateOptionalHorizontalAlignmentField(OptionalValue, SlotPrefix + TEXT(".optional"), OutErrors);
			ValidateOptionalVerticalAlignmentField(OptionalValue, SlotPrefix + TEXT(".optional"), OutErrors);
			static const TArray<FString> UniformGridIntegerFields = {
				TEXT("row"),
				TEXT("column")
			};
			for (const FString& IntField : UniformGridIntegerFields)
			{
				if (OptionalValue->HasField(IntField))
				{
					int32 Number = 0;
					if (!TryGetIntField(OptionalValue, IntField, Number))
					{
						OutErrors.Add(FString::Printf(TEXT("%s.optional.%s must be a number."), *SlotPrefix, *IntField));
					}
				}
			}
			return;
		}

		OutWarnings.Add(FString::Printf(TEXT("%s uses unsupported slot type '%s'; review/export will preserve it, but validate/apply only understand common panel slot types."), *SlotPrefix, *SlotType));
	}

	static bool ValidateWidgetSpecObject(
		const TSharedPtr<FJsonObject>& SpecObject,
		TArray<FString>& OutErrors,
		TArray<FString>& OutWarnings,
		TArray<FWidgetSpecNodeData>& OutNodes)
	{
		OutErrors.Reset();
		OutWarnings.Reset();
		OutNodes.Reset();

		double SpecVersion = 0.0;
		if (!SpecObject.IsValid() || !SpecObject->TryGetNumberField(TEXT("spec_version"), SpecVersion))
		{
			OutErrors.Add(TEXT("Missing top-level spec_version."));
		}

		FString Kind;
		SpecObject->TryGetStringField(TEXT("kind"), Kind);
		if (!Kind.Equals(TEXT("widget_blueprint_spec"), ESearchCase::IgnoreCase))
		{
			OutErrors.Add(TEXT("Top-level kind must be 'widget_blueprint_spec'."));
		}

		const TSharedPtr<FJsonObject>* AssetObject = nullptr;
		if (!GetRequiredObjectField(SpecObject, TEXT("asset"), AssetObject))
		{
			OutErrors.Add(TEXT("Missing top-level asset object."));
		}
		else
		{
			FString WidgetPath;
			(*AssetObject)->TryGetStringField(TEXT("widget_path"), WidgetPath);
			if (WidgetPath.IsEmpty())
			{
				OutErrors.Add(TEXT("asset.widget_path is required."));
			}

			FString ParentClass;
			(*AssetObject)->TryGetStringField(TEXT("parent_class"), ParentClass);
			if (ParentClass.IsEmpty())
			{
				OutWarnings.Add(TEXT("asset.parent_class is empty; apply_widget_spec will default to /Script/UMG.UserWidget."));
			}
		}

		const TSharedPtr<FJsonObject>* TreeObject = nullptr;
		if (!GetRequiredObjectField(SpecObject, TEXT("tree"), TreeObject))
		{
			OutErrors.Add(TEXT("Missing top-level tree object."));
		}
		else
		{
			FString RootName;
			(*TreeObject)->TryGetStringField(TEXT("root"), RootName);
			OutNodes = ParseSpecNodes(*TreeObject);
			if (OutNodes.Num() == 0)
			{
				OutErrors.Add(TEXT("tree.nodes must contain at least one widget node."));
			}

			TSet<FString> NodeNames;
			int32 RootNodeCount = 0;
			for (const FWidgetSpecNodeData& Node : OutNodes)
			{
				if (NodeNames.Contains(Node.Name))
				{
					OutErrors.Add(FString::Printf(TEXT("Duplicate widget node '%s'."), *Node.Name));
				}
				NodeNames.Add(Node.Name);

				if (Node.Parent.IsEmpty())
				{
					++RootNodeCount;
				}
				else if (!NodeNames.Contains(Node.Parent))
				{
					bool bParentFound = false;
					for (const FWidgetSpecNodeData& CandidateNode : OutNodes)
					{
						if (CandidateNode.Name.Equals(Node.Parent, ESearchCase::IgnoreCase))
						{
							bParentFound = true;
							break;
						}
					}
					if (!bParentFound)
					{
						OutErrors.Add(FString::Printf(TEXT("Widget '%s' references missing parent '%s'."), *Node.Name, *Node.Parent));
					}
				}
			}

			if (!RootName.IsEmpty() && !NodeNames.Contains(RootName))
			{
				OutErrors.Add(FString::Printf(TEXT("tree.root '%s' is not present in tree.nodes."), *RootName));
			}
			if (RootNodeCount != 1)
			{
				OutErrors.Add(FString::Printf(TEXT("Expected exactly one root node (parent=null); found %d."), RootNodeCount));
			}
			else if (!RootName.IsEmpty())
			{
				TSet<FString> ReachableNames;
				TFunction<void(const FString&)> VisitNode;
				VisitNode = [&](const FString& CurrentName)
				{
					if (ReachableNames.Contains(CurrentName))
					{
						return;
					}

					ReachableNames.Add(CurrentName);
					for (const FWidgetSpecNodeData& CandidateNode : OutNodes)
					{
						if (CandidateNode.Parent.Equals(CurrentName, ESearchCase::IgnoreCase))
						{
							VisitNode(CandidateNode.Name);
						}
					}
				};
				VisitNode(RootName);

				for (const FWidgetSpecNodeData& Node : OutNodes)
				{
					if (!ReachableNames.Contains(Node.Name))
					{
						OutErrors.Add(FString::Printf(TEXT("Widget '%s' is not reachable from tree.root '%s'. Check for cycles or disconnected subtrees."), *Node.Name, *RootName));
					}
				}
			}
		}

		const TSharedPtr<FJsonObject>* WidgetsObject = nullptr;
		if (!GetRequiredObjectField(SpecObject, TEXT("widgets"), WidgetsObject))
		{
			OutErrors.Add(TEXT("Missing top-level widgets object."));
		}
		else
		{
			for (const FWidgetSpecNodeData& Node : OutNodes)
			{
				const TSharedPtr<FJsonObject>* WidgetObject = nullptr;
				if (!(*WidgetsObject)->TryGetObjectField(Node.Name, WidgetObject) || !WidgetObject || !(*WidgetObject).IsValid())
				{
					OutErrors.Add(FString::Printf(TEXT("widgets.%s entry is missing."), *Node.Name));
					continue;
				}

				const TSharedPtr<FJsonObject>* IdentityObject = nullptr;
				if (!(*WidgetObject)->TryGetObjectField(TEXT("identity"), IdentityObject) || !IdentityObject || !(*IdentityObject).IsValid())
				{
					OutErrors.Add(FString::Printf(TEXT("widgets.%s.identity entry is missing."), *Node.Name));
					continue;
				}

				FString IdentityType;
				(*IdentityObject)->TryGetStringField(TEXT("type"), IdentityType);
				if (!IdentityType.IsEmpty() && !IdentityType.Equals(Node.Type, ESearchCase::IgnoreCase))
				{
					OutErrors.Add(FString::Printf(TEXT("widgets.%s.identity.type ('%s') does not match tree node type ('%s')."), *Node.Name, *IdentityType, *Node.Type));
				}
			}
		}

		const TSharedPtr<FJsonObject>* SlotsObject = nullptr;
		if (SpecObject->TryGetObjectField(TEXT("slots"), SlotsObject) && SlotsObject && (*SlotsObject).IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*SlotsObject)->Values)
			{
				const FString& WidgetName = Pair.Key;
				bool bWidgetExists = false;
				for (const FWidgetSpecNodeData& Node : OutNodes)
				{
					if (Node.Name.Equals(WidgetName, ESearchCase::IgnoreCase))
					{
						bWidgetExists = true;
						if (Node.Parent.IsEmpty())
						{
							OutWarnings.Add(FString::Printf(TEXT("slots.%s is defined for the root widget; root widgets do not use parent panel slots."), *WidgetName));
						}
						break;
					}
				}

				if (!bWidgetExists)
				{
					OutErrors.Add(FString::Printf(TEXT("slots.%s references a widget that is not present in tree.nodes."), *WidgetName));
					continue;
				}

				if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object)
				{
					OutErrors.Add(FString::Printf(TEXT("slots.%s must be an object."), *WidgetName));
					continue;
				}

				ValidateSlotSpecObject(WidgetName, Pair.Value->AsObject(), OutErrors, OutWarnings);
			}
		}

		const TSharedPtr<FJsonObject>* BindingsObject = nullptr;
		if (SpecObject->TryGetObjectField(TEXT("bindings"), BindingsObject) && BindingsObject && (*BindingsObject).IsValid())
		{
			const TSharedPtr<FJsonObject>* MVVMObject = nullptr;
			if ((*BindingsObject)->TryGetObjectField(TEXT("mvvm"), MVVMObject) && MVVMObject && (*MVVMObject).IsValid())
			{
				TSet<FString> ViewModelNames;
				const TArray<TSharedPtr<FJsonValue>>* ViewModelValues = nullptr;
				if ((*MVVMObject)->TryGetArrayField(TEXT("view_models"), ViewModelValues) && ViewModelValues)
				{
					for (const TSharedPtr<FJsonValue>& ViewModelValue : *ViewModelValues)
					{
						if (!ViewModelValue.IsValid() || ViewModelValue->Type != EJson::Object)
						{
							continue;
						}
						FString ViewModelName;
						ViewModelValue->AsObject()->TryGetStringField(TEXT("name"), ViewModelName);
						if (!ViewModelName.IsEmpty())
						{
							ViewModelNames.Add(ViewModelName);
						}
					}
				}

				const TArray<TSharedPtr<FJsonValue>>* BindingValues = nullptr;
				if ((*MVVMObject)->TryGetArrayField(TEXT("property_bindings"), BindingValues) && BindingValues)
				{
					for (const TSharedPtr<FJsonValue>& BindingValue : *BindingValues)
					{
						if (!BindingValue.IsValid() || BindingValue->Type != EJson::Object)
						{
							continue;
						}

						const TSharedPtr<FJsonObject> BindingObject = BindingValue->AsObject();
						FString ViewModelName;
						FString WidgetName;
						BindingObject->TryGetStringField(TEXT("view_model_name"), ViewModelName);
						BindingObject->TryGetStringField(TEXT("widget_name"), WidgetName);
						if (!ViewModelName.IsEmpty() && !ViewModelNames.Contains(ViewModelName))
						{
							OutErrors.Add(FString::Printf(TEXT("MVVM binding references missing view model '%s'."), *ViewModelName));
						}
						if (!WidgetName.IsEmpty())
						{
							if (!SpecContainsNodeName(OutNodes, WidgetName))
							{
								OutErrors.Add(FString::Printf(TEXT("MVVM binding references missing widget '%s'."), *WidgetName));
							}
						}
					}
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* EventsArray = nullptr;
			if ((*BindingsObject)->TryGetArrayField(TEXT("events"), EventsArray) && EventsArray)
			{
				for (int32 Index = 0; Index < EventsArray->Num(); ++Index)
				{
					const TSharedPtr<FJsonValue>& EventValue = (*EventsArray)[Index];
					if (!EventValue.IsValid() || EventValue->Type != EJson::Object)
					{
						OutErrors.Add(FString::Printf(TEXT("bindings.events[%d] must be an object."), Index));
						continue;
					}

					const TSharedPtr<FJsonObject> EventObject = EventValue->AsObject();
					FString WidgetName;
					FString EventName;
					FString FunctionName;
					EventObject->TryGetStringField(TEXT("widget_name"), WidgetName);
					EventObject->TryGetStringField(TEXT("event_name"), EventName);
					EventObject->TryGetStringField(TEXT("function_name"), FunctionName);
					if (WidgetName.IsEmpty() || EventName.IsEmpty() || FunctionName.IsEmpty())
					{
						OutErrors.Add(FString::Printf(TEXT("bindings.events[%d] requires widget_name, event_name, and function_name."), Index));
						continue;
					}
					if (!SpecContainsNodeName(OutNodes, WidgetName))
					{
						OutErrors.Add(FString::Printf(TEXT("bindings.events[%d] references missing widget '%s'."), Index, *WidgetName));
					}
				}
				if (EventsArray->Num() > 0)
				{
					OutWarnings.Add(TEXT("bindings.events entries will be recorded component-by-component, but full Blueprint graph wiring still requires manual verification."));
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* DelegatesArray = nullptr;
			if ((*BindingsObject)->TryGetArrayField(TEXT("delegates"), DelegatesArray) && DelegatesArray)
			{
				for (int32 Index = 0; Index < DelegatesArray->Num(); ++Index)
				{
					const TSharedPtr<FJsonValue>& DelegateValue = (*DelegatesArray)[Index];
					if (!DelegateValue.IsValid() || DelegateValue->Type != EJson::Object)
					{
						OutErrors.Add(FString::Printf(TEXT("bindings.delegates[%d] must be an object."), Index));
						continue;
					}

					const TSharedPtr<FJsonObject> DelegateObject = DelegateValue->AsObject();
					FString WidgetName;
					FString DelegateName;
					FString FunctionName;
					DelegateObject->TryGetStringField(TEXT("widget_name"), WidgetName);
					DelegateObject->TryGetStringField(TEXT("delegate_name"), DelegateName);
					DelegateObject->TryGetStringField(TEXT("function_name"), FunctionName);
					if (WidgetName.IsEmpty() || DelegateName.IsEmpty() || FunctionName.IsEmpty())
					{
						OutErrors.Add(FString::Printf(TEXT("bindings.delegates[%d] requires widget_name, delegate_name, and function_name."), Index));
						continue;
					}
					if (!SpecContainsNodeName(OutNodes, WidgetName))
					{
						OutErrors.Add(FString::Printf(TEXT("bindings.delegates[%d] references missing widget '%s'."), Index, *WidgetName));
					}
				}
				if (DelegatesArray->Num() > 0)
				{
					OutWarnings.Add(TEXT("bindings.delegates entries will be recorded component-by-component, but arbitrary delegate graph wiring still requires manual verification."));
				}
			}
		}

		return OutErrors.Num() == 0;
	}

	static bool ApplyCanvasSlotLayout(
		UWidgetBlueprint* WidgetBlueprint,
		UWidget* Widget,
		const TSharedPtr<FJsonObject>& SlotObject,
		const FString& WidgetName,
		TArray<FString>& InOutChangedItems,
		TArray<FString>& InOutWarnings)
	{
		if (!WidgetBlueprint || !Widget || !SlotObject.IsValid())
		{
			return false;
		}

		FString SlotType;
		SlotObject->TryGetStringField(TEXT("slot_type"), SlotType);
		if (!SlotType.Equals(TEXT("CanvasPanelSlot"), ESearchCase::IgnoreCase))
		{
			InOutWarnings.Add(FString::Printf(TEXT("Skipped slot layout for %s because slot type '%s' is not currently supported."), *WidgetName, *SlotType));
			return false;
		}

		UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Widget->Slot);
		if (!CanvasSlot)
		{
			InOutWarnings.Add(FString::Printf(TEXT("Skipped slot layout for %s because the live slot is not a CanvasPanelSlot."), *WidgetName));
			return false;
		}

		const TSharedPtr<FJsonObject>* RequiredObject = nullptr;
		if (!SlotObject->TryGetObjectField(TEXT("required"), RequiredObject) || !RequiredObject || !(*RequiredObject).IsValid())
		{
			InOutWarnings.Add(FString::Printf(TEXT("Skipped slot layout for %s because slots.%s.required is missing."), *WidgetName, *WidgetName));
			return false;
		}

		const TSharedPtr<FJsonObject>* AnchorsObject = nullptr;
		if (!(*RequiredObject)->TryGetObjectField(TEXT("anchors"), AnchorsObject) || !AnchorsObject || !(*AnchorsObject).IsValid())
		{
			InOutWarnings.Add(FString::Printf(TEXT("Skipped slot layout for %s because anchors are missing."), *WidgetName));
			return false;
		}

		FVector2D Minimum;
		FVector2D Maximum;
		FVector2D Alignment;
		FVector2D Position;
		bool bAutoSize = false;
		if (!TryGetVector2Field(*AnchorsObject, TEXT("minimum"), Minimum) ||
			!TryGetVector2Field(*AnchorsObject, TEXT("maximum"), Maximum) ||
			!TryGetVector2Field(*RequiredObject, TEXT("alignment"), Alignment) ||
			!TryGetVector2Field(*RequiredObject, TEXT("position"), Position) ||
			!TryGetBoolField(*RequiredObject, TEXT("auto_size"), bAutoSize))
		{
			InOutWarnings.Add(FString::Printf(TEXT("Skipped slot layout for %s because one or more required CanvasPanelSlot fields are invalid."), *WidgetName));
			return false;
		}

		CanvasSlot->Modify();
		Widget->Modify();
		WidgetBlueprint->Modify();

		CanvasSlot->SetAnchors(FAnchors(Minimum.X, Minimum.Y, Maximum.X, Maximum.Y));
		CanvasSlot->SetAlignment(Alignment);
		CanvasSlot->SetPosition(Position);
		CanvasSlot->SetAutoSize(bAutoSize);

		const TSharedPtr<FJsonObject>* OptionalObject = nullptr;
		if (SlotObject->TryGetObjectField(TEXT("optional"), OptionalObject) && OptionalObject && (*OptionalObject).IsValid())
		{
			if (!bAutoSize)
			{
				FVector2D Size;
				if (TryGetVector2Field(*OptionalObject, TEXT("size"), Size))
				{
					CanvasSlot->SetSize(Size);
				}

				const TSharedPtr<FJsonObject>* OffsetsObject = nullptr;
				if ((*OptionalObject)->TryGetObjectField(TEXT("offsets"), OffsetsObject) && OffsetsObject && (*OffsetsObject).IsValid())
				{
					double Left = 0.0;
					double Top = 0.0;
					double Right = 0.0;
					double Bottom = 0.0;
					if ((*OffsetsObject)->TryGetNumberField(TEXT("left"), Left) &&
						(*OffsetsObject)->TryGetNumberField(TEXT("top"), Top) &&
						(*OffsetsObject)->TryGetNumberField(TEXT("right"), Right) &&
						(*OffsetsObject)->TryGetNumberField(TEXT("bottom"), Bottom))
					{
						CanvasSlot->SetOffsets(FMargin(Left, Top, Right, Bottom));
					}
				}
			}

			double ZOrder = 0.0;
			if ((*OptionalObject)->TryGetNumberField(TEXT("z_order"), ZOrder))
			{
				CanvasSlot->SetZOrder(static_cast<int32>(ZOrder));
			}
		}

		InOutChangedItems.Add(FString::Printf(TEXT("updated %s CanvasPanelSlot layout"), *WidgetName));
		return true;
	}

	static bool ApplySupportedPanelSlotLayout(
		UWidgetBlueprint* WidgetBlueprint,
		UWidget* Widget,
		const TSharedPtr<FJsonObject>& SlotObject,
		const FString& WidgetName,
		TArray<FString>& InOutChangedItems,
		TArray<FString>& InOutWarnings)
	{
		if (!WidgetBlueprint || !Widget || !SlotObject.IsValid())
		{
			return false;
		}

		FString SlotType;
		SlotObject->TryGetStringField(TEXT("slot_type"), SlotType);
		if (SlotType.Equals(TEXT("CanvasPanelSlot"), ESearchCase::IgnoreCase))
		{
			return ApplyCanvasSlotLayout(WidgetBlueprint, Widget, SlotObject, WidgetName, InOutChangedItems, InOutWarnings);
		}

		const TSharedPtr<FJsonObject>* OptionalObject = nullptr;
		if (!SlotObject->TryGetObjectField(TEXT("optional"), OptionalObject) || !OptionalObject || !(*OptionalObject).IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonObject> OptionalValue = *OptionalObject;
		bool bApplied = false;

		auto ApplyHorizontalAlignment = [&](auto* PanelSlot)
		{
			FString HorizontalAlignment;
			EHorizontalAlignment ParsedAlignment = HAlign_Fill;
			if (TryGetStringField(OptionalValue, TEXT("horizontal_alignment"), HorizontalAlignment) && TryParseHorizontalAlignment(HorizontalAlignment, ParsedAlignment))
			{
				PanelSlot->SetHorizontalAlignment(ParsedAlignment);
				bApplied = true;
			}
		};

		auto ApplyVerticalAlignment = [&](auto* PanelSlot)
		{
			FString VerticalAlignment;
			EVerticalAlignment ParsedAlignment = VAlign_Fill;
			if (TryGetStringField(OptionalValue, TEXT("vertical_alignment"), VerticalAlignment) && TryParseVerticalAlignment(VerticalAlignment, ParsedAlignment))
			{
				PanelSlot->SetVerticalAlignment(ParsedAlignment);
				bApplied = true;
			}
		};

		if (UHorizontalBoxSlot* HorizontalSlot = Cast<UHorizontalBoxSlot>(Widget->Slot))
		{
			if (SlotType.Equals(TEXT("HorizontalBoxSlot"), ESearchCase::IgnoreCase))
			{
				FMargin Padding;
				if (TryGetMarginObject(OptionalValue, TEXT("padding"), Padding))
				{
					HorizontalSlot->SetPadding(Padding);
					bApplied = true;
				}
				FSlateChildSize ChildSize;
				if (TryGetChildSizeObject(OptionalValue, TEXT("size"), ChildSize))
				{
					HorizontalSlot->SetSize(ChildSize);
					bApplied = true;
				}
				ApplyHorizontalAlignment(HorizontalSlot);
				ApplyVerticalAlignment(HorizontalSlot);
			}
		}
		else if (UVerticalBoxSlot* VerticalSlot = Cast<UVerticalBoxSlot>(Widget->Slot))
		{
			if (SlotType.Equals(TEXT("VerticalBoxSlot"), ESearchCase::IgnoreCase))
			{
				FMargin Padding;
				if (TryGetMarginObject(OptionalValue, TEXT("padding"), Padding))
				{
					VerticalSlot->SetPadding(Padding);
					bApplied = true;
				}
				FSlateChildSize ChildSize;
				if (TryGetChildSizeObject(OptionalValue, TEXT("size"), ChildSize))
				{
					VerticalSlot->SetSize(ChildSize);
					bApplied = true;
				}
				ApplyHorizontalAlignment(VerticalSlot);
				ApplyVerticalAlignment(VerticalSlot);
			}
		}
		else if (UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(Widget->Slot))
		{
			if (SlotType.Equals(TEXT("OverlaySlot"), ESearchCase::IgnoreCase))
			{
				FMargin Padding;
				if (TryGetMarginObject(OptionalValue, TEXT("padding"), Padding))
				{
					OverlaySlot->SetPadding(Padding);
					bApplied = true;
				}
				ApplyHorizontalAlignment(OverlaySlot);
				ApplyVerticalAlignment(OverlaySlot);
			}
		}
		else if (UScrollBoxSlot* ScrollBoxSlot = Cast<UScrollBoxSlot>(Widget->Slot))
		{
			if (SlotType.Equals(TEXT("ScrollBoxSlot"), ESearchCase::IgnoreCase))
			{
				FMargin Padding;
				if (TryGetMarginObject(OptionalValue, TEXT("padding"), Padding))
				{
					ScrollBoxSlot->SetPadding(Padding);
					bApplied = true;
				}
				FSlateChildSize ChildSize;
				if (TryGetChildSizeObject(OptionalValue, TEXT("size"), ChildSize))
				{
					ScrollBoxSlot->SetSize(ChildSize);
					bApplied = true;
				}
				ApplyHorizontalAlignment(ScrollBoxSlot);
				ApplyVerticalAlignment(ScrollBoxSlot);
			}
		}
		else if (UWrapBoxSlot* WrapBoxSlot = Cast<UWrapBoxSlot>(Widget->Slot))
		{
			if (SlotType.Equals(TEXT("WrapBoxSlot"), ESearchCase::IgnoreCase))
			{
				FMargin Padding;
				if (TryGetMarginObject(OptionalValue, TEXT("padding"), Padding))
				{
					WrapBoxSlot->SetPadding(Padding);
					bApplied = true;
				}
				ApplyHorizontalAlignment(WrapBoxSlot);
				ApplyVerticalAlignment(WrapBoxSlot);

				bool bFillEmptySpace = false;
				if (TryGetBoolField(OptionalValue, TEXT("fill_empty_space"), bFillEmptySpace))
				{
					WrapBoxSlot->SetFillEmptySpace(bFillEmptySpace);
					bApplied = true;
				}

				double FillSpanWhenLessThan = 0.0;
				if (TryGetNumberField(OptionalValue, TEXT("fill_span_when_less_than"), FillSpanWhenLessThan))
				{
					WrapBoxSlot->SetFillSpanWhenLessThan(static_cast<float>(FillSpanWhenLessThan));
					bApplied = true;
				}

				bool bNewLine = false;
				if (TryGetBoolField(OptionalValue, TEXT("new_line"), bNewLine))
				{
					WrapBoxSlot->SetNewLine(bNewLine);
					bApplied = true;
				}
			}
		}
		else if (UGridSlot* GridSlot = Cast<UGridSlot>(Widget->Slot))
		{
			if (SlotType.Equals(TEXT("GridSlot"), ESearchCase::IgnoreCase))
			{
				FMargin Padding;
				if (TryGetMarginObject(OptionalValue, TEXT("padding"), Padding))
				{
					GridSlot->SetPadding(Padding);
					bApplied = true;
				}
				ApplyHorizontalAlignment(GridSlot);
				ApplyVerticalAlignment(GridSlot);

				int32 ParsedValue = 0;
				if (TryGetIntField(OptionalValue, TEXT("row"), ParsedValue))
				{
					GridSlot->SetRow(ParsedValue);
					bApplied = true;
				}
				if (TryGetIntField(OptionalValue, TEXT("row_span"), ParsedValue))
				{
					GridSlot->SetRowSpan(ParsedValue);
					bApplied = true;
				}
				if (TryGetIntField(OptionalValue, TEXT("column"), ParsedValue))
				{
					GridSlot->SetColumn(ParsedValue);
					bApplied = true;
				}
				if (TryGetIntField(OptionalValue, TEXT("column_span"), ParsedValue))
				{
					GridSlot->SetColumnSpan(ParsedValue);
					bApplied = true;
				}
				if (TryGetIntField(OptionalValue, TEXT("layer"), ParsedValue))
				{
					GridSlot->SetLayer(ParsedValue);
					bApplied = true;
				}

				FVector2D Nudge;
				if (TryGetVector2Field(OptionalValue, TEXT("nudge"), Nudge))
				{
					GridSlot->SetNudge(Nudge);
					bApplied = true;
				}
			}
		}
		else if (UUniformGridSlot* UniformGridSlot = Cast<UUniformGridSlot>(Widget->Slot))
		{
			if (SlotType.Equals(TEXT("UniformGridSlot"), ESearchCase::IgnoreCase))
			{
				ApplyHorizontalAlignment(UniformGridSlot);
				ApplyVerticalAlignment(UniformGridSlot);

				int32 Row = 0;
				if (TryGetIntField(OptionalValue, TEXT("row"), Row))
				{
					UniformGridSlot->SetRow(Row);
					bApplied = true;
				}

				int32 Column = 0;
				if (TryGetIntField(OptionalValue, TEXT("column"), Column))
				{
					UniformGridSlot->SetColumn(Column);
					bApplied = true;
				}
			}
		}
		else if (UBorderSlot* BorderSlot = Cast<UBorderSlot>(Widget->Slot))
		{
			if (SlotType.Equals(TEXT("BorderSlot"), ESearchCase::IgnoreCase))
			{
				FMargin Padding;
				if (TryGetMarginObject(OptionalValue, TEXT("padding"), Padding))
				{
					BorderSlot->SetPadding(Padding);
					bApplied = true;
				}
				ApplyHorizontalAlignment(BorderSlot);
				ApplyVerticalAlignment(BorderSlot);
			}
		}
		else if (USizeBoxSlot* SizeBoxSlot = Cast<USizeBoxSlot>(Widget->Slot))
		{
			if (SlotType.Equals(TEXT("SizeBoxSlot"), ESearchCase::IgnoreCase))
			{
				FMargin Padding;
				if (TryGetMarginObject(OptionalValue, TEXT("padding"), Padding))
				{
					SizeBoxSlot->SetPadding(Padding);
					bApplied = true;
				}
				ApplyHorizontalAlignment(SizeBoxSlot);
				ApplyVerticalAlignment(SizeBoxSlot);
			}
		}
		else if (UScaleBoxSlot* ScaleBoxSlot = Cast<UScaleBoxSlot>(Widget->Slot))
		{
			if (SlotType.Equals(TEXT("ScaleBoxSlot"), ESearchCase::IgnoreCase))
			{
				ApplyHorizontalAlignment(ScaleBoxSlot);
				ApplyVerticalAlignment(ScaleBoxSlot);
			}
		}

		if (!bApplied)
		{
			if (!SlotType.IsEmpty())
			{
				InOutWarnings.Add(FString::Printf(TEXT("Skipped slot layout for %s because slot type '%s' is unsupported or its layout fields were empty."), *WidgetName, *SlotType));
			}
			return false;
		}

		WidgetBlueprint->Modify();
		Widget->Modify();
		InOutChangedItems.Add(FString::Printf(TEXT("updated %s %s layout"), *WidgetName, *SlotType));
		return true;
	}

	static FString GetJsonStringField(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName)
	{
		FString Value;
		if (JsonObject.IsValid())
		{
			JsonObject->TryGetStringField(FieldName, Value);
		}
		return Value;
	}

	static bool EnsureWidgetBlueprintForSpec(
		const TSharedPtr<FJsonObject>& SpecObject,
		UWidgetBlueprint*& OutWidgetBlueprint,
		FString& OutCanonicalWidgetPath,
		TArray<FString>& InOutChangedItems,
		TArray<FString>& InOutWarnings,
		TSharedPtr<FJsonObject>& OutError)
	{
		const TSharedPtr<FJsonObject>* AssetObject = nullptr;
		SpecObject->TryGetObjectField(TEXT("asset"), AssetObject);
		if (!AssetObject || !(*AssetObject).IsValid())
		{
			OutError = BuildSpecError(TEXT("apply_widget_spec"), TEXT("INVALID_SPEC"), TEXT("Missing asset section in spec."));
			return false;
		}

		const FString RequestedWidgetPath = GetJsonStringField(*AssetObject, TEXT("widget_path"));
		if (RequestedWidgetPath.IsEmpty())
		{
			OutError = BuildSpecError(TEXT("apply_widget_spec"), TEXT("INVALID_SPEC"), TEXT("asset.widget_path is required."));
			return false;
		}

		TSharedPtr<FJsonObject> LoadError;
		if (LoadWidgetBlueprintAsset(RequestedWidgetPath, OutWidgetBlueprint, OutCanonicalWidgetPath, LoadError, TEXT("apply_widget_spec")))
		{
			return true;
		}

		UClass* ParentClass = ResolveWidgetParentClass(GetJsonStringField(*AssetObject, TEXT("parent_class")));
		if (!ParentClass)
		{
			OutError = BuildSpecError(
				TEXT("apply_widget_spec"),
				TEXT("PARENT_CLASS_NOT_FOUND"),
				FString::Printf(TEXT("Could not resolve parent class '%s' for asset '%s'."), *GetJsonStringField(*AssetObject, TEXT("parent_class")), *RequestedWidgetPath)
			);
			return false;
		}

		const FString PackagePath = GetPackagePathFromAnyWidgetPath(RequestedWidgetPath);
		const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
		const FString DestinationPath = FPackageName::GetLongPackagePath(PackagePath);
		if (AssetName.IsEmpty() || DestinationPath.IsEmpty())
		{
			OutError = BuildSpecError(
				TEXT("apply_widget_spec"),
				TEXT("INVALID_WIDGET_PATH"),
				FString::Printf(TEXT("Spec asset.widget_path '%s' is not a valid package path."), *RequestedWidgetPath)
			);
			return false;
		}

		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
		Factory->BlueprintType = BPTYPE_Normal;
		Factory->ParentClass = ParentClass;

		UObject* NewAsset = AssetTools.CreateAsset(AssetName, DestinationPath, UWidgetBlueprint::StaticClass(), Factory);
		OutWidgetBlueprint = Cast<UWidgetBlueprint>(NewAsset);
		if (!OutWidgetBlueprint)
		{
			OutError = BuildSpecError(
				TEXT("apply_widget_spec"),
				TEXT("CREATE_WIDGET_FAILED"),
				FString::Printf(TEXT("Failed to create Widget Blueprint '%s'."), *RequestedWidgetPath)
			);
			return false;
		}

		OutCanonicalWidgetPath = OutWidgetBlueprint->GetPathName();
		InOutChangedItems.Add(FString::Printf(TEXT("created widget blueprint %s"), *OutCanonicalWidgetPath));
		if (!UEditorAssetLibrary::SaveAsset(OutCanonicalWidgetPath, false))
		{
			InOutWarnings.Add(TEXT("The new Widget Blueprint was created, but the initial save failed."));
		}

		return true;
	}

	static void FinalizeWidgetApply(
		UWidgetBlueprint* WidgetBlueprint,
		const FString& WidgetPath,
		bool bCompileAfter,
		bool bSaveAfter,
		bool bOpenAfter,
		TArray<FString>& InOutChangedItems,
		TArray<FString>& InOutWarnings)
	{
		if (!WidgetBlueprint)
		{
			return;
		}

		if (bCompileAfter)
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

		if (bSaveAfter)
		{
			if (UEditorAssetLibrary::SaveAsset(WidgetPath, false))
			{
				InOutChangedItems.Add(TEXT("saved widget blueprint"));
			}
			else
			{
				InOutWarnings.Add(TEXT("Failed to save the Widget Blueprint after applying the spec."));
			}
		}

		if (bOpenAfter)
		{
			if (OpenWidgetAsset(WidgetPath))
			{
				InOutChangedItems.Add(TEXT("opened widget blueprint editor"));
			}
			else
			{
				InOutWarnings.Add(TEXT("Failed to open the Widget Blueprint editor tab after applying the spec."));
			}
		}
	}

	static void RefreshExistingWidgetsByName(const FString& CanonicalWidgetPath, TMap<FString, FWidgetInfo>& OutWidgetsByName)
	{
		OutWidgetsByName.Reset();
		for (const FWidgetInfo& WidgetInfo : UWidgetService::ListComponents(CanonicalWidgetPath))
		{
			OutWidgetsByName.Add(WidgetInfo.WidgetName, WidgetInfo);
		}
	}

	static bool RemoveExtraWidgetsFromSpec(
		UWidgetBlueprint* WidgetBlueprint,
		const FString& CanonicalWidgetPath,
		const TArray<FWidgetSpecNodeData>& Nodes,
		TMap<FString, FWidgetInfo>& InOutExistingWidgetsByName,
		TArray<FString>& InOutChangedItems,
		TArray<FString>& InOutWarnings,
		TSharedPtr<FJsonObject>& OutError)
	{
		TSet<FString> SpecWidgetNames;
		for (const FWidgetSpecNodeData& Node : Nodes)
		{
			SpecWidgetNames.Add(Node.Name);
		}

		TSet<FString> ExtraWidgetNames;
		for (const TPair<FString, FWidgetInfo>& Pair : InOutExistingWidgetsByName)
		{
			if (!SpecWidgetNames.Contains(Pair.Key))
			{
				if (Pair.Value.bIsRootWidget)
				{
					OutError = BuildSpecError(
						TEXT("apply_widget_spec"),
						TEXT("ROOT_REPLACEMENT_NOT_SUPPORTED"),
						FString::Printf(TEXT("The live root widget '%s' is not present in the spec. Replacing an existing root widget is not supported automatically."), *Pair.Key),
						{ TEXT("Create a fresh widget asset from the spec, or manually replace the live root before reapplying.") }
					);
					return false;
				}

				ExtraWidgetNames.Add(Pair.Key);
			}
		}

		TArray<FString> ExtraRemovalRoots;
		for (const FString& ExtraWidgetName : ExtraWidgetNames)
		{
			const FWidgetInfo* WidgetInfo = InOutExistingWidgetsByName.Find(ExtraWidgetName);
			if (!WidgetInfo)
			{
				continue;
			}

			if (!ExtraWidgetNames.Contains(WidgetInfo->ParentWidget))
			{
				ExtraRemovalRoots.Add(ExtraWidgetName);
			}
		}

		ExtraRemovalRoots.Sort();
		for (const FString& RemovalRootName : ExtraRemovalRoots)
		{
			const FWidgetRemoveComponentResult RemoveResult = UWidgetService::RemoveComponent(CanonicalWidgetPath, RemovalRootName, true);
			if (!RemoveResult.bSuccess)
			{
				OutError = BuildSpecError(
					TEXT("apply_widget_spec"),
					TEXT("REMOVE_COMPONENT_FAILED"),
					RemoveResult.ErrorMessage.IsEmpty()
						? FString::Printf(TEXT("Failed to remove extra widget '%s' while synchronizing the spec."), *RemovalRootName)
						: RemoveResult.ErrorMessage,
					{ TEXT("Use export_widget_review to inspect the intended tree, then confirm whether remove_missing_widgets should stay enabled.") }
				);
				return false;
			}

			if (RemoveResult.RemovedComponents.Num() > 0)
			{
				InOutChangedItems.Add(FString::Printf(TEXT("removed widget subtree %s (%d widget(s))"), *RemovalRootName, RemoveResult.RemovedComponents.Num()));
			}
			else
			{
				InOutChangedItems.Add(FString::Printf(TEXT("removed widget %s"), *RemovalRootName));
			}
		}

		if (ExtraRemovalRoots.Num() > 0)
		{
			WidgetBlueprint->Modify();
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
			RefreshExistingWidgetsByName(CanonicalWidgetPath, InOutExistingWidgetsByName);
		}

		return true;
	}

	static bool AddMissingWidgetsFromSpec(
		const FString& CanonicalWidgetPath,
		const TArray<FWidgetSpecNodeData>& Nodes,
		TMap<FString, FWidgetInfo>& InOutExistingWidgetsByName,
		TArray<FString>& InOutChangedItems,
		TSharedPtr<FJsonObject>& OutError)
	{
		int32 RemainingNodes = Nodes.Num();
		int32 SafetyCounter = 0;
		while (RemainingNodes > 0 && SafetyCounter < Nodes.Num() * Nodes.Num())
		{
			bool bMadeProgress = false;
			for (const FWidgetSpecNodeData& Node : Nodes)
			{
				if (InOutExistingWidgetsByName.Contains(Node.Name))
				{
					continue;
				}

				if (Node.Parent.IsEmpty())
				{
					bool bHasLiveRoot = false;
					for (const TPair<FString, FWidgetInfo>& ExistingPair : InOutExistingWidgetsByName)
					{
						if (ExistingPair.Value.bIsRootWidget)
						{
							bHasLiveRoot = true;
							break;
						}
					}

					if (bHasLiveRoot)
					{
						OutError = BuildSpecError(
							TEXT("apply_widget_spec"),
							TEXT("ROOT_REPLACEMENT_NOT_SUPPORTED"),
							FString::Printf(TEXT("The spec expects root widget '%s', but the live Widget Blueprint already has a different root widget."), *Node.Name),
							{ TEXT("Create a fresh widget asset from the spec, or manually replace the existing root widget before reapplying.") }
						);
						return false;
					}
				}
				else if (!InOutExistingWidgetsByName.Contains(Node.Parent))
				{
					continue;
				}

				const FWidgetAddComponentResult AddResult = UWidgetService::AddComponent(
					CanonicalWidgetPath,
					Node.Type,
					Node.Name,
					Node.Parent,
					Node.bIsVariable);
				if (!AddResult.bSuccess)
				{
					OutError = BuildSpecError(
						TEXT("apply_widget_spec"),
						TEXT("ADD_COMPONENT_FAILED"),
						AddResult.ErrorMessage.IsEmpty()
							? FString::Printf(TEXT("Failed to add component '%s' while applying the widget spec."), *Node.Name)
							: AddResult.ErrorMessage,
						{ TEXT("Use export_widget_review to inspect the intended tree, then compare it against the live widget hierarchy.") }
					);
					return false;
				}

				bMadeProgress = true;
				--RemainingNodes;
				FWidgetInfo NewInfo;
				NewInfo.WidgetName = AddResult.ComponentName;
				NewInfo.WidgetClass = AddResult.ComponentType;
				NewInfo.ParentWidget = Node.Parent;
				NewInfo.bIsVariable = Node.bIsVariable;
				NewInfo.bIsRootWidget = Node.Parent.IsEmpty();
				InOutExistingWidgetsByName.Add(Node.Name, NewInfo);
				InOutChangedItems.Add(FString::Printf(TEXT("added component %s (%s) under %s"), *Node.Name, *Node.Type, Node.Parent.IsEmpty() ? TEXT("(root)") : *Node.Parent));
			}

			if (!bMadeProgress)
			{
				break;
			}

			++SafetyCounter;
		}

		if (RemainingNodes > 0)
		{
			OutError = BuildSpecError(
				TEXT("apply_widget_spec"),
				TEXT("TREE_RESOLUTION_FAILED"),
				TEXT("Failed to resolve the widget tree order from the spec. Ensure every node's parent appears in tree.nodes and that the root node has parent=null.")
			);
			return false;
		}

		return true;
	}

	static bool ReparentExistingWidgetsToMatchSpec(
		UWidgetBlueprint* WidgetBlueprint,
		const TArray<FWidgetSpecNodeData>& Nodes,
		TMap<FString, FWidgetInfo>& InOutExistingWidgetsByName,
		TArray<FString>& InOutChangedItems,
		TSharedPtr<FJsonObject>& OutError)
	{
		for (const FWidgetSpecNodeData& Node : Nodes)
		{
			FWidgetInfo* ExistingInfo = InOutExistingWidgetsByName.Find(Node.Name);
			if (!ExistingInfo)
			{
				continue;
			}

			if (Node.Parent.Equals(ExistingInfo->ParentWidget, ESearchCase::IgnoreCase))
			{
				continue;
			}

			if (Node.Parent.IsEmpty() || ExistingInfo->bIsRootWidget)
			{
				OutError = BuildSpecError(
					TEXT("apply_widget_spec"),
					TEXT("ROOT_REPLACEMENT_NOT_SUPPORTED"),
					FString::Printf(TEXT("Widget '%s' would need to move %s root position, but replacing an existing root widget is not supported automatically."),
						*Node.Name,
						Node.Parent.IsEmpty() ? TEXT("into the") : TEXT("out of the")),
					{ TEXT("Create a fresh widget asset from the spec, or manually repair the live root hierarchy before reapplying.") }
				);
				return false;
			}

			UWidget* Widget = FindWidgetByName(WidgetBlueprint, Node.Name);
			UWidget* NewParentWidget = FindWidgetByName(WidgetBlueprint, Node.Parent);
			UPanelWidget* OldParent = Widget ? Widget->GetParent() : nullptr;
			UPanelWidget* NewParent = Cast<UPanelWidget>(NewParentWidget);
			if (!Widget || !OldParent || !NewParent)
			{
				OutError = BuildSpecError(
					TEXT("apply_widget_spec"),
					TEXT("REPARENT_FAILED"),
					FString::Printf(TEXT("Failed to reparent '%s' under '%s' because the live widget or parent panel was not found."), *Node.Name, *Node.Parent),
					{ TEXT("Open the Widget Blueprint designer and confirm the live hierarchy before reapplying.") }
				);
				return false;
			}

			if (!OldParent->RemoveChild(Widget))
			{
				OutError = BuildSpecError(
					TEXT("apply_widget_spec"),
					TEXT("REPARENT_FAILED"),
					FString::Printf(TEXT("Failed to detach '%s' from its current parent while reparenting."), *Node.Name)
				);
				return false;
			}

			if (!NewParent->AddChild(Widget))
			{
				OutError = BuildSpecError(
					TEXT("apply_widget_spec"),
					TEXT("REPARENT_FAILED"),
					FString::Printf(TEXT("Failed to attach '%s' under new parent '%s'."), *Node.Name, *Node.Parent)
				);
				return false;
			}

			WidgetBlueprint->Modify();
			Widget->Modify();
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
			InOutChangedItems.Add(FString::Printf(TEXT("reparented %s from %s to %s"), *Node.Name, ExistingInfo->ParentWidget.IsEmpty() ? TEXT("(root)") : *ExistingInfo->ParentWidget, *Node.Parent));
			ExistingInfo->ParentWidget = Node.Parent;
			ExistingInfo->bIsRootWidget = false;
		}

		return true;
	}

	static bool EnsureTreeMatchesSpec(
		UWidgetBlueprint* WidgetBlueprint,
		const FString& CanonicalWidgetPath,
		const TArray<FWidgetSpecNodeData>& Nodes,
		bool bRemoveMissingWidgets,
		TArray<FString>& InOutChangedItems,
		TArray<FString>& InOutWarnings,
		TSharedPtr<FJsonObject>& OutError)
	{
		if (!WidgetBlueprint)
		{
			OutError = BuildSpecError(TEXT("apply_widget_spec"), TEXT("WIDGET_LOAD_FAILED"), TEXT("Widget Blueprint failed to load before applying the tree."));
			return false;
		}

		TMap<FString, FWidgetInfo> ExistingWidgetsByName;
		RefreshExistingWidgetsByName(CanonicalWidgetPath, ExistingWidgetsByName);

		if (bRemoveMissingWidgets)
		{
			if (!RemoveExtraWidgetsFromSpec(WidgetBlueprint, CanonicalWidgetPath, Nodes, ExistingWidgetsByName, InOutChangedItems, InOutWarnings, OutError))
			{
				return false;
			}
		}

		if (!AddMissingWidgetsFromSpec(CanonicalWidgetPath, Nodes, ExistingWidgetsByName, InOutChangedItems, OutError))
		{
			return false;
		}

		RefreshExistingWidgetsByName(CanonicalWidgetPath, ExistingWidgetsByName);
		if (!ReparentExistingWidgetsToMatchSpec(WidgetBlueprint, Nodes, ExistingWidgetsByName, InOutChangedItems, OutError))
		{
			return false;
		}

		return true;
	}

	static void ApplyWidgetPropertiesFromSpec(
		const FString& CanonicalWidgetPath,
		const TSharedPtr<FJsonObject>& SpecObject,
		TArray<FString>& InOutChangedItems,
		TArray<FString>& InOutWarnings)
	{
		const TSharedPtr<FJsonObject>* WidgetsObject = nullptr;
		if (!SpecObject->TryGetObjectField(TEXT("widgets"), WidgetsObject) || !WidgetsObject || !(*WidgetsObject).IsValid())
		{
			return;
		}

		static const TArray<FString> SectionNames = {
			TEXT("content"),
			TEXT("appearance"),
			TEXT("behavior"),
			TEXT("accessibility"),
			TEXT("advanced")
		};

		for (const TPair<FString, TSharedPtr<FJsonValue>>& WidgetPair : (*WidgetsObject)->Values)
		{
			if (!WidgetPair.Value.IsValid() || WidgetPair.Value->Type != EJson::Object)
			{
				continue;
			}

			const FString& WidgetName = WidgetPair.Key;
			const TSharedPtr<FJsonObject> WidgetObject = WidgetPair.Value->AsObject();
			for (const FString& SectionName : SectionNames)
			{
				const TSharedPtr<FJsonObject>* SectionObject = nullptr;
				if (!WidgetObject->TryGetObjectField(SectionName, SectionObject) || !SectionObject || !(*SectionObject).IsValid())
				{
					continue;
				}

				for (const TPair<FString, TSharedPtr<FJsonValue>>& PropertyPair : (*SectionObject)->Values)
				{
					const FString PropertyValue = JsonValueToString(PropertyPair.Value);
					if (PropertyValue.IsEmpty())
					{
						continue;
					}

					if (UWidgetService::SetProperty(CanonicalWidgetPath, WidgetName, PropertyPair.Key, PropertyValue))
					{
						InOutChangedItems.Add(FString::Printf(TEXT("set %s.%s = %s"), *WidgetName, *PropertyPair.Key, *PropertyValue));
					}
					else
					{
						InOutWarnings.Add(FString::Printf(TEXT("Failed to set %s.%s from the widget spec."), *WidgetName, *PropertyPair.Key));
					}
				}
			}
		}
	}

	static void ApplyWidgetSlotsFromSpec(
		UWidgetBlueprint* WidgetBlueprint,
		const FString& CanonicalWidgetPath,
		const TSharedPtr<FJsonObject>& SpecObject,
		TArray<FString>& InOutChangedItems,
		TArray<FString>& InOutWarnings)
	{
		const TSharedPtr<FJsonObject>* SlotsObject = nullptr;
		if (!SpecObject->TryGetObjectField(TEXT("slots"), SlotsObject) || !SlotsObject || !(*SlotsObject).IsValid())
		{
			return;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& SlotPair : (*SlotsObject)->Values)
		{
			if (!SlotPair.Value.IsValid() || SlotPair.Value->Type != EJson::Object)
			{
				continue;
			}

			if (UWidget* Widget = FindWidgetByName(WidgetBlueprint, SlotPair.Key))
			{
				ApplySupportedPanelSlotLayout(WidgetBlueprint, Widget, SlotPair.Value->AsObject(), SlotPair.Key, InOutChangedItems, InOutWarnings);
			}
			else
			{
				InOutWarnings.Add(FString::Printf(TEXT("Skipped slot layout for '%s' because that widget was not found after tree apply."), *SlotPair.Key));
			}
		}
	}

	static void ApplyBindingsFromSpec(
		const FString& CanonicalWidgetPath,
		const TSharedPtr<FJsonObject>& SpecObject,
		TArray<FString>& InOutChangedItems,
		TArray<FString>& InOutWarnings)
	{
		const TSharedPtr<FJsonObject>* BindingsObject = nullptr;
		if (!SpecObject->TryGetObjectField(TEXT("bindings"), BindingsObject) || !BindingsObject || !(*BindingsObject).IsValid())
		{
			return;
		}

		const TSharedPtr<FJsonObject>* MVVMObject = nullptr;
		if ((*BindingsObject)->TryGetObjectField(TEXT("mvvm"), MVVMObject) && MVVMObject && (*MVVMObject).IsValid())
		{
			const TArray<FWidgetViewModelInfo> ExistingViewModels = UWidgetService::ListViewModels(CanonicalWidgetPath);
			TSet<FString> ExistingViewModelNames;
			for (const FWidgetViewModelInfo& ExistingViewModel : ExistingViewModels)
			{
				ExistingViewModelNames.Add(ExistingViewModel.ViewModelName);
			}

			const TArray<TSharedPtr<FJsonValue>>* ViewModelValues = nullptr;
			if ((*MVVMObject)->TryGetArrayField(TEXT("view_models"), ViewModelValues) && ViewModelValues)
			{
				for (const TSharedPtr<FJsonValue>& ViewModelValue : *ViewModelValues)
				{
					if (!ViewModelValue.IsValid() || ViewModelValue->Type != EJson::Object)
					{
						continue;
					}

					const TSharedPtr<FJsonObject> ViewModelObject = ViewModelValue->AsObject();
					const FString ViewModelName = GetJsonStringField(ViewModelObject, TEXT("name"));
					if (ViewModelName.IsEmpty() || ExistingViewModelNames.Contains(ViewModelName))
					{
						continue;
					}

					const FString ViewModelClass = GetJsonStringField(ViewModelObject, TEXT("class"));
					const FString CreationType = GetJsonStringField(ViewModelObject, TEXT("creation_type"));
					if (UWidgetService::AddViewModel(CanonicalWidgetPath, ViewModelClass, ViewModelName, CreationType.IsEmpty() ? TEXT("CreateInstance") : CreationType))
					{
						InOutChangedItems.Add(FString::Printf(TEXT("added view model %s"), *ViewModelName));
						ExistingViewModelNames.Add(ViewModelName);
					}
					else
					{
						InOutWarnings.Add(FString::Printf(TEXT("Failed to add view model '%s' from the spec."), *ViewModelName));
					}
				}
			}

			const TArray<FWidgetViewModelBindingInfo> ExistingBindings = UWidgetService::ListViewModelBindings(CanonicalWidgetPath);
			TSet<FString> ExistingBindingKeys;
			for (const FWidgetViewModelBindingInfo& ExistingBinding : ExistingBindings)
			{
				ExistingBindingKeys.Add(ExistingBinding.SourcePath + TEXT("->") + ExistingBinding.DestinationPath + TEXT("|") + ExistingBinding.BindingMode);
			}

			const TArray<TSharedPtr<FJsonValue>>* BindingValues = nullptr;
			if ((*MVVMObject)->TryGetArrayField(TEXT("property_bindings"), BindingValues) && BindingValues)
			{
				for (const TSharedPtr<FJsonValue>& BindingValue : *BindingValues)
				{
					if (!BindingValue.IsValid() || BindingValue->Type != EJson::Object)
					{
						continue;
					}

					const TSharedPtr<FJsonObject> BindingObject = BindingValue->AsObject();
					const FString RawSourcePath = GetJsonStringField(BindingObject, TEXT("raw_source_path"));
					const FString RawDestinationPath = GetJsonStringField(BindingObject, TEXT("raw_destination_path"));
					const FString BindingMode = GetJsonStringField(BindingObject, TEXT("binding_mode"));
					const FString BindingKey = RawSourcePath + TEXT("->") + RawDestinationPath + TEXT("|") + BindingMode;
					if (!BindingKey.IsEmpty() && ExistingBindingKeys.Contains(BindingKey))
					{
						continue;
					}

					const FString ViewModelName = GetJsonStringField(BindingObject, TEXT("view_model_name"));
					const FString ViewModelProperty = GetJsonStringField(BindingObject, TEXT("view_model_property"));
					const FString WidgetName = GetJsonStringField(BindingObject, TEXT("widget_name"));
					const FString WidgetProperty = GetJsonStringField(BindingObject, TEXT("widget_property"));
					if (ViewModelName.IsEmpty() || ViewModelProperty.IsEmpty() || WidgetName.IsEmpty() || WidgetProperty.IsEmpty())
					{
						InOutWarnings.Add(TEXT("Skipped an MVVM binding because view_model_name/view_model_property/widget_name/widget_property was incomplete."));
						continue;
					}

					if (UWidgetService::AddViewModelBinding(CanonicalWidgetPath, ViewModelName, ViewModelProperty, WidgetName, WidgetProperty, BindingMode.IsEmpty() ? TEXT("OneWayToDestination") : BindingMode))
					{
						InOutChangedItems.Add(FString::Printf(TEXT("added MVVM binding %s.%s -> %s.%s"), *ViewModelName, *ViewModelProperty, *WidgetName, *WidgetProperty));
						ExistingBindingKeys.Add(BindingKey);
					}
					else
					{
						InOutWarnings.Add(FString::Printf(TEXT("Failed to add MVVM binding %s.%s -> %s.%s."), *ViewModelName, *ViewModelProperty, *WidgetName, *WidgetProperty));
					}
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* EventsArray = nullptr;
		if ((*BindingsObject)->TryGetArrayField(TEXT("events"), EventsArray) && EventsArray)
		{
			bool bRecordedEventBinding = false;
			for (const TSharedPtr<FJsonValue>& EventValue : *EventsArray)
			{
				if (!EventValue.IsValid() || EventValue->Type != EJson::Object)
				{
					continue;
				}

				const TSharedPtr<FJsonObject> EventObject = EventValue->AsObject();
				const FString WidgetName = GetJsonStringField(EventObject, TEXT("widget_name"));
				const FString EventName = GetJsonStringField(EventObject, TEXT("event_name"));
				const FString FunctionName = GetJsonStringField(EventObject, TEXT("function_name"));
				if (WidgetName.IsEmpty() || EventName.IsEmpty() || FunctionName.IsEmpty())
				{
					InOutWarnings.Add(TEXT("Skipped an event binding because widget_name/event_name/function_name was incomplete."));
					continue;
				}

				if (UWidgetService::BindEvent(CanonicalWidgetPath, WidgetName, EventName, FunctionName))
				{
					InOutChangedItems.Add(FString::Printf(TEXT("recorded event binding %s.%s -> %s"), *WidgetName, *EventName, *FunctionName));
					bRecordedEventBinding = true;
				}
				else
				{
					InOutWarnings.Add(FString::Printf(TEXT("Failed to record event binding %s.%s -> %s."), *WidgetName, *EventName, *FunctionName));
				}
			}

			if (bRecordedEventBinding)
			{
				InOutWarnings.Add(TEXT("Event bindings were recorded component-by-component, but full Blueprint graph wiring still requires manual verification."));
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* DelegatesArray = nullptr;
		if ((*BindingsObject)->TryGetArrayField(TEXT("delegates"), DelegatesArray) && DelegatesArray)
		{
			bool bRecordedDelegateBinding = false;
			for (const TSharedPtr<FJsonValue>& DelegateValue : *DelegatesArray)
			{
				if (!DelegateValue.IsValid() || DelegateValue->Type != EJson::Object)
				{
					continue;
				}

				const TSharedPtr<FJsonObject> DelegateObject = DelegateValue->AsObject();
				const FString WidgetName = GetJsonStringField(DelegateObject, TEXT("widget_name"));
				const FString DelegateName = GetJsonStringField(DelegateObject, TEXT("delegate_name"));
				const FString FunctionName = GetJsonStringField(DelegateObject, TEXT("function_name"));
				if (WidgetName.IsEmpty() || DelegateName.IsEmpty() || FunctionName.IsEmpty())
				{
					InOutWarnings.Add(TEXT("Skipped a delegate binding because widget_name/delegate_name/function_name was incomplete."));
					continue;
				}

				if (UWidgetService::BindEvent(CanonicalWidgetPath, WidgetName, DelegateName, FunctionName))
				{
					InOutChangedItems.Add(FString::Printf(TEXT("recorded delegate binding %s.%s -> %s"), *WidgetName, *DelegateName, *FunctionName));
					bRecordedDelegateBinding = true;
				}
				else
				{
					InOutWarnings.Add(FString::Printf(TEXT("Failed to record delegate binding %s.%s -> %s."), *WidgetName, *DelegateName, *FunctionName));
				}
			}

			if (bRecordedDelegateBinding)
			{
				InOutWarnings.Add(TEXT("Delegate bindings were recorded component-by-component, but arbitrary delegate graph wiring still requires manual verification."));
			}
		}
	}

	static FString IndentLines(const FString& Input, int32 Spaces)
	{
		if (Input.IsEmpty())
		{
			return FString();
		}

		const FString Prefix = FString::ChrN(Spaces, TCHAR(' '));
		TArray<FString> Lines;
		Input.ParseIntoArrayLines(Lines, false);

		FString Output;
		for (int32 Index = 0; Index < Lines.Num(); ++Index)
		{
			Output += Prefix + Lines[Index];
			if (Index + 1 < Lines.Num())
			{
				Output += TEXT("\n");
			}
		}

		return Output;
	}

	static FString BuildTreeMarkdownRecursive(const FString& NodeName, const TMap<FString, FWidgetSpecNodeData>& NodesByName, const TMap<FString, TArray<FString>>& ChildrenByParent, int32 Depth)
	{
		const FWidgetSpecNodeData* Node = NodesByName.Find(NodeName);
		if (!Node)
		{
			return FString();
		}

		FString Output = FString::ChrN(Depth * 2, TCHAR(' ')) + FString::Printf(TEXT("- %s (%s)\n"), *Node->Name, *Node->Type);
		if (const TArray<FString>* Children = ChildrenByParent.Find(NodeName))
		{
			for (const FString& ChildName : *Children)
			{
				Output += BuildTreeMarkdownRecursive(ChildName, NodesByName, ChildrenByParent, Depth + 1);
			}
		}

		return Output;
	}

	static bool IsPriorityPropertyName(const FString& PropertyName)
	{
		static const TSet<FString> PriorityNames = {
			TEXT("Text"),
			TEXT("HintText"),
			TEXT("Justification"),
			TEXT("Visibility"),
			TEXT("bIsEnabled"),
			TEXT("ColorAndOpacity"),
			TEXT("Font"),
			TEXT("Percent"),
			TEXT("CheckedState")
		};
		return PriorityNames.Contains(PropertyName);
	}

	static void AppendPropertySectionMarkdown(FString& InOutMarkdown, const FString& Heading, const TSharedPtr<FJsonObject>* SectionObject)
	{
		if (!SectionObject || !(*SectionObject).IsValid() || (*SectionObject)->Values.Num() == 0)
		{
			return;
		}

		TArray<FString> PropertyNames;
		(*SectionObject)->Values.GetKeys(PropertyNames);
		PropertyNames.Sort([](const FString& Left, const FString& Right)
		{
			const bool bLeftPriority = IsPriorityPropertyName(Left);
			const bool bRightPriority = IsPriorityPropertyName(Right);
			if (bLeftPriority != bRightPriority)
			{
				return bLeftPriority;
			}
			return Left < Right;
		});

		InOutMarkdown += FString::Printf(TEXT("#### %s\n"), *Heading);
		for (const FString& PropertyName : PropertyNames)
		{
			if (const TSharedPtr<FJsonValue>* PropertyValue = (*SectionObject)->Values.Find(PropertyName))
			{
				InOutMarkdown += FString::Printf(TEXT("- `%s`: `%s`\n"), *PropertyName, *JsonValueToString(*PropertyValue));
			}
		}
		InOutMarkdown += TEXT("\n");
	}

	static bool IsPriorityLayoutFieldName(const FString& FieldName)
	{
		static const TSet<FString> PriorityNames = {
			TEXT("anchors"),
			TEXT("alignment"),
			TEXT("position"),
			TEXT("auto_size"),
			TEXT("size"),
			TEXT("offsets"),
			TEXT("padding"),
			TEXT("horizontal_alignment"),
			TEXT("vertical_alignment"),
			TEXT("row"),
			TEXT("column")
		};
		return PriorityNames.Contains(FieldName);
	}

	static FString FormatJsonValueForMarkdown(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid() || Value->IsNull())
		{
			return TEXT("null");
		}

		if (Value->Type == EJson::Array)
		{
			TArray<FString> Parts;
			for (const TSharedPtr<FJsonValue>& ArrayValue : Value->AsArray())
			{
				Parts.Add(JsonValueToString(ArrayValue));
			}
			return FString::Printf(TEXT("[%s]"), *FString::Join(Parts, TEXT(", ")));
		}

		if (Value->Type == EJson::Object)
		{
			return SerializeJson(Value->AsObject(), false);
		}

		return JsonValueToString(Value);
	}

	static void AppendJsonObjectSectionMarkdown(FString& InOutMarkdown, const FString& Heading, const TSharedPtr<FJsonObject>* SectionObject, bool bUseLayoutPriority = false)
	{
		if (!SectionObject || !(*SectionObject).IsValid() || (*SectionObject)->Values.Num() == 0)
		{
			return;
		}

		TArray<FString> FieldNames;
		(*SectionObject)->Values.GetKeys(FieldNames);
		FieldNames.Sort([bUseLayoutPriority](const FString& Left, const FString& Right)
		{
			if (bUseLayoutPriority)
			{
				const bool bLeftPriority = IsPriorityLayoutFieldName(Left);
				const bool bRightPriority = IsPriorityLayoutFieldName(Right);
				if (bLeftPriority != bRightPriority)
				{
					return bLeftPriority;
				}
			}
			return Left < Right;
		});

		InOutMarkdown += FString::Printf(TEXT("#### %s\n"), *Heading);
		for (const FString& FieldName : FieldNames)
		{
			if (const TSharedPtr<FJsonValue>* FieldValue = (*SectionObject)->Values.Find(FieldName))
			{
				InOutMarkdown += FString::Printf(TEXT("- `%s`: `%s`\n"), *FieldName, *FormatJsonValueForMarkdown(*FieldValue));
			}
		}
		InOutMarkdown += TEXT("\n");
	}

	static FString BuildBindingsYamlBlock(const TSharedPtr<FJsonObject>& SpecObject)
	{
		const TSharedPtr<FJsonObject>* BindingsObject = nullptr;
		if (!SpecObject->TryGetObjectField(TEXT("bindings"), BindingsObject) || !BindingsObject || !(*BindingsObject).IsValid())
		{
			return TEXT("mvvm:\n  view_models: []\n  property_bindings: []\nevents: []\ndelegates: []");
		}

		FString Output;
		Output += TEXT("mvvm:\n");
		const TSharedPtr<FJsonObject>* MVVMObject = nullptr;
		if ((*BindingsObject)->TryGetObjectField(TEXT("mvvm"), MVVMObject) && MVVMObject && (*MVVMObject).IsValid())
		{
			Output += TEXT("  view_models:\n");
			const TArray<TSharedPtr<FJsonValue>>* ViewModels = nullptr;
			if ((*MVVMObject)->TryGetArrayField(TEXT("view_models"), ViewModels) && ViewModels && ViewModels->Num() > 0)
			{
				for (const TSharedPtr<FJsonValue>& ViewModelValue : *ViewModels)
				{
					if (!ViewModelValue.IsValid() || ViewModelValue->Type != EJson::Object)
					{
						continue;
					}

					const TSharedPtr<FJsonObject> ViewModelObject = ViewModelValue->AsObject();
					Output += FString::Printf(TEXT("    - name: %s\n"), *GetJsonStringField(ViewModelObject, TEXT("name")));
					Output += FString::Printf(TEXT("      class: %s\n"), *GetJsonStringField(ViewModelObject, TEXT("class")));
					Output += FString::Printf(TEXT("      creation_type: %s\n"), *GetJsonStringField(ViewModelObject, TEXT("creation_type")));
				}
			}
			else
			{
				Output += TEXT("    []\n");
			}

			Output += TEXT("  property_bindings:\n");
			const TArray<TSharedPtr<FJsonValue>>* PropertyBindings = nullptr;
			if ((*MVVMObject)->TryGetArrayField(TEXT("property_bindings"), PropertyBindings) && PropertyBindings && PropertyBindings->Num() > 0)
			{
				for (const TSharedPtr<FJsonValue>& BindingValue : *PropertyBindings)
				{
					if (!BindingValue.IsValid() || BindingValue->Type != EJson::Object)
					{
						continue;
					}

					const TSharedPtr<FJsonObject> BindingObject = BindingValue->AsObject();
					Output += TEXT("    - source:\n");
					Output += FString::Printf(TEXT("        view_model: %s\n"), *GetJsonStringField(BindingObject, TEXT("view_model_name")));
					Output += FString::Printf(TEXT("        path: %s\n"), *GetJsonStringField(BindingObject, TEXT("view_model_property")));
					Output += TEXT("      target:\n");
					Output += FString::Printf(TEXT("        widget: %s\n"), *GetJsonStringField(BindingObject, TEXT("widget_name")));
					Output += FString::Printf(TEXT("        property: %s\n"), *GetJsonStringField(BindingObject, TEXT("widget_property")));
					Output += FString::Printf(TEXT("      mode: %s\n"), *GetJsonStringField(BindingObject, TEXT("binding_mode")));
				}
			}
			else
			{
				Output += TEXT("    []\n");
			}
		}
		else
		{
			Output += TEXT("  view_models: []\n");
			Output += TEXT("  property_bindings: []\n");
		}

		Output += TEXT("events:\n");
		const TArray<TSharedPtr<FJsonValue>>* Events = nullptr;
		if ((*BindingsObject)->TryGetArrayField(TEXT("events"), Events) && Events && Events->Num() > 0)
		{
			for (const TSharedPtr<FJsonValue>& EventValue : *Events)
			{
				if (!EventValue.IsValid() || EventValue->Type != EJson::Object)
				{
					continue;
				}

				const TSharedPtr<FJsonObject> EventObject = EventValue->AsObject();
				Output += FString::Printf(TEXT("  - widget: %s\n"), *GetJsonStringField(EventObject, TEXT("widget_name")));
				Output += FString::Printf(TEXT("    event: %s\n"), *GetJsonStringField(EventObject, TEXT("event_name")));
				Output += FString::Printf(TEXT("    function: %s\n"), *GetJsonStringField(EventObject, TEXT("function_name")));
			}
		}
		else
		{
			Output += TEXT("  []\n");
		}

		Output += TEXT("delegates:\n");
		const TArray<TSharedPtr<FJsonValue>>* Delegates = nullptr;
		if ((*BindingsObject)->TryGetArrayField(TEXT("delegates"), Delegates) && Delegates && Delegates->Num() > 0)
		{
			for (const TSharedPtr<FJsonValue>& DelegateValue : *Delegates)
			{
				if (!DelegateValue.IsValid() || DelegateValue->Type != EJson::Object)
				{
					continue;
				}

				const TSharedPtr<FJsonObject> DelegateObject = DelegateValue->AsObject();
				Output += FString::Printf(TEXT("  - widget: %s\n"), *GetJsonStringField(DelegateObject, TEXT("widget_name")));
				Output += FString::Printf(TEXT("    delegate: %s\n"), *GetJsonStringField(DelegateObject, TEXT("delegate_name")));
				Output += FString::Printf(TEXT("    function: %s\n"), *GetJsonStringField(DelegateObject, TEXT("function_name")));
			}
		}
		else
		{
			Output += TEXT("  []\n");
		}
		return Output;
	}

	static FString BuildWidgetReviewMarkdown(const TSharedPtr<FJsonObject>& SpecObject, const FString& SpecPath)
	{
		const TSharedPtr<FJsonObject>* AssetObject = nullptr;
		const TSharedPtr<FJsonObject>* TreeObject = nullptr;
		const TSharedPtr<FJsonObject>* WidgetsObject = nullptr;
		const TSharedPtr<FJsonObject>* SlotsObject = nullptr;
		SpecObject->TryGetObjectField(TEXT("asset"), AssetObject);
		SpecObject->TryGetObjectField(TEXT("tree"), TreeObject);
		SpecObject->TryGetObjectField(TEXT("widgets"), WidgetsObject);
		SpecObject->TryGetObjectField(TEXT("slots"), SlotsObject);

		const FString WidgetPath = AssetObject && (*AssetObject).IsValid() ? GetJsonStringField(*AssetObject, TEXT("widget_path")) : TEXT("");
		const FString ParentClass = AssetObject && (*AssetObject).IsValid() ? GetJsonStringField(*AssetObject, TEXT("parent_class")) : TEXT("");
		const FString RootName = TreeObject && (*TreeObject).IsValid() ? GetJsonStringField(*TreeObject, TEXT("root")) : TEXT("");

		TSharedPtr<FJsonObject> TreeValue = (TreeObject && *TreeObject) ? *TreeObject : TSharedPtr<FJsonObject>();
		TArray<FWidgetSpecNodeData> Nodes = ParseSpecNodes(TreeValue);
		TMap<FString, FWidgetSpecNodeData> NodesByName;
		TMap<FString, TArray<FString>> ChildrenByParent;
		for (const FWidgetSpecNodeData& Node : Nodes)
		{
			NodesByName.Add(Node.Name, Node);
			ChildrenByParent.FindOrAdd(Node.Parent).Add(Node.Name);
		}

		FString Markdown;
		Markdown += FString::Printf(TEXT("# Widget Review: `%s`\n\n"), *WidgetPath);
		Markdown += TEXT("## Asset\n");
		Markdown += FString::Printf(TEXT("- Widget Path: `%s`\n"), *WidgetPath);
		Markdown += FString::Printf(TEXT("- Parent Class: `%s`\n"), *ParentClass);
		if (!SpecPath.IsEmpty())
		{
			Markdown += FString::Printf(TEXT("- Canonical Spec: `%s`\n"), *SpecPath);
		}
		Markdown += TEXT("\n");

		Markdown += TEXT("## Tree\n");
		if (!RootName.IsEmpty())
		{
			Markdown += BuildTreeMarkdownRecursive(RootName, NodesByName, ChildrenByParent, 0);
		}
		else
		{
			Markdown += TEXT("- (no root)\n");
		}
		Markdown += TEXT("\n");

		Markdown += TEXT("## Review Focus\n");
		Markdown += TEXT("- Confirm the parent/child tree is correct before applying.\n");
		Markdown += TEXT("- Confirm every `Layout` section matches the intended panel slot layout and parent panel type.\n");
		Markdown += TEXT("- Confirm only the expected widget properties are present; omitted properties are left unchanged/defaulted.\n\n");

		for (const FWidgetSpecNodeData& Node : Nodes)
		{
			Markdown += FString::Printf(TEXT("## %s\n"), *Node.Name);
			Markdown += TEXT("### Identity\n");
			Markdown += FString::Printf(TEXT("- Type: `%s`\n"), *Node.Type);
			Markdown += FString::Printf(TEXT("- Parent: `%s`\n"), Node.Parent.IsEmpty() ? TEXT("(root)") : *Node.Parent);
			Markdown += FString::Printf(TEXT("- Variable: `%s`\n\n"), Node.bIsVariable ? TEXT("true") : TEXT("false"));

			if (WidgetsObject && (*WidgetsObject).IsValid())
			{
				const TSharedPtr<FJsonObject>* WidgetObject = nullptr;
				if ((*WidgetsObject)->TryGetObjectField(Node.Name, WidgetObject) && WidgetObject && (*WidgetObject).IsValid())
				{
					Markdown += TEXT("### Props\n\n");
					const TSharedPtr<FJsonObject>* ContentSection = nullptr;
					const TSharedPtr<FJsonObject>* AppearanceSection = nullptr;
					const TSharedPtr<FJsonObject>* BehaviorSection = nullptr;
					const TSharedPtr<FJsonObject>* AccessibilitySection = nullptr;
					const TSharedPtr<FJsonObject>* AdvancedSection = nullptr;
					(*WidgetObject)->TryGetObjectField(TEXT("content"), ContentSection);
					(*WidgetObject)->TryGetObjectField(TEXT("appearance"), AppearanceSection);
					(*WidgetObject)->TryGetObjectField(TEXT("behavior"), BehaviorSection);
					(*WidgetObject)->TryGetObjectField(TEXT("accessibility"), AccessibilitySection);
					(*WidgetObject)->TryGetObjectField(TEXT("advanced"), AdvancedSection);

					const int32 PropertyCount =
						(ContentSection && (*ContentSection).IsValid() ? (*ContentSection)->Values.Num() : 0) +
						(AppearanceSection && (*AppearanceSection).IsValid() ? (*AppearanceSection)->Values.Num() : 0) +
						(BehaviorSection && (*BehaviorSection).IsValid() ? (*BehaviorSection)->Values.Num() : 0) +
						(AccessibilitySection && (*AccessibilitySection).IsValid() ? (*AccessibilitySection)->Values.Num() : 0) +
						(AdvancedSection && (*AdvancedSection).IsValid() ? (*AdvancedSection)->Values.Num() : 0);

					if (PropertyCount == 0)
					{
						Markdown += TEXT("- No explicit widget properties in spec.\n\n");
					}
					else
					{
						AppendPropertySectionMarkdown(Markdown, TEXT("Content"), ContentSection);
						AppendPropertySectionMarkdown(Markdown, TEXT("Appearance"), AppearanceSection);
						AppendPropertySectionMarkdown(Markdown, TEXT("Behavior"), BehaviorSection);
						AppendPropertySectionMarkdown(Markdown, TEXT("Accessibility"), AccessibilitySection);
						AppendPropertySectionMarkdown(Markdown, TEXT("Advanced"), AdvancedSection);
					}
				}
			}

			Markdown += TEXT("### Layout\n\n");
			if (SlotsObject && (*SlotsObject).IsValid())
			{
				const TSharedPtr<FJsonObject>* SlotObject = nullptr;
				if ((*SlotsObject)->TryGetObjectField(Node.Name, SlotObject) && SlotObject && (*SlotObject).IsValid())
				{
					Markdown += FString::Printf(TEXT("- Slot Type: `%s`\n"), *GetJsonStringField(*SlotObject, TEXT("slot_type")));
					const TSharedPtr<FJsonObject>* RequiredObject = nullptr;
					(*SlotObject)->TryGetObjectField(TEXT("required"), RequiredObject);
					AppendJsonObjectSectionMarkdown(Markdown, TEXT("Required"), RequiredObject, true);

					const TSharedPtr<FJsonObject>* OptionalObject = nullptr;
					(*SlotObject)->TryGetObjectField(TEXT("optional"), OptionalObject);
					AppendJsonObjectSectionMarkdown(Markdown, TEXT("Optional"), OptionalObject, true);
				}
				else
				{
					Markdown += TEXT("- No slot layout entry.\n");
				}
			}
			else
			{
				Markdown += TEXT("- No slot layout entry.\n");
			}
			Markdown += TEXT("\n");
		}

		Markdown += TEXT("## Bindings\n\n");
		Markdown += TEXT("```yaml\n");
		Markdown += BuildBindingsYamlBlock(SpecObject);
		Markdown += TEXT("```\n");
		return Markdown;
	}

	static void GatherWidgetBlueprintAssetsForBatch(const FString& PathFilter, int32 MaxResults, TArray<FAssetData>& OutAssets, int32& OutTruncatedCount)
	{
		OutAssets.Reset();
		OutTruncatedCount = 0;

		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		FARFilter Filter;
		Filter.ClassPaths.Add(WidgetBlueprintClassPath);
		Filter.bRecursivePaths = true;
		if (!PathFilter.IsEmpty())
		{
			Filter.PackagePaths.Add(FName(*PathFilter));
		}

		AssetRegistry.GetAssets(Filter, OutAssets);
		OutAssets.Sort([](const FAssetData& Left, const FAssetData& Right)
		{
			return Left.GetObjectPathString() < Right.GetObjectPathString();
		});

		if (MaxResults > 0 && OutAssets.Num() > MaxResults)
		{
			OutTruncatedCount = OutAssets.Num() - MaxResults;
			OutAssets.SetNum(MaxResults);
		}
	}

	static FString BuildDefaultBatchManifestPath(const FString& PathFilter)
	{
		FString BaseName = PathFilter;
		BaseName.ReplaceInline(TEXT("/"), TEXT("__"));
		BaseName = FPaths::MakeValidFileName(BaseName);
		if (BaseName.IsEmpty())
		{
			BaseName = TEXT("AllWidgetBlueprints");
		}
		return FPaths::Combine(GetWidgetSpecDirectory(), BaseName + TEXT(".widget.batch.json"));
	}

	static FString HandleExportWidgetSpec(const TMap<FString, FString>& Params)
	{
		const FString RequestedWidgetPath = ExtractParam(Params, TEXT("widget_path"));
		if (RequestedWidgetPath.IsEmpty())
		{
			return SerializeJson(BuildSpecError(
				TEXT("export_widget_spec"),
				TEXT("MISSING_WIDGET_PATH"),
				TEXT("The 'widget_path' parameter is required.")
			));
		}

		UWidgetBlueprint* WidgetBlueprint = nullptr;
		FString CanonicalWidgetPath;
		TSharedPtr<FJsonObject> LoadError;
		if (!LoadWidgetBlueprintAsset(RequestedWidgetPath, WidgetBlueprint, CanonicalWidgetPath, LoadError, TEXT("export_widget_spec")))
		{
			return SerializeJson(LoadError);
		}

		const bool bIncludeAllProperties = ExtractBoolParam(Params, TEXT("include_all_properties"), false);
		const bool bWriteReview = ExtractBoolParam(Params, TEXT("write_review"), true);
		const FString SpecPath = ResolveOutputPath(ExtractParam(Params, TEXT("spec_path")), BuildDefaultSpecPath(CanonicalWidgetPath));
		const FString ReviewPath = ResolveOutputPath(ExtractParam(Params, TEXT("review_path")), BuildDefaultReviewPathFromWidgetPath(CanonicalWidgetPath));

		const TSharedPtr<FJsonObject> SpecObject = BuildWidgetSpecObject(WidgetBlueprint, CanonicalWidgetPath, bIncludeAllProperties);
		if (!SaveTextFile(SpecPath, SerializeJson(SpecObject, true)))
		{
			return SerializeJson(BuildSpecError(
				TEXT("export_widget_spec"),
				TEXT("SPEC_WRITE_FAILED"),
				FString::Printf(TEXT("Failed to write widget spec file '%s'."), *SpecPath)
			));
		}

		TArray<FString> ChangedItems = { FString::Printf(TEXT("wrote widget spec file %s"), *SpecPath) };
		TArray<FString> Warnings;
		if (bWriteReview)
		{
			if (SaveTextFile(ReviewPath, BuildWidgetReviewMarkdown(SpecObject, SpecPath)))
			{
				ChangedItems.Add(FString::Printf(TEXT("wrote widget review file %s"), *ReviewPath));
			}
			else
			{
				Warnings.Add(FString::Printf(TEXT("Failed to write widget review file '%s'."), *ReviewPath));
			}
		}

		const TSharedPtr<FJsonObject> DataObject = MakeShared<FJsonObject>();
		DataObject->SetStringField(TEXT("widget_path"), GetPackagePathFromAnyWidgetPath(CanonicalWidgetPath));
		DataObject->SetStringField(TEXT("object_path"), CanonicalWidgetPath);
		DataObject->SetStringField(TEXT("spec_path"), SpecPath);
		DataObject->SetStringField(TEXT("review_path"), bWriteReview ? ReviewPath : TEXT(""));
		DataObject->SetBoolField(TEXT("include_all_properties"), bIncludeAllProperties);
		DataObject->SetNumberField(TEXT("component_count"), UWidgetService::ListComponents(CanonicalWidgetPath).Num());
		DataObject->SetNumberField(TEXT("view_model_count"), UWidgetService::ListViewModels(CanonicalWidgetPath).Num());
		DataObject->SetNumberField(TEXT("binding_count"), UWidgetService::ListViewModelBindings(CanonicalWidgetPath).Num());

		return SerializeJson(BuildSpecSuccess(
			TEXT("export_widget_spec"),
			FString::Printf(TEXT("Exported widget spec for %s."), *CanonicalWidgetPath),
			DataObject,
			ChangedItems,
			{
				TEXT("Review the generated .widget.review.md file before apply_widget_spec."),
				TEXT("Edit the canonical .widget.json file if you want to change the tree, slot layout, or grouped widget properties.")
			},
			Warnings,
			{ CanonicalWidgetPath }
		));
	}

	static FString HandleExportWidgetSpecBatch(const TMap<FString, FString>& Params)
	{
		const FString PathFilter = ExtractParam(Params, TEXT("path_filter"));
		if (PathFilter.IsEmpty())
		{
			return SerializeJson(BuildSpecError(
				TEXT("export_widget_spec_batch"),
				TEXT("MISSING_PATH_FILTER"),
				TEXT("The 'path_filter' parameter is required, for example /Game/EasyGameUI.")
			));
		}

		const bool bIncludeAllProperties = ExtractBoolParam(Params, TEXT("include_all_properties"), false);
		const bool bWriteReview = ExtractBoolParam(Params, TEXT("write_review"), true);
		const int32 MaxResults = FMath::Clamp(FCString::Atoi(*ExtractParam(Params, TEXT("max_results"))), 0, 2000);

		TArray<FAssetData> Assets;
		int32 TruncatedCount = 0;
		GatherWidgetBlueprintAssetsForBatch(PathFilter, MaxResults == 0 ? 2000 : MaxResults, Assets, TruncatedCount);
		if (Assets.Num() == 0)
		{
			return SerializeJson(BuildSpecError(
				TEXT("export_widget_spec_batch"),
				TEXT("NO_WIDGETS_FOUND"),
				FString::Printf(TEXT("No Widget Blueprints were found under '%s'."), *PathFilter)
			));
		}

		const FString SpecOutputDir = ExtractParam(Params, TEXT("spec_output_dir"));
		const FString ReviewOutputDir = ExtractParam(Params, TEXT("review_output_dir"));
		const FString ManifestPath = ResolveOutputPath(ExtractParam(Params, TEXT("manifest_path")), BuildDefaultBatchManifestPath(PathFilter));

		TArray<FString> Warnings;
		TArray<FString> AffectedAssets;
		TArray<TSharedPtr<FJsonObject>> ExportObjects;

		for (const FAssetData& AssetData : Assets)
		{
			const FString CanonicalWidgetPath = AssetData.GetObjectPathString();
			UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(AssetData.GetAsset());
			if (!WidgetBlueprint)
			{
				WidgetBlueprint = Cast<UWidgetBlueprint>(UEditorAssetLibrary::LoadAsset(CanonicalWidgetPath));
			}

			if (!WidgetBlueprint)
			{
				Warnings.Add(FString::Printf(TEXT("Skipped %s because the Widget Blueprint failed to load."), *CanonicalWidgetPath));
				continue;
			}

			const FString SpecPath = SpecOutputDir.IsEmpty()
				? BuildDefaultSpecPath(CanonicalWidgetPath)
				: ResolveOutputPath(FPaths::Combine(SpecOutputDir, MakeWidgetSpecBaseName(CanonicalWidgetPath) + TEXT(".widget.json")), TEXT(""));
			const FString ReviewPath = ReviewOutputDir.IsEmpty()
				? BuildDefaultReviewPathFromWidgetPath(CanonicalWidgetPath)
				: ResolveOutputPath(FPaths::Combine(ReviewOutputDir, MakeWidgetSpecBaseName(CanonicalWidgetPath) + TEXT(".widget.review.md")), TEXT(""));

			const TSharedPtr<FJsonObject> SpecObject = BuildWidgetSpecObject(WidgetBlueprint, CanonicalWidgetPath, bIncludeAllProperties);
			if (!SaveTextFile(SpecPath, SerializeJson(SpecObject, true)))
			{
				Warnings.Add(FString::Printf(TEXT("Failed to write spec for %s."), *CanonicalWidgetPath));
				continue;
			}

			bool bReviewWritten = false;
			if (bWriteReview)
			{
				bReviewWritten = SaveTextFile(ReviewPath, BuildWidgetReviewMarkdown(SpecObject, SpecPath));
				if (!bReviewWritten)
				{
					Warnings.Add(FString::Printf(TEXT("Failed to write review markdown for %s."), *CanonicalWidgetPath));
				}
			}

			const TSharedPtr<FJsonObject> ExportObject = MakeShared<FJsonObject>();
			ExportObject->SetStringField(TEXT("widget_path"), GetPackagePathFromAnyWidgetPath(CanonicalWidgetPath));
			ExportObject->SetStringField(TEXT("object_path"), CanonicalWidgetPath);
			ExportObject->SetStringField(TEXT("spec_path"), SpecPath);
			ExportObject->SetStringField(TEXT("review_path"), bWriteReview && bReviewWritten ? ReviewPath : TEXT(""));
			ExportObject->SetNumberField(TEXT("component_count"), UWidgetService::ListComponents(CanonicalWidgetPath).Num());
			ExportObject->SetNumberField(TEXT("view_model_count"), UWidgetService::ListViewModels(CanonicalWidgetPath).Num());
			ExportObject->SetNumberField(TEXT("binding_count"), UWidgetService::ListViewModelBindings(CanonicalWidgetPath).Num());
			ExportObjects.Add(ExportObject);
			AffectedAssets.Add(CanonicalWidgetPath);
		}

		const TSharedPtr<FJsonObject> ManifestObject = MakeShared<FJsonObject>();
		ManifestObject->SetNumberField(TEXT("spec_version"), 1);
		ManifestObject->SetStringField(TEXT("kind"), TEXT("widget_spec_batch_export"));
		ManifestObject->SetStringField(TEXT("path_filter"), PathFilter);
		ManifestObject->SetBoolField(TEXT("include_all_properties"), bIncludeAllProperties);
		ManifestObject->SetBoolField(TEXT("write_review"), bWriteReview);
		ManifestObject->SetNumberField(TEXT("requested_count"), Assets.Num() + TruncatedCount);
		ManifestObject->SetNumberField(TEXT("exported_count"), ExportObjects.Num());
		ManifestObject->SetNumberField(TEXT("truncated_count"), TruncatedCount);
		AddJsonObjectArrayField(ManifestObject, TEXT("exports"), ExportObjects);

		if (!SaveTextFile(ManifestPath, SerializeJson(ManifestObject, true)))
		{
			Warnings.Add(FString::Printf(TEXT("Failed to write batch manifest file '%s'."), *ManifestPath));
		}

		const TSharedPtr<FJsonObject> DataObject = MakeShared<FJsonObject>();
		DataObject->SetStringField(TEXT("path_filter"), PathFilter);
		DataObject->SetStringField(TEXT("manifest_path"), ManifestPath);
		DataObject->SetNumberField(TEXT("requested_count"), Assets.Num() + TruncatedCount);
		DataObject->SetNumberField(TEXT("exported_count"), ExportObjects.Num());
		DataObject->SetNumberField(TEXT("truncated_count"), TruncatedCount);
		DataObject->SetBoolField(TEXT("include_all_properties"), bIncludeAllProperties);
		DataObject->SetBoolField(TEXT("write_review"), bWriteReview);

		return SerializeJson(BuildSpecSuccess(
			TEXT("export_widget_spec_batch"),
			FString::Printf(TEXT("Exported %d widget spec(s) under %s."), ExportObjects.Num(), *PathFilter),
			DataObject,
			{
				FString::Printf(TEXT("wrote widget batch manifest %s"), *ManifestPath)
			},
			{
				TEXT("Review the generated batch manifest first, then inspect the .widget.review.md files for the reusable base controls."),
				TEXT("Use the manifest as the inventory for inheritance/composition planning and MVVM retrofits.")
			},
			Warnings,
			AffectedAssets
		));
	}

	static FString HandleValidateWidgetSpec(const TMap<FString, FString>& Params)
	{
		TSharedPtr<FJsonObject> SpecObject;
		FString ResolvedSpecPath;
		TSharedPtr<FJsonObject> LoadError;
		if (!LoadSpecObject(Params, TEXT("validate_widget_spec"), SpecObject, ResolvedSpecPath, LoadError))
		{
			return SerializeJson(LoadError);
		}

		TArray<FString> Errors;
		TArray<FString> Warnings;
		TArray<FWidgetSpecNodeData> Nodes;
		const bool bIsValid = ValidateWidgetSpecObject(SpecObject, Errors, Warnings, Nodes);
		if (!bIsValid)
		{
			const TSharedPtr<FJsonObject> ErrorObject = BuildSpecError(
				TEXT("validate_widget_spec"),
				TEXT("SPEC_VALIDATION_FAILED"),
				FString::Printf(TEXT("Widget spec validation failed with %d error(s)."), Errors.Num()),
				{ TEXT("Fix the reported tree/widgets/slots/bindings issues, then rerun validate_widget_spec.") },
				Warnings
			);
			AddStringArrayField(ErrorObject, TEXT("validation_errors"), Errors);
			if (!ResolvedSpecPath.IsEmpty())
			{
				ErrorObject->SetStringField(TEXT("spec_path"), ResolvedSpecPath);
			}
			return SerializeJson(ErrorObject);
		}

		const TSharedPtr<FJsonObject> DataObject = MakeShared<FJsonObject>();
		if (!ResolvedSpecPath.IsEmpty())
		{
			DataObject->SetStringField(TEXT("spec_path"), ResolvedSpecPath);
		}
		DataObject->SetBoolField(TEXT("is_valid"), true);
		DataObject->SetNumberField(TEXT("widget_count"), Nodes.Num());
		AddStringArrayField(DataObject, TEXT("warnings"), Warnings);

		return SerializeJson(BuildSpecSuccess(
			TEXT("validate_widget_spec"),
			FString::Printf(TEXT("Widget spec is valid (%d widget node(s))."), Nodes.Num()),
			DataObject,
			{},
			{
				TEXT("Use widget_blueprint for incremental live edits."),
				TEXT("Keep apply_widget_spec disabled in MCP unless you are doing a controlled one-off materialization outside the normal live editing flow.")
			},
			Warnings
		));
	}

	static FString HandleApplyWidgetSpec(const TMap<FString, FString>& Params)
	{
		TSharedPtr<FJsonObject> SpecObject;
		FString ResolvedSpecPath;
		TSharedPtr<FJsonObject> LoadError;
		if (!LoadSpecObject(Params, TEXT("apply_widget_spec"), SpecObject, ResolvedSpecPath, LoadError))
		{
			return SerializeJson(LoadError);
		}

		TArray<FString> ValidationErrors;
		TArray<FString> ValidationWarnings;
		TArray<FWidgetSpecNodeData> Nodes;
		if (!ValidateWidgetSpecObject(SpecObject, ValidationErrors, ValidationWarnings, Nodes))
		{
			const TSharedPtr<FJsonObject> ErrorObject = BuildSpecError(
				TEXT("apply_widget_spec"),
				TEXT("SPEC_VALIDATION_FAILED"),
				FString::Printf(TEXT("Refusing to apply an invalid widget spec (%d error(s))."), ValidationErrors.Num()),
				{ TEXT("Run validate_widget_spec to inspect and fix the spec errors first.") },
				ValidationWarnings
			);
			AddStringArrayField(ErrorObject, TEXT("validation_errors"), ValidationErrors);
			return SerializeJson(ErrorObject);
		}

		TArray<FString> ChangedItems;
		TArray<FString> Warnings = ValidationWarnings;
		TSharedPtr<FJsonObject> ApplyError;
		UWidgetBlueprint* WidgetBlueprint = nullptr;
		FString CanonicalWidgetPath;
		if (!EnsureWidgetBlueprintForSpec(SpecObject, WidgetBlueprint, CanonicalWidgetPath, ChangedItems, Warnings, ApplyError))
		{
			return SerializeJson(ApplyError);
		}

		const TSharedPtr<FJsonObject>* ApplyObject = nullptr;
		bool bCompileAfter = true;
		bool bSaveAfter = true;
		bool bOpenAfter = false;
		bool bRemoveMissingWidgets = false;
		if (SpecObject->TryGetObjectField(TEXT("apply"), ApplyObject) && ApplyObject && (*ApplyObject).IsValid())
		{
			(*ApplyObject)->TryGetBoolField(TEXT("compile"), bCompileAfter);
			(*ApplyObject)->TryGetBoolField(TEXT("save"), bSaveAfter);
			(*ApplyObject)->TryGetBoolField(TEXT("open"), bOpenAfter);
			(*ApplyObject)->TryGetBoolField(TEXT("remove_missing_widgets"), bRemoveMissingWidgets);
		}

		if (const TOptional<bool> OverrideCompile = TryExtractBoolParam(Params, TEXT("compile_after")))
		{
			bCompileAfter = *OverrideCompile;
		}
		if (const TOptional<bool> OverrideSave = TryExtractBoolParam(Params, TEXT("save_after")))
		{
			bSaveAfter = *OverrideSave;
		}
		if (const TOptional<bool> OverrideOpen = TryExtractBoolParam(Params, TEXT("open_after")))
		{
			bOpenAfter = *OverrideOpen;
		}
		if (const TOptional<bool> OverrideRemoveMissing = TryExtractBoolParam(Params, TEXT("remove_missing_widgets")))
		{
			bRemoveMissingWidgets = *OverrideRemoveMissing;
		}

		if (!EnsureTreeMatchesSpec(WidgetBlueprint, CanonicalWidgetPath, Nodes, bRemoveMissingWidgets, ChangedItems, Warnings, ApplyError))
		{
			return SerializeJson(ApplyError);
		}

		WidgetBlueprint = Cast<UWidgetBlueprint>(UEditorAssetLibrary::LoadAsset(CanonicalWidgetPath));
		ApplyWidgetPropertiesFromSpec(CanonicalWidgetPath, SpecObject, ChangedItems, Warnings);
		ApplyWidgetSlotsFromSpec(WidgetBlueprint, CanonicalWidgetPath, SpecObject, ChangedItems, Warnings);
		ApplyBindingsFromSpec(CanonicalWidgetPath, SpecObject, ChangedItems, Warnings);

		FinalizeWidgetApply(WidgetBlueprint, CanonicalWidgetPath, bCompileAfter, bSaveAfter, bOpenAfter, ChangedItems, Warnings);

		const TSharedPtr<FJsonObject> DataObject = MakeShared<FJsonObject>();
		DataObject->SetStringField(TEXT("widget_path"), GetPackagePathFromAnyWidgetPath(CanonicalWidgetPath));
		DataObject->SetStringField(TEXT("object_path"), CanonicalWidgetPath);
		DataObject->SetStringField(TEXT("spec_path"), ResolvedSpecPath);
		DataObject->SetBoolField(TEXT("compile_after"), bCompileAfter);
		DataObject->SetBoolField(TEXT("save_after"), bSaveAfter);
		DataObject->SetBoolField(TEXT("open_after"), bOpenAfter);
		DataObject->SetBoolField(TEXT("remove_missing_widgets"), bRemoveMissingWidgets);
		DataObject->SetNumberField(TEXT("component_count"), UWidgetService::ListComponents(CanonicalWidgetPath).Num());

		return SerializeJson(BuildSpecSuccess(
			TEXT("apply_widget_spec"),
			FString::Printf(TEXT("Applied widget spec to %s."), *CanonicalWidgetPath),
			DataObject,
			ChangedItems,
			{
				TEXT("Open the Widget Blueprint designer and verify the tree, panel slot layout, and grouped properties."),
				TEXT("Preview the widget or run PIE to verify MVVM bindings, recorded event/delegate requests, and runtime behavior.")
			},
			Warnings,
			{ CanonicalWidgetPath }
		));
	}

	static FString HandleExportWidgetReview(const TMap<FString, FString>& Params)
	{
		TSharedPtr<FJsonObject> SpecObject;
		FString ResolvedSpecPath;
		TSharedPtr<FJsonObject> LoadError;
		FString ReviewSourceWidgetPath;

		if (!ExtractParam(Params, TEXT("spec_path")).IsEmpty() || !ExtractParam(Params, TEXT("spec_content")).IsEmpty())
		{
			if (!LoadSpecObject(Params, TEXT("export_widget_review"), SpecObject, ResolvedSpecPath, LoadError))
			{
				return SerializeJson(LoadError);
			}
		}
		else
		{
			ReviewSourceWidgetPath = ExtractParam(Params, TEXT("widget_path"));
			if (ReviewSourceWidgetPath.IsEmpty())
			{
				return SerializeJson(BuildSpecError(
					TEXT("export_widget_review"),
					TEXT("MISSING_INPUT"),
					TEXT("Provide widget_path, spec_path, or spec_content.")
				));
			}

			UWidgetBlueprint* WidgetBlueprint = nullptr;
			FString CanonicalWidgetPath;
			if (!LoadWidgetBlueprintAsset(ReviewSourceWidgetPath, WidgetBlueprint, CanonicalWidgetPath, LoadError, TEXT("export_widget_review")))
			{
				return SerializeJson(LoadError);
			}

			SpecObject = BuildWidgetSpecObject(WidgetBlueprint, CanonicalWidgetPath, ExtractBoolParam(Params, TEXT("include_all_properties"), false));
		}

		const TSharedPtr<FJsonObject>* AssetObject = nullptr;
		SpecObject->TryGetObjectField(TEXT("asset"), AssetObject);
		const FString ReviewWidgetPath = AssetObject && (*AssetObject).IsValid()
			? GetJsonStringField(*AssetObject, TEXT("widget_path"))
			: TEXT("");

		const FString ReviewPath = ResolveOutputPath(
			ExtractParam(Params, TEXT("review_path")),
			!ResolvedSpecPath.IsEmpty()
				? BuildDefaultReviewPathFromSpecPath(ResolvedSpecPath)
				: BuildDefaultReviewPathFromWidgetPath(ReviewWidgetPath));

		const FString Markdown = BuildWidgetReviewMarkdown(SpecObject, ResolvedSpecPath);
		if (!SaveTextFile(ReviewPath, Markdown))
		{
			return SerializeJson(BuildSpecError(
				TEXT("export_widget_review"),
				TEXT("REVIEW_WRITE_FAILED"),
				FString::Printf(TEXT("Failed to write widget review file '%s'."), *ReviewPath)
			));
		}

		const TSharedPtr<FJsonObject> DataObject = MakeShared<FJsonObject>();
		DataObject->SetStringField(TEXT("review_path"), ReviewPath);
		if (!ResolvedSpecPath.IsEmpty())
		{
			DataObject->SetStringField(TEXT("spec_path"), ResolvedSpecPath);
		}
		DataObject->SetStringField(TEXT("widget_path"), ReviewWidgetPath);

		return SerializeJson(BuildSpecSuccess(
			TEXT("export_widget_review"),
			FString::Printf(TEXT("Exported widget review to %s."), *ReviewPath),
			DataObject,
			{ FString::Printf(TEXT("wrote widget review file %s"), *ReviewPath) },
			{
				TEXT("Use the review file to confirm tree, slot layout, and grouped properties before any live widget_blueprint edits."),
				TEXT("If you edit the canonical .widget.json, rerun export_widget_review to regenerate the human-readable review.")
			}
		));
	}
}

REGISTER_VIBEUE_TOOL(export_widget_spec,
	"Export a canonical widget spec file plus a human-readable review document for an existing Widget Blueprint. The canonical file is a .widget.json spec; the review file is markdown organized by tree, grouped props, and slot layout.",
	"Widget",
	TOOL_PARAMS(
		TOOL_PARAM("widget_path", "Widget Blueprint asset path (package or object path).", "string", true),
		TOOL_PARAM("spec_path", "Optional output path for the canonical .widget.json file. Relative paths are resolved from the project root.", "string", false),
		TOOL_PARAM("review_path", "Optional output path for the human-readable review markdown.", "string", false),
		TOOL_PARAM("include_all_properties", "Whether to include properties that still match class defaults. Default false exports only explicit deviations.", "boolean", false),
		TOOL_PARAM("write_review", "Whether to also emit a .widget.review.md file. Default true.", "boolean", false)
	),
	{
		return HandleExportWidgetSpec(Params);
	}
)

REGISTER_VIBEUE_TOOL(export_widget_spec_batch,
	"Batch-export canonical widget specs and review markdown files for every Widget Blueprint under a package path. Writes a batch manifest so large UI libraries can be inventoried before inheritance/composition work.",
	"Widget",
	TOOL_PARAMS(
		TOOL_PARAM("path_filter", "Package path filter such as /Game/EasyGameUI.", "string", true),
		TOOL_PARAM("spec_output_dir", "Optional output directory for .widget.json files. Defaults to Saved/VibeUE/WidgetSpecs.", "string", false),
		TOOL_PARAM("review_output_dir", "Optional output directory for .widget.review.md files. Defaults to Saved/VibeUE/WidgetSpecs.", "string", false),
		TOOL_PARAM("manifest_path", "Optional output path for the batch manifest JSON file.", "string", false),
		TOOL_PARAM("include_all_properties", "Whether to include properties that still match class defaults. Default false exports only explicit deviations.", "boolean", false),
		TOOL_PARAM("write_review", "Whether to emit .widget.review.md files alongside the specs. Default true.", "boolean", false),
		TOOL_PARAM("max_results", "Optional cap on the number of Widget Blueprints exported. Use 0 for no practical cap.", "number", false)
	),
	{
		return HandleExportWidgetSpecBatch(Params);
	}
)

REGISTER_VIBEUE_TOOL(validate_widget_spec,
	"Validate a canonical .widget.json spec before apply. Checks asset metadata, tree integrity, grouped widget entries, supported panel slot layout data, MVVM references, and event/delegate request shape.",
	"Widget",
	TOOL_PARAMS(
		TOOL_PARAM("spec_path", "Path to the canonical .widget.json file.", "string", false),
		TOOL_PARAM("spec_content", "Inline widget spec JSON content. Use this when generating/editing the spec outside the filesystem.", "string", false)
	),
	{
		return HandleValidateWidgetSpec(Params);
	}
)

REGISTER_VIBEUE_TOOL(export_widget_review,
	"Export a human-readable markdown review for a widget spec or a live Widget Blueprint. The review groups widget properties, highlights supported slot layout, and prints MVVM/event/delegate requests as a YAML-style block for inspection.",
	"Widget",
	TOOL_PARAMS(
		TOOL_PARAM("widget_path", "Optional live Widget Blueprint path. If omitted, provide spec_path or spec_content.", "string", false),
		TOOL_PARAM("spec_path", "Optional canonical .widget.json path to render into review markdown.", "string", false),
		TOOL_PARAM("spec_content", "Optional inline canonical widget spec JSON.", "string", false),
		TOOL_PARAM("review_path", "Optional output path for the markdown review file. Relative paths are resolved from the project root.", "string", false),
		TOOL_PARAM("include_all_properties", "When exporting directly from a live widget, include properties that still match class defaults.", "boolean", false)
	),
	{
		return HandleExportWidgetReview(Params);
	}
)
