// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Core/ToolRegistry.h"
#include "Reflection/VibeReflectionUtils.h"
#include "Tools/ToolEnvelopeUtils.h"

#include "Core/ErrorCodes.h"
#include "Core/JsonValueHelper.h"

#include "AssetToolsModule.h"
#include "Factories/DataTableFactory.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/DataTable.h"
#include "EngineUtils.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "Json.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "DataTableEditorUtils.h"

namespace
{
	struct FLoadedMapContext
	{
		UWorld* World = nullptr;
		FString TargetMapPath;
		FString PreviousMapPath;
		bool bMapSwitched = false;
	};

	static bool ShouldExposeMapReadableProperty(FProperty* Property);

	static FString ExtractParamFromJson(const TMap<FString, FString>& Params, const FString& FieldName)
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

	static bool ExtractBoolParam(const TMap<FString, FString>& Params, const FString& FieldName, bool bDefaultValue)
	{
		const FString Value = ExtractParamFromJson(Params, FieldName);
		if (Value.IsEmpty())
		{
			return bDefaultValue;
		}

		return Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("1"));
	}

	static int32 ExtractIntParam(const TMap<FString, FString>& Params, const FString& FieldName, int32 DefaultValue, int32 MinValue, int32 MaxValue)
	{
		const FString Value = ExtractParamFromJson(Params, FieldName);
		if (Value.IsEmpty())
		{
			return FMath::Clamp(DefaultValue, MinValue, MaxValue);
		}
		return FMath::Clamp(FCString::Atoi(*Value), MinValue, MaxValue);
	}

	static FString SerializeJsonObject(const TSharedPtr<FJsonObject>& JsonObject)
	{
		FString JsonString;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
		FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
		return JsonString;
	}

	static TSharedPtr<FJsonObject> BuildErrorResponse(const FString& ErrorCode, const FString& ErrorMessage)
	{
		const TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
		ErrorObj->SetBoolField(TEXT("success"), false);
		ErrorObj->SetStringField(TEXT("error_code"), ErrorCode);
		ErrorObj->SetStringField(TEXT("error_message"), ErrorMessage);
		return ErrorObj;
	}

	static bool ParseJsonObjectParam(const TMap<FString, FString>& Params, const FString& FieldName, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
	{
		const FString JsonText = ExtractParamFromJson(Params, FieldName);
		if (JsonText.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Parameter '%s' is required."), *FieldName);
			return false;
		}

		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
		{
			OutError = FString::Printf(TEXT("Parameter '%s' must be a valid JSON object."), *FieldName);
			return false;
		}

		return true;
	}

	static bool ParseJsonArrayParamOptional(const TMap<FString, FString>& Params, const FString& FieldName, TArray<TSharedPtr<FJsonValue>>& OutArray, FString& OutError)
	{
		const FString JsonText = ExtractParamFromJson(Params, FieldName);
		if (JsonText.IsEmpty())
		{
			OutArray.Reset();
			return true;
		}

		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutArray))
		{
			OutError = FString::Printf(TEXT("Parameter '%s' must be a valid JSON array."), *FieldName);
			return false;
		}

		return true;
	}

	static FString GetCurrentEditorMapPath()
	{
		if (!GEditor)
		{
			return FString();
		}

		if (UWorld* CurrentWorld = GEditor->GetEditorWorldContext().World())
		{
			if (UPackage* Package = CurrentWorld->GetOutermost())
			{
				return Package->GetName();
			}
		}

		return FString();
	}

	static bool IsPieSessionActive()
	{
		return GEditor && (GEditor->PlayWorld != nullptr || GIsPlayInEditorWorld);
	}

	static bool EnsureMapLoadedForEditing(const FString& MapPath, FLoadedMapContext& OutContext, FString& OutError)
	{
		OutContext = FLoadedMapContext();
		OutContext.TargetMapPath = MapPath;

		if (!GEditor)
		{
			OutError = TEXT("Editor is not available.");
			return false;
		}

		if (MapPath.IsEmpty())
		{
			OutError = TEXT("Map path cannot be empty.");
			return false;
		}

		if (IsPieSessionActive())
		{
			OutError = TEXT("Map loading/editing tools cannot run while PIE is active or tearing down. Stop PIE first, then retry the map operation.");
			return false;
		}

		UWorld* CurrentWorld = GEditor->GetEditorWorldContext().World();
		const FString CurrentMapPath = GetCurrentEditorMapPath();

		if (CurrentWorld && CurrentMapPath.Equals(MapPath, ESearchCase::IgnoreCase))
		{
			OutContext.World = CurrentWorld;
			return true;
		}

		if (CurrentWorld)
		{
			if (UPackage* CurrentPackage = CurrentWorld->GetOutermost())
			{
				if (CurrentPackage->IsDirty())
				{
					OutError = FString::Printf(
						TEXT("Current editor map '%s' has unsaved changes. Save or discard it before editing another map asset."),
						*CurrentPackage->GetName());
					return false;
				}
			}
			OutContext.PreviousMapPath = CurrentMapPath;
		}

		UWorld* LoadedWorld = UEditorLoadingAndSavingUtils::LoadMap(MapPath);
		if (!LoadedWorld)
		{
			OutError = FString::Printf(TEXT("Failed to load map '%s'."), *MapPath);
			return false;
		}

		OutContext.World = LoadedWorld;
		OutContext.bMapSwitched = true;
		return true;
	}

	static bool SaveEditedMap(const FLoadedMapContext& Context, FString& OutError)
	{
		if (!Context.World)
		{
			OutError = TEXT("No world is loaded for saving.");
			return false;
		}

		const FString MapPathToSave = Context.TargetMapPath.IsEmpty()
			? Context.World->GetOutermost()->GetName()
			: Context.TargetMapPath;

		if (!UEditorLoadingAndSavingUtils::SaveMap(Context.World, MapPathToSave))
		{
			OutError = FString::Printf(TEXT("Failed to save map '%s'."), *MapPathToSave);
			return false;
		}

		return true;
	}

	static bool RestorePreviousMap(const FLoadedMapContext& Context, FString& OutError)
	{
		if (!Context.bMapSwitched || Context.PreviousMapPath.IsEmpty())
		{
			return true;
		}

		if (!UEditorLoadingAndSavingUtils::LoadMap(Context.PreviousMapPath))
		{
			OutError = FString::Printf(TEXT("Failed to restore previous map '%s'."), *Context.PreviousMapPath);
			return false;
		}

		return true;
	}

	static bool ParseTransformParam(const TMap<FString, FString>& Params, const FString& FieldName, FTransform& OutTransform, FString& OutError)
	{
		OutTransform = FTransform::Identity;

		const FString JsonText = ExtractParamFromJson(Params, FieldName);
		if (JsonText.IsEmpty())
		{
			return true;
		}

		TSharedPtr<FJsonObject> JsonObj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
		{
			OutError = FString::Printf(TEXT("Parameter '%s' must be a valid JSON object."), *FieldName);
			return false;
		}

		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		FVector Scale = FVector::OneVector;

		FJsonValueHelper::TryGetVectorField(JsonObj, TEXT("location"), Location);
		FJsonValueHelper::TryGetRotator(JsonObj->TryGetField(TEXT("rotation")), Rotation);
		FJsonValueHelper::TryGetVectorField(JsonObj, TEXT("scale"), Scale);

		OutTransform = FTransform(Rotation, Location, Scale);
		return true;
	}

	static FString NormalizeBlueprintClassPath(const FString& RawClassPath)
	{
		if (RawClassPath.IsEmpty())
		{
			return FString();
		}

		if (RawClassPath.Contains(TEXT(".")))
		{
			return RawClassPath;
		}

		if (RawClassPath.StartsWith(TEXT("/Game/")))
		{
			const FString ShortName = FPackageName::GetShortName(RawClassPath);
			return RawClassPath + TEXT(".") + ShortName + TEXT("_C");
		}

		return RawClassPath;
	}

	static bool ResolveActorClass(const FString& ActorClassParam, UClass*& OutClass, FString& OutError)
	{
		FString ClassSpecifier = ActorClassParam.TrimStartAndEnd();
		if (ClassSpecifier.IsEmpty())
		{
			OutError = TEXT("Actor class cannot be empty.");
			return false;
		}

		if (!ClassSpecifier.Contains(TEXT(":")))
		{
			ClassSpecifier = TEXT("class:") + NormalizeBlueprintClassPath(ClassSpecifier);
		}

		FResolvedObjectTarget ResolvedTarget;
		if (!FVibeReflectionUtils::ResolveTarget(ClassSpecifier, ResolvedTarget, OutError))
		{
			return false;
		}

		OutClass = Cast<UClass>(ResolvedTarget.TargetObject);
		if (!OutClass || !OutClass->IsChildOf(AActor::StaticClass()))
		{
			OutError = FString::Printf(TEXT("Resolved class '%s' is not an Actor class."), ResolvedTarget.ResolvedObjectPath.IsEmpty() ? *ClassSpecifier : *ResolvedTarget.ResolvedObjectPath);
			return false;
		}

		return true;
	}

	static TSharedPtr<FJsonObject> BuildActorJson(AActor* Actor, const FString& MapPath)
	{
		const TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
		ActorObj->SetStringField(TEXT("map_path"), MapPath);
		ActorObj->SetStringField(TEXT("actor_name"), Actor ? Actor->GetName() : FString());
		ActorObj->SetStringField(TEXT("actor_label"), Actor ? Actor->GetActorLabel() : FString());
		ActorObj->SetStringField(TEXT("actor_class"), Actor ? Actor->GetClass()->GetPathName() : FString());
		ActorObj->SetStringField(TEXT("actor_path"), Actor ? Actor->GetPathName() : FString());
		ActorObj->SetStringField(TEXT("folder_path"), Actor ? Actor->GetFolderPath().ToString() : FString());

		if (Actor)
		{
			const FVector Location = Actor->GetActorLocation();
			const FRotator Rotation = Actor->GetActorRotation();
			const FVector Scale = Actor->GetActorScale3D();

			const TSharedPtr<FJsonObject> LocationObj = MakeShared<FJsonObject>();
			LocationObj->SetNumberField(TEXT("x"), Location.X);
			LocationObj->SetNumberField(TEXT("y"), Location.Y);
			LocationObj->SetNumberField(TEXT("z"), Location.Z);
			ActorObj->SetObjectField(TEXT("location"), LocationObj);

			const TSharedPtr<FJsonObject> RotationObj = MakeShared<FJsonObject>();
			RotationObj->SetNumberField(TEXT("pitch"), Rotation.Pitch);
			RotationObj->SetNumberField(TEXT("yaw"), Rotation.Yaw);
			RotationObj->SetNumberField(TEXT("roll"), Rotation.Roll);
			ActorObj->SetObjectField(TEXT("rotation"), RotationObj);

			const TSharedPtr<FJsonObject> ScaleObj = MakeShared<FJsonObject>();
			ScaleObj->SetNumberField(TEXT("x"), Scale.X);
			ScaleObj->SetNumberField(TEXT("y"), Scale.Y);
			ScaleObj->SetNumberField(TEXT("z"), Scale.Z);
			ActorObj->SetObjectField(TEXT("scale"), ScaleObj);
		}

		return ActorObj;
	}

	static AActor* FindActorInWorld(UWorld* World, const FString& ActorIdentifier)
	{
		if (!World || ActorIdentifier.IsEmpty())
		{
			return nullptr;
		}

		const FString Search = ActorIdentifier.TrimStartAndEnd();
		const FString LowerSearch = Search.ToLower();

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor)
			{
				continue;
			}

			if (Actor->GetPathName().Equals(Search, ESearchCase::CaseSensitive) ||
				Actor->GetActorLabel().Equals(Search, ESearchCase::IgnoreCase) ||
				Actor->GetName().Equals(Search, ESearchCase::IgnoreCase))
			{
				return Actor;
			}
		}

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor)
			{
				continue;
			}

			if (Actor->GetPathName().ToLower().Contains(LowerSearch) ||
				Actor->GetActorLabel().ToLower().Contains(LowerSearch) ||
				Actor->GetName().ToLower().Contains(LowerSearch))
			{
				return Actor;
			}
		}

		return nullptr;
	}

	static TSharedPtr<FJsonObject> BuildDataTableSchemaJson(const UScriptStruct* RowStruct, bool bIncludeInherited)
	{
		const TSharedPtr<FJsonObject> SchemaObj = MakeShared<FJsonObject>();
		SchemaObj->SetStringField(TEXT("row_struct_name"), RowStruct ? RowStruct->GetName() : FString());
		SchemaObj->SetStringField(TEXT("row_struct_path"), RowStruct ? RowStruct->GetPathName() : FString());

		TArray<TSharedPtr<FJsonValue>> ColumnsArray;
		if (RowStruct)
		{
			const EFieldIterationFlags IterationFlags = bIncludeInherited ? EFieldIterationFlags::IncludeSuper : EFieldIterationFlags::None;
			for (TFieldIterator<FProperty> It(RowStruct, IterationFlags); It; ++It)
			{
				FProperty* Property = *It;
				if (!ShouldExposeMapReadableProperty(Property))
				{
					continue;
				}

				const TSharedPtr<FJsonObject> ColumnObj = MakeShared<FJsonObject>();
				ColumnObj->SetStringField(TEXT("name"), Property->GetName());
				ColumnObj->SetStringField(TEXT("type"), FVibeReflectionUtils::GetPropertyTypeString(Property));
				ColumnObj->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
				ColumnObj->SetBoolField(TEXT("editable"), !Property->HasAnyPropertyFlags(CPF_EditConst | CPF_BlueprintReadOnly));

				if (Property->HasMetaData(TEXT("Category")))
				{
					ColumnObj->SetStringField(TEXT("category"), Property->GetMetaData(TEXT("Category")));
				}
				if (Property->HasMetaData(TEXT("Tooltip")))
				{
					ColumnObj->SetStringField(TEXT("tooltip"), Property->GetMetaData(TEXT("Tooltip")));
				}

				ColumnsArray.Add(MakeShared<FJsonValueObject>(ColumnObj));
			}
		}

		SchemaObj->SetArrayField(TEXT("columns"), ColumnsArray);
		return SchemaObj;
	}

	static UScriptStruct* ResolveRowStructObject(const FString& RowStructObjectPath, FString& OutError)
	{
		if (RowStructObjectPath.IsEmpty())
		{
			OutError = TEXT("Row struct object path cannot be empty.");
			return nullptr;
		}

		UObject* LoadedObject = UEditorAssetLibrary::LoadAsset(RowStructObjectPath);
		UScriptStruct* RowStruct = Cast<UScriptStruct>(LoadedObject);
		if (!RowStruct)
		{
			RowStruct = LoadObject<UScriptStruct>(nullptr, *RowStructObjectPath);
		}

		if (!RowStruct)
		{
			OutError = FString::Printf(TEXT("Failed to load row struct object '%s'."), *RowStructObjectPath);
			return nullptr;
		}

		if (!FDataTableEditorUtils::IsValidTableStruct(RowStruct))
		{
			OutError = FString::Printf(TEXT("Struct '%s' is not a valid DataTable row struct."), *RowStruct->GetPathName());
			return nullptr;
		}

		return RowStruct;
	}

	static bool ShouldExposeMapReadableProperty(FProperty* Property)
	{
		if (!Property)
		{
			return false;
		}

		if (Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient))
		{
			return false;
		}

		return true;
	}

	static void BuildWorldSettingsValuesObject(
		AWorldSettings* WorldSettings,
		TSharedPtr<FJsonObject>& OutValuesObj,
		const TArray<TSharedPtr<FJsonValue>>& PathsArray,
		bool bIncludeInherited,
		int32 MaxDepth,
		int32 MaxProperties,
		TArray<TSharedPtr<FJsonValue>>& OutErrors)
	{
		OutValuesObj = MakeShared<FJsonObject>();

		if (!WorldSettings)
		{
			return;
		}

		if (PathsArray.Num() > 0)
		{
			for (const TSharedPtr<FJsonValue>& PathValue : PathsArray)
			{
				FString Path;
				if (!PathValue.IsValid() || !PathValue->TryGetString(Path))
				{
					continue;
				}

				FResolvedPropertyPath ResolvedPath;
				FString ResolveError;
				if (!FVibeReflectionUtils::ResolvePropertyPath(WorldSettings, Path, ResolvedPath, ResolveError))
				{
					const TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
					ErrorObj->SetStringField(TEXT("path"), Path);
					ErrorObj->SetStringField(TEXT("error"), ResolveError);
					OutErrors.Add(MakeShared<FJsonValueObject>(ErrorObj));
					continue;
				}

				OutValuesObj->SetObjectField(ResolvedPath.CanonicalPath, FVibeReflectionUtils::BuildTypedValueJson(ResolvedPath.Property, ResolvedPath.ValuePtr, MaxDepth));
			}
			return;
		}

		const EFieldIterationFlags IterationFlags = bIncludeInherited ? EFieldIterationFlags::IncludeSuper : EFieldIterationFlags::None;
		int32 Count = 0;
		for (TFieldIterator<FProperty> It(WorldSettings->GetClass(), IterationFlags); It; ++It)
		{
			if (MaxProperties > 0 && Count >= MaxProperties)
			{
				break;
			}

			FProperty* Property = *It;
			if (!ShouldExposeMapReadableProperty(Property))
			{
				continue;
			}

			const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(WorldSettings);
			OutValuesObj->SetObjectField(Property->GetName(), FVibeReflectionUtils::BuildTypedValueJson(Property, ValuePtr, MaxDepth));
			++Count;
		}
	}

	static bool PassesActorFilter(AActor* Actor, const FString& ClassFilter, const FString& LabelFilter, bool bIncludeHidden)
	{
		if (!Actor)
		{
			return false;
		}

		if (!bIncludeHidden && Actor->IsHidden())
		{
			return false;
		}

		if (!LabelFilter.IsEmpty())
		{
			const FString LowerLabelFilter = LabelFilter.ToLower();
			if (!Actor->GetActorLabel().ToLower().Contains(LowerLabelFilter) &&
				!Actor->GetName().ToLower().Contains(LowerLabelFilter))
			{
				return false;
			}
		}

		if (!ClassFilter.IsEmpty())
		{
			UClass* FilterClass = nullptr;
			FString ClassResolveError;
			const bool bResolvedClass = ResolveActorClass(ClassFilter, FilterClass, ClassResolveError);
			if (bResolvedClass)
			{
				return Actor->GetClass()->IsChildOf(FilterClass);
			}

			const FString LowerClassFilter = ClassFilter.ToLower();
			if (!Actor->GetClass()->GetName().ToLower().Contains(LowerClassFilter) &&
				!Actor->GetClass()->GetPathName().ToLower().Contains(LowerClassFilter))
			{
				return false;
			}
		}

		return true;
	}

	static FString HandleGetMapWorldSettings(const TMap<FString, FString>& Params)
	{
		const FString MapPath = ExtractParamFromJson(Params, TEXT("map_path"));
		const bool bRestorePreviousMap = ExtractBoolParam(Params, TEXT("restore_previous_map"), false);
		const bool bIncludeSchema = ExtractBoolParam(Params, TEXT("include_schema"), false);
		const bool bIncludeInherited = ExtractBoolParam(Params, TEXT("include_inherited"), true);
		const int32 MaxDepth = ExtractIntParam(Params, TEXT("max_depth"), 3, 1, 8);
		const int32 MaxProperties = ExtractIntParam(Params, TEXT("max_properties"), 128, 0, 4096);

		TArray<TSharedPtr<FJsonValue>> PathsArray;
		FString Error;
		if (!ParseJsonArrayParamOptional(Params, TEXT("paths_json"), PathsArray, Error))
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PARAM_INVALID, Error));
		}

		FLoadedMapContext MapContext;
		if (!EnsureMapLoadedForEditing(MapPath, MapContext, Error))
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PARAM_INVALID, Error));
		}

		AWorldSettings* WorldSettings = MapContext.World ? MapContext.World->GetWorldSettings() : nullptr;
		if (!WorldSettings)
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PROPERTY_NOT_FOUND, TEXT("WorldSettings is not available for the loaded map.")));
		}

		TArray<TSharedPtr<FJsonValue>> ErrorsArray;
		TSharedPtr<FJsonObject> ValuesObj;
		BuildWorldSettingsValuesObject(WorldSettings, ValuesObj, PathsArray, bIncludeInherited, MaxDepth, MaxProperties, ErrorsArray);

		bool bRestoreSuccess = true;
		FString RestoreError;
		if (bRestorePreviousMap)
		{
			bRestoreSuccess = RestorePreviousMap(MapContext, RestoreError);
		}

		const TSharedPtr<FJsonObject> TargetObj = MakeShared<FJsonObject>();
		TargetObj->SetStringField(TEXT("world_settings_path"), WorldSettings->GetPathName());
		TargetObj->SetStringField(TEXT("world_settings_class"), WorldSettings->GetClass()->GetPathName());

		const TSharedPtr<FJsonObject> ResponseObj = MakeShared<FJsonObject>();
		ResponseObj->SetBoolField(TEXT("success"), ErrorsArray.Num() == 0);
		ResponseObj->SetStringField(TEXT("map_path"), MapContext.TargetMapPath);
		ResponseObj->SetObjectField(TEXT("target"), TargetObj);
		ResponseObj->SetObjectField(TEXT("values"), ValuesObj.ToSharedRef());
		ResponseObj->SetArrayField(TEXT("errors"), ErrorsArray);
		ResponseObj->SetBoolField(TEXT("restore_previous_map"), bRestorePreviousMap);
		ResponseObj->SetBoolField(TEXT("restore_success"), bRestoreSuccess);
		if (bIncludeSchema)
		{
			ResponseObj->SetObjectField(TEXT("schema"), FVibeReflectionUtils::BuildObjectSchemaJson(WorldSettings, bIncludeInherited, MaxDepth));
		}
		if (!RestoreError.IsEmpty())
		{
			ResponseObj->SetStringField(TEXT("restore_error"), RestoreError);
		}
		return SerializeJsonObject(ResponseObj);
	}

	static FString HandleListMapActors(const TMap<FString, FString>& Params)
	{
		const FString MapPath = ExtractParamFromJson(Params, TEXT("map_path"));
		const FString ClassFilter = ExtractParamFromJson(Params, TEXT("class_filter"));
		const FString LabelFilter = ExtractParamFromJson(Params, TEXT("label_filter"));
		const bool bIncludeHidden = ExtractBoolParam(Params, TEXT("include_hidden"), false);
		const bool bRestorePreviousMap = ExtractBoolParam(Params, TEXT("restore_previous_map"), false);
		const int32 MaxResults = ExtractIntParam(Params, TEXT("max_results"), 200, 0, 10000);

		FLoadedMapContext MapContext;
		FString Error;
		if (!EnsureMapLoadedForEditing(MapPath, MapContext, Error))
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PARAM_INVALID, Error));
		}

		TArray<TSharedPtr<FJsonValue>> ActorsArray;
		int32 TotalMatched = 0;
		for (TActorIterator<AActor> It(MapContext.World); It; ++It)
		{
			AActor* Actor = *It;
			if (!PassesActorFilter(Actor, ClassFilter, LabelFilter, bIncludeHidden))
			{
				continue;
			}

			++TotalMatched;
			if (MaxResults == 0 || ActorsArray.Num() < MaxResults)
			{
				ActorsArray.Add(MakeShared<FJsonValueObject>(BuildActorJson(Actor, MapContext.TargetMapPath)));
			}
		}

		bool bRestoreSuccess = true;
		FString RestoreError;
		if (bRestorePreviousMap)
		{
			bRestoreSuccess = RestorePreviousMap(MapContext, RestoreError);
		}

		const TSharedPtr<FJsonObject> ResponseObj = MakeShared<FJsonObject>();
		ResponseObj->SetBoolField(TEXT("success"), true);
		ResponseObj->SetStringField(TEXT("map_path"), MapContext.TargetMapPath);
		ResponseObj->SetStringField(TEXT("class_filter"), ClassFilter);
		ResponseObj->SetStringField(TEXT("label_filter"), LabelFilter);
		ResponseObj->SetBoolField(TEXT("include_hidden"), bIncludeHidden);
		ResponseObj->SetNumberField(TEXT("matched_count"), TotalMatched);
		ResponseObj->SetNumberField(TEXT("returned_count"), ActorsArray.Num());
		ResponseObj->SetArrayField(TEXT("actors"), ActorsArray);
		ResponseObj->SetBoolField(TEXT("restore_previous_map"), bRestorePreviousMap);
		ResponseObj->SetBoolField(TEXT("restore_success"), bRestoreSuccess);
		if (!RestoreError.IsEmpty())
		{
			ResponseObj->SetStringField(TEXT("restore_error"), RestoreError);
		}
		return SerializeJsonObject(ResponseObj);
	}

	static FString HandleGetDataTableRows(const TMap<FString, FString>& Params)
	{
		const FString TablePath = ExtractParamFromJson(Params, TEXT("table_path"));
		const int32 MaxDepth = ExtractIntParam(Params, TEXT("max_depth"), 4, 1, 8);
		const int32 MaxRows = ExtractIntParam(Params, TEXT("max_rows"), 200, 0, 100000);

		TArray<TSharedPtr<FJsonValue>> RowNamesArray;
		FString Error;
		if (!ParseJsonArrayParamOptional(Params, TEXT("row_names_json"), RowNamesArray, Error))
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PARAM_INVALID, Error));
		}

		UDataTable* DataTable = Cast<UDataTable>(UEditorAssetLibrary::LoadAsset(TablePath));
		if (!DataTable)
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PROPERTY_NOT_FOUND, FString::Printf(TEXT("Failed to load DataTable '%s'."), *TablePath)));
		}

		const UScriptStruct* RowStruct = DataTable->GetRowStruct();
		if (!RowStruct)
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PROPERTY_NOT_FOUND, TEXT("DataTable has no row struct.")));
		}

		TArray<FName> RowNames;
		if (RowNamesArray.Num() > 0)
		{
			for (const TSharedPtr<FJsonValue>& RowNameValue : RowNamesArray)
			{
				FString RowNameString;
				if (RowNameValue.IsValid() && FJsonValueHelper::TryGetString(RowNameValue, RowNameString) && !RowNameString.IsEmpty())
				{
					RowNames.Add(FName(*RowNameString));
				}
			}
		}
		else
		{
			RowNames = DataTable->GetRowNames();
		}

		TSharedPtr<FJsonObject> RowsObj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ErrorsArray;
		int32 ReturnedCount = 0;

		for (const FName& RowName : RowNames)
		{
			if (MaxRows > 0 && ReturnedCount >= MaxRows)
			{
				break;
			}

			const uint8* RowData = DataTable->FindRowUnchecked(RowName);
			if (!RowData)
			{
				const TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
				ErrorObj->SetStringField(TEXT("row_name"), RowName.ToString());
				ErrorObj->SetStringField(TEXT("error"), TEXT("Row not found."));
				ErrorsArray.Add(MakeShared<FJsonValueObject>(ErrorObj));
				continue;
			}

			RowsObj->SetObjectField(RowName.ToString(), FVibeReflectionUtils::BuildStructValueJson(RowStruct, RowData, MaxDepth));
			++ReturnedCount;
		}

		const TSharedPtr<FJsonObject> ResponseObj = MakeShared<FJsonObject>();
		ResponseObj->SetBoolField(TEXT("success"), ErrorsArray.Num() == 0);
		ResponseObj->SetStringField(TEXT("table_path"), TablePath);
		ResponseObj->SetStringField(TEXT("row_struct_path"), RowStruct->GetPathName());
		ResponseObj->SetNumberField(TEXT("requested_count"), RowNames.Num());
		ResponseObj->SetNumberField(TEXT("returned_count"), ReturnedCount);
		ResponseObj->SetObjectField(TEXT("rows"), RowsObj.ToSharedRef());
		ResponseObj->SetArrayField(TEXT("errors"), ErrorsArray);
		return SerializeJsonObject(ResponseObj);
	}

	static FString HandleCreateDataTableWithRowStructObject(const TMap<FString, FString>& Params)
	{
		const FString RowStructObjectPath = ExtractParamFromJson(Params, TEXT("row_struct_object_path"));
		const FString AssetName = ExtractParamFromJson(Params, TEXT("asset_name"));
		FString AssetPath = ExtractParamFromJson(Params, TEXT("asset_path"));
		const bool bSave = ExtractBoolParam(Params, TEXT("save"), true);

		if (AssetName.IsEmpty())
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PARAM_INVALID, TEXT("Asset name cannot be empty.")));
		}

		FString Error;
		UScriptStruct* RowStruct = ResolveRowStructObject(RowStructObjectPath, Error);
		if (!RowStruct)
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PARAM_INVALID, Error));
		}

		if (AssetPath.IsEmpty())
		{
			AssetPath = TEXT("/Game/Data");
		}
		if (!AssetPath.StartsWith(TEXT("/Game")) && !AssetPath.StartsWith(TEXT("/Engine")))
		{
			AssetPath = AssetPath.StartsWith(TEXT("/")) ? TEXT("/Game") + AssetPath : TEXT("/Game/") + AssetPath;
		}

		const FString FullAssetPath = AssetPath / AssetName;
		if (UEditorAssetLibrary::DoesAssetExist(FullAssetPath))
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::BLUEPRINT_ALREADY_EXISTS, FString::Printf(TEXT("A DataTable already exists at '%s'."), *FullAssetPath)));
		}

		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		UDataTableFactory* Factory = NewObject<UDataTableFactory>();
		Factory->Struct = RowStruct;

		UObject* NewAsset = AssetTools.CreateAsset(AssetName, AssetPath, UDataTable::StaticClass(), Factory);
		UDataTable* DataTable = Cast<UDataTable>(NewAsset);
		if (!DataTable)
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::WIDGET_CREATE_FAILED, FString::Printf(TEXT("Failed to create DataTable '%s'."), *FullAssetPath)));
		}

		DataTable->MarkPackageDirty();
		bool bSaved = !bSave;
		if (bSave)
		{
			bSaved = UEditorAssetLibrary::SaveLoadedAsset(DataTable, false);
		}

		const TSharedPtr<FJsonObject> ResponseObj = MakeShared<FJsonObject>();
		ResponseObj->SetBoolField(TEXT("success"), bSaved);
		ResponseObj->SetStringField(TEXT("table_path"), DataTable->GetPathName());
		ResponseObj->SetStringField(TEXT("row_struct_path"), RowStruct->GetPathName());
		ResponseObj->SetBoolField(TEXT("saved"), bSaved);
		ResponseObj->SetObjectField(TEXT("schema"), BuildDataTableSchemaJson(RowStruct, true));
		return SerializeJsonObject(ResponseObj);
	}

	static FString HandleGetDataTableSchema(const TMap<FString, FString>& Params)
	{
		const FString TablePath = ExtractParamFromJson(Params, TEXT("table_path"));
		const bool bIncludeInherited = ExtractBoolParam(Params, TEXT("include_inherited"), true);

		UDataTable* DataTable = Cast<UDataTable>(UEditorAssetLibrary::LoadAsset(TablePath));
		if (!DataTable)
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PROPERTY_NOT_FOUND, FString::Printf(TEXT("Failed to load DataTable '%s'."), *TablePath)));
		}

		const UScriptStruct* RowStruct = DataTable->GetRowStruct();
		if (!RowStruct)
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PROPERTY_NOT_FOUND, TEXT("DataTable has no row struct.")));
		}

		const TSharedPtr<FJsonObject> ResponseObj = MakeShared<FJsonObject>();
		ResponseObj->SetBoolField(TEXT("success"), true);
		ResponseObj->SetStringField(TEXT("table_path"), TablePath);
		ResponseObj->SetObjectField(TEXT("schema"), BuildDataTableSchemaJson(RowStruct, bIncludeInherited));
		ResponseObj->SetNumberField(TEXT("row_count"), DataTable->GetRowNames().Num());
		return SerializeJsonObject(ResponseObj);
	}

	static FString HandleSetMapWorldSettings(const TMap<FString, FString>& Params)
	{
		const FString MapPath = ExtractParamFromJson(Params, TEXT("map_path"));
		const bool bSave = ExtractBoolParam(Params, TEXT("save"), true);
		const bool bRestorePreviousMap = ExtractBoolParam(Params, TEXT("restore_previous_map"), false);
		const int32 MaxDepth = ExtractIntParam(Params, TEXT("max_depth"), 8, 1, 16);

		TSharedPtr<FJsonObject> ValuesObj;
		FString Error;
		if (!ParseJsonObjectParam(Params, TEXT("values_json"), ValuesObj, Error))
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PARAM_INVALID, Error));
		}

		FLoadedMapContext MapContext;
		if (!EnsureMapLoadedForEditing(MapPath, MapContext, Error))
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PARAM_INVALID, Error));
		}

		AWorldSettings* WorldSettings = MapContext.World ? MapContext.World->GetWorldSettings() : nullptr;
		if (!WorldSettings)
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PROPERTY_NOT_FOUND, TEXT("WorldSettings is not available for the loaded map.")));
		}

		const FScopedTransaction Transaction(NSLOCTEXT("VibeUE", "SetMapWorldSettings", "Set Map World Settings"));
		WorldSettings->Modify();
		if (MapContext.World->PersistentLevel)
		{
			MapContext.World->PersistentLevel->Modify();
		}

		TArray<TSharedPtr<FJsonValue>> ResultsArray;
		TArray<TSharedPtr<FJsonValue>> ErrorsArray;
		int32 ModifiedCount = 0;
		int32 FailedCount = 0;
		const int32 RequestedCount = ValuesObj->Values.Num();

		for (const auto& Pair : ValuesObj->Values)
		{
			FResolvedPropertyPath ResolvedPath;
			FString ResolveError;
			const TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
			ResultObj->SetStringField(TEXT("requested_path"), Pair.Key);

			if (!FVibeReflectionUtils::ResolvePropertyPath(WorldSettings, Pair.Key, ResolvedPath, ResolveError))
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error"), ResolveError);
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				ErrorsArray.Add(MakeShared<FJsonValueObject>(FToolEnvelopeUtils::BuildErrorEntry(TEXT("requested_path"), Pair.Key, ResolveError, VibeUE::ErrorCodes::PROPERTY_NOT_FOUND)));
				++FailedCount;
				continue;
			}

			if (!FVibeReflectionUtils::SetPropertyValueFromJson(ResolvedPath.Property, ResolvedPath.ValuePtr, Pair.Value, ResolveError, MaxDepth))
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("path"), ResolvedPath.CanonicalPath);
				ResultObj->SetStringField(TEXT("error"), ResolveError);
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				ErrorsArray.Add(MakeShared<FJsonValueObject>(FToolEnvelopeUtils::BuildErrorEntry(TEXT("path"), ResolvedPath.CanonicalPath, ResolveError, VibeUE::ErrorCodes::PROPERTY_SET_FAILED)));
				++FailedCount;
				continue;
			}

			ResultObj->SetBoolField(TEXT("success"), true);
			ResultObj->SetStringField(TEXT("path"), ResolvedPath.CanonicalPath);
			ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
			++ModifiedCount;
		}

		MapContext.World->MarkPackageDirty();
		if (MapContext.World->PersistentLevel)
		{
			MapContext.World->PersistentLevel->MarkPackageDirty();
		}

		bool bSaved = !bSave;
		FString SaveError;
		if (bSave && ModifiedCount > 0)
		{
			bSaved = SaveEditedMap(MapContext, SaveError);
			if (!bSaved)
			{
				ErrorsArray.Add(MakeShared<FJsonValueObject>(FToolEnvelopeUtils::BuildErrorEntry(TEXT("map_path"), MapContext.TargetMapPath, SaveError, VibeUE::ErrorCodes::PROPERTY_SET_FAILED)));
				++FailedCount;
			}
		}

		bool bRestoreSuccess = true;
		FString RestoreError;
		if (bRestorePreviousMap)
		{
			bRestoreSuccess = RestorePreviousMap(MapContext, RestoreError);
			if (!bRestoreSuccess && !RestoreError.IsEmpty())
			{
				ErrorsArray.Add(MakeShared<FJsonValueObject>(FToolEnvelopeUtils::BuildErrorEntry(TEXT("map_path"), MapContext.TargetMapPath, RestoreError)));
			}
		}

		const TSharedPtr<FJsonObject> TargetObj = MakeShared<FJsonObject>();
		TargetObj->SetStringField(TEXT("map_path"), MapContext.TargetMapPath);
		TargetObj->SetStringField(TEXT("world_settings_path"), WorldSettings->GetPathName());

		FBatchToolEnvelope Envelope;
		Envelope.Operation = TEXT("set_map_world_settings");
		Envelope.ResourceKind = TEXT("world_settings");
		Envelope.Target = TargetObj;
		Envelope.RequestedCount = RequestedCount;
		Envelope.SuccessCount = ModifiedCount;
		Envelope.FailedCount = FailedCount;
		Envelope.Results = ResultsArray;
		Envelope.Errors = ErrorsArray;
		Envelope.bIncludeSaveFields = true;
		Envelope.bSaveRequested = bSave;
		Envelope.bSaved = bSaved;
		Envelope.SaveError = SaveError;
		Envelope.bIncludeRestoreFields = true;
		Envelope.bRestoreRequested = bRestorePreviousMap;
		Envelope.bRestoreSuccess = bRestoreSuccess;
		Envelope.RestoreError = RestoreError;

		const TSharedPtr<FJsonObject> ResponseObj = FToolEnvelopeUtils::BuildBatchEnvelope(Envelope);
		ResponseObj->SetStringField(TEXT("map_path"), MapContext.TargetMapPath);
		ResponseObj->SetNumberField(TEXT("modified_count"), ModifiedCount);
		return FToolEnvelopeUtils::SerializeJsonObject(ResponseObj);
	}

	static FString HandleSpawnActorInMap(const TMap<FString, FString>& Params)
	{
		const FString MapPath = ExtractParamFromJson(Params, TEXT("map_path"));
		const FString ActorClassParam = ExtractParamFromJson(Params, TEXT("actor_class"));
		const FString ActorLabel = ExtractParamFromJson(Params, TEXT("actor_label"));
		const FString FolderPath = ExtractParamFromJson(Params, TEXT("folder_path"));
		const bool bSave = ExtractBoolParam(Params, TEXT("save"), true);
		const bool bRestorePreviousMap = ExtractBoolParam(Params, TEXT("restore_previous_map"), false);
		const bool bSelectActor = ExtractBoolParam(Params, TEXT("select_actor"), false);

		FLoadedMapContext MapContext;
		FString Error;
		if (!EnsureMapLoadedForEditing(MapPath, MapContext, Error))
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PARAM_INVALID, Error));
		}

		UClass* ActorClass = nullptr;
		if (!ResolveActorClass(ActorClassParam, ActorClass, Error))
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PARAM_INVALID, Error));
		}

		FTransform SpawnTransform = FTransform::Identity;
		if (!ParseTransformParam(Params, TEXT("transform_json"), SpawnTransform, Error))
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PARAM_INVALID, Error));
		}

		const FScopedTransaction Transaction(NSLOCTEXT("VibeUE", "SpawnActorInMap", "Spawn Actor In Map"));
		MapContext.World->Modify();
		if (MapContext.World->PersistentLevel)
		{
			MapContext.World->PersistentLevel->Modify();
		}

		FActorSpawnParameters SpawnParams;
		SpawnParams.OverrideLevel = MapContext.World->PersistentLevel;
		SpawnParams.ObjectFlags |= RF_Transactional;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		AActor* NewActor = MapContext.World->SpawnActor<AActor>(ActorClass, SpawnTransform, SpawnParams);
		if (!NewActor)
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::COMPONENT_ADD_FAILED, FString::Printf(TEXT("Failed to spawn actor of class '%s' in map '%s'."), *ActorClass->GetName(), *MapPath)));
		}

		NewActor->Modify();
		if (!ActorLabel.IsEmpty())
		{
			NewActor->SetActorLabel(ActorLabel);
		}
		if (!FolderPath.IsEmpty())
		{
			NewActor->SetFolderPath(*FolderPath);
		}

		MapContext.World->MarkPackageDirty();
		if (MapContext.World->PersistentLevel)
		{
			MapContext.World->PersistentLevel->MarkPackageDirty();
		}

		if (bSelectActor && GEditor)
		{
			GEditor->SelectNone(false, true, false);
			GEditor->SelectActor(NewActor, true, true, true);
		}

		bool bSaved = !bSave;
		FString SaveError;
		if (bSave)
		{
			bSaved = SaveEditedMap(MapContext, SaveError);
		}

		bool bRestoreSuccess = true;
		FString RestoreError;
		if (bRestorePreviousMap)
		{
			bRestoreSuccess = RestorePreviousMap(MapContext, RestoreError);
		}

		const TSharedPtr<FJsonObject> ResponseObj = MakeShared<FJsonObject>();
		ResponseObj->SetBoolField(TEXT("success"), bSaved);
		ResponseObj->SetObjectField(TEXT("actor"), BuildActorJson(NewActor, MapContext.TargetMapPath));
		ResponseObj->SetBoolField(TEXT("saved"), bSaved);
		ResponseObj->SetBoolField(TEXT("restore_previous_map"), bRestorePreviousMap);
		ResponseObj->SetBoolField(TEXT("restore_success"), bRestoreSuccess);
		if (!SaveError.IsEmpty())
		{
			ResponseObj->SetStringField(TEXT("save_error"), SaveError);
		}
		if (!RestoreError.IsEmpty())
		{
			ResponseObj->SetStringField(TEXT("restore_error"), RestoreError);
		}
		return SerializeJsonObject(ResponseObj);
	}

	static FString HandleUpdateActorInMap(const TMap<FString, FString>& Params)
	{
		const FString MapPath = ExtractParamFromJson(Params, TEXT("map_path"));
		const FString ActorIdentifier = ExtractParamFromJson(Params, TEXT("actor_identifier"));
		const FString ActorLabel = ExtractParamFromJson(Params, TEXT("actor_label"));
		const FString FolderPath = ExtractParamFromJson(Params, TEXT("folder_path"));
		const bool bSave = ExtractBoolParam(Params, TEXT("save"), true);
		const bool bRestorePreviousMap = ExtractBoolParam(Params, TEXT("restore_previous_map"), false);
		const bool bSelectActor = ExtractBoolParam(Params, TEXT("select_actor"), false);
		const int32 MaxDepth = ExtractIntParam(Params, TEXT("max_depth"), 8, 1, 16);

		FLoadedMapContext MapContext;
		FString Error;
		if (!EnsureMapLoadedForEditing(MapPath, MapContext, Error))
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PARAM_INVALID, Error));
		}

		AActor* TargetActor = FindActorInWorld(MapContext.World, ActorIdentifier);
		if (!TargetActor)
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::COMPONENT_NOT_FOUND, FString::Printf(TEXT("Actor '%s' was not found in map '%s'."), *ActorIdentifier, *MapPath)));
		}

		const FScopedTransaction Transaction(NSLOCTEXT("VibeUE", "UpdateActorInMap", "Update Actor In Map"));
		TargetActor->Modify();
		if (MapContext.World->PersistentLevel)
		{
			MapContext.World->PersistentLevel->Modify();
		}

		const FString TransformJson = ExtractParamFromJson(Params, TEXT("transform_json"));
		if (!TransformJson.IsEmpty())
		{
			FTransform NewTransform = FTransform::Identity;
			if (!ParseTransformParam(Params, TEXT("transform_json"), NewTransform, Error))
			{
				return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PARAM_INVALID, Error));
			}
			TargetActor->SetActorTransform(NewTransform);
		}

		TArray<TSharedPtr<FJsonValue>> ResultsArray;
		int32 ModifiedCount = 0;
		int32 FailedCount = 0;

		const FString ValuesJson = ExtractParamFromJson(Params, TEXT("values_json"));
		if (!ValuesJson.IsEmpty())
		{
			TSharedPtr<FJsonObject> ValuesObj;
			if (!ParseJsonObjectParam(Params, TEXT("values_json"), ValuesObj, Error))
			{
				return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PARAM_INVALID, Error));
			}

			for (const auto& Pair : ValuesObj->Values)
			{
				FResolvedPropertyPath ResolvedPath;
				FString ResolveError;
				const TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
				ResultObj->SetStringField(TEXT("requested_path"), Pair.Key);

				if (!FVibeReflectionUtils::ResolvePropertyPath(TargetActor, Pair.Key, ResolvedPath, ResolveError))
				{
					ResultObj->SetBoolField(TEXT("success"), false);
					ResultObj->SetStringField(TEXT("error"), ResolveError);
					ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
					++FailedCount;
					continue;
				}

				if (!FVibeReflectionUtils::SetPropertyValueFromJson(ResolvedPath.Property, ResolvedPath.ValuePtr, Pair.Value, ResolveError, MaxDepth))
				{
					ResultObj->SetBoolField(TEXT("success"), false);
					ResultObj->SetStringField(TEXT("path"), ResolvedPath.CanonicalPath);
					ResultObj->SetStringField(TEXT("error"), ResolveError);
					ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
					++FailedCount;
					continue;
				}

				ResultObj->SetBoolField(TEXT("success"), true);
				ResultObj->SetStringField(TEXT("path"), ResolvedPath.CanonicalPath);
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				++ModifiedCount;
			}
		}

		if (!ActorLabel.IsEmpty())
		{
			TargetActor->SetActorLabel(ActorLabel);
			++ModifiedCount;
		}
		if (!FolderPath.IsEmpty())
		{
			TargetActor->SetFolderPath(*FolderPath);
			++ModifiedCount;
		}

		MapContext.World->MarkPackageDirty();
		if (MapContext.World->PersistentLevel)
		{
			MapContext.World->PersistentLevel->MarkPackageDirty();
		}

		if (bSelectActor && GEditor)
		{
			GEditor->SelectNone(false, true, false);
			GEditor->SelectActor(TargetActor, true, true, true);
		}

		bool bSaved = !bSave;
		FString SaveError;
		if (bSave && ModifiedCount > 0)
		{
			bSaved = SaveEditedMap(MapContext, SaveError);
			if (!bSaved)
			{
				++FailedCount;
			}
		}

		bool bRestoreSuccess = true;
		FString RestoreError;
		if (bRestorePreviousMap)
		{
			bRestoreSuccess = RestorePreviousMap(MapContext, RestoreError);
		}

		const TSharedPtr<FJsonObject> ResponseObj = MakeShared<FJsonObject>();
		ResponseObj->SetBoolField(TEXT("success"), FailedCount == 0 && ModifiedCount > 0 && bSaved);
		ResponseObj->SetObjectField(TEXT("actor"), BuildActorJson(TargetActor, MapContext.TargetMapPath));
		ResponseObj->SetNumberField(TEXT("modified_count"), ModifiedCount);
		ResponseObj->SetNumberField(TEXT("failed_count"), FailedCount);
		ResponseObj->SetArrayField(TEXT("results"), ResultsArray);
		ResponseObj->SetBoolField(TEXT("saved"), bSaved);
		ResponseObj->SetBoolField(TEXT("restore_previous_map"), bRestorePreviousMap);
		ResponseObj->SetBoolField(TEXT("restore_success"), bRestoreSuccess);
		if (!SaveError.IsEmpty())
		{
			ResponseObj->SetStringField(TEXT("save_error"), SaveError);
		}
		if (!RestoreError.IsEmpty())
		{
			ResponseObj->SetStringField(TEXT("restore_error"), RestoreError);
		}
		return SerializeJsonObject(ResponseObj);
	}

	static FString HandleRemoveActorFromMap(const TMap<FString, FString>& Params)
	{
		const FString MapPath = ExtractParamFromJson(Params, TEXT("map_path"));
		const FString ActorIdentifier = ExtractParamFromJson(Params, TEXT("actor_identifier"));
		const bool bSave = ExtractBoolParam(Params, TEXT("save"), true);
		const bool bRestorePreviousMap = ExtractBoolParam(Params, TEXT("restore_previous_map"), false);

		FLoadedMapContext MapContext;
		FString Error;
		if (!EnsureMapLoadedForEditing(MapPath, MapContext, Error))
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PARAM_INVALID, Error));
		}

		AActor* TargetActor = FindActorInWorld(MapContext.World, ActorIdentifier);
		if (!TargetActor)
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::COMPONENT_NOT_FOUND, FString::Printf(TEXT("Actor '%s' was not found in map '%s'."), *ActorIdentifier, *MapPath)));
		}

		const TSharedPtr<FJsonObject> RemovedActorObj = BuildActorJson(TargetActor, MapContext.TargetMapPath);

		const FScopedTransaction Transaction(NSLOCTEXT("VibeUE", "RemoveActorFromMap", "Remove Actor From Map"));
		TargetActor->Modify();
		if (MapContext.World->PersistentLevel)
		{
			MapContext.World->PersistentLevel->Modify();
		}

		const bool bDestroyed = MapContext.World->EditorDestroyActor(TargetActor, true);
		if (!bDestroyed)
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::NODE_DELETE_FAILED, FString::Printf(TEXT("Failed to remove actor '%s' from map '%s'."), *ActorIdentifier, *MapPath)));
		}

		MapContext.World->MarkPackageDirty();
		if (MapContext.World->PersistentLevel)
		{
			MapContext.World->PersistentLevel->MarkPackageDirty();
		}

		bool bSaved = !bSave;
		FString SaveError;
		if (bSave)
		{
			bSaved = SaveEditedMap(MapContext, SaveError);
		}

		bool bRestoreSuccess = true;
		FString RestoreError;
		if (bRestorePreviousMap)
		{
			bRestoreSuccess = RestorePreviousMap(MapContext, RestoreError);
		}

		const TSharedPtr<FJsonObject> ResponseObj = MakeShared<FJsonObject>();
		ResponseObj->SetBoolField(TEXT("success"), bSaved);
		ResponseObj->SetObjectField(TEXT("removed_actor"), RemovedActorObj);
		ResponseObj->SetBoolField(TEXT("saved"), bSaved);
		ResponseObj->SetBoolField(TEXT("restore_previous_map"), bRestorePreviousMap);
		ResponseObj->SetBoolField(TEXT("restore_success"), bRestoreSuccess);
		if (!SaveError.IsEmpty())
		{
			ResponseObj->SetStringField(TEXT("save_error"), SaveError);
		}
		if (!RestoreError.IsEmpty())
		{
			ResponseObj->SetStringField(TEXT("restore_error"), RestoreError);
		}
		return SerializeJsonObject(ResponseObj);
	}

	static FString HandleMutateMapActors(const TMap<FString, FString>& Params)
	{
		const FString MapPath = ExtractParamFromJson(Params, TEXT("map_path"));
		const bool bSave = ExtractBoolParam(Params, TEXT("save"), true);
		const bool bRestorePreviousMap = ExtractBoolParam(Params, TEXT("restore_previous_map"), false);
		const int32 MaxDepth = ExtractIntParam(Params, TEXT("max_depth"), 8, 1, 16);

		TArray<TSharedPtr<FJsonValue>> OperationsArray;
		FString Error;
		if (!ParseJsonArrayParamOptional(Params, TEXT("operations_json"), OperationsArray, Error) || OperationsArray.Num() == 0)
		{
			if (Error.IsEmpty())
			{
				Error = TEXT("operations_json is required and must contain at least one operation.");
			}
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PARAM_INVALID, Error));
		}

		FLoadedMapContext MapContext;
		if (!EnsureMapLoadedForEditing(MapPath, MapContext, Error))
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PARAM_INVALID, Error));
		}

		TArray<TSharedPtr<FJsonValue>> ResultsArray;
		TArray<TSharedPtr<FJsonValue>> ErrorsArray;
		int32 MutatedCount = 0;
		int32 FailedCount = 0;
		const int32 RequestedCount = OperationsArray.Num();

		for (const TSharedPtr<FJsonValue>& OperationValue : OperationsArray)
		{
			TSharedPtr<FJsonObject> OperationObj;
			if (!FJsonValueHelper::TryGetObject(OperationValue, OperationObj) || !OperationObj.IsValid())
			{
				const TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error"), TEXT("Each operation must be a JSON object."));
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				ErrorsArray.Add(MakeShared<FJsonValueObject>(FToolEnvelopeUtils::BuildErrorEntry(TEXT("op"), TEXT("unknown"), TEXT("Each operation must be a JSON object."), VibeUE::ErrorCodes::PARAM_INVALID)));
				++FailedCount;
				continue;
			}

			FString OpName;
			OperationObj->TryGetStringField(TEXT("op"), OpName);
			OpName = OpName.TrimStartAndEnd().ToLower();

			TMap<FString, FString> OperationParams;
			OperationParams.Add(TEXT("map_path"), MapPath);
			OperationParams.Add(TEXT("save"), TEXT("false"));
			OperationParams.Add(TEXT("restore_previous_map"), TEXT("false"));
			OperationParams.Add(TEXT("max_depth"), FString::FromInt(MaxDepth));

			auto AddJsonFieldIfPresent = [&](const FString& FieldName)
			{
				if (const TSharedPtr<FJsonValue>* FieldValue = OperationObj->Values.Find(FieldName))
				{
					FString JsonText;
					const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
					FJsonSerializer::Serialize(FieldValue->ToSharedRef(), TEXT(""), Writer);
					OperationParams.Add(FieldName, JsonText);
				}
			};

			auto AddStringFieldIfPresent = [&](const FString& FieldName)
			{
				FString FieldString;
				if (OperationObj->TryGetStringField(FieldName, FieldString))
				{
					OperationParams.Add(FieldName, FieldString);
				}
			};

			auto AddBoolFieldIfPresent = [&](const FString& FieldName)
			{
				bool bValue = false;
				if (OperationObj->TryGetBoolField(FieldName, bValue))
				{
					OperationParams.Add(FieldName, bValue ? TEXT("true") : TEXT("false"));
				}
			};

			AddStringFieldIfPresent(TEXT("actor_identifier"));
			AddStringFieldIfPresent(TEXT("actor_class"));
			AddStringFieldIfPresent(TEXT("actor_label"));
			AddStringFieldIfPresent(TEXT("folder_path"));
			AddBoolFieldIfPresent(TEXT("select_actor"));
			AddJsonFieldIfPresent(TEXT("transform_json"));
			AddJsonFieldIfPresent(TEXT("values_json"));

			FString ResultJson;
			if (OpName == TEXT("spawn"))
			{
				ResultJson = HandleSpawnActorInMap(OperationParams);
			}
			else if (OpName == TEXT("update"))
			{
				ResultJson = HandleUpdateActorInMap(OperationParams);
			}
			else if (OpName == TEXT("remove"))
			{
				ResultJson = HandleRemoveActorFromMap(OperationParams);
			}
			else
			{
				const TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
				ResultObj->SetStringField(TEXT("op"), OpName);
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error"), TEXT("Unsupported op. Supported values: spawn, update, remove."));
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				ErrorsArray.Add(MakeShared<FJsonValueObject>(FToolEnvelopeUtils::BuildErrorEntry(TEXT("op"), OpName, TEXT("Unsupported op. Supported values: spawn, update, remove."), VibeUE::ErrorCodes::PARAM_INVALID)));
				++FailedCount;
				continue;
			}

			TSharedPtr<FJsonObject> ResultObj;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResultJson);
			if (!FJsonSerializer::Deserialize(Reader, ResultObj) || !ResultObj.IsValid())
			{
				const TSharedPtr<FJsonObject> ParseErrorObj = MakeShared<FJsonObject>();
				ParseErrorObj->SetStringField(TEXT("op"), OpName);
				ParseErrorObj->SetBoolField(TEXT("success"), false);
				ParseErrorObj->SetStringField(TEXT("error"), TEXT("Failed to parse operation result JSON."));
				ResultsArray.Add(MakeShared<FJsonValueObject>(ParseErrorObj));
				ErrorsArray.Add(MakeShared<FJsonValueObject>(FToolEnvelopeUtils::BuildErrorEntry(TEXT("op"), OpName, TEXT("Failed to parse operation result JSON."), TEXT("TOOL_RESULT_PARSE_FAILED"))));
				++FailedCount;
				continue;
			}

			ResultObj->SetStringField(TEXT("op"), OpName);
			ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));

			bool bOpSuccess = false;
			ResultObj->TryGetBoolField(TEXT("success"), bOpSuccess);
			if (bOpSuccess)
			{
				++MutatedCount;
			}
			else
			{
				FString ErrorMessage;
				if (!ResultObj->TryGetStringField(TEXT("error"), ErrorMessage))
				{
					ResultObj->TryGetStringField(TEXT("error_message"), ErrorMessage);
				}
				ErrorsArray.Add(MakeShared<FJsonValueObject>(FToolEnvelopeUtils::BuildErrorEntry(TEXT("op"), OpName, ErrorMessage.IsEmpty() ? TEXT("Operation failed.") : ErrorMessage)));
				++FailedCount;
			}
		}

		bool bSaved = !bSave;
		FString SaveError;
		if (bSave && MutatedCount > 0)
		{
			bSaved = SaveEditedMap(MapContext, SaveError);
			if (!bSaved)
			{
				ErrorsArray.Add(MakeShared<FJsonValueObject>(FToolEnvelopeUtils::BuildErrorEntry(TEXT("map_path"), MapPath, SaveError, VibeUE::ErrorCodes::PROPERTY_SET_FAILED)));
				++FailedCount;
			}
		}

		bool bRestoreSuccess = true;
		FString RestoreError;
		if (bRestorePreviousMap)
		{
			bRestoreSuccess = RestorePreviousMap(MapContext, RestoreError);
			if (!bRestoreSuccess && !RestoreError.IsEmpty())
			{
				ErrorsArray.Add(MakeShared<FJsonValueObject>(FToolEnvelopeUtils::BuildErrorEntry(TEXT("map_path"), MapPath, RestoreError)));
			}
		}

		const TSharedPtr<FJsonObject> TargetObj = MakeShared<FJsonObject>();
		TargetObj->SetStringField(TEXT("map_path"), MapPath);

		FBatchToolEnvelope Envelope;
		Envelope.Operation = TEXT("mutate_map_actors");
		Envelope.ResourceKind = TEXT("map_actors");
		Envelope.Target = TargetObj;
		Envelope.RequestedCount = RequestedCount;
		Envelope.SuccessCount = MutatedCount;
		Envelope.FailedCount = FailedCount;
		Envelope.Results = ResultsArray;
		Envelope.Errors = ErrorsArray;
		Envelope.bIncludeSaveFields = true;
		Envelope.bSaveRequested = bSave;
		Envelope.bSaved = bSaved;
		Envelope.SaveError = SaveError;
		Envelope.bIncludeRestoreFields = true;
		Envelope.bRestoreRequested = bRestorePreviousMap;
		Envelope.bRestoreSuccess = bRestoreSuccess;
		Envelope.RestoreError = RestoreError;

		const TSharedPtr<FJsonObject> ResponseObj = FToolEnvelopeUtils::BuildBatchEnvelope(Envelope);
		ResponseObj->SetStringField(TEXT("map_path"), MapPath);
		ResponseObj->SetNumberField(TEXT("mutated_count"), MutatedCount);
		return FToolEnvelopeUtils::SerializeJsonObject(ResponseObj);
	}

	static FString HandleMutateDataTableRows(const TMap<FString, FString>& Params)
	{
		const FString TablePath = ExtractParamFromJson(Params, TEXT("table_path"));
		const bool bSave = ExtractBoolParam(Params, TEXT("save"), true);
		const int32 MaxDepth = ExtractIntParam(Params, TEXT("max_depth"), 8, 1, 16);
		FString Error;

		UDataTable* DataTable = Cast<UDataTable>(UEditorAssetLibrary::LoadAsset(TablePath));
		if (!DataTable)
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PROPERTY_NOT_FOUND, FString::Printf(TEXT("Failed to load DataTable '%s'."), *TablePath)));
		}

		const UScriptStruct* RowStruct = DataTable->GetRowStruct();
		if (!RowStruct)
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PROPERTY_NOT_FOUND, TEXT("DataTable has no row struct.")));
		}

		const FScopedTransaction Transaction(NSLOCTEXT("VibeUE", "UpsertDataTableRows", "Upsert Data Table Rows"));
		DataTable->Modify();

		TArray<TSharedPtr<FJsonValue>> ResultsArray;
		TArray<TSharedPtr<FJsonValue>> ErrorsArray;
		int32 MutatedCount = 0;
		int32 FailedCount = 0;
		int32 RequestedCount = 0;

		const FString OperationsJson = ExtractParamFromJson(Params, TEXT("operations_json"));
		if (!OperationsJson.IsEmpty())
		{
			TArray<TSharedPtr<FJsonValue>> OperationsArray;
			if (!ParseJsonArrayParamOptional(Params, TEXT("operations_json"), OperationsArray, Error))
			{
				return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PARAM_INVALID, Error));
			}

			RequestedCount = OperationsArray.Num();

			for (const TSharedPtr<FJsonValue>& OperationValue : OperationsArray)
			{
				TSharedPtr<FJsonObject> OperationObj;
				if (!FJsonValueHelper::TryGetObject(OperationValue, OperationObj) || !OperationObj.IsValid())
				{
					const TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
					ResultObj->SetBoolField(TEXT("success"), false);
					ResultObj->SetStringField(TEXT("error"), TEXT("Operation entry must be a JSON object."));
					ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
					ErrorsArray.Add(MakeShared<FJsonValueObject>(FToolEnvelopeUtils::BuildErrorEntry(TEXT("op"), TEXT("unknown"), TEXT("Operation entry must be a JSON object."), VibeUE::ErrorCodes::PARAM_INVALID)));
					++FailedCount;
					continue;
				}

				FString OpName;
				OperationObj->TryGetStringField(TEXT("op"), OpName);
				OpName = OpName.TrimStartAndEnd().ToLower();

				FString RowNameString;
				OperationObj->TryGetStringField(TEXT("row_name"), RowNameString);
				const FName RowName(*RowNameString);

				const TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
				ResultObj->SetStringField(TEXT("op"), OpName);
				ResultObj->SetStringField(TEXT("row_name"), RowNameString);

				if (OpName == TEXT("upsert"))
				{
					if (RowNameString.IsEmpty())
					{
						ResultObj->SetBoolField(TEXT("success"), false);
						ResultObj->SetStringField(TEXT("error"), TEXT("upsert requires row_name."));
						ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
						ErrorsArray.Add(MakeShared<FJsonValueObject>(FToolEnvelopeUtils::BuildErrorEntry(TEXT("op"), OpName, TEXT("upsert requires row_name."), VibeUE::ErrorCodes::PARAM_INVALID)));
						++FailedCount;
						continue;
					}

					const uint8* ExistingRowData = DataTable->FindRowUnchecked(RowName);
					uint8* WorkingRowData = static_cast<uint8*>(FMemory::Malloc(RowStruct->GetStructureSize()));
					RowStruct->InitializeStruct(WorkingRowData);
					if (ExistingRowData)
					{
						RowStruct->CopyScriptStruct(WorkingRowData, ExistingRowData);
					}

					TSharedPtr<FJsonValue> RowPayload = OperationObj->TryGetField(TEXT("row"));
					if (!RowPayload.IsValid())
					{
						RowPayload = OperationObj->TryGetField(TEXT("value"));
					}

					FString RowError;
					if (!RowPayload.IsValid() || !FVibeReflectionUtils::SetStructValueFromJson(RowStruct, WorkingRowData, RowPayload, RowError, MaxDepth))
					{
						ResultObj->SetBoolField(TEXT("success"), false);
						ResultObj->SetStringField(TEXT("error"), RowPayload.IsValid() ? RowError : TEXT("upsert requires row or value payload."));
						ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
						ErrorsArray.Add(MakeShared<FJsonValueObject>(FToolEnvelopeUtils::BuildErrorEntry(TEXT("row_name"), RowNameString, RowPayload.IsValid() ? RowError : TEXT("upsert requires row or value payload."), VibeUE::ErrorCodes::PROPERTY_SET_FAILED)));
						++FailedCount;
						RowStruct->DestroyStruct(WorkingRowData);
						FMemory::Free(WorkingRowData);
						continue;
					}

					DataTable->AddRow(RowName, WorkingRowData, RowStruct);
					ResultObj->SetBoolField(TEXT("success"), true);
					ResultObj->SetBoolField(TEXT("updated_existing"), ExistingRowData != nullptr);
					ResultObj->SetObjectField(TEXT("row"), FVibeReflectionUtils::BuildStructValueJson(RowStruct, WorkingRowData, 4));
					ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
					++MutatedCount;

					RowStruct->DestroyStruct(WorkingRowData);
					FMemory::Free(WorkingRowData);
					continue;
				}

				if (OpName == TEXT("delete"))
				{
					if (RowNameString.IsEmpty())
					{
						ResultObj->SetBoolField(TEXT("success"), false);
						ResultObj->SetStringField(TEXT("error"), TEXT("delete requires row_name."));
						ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
						ErrorsArray.Add(MakeShared<FJsonValueObject>(FToolEnvelopeUtils::BuildErrorEntry(TEXT("op"), OpName, TEXT("delete requires row_name."), VibeUE::ErrorCodes::PARAM_INVALID)));
						++FailedCount;
						continue;
					}

					if (!DataTable->FindRowUnchecked(RowName))
					{
						ResultObj->SetBoolField(TEXT("success"), false);
						ResultObj->SetStringField(TEXT("error"), TEXT("Row not found."));
						ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
						ErrorsArray.Add(MakeShared<FJsonValueObject>(FToolEnvelopeUtils::BuildErrorEntry(TEXT("row_name"), RowNameString, TEXT("Row not found."), VibeUE::ErrorCodes::PROPERTY_NOT_FOUND)));
						++FailedCount;
						continue;
					}

					DataTable->RemoveRow(RowName);
					ResultObj->SetBoolField(TEXT("success"), true);
					ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
					++MutatedCount;
					continue;
				}

				if (OpName == TEXT("rename"))
				{
					FString NewRowNameString;
					OperationObj->TryGetStringField(TEXT("new_row_name"), NewRowNameString);
					const FName NewRowName(*NewRowNameString);
					ResultObj->SetStringField(TEXT("new_row_name"), NewRowNameString);

					if (RowNameString.IsEmpty() || NewRowNameString.IsEmpty())
					{
						ResultObj->SetBoolField(TEXT("success"), false);
						ResultObj->SetStringField(TEXT("error"), TEXT("rename requires row_name and new_row_name."));
						ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
						ErrorsArray.Add(MakeShared<FJsonValueObject>(FToolEnvelopeUtils::BuildErrorEntry(TEXT("op"), OpName, TEXT("rename requires row_name and new_row_name."), VibeUE::ErrorCodes::PARAM_INVALID)));
						++FailedCount;
						continue;
					}

					const uint8* ExistingRowData = DataTable->FindRowUnchecked(RowName);
					if (!ExistingRowData)
					{
						ResultObj->SetBoolField(TEXT("success"), false);
						ResultObj->SetStringField(TEXT("error"), TEXT("Source row not found."));
						ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
						ErrorsArray.Add(MakeShared<FJsonValueObject>(FToolEnvelopeUtils::BuildErrorEntry(TEXT("row_name"), RowNameString, TEXT("Source row not found."), VibeUE::ErrorCodes::PROPERTY_NOT_FOUND)));
						++FailedCount;
						continue;
					}

					if (DataTable->FindRowUnchecked(NewRowName))
					{
						ResultObj->SetBoolField(TEXT("success"), false);
						ResultObj->SetStringField(TEXT("error"), TEXT("Target row name already exists."));
						ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
						ErrorsArray.Add(MakeShared<FJsonValueObject>(FToolEnvelopeUtils::BuildErrorEntry(TEXT("new_row_name"), NewRowNameString, TEXT("Target row name already exists."), VibeUE::ErrorCodes::BLUEPRINT_ALREADY_EXISTS)));
						++FailedCount;
						continue;
					}

					DataTable->AddRow(NewRowName, ExistingRowData, RowStruct);
					DataTable->RemoveRow(RowName);
					ResultObj->SetBoolField(TEXT("success"), true);
					ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
					++MutatedCount;
					continue;
				}

				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error"), TEXT("Unsupported op. Supported values: upsert, delete, rename."));
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				ErrorsArray.Add(MakeShared<FJsonValueObject>(FToolEnvelopeUtils::BuildErrorEntry(TEXT("op"), OpName, TEXT("Unsupported op. Supported values: upsert, delete, rename."), VibeUE::ErrorCodes::PARAM_INVALID)));
				++FailedCount;
			}
		}
		else
		{
			TSharedPtr<FJsonObject> RowsObj;
			if (!ParseJsonObjectParam(Params, TEXT("rows_json"), RowsObj, Error))
			{
				return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PARAM_INVALID, Error));
			}

			RequestedCount = RowsObj->Values.Num();

			for (const auto& Pair : RowsObj->Values)
			{
				const FName RowName(*Pair.Key);
				const uint8* ExistingRowData = DataTable->FindRowUnchecked(RowName);
				uint8* WorkingRowData = static_cast<uint8*>(FMemory::Malloc(RowStruct->GetStructureSize()));
				RowStruct->InitializeStruct(WorkingRowData);

				if (ExistingRowData)
				{
					RowStruct->CopyScriptStruct(WorkingRowData, ExistingRowData);
				}

				const TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
				ResultObj->SetStringField(TEXT("op"), TEXT("upsert"));
				ResultObj->SetStringField(TEXT("row_name"), Pair.Key);
				ResultObj->SetBoolField(TEXT("updated_existing"), ExistingRowData != nullptr);

				FString RowError;
				if (!FVibeReflectionUtils::SetStructValueFromJson(RowStruct, WorkingRowData, Pair.Value, RowError, MaxDepth))
				{
					ResultObj->SetBoolField(TEXT("success"), false);
					ResultObj->SetStringField(TEXT("error"), RowError);
					ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
					ErrorsArray.Add(MakeShared<FJsonValueObject>(FToolEnvelopeUtils::BuildErrorEntry(TEXT("row_name"), Pair.Key, RowError, VibeUE::ErrorCodes::PROPERTY_SET_FAILED)));
					++FailedCount;
					RowStruct->DestroyStruct(WorkingRowData);
					FMemory::Free(WorkingRowData);
					continue;
				}

				DataTable->AddRow(RowName, WorkingRowData, RowStruct);
				ResultObj->SetBoolField(TEXT("success"), true);
				ResultObj->SetObjectField(TEXT("row"), FVibeReflectionUtils::BuildStructValueJson(RowStruct, WorkingRowData, 4));
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				++MutatedCount;

				RowStruct->DestroyStruct(WorkingRowData);
				FMemory::Free(WorkingRowData);
			}
		}

		DataTable->MarkPackageDirty();

		bool bSaved = !bSave;
		FString SaveError;
		if (bSave)
		{
			bSaved = UEditorAssetLibrary::SaveLoadedAsset(DataTable, false);
			if (!bSaved)
			{
				SaveError = FString::Printf(TEXT("Failed to save DataTable '%s'."), *TablePath);
				ErrorsArray.Add(MakeShared<FJsonValueObject>(FToolEnvelopeUtils::BuildErrorEntry(TEXT("table_path"), TablePath, SaveError, VibeUE::ErrorCodes::PROPERTY_SET_FAILED)));
			}
		}

		const TSharedPtr<FJsonObject> TargetObj = MakeShared<FJsonObject>();
		TargetObj->SetStringField(TEXT("table_path"), TablePath);
		TargetObj->SetStringField(TEXT("row_struct_path"), RowStruct->GetPathName());

		FBatchToolEnvelope Envelope;
		Envelope.Operation = TEXT("mutate_data_table_rows");
		Envelope.ResourceKind = TEXT("data_table_rows");
		Envelope.Target = TargetObj;
		Envelope.RequestedCount = RequestedCount;
		Envelope.SuccessCount = MutatedCount;
		Envelope.FailedCount = FailedCount;
		Envelope.Results = ResultsArray;
		Envelope.Errors = ErrorsArray;
		Envelope.bIncludeSaveFields = true;
		Envelope.bSaveRequested = bSave;
		Envelope.bSaved = bSaved;
		Envelope.SaveError = SaveError;

		const TSharedPtr<FJsonObject> ResponseObj = FToolEnvelopeUtils::BuildBatchEnvelope(Envelope);
		ResponseObj->SetStringField(TEXT("table_path"), TablePath);
		ResponseObj->SetStringField(TEXT("row_struct_path"), RowStruct->GetPathName());
		ResponseObj->SetNumberField(TEXT("mutated_count"), MutatedCount);
		return FToolEnvelopeUtils::SerializeJsonObject(ResponseObj);
	}
}

