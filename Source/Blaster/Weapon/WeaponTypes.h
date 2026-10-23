#pragma once
#define TRACE_LENGTH 80000
#define CUSTOM_DEPTH_PURPLE 250
#define CUSTOM_DEPTH_BLUE 251
#define CUSTOM_DEPTH_TAN 252
UENUM(Blueprintable)
enum class EWeaponType : uint8
{
	EWT_AssaultRifle UMETA(DisplayName = "Assault Rifle"),
	EWT_RocketLaucher UMETA(DisplayName = "Rocket Launcher"),
	EWT_Pistol UMETA(DisplayName = "Pistol"),
	EWT_SubmachineGun UMETA(DisplayName = "Submachine Gun"),
	EWT_ShotGun UMETA(DisplayName = "Shotgun"),
	EWT_SniperRifle UMETA(DisplayName = "Sniper Rifle"),
	EWT_GrenadeLaucher UMETA(DisplayName = "Grenade Launcher"),
	
	EWT_MAX UMETA(DisplayName = "Default MAX")
};
