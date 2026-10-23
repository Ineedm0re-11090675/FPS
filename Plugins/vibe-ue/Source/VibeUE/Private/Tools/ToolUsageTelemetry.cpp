// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Tools/ToolUsageTelemetry.h"

#include "Json.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"

namespace
{
	static TSharedPtr<FJsonObject> BuildEventJson(const FToolUsageEvent& Event)
	{
		TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
		JsonObj->SetStringField(TEXT("timestamp_utc"), Event.TimestampUtc.ToIso8601());
		JsonObj->SetStringField(TEXT("date_utc"), Event.TimestampUtc.ToString(TEXT("%Y-%m-%d")));
		JsonObj->SetStringField(TEXT("tool_name"), Event.ToolName);
		JsonObj->SetStringField(TEXT("action_name"), Event.ActionName);
		JsonObj->SetStringField(TEXT("request_id"), Event.RequestId);
		JsonObj->SetStringField(TEXT("session_id"), Event.SessionId);
		JsonObj->SetBoolField(TEXT("success"), Event.bSuccess);
		JsonObj->SetNumberField(TEXT("duration_ms"), Event.DurationMs);
		JsonObj->SetStringField(TEXT("error_code"), Event.ErrorCode);
		JsonObj->SetStringField(TEXT("error_summary"), Event.ErrorSummary);
		JsonObj->SetNumberField(TEXT("changed_items_count"), Event.ChangedItems.Num());
		JsonObj->SetNumberField(TEXT("verification_hints_count"), Event.VerificationHints.Num());
		JsonObj->SetNumberField(TEXT("warnings_count"), Event.Warnings.Num());
		JsonObj->SetNumberField(TEXT("affected_assets_count"), Event.AffectedAssets.Num());
		JsonObj->SetNumberField(TEXT("actor_count_delta"), Event.ActorCountDelta);
		JsonObj->SetNumberField(TEXT("new_dirty_package_count"), Event.NewDirtyPackageCount);

		TArray<TSharedPtr<FJsonValue>> ChangedItemsArray;
		for (const FString& Item : Event.ChangedItems)
		{
			ChangedItemsArray.Add(MakeShared<FJsonValueString>(Item));
		}
		JsonObj->SetArrayField(TEXT("changed_items"), ChangedItemsArray);

		TArray<TSharedPtr<FJsonValue>> WarningsArray;
		for (const FString& Item : Event.Warnings)
		{
			WarningsArray.Add(MakeShared<FJsonValueString>(Item));
		}
		JsonObj->SetArrayField(TEXT("warnings"), WarningsArray);

		TArray<TSharedPtr<FJsonValue>> AffectedAssetsArray;
		for (const FString& Item : Event.AffectedAssets)
		{
			AffectedAssetsArray.Add(MakeShared<FJsonValueString>(Item));
		}
		JsonObj->SetArrayField(TEXT("affected_assets"), AffectedAssetsArray);

		TArray<TSharedPtr<FJsonValue>> NewDirtyPackagesArray;
		for (const FString& Item : Event.NewDirtyPackages)
		{
			NewDirtyPackagesArray.Add(MakeShared<FJsonValueString>(Item));
		}
		JsonObj->SetArrayField(TEXT("new_dirty_packages"), NewDirtyPackagesArray);

		TArray<TSharedPtr<FJsonValue>> SkillNamesArray;
		if (Event.ToolName.Equals(TEXT("manage_skills"), ESearchCase::IgnoreCase))
		{
			if (const FString* SkillName = Event.Arguments.Find(TEXT("skill_name")))
			{
				if (!SkillName->IsEmpty())
				{
					SkillNamesArray.Add(MakeShared<FJsonValueString>(*SkillName));
				}
			}
			if (const FString* ParamsJson = Event.Arguments.Find(TEXT("ParamsJson")))
			{
				TSharedPtr<FJsonObject> ParamsObj;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(*ParamsJson);
				if (FJsonSerializer::Deserialize(Reader, ParamsObj) && ParamsObj.IsValid())
				{
					const TArray<TSharedPtr<FJsonValue>>* SkillNames = nullptr;
					if (ParamsObj->TryGetArrayField(TEXT("skill_names"), SkillNames) && SkillNames)
					{
						for (const TSharedPtr<FJsonValue>& SkillValue : *SkillNames)
						{
							FString SkillName;
							if (SkillValue.IsValid() && SkillValue->TryGetString(SkillName) && !SkillName.IsEmpty())
							{
								SkillNamesArray.Add(MakeShared<FJsonValueString>(SkillName));
							}
						}
					}
				}
			}
		}
		if (SkillNamesArray.Num() > 0)
		{
			JsonObj->SetArrayField(TEXT("skill_names"), SkillNamesArray);
		}

		return JsonObj;
	}