REGISTER_VIBEUE_TOOL(set_map_world_settings,
	"Load a map asset into the editor, set typed properties on its WorldSettings, and optionally save and restore the previously open map.",
	"World",
	TOOL_PARAMS(
		TOOL_PARAM("map_path", "Target map asset path such as /Game/Foo/L_MyMap.", "string", true),
		TOOL_PARAM("values_json", "JSON object mapping WorldSettings property paths to typed JSON values or raw JSON values.", "string", true),
		TOOL_PARAM("save", "Whether to save the map after modification (default true).", "boolean", false),
		TOOL_PARAM("restore_previous_map", "Whether to reopen the previously loaded editor map after saving (default false).", "boolean", false),
		TOOL_PARAM("max_depth", "Maximum recursive depth for complex typed values during writes.", "number", false)
	),
	{
		return HandleSetMapWorldSettings(Params);
	}
);

REGISTER_VIBEUE_TOOL(get_map_world_settings,
	"Load a map asset into the editor, inspect its WorldSettings values, optionally include schema details, and optionally restore the previously open map.",
	"World",
	TOOL_PARAMS(
		TOOL_PARAM("map_path", "Target map asset path such as /Game/Foo/L_MyMap.", "string", true),
		TOOL_PARAM("paths_json", "Optional JSON array of WorldSettings property paths to read. If omitted, returns top-level properties up to max_properties.", "string", false),
		TOOL_PARAM("include_schema", "Whether to include the WorldSettings property schema in the response (default false).", "boolean", false),
		TOOL_PARAM("include_inherited", "Whether top-level property enumeration should include inherited properties when paths_json is omitted (default true).", "boolean", false),
		TOOL_PARAM("max_depth", "Expansion depth for complex returned values and optional schema.", "number", false),
		TOOL_PARAM("max_properties", "Maximum number of top-level properties to include when paths_json is omitted. Use 0 for no cap.", "number", false),
		TOOL_PARAM("restore_previous_map", "Whether to reopen the previously loaded editor map after reading (default false).", "boolean", false)
	),
	{
		return HandleGetMapWorldSettings(Params);
	}
);

