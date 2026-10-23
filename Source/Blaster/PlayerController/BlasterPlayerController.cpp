// Fill out your copyright notice in the Description page of Project Settings.
#include "BlasterPlayerController.h"

#include "Blaster/BlasterComponent/CombatComponent.h"
#include "Blaster/BlasterType/Announcement.h"
#include "Blaster/Character/MyBlasterCharacter.h"
#include "Blaster/Gamemode/BlasterGameMode.h"
#include "Blaster/GameState/BlasterGameState.h"
#include "Blaster/HUD/Announcement.h"
#include "Blaster/HUD/BlasterHUD.h"
#include "Blaster/HUD/CharacterOverlay.h"
#include "Blaster/HUD/ReturnToMainMenu.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "Components/Image.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "GameFramework/GameMode.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"


void ABlasterPlayerController::BeginPlay()
{
	Super::BeginPlay();
	BlasterHUD=Cast<ABlasterHUD>(GetHUD());
	ServerCheckMatchState();
	RequestServerTimeSync();
}

void ABlasterPlayerController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	AMyBlasterCharacter* BlasterCharacter=Cast<AMyBlasterCharacter>(InPawn);
	if(BlasterCharacter)
	{
		SetHealth(BlasterCharacter->GetHealth(),BlasterCharacter->GetMaxHealth());
		if (ABlasterPlayerState* BlasterPlayerState = GetPlayerState<ABlasterPlayerState>())
		{
			if (UCombatComponent* CombatComponent = BlasterCharacter->GetCombat())
			{
				CombatComponent->SetGrenadeAmount(BlasterPlayerState->GetGrenadeAmount());
			}
		}
	}
}

void ABlasterPlayerController::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	SetHudTime();
	//这么做的意义是因为client和server之间有延迟
	CheckTimeSync(DeltaTime);
	PollInit();
	CheckPing(DeltaTime);
}
void ABlasterPlayerController::CheckPing(float DeltaTime)
{
	if (!IsLocalController()) return;

	HighPingRunningTime += DeltaTime;
	if (HighPingRunningTime <= CheckPingFrequency) return;
	HighPingRunningTime = 0.f;

	APlayerState* CurrentPlayerState = GetPlayerState<APlayerState>();
	if (CurrentPlayerState == nullptr) return;

	const float CurrentPing = CurrentPlayerState->GetPingInMilliseconds();
	const bool bHighPing = CurrentPing > HighPingThreshold;

	if (bHighPing)
	{
		if (!bHighPingWarningActive)
		{
			HighPingWarning();
		}
		ServerReportPingState(true);
	}
	else
	{
		if (bHighPingWarningActive)
		{
			StopHighPingWarning();
		}
		ServerReportPingState(false);
	}
}
void ABlasterPlayerController::ServerReportPingState_Implementation(bool bHighPing)
{
	HighPingDelegate.Broadcast(bHighPing);
}
void ABlasterPlayerController::CheckTimeSync(float Delta)
{
	TimeSyncStart+=Delta;
	if (TimeSyncStart>TimeSyncFrequency)
	{
		RequestServerTimeSync();
		TimeSyncStart = 0;
	}
}

void ABlasterPlayerController::GetLifetimeReplicatedProps(TArray<class FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ABlasterPlayerController,MatchState);
	DOREPLIFETIME(ABlasterPlayerController,bShowTeamScore );
}


void ABlasterPlayerController::SetHealth(float Health, float MaxHealth)
{
	BlasterHUD= BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()):BlasterHUD;
	bool bHUDvalid=BlasterHUD &&
		BlasterHUD->CharacterOverlay &&
		BlasterHUD->CharacterOverlay->HealthBar &&
		 BlasterHUD->CharacterOverlay->HealthText; 
	if(bHUDvalid)
	{
		const float HealthPercent = Health/MaxHealth;
		BlasterHUD->CharacterOverlay->HealthBar->SetPercent(HealthPercent);
		FString HealthText=FString::Printf(TEXT("%d/%d"),FMath::CeilToInt(Health),FMath::CeilToInt(MaxHealth));
		BlasterHUD->CharacterOverlay->HealthText->SetText(FText::FromString(HealthText));
	}else
	{
		bInitHealth=true;
		HUDHealth=MaxHealth;
		HudMaxHealth=MaxHealth;
	}
}