	static FString SerializeObject(const TSharedPtr<FJsonObject>& JsonObj)
	{
		FString JsonString;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonString);
		FJsonSerializer::Serialize(JsonObj.ToSharedRef(), Writer);
		return JsonString;
	}
}

FString FToolUsageTelemetry::GetUsageRootDir()
{
	const FString Dir = FPaths::ProjectSavedDir() / TEXT("VibeUE") / TEXT("Analytics") / TEXT("ToolUsage");
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	PlatformFile.CreateDirectoryTree(*Dir);
	return Dir;
}

FString FToolUsageTelemetry::GetDailyUsageFilePath(const FDateTime& UtcTime)
{
	return GetUsageRootDir() / FString::Printf(TEXT("%s.jsonl"), *UtcTime.ToString(TEXT("%Y-%m-%d")));
}

bool FToolUsageTelemetry::RecordToolUsage(const FToolUsageEvent& Event, FString& OutError)
{
	const FString FilePath = GetDailyUsageFilePath(Event.TimestampUtc);
	const FString Line = SerializeObject(BuildEventJson(Event)) + LINE_TERMINATOR;
	if (!FFileHelper::SaveStringToFile(Line, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append))
	{
		OutError = FString::Printf(TEXT("Failed to append usage telemetry to '%s'."), *FilePath);
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FToolUsageTelemetry::BuildUsageReport(int32 Days, const FString& ToolFilter, bool bIncludeEvents, int32 MaxEventsPerDay)
{
	const TSharedPtr<FJsonObject> RootObj = MakeShared<FJsonObject>();
	RootObj->SetBoolField(TEXT("success"), true);
	RootObj->SetStringField(TEXT("generated_at_utc"), FDateTime::UtcNow().ToIso8601());

	const int32 ClampedDays = FMath::Clamp(Days, 1, 30);
	const int32 ClampedMaxEvents = FMath::Clamp(MaxEventsPerDay, 1, 5000);
	RootObj->SetNumberField(TEXT("days"), ClampedDays);
	RootObj->SetStringField(TEXT("tool_filter"), ToolFilter);

	TMap<FString, int32> ToolPV;
	TMap<FString, TSet<FString>> ToolUV;
	TMap<FString, int32> ToolFailures;
	TMap<FString, int32> ErrorCodeCounts;
	TMap<FString, int32> SkillLoads;

	TArray<TSharedPtr<FJsonValue>> DailyArray;
	int32 TotalEvents = 0;

	for (int32 DayOffset = 0; DayOffset < ClampedDays; ++DayOffset)
	{
		const FDateTime DayUtc = FDateTime::UtcNow() - FTimespan(DayOffset, 0, 0, 0);
		const FString FilePath = GetDailyUsageFilePath(DayUtc);
		if (!FPaths::FileExists(FilePath))
		{
			continue;
		}

		TArray<FString> Lines;
		if (!FFileHelper::LoadFileToStringArray(Lines, *FilePath))
		{
			continue;
		}

		int32 DayCount = 0;
		TArray<TSharedPtr<FJsonValue>> DayEvents;

		for (const FString& Line : Lines)
		{
			TSharedPtr<FJsonObject> EventObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
			if (!FJsonSerializer::Deserialize(Reader, EventObj) || !EventObj.IsValid())
			{
				continue;
			}

			FString ToolName;
			EventObj->TryGetStringField(TEXT("tool_name"), ToolName);
			if (!ToolFilter.IsEmpty() && !ToolName.Contains(ToolFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}

			++DayCount;
			++TotalEvents;
			ToolPV.FindOrAdd(ToolName)++;

			FString SessionId;
			EventObj->TryGetStringField(TEXT("session_id"), SessionId);
			if (!SessionId.IsEmpty())
			{
				ToolUV.FindOrAdd(ToolName).Add(SessionId);
			}

			bool bSuccess = false;
			EventObj->TryGetBoolField(TEXT("success"), bSuccess);
			if (!bSuccess)
			{
				ToolFailures.FindOrAdd(ToolName)++;
				FString ErrorCode;
				EventObj->TryGetStringField(TEXT("error_code"), ErrorCode);
				if (!ErrorCode.IsEmpty())
				{
					ErrorCodeCounts.FindOrAdd(ErrorCode)++;
				}
			}

			if (ToolName.Equals(TEXT("manage_skills"), ESearchCase::IgnoreCase))
			{
				const TArray<TSharedPtr<FJsonValue>>* SkillNames = nullptr;
				if (EventObj->TryGetArrayField(TEXT("skill_names"), SkillNames) && SkillNames)
				{
					for (const TSharedPtr<FJsonValue>& SkillValue : *SkillNames)
					{
						FString SkillName;
						if (SkillValue.IsValid() && SkillValue->TryGetString(SkillName) && !SkillName.IsEmpty())
						{
							SkillLoads.FindOrAdd(SkillName)++;
						}
					}
				}
			}

			if (bIncludeEvents && DayEvents.Num() < ClampedMaxEvents)
			{
				DayEvents.Add(MakeShared<FJsonValueObject>(EventObj));
			}
		}

		const TSharedPtr<FJsonObject> DayObj = MakeShared<FJsonObject>();
		DayObj->SetStringField(TEXT("date_utc"), DayUtc.ToString(TEXT("%Y-%m-%d")));
		DayObj->SetNumberField(TEXT("event_count"), DayCount);
		if (bIncludeEvents)
		{
			DayObj->SetArrayField(TEXT("events"), DayEvents);
		}
		DailyArray.Add(MakeShared<FJsonValueObject>(DayObj));
	}

	const TSharedPtr<FJsonObject> SummaryObj = MakeShared<FJsonObject>();
	SummaryObj->SetNumberField(TEXT("total_events"), TotalEvents);
	RootObj->SetObjectField(TEXT("summary"), SummaryObj);

	TArray<TSharedPtr<FJsonValue>> ToolArray;
	for (const auto& Pair : ToolPV)
	{
		const TSharedPtr<FJsonObject> ToolObj = MakeShared<FJsonObject>();
		ToolObj->SetStringField(TEXT("tool_name"), Pair.Key);
		ToolObj->SetNumberField(TEXT("pv"), Pair.Value);
		ToolObj->SetNumberField(TEXT("uv"), ToolUV.Contains(Pair.Key) ? ToolUV[Pair.Key].Num() : 0);
		ToolObj->SetNumberField(TEXT("failures"), ToolFailures.FindRef(Pair.Key));
		ToolArray.Add(MakeShared<FJsonValueObject>(ToolObj));
	}
	RootObj->SetArrayField(TEXT("tool_usage"), ToolArray);

	TArray<TSharedPtr<FJsonValue>> ErrorArray;
	for (const auto& Pair : ErrorCodeCounts)
	{
		const TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
		ErrorObj->SetStringField(TEXT("error_code"), Pair.Key);
		ErrorObj->SetNumberField(TEXT("count"), Pair.Value);
		ErrorArray.Add(MakeShared<FJsonValueObject>(ErrorObj));
	}
	RootObj->SetArrayField(TEXT("error_codes"), ErrorArray);

	TArray<TSharedPtr<FJsonValue>> SkillArray;
	for (const auto& Pair : SkillLoads)
	{
		const TSharedPtr<FJsonObject> SkillObj = MakeShared<FJsonObject>();
		SkillObj->SetStringField(TEXT("skill_name"), Pair.Key);
		SkillObj->SetNumberField(TEXT("load_count"), Pair.Value);
		SkillArray.Add(MakeShared<FJsonValueObject>(SkillObj));
	}
	RootObj->SetArrayField(TEXT("skill_loads"), SkillArray);

	RootObj->SetArrayField(TEXT("daily"), DailyArray);
	return RootObj;
}
