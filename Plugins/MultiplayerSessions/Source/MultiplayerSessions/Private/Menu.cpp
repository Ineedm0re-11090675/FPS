// Fill out your copyright notice in the Description page of Project Settings.


#include "Menu.h"
#include "Components/Button.h"
#include "MultiPlayerSessionsSubSystem.h"
#include "OnlineSessionSettings.h"
#include "OnlineSubsystem.h"

void UMenu::HandleLevelRemovedFromWorld(ULevel*, UWorld* InWorld)
{
	if (InWorld == GetWorld())
	{
		MenuTearDown();
	}
}

void UMenu::HostButtonClicked()
{
	if (HostButton)
	{
		HostButton->SetIsEnabled(false);
	}
	if (MultiPlayerSessionsSubSystem)
	{
		MultiPlayerSessionsSubSystem->CreateSession(NumPublicConnections, MatchType);
	}
	else if (HostButton)
	{
		HostButton->SetIsEnabled(true);
	}
}

bool UMenu::Initialize()
{
	if (!Super::Initialize())
	{
		return false; 
	}
	if (HostButton)
	{
		HostButton->OnClicked.AddUniqueDynamic(this, &ThisClass::HostButtonClicked);
	}
	if (JoinButton)
	{
		JoinButton->OnClicked.AddUniqueDynamic(this, &ThisClass::JoinButtonClicked);
	}
	LevelRemovedFromWorldHandle = FWorldDelegates::LevelRemovedFromWorld.AddUObject(this, &UMenu::HandleLevelRemovedFromWorld);
	return true;
}

void UMenu::JoinButtonClicked()
{
	if (JoinButton)
	{
		JoinButton->SetIsEnabled(false);
	}
	if (MultiPlayerSessionsSubSystem)
	{
		MultiPlayerSessionsSubSystem->FindSession(10000);
	}
	else if (JoinButton)
	{
		JoinButton->SetIsEnabled(true);
	}
}

void UMenu::MenuSetUp(int32 NumPublicConnectionsToHost, FString MatchTypeToHost, FString LobbyPath)
{
	PathToLobby = FString::Printf(TEXT("%s?listen"), *LobbyPath);
	NumPublicConnections = NumPublicConnectionsToHost;
	MatchType = MatchTypeToHost;
	AddToViewport();
	SetVisibility(ESlateVisibility::Visible);
	SetIsFocusable(true);

	UWorld* World = GetWorld();
	if (World)
	{
		APlayerController* PlayerController = World->GetFirstPlayerController();
		if (PlayerController)
		{
			FInputModeUIOnly InputModeData;
			InputModeData.SetWidgetToFocus(TakeWidget());
			InputModeData.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
			PlayerController->SetInputMode(InputModeData);
			PlayerController->SetShowMouseCursor(true);
		}
	}
	UGameInstance* GameInstance = GetGameInstance();
	if (GameInstance)
	{
		MultiPlayerSessionsSubSystem = GameInstance->GetSubsystem<UMultiPlayerSessionsSubSystem>();
	}
	if (MultiPlayerSessionsSubSystem)
	{
		//对这个Mult..做了一个绑定
		MultiPlayerSessionsSubSystem->MultiplayerOnCreateSessionComplete.AddUniqueDynamic(this, &UMenu::OnCreateSession);
		MultiPlayerSessionsSubSystem->MultiplayerOnStartSessionComplete.AddUniqueDynamic(this, &UMenu::OnStartSession);
		MultiPlayerSessionsSubSystem->MultiplayerOnDestroySessionComplete.AddUniqueDynamic(this, &UMenu::OnDestroySession);
		MultiPlayerSessionsSubSystem->MultiplayerOnFindSessionsComplete.AddUObject(this, &UMenu::OnFindSessionsComplete);
		MultiPlayerSessionsSubSystem->MultiplayerOnJoinSessionComplete.AddUObject(this, &UMenu::OnJoinSession);
	} 
}

void UMenu::MenuTearDown()
{
	//把这个菜单从屏幕上移除
	RemoveFromParent();
	RestoreGameInputMode();
}