void ABlasterPlayerController::SetShield(float Shield, float MaxShield)
{
	BlasterHUD= BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()):BlasterHUD;
	bool bHUDvalid=BlasterHUD &&
		BlasterHUD->CharacterOverlay &&
		BlasterHUD->CharacterOverlay->ShieldBar &&
		 BlasterHUD->CharacterOverlay->ShieldText; 
	if(bHUDvalid)
	{
		const float HealthPercent = Shield/MaxShield;
		BlasterHUD->CharacterOverlay->ShieldBar->SetPercent(HealthPercent);
		FString	SText=FString::Printf(TEXT("%d/%d"),FMath::CeilToInt(Shield),FMath::CeilToInt(MaxShield));
		BlasterHUD->CharacterOverlay->ShieldText->SetText(FText::FromString(SText));
	}else
	{
		bInitShield=true;
		HUDShield=Shield;
		HudMaxShield=MaxShield;
	}
}

void ABlasterPlayerController::SetScore(float Score)
{
	BlasterHUD= BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()):BlasterHUD;
	bool bHUDvalid=BlasterHUD &&
		BlasterHUD->CharacterOverlay &&
		BlasterHUD->CharacterOverlay->ScoreAmount;
	if(bHUDvalid)
	{
		FString ScoreText=FString::Printf(TEXT("%d"),FMath::FloorToInt(Score));
		BlasterHUD->CharacterOverlay->ScoreAmount->SetText(FText::FromString(ScoreText));
	}else
	{
		bInitScore=true;
		HUDScore=Score;
	}
}

void ABlasterPlayerController::SetDefeat(int32 Defeats)
{
	BlasterHUD= BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()):BlasterHUD;
	bool bHUDvalid=BlasterHUD &&
		BlasterHUD->CharacterOverlay &&
		BlasterHUD->CharacterOverlay->DefeatAmount;
	if(bHUDvalid)
	{
		FString DefeatText=FString::Printf(TEXT("%d"),Defeats);
		BlasterHUD->CharacterOverlay->DefeatAmount->SetText(FText::FromString(DefeatText));
	}else
	{
		bInitDefeats=true;
		HUDDefeats=Defeats;
	}
}

void ABlasterPlayerController::SetAmmo(int32 Ammo)
{
	BlasterHUD= BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()):BlasterHUD;
	bool bHUDvalid=BlasterHUD &&
		BlasterHUD->CharacterOverlay;
	if(bHUDvalid)
	{
		if (BlasterHUD->CharacterOverlay->WeaponAmmoAmount)
		{
			FString AmmoText=FString::Printf(TEXT("%d"),Ammo);
			BlasterHUD->CharacterOverlay->WeaponAmmoAmount->SetText(FText::FromString(AmmoText ));
		}
		BlasterHUD->CharacterOverlay->SetWeaponAmmoVisible(true);
	}else
	{
		bInitWeaponAmmo=true;
		HUDWeaponAmmo = Ammo;
	}
}

void ABlasterPlayerController::SetCarriedAmmo(int32 Ammo)
{
	BlasterHUD= BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()):BlasterHUD;
	bool bHUDvalid=BlasterHUD &&
		BlasterHUD->CharacterOverlay;
	if(bHUDvalid)
	{
		if (BlasterHUD->CharacterOverlay->CarriedAmmoAmount)
		{
			FString AmmoText=FString::Printf(TEXT("%d"),Ammo);
			BlasterHUD->CharacterOverlay->CarriedAmmoAmount->SetText(FText::FromString(AmmoText));
		}
		BlasterHUD->CharacterOverlay->SetWeaponAmmoVisible(true);
	}else
	{
		bInitCarriedAmmo=true;
		HUDCarriedAmmo=Ammo;
	}
}

