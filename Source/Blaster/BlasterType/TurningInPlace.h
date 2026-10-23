#pragma once

UENUM(Blueprintable)
enum class ETurningInPlace : uint8
{
	ETIP_Left UMETA(DisplayName = "Turn Left"),
	ETIP_Right UMETA(DisplayName = "Turn Right"),
	ETIP_Noturning UMETA(DisplayName = "No Turning"),
	ETIP_MAX UMETA(DisplayName = "Default")
 };