REGISTER_VIBEUE_TOOL(list_map_actors,
	"Load a map asset into the editor and list actors from it with optional class and label filters, without modifying the map.",
	"World",
	TOOL_PARAMS(
		TOOL_PARAM("map_path", "Target map asset path such as /Game/Foo/L_MyMap.", "string", true),
		TOOL_PARAM("class_filter", "Optional actor class filter. Accepts class paths or case-insensitive name/path substrings.", "string", false),
		TOOL_PARAM("label_filter", "Optional actor label/name substring filter.", "string", false),
		TOOL_PARAM("include_hidden", "Whether to include hidden actors (default false).", "boolean", false),
		TOOL_PARAM("max_results", "Maximum number of actors to return. Use 0 for no cap.", "number", false),
		TOOL_PARAM("restore_previous_map", "Whether to reopen the previously loaded editor map after reading (default false).", "boolean", false)
	),
	{
		return HandleListMapActors(Params);
	}
);

REGISTER_VIBEUE_TOOL(mutate_map_actors,
	"Mutate actors inside a map through one batch-oriented mutation tool. operations_json items use op=spawn|update|remove. spawn expects actor_class and optional transform_json/actor_label/folder_path/select_actor. update expects actor_identifier and optional transform_json/values_json/actor_label/folder_path/select_actor. remove expects actor_identifier. Shared save and restore semantics apply to the whole batch.",
	"World",
	TOOL_PARAMS(
		TOOL_PARAM("map_path", "Target map asset path such as /Game/Foo/L_MyMap.", "string", true),
		TOOL_PARAM("operations_json", "JSON array of actor mutation operations. Each item uses op=spawn|update|remove and the fields required by that operation.", "string", true),
		TOOL_PARAM("save", "Whether to save the map after applying mutations (default true).", "boolean", false),
		TOOL_PARAM("restore_previous_map", "Whether to reopen the previously loaded editor map after saving (default false).", "boolean", false),
		TOOL_PARAM("max_depth", "Maximum recursive depth for complex typed values during update operations.", "number", false)
	),
	{
		return HandleMutateMapActors(Params);
	}
);