void ABlasterPlayerController::SetWeaponAmmoVisible(bool bVisible)
{
	BlasterHUD= BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()):BlasterHUD;
	bool bHUDvalid=BlasterHUD &&
		BlasterHUD->CharacterOverlay;
	if(bHUDvalid)
	{
		BlasterHUD->CharacterOverlay->SetWeaponAmmoVisible(bVisible);
	}
}

void ABlasterPlayerController::SetHUDGrenade(int32 GrenadeAmount)
{
	HUDGrenade = GrenadeAmount;

	BlasterHUD = BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()) : BlasterHUD;
	bool bHUDvalid = BlasterHUD &&
		BlasterHUD->CharacterOverlay &&
		BlasterHUD->CharacterOverlay->GrenadeAmount;

	if (bHUDvalid)
	{
		FString GrenadeText = FString::Printf(TEXT("%d"), GrenadeAmount);
		BlasterHUD->CharacterOverlay->GrenadeAmount->SetText(FText::FromString(GrenadeText));
	}
}


void ABlasterPlayerController::SetHUDMatchCountDown(float time)
{
	BlasterHUD= BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()):BlasterHUD;
	bool bHUDvalid=BlasterHUD &&
		BlasterHUD->CharacterOverlay &&
		BlasterHUD->CharacterOverlay->Timer;
	if(bHUDvalid)
	{
		if (time <0.f)
		{
			BlasterHUD->CharacterOverlay->Timer->SetText(FText())
;			return;
		}
		int32 Minutes=FMath::FloorToInt(time/60.0);
		int32 Seconds=FMath::FloorToInt(time-Minutes*60.0);
		FString TimeText=FString::Printf(TEXT("%02d:%02d"),Minutes,Seconds);
		BlasterHUD->CharacterOverlay->Timer->SetText(FText::FromString(TimeText));
	}
}


void ABlasterPlayerController::SetHUDWarmUpCountDown(float time)
{
	BlasterHUD= BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()):BlasterHUD;
	bool bHUDvalid=BlasterHUD &&
		BlasterHUD->Announcement&&
		BlasterHUD->Announcement->WarmUpTime;
	if(bHUDvalid)
	{
		if (time <0.f)
		{
			BlasterHUD->Announcement->WarmUpTime->SetText(FText());
			return;
		}
		int32 Minutes=FMath::FloorToInt(time/60.0);
		int32 Seconds=FMath::FloorToInt(time-Minutes*60.0);
		FString TimeText=FString::Printf(TEXT("%02d:%02d"),Minutes,Seconds);
		BlasterHUD->Announcement->WarmUpTime->SetText(FText::FromString(TimeText));
	}
}


void ABlasterPlayerController::SetHudTime()
{
	if (!HasAuthority() && !bHasSyncedServerTime)
	{
		return;
	}
	float TimeLeft=0.f;
	if (MatchState==MatchState::WaitingToStart)
	{
		TimeLeft = WarmUpTime - GetServerTime() + LevelStartTime;
	}
	if (MatchState==MatchState::InProgress)	
	{
		TimeLeft= WarmUpTime + MatchTime - GetServerTime() + LevelStartTime;
	}
	if (MatchState==MatchState::Cooldown)
	{
		TimeLeft= WarmUpTime + MatchTime + CooldownTime - GetServerTime() + LevelStartTime;
	}
	uint32 SecondsLeft=FMath::CeilToInt(TimeLeft);
	if (HasAuthority())
	{
		BlasterGameMode = BlasterGameMode == nullptr ? Cast<ABlasterGameMode>(UGameplayStatics::GetGameMode(this)) : BlasterGameMode;
		if (BlasterGameMode)
		{
			SecondsLeft = FMath::CeilToInt(BlasterGameMode->GetCountDownTime());
		}
	}
	if (CountDownInt !=SecondsLeft)
	{
		if (MatchState==MatchState::WaitingToStart || MatchState==MatchState::Cooldown)
		{
			SetHUDWarmUpCountDown(TimeLeft);
		}
		if (MatchState==MatchState::InProgress)
		{
			SetHUDMatchCountDown(TimeLeft);			
		}
	}
	CountDownInt=SecondsLeft;
}

