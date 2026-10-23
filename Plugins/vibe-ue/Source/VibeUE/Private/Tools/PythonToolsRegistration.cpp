// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Tools/PythonTools.h"
#include "Tools/PythonDiscoveryService.h"
#include "Core/ToolRegistry.h"
#include "Json.h"

// Helper function to extract a field from ParamsJson
static FString ExtractParamFromJson(const TMap<FString, FString>& Params, const FString& FieldName)
{
	// First check if parameter exists directly (case-insensitive check for action/Action)
	const FString* DirectParam = Params.Find(FieldName);
	if (DirectParam)
	{
		return *DirectParam;
	}
	
	// Also check capitalized version (MCP server capitalizes 'action' to 'Action')
	FString CapitalizedField = FieldName;
	if (CapitalizedField.Len() > 0)
	{
		CapitalizedField[0] = FChar::ToUpper(CapitalizedField[0]);
	}
	DirectParam = Params.Find(CapitalizedField);
	if (DirectParam)
	{
		return *DirectParam;
	}

	// Otherwise, try to extract from ParamsJson
	const FString* ParamsJsonStr = Params.Find(TEXT("ParamsJson"));
	if (!ParamsJsonStr)
	{
		return FString();
	}

	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(*ParamsJsonStr);
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

// Helper to extract with default value
static FString ExtractParamWithDefault(const TMap<FString, FString>& Params, const FString& FieldName, const FString& DefaultValue)
{
	FString Value = ExtractParamFromJson(Params, FieldName);
	return Value.IsEmpty() ? DefaultValue : Value;
}

// Helper to extract boolean
static bool ExtractBoolParam(const TMap<FString, FString>& Params, const FString& FieldName, bool DefaultValue)
{
	FString Value = ExtractParamFromJson(Params, FieldName);
	if (Value.IsEmpty())
	{
		return DefaultValue;
	}
	return Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("1"));
}

// Helper to extract integer
static int32 ExtractIntParam(const TMap<FString, FString>& Params, const FString& FieldName, int32 DefaultValue)
{
	FString Value = ExtractParamFromJson(Params, FieldName);
	if (Value.IsEmpty())
	{
		return DefaultValue;
	}
	return FCString::Atoi(*Value);
}

static FString SerializeJsonObject(const TSharedPtr<FJsonObject>& JsonObj)
{
	FString JsonString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
	FJsonSerializer::Serialize(JsonObj.ToSharedRef(), Writer);
	return JsonString;
}

static TSharedPtr<FJsonObject> BuildPythonErrorResponse(const FString& ErrorCode, const FString& ErrorMessage)
{
	TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
	ErrorObj->SetBoolField(TEXT("success"), false);
	ErrorObj->SetStringField(TEXT("error_code"), ErrorCode);
	ErrorObj->SetStringField(TEXT("error_message"), ErrorMessage);
	return ErrorObj;
}

static bool ParseJsonObjectString(const FString& JsonText, TSharedPtr<FJsonObject>& OutObject)
{
	if (JsonText.IsEmpty())
	{
		return false;
	}

	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
}

static FString BuildWrappedPythonDiscoveryResponse(const FString& Scope, const FString& Target, const FString& PayloadJson)
{
	TSharedPtr<FJsonObject> PayloadObj;
	if (!ParseJsonObjectString(PayloadJson, PayloadObj) || !PayloadObj.IsValid())
	{
		return SerializeJsonObject(BuildPythonErrorResponse(TEXT("PYTHON_INTROSPECTION_FAILED"), TEXT("Failed to parse python discovery payload.")));
	}

	const bool bSuccess = PayloadObj->GetBoolField(TEXT("success"));
	PayloadObj->SetStringField(TEXT("scope"), Scope);
	if (!Target.IsEmpty())
	{
		PayloadObj->SetStringField(TEXT("target"), Target);
	}
	return SerializeJsonObject(PayloadObj);
}