REGISTER_VIBEUE_TOOL(mutate_data_table_rows,
	"Mutate DataTable rows through one typed operation surface. operations_json items use op=upsert|delete|rename. upsert expects row_name plus row or value payload. delete expects row_name. rename expects row_name and new_row_name. rows_json remains available as a shorthand for pure upsert batches.",
	"Data",
	TOOL_PARAMS(
		TOOL_PARAM("table_path", "Target DataTable asset path.", "string", true),
		TOOL_PARAM("operations_json", "Optional JSON array of row mutation operations. Each item uses op=upsert|delete|rename.", "string", false),
		TOOL_PARAM("rows_json", "Compatibility shorthand for pure upsert batches: JSON object mapping row names to typed JSON row objects or raw field objects.", "string", false),
		TOOL_PARAM("save", "Whether to save the DataTable after modification (default true).", "boolean", false),
		TOOL_PARAM("max_depth", "Maximum recursive depth for complex row field writes.", "number", false)
	),
	{
		return HandleMutateDataTableRows(Params);
	}
);

REGISTER_VIBEUE_TOOL(get_data_table_rows,
	"Read DataTable rows as typed struct JSON. If row_names_json is omitted, returns rows from the whole table up to max_rows.",
	"Data",
	TOOL_PARAMS(
		TOOL_PARAM("table_path", "Target DataTable asset path.", "string", true),
		TOOL_PARAM("row_names_json", "Optional JSON array of row names to fetch.", "string", false),
		TOOL_PARAM("max_rows", "Maximum number of rows to return when row_names_json is omitted or larger than the cap. Use 0 for no cap.", "number", false),
		TOOL_PARAM("max_depth", "Expansion depth for complex row values.", "number", false)
	),
	{
		return HandleGetDataTableRows(Params);
	}
);

REGISTER_VIBEUE_TOOL(create_data_table_with_row_struct_object,
	"Create a DataTable by directly binding a loaded row struct object path, including Blueprint-authored UserDefinedStruct assets that are valid for DataTables.",
	"Data",
	TOOL_PARAMS(
		TOOL_PARAM("row_struct_object_path", "Row struct object path, including Blueprint-authored UserDefinedStruct assets.", "string", true),
		TOOL_PARAM("asset_path", "Destination asset folder, default /Game/Data.", "string", false),
		TOOL_PARAM("asset_name", "New DataTable asset name.", "string", true),
		TOOL_PARAM("save", "Whether to save the new DataTable asset immediately (default true).", "boolean", false)
	),
	{
		return HandleCreateDataTableWithRowStructObject(Params);
	}
);

REGISTER_VIBEUE_TOOL(get_data_table_schema,
	"Inspect a DataTable and return its row struct schema, including column metadata and row count.",
	"Data",
	TOOL_PARAMS(
		TOOL_PARAM("table_path", "Target DataTable asset path.", "string", true),
		TOOL_PARAM("include_inherited", "Whether to include inherited row struct properties (default true).", "boolean", false)
	),
	{
		return HandleGetDataTableSchema(Params);
	}
);