void ABlasterPlayerController::ServerRequestServerTime_Implementation(float TimeOfClientRequest)
{
	float ServerTimeOfReceipt=GetWorld()->GetTimeSeconds();
	ClientReportServerTime( TimeOfClientRequest,ServerTimeOfReceipt);
}

void ABlasterPlayerController::ClientReportServerTime_Implementation(float TimeOfClientRequest,
	float TimeServerReceiveClientRequest)
{
	float RoundTripTime = GetWorld()->GetTimeSeconds()-TimeOfClientRequest;
	SingleTripTime=RoundTripTime *0.5f;
	float CurrentServerTime=TimeServerReceiveClientRequest + (0.5f*RoundTripTime);
	ClientServerDelta= CurrentServerTime-GetWorld()->GetTimeSeconds();
	bHasSyncedServerTime=true;
}
float ABlasterPlayerController::GetServerTime()
{
	if (HasAuthority())
	{
		return GetWorld()->GetTimeSeconds();
	}else
	{
		return GetWorld()->GetTimeSeconds()+ClientServerDelta;
	}
}

void ABlasterPlayerController::ReceivedPlayer()
{
	Super::ReceivedPlayer();
	RequestServerTimeSync();
}


void ABlasterPlayerController::HighPingWarning()
{
	BlasterHUD= BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()):BlasterHUD;
	bool bHUDvalid=BlasterHUD &&
		BlasterHUD->CharacterOverlay &&
		BlasterHUD->CharacterOverlay->HighPingWifiSignal &&
		 BlasterHUD->CharacterOverlay->Shing; 
	if(bHUDvalid)
	{
		bHighPingWarningActive = true;
		BlasterHUD->CharacterOverlay->HighPingWifiSignal->SetOpacity(1.f);
		if (!BlasterHUD->CharacterOverlay->IsAnimationPlaying(BlasterHUD->CharacterOverlay->Shing))
		{
			BlasterHUD->CharacterOverlay->PlayAnimation( BlasterHUD->CharacterOverlay->Shing ,
				0.f,
				0);
		}
	}
}

void ABlasterPlayerController::StopHighPingWarning()
{
	bHighPingWarningActive = false;
	BlasterHUD= BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()):BlasterHUD;
	bool bHUDvalid=BlasterHUD &&
		BlasterHUD->CharacterOverlay &&
		BlasterHUD->CharacterOverlay->HighPingWifiSignal &&
		 BlasterHUD->CharacterOverlay->Shing; 
	if(bHUDvalid)
	{
		BlasterHUD->CharacterOverlay->HighPingWifiSignal->SetOpacity(0.f);
		if (BlasterHUD->CharacterOverlay->IsAnimationPlaying(BlasterHUD->CharacterOverlay->Shing))
		{
			BlasterHUD->CharacterOverlay->StopAnimation( BlasterHUD->CharacterOverlay->Shing );
		}
	}
}

void ABlasterPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();
	if (InputComponent == nullptr) return;
	InputComponent->BindAction("Quit",IE_Pressed,this,&ABlasterPlayerController::ShowReturnToMainMenu);
}

void ABlasterPlayerController::ShowReturnToMainMenu()
{
	//Show
	if (ReturnToMainMenuWidget == nullptr) return;
	if (ReturnToMainMenu == nullptr)
	{
		ReturnToMainMenu = CreateWidget<UReturnToMainMenu>(this,ReturnToMainMenuWidget);
	}
	if (ReturnToMainMenu)
	{
		bReturnToMainMenuOpen = !bReturnToMainMenuOpen;
		if (bReturnToMainMenuOpen)
		{
			ReturnToMainMenu->SetUpMenu();
		}else
		{
			ReturnToMainMenu->TearDownMenu();
		}
	}
}

