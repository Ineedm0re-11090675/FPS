// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Core/ToolRegistry.h"
#include "Reflection/VibeReflectionUtils.h"
#include "Tools/ToolEnvelopeUtils.h"

#include "Core/ErrorCodes.h"
#include "Json.h"

namespace
{
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

	static TSharedPtr<FJsonObject> BuildTargetInfoJson(const FResolvedObjectTarget& Target)
	{
		const TSharedPtr<FJsonObject> TargetObj = MakeShared<FJsonObject>();
		TargetObj->SetStringField(TEXT("requested_target"), Target.RequestedTarget);
		TargetObj->SetStringField(TEXT("resolved_kind"), Target.ResolvedKind);
		TargetObj->SetStringField(TEXT("resolved_object_path"), Target.ResolvedObjectPath);
		TargetObj->SetBoolField(TEXT("is_blueprint_cdo"), Target.bIsBlueprintCDO);
		if (Target.SaveObject)
		{
			TargetObj->SetStringField(TEXT("save_object_path"), Target.SaveObject->GetPathName());
		}
		return TargetObj;
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

	static bool ParseJsonArrayParam(const TMap<FString, FString>& Params, const FString& FieldName, TArray<TSharedPtr<FJsonValue>>& OutArray, FString& OutError)
	{
		const FString JsonText = ExtractParamFromJson(Params, FieldName);
		if (JsonText.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Parameter '%s' is required."), *FieldName);
			return false;
		}

		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutArray))
		{
			OutError = FString::Printf(TEXT("Parameter '%s' must be a valid JSON array."), *FieldName);
			return false;
		}

		return true;
	}

	static FString HandleGetObjectSchema(const TMap<FString, FString>& Params)
	{
		const FString TargetParam = ExtractParamFromJson(Params, TEXT("target"));
		const bool bIncludeInherited = ExtractBoolParam(Params, TEXT("include_inherited"), true);
		const int32 MaxDepth = ExtractIntParam(Params, TEXT("max_depth"), 2, 1, 8);

		FResolvedObjectTarget ResolvedTarget;
		FString Error;
		if (!FVibeReflectionUtils::ResolveTarget(TargetParam, ResolvedTarget, Error))
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PARAM_INVALID, Error));
		}

		const TSharedPtr<FJsonObject> ResponseObj = MakeShared<FJsonObject>();
		ResponseObj->SetBoolField(TEXT("success"), true);
		ResponseObj->SetObjectField(TEXT("target"), BuildTargetInfoJson(ResolvedTarget));
		ResponseObj->SetObjectField(TEXT("schema"), FVibeReflectionUtils::BuildObjectSchemaJson(ResolvedTarget.TargetObject, bIncludeInherited, MaxDepth));
		return SerializeJsonObject(ResponseObj);
	}

	static FString HandleGetObjectProperties(const TMap<FString, FString>& Params)
	{
		const FString TargetParam = ExtractParamFromJson(Params, TEXT("target"));
		const int32 MaxDepth = ExtractIntParam(Params, TEXT("max_depth"), 4, 1, 8);

		FResolvedObjectTarget ResolvedTarget;
		FString Error;
		if (!FVibeReflectionUtils::ResolveTarget(TargetParam, ResolvedTarget, Error))
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PARAM_INVALID, Error));
		}

		TArray<TSharedPtr<FJsonValue>> PathsArray;
		if (!ParseJsonArrayParam(Params, TEXT("paths_json"), PathsArray, Error))
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PARAM_INVALID, Error));
		}

		const TSharedPtr<FJsonObject> ValuesObj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ErrorsArray;

		for (const TSharedPtr<FJsonValue>& PathValue : PathsArray)
		{
			FString Path;
			if (!PathValue.IsValid() || !PathValue->TryGetString(Path))
			{
				continue;
			}

			FResolvedPropertyPath ResolvedPath;
			FString ResolveError;
			if (!FVibeReflectionUtils::ResolvePropertyPath(ResolvedTarget.TargetObject, Path, ResolvedPath, ResolveError))
			{
				const TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
				ErrorObj->SetStringField(TEXT("path"), Path);
				ErrorObj->SetStringField(TEXT("error"), ResolveError);
				ErrorsArray.Add(MakeShared<FJsonValueObject>(ErrorObj));
				continue;
			}

			ValuesObj->SetObjectField(ResolvedPath.CanonicalPath, FVibeReflectionUtils::BuildTypedValueJson(ResolvedPath.Property, ResolvedPath.ValuePtr, MaxDepth));
		}

		const TSharedPtr<FJsonObject> ResponseObj = MakeShared<FJsonObject>();
		ResponseObj->SetBoolField(TEXT("success"), ErrorsArray.Num() == 0);
		ResponseObj->SetObjectField(TEXT("target"), BuildTargetInfoJson(ResolvedTarget));
		ResponseObj->SetObjectField(TEXT("values"), ValuesObj);
		ResponseObj->SetArrayField(TEXT("errors"), ErrorsArray);
		return SerializeJsonObject(ResponseObj);
	}

	static FString HandleSetObjectProperties(const TMap<FString, FString>& Params)
	{
		const FString TargetParam = ExtractParamFromJson(Params, TEXT("target"));
		const bool bSave = ExtractBoolParam(Params, TEXT("save"), true);
		const bool bCompileBlueprint = ExtractBoolParam(Params, TEXT("compile_blueprint"), true);
		const int32 MaxDepth = ExtractIntParam(Params, TEXT("max_depth"), 8, 1, 16);

		FResolvedObjectTarget ResolvedTarget;
		FString Error;
		if (!FVibeReflectionUtils::ResolveTarget(TargetParam, ResolvedTarget, Error))
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PARAM_INVALID, Error));
		}

		TSharedPtr<FJsonObject> ValuesObj;
		if (!ParseJsonObjectParam(Params, TEXT("values_json"), ValuesObj, Error))
		{
			return SerializeJsonObject(BuildErrorResponse(VibeUE::ErrorCodes::PARAM_INVALID, Error));
		}

		TArray<TSharedPtr<FJsonValue>> ResultsArray;
		TArray<TSharedPtr<FJsonValue>> ErrorsArray;
		int32 ModifiedCount = 0;
		int32 FailedCount = 0;
		const int32 RequestedCount = ValuesObj->Values.Num();
		FString SaveError;

		for (const auto& Pair : ValuesObj->Values)
		{
			FResolvedPropertyPath ResolvedPath;
			FString ResolveError;
			const TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
			ResultObj->SetStringField(TEXT("requested_path"), Pair.Key);

			if (!FVibeReflectionUtils::ResolvePropertyPath(ResolvedTarget.TargetObject, Pair.Key, ResolvedPath, ResolveError))
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

		if (bSave && ModifiedCount > 0)
		{
			if (!FVibeReflectionUtils::SaveResolvedTarget(ResolvedTarget, bCompileBlueprint, SaveError))
			{
				const TSharedPtr<FJsonObject> SaveErrorObj = MakeShared<FJsonObject>();
				SaveErrorObj->SetBoolField(TEXT("success"), false);
				SaveErrorObj->SetStringField(TEXT("error"), SaveError);
				ResultsArray.Add(MakeShared<FJsonValueObject>(SaveErrorObj));
				ErrorsArray.Add(MakeShared<FJsonValueObject>(FToolEnvelopeUtils::BuildErrorEntry(TEXT("target"), ResolvedTarget.ResolvedObjectPath, SaveError, VibeUE::ErrorCodes::PROPERTY_SET_FAILED)));
				++FailedCount;
			}
		}

		FBatchToolEnvelope Envelope;
		Envelope.Operation = TEXT("set_object_properties");
		Envelope.ResourceKind = TEXT("object");
		Envelope.Target = BuildTargetInfoJson(ResolvedTarget);
		Envelope.RequestedCount = RequestedCount;
		Envelope.SuccessCount = ModifiedCount;
		Envelope.FailedCount = FailedCount;
		Envelope.Results = ResultsArray;
		Envelope.Errors = ErrorsArray;
		Envelope.bIncludeSaveFields = true;
		Envelope.bSaveRequested = bSave;
		Envelope.bSaved = !bSave || (ModifiedCount > 0 && SaveError.IsEmpty());
		Envelope.SaveError = SaveError;

		const TSharedPtr<FJsonObject> ResponseObj = FToolEnvelopeUtils::BuildBatchEnvelope(Envelope);
		ResponseObj->SetNumberField(TEXT("modified_count"), ModifiedCount);
		return FToolEnvelopeUtils::SerializeJsonObject(ResponseObj);
	}
}