static FString HandleDiscoverPythonApi(const TMap<FString, FString>& Params)
{
	FString Scope = ExtractParamWithDefault(Params, TEXT("scope"), TEXT(""));
	if (Scope.IsEmpty())
	{
		if (!ExtractParamFromJson(Params, TEXT("class_name")).IsEmpty())
		{
			Scope = TEXT("class");
		}
		else if (!ExtractParamFromJson(Params, TEXT("function_name")).IsEmpty() || !ExtractParamFromJson(Params, TEXT("function_path")).IsEmpty())
		{
			Scope = TEXT("function");
		}
		else if (!ExtractParamFromJson(Params, TEXT("module_name")).IsEmpty())
		{
			Scope = TEXT("module");
		}
		else
		{
			Scope = TEXT("module");
		}
	}

	Scope = Scope.TrimStartAndEnd().ToLower();

	auto Service = UPythonTools::GetDiscoveryService();
	if (!Service.IsValid())
	{
		return SerializeJsonObject(BuildPythonErrorResponse(TEXT("PYTHON_SERVICE_UNAVAILABLE"), TEXT("Python discovery service is not available")));
	}

	if (Scope == TEXT("module"))
	{
		const FString ModuleName = ExtractParamWithDefault(Params, TEXT("target"), ExtractParamWithDefault(Params, TEXT("module_name"), TEXT("unreal")));
		const FString NameFilter = ExtractParamWithDefault(Params, TEXT("filter"), ExtractParamWithDefault(Params, TEXT("name_filter"), TEXT("")));
		const int32 MaxItems = ExtractIntParam(Params, TEXT("max_items"), 100);
		const bool IncludeClasses = ExtractBoolParam(Params, TEXT("include_classes"), true);
		const bool IncludeFunctions = ExtractBoolParam(Params, TEXT("include_functions"), true);
		const bool CaseSensitive = ExtractBoolParam(Params, TEXT("case_sensitive"), false);

		if (!ModuleName.Equals(TEXT("unreal"), ESearchCase::IgnoreCase))
		{
			return SerializeJsonObject(BuildPythonErrorResponse(TEXT("PARAM_INVALID"), TEXT("discover_python_api currently supports module scope for the 'unreal' module only.")));
		}

		auto Result = Service->DiscoverUnrealModule(1, NameFilter, MaxItems, IncludeClasses, IncludeFunctions, CaseSensitive);
		if (Result.IsError())
		{
			return SerializeJsonObject(BuildPythonErrorResponse(Result.GetErrorCode(), Result.GetErrorMessage()));
		}

		return BuildWrappedPythonDiscoveryResponse(Scope, ModuleName, UPythonTools::ConvertModuleInfoToJson(Result.GetValue()));
	}

	if (Scope == TEXT("class"))
	{
		const FString ClassName = ExtractParamWithDefault(Params, TEXT("target"), ExtractParamFromJson(Params, TEXT("class_name")));
		if (ClassName.IsEmpty())
		{
			return SerializeJsonObject(BuildPythonErrorResponse(TEXT("PARAM_INVALID"), TEXT("class scope requires target or class_name.")));
		}

		const FString MethodFilter = ExtractParamWithDefault(Params, TEXT("filter"), ExtractParamWithDefault(Params, TEXT("method_filter"), TEXT("")));
		const int32 MaxMethods = ExtractIntParam(Params, TEXT("max_items"), ExtractIntParam(Params, TEXT("max_methods"), 0));
		const bool IncludeInherited = ExtractBoolParam(Params, TEXT("include_inherited"), false);
		const bool IncludePrivate = ExtractBoolParam(Params, TEXT("include_private"), false);

		auto Result = Service->DiscoverClass(ClassName, MethodFilter, MaxMethods, IncludeInherited, IncludePrivate);
		if (Result.IsError())
		{
			return SerializeJsonObject(BuildPythonErrorResponse(Result.GetErrorCode(), Result.GetErrorMessage()));
		}

		return BuildWrappedPythonDiscoveryResponse(Scope, ClassName, UPythonTools::ConvertClassInfoToJson(Result.GetValue()));
	}

	if (Scope == TEXT("function"))
	{
		FString FunctionName = ExtractParamWithDefault(Params, TEXT("target"), ExtractParamFromJson(Params, TEXT("function_name")));
		if (FunctionName.IsEmpty())
		{
			FunctionName = ExtractParamFromJson(Params, TEXT("function_path"));
		}

		if (FunctionName.IsEmpty())
		{
			return SerializeJsonObject(BuildPythonErrorResponse(TEXT("PARAM_INVALID"), TEXT("function scope requires target, function_name, or function_path.")));
		}

		auto Result = Service->DiscoverFunction(FunctionName);
		if (Result.IsError())
		{
			return SerializeJsonObject(BuildPythonErrorResponse(Result.GetErrorCode(), Result.GetErrorMessage()));
		}

		return BuildWrappedPythonDiscoveryResponse(Scope, FunctionName, UPythonTools::ConvertFunctionInfoToJson(Result.GetValue()));
	}

	if (Scope == TEXT("subsystems"))
	{
		const FString Filter = ExtractParamWithDefault(Params, TEXT("filter"), TEXT(""));
		const int32 MaxItems = ExtractIntParam(Params, TEXT("max_items"), 0);

		auto Result = Service->ListEditorSubsystems();
		if (Result.IsError())
		{
			return SerializeJsonObject(BuildPythonErrorResponse(Result.GetErrorCode(), Result.GetErrorMessage()));
		}

		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FString& Name : Result.GetValue())
		{
			if (!Filter.IsEmpty() && !Name.Contains(Filter, ESearchCase::IgnoreCase))
			{
				continue;
			}
			Items.Add(MakeShared<FJsonValueString>(Name));
			if (MaxItems > 0 && Items.Num() >= MaxItems)
			{
				break;
			}
		}

		TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
		JsonObj->SetBoolField(TEXT("success"), true);
		JsonObj->SetStringField(TEXT("scope"), Scope);
		JsonObj->SetArrayField(TEXT("subsystems"), Items);
		return SerializeJsonObject(JsonObj);
	}

	if (Scope == TEXT("search"))
	{
		const FString Pattern = ExtractParamWithDefault(Params, TEXT("target"), ExtractParamWithDefault(Params, TEXT("filter"), TEXT("")));
		const FString SearchType = ExtractParamWithDefault(Params, TEXT("search_type"), TEXT("all"));
		const int32 MaxItems = ExtractIntParam(Params, TEXT("max_items"), 100);

		if (Pattern.IsEmpty())
		{
			return SerializeJsonObject(BuildPythonErrorResponse(TEXT("PARAM_INVALID"), TEXT("search scope requires target or filter.")));
		}

		auto Result = Service->SearchAPI(Pattern, SearchType);
		if (Result.IsError())
		{
			return SerializeJsonObject(BuildPythonErrorResponse(Result.GetErrorCode(), Result.GetErrorMessage()));
		}

		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FString& Item : Result.GetValue())
		{
			Items.Add(MakeShared<FJsonValueString>(Item));
			if (MaxItems > 0 && Items.Num() >= MaxItems)
			{
				break;
			}
		}

		TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
		JsonObj->SetBoolField(TEXT("success"), true);
		JsonObj->SetStringField(TEXT("scope"), Scope);
		JsonObj->SetStringField(TEXT("target"), Pattern);
		JsonObj->SetStringField(TEXT("search_type"), SearchType);
		JsonObj->SetArrayField(TEXT("results"), Items);
		return SerializeJsonObject(JsonObj);
	}

	return SerializeJsonObject(BuildPythonErrorResponse(
		TEXT("PARAM_INVALID"),
		TEXT("scope must be one of: module, class, function, subsystems, search.")));
}