void ABlasterPlayerController::BroadCastElim(APlayerState* Attacker, APlayerState* Victim)
{
	ClientBroadCastElim(Attacker,Victim);
}

void ABlasterPlayerController::InitTeamScore()
{
	BlasterHUD= BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()):BlasterHUD;
	bool bHUDvalid=BlasterHUD &&
		BlasterHUD->CharacterOverlay &&
		BlasterHUD->CharacterOverlay->RedTeamTScore &&
		 BlasterHUD->CharacterOverlay->BlueTeamTScore &&
		 BlasterHUD->CharacterOverlay->TeamSlash; 
	if(bHUDvalid)
	{
		// Codex: Use replicated GameState scores so late client HUD init mirrors the current team score.
		const ABlasterGameState* BlasterGameState = Cast<ABlasterGameState>(UGameplayStatics::GetGameState(this));
		const int32 RedTeamScore = BlasterGameState ? FMath::RoundToInt(BlasterGameState->RedTeamTotalScore) : 0;
		const int32 BlueTeamScore = BlasterGameState ? FMath::RoundToInt(BlasterGameState->BlueTeamTotalScore) : 0;
		BlasterHUD->CharacterOverlay->TeamSlash->SetText(FText::FromString("/"));
		BlasterHUD->CharacterOverlay->RedTeamTScore->SetText(FText::FromString(FString::FromInt(RedTeamScore)));
		BlasterHUD->CharacterOverlay->BlueTeamTScore->SetText(FText::FromString(FString::FromInt(BlueTeamScore)));
	}
}

void ABlasterPlayerController::HideTeamScore()
{
	BlasterHUD= BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()):BlasterHUD;
	bool bHUDvalid=BlasterHUD &&
		BlasterHUD->CharacterOverlay &&
		BlasterHUD->CharacterOverlay->RedTeamTScore &&
		 BlasterHUD->CharacterOverlay->BlueTeamTScore &&
		 BlasterHUD->CharacterOverlay->TeamSlash; 
	if(bHUDvalid)
	{
		BlasterHUD->CharacterOverlay->TeamSlash->SetText(FText());
		BlasterHUD->CharacterOverlay->RedTeamTScore->SetText(FText());
		BlasterHUD->CharacterOverlay->BlueTeamTScore->SetText(FText());
	}
}

void ABlasterPlayerController::SetRedTeamScore(int32 TeamScore)
{
	BlasterHUD= BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()):BlasterHUD;
	bool bHUDvalid=BlasterHUD &&
		BlasterHUD->CharacterOverlay &&
		BlasterHUD->CharacterOverlay->RedTeamTScore;
	if(bHUDvalid)
	{
		// Codex: If score replication arrives after a missed init, restore the team score separator too.
		if (bShowTeamScore && BlasterHUD->CharacterOverlay->TeamSlash)
		{
			BlasterHUD->CharacterOverlay->TeamSlash->SetText(FText::FromString("/"));
		}
		FString ScoreText = FString::FromInt(TeamScore);
		BlasterHUD->CharacterOverlay->RedTeamTScore->SetText(FText::FromString(ScoreText));
	}
}

void ABlasterPlayerController::SetBlueTeamScore(int32 TeamScore)
{
	BlasterHUD= BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()):BlasterHUD;
	bool bHUDvalid=BlasterHUD &&
		BlasterHUD->CharacterOverlay &&
		BlasterHUD->CharacterOverlay->BlueTeamTScore;
	if(bHUDvalid)
	{
		// Codex: Same fallback as red score so either team score update can recover the separator.
		if (bShowTeamScore && BlasterHUD->CharacterOverlay->TeamSlash)
		{
			BlasterHUD->CharacterOverlay->TeamSlash->SetText(FText::FromString("/"));
		}
		FString ScoreText = FString::FromInt(TeamScore);
		BlasterHUD->CharacterOverlay->BlueTeamTScore->SetText(FText::FromString(ScoreText));
	}
}