REGISTER_VIBEUE_TOOL(get_object_schema,
	"Inspect a UObject, asset, class, or Blueprint CDO and return a typed property schema. Use target prefixes like cdo:, class:, object:, or asset: for ambiguous targets.",
	"Reflection",
	TOOL_PARAMS(
		TOOL_PARAM("target", "Target object specifier. Supports asset/object paths and prefixes cdo:, class:, object:, asset:.", "string", true),
		TOOL_PARAM("include_inherited", "Whether to include inherited properties (default true).", "boolean", false),
		TOOL_PARAM("max_depth", "Schema expansion depth for nested struct properties (default 2).", "number", false)
	),
	{
		return HandleGetObjectSchema(Params);
	}
);

REGISTER_VIBEUE_TOOL(get_object_properties,
	"Read typed property values from a UObject, asset, class, or Blueprint CDO. Property paths support nested struct fields and array indices like Foo.Bar[0].Baz.",
	"Reflection",
	TOOL_PARAMS(
		TOOL_PARAM("target", "Target object specifier. Supports asset/object paths and prefixes cdo:, class:, object:, asset:.", "string", true),
		TOOL_PARAM("paths_json", "JSON array of property paths to read.", "string", true),
		TOOL_PARAM("max_depth", "Expansion depth for returned complex values such as structs, arrays, maps, and sets.", "number", false)
	),
	{
		return HandleGetObjectProperties(Params);
	}
);

REGISTER_VIBEUE_TOOL(set_object_properties,
	"Set typed property values on a UObject, asset, class, or Blueprint CDO using structured JSON values instead of Unreal text import syntax.",
	"Reflection",
	TOOL_PARAMS(
		TOOL_PARAM("target", "Target object specifier. Supports asset/object paths and prefixes cdo:, class:, object:, asset:.", "string", true),
		TOOL_PARAM("values_json", "JSON object mapping property paths to typed JSON values or raw JSON values.", "string", true),
		TOOL_PARAM("save", "Whether to save the underlying asset after modification (default true).", "boolean", false),
		TOOL_PARAM("compile_blueprint", "Whether to compile the owning blueprint when the target resolves to a Blueprint CDO (default true).", "boolean", false),
		TOOL_PARAM("max_depth", "Maximum recursive depth for complex values during writes.", "number", false)
	),
	{
		return HandleSetObjectProperties(Params);
	}
);
