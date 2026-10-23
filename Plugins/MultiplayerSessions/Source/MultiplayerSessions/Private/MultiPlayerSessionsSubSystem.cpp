// Fill out your copyright notice in the Description page of Project Settings.


#include "MultiPlayerSessionsSubSystem.h"
#include "OnlineSubsystem.h"
#include "OnlineSessionSettings.h"
#include "Online/OnlineSessionNames.h"
UMultiPlayerSessionsSubSystem::UMultiPlayerSessionsSubSystem()
	: OnCreateSessionCompleteDelegate(FOnCreateSessionCompleteDelegate::CreateUObject(this, &ThisClass::OnCreateSessionComplete)),
	  OnFindSessionsCompleteDelegate(FOnFindSessionsCompleteDelegate::CreateUObject(this, &ThisClass::OnFindSessionsComplete)),
	  OnJoinSessionCompleteDelegate(FOnJoinSessionCompleteDelegate::CreateUObject(this, &ThisClass::OnJoinSessionComplete)),
	  OnDestroySessionCompleteDelegate(FOnDestroySessionCompleteDelegate::CreateUObject(this, &ThisClass::OnDestroySessionComplete)),
	  OnStartSessionCompleteDelegate(FOnStartSessionCompleteDelegate::CreateUObject(this, &ThisClass::OnStartSessionComplete))
{
	IOnlineSubsystem* OnlineSubsystem = IOnlineSubsystem::Get();
	if (OnlineSubsystem)
	{
		OnlineSessionInterface = OnlineSubsystem->GetSessionInterface();
	}
}

void UMultiPlayerSessionsSubSystem::CreateSession(int32 NumToPlay, FString MatchType)
{
	IOnlineSubsystem* OnlineSubsystem = IOnlineSubsystem::Get();
	OnlineSessionInterface = OnlineSubsystem ? OnlineSubsystem->GetSessionInterface() : nullptr;
	if (!OnlineSessionInterface.IsValid())
	{
		MultiplayerOnCreateSessionComplete.Broadcast(false);
		return;
	}
	auto ExistingSession = OnlineSessionInterface->GetNamedSession(NAME_GameSession);
	if (ExistingSession)
	{
		bCreateSessionOnDestroy=true;
		LastNumberOfPlayers=NumToPlay;
		LastMatchType=MatchType;
		DestroySession();
		return;
	}
	//
	//我们做了声明但是没有做绑定，所以在此绑定。并且绑定后会传回一个销毁指针，我们做个保留
	//
	OnCreateSessionCompleteDelegateHandle= OnlineSessionInterface->AddOnCreateSessionCompleteDelegate_Handle(OnCreateSessionCompleteDelegate);
	//setting initialize
	LastSessionSettings = MakeShareable(new FOnlineSessionSettings());
	LastSessionSettings->bIsLANMatch= OnlineSubsystem->GetSubsystemName() == "NULL"?true:false;
	LastSessionSettings->NumPublicConnections=NumToPlay;
	LastSessionSettings->bAllowJoinInProgress = true;
	LastSessionSettings->bAllowJoinViaPresence=true;
	LastSessionSettings->bShouldAdvertise=true;
	LastSessionSettings->bUsesPresence=true;
	LastSessionSettings->bUseLobbiesIfAvailable = true;
	//给这个版本的房间标记为1，下一个版本我可以写2，这样不同版本不会相互连接了
	LastSessionSettings->BuildUniqueId=1;
	LastSessionSettings->Set(FName("MatchType"),MatchType,EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);

	//Create
	UWorld* World = GetWorld();
	const ULocalPlayer* LocalPlayer = World ? World->GetFirstLocalPlayerFromController() : nullptr;
	if (LocalPlayer == nullptr)
	{
		OnlineSessionInterface->ClearOnCreateSessionCompleteDelegate_Handle(OnCreateSessionCompleteDelegateHandle);
		MultiplayerOnCreateSessionComplete.Broadcast(false);
		return;
	}
	if (!OnlineSessionInterface->CreateSession(*LocalPlayer->GetPreferredUniqueNetId(),NAME_GameSession,*LastSessionSettings))
	{
		OnlineSessionInterface->ClearOnCreateSessionCompleteDelegate_Handle(OnCreateSessionCompleteDelegateHandle);
		MultiplayerOnCreateSessionComplete.Broadcast(false);
	}
}