void ABlasterPlayerController::ClientBroadCastElim_Implementation(APlayerState* Attacker, APlayerState* Victim)
{
	APlayerState* Self =GetPlayerState<APlayerState>();
	if (Self && Attacker && Victim)
	{
		BlasterHUD = BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()) : BlasterHUD;
		if (BlasterHUD)
		{
			if (Attacker == Self && Victim != Self)
			{
				BlasterHUD->AddElimAnnouncement("You",Victim->GetPlayerName());
				return;
			}
			if (Victim == Self && Attacker != Self)
			{
				BlasterHUD->AddElimAnnouncement(Attacker->GetPlayerName(),"You");
				return;
			}
			if (Victim == Attacker && Attacker == Self)
			{
				BlasterHUD->AddElimAnnouncement("You","Yourself");
				return;
			}
				if (Victim == Attacker && Attacker != Self)
				{
					BlasterHUD->AddElimAnnouncement(Victim->GetPlayerName(),"Itself");
					return;
				}
			BlasterHUD->AddElimAnnouncement(Attacker->GetPlayerName(),Victim->GetPlayerName());
		}
	}
}

void ABlasterPlayerController::OnRep_ShowTeamScore()
{
	BlasterHUD= BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()):BlasterHUD;
	if (BlasterHUD && BlasterHUD->CharacterOverlay == nullptr && MatchState == MatchState::InProgress)
	{
		// Codex: bShowTeamScore can replicate before the overlay exists on clients; create it before applying visibility.
		BlasterHUD->AddCharacterOverlay();
	}

	if (bShowTeamScore)
	{
		InitTeamScore();
	}else
	{
		HideTeamScore();
	}
}



void ABlasterPlayerController::RequestServerTimeSync()
{
	if (IsLocalController() && !HasAuthority())
	{
		ServerRequestServerTime(GetWorld()->GetTimeSeconds());
	}
}

	
void ABlasterPlayerController::HandleMatchHasStarted(bool bTeamMatch)
{
	if (HasAuthority()) bShowTeamScore = bTeamMatch;

	BlasterHUD = BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()) : BlasterHUD;
	if (BlasterHUD)
	{
		if (BlasterHUD->CharacterOverlay == nullptr) BlasterHUD->AddCharacterOverlay();

		if (BlasterHUD->Announcement)
		{
			BlasterHUD->Announcement->SetVisibility(ESlateVisibility::Hidden);
		}

		if (!HasAuthority())
		{
			// Codex: ClientJoinMidGame may be the only path that knows this is a team match.
			if (bTeamMatch)
			{
				bShowTeamScore = true;
			}

			if (bShowTeamScore)
			{
				InitTeamScore();
			}
			return;
		}

		if (bTeamMatch)
		{
			InitTeamScore();
		}
		else
		{
			HideTeamScore();
		}
	}
}
FString ABlasterPlayerController::GetInfoText(const TArray<ABlasterPlayerState*>& Player)
{
	ABlasterPlayerState* BlasterPlayerState =GetPlayerState<ABlasterPlayerState>();
	if (BlasterPlayerState == nullptr) return FString();
	FString InfoTextString;
	if (Player.Num() == 0)
	{
		InfoTextString=Announcement::NoWinner;
	}else if (Player.Num() == 1 && BlasterPlayerState == Player[0])
	{
		InfoTextString= Announcement::YouWin;
	}else if (Player.Num() == 1)
	{
		InfoTextString= FString::Printf(TEXT("Winner: \n %s"),*Player[0]->GetPlayerName());
	}else if (Player.Num() > 1)
	{
		InfoTextString = Announcement::PlayerTiedForTheWin;
		InfoTextString.Append(FString("\n"));
		for (auto TopPlayer : Player)
		{
			InfoTextString.Append(FString::Printf(TEXT("%s\n"),*TopPlayer->GetPlayerName()));
		}
	}
	return InfoTextString;
}

