// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Core/ToolRegistry.h"
#include "Tools/ToolEnvelopeUtils.h"
#include "Tools/ToolUsageTelemetry.h"

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
}

REGISTER_VIBEUE_TOOL(get_tool_usage_report,
	"Read local VibeUE tool usage telemetry aggregated by day and by tool. Use this to evaluate which tools or skills are actually used, their UV/PV, and common failure/error-code patterns.",
	"Telemetry",
	TOOL_PARAMS(
		TOOL_PARAM("days", "How many recent UTC days to include (default 7, max 30).", "number", false),
		TOOL_PARAM("tool_filter", "Optional tool name substring filter.", "string", false),
		TOOL_PARAM("include_events", "Whether to include raw daily event entries in the report (default false).", "boolean", false),
		TOOL_PARAM("max_events_per_day", "Maximum number of raw events to include per day when include_events=true.", "number", false)
	),
	{
		const int32 Days = ExtractIntParam(Params, TEXT("days"), 7, 1, 30);
		const FString ToolFilter = ExtractParamFromJson(Params, TEXT("tool_filter"));
		const bool bIncludeEvents = ExtractBoolParam(Params, TEXT("include_events"), false);
		const int32 MaxEventsPerDay = ExtractIntParam(Params, TEXT("max_events_per_day"), 200, 1, 5000);
		return FToolEnvelopeUtils::SerializeJsonObject(FToolUsageTelemetry::BuildUsageReport(Days, ToolFilter, bIncludeEvents, MaxEventsPerDay));
	}
);