void UMultiPlayerSessionsSubSystem::Deinitialize()
{
	if (OnlineSessionInterface.IsValid())
	{
		if (OnCreateSessionCompleteDelegateHandle.IsValid())
		{
			OnlineSessionInterface->ClearOnCreateSessionCompleteDelegate_Handle(OnCreateSessionCompleteDelegateHandle);
		}
		if (OnFindSessionsCompleteDelegateHandle.IsValid())
		{
			OnlineSessionInterface->ClearOnFindSessionsCompleteDelegate_Handle(OnFindSessionsCompleteDelegateHandle);
		}
		if (OnJoinSessionCompleteDelegateHandle.IsValid())
		{
			OnlineSessionInterface->ClearOnJoinSessionCompleteDelegate_Handle(OnJoinSessionCompleteDelegateHandle);
		}
		if (OnDestroySessionCompleteDelegateHandle.IsValid())
		{
			OnlineSessionInterface->ClearOnDestroySessionCompleteDelegate_Handle(OnDestroySessionCompleteDelegateHandle);
		}
		if (OnStartSessionCompleteDelegateHandle.IsValid())
		{
			OnlineSessionInterface->ClearOnStartSessionCompleteDelegate_Handle(OnStartSessionCompleteDelegateHandle);
		}
	}

	OnlineSessionInterface.Reset();
	Super::Deinitialize();
}

void UMultiPlayerSessionsSubSystem::DestroySession()
{
	IOnlineSubsystem* OnlineSubsystem = IOnlineSubsystem::Get();
	OnlineSessionInterface = OnlineSubsystem ? OnlineSubsystem->GetSessionInterface() : nullptr;
	if (!OnlineSessionInterface.IsValid())
	{
		MultiplayerOnDestroySessionComplete.Broadcast(true);
		return;
	}
	if (OnlineSessionInterface->GetNamedSession(NAME_GameSession) == nullptr)
	{
		MultiplayerOnDestroySessionComplete.Broadcast(true);
		return;
	}
	OnDestroySessionCompleteDelegateHandle=OnlineSessionInterface->AddOnDestroySessionCompleteDelegate_Handle(OnDestroySessionCompleteDelegate);
	if(!OnlineSessionInterface->DestroySession(NAME_GameSession))
	{
		OnlineSessionInterface->ClearOnDestroySessionCompleteDelegate_Handle(OnDestroySessionCompleteDelegateHandle);
		MultiplayerOnDestroySessionComplete.Broadcast(false);
	}
}

void UMultiPlayerSessionsSubSystem::FindSession(int32 MaxSearchResults)
{
	IOnlineSubsystem* OnlineSubsystem = IOnlineSubsystem::Get();
	OnlineSessionInterface = OnlineSubsystem ? OnlineSubsystem->GetSessionInterface() : nullptr;
	if (!OnlineSessionInterface.IsValid())
	{
		MultiplayerOnFindSessionsComplete.Broadcast(TArray<FOnlineSessionSearchResult>(), false);
		return;
	}
	OnFindSessionsCompleteDelegateHandle=OnlineSessionInterface->AddOnFindSessionsCompleteDelegate_Handle(OnFindSessionsCompleteDelegate);
	LastSessionSearch=MakeShareable(new FOnlineSessionSearch());
	LastSessionSearch->MaxSearchResults=MaxSearchResults;
	LastSessionSearch->bIsLanQuery=OnlineSubsystem->GetSubsystemName() == "NULL"?true:false;
	LastSessionSearch->QuerySettings.Set(SEARCH_LOBBIES,true,EOnlineComparisonOp::Equals);
	UWorld* World = GetWorld();
	const ULocalPlayer* LocalPlayer = World ? World->GetFirstLocalPlayerFromController() : nullptr;
	if (LocalPlayer == nullptr)
	{
		OnlineSessionInterface->ClearOnFindSessionsCompleteDelegate_Handle(OnFindSessionsCompleteDelegateHandle);
		MultiplayerOnFindSessionsComplete.Broadcast(TArray<FOnlineSessionSearchResult>(), false);
		return;
	}
	if (!OnlineSessionInterface->FindSessions(*LocalPlayer->GetPreferredUniqueNetId(),LastSessionSearch.ToSharedRef()))
	{
		OnlineSessionInterface->ClearOnFindSessionsCompleteDelegate_Handle(OnFindSessionsCompleteDelegateHandle);
		MultiplayerOnFindSessionsComplete.Broadcast(TArray<FOnlineSessionSearchResult>(),false);
	}
}