FString ABlasterPlayerController::GetTeamInfoText(class ABlasterGameState* GameState)
{
	if (GameState == nullptr) return FString();
	FString InfoTextString;

	const int32 RedTeamScore = GameState->RedTeamTotalScore;
	const int32 BlueTeamScore = GameState->BlueTeamTotalScore;

	if (RedTeamScore == 0 && BlueTeamScore == 0)
	{
		InfoTextString = Announcement::NoWinner;
	}
	else if (RedTeamScore ==BlueTeamScore)
	{
		InfoTextString = FString::Printf(TEXT("%s\n"),*Announcement::TeamTiedForTheWin);
		InfoTextString.Append(Announcement::RedTeam);
		InfoTextString.Append(TEXT("\n"));
		InfoTextString.Append(Announcement::BlueTeam);
		InfoTextString.Append(TEXT("\n"));
	}else if (BlueTeamScore > RedTeamScore)
	{
		InfoTextString = Announcement::BlueTeamWin;
		InfoTextString.Append(TEXT("\n"));
		InfoTextString.Append(FString::Printf(TEXT("%s : %d\n"),*Announcement::BlueTeam,BlueTeamScore));
		InfoTextString.Append(FString::Printf(TEXT("%s : %d\n"),*Announcement::RedTeam,RedTeamScore));
	}else if (BlueTeamScore < RedTeamScore)
	{
		InfoTextString = Announcement::RedTeamWin;
		InfoTextString.Append(TEXT("\n"));
		InfoTextString.Append(FString::Printf(TEXT("%s : %d\n"),*Announcement::RedTeam,RedTeamScore));
		InfoTextString.Append(FString::Printf(TEXT("%s : %d\n"),*Announcement::BlueTeam,BlueTeamScore));
	}
	return InfoTextString;
}

void ABlasterPlayerController::HandleCoolDown()
{ 
	BlasterHUD = BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()) : BlasterHUD;
	if (BlasterHUD) 
	{
		if (BlasterHUD->CharacterOverlay)
		{
			BlasterHUD->CharacterOverlay->RemoveFromParent();
		}
		bool bHUDValid=BlasterHUD->Announcement && BlasterHUD->Announcement->AnnouncementText && BlasterHUD->Announcement->InfoText;
		if (bHUDValid)
		{
			BlasterHUD->Announcement->SetVisibility(ESlateVisibility::Visible);
			FString AnnouncementText =Announcement::NewMatchStartsIn;
			BlasterHUD->Announcement->AnnouncementText->SetText(FText::FromString(AnnouncementText));

			ABlasterGameState* BlasterGameState= Cast<ABlasterGameState>(UGameplayStatics::GetGameState(this));
			if (BlasterGameState)
			{
				TArray<ABlasterPlayerState*> TopPlayers = BlasterGameState->TopScoringPlayers;
				FString InfoTextString = bShowTeamScore ? GetTeamInfoText(BlasterGameState) : GetInfoText(TopPlayers);
	 			BlasterHUD->Announcement->InfoText->SetText(FText::FromString(InfoTextString));
			}
		}
	}
	AMyBlasterCharacter* BlasterCharacter = Cast<AMyBlasterCharacter>(GetPawn());
	if (BlasterCharacter && BlasterCharacter->GetCombat())
	{
		BlasterCharacter->bDisableGamePlay=true;
		BlasterCharacter->GetCombat()->FireButtonPressed(false);
	}
}



