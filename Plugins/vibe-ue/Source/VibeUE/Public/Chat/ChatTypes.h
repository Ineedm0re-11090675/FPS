// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "ChatTypes.generated.h"

/**
 * Status of a task in the task list
 */
UENUM()
enum class EVibeUETaskStatus : uint8
{
    NotStarted    UMETA(DisplayName = "Not Started"),
    InProgress    UMETA(DisplayName = "In Progress"),
    Completed     UMETA(DisplayName = "Completed")
};

/**
 * A single task item in the task list
 */
USTRUCT()
struct VIBEUE_API FVibeUETaskItem
{
    GENERATED_BODY()

    UPROPERTY()
    int32 Id = 0;

    UPROPERTY()
    FString Title;

    UPROPERTY()
    EVibeUETaskStatus Status = EVibeUETaskStatus::NotStarted;

    /** Convert to JSON */
    TSharedPtr<FJsonObject> ToJson() const;

    /** Create from JSON */
    static FVibeUETaskItem FromJson(const TSharedPtr<FJsonObject>& JsonObject);

    /** Get status as string ("not-started", "in-progress", "completed") */
    FString GetStatusString() const;

    /** Parse status from string */
    static EVibeUETaskStatus ParseStatus(const FString& StatusStr);
};