void UMultiPlayerSessionsSubSystem::JoinSession(const FOnlineSessionSearchResult& SessionResult)
{
	IOnlineSubsystem* OnlineSubsystem = IOnlineSubsystem::Get();
	OnlineSessionInterface = OnlineSubsystem ? OnlineSubsystem->GetSessionInterface() : nullptr;
	if (!OnlineSessionInterface.IsValid())
	{
		MultiplayerOnJoinSessionComplete.Broadcast(EOnJoinSessionCompleteResult::UnknownError);
		return;
	}
	UWorld* World = GetWorld();
	const ULocalPlayer* LocalPlayer = World ? World->GetFirstLocalPlayerFromController() : nullptr;
	if (LocalPlayer == nullptr)
	{
		MultiplayerOnJoinSessionComplete.Broadcast(EOnJoinSessionCompleteResult::UnknownError);
		return;
	}
	OnJoinSessionCompleteDelegateHandle=OnlineSessionInterface->AddOnJoinSessionCompleteDelegate_Handle(OnJoinSessionCompleteDelegate);
	if (!OnlineSessionInterface->JoinSession(*LocalPlayer->GetPreferredUniqueNetId(),NAME_GameSession,SessionResult))
	{
		OnlineSessionInterface->ClearOnJoinSessionCompleteDelegate_Handle(OnJoinSessionCompleteDelegateHandle);
		MultiplayerOnJoinSessionComplete.Broadcast(EOnJoinSessionCompleteResult::UnknownError);
	}
}

void UMultiPlayerSessionsSubSystem::OnCreateSessionComplete(FName, bool bWasSuccessful)
{
	//触发CallBack记得去删除这个Delegate否则会一直在List里，让下一次的Create受阻
	if (OnlineSessionInterface)
	{
		OnlineSessionInterface->ClearOnCreateSessionCompleteDelegate_Handle(OnCreateSessionCompleteDelegateHandle);
	}
	MultiplayerOnCreateSessionComplete.Broadcast(bWasSuccessful);
}

void UMultiPlayerSessionsSubSystem::OnDestroySessionComplete(FName, bool bWasSuccessful)
{
	if (OnlineSessionInterface)
	{
		OnlineSessionInterface->ClearOnDestroySessionCompleteDelegate_Handle(OnDestroySessionCompleteDelegateHandle);
	}
	if (bCreateSessionOnDestroy)
	{
		bCreateSessionOnDestroy = false;
		if (bWasSuccessful)
		{
			CreateSession(LastNumberOfPlayers, LastMatchType);
		}
		else
		{
			MultiplayerOnCreateSessionComplete.Broadcast(false);
		}
		return;
	}
	MultiplayerOnDestroySessionComplete.Broadcast(bWasSuccessful);
}

void UMultiPlayerSessionsSubSystem::OnFindSessionsComplete(bool bWasSuccessful)
{
	if (OnlineSessionInterface)
	{
		OnlineSessionInterface->ClearOnFindSessionsCompleteDelegate_Handle(OnFindSessionsCompleteDelegateHandle);
	}
	if (!LastSessionSearch.IsValid() || LastSessionSearch->SearchResults.Num() <= 0)
	{
		MultiplayerOnFindSessionsComplete.Broadcast(TArray<FOnlineSessionSearchResult>(),false);
		return; 
	}
	MultiplayerOnFindSessionsComplete.Broadcast(LastSessionSearch->SearchResults,bWasSuccessful);
}

void UMultiPlayerSessionsSubSystem::OnJoinSessionComplete(FName, EOnJoinSessionCompleteResult::Type Result)
{
	if (OnlineSessionInterface)
	{
		OnlineSessionInterface->ClearOnJoinSessionCompleteDelegate_Handle(OnJoinSessionCompleteDelegateHandle);
	}
	MultiplayerOnJoinSessionComplete.Broadcast(Result);
}

void UMultiPlayerSessionsSubSystem::OnStartSessionComplete(FName, bool)
{
}

void UMultiPlayerSessionsSubSystem::StartSession()
{
}
