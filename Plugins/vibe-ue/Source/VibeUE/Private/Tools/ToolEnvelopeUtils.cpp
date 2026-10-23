// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Tools/ToolEnvelopeUtils.h"

#include "Json.h"

FString FToolEnvelopeUtils::SerializeJsonObject(const TSharedPtr<FJsonObject>& JsonObject)
{
	FString JsonString;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
	FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
	return JsonString;
}

TSharedPtr<FJsonObject> FToolEnvelopeUtils::BuildErrorEntry(
	const FString& IdentifierField,
	const FString& IdentifierValue,
	const FString& ErrorMessage,
	const FString& ErrorCode)
{
	const TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
	if (!IdentifierField.IsEmpty() && !IdentifierValue.IsEmpty())
	{
		ErrorObj->SetStringField(IdentifierField, IdentifierValue);
	}
	ErrorObj->SetStringField(TEXT("error"), ErrorMessage);
	if (!ErrorCode.IsEmpty())
	{
		ErrorObj->SetStringField(TEXT("error_code"), ErrorCode);
	}
	return ErrorObj;
}

TSharedPtr<FJsonObject> FToolEnvelopeUtils::BuildBatchEnvelope(const FBatchToolEnvelope& Envelope)
{
	const TSharedPtr<FJsonObject> RootObj = MakeShared<FJsonObject>();
	RootObj->SetBoolField(TEXT("success"), Envelope.FailedCount == 0 && Envelope.SuccessCount > 0);
	RootObj->SetStringField(TEXT("operation"), Envelope.Operation);
	RootObj->SetStringField(TEXT("resource_kind"), Envelope.ResourceKind);

	if (Envelope.Target.IsValid())
	{
		RootObj->SetObjectField(TEXT("target"), Envelope.Target);
	}

	const TSharedPtr<FJsonObject> SummaryObj = MakeShared<FJsonObject>();
	SummaryObj->SetNumberField(TEXT("requested_count"), Envelope.RequestedCount);
	SummaryObj->SetNumberField(TEXT("success_count"), Envelope.SuccessCount);
	SummaryObj->SetNumberField(TEXT("failed_count"), Envelope.FailedCount);
	RootObj->SetObjectField(TEXT("summary"), SummaryObj);

	RootObj->SetArrayField(TEXT("results"), Envelope.Results);
	RootObj->SetArrayField(TEXT("errors"), Envelope.Errors);

	if (Envelope.bIncludeSaveFields)
	{
		RootObj->SetBoolField(TEXT("save_requested"), Envelope.bSaveRequested);
		RootObj->SetBoolField(TEXT("saved"), Envelope.bSaved);
		if (!Envelope.SaveError.IsEmpty())
		{
			RootObj->SetStringField(TEXT("save_error"), Envelope.SaveError);
		}
	}

	if (Envelope.bIncludeRestoreFields)
	{
		RootObj->SetBoolField(TEXT("restore_requested"), Envelope.bRestoreRequested);
		RootObj->SetBoolField(TEXT("restore_success"), Envelope.bRestoreSuccess);
		if (!Envelope.RestoreError.IsEmpty())
		{
			RootObj->SetStringField(TEXT("restore_error"), Envelope.RestoreError);
		}
	}

	return RootObj;
}