// Register execute_python_code tool
REGISTER_VIBEUE_TOOL(execute_python_code,
	"Execute Python code in Unreal Engine. IMPORTANT: Use 'import unreal' (lowercase). For subsystems use: unreal.get_editor_subsystem(unreal.LevelEditorSubsystem). Returns stdout, stderr, and execution status.",
	"Python",
	TOOL_PARAMS(
		TOOL_PARAM("code", "Python code to execute. Must start with 'import unreal' (lowercase). For editor subsystems use unreal.get_editor_subsystem()", "string", true)
	),
	{
		FString Code = ExtractParamFromJson(Params, TEXT("code"));
		return UPythonTools::ExecutePythonCode(Code);
	}
);

REGISTER_VIBEUE_TOOL(discover_python_api,
	"Unified Python API discovery tool. Supports scope=module|class|function|subsystems|search. Use this before execute_python_code to inspect the Unreal Python surface without choosing between multiple narrow discovery tools.",
	"Python",
	TOOL_PARAMS(
		TOOL_PARAM("scope", "Discovery scope: module, class, function, subsystems, or search. If omitted, inferred from legacy params or defaults to module.", "string", false),
		TOOL_PARAM("target", "Primary target for the chosen scope. Examples: unreal, unreal.EditorActorSubsystem, unreal.load_asset, or a search pattern.", "string", false),
		TOOL_PARAM("filter", "Optional generic filter. Alias of name_filter/method_filter depending on scope.", "string", false),
		TOOL_PARAM("max_items", "Optional cap for returned items. For class scope this aliases max_methods.", "number", false),
		TOOL_PARAM("include_classes", "Module scope only: include classes (default true).", "boolean", false),
		TOOL_PARAM("include_functions", "Module scope only: include functions (default true).", "boolean", false),
		TOOL_PARAM("case_sensitive", "Module scope only: whether filtering is case-sensitive (default false).", "boolean", false),
		TOOL_PARAM("include_inherited", "Class scope only: include inherited methods (default false).", "boolean", false),
		TOOL_PARAM("include_private", "Class scope only: include private methods (default false).", "boolean", false),
		TOOL_PARAM("search_type", "Search scope only: class, function, or all (default all).", "string", false),
		TOOL_PARAM("module_name", "Legacy alias for target when scope=module.", "string", false),
		TOOL_PARAM("class_name", "Legacy alias for target when scope=class.", "string", false),
		TOOL_PARAM("function_name", "Legacy alias for target when scope=function.", "string", false),
		TOOL_PARAM("function_path", "Legacy alias for target when scope=function.", "string", false),
		TOOL_PARAM("name_filter", "Legacy alias for module filter.", "string", false),
		TOOL_PARAM("method_filter", "Legacy alias for class filter.", "string", false),
		TOOL_PARAM("max_methods", "Legacy alias for class result cap.", "number", false)
	),
	{
		return HandleDiscoverPythonApi(Params);
	}
);
