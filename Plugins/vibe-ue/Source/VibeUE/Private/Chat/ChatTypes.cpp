// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Chat/ChatTypes.h"
TSharedPtr<FJsonObject> FVibeUETaskItem::ToJson() const
{
    TSharedPtr<FJsonObject> JsonObject = MakeShared<FJsonObject>();
    JsonObject->SetNumberField(TEXT("id"), Id);
    JsonObject->SetStringField(TEXT("title"), Title);
    JsonObject->SetStringField(TEXT("status"), GetStatusString());
    return JsonObject;
}

FVibeUETaskItem FVibeUETaskItem::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
    FVibeUETaskItem Item;
    if (JsonObject.IsValid())
    {
        Item.Id = JsonObject->GetIntegerField(TEXT("id"));
        JsonObject->TryGetStringField(TEXT("title"), Item.Title);

        FString StatusStr;
        if (JsonObject->TryGetStringField(TEXT("status"), StatusStr))
        {
            Item.Status = ParseStatus(StatusStr);
        }
    }
    return Item;
}

FString FVibeUETaskItem::GetStatusString() const
{
    switch (Status)
    {
        case EVibeUETaskStatus::NotStarted:  return TEXT("not-started");
        case EVibeUETaskStatus::InProgress:  return TEXT("in-progress");
        case EVibeUETaskStatus::Completed:   return TEXT("completed");
        default:                             return TEXT("not-started");
    }
}

EVibeUETaskStatus FVibeUETaskItem::ParseStatus(const FString& StatusStr)
{
    if (StatusStr.Equals(TEXT("in-progress"), ESearchCase::IgnoreCase) ||
        StatusStr.Equals(TEXT("in_progress"), ESearchCase::IgnoreCase))
    {
        return EVibeUETaskStatus::InProgress;
    }
    if (StatusStr.Equals(TEXT("completed"), ESearchCase::IgnoreCase))
    {
        return EVibeUETaskStatus::Completed;
    }
    return EVibeUETaskStatus::NotStarted;
}