void UMenu::NativeDestruct()
{
	RestoreGameInputMode();
	if (LevelRemovedFromWorldHandle.IsValid())
	{
		FWorldDelegates::LevelRemovedFromWorld.Remove(LevelRemovedFromWorldHandle);
		LevelRemovedFromWorldHandle.Reset();
	}
	Super::NativeDestruct();
}

void UMenu::OnCreateSession(bool bWasSuccessful)
{
	if (bWasSuccessful)
	{
		UWorld* World = GetWorld();
		if (World && World->GetNetMode() != NM_Client)
		{
			if (World->ServerTravel(PathToLobby))
			{
				MenuTearDown();
			}
			else
			{
				if (HostButton)
				{
					HostButton->SetIsEnabled(true);
				}
			}
			//
			//注意每次使用Menu之后，UI交互是没有解绑的，所以无法移动还在输入阶段，必须卸载这一功能
			//
		}
		else if (HostButton)
		{
			HostButton->SetIsEnabled(true);
		}
	}
	else
	{
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(
				-1,
				15.f,
				FColor::Red,
				FString(TEXT("Session Create Failed")));
		}
		if (HostButton)
		{
			HostButton->SetIsEnabled(true);
		}
	}
}

void UMenu::OnDestroySession(bool)
{
}

void UMenu::OnFindSessionsComplete(const TArray<FOnlineSessionSearchResult>& Results, bool bWasSuccessful)
{
	if (MultiPlayerSessionsSubSystem == nullptr)
	{
		return;
	}
	for (auto& Result : Results)
	{
		FString SettingValue;
		Result.Session.SessionSettings.Get("MatchType", SettingValue);
		if (SettingValue == MatchType)
		{
			MultiPlayerSessionsSubSystem->JoinSession(Result);
			return;
		}
	}
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(
			-1,
			15.f,
			FColor::Red,
			FString(TEXT("No Room Found")));
	}
	if (!bWasSuccessful || Results.Num() == 0)
	{
		if (JoinButton)
		{
			JoinButton->SetIsEnabled(true);
		}
	}
}

void UMenu::OnJoinSession(EOnJoinSessionCompleteResult::Type Result)
{
	if (Result != EOnJoinSessionCompleteResult::Success)
	{
		if (JoinButton)
		{
			JoinButton->SetIsEnabled(true);
		}
		return;
	}
	IOnlineSubsystem* OnlineSubsystem = IOnlineSubsystem::Get();
	if (!OnlineSubsystem)
	{
		if (JoinButton)
		{
			JoinButton->SetIsEnabled(true);
		}
		return;
	}
	IOnlineSessionPtr OnlineSessionInterface = OnlineSubsystem->GetSessionInterface();
	if (!OnlineSessionInterface)
	{
		if (JoinButton)
		{
			JoinButton->SetIsEnabled(true);
		}
		return;
	}
	
	FString Address;
	if (!OnlineSessionInterface->GetResolvedConnectString(NAME_GameSession, Address) || Address.IsEmpty())
	{
		if (JoinButton)
		{
			JoinButton->SetIsEnabled(true);
		}
		return;
	}
	UGameInstance* GameInstance = GetGameInstance();
	if (!GameInstance)
	{
		if (JoinButton)
		{
			JoinButton->SetIsEnabled(true);
		}
		return;
	}
	APlayerController* PlayerController = GameInstance->GetFirstLocalPlayerController();
	if (!PlayerController)
	{
		if (JoinButton)
		{
			JoinButton->SetIsEnabled(true);
		}
		return;
	}

	MenuTearDown();
	PlayerController->ClientTravel(Address, TRAVEL_Absolute);
}

void UMenu::OnStartSession(bool)
{
}

void UMenu::RestoreGameInputMode()
{
	UWorld* World = GetWorld();
	if (World)
	{
		APlayerController* PlayerController = World->GetFirstPlayerController();
		if (PlayerController)
		{
			FInputModeGameOnly InputModeData;
			PlayerController->SetInputMode(InputModeData);
			PlayerController->SetShowMouseCursor(false);
		}
	}
}