void ABlasterPlayerController::OnSetMatchState(FName State,bool bTeamMatch)
{
	MatchState=State;
	
	if (MatchState== MatchState::InProgress)
	{
		HandleMatchHasStarted(bTeamMatch);
	}
	if (MatchState == MatchState::WaitingToStart)
	{
		if (IsLocalController())
		{
			if (BlasterHUD)
			{
				BlasterHUD->AddAnnouncement();
			}
		}
	}
	if (MatchState == MatchState::Cooldown)
	{
		if (IsLocalController())
		{
			HandleCoolDown();
		}
	}
}
void ABlasterPlayerController::OnRep_MatchState()
{
	if (MatchState== MatchState::InProgress)
	{
		if (IsLocalController())
		{
			HandleMatchHasStarted();
		}
	} 
	if (MatchState == MatchState::WaitingToStart)
	{
		if (IsLocalController())
		{
			if (BlasterHUD)
			{
				BlasterHUD->AddAnnouncement();
			}
		}
	}
	if (MatchState == MatchState::Cooldown)
	{
		if (IsLocalController())
		{
			HandleCoolDown();
		}
	}
}




void ABlasterPlayerController::PollInit()
{
	BlasterHUD = BlasterHUD == nullptr ? Cast<ABlasterHUD>(GetHUD()) : BlasterHUD;

	if (CharacterOverlay == nullptr && BlasterHUD && BlasterHUD->CharacterOverlay)
	{
		CharacterOverlay = BlasterHUD->CharacterOverlay;

		if (CharacterOverlay)
		{
			if (bInitHealth)SetHealth(HUDHealth, HudMaxHealth);
			if (bInitShield)SetShield(HUDShield, HudMaxShield);
			if (bInitDefeats) SetDefeat(HUDDefeats);
			if (bInitScore) SetScore(HUDScore);
			if (bInitWeaponAmmo) SetAmmo(HUDWeaponAmmo);
			if (bInitCarriedAmmo) SetCarriedAmmo(HUDCarriedAmmo);
			if (MatchState == MatchState::InProgress)
			{
				// Codex: Replay team score visibility once CharacterOverlay is actually ready on the client.
				if (bShowTeamScore)
				{
					InitTeamScore();
				}
				else
				{
					HideTeamScore();
				}
			}
		}
	}

	if (CharacterOverlay == nullptr) return;

	AMyBlasterCharacter* Character = Cast<AMyBlasterCharacter>(GetPawn());
	if (Character != HUDInitializedCharacter)
	{
		HUDInitializedCharacter = Character;

		if (Character)
		{
			SetHealth(Character->GetHealth(), Character->GetMaxHealth());
			SetShield(Character->GetShield(), Character->GetMaxShield());
			if (Character->GetCombat())
			{
				SetHUDGrenade(Character->GetCombat()->GetGrenadeAmount());
			}
		}
	}
}
void ABlasterPlayerController::ServerCheckMatchState_Implementation()
{
	ABlasterGameMode* GameMode=Cast<ABlasterGameMode>(UGameplayStatics::GetGameMode(this));

	if (GameMode)
	{
		WarmUpTime =GameMode->WarmUpTime;
		MatchTime =GameMode->MatchTime;
		//这个来自Gamemode比较重要
		LevelStartTime =GameMode->LevelStartTime;
		CooldownTime =GameMode->CooldownTime;
		MatchState=GameMode->GetMatchState();
		const bool bTeamMatch = GameMode->bTeamsMatch;
		if (MatchState == MatchState::InProgress)
		{
			// Codex: Server-side owner controller must replicate team HUD mode for clients that sync after match start.
			bShowTeamScore = bTeamMatch;
		}
		ClientJoinMidGame(MatchState,WarmUpTime,MatchTime,LevelStartTime,CooldownTime,bTeamMatch);
	}
}
void ABlasterPlayerController::ClientJoinMidGame_Implementation(FName State, float WarmUp, float Match, float Starting,float Cooldown,bool bTeamMatch)
{
	//这两个函数解决了中途加入的client的gamemode状态获悉
	WarmUpTime =WarmUp;
	MatchTime =Match;
	LevelStartTime =Starting;
	MatchState=State;
	CooldownTime =Cooldown;
	bShowTeamScore = bTeamMatch; // Codex: Preserve team match mode on clients using the join-mid-game sync path.
	OnSetMatchState(MatchState,bTeamMatch);
}
