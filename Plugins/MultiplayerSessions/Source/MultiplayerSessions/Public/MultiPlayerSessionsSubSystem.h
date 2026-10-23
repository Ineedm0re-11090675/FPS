// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Interfaces/OnlineSessionInterface.h"

#include "MultiPlayerSessionsSubSystem.generated.h"

//
//声明一些自定义Delegate
//在此对 Delegate 的 Declare 做点讲解
//Dynamic动态 MultCast多播放，通知所以关心结果的对象 OnePram 带几个入参，这决定了CallBack函数的入参
//为啥这里有Dynamic和Multicast之分？Dynamic兼容蓝图，bool蓝图认识，所以可以。
//但是TArray，EOnjoin...这些蓝图不认识，可能会报错，所以不要Dynamic
//
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FMultiplayerOnCreateSessionComplete,bool,bSuccess);
DECLARE_MULTICAST_DELEGATE_TwoParams(FMultiplayerOnFindSessionsComplete,const TArray<FOnlineSessionSearchResult>& Results,bool bWasSuccessful);
DECLARE_MULTICAST_DELEGATE_OneParam(FMultiplayerOnJoinSessionComplete,EOnJoinSessionCompleteResult::Type Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FMultiplayerOnDestroySessionComplete,bool,bSuccess);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FMultiplayerOnStartSessionComplete,bool,bSuccess);
/**  
 * 
 */
UCLASS()
class MULTIPLAYERSESSIONS_API UMultiPlayerSessionsSubSystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()
public:
	UMultiPlayerSessionsSubSystem();
	virtual void Deinitialize() override;
	//
	//去生产，寻找，连接 sessions，会被Menu class call
	//
	void CreateSession(int32 NumToPlay,FString MatchType);
	void FindSession(int32 MaxSearchResults);
	void JoinSession(const FOnlineSessionSearchResult& SessionResult);
	void DestroySession();
	void StartSession();
	//
	//对于自定义的委托，生成这个委托
	//
	FMultiplayerOnCreateSessionComplete MultiplayerOnCreateSessionComplete;
	FMultiplayerOnFindSessionsComplete MultiplayerOnFindSessionsComplete;
	FMultiplayerOnJoinSessionComplete MultiplayerOnJoinSessionComplete;
	FMultiplayerOnDestroySessionComplete MultiplayerOnDestroySessionComplete;
	FMultiplayerOnStartSessionComplete MultiplayerOnStartSessionComplete;
private:
	IOnlineSessionPtr OnlineSessionInterface;
	TSharedPtr<FOnlineSessionSettings>LastSessionSettings;
	//生成一些委托
	//这里的DeleteHandle是为了删除生成的list中的handle
	
	//在此生成Delegate
	FOnCreateSessionCompleteDelegate OnCreateSessionCompleteDelegate;
	FDelegateHandle OnCreateSessionCompleteDelegateHandle;

	FOnFindSessionsCompleteDelegate OnFindSessionsCompleteDelegate;
	FDelegateHandle OnFindSessionsCompleteDelegateHandle;
	
	FOnJoinSessionCompleteDelegate OnJoinSessionCompleteDelegate;
	FDelegateHandle OnJoinSessionCompleteDelegateHandle;

	FOnDestroySessionCompleteDelegate OnDestroySessionCompleteDelegate;
	FDelegateHandle OnDestroySessionCompleteDelegateHandle;
	
	FOnStartSessionCompleteDelegate OnStartSessionCompleteDelegate;
	FDelegateHandle OnStartSessionCompleteDelegateHandle;
	bool bCreateSessionOnDestroy{false};
	int32 LastNumberOfPlayers;
	FString LastMatchType;
protected:
	//写callback
	void OnCreateSessionComplete(FName SessionName, bool bWasSuccessful);
	void OnFindSessionsComplete(bool bWasSuccessful);
	void OnJoinSessionComplete(FName SessionName,EOnJoinSessionCompleteResult::Type Result);
	void OnDestroySessionComplete(FName SessionName, bool bWasSuccessful);
	void OnStartSessionComplete(FName SessionName, bool bWasSuccessful);
	TSharedPtr<FOnlineSessionSearch> LastSessionSearch;
};
 
