// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

struct VIBEUE_API FToolUsageEvent
{
	FDateTime TimestampUtc;
	FString ToolName;
	FString ActionName;
	FString RequestId;
	FString SessionId;
	bool bSuccess = false;
	double DurationMs = 0.0;
	FString ErrorCode;
	FString ErrorSummary;
	TArray<FString> ChangedItems;
	TArray<FString> VerificationHints;
	TArray<FString> Warnings;
	TArray<FString> AffectedAssets;
	int32 ActorCountDelta = 0;
	int32 NewDirtyPackageCount = 0;
	TArray<FString> NewDirtyPackages;
	TMap<FString, FString> Arguments;
};

class VIBEUE_API FToolUsageTelemetry
{
public:
	static FString GetUsageRootDir();
	static FString GetDailyUsageFilePath(const FDateTime& UtcTime);
	static bool RecordToolUsage(const FToolUsageEvent& Event, FString& OutError);
	static TSharedPtr<FJsonObject> BuildUsageReport(int32 Days, const FString& ToolFilter, bool bIncludeEvents, int32 MaxEventsPerDay);
};
