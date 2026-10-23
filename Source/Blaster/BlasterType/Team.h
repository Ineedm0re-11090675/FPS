#pragma once

UENUM(Blueprintable)
enum class ETeam :uint8
{
	ET_RedTeam UMETA(DisplayName = "Red Team"),
	ET_BlueTeam UMETA(DisplayName = "Blue Team"),
	ET_NoTeam UMETA(DisplayName = "No Team"),
	
	ET_MaxDefault UMETA(DisplayName = "Max Default"),
};
