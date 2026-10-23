// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Interfaces/OnlineSessionInterface.h"

#include "Menu.generated.h"
/**
 * 
 */
UCLASS() 
class MULTIPLAYERSESSIONS_API UMenu : public UUserWidget
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable)  
	void MenuSetUp(int32 NumPublicConnectionsToHost = 2, FString MatchTypeToHost = TEXT("Free For All"), FString LobbyPath = FString(TEXT("/Game/maps/Lobby")));
private:
	
	FDelegateHandle LevelRemovedFromWorldHandle;
	//反射宏，meta= ——————是将当前的按钮指针附在同名的按钮插件上
	UPROPERTY(meta=(BindWidget))
	class UButton* HostButton;
	
	UPROPERTY(meta=(BindWidget))
	UButton* JoinButton;

	UFUNCTION(BlueprintCallable)
	void HostButtonClicked();
	UFUNCTION(BlueprintCallable)
	void JoinButtonClicked();
	//
	//撤销UI
	//
	void RestoreGameInputMode();
	void MenuTearDown();
	void HandleLevelRemovedFromWorld(ULevel* InLevel, UWorld* InWorld);
	
	//拥有处理所有session功能的子系统指针
	class UMultiPlayerSessionsSubSystem* MultiPlayerSessionsSubSystem;
	//基础变量
	int32 NumPublicConnections{2};
	FString MatchType{TEXT("Free For All")};
	FString PathToLobby{TEXT("")};
protected:
	virtual bool Initialize() override;
	virtual void NativeDestruct() override;
	//
	//对自定义委托的callback函数,因为委托是动态，所以这里必须变成动态，加上UFUNCTION
	//
	UFUNCTION()
	void OnCreateSession(bool bWasSuccessful);
	UFUNCTION()
	void OnDestroySession(bool bWasSuccessful);
	UFUNCTION()
	void OnStartSession(bool bWasSuccessful);
	void OnJoinSession(EOnJoinSessionCompleteResult::Type Result);
	void OnFindSessionsComplete(const TArray<FOnlineSessionSearchResult>& Results, bool bWasSuccessful);
};  